# Production roadmap and target design

This document is the plan to take `rockchip-vaapi` from a working
proof-of-concept (Firefox H.264+VP9 decode) to a **fully featured, idiomatic,
maintainable, production-ready** VA-API driver over Rockchip MPP.

It has two parts: the **target design** (what a well-built version looks like)
and the **phased plan** (how we get there, with an exit gate per phase). It is
a living document — revise it as phases close.

## Scope

Decided 2026-07-21:

- **Decode and encode.** Decode is the foundation and ships first; encode
  (`VAEntrypointEncSlice` over MPP's rkvenc2) is a full later phase, not an
  afterthought.
- **Codecs:** H.264, HEVC, VP9 decode; H.264, HEVC encode. **10-bit / HDR is a
  v1 requirement** (HEVC Main10, VP9 Profile 2, P010). **AV1 decode is out of
  scope** — VA-API hands headerless tile data and MPP needs a full OBU stream;
  Firefox/Chromium fall back to hardware VP9 for AV1 content. Revisit later.
- **Targets:** the whole Linux desktop — Firefox, Chromium/Electron, VLC, mpv,
  the GStreamer `va` plugins, and stock distro FFmpeg. Not Firefox-only.
- **Posture:** independent maintained fork (`yisding/rockchip-vaapi`),
  LGPL-2.1, attribution to the original author preserved; develop in the open
  and offer changes upstream if `woodyst` revives.

### Definition of "production-ready"

The bar every phase is measured against:

1. **Correctness** — decode is bit-exact vs a software reference on conformance
   vectors, not just synthetic clips; encode round-trips within a PSNR bound
   and interops with standard decoders.
2. **Memory safety** — clean under ASan/UBSan and Valgrind on the test path.
   The driver runs *inside* the browser's media process; a heap bug is a
   browser RCE surface.
3. **Concurrency safety** — multiple simultaneous decode/encode contexts in one
   process (browsers do this) with no data races (clean under TSan).
4. **No silent failure** — every unsupported input returns a real `VAStatus`;
   no silent truncation, no "succeeded but produced garbage."
5. **Stability** — a multi-hour soak (looping 4K decode, resolution switches,
   seeks) with flat memory and no fd/buffer leaks.
6. **Observability** — structured, leveled logging; a self-test/conformance
   target a maintainer can run in one command.
7. **Packaging & docs** — versioned `.deb`, the browser sandbox story handled
   explicitly, design/roadmap/testing docs kept current.
8. **CI** — build + static analysis on every push; a documented hardware gate.

---

## Target design

The PoC is one ~1,600-line file with fixed-size global arrays, per-frame CPU
copies, and `usleep` polling. The target factors it into clear modules with a
sound object model, a zero-copy buffer path, and a real threading model —
following how mature VA drivers (Intel `i965`/`iHD`, Mesa Gallium) are built.

### Module layout

```
src/
  driver.c            __vaDriverInit, vtable wiring, capability queries
  object_heap.{c,h}   generation-tagged handle allocator (configs, contexts,
                      surfaces, buffers, images) — replaces the fixed arrays
  context.c           decode/encode context lifecycle + the per-context worker
  surface.c           VA surface <-> MPP buffer binding, dmabuf export
  buffer.c            VA buffer/image objects, map/unmap
  mpp_dec.c           MPP decode wrapper (packet in, frame out)
  mpp_enc.c           MPP encode wrapper (frame in, packet out)
  codec/
    bitstream.{c,h}   shared bit writer + Annex-B/emulation-prevention
    h264.c            SPS/PPS(+scaling) reconstruction, decode & encode params
    hevc.c            VPS/SPS/PPS(+scaling) reconstruction, decode & encode
    vp9.c             frame/superframe handling, show_existing/altref logic
  convert.c           RGA-backed format conversion (NV15->P010, etc.)
  export.c            VADRMPRIMESurfaceDescriptor construction (all consumers)
  log.{c,h}           leveled logging
```

### Object model

Replace `MAX_SURFACES`/`MAX_CONTEXTS` fixed arrays and `base|index` IDs with a
**generation-tagged object heap** (the libva-idiomatic pattern used by
`i965`): each handle encodes `(type, index, generation)`; lookup validates the
generation, so a stale VA handle after destroy is rejected instead of aliasing
a recycled slot. Grows dynamically; no arbitrary ceilings. All object tables
live behind the driver lock (below).

### Buffer / DPB model — the central redesign

The PoC copies every decoded frame out of MPP's small internal pool into a
per-surface buffer (~1.5 GB/s at 4K60) because exporting MPP's own buffers let
MPP recycle a frame the compositor was still showing. The target is
**zero-copy via an MPP external buffer group**:

- On context creation, allocate a pool of DRM/dma-heap buffers sized for
  `DPB + display-pipeline depth` and commit it with
  `MPP_DEC_SET_EXT_BUF_GROUP`, so MPP decodes **directly into buffers we own**.
- Bind each VA surface to the MPP buffer that holds its frame; keep an extra
  ref on that `MppFrame`/buffer until VA releases or reuses the surface, then
  return it to the group. This is the ownership inversion `libv4l-rkmpp`
  already proves works over MPP.
- `vaExportSurfaceHandle` dups the buffer's dmabuf fd directly — no copy.
- Frame→surface routing: with an external group MPP returns the buffer index,
  which maps to a surface deterministically, removing the PoC's PTS/FIFO
  guesswork. (Keep VP9's `show_existing_frame`/altref special-casing.)

Preserve the one PoC behavior that matters: a **pre-decode placeholder buffer**
so Firefox's `ExportSurfaceHandle` capability probe (before any decode)
succeeds.

### Threading & synchronization

- **One driver lock** guards the object heap and cross-object state. Short
  critical sections; never held across an MPP call.
- **Per-context worker thread** owns the MPP `mpi` and runs the drain loop
  (`decode_get_frame` in blocking/timeout mode), signaling a **per-surface
  fence** (condvar or eventfd) on completion. `EndPicture` enqueues work;
  `vaSyncSurface`/`vaSyncSurface2` wait on the fence with the caller's timeout.
  This deletes all `usleep` polling and makes sync honest.
- Handles MPP input backpressure by draining output and retrying the put
  (already fixed in the PoC; formalize it in the worker).

### Format & 10-bit path

- 8-bit decode outputs NV12; export as NV12 (composed) or R8+GR88 split layers
  (Firefox `DMABufSurfaceYUV`) — the PoC's `export.c` logic, kept.
- **10-bit:** request MPP **AFBC NV15** and convert **NV15→P010 via RGA**
  (`convert.c`, librga im2d) for universal app compatibility. Linear decoder
  NV15 is accepted only when its byte and pixel strides are exactly
  representable; RK3588's normal VDPU383 linear stride is not. Export P010 as
  R16+GR1616 split (Firefox) or composed P010 (mpv/GStreamer). Investigate
  **direct NV15 dmabuf export**
  (`DRM_FORMAT_NV15`) as a later zero-copy optimization where the consumer
  accepts it.

### Error handling & capability idiom

- One `mpp_ret → VAStatus` mapping; every path returns a real status.
- Advertise a profile only when its decode/encode path is implemented **and**
  validated (the PoC now gates on `profile_supported()` — keep that as the
  single source of truth, extended per phase).
- No fixed per-frame slice cap that truncates silently: grow the slice list, or
  return `VA_STATUS_ERROR_MAX_NUM_EXCEEDED`.

---

## Phased plan

Each phase is independently shippable and ends at a gate — a concrete,
re-runnable result. Rough sizing assumes one focused engineer; treat as
relative, not calendar-exact.

### Phase 0 — Baseline & harness  ✅ complete

Fork, build against the ROCK 5B stack, the original `framemd5` gate, and the
three correctness fixes (PPS ref-counts, VP9 backpressure, VP9 PTS routing) are
**done** on `main`. The remaining Phase 0 implementation also landed:

- The gate uses checksum-pinned **real conformance vectors** (ITU-T H.264 and
  official libvpx VP9), forces declared VA-API cases through hardware-frame
  output, and retains `testsrc2` only as a supplemental matrix.
- Keep profile advertising tied to that gate: the SVA Constrained Baseline
  vector is software-exact after header reconstruction but corrupt in MPP, so
  withdraw `VAProfileH264ConstrainedBaseline` and require software fallback
  until the hardware path is corrected.
- **Track 14 crash hardening:** the libvpx
  `vp90-2-10-show-existing-frame2` vector exposed an independent driver
  overflow: MPP returned a 768-byte VP9 stride for a 352-pixel frame, while
  the permanent VA surface buffer was sized for a 352-byte stride. The copy
  ran past that dmabuf, and the export descriptor also claimed the larger
  size; FFmpeg subsequently segfaulted. Placeholder surfaces now reserve MPP's
  codec alignment, every copy is bounds-checked, and unexpected layouts become
  `VA_STATUS_ERROR_DECODING_ERROR`.
- **VP9 hidden-reference bridging:** FFmpeg consumes `show_existing_frame`
  packets internally and later reuses the referenced VA surface, while MPP
  normally withholds `show_frame=0` buffers from `decode_get_frame`. The driver
  now parses the Profile 0 refresh mask and submits a minimal synthetic repeat
  so MPP exposes the completed hidden buffer for a bounds-checked copy into the
  correct VA surface. The parser is covered by real-vector headers,
  ASan/UBSan, exhaustive short-prefix checks, and Valgrind; its hardware gate
  remains quarantined until the fixed kernel is booted.
- The complete driver has an **ASan/UBSan build**; the safe conformance subset
  is green with that instrumented driver loaded into FFmpeg. Hardware-
  independent reconstruction/layout tests also run with full **Valgrind** leak
  checking.
- The **CI skeleton** runs AArch64 cross-builds, sanitizer unit tests,
  Valgrind, ShellCheck, and clang-tidy on every push. The on-board hardware
  gate is a guarded manual self-hosted job. Kernel-crash vectors additionally
  require an exact audited `uname -r` plus running kernel-notes fingerprint,
  preventing a stale manual confirmation from running them on a vulnerable
  build that shares the same release string.

Phase 0 closed on 2026-07-21 on the audited fixed kernel build `#3`. The full
unquarantined conformance gate passed every pinned H.264 and VP9 vector
bit-exact, including `vp90-2-10-show-existing-frame2`; the full ASan/UBSan gate
also passed the pinned vectors, synthetic H.264 matrix (including 4K), five VP9
determinism runs, and the unsupported-codec fallback check. The release and
kernel-notes guards were also verified to reject mismatches before device
access. A skipped required vector remains deliberately reported as blocked,
never green.

The full Phase 0 normal and ASan/UBSan gates were rerun after the first Phase 2
slice on 2026-07-22 against the exact audited `Pd222-C4ad2` kernel build `#4`.
Every pinned H.264/VP9 hardware vector, all eight unadvertised HEVC software-
fallback vectors, the H.264 reference/B-frame and 4K matrix, five VP9
determinism passes, and the unsupported-codec fallback check were green. The
risky hidden-reference vector was bit-exact in both runs. The running kernel
notes hash is
`db292410e58bd9c658a0b32b6fc7c7895f3ac4a349ae3c292c441e92e340690e`,
and its boot-image MD5 exactly matches the archived fixed `Pd222` deb.

**Gate:** ✅ conformance-vector decode bit-exact for the shipping profiles;
gate green under ASan; CI builds and lints on push.

### Phase 1 — Architectural renovation (decode core)  ✅ complete

The foundation everything else builds on. No new codecs — restructure.

Completed 2026-07-22.
The dynamic generation-tagged heap and driver object lock cover configs,
contexts, surfaces, buffers, and distinct image objects. Acquired objects are
reference-counted across short lock sections; stale/type-confused handles are
rejected; exhausted-generation slots are retired; contexts retain their
resolved profile independently of config lifetime. The on-board lifecycle
gate exceeds every former fixed ceiling (32 configs, 9 MPP contexts, 65
surfaces, 300 buffers, and 300 images) and passes normally and under
ASan/UBSan. Concurrent heap lifetimes pass TSan. After the migration, the
normal and sanitized Phase 0 decode gates remain bit-exact for every pinned and
supplemental case. MPP info-change now creates a driver-owned 24-buffer DRM
pool, commits it as `EXT_DMA` with stable indices, and binds returned frames
directly to VA surfaces. Surfaces retain the frame plus the borrowed fd's
backing buffer until reuse; export and image readback select that active
buffer. The per-frame decoded-pixel memcpy has been deleted. A repeatable
normal and ASan/UBSan gate observed 1,440 bit-exact external-pool frames across
12 H.264/VP9 contexts (including 4K and five VP9 runs), with no internal
fallback or ownership mismatch. Direct CPU readback is bracketed by dma-buf
sync; the missing transition initially caused intermittent one-frame VP9
mismatches, while the corrected path passed a focused 100-decode stress with
zero mismatches. The pool itself is refcounted across its context and bound
surfaces, preventing MPP orphan-group retention when a context is destroyed
before its surfaces. Each context now has one worker that owns runtime MPP
submission, backpressure draining, info-change handling, and output routing.
`EndPicture` queues an owned packet; H.264 outputs resolve a unique token and
VP9 keeps ordered routes, with both carrying the target surface's generation
fence. `vaSyncSurface` and `vaSyncSurface2` wait on the per-surface condition
instead of polling MPP, and the latter honors zero/finite/infinite timeouts.
Context teardown joins the worker and fails queued fences before MPP teardown.
Field-coded H.264 shares one fence and route token across its two declared
field submissions because MPP returns the completed frame with the first
field's PTS; other codec/surface reuse advances the fence normally. This
distinction fixed both the initially stalled CABREF field vector and an
early-ready VP9 hidden-frame race. Focused follow-up passed the field vector
3/3 and the previously nondeterministic big-superframe vector 20/20.
The 1,440-frame normal and ASan/UBSan zero-copy gates remain bit-exact with
matched worker start/stop and pool create/destroy counts, while the on-board
lifecycle/fence gate is clean normally and under ASan/UBSan and TSan. After
the worker slice, the complete risky-enabled Phase 0 normal and ASan/UBSan
gates are green again. The subsequent module-separation and active-concurrency
slices are described below.

Module separation is complete: the private shared object model now lives in
`driver_internal.h`, VA buffer/image lifecycle has moved to `buffer.c`, and
the former global logger is a thread-safe `log.c` module. DRM PRIME 2
descriptor construction now lives in `export.c` behind the surface-sync
interface in `surface.h`; surface lifecycle, fence waits, status, and image
readback now live in `surface.c`. Context lifecycle, picture submission,
and render-target ownership now live in `context.c`; external-pool management,
packet construction, frame routing, and MPP worker ownership live in
`mpp_dec.c`, leaving the main translation unit as the vtable/capability shell.
The clean build, ASan/UBSan, heap TSan, clang-tidy, all three on-board
lifecycle/export variants, and both normal and sanitized 1,440-frame
zero-copy/worker audits remain green at this boundary.

The two-active-decoder gate is also complete. One FFmpeg process decodes a
120-frame H.264 stream and a 120-frame VP9 stream through two simultaneous VA
contexts; the audit requires both workers to overlap, exactly two clean
external pools, and all 240 external frames to match software bit-exactly.
The bit-exact workload passes normally and with the complete ASan/UBSan driver.
The complete TSan driver downloads all 240 NV12 frames directly to rawvideo
sinks with no suppressions; its harness limits FFmpeg to one decoder thread
per input so uninstrumented FFmpeg frame-thread and swscale races do not
obscure the two instrumented driver workers.

The final paced 4K soak ran for 7,200 seconds and completed 216,005 external
frames with every observed external-pool and worker creation matched by
teardown. Post-warmup RSS started at 191,288 KiB and ended at 203,512 KiB,
with a 47,844 KiB span; the fd head/tail medians were both 55 with a bounded
24-fd transient span. No fallback, stale route, submission, ownership, or
unsafe-layout marker appeared. After the soak, the complete risky-enabled
Phase 0 gate was rerun on the final tree: all pinned vectors, supplemental
H.264 matrix, 4K case, five VP9 determinism runs, and software fallback passed
both normally and with the complete ASan/UBSan driver.

- Split the monolith into the module layout above; introduce the object heap.
- Implement the **external-buffer-group zero-copy model** and delete the
  per-frame memcpy.
- Implement the **per-context worker + per-surface fence** sync model; delete
  polling.
- Add the driver lock; make **two concurrent decode contexts** correct.

**Gate:** ✅ H.264 + VP9 remain conformance-vector bit-exact; **no per-frame
copy** (verify via perf counters / memory bandwidth); clean under ASan **and**
TSan; two simultaneous decoders in one process decode correctly; multi-hour
4K soak with flat memory and no fd leaks.

### Phase 2 — HEVC decode + 10-bit / HDR  (~2–3 wk)

- **HEVC decode:** reconstruct VPS/SPS/PPS from `VAPictureParameterBufferHEVC`
  + `VASliceParameterBufferHEVC`, including **scaling lists** from
  `VAIQMatrixBufferHEVC` (HEVC scaling data *is* in the VA buffers, so this is
  spec-honest, unlike the H.264 PoC shortcuts). Main first, then Main10.
- **10-bit path:** NV15→P010 via RGA (`convert.c`); wire P010 export for all
  consumers.
- Backfill **H.264 spec-honesty**: honor `VAIQMatrixBufferH264` scaling
  matrices, derive level from the stream instead of hardcoding 5.1.

**Progress (2026-07-22, host slice):** H.264 scaling-list reconstruction was
already present; SPS level selection now derives the lowest representable
Annex A level from frame size, DPB size, and the preserved bi-pred constraint.
HEVC Main/Main10 VPS/SPS/PPS reconstruction, scaling lists, current RPS
materialization, slice PPS-ID parsing, Annex B packet assembly, and
display-reordering-safe token routing are implemented fail-closed. Normal,
ASan/UBSan, Valgrind, and clang-tidy checks pass, and FFmpeg's independent
`trace_headers` parser accepts both Main and Main10 bundles (including 10-bit
scaling data). Eight checksum-pinned FFmpeg FATE HEVC Main conformance streams
are in the manifest but deliberately remain `software-fallback`: no HEVC
profile is advertised until the full on-device bit-exact gate passes. The
post-slice Phase 0 hardware regression is green normally and under ASan/UBSan,
but that proves only the already-shipping profiles and HEVC fallback contract;
HEVC Main hardware output remains pending rather than inferred from it.

**Progress (2026-07-26, Main10 hardware slice):** A narrow
`RK_VAAPI_EXPERIMENTAL_PROFILES=hevc-main10` gate now exposes
`VAProfileHEVCMain10` with `VA_RT_FORMAT_YUV420_10`. Context creation requests
`MPP_FRAME_FBC_AFBC_V2`; this is required because VDPU383 reports a 448-byte
linear NV15 stride for 320 pixels, which cannot be represented by librga's
64-pixel-aligned compact format. The decode worker validates AFBC metadata,
uses `mpp_frame_get_fbc_hdr_stride()` as the RGA pixel stride, and applies
MPP's `offset_x`/`offset_y` to the source rectangle before writing a
driver-owned linear P010 buffer. Ignoring the measured four-row AFBC offset
produced a shifted image at 21.7 dB PSNR; honoring it is byte-exact.

`make check-hevc-main10-experimental` generates a 48-frame Main10 stream and
runs the pinned FATE `WP_A_MAIN10_Toshiba_3.bit` weighted-prediction vector
(416x240, 256 frames), compares downloaded P010 bytes against software decode,
and audits one AFBC conversion per frame. Both pass bit-exactly on the
2026-07-25 kernel/librga stack. Direct-backend triage rejected three other
candidate vectors rather than laundering them through the VA gate:
`DBLK_A_MAIN10_VIXS_2.bit` duplicated/missed direct-MPP frames,
`WPP_A_ericsson_MAIN10_2.bit` emitted an extra direct-MPP frame, and
`TSUNEQBD_A_MAIN10_Technicolor_2.bit` uses unequal luma/chroma bit depths that
FFmpeg rejects and P010 cannot represent. The profile remains hidden by
default until broader Main10 conformance and HDR metadata/playback gates pass;
VP9 Profile 2 remains unadvertised and is not covered by this result.

**Progress (2026-07-26, VP9 Profile 2 hardware slice):** The VP9
uncompressed-header parser and synthetic hidden-reference repeat now support
Profiles 0 and 2 with exact profile matching. Profile 2 context creation is
available only under `RK_VAAPI_EXPERIMENTAL_PROFILES=vp9-profile2`, reports
`VA_RT_FORMAT_YUV420_10`, and requires the same MPP AFBC-to-RGA P010 path as
HEVC Main10. A generated lossless 48-frame 320x240 gate is byte-identical to
software and audits 48 AFBC conversions. A direct RKMPP AFBC plus RGA
discriminator is also byte-identical; direct linear NV15 misread as P010 is
not.

The checksum-pinned official WebM/libvpx
`vp92-2-20-10bit-yuv420.webm` Profile 2 vector is also P010 bit-exact for all
10 displayed frames. Its driver audit requires 11 AFBC conversions, retaining
coverage of the additional hidden/reference decode output instead of equating
decoder outputs with display count. The default conformance gate keeps this
profile on software fallback; Profile 2 remains hidden pending app validation.

**Progress (2026-07-26, HEVC hardware gate):** Added a gated HEVC Main
validation path without advertising HEVC by default:
`RK_VAAPI_EXPERIMENTAL_PROFILES=hevc-main` enables `VAProfileHEVCMain`, and
`make check-hevc-experimental` runs only the pinned HEVC conformance vectors
with fail-fast FFmpeg timeouts. The RPS rewrite now keeps valid
`ReferenceFrames[]` entries that are not current-picture refs as follow
references (`used_by_curr_pic=0`), preserving DPB state needed by later pictures.
Together with PPS emission before every access unit, the initial forced
hardware sweep had five bit-exact HEVC Main vectors:
`PPS_A_qualcomm_7.bit`, `RPS_A_docomo_4.bit`, `VPSID_A_VIDYO_2.bit`,
`WPP_A_ericsson_MAIN_2.bit`, and `WP_A_Toshiba_3.bit`.

**Progress (2026-07-26, HEVC 7/8):** SPS-backed short- and long-term references
are now supported without reconstructing VA-hidden SPS tables. The slice
rewriter consumes the original table-selection syntax using the VA-provided
counts, then materializes the current DPB reference state from
`ReferenceFrames[]` as an explicit slice RPS. `LTRPSPS_A_Qualcomm_1.bit` is
bit-exact for all 500 frames.

Scaling matrices now live in the reconstructed PPS, matching the
picture-scoped `VAIQMatrixBufferHEVC`, while the SPS only enables their use.
The PPS is emitted before every access unit. VPS/SPS bytes are regenerated and
compared with the last successfully queued sequence state, so genuine sequence
changes are propagated without resetting MPP's DPB at every CRA.
`SLIST_A_Sony_4.bit` is consequently bit-exact for all 65 frames, including its
post-CRA RASL pictures.

The complete forced-hardware sweep is now 7/8 bit-exact. The gate remains
non-green only for `TILES_A_Cisco_2.bit`: direct MPP decode of the original
Annex B stream reports `errinfo=1` from frame 1 onward, and the reconstructed
VA-API path reports the same backend error. The driver maps
errored/discarded MPP frames to `VA_STATUS_ERROR_DECODING_ERROR` and resets the
decoder during teardown so in-flight MPP buffers are not leaked. The safe
advertised hardware subset (`check-safe`) still passes with HEVC software
fallback and the risky VP9 vector blocked. HEVC Main stays hidden.

**Progress (2026-07-26, HDR10 metadata slice):**
`make check-hevc-main10-hdr-experimental` generates a 24-frame Main10 HDR10
stream and validates the complete hardware-frame boundary. Its downloaded
P010 bytes are identical to software decode, with one audited AFBC-to-RGA
conversion per frame. Every hardware-decoded frame retains limited-range
BT.2020 non-constant-luminance color, BT.2020 primaries, SMPTE ST 2084 (PQ),
the expected mastering-display chromaticities and 0.0001-1000 nit luminance,
and MaxCLL 1000/MaxFALL 400 metadata.

The original HEVC VUI and prefix/suffix SEI messages are parsed by libavcodec
before VA submission and carried on its output `AVFrame`; they do not need to
be reconstructed into the SPS sent privately to MPP. VA's HEVC picture
parameters do not expose those original syntax elements, so the reconstructed
SPS deliberately keeps `vui_parameters_present_flag=0`. This contract is now
tested instead of depending on MPP to reproduce application-facing metadata.
Actual HDR presentation in Firefox and mpv remains an app/display-system gate.

**Progress (2026-07-26, P010 consumer contract):** Surface creation now
validates the requested RT format and `VASurfaceAttribPixelFormat`, records
NV12 or P010 before decode, and sizes the placeholder DMA-BUF for its declared
linear layout. A pre-decode P010 export therefore reports P010 rather than
NV12 and is valid in both composed P010 and split R16/GR1616 forms. The
driver-object gate checks both descriptors normally and under ASan/UBSan; the
HDR Main10 and shipping-profile hardware regressions remain green.

**Gate:** HEVC Main bit-exact vs software on conformance vectors; HEVC Main10 /
VP9 P2 bit-exact after P010 repacking; HDR HEVC plays correctly in Firefox and
mpv on-device.

### Phase 3 — Production hardening & the app matrix  (~2–3 wk)

Make it real software on real apps.

- **Firefox:** ship a proper **RDD sandbox policy patch** (whitelist the MPP
  `'v'` ioctl family + dma-heap `'H'`) as a packaged, documented alternative to
  `MOZ_DISABLE_RDD_SANDBOX=1`.
- **Chromium:** validate stock-build VA-API behind runtime flags; test the
  **`/dev/dri/` device-node aliasing** sidestep for the deb build (does not
  need a Chromium patch if the GPU sandbox allows the ioctl); document the snap
  device-cgroup caveat.
- **App matrix on-device:** Firefox, Chromium, VLC (`hw/vaapi`), mpv
  (`--hwdec=vaapi`), GStreamer `va`, stock `ffmpeg -hwaccel vaapi` — each
  playing 8-bit and 10-bit H.264/HEVC/VP9.
- Full **conformance-vector suite** as the CI hardware gate; **soak &
  leak** runs; structured logging finalized.
- **Packaging:** versioned release + PPA; a small config package for the
  per-app enablement (flags/env/policy).

**Progress (2026-07-26, GStreamer app slice):** Stock GStreamer 1.28's `va`
plugin loads the local driver and registers `vah264dec`, `vah265dec`, and
`vavp9dec` when its supported `GST_VA_ALL_DRIVERS=1` override permits the
Rockchip vendor string. `make check-gstreamer-va` negotiates system-memory
NV12/P010 output and is byte-identical to software for pinned H.264 High
(10 frames), VP9 Profile 0 (1 frame), VP9 Profile 2 (10 displayed/11 decoded
frames), and HEVC Main10 (256 frames). Driver audits show no stale routes,
decode errors, buffer mismatch, or linear 10-bit fallback. DMABuf display-sink
and HDR presentation remain open, so this closes one app/readback slice rather
than the GStreamer matrix row.

**Progress (2026-07-26, packaging slice):** Debian packaging now declares its
RGA build dependency and produces separate `rockchip-vaapi` and
`rockchip-vaapi-config` packages. The optional config package selects the
driver and enables GStreamer's supported vendor override, but does not alter
browser sandboxes or display backends. Upgrading the driver removes legacy
ysp2 environment files that globally disabled Firefox's RDD sandbox. The
Firefox policy patch and clean-image install test remain open.

**Gate:** the app matrix passes on-device; conformance suite green; clean soak;
`.deb` + config packages install and enable HW decode from a clean image.

### Phase 4 — Encode (`VAEntrypointEncSlice`)  (~3–4 wk)

A second subsystem over MPP's rkvenc2.

- **H.264 encode first, then HEVC:** map `VAEncSequenceParameterBuffer*` /
  `VAEncPictureParameterBuffer*` / `VAEncSliceParameterBuffer*` and the coded
  buffer onto `mpp_enc_cfg_*` + `encode_put_frame`/`encode_get_packet`;
  return the bitstream (with `MPP_ENC_GET_HDR_SYNC` headers) in the VA coded
  buffer.
- **Rate control:** VA CBR/VBR/CQP → `rc:mode`; expose QP, GOP, bitrate,
  profile/level, keyframe forcing.
- Input: accept app raw surfaces (NV12/others), RGA-convert as needed.
- **Cross-dependency:** coordinate with the ROCK 5B kernel **RKVENC2 slice-FIFO
  overflow** hardening (a known forward-port finding) before advertising
  low-delay/multi-slice encode.

**Progress (2026-07-26, H.264/HEVC frame encode):** A hidden
`RK_VAAPI_EXPERIMENTAL_ENCODE` path now exposes H.264 Main/High and HEVC Main
`VAEntrypointEncSlice`, accepts checked NV12 uploads, maps VA
sequence/picture/slice plus frame-rate/rate-control state to MPP, and returns
MPP-generated Annex B through `VACodedBufferSegment`. HEVC additionally
advertises MPP's native 64x64 CTU/block-size contract. Stock FFmpeg
`h264_vaapi` produces 48/48 interoperable High-profile frames in CQP, CBR, and
VBR modes at 48.50, 46.26, and 45.16 dB average PSNR. GStreamer 1.28 registers
`vah264enc` and passes the same 48-frame High-profile round trip. `hevc_vaapi`
produces parser-clean 48/48 Main-profile streams at 45.19, 44.46, and 40.91 dB;
`vah265enc` passes at 45.31 dB. Normal and ASan/UBSan app gates pass for both
codecs. Concurrent 96-frame H.264 and HEVC encoder runs also pass while the
complete shipping synthetic decode matrix runs in parallel. The implementation
intentionally exposes one full-frame slice only; additional input formats,
WebRTC, multi-slice, and long encode soak remain open.

**Gate:** encode → standard-decoder round-trip within a PSNR bound;
interoperable bitstreams (ffmpeg/browsers decode them); GStreamer `vah264enc` /
`ffmpeg -c:v h264_vaapi` / a WebRTC send path work on-device; encode contexts
concurrent with decode contexts are race-free.

### Phase 5 — Release & maintenance  (ongoing)

- Tagged release, GitHub Release + PPA, changelog discipline.
- **Rebase/regression discipline:** re-run the conformance + app matrix on each
  libva/MPP/kernel bump; keep the sandbox patches current per browser
  milestone.
- Offer the correctness fixes and the driver upstream (libva ecosystem /
  original author) if there's appetite.

---

## Cross-cutting concerns

- **Testing:** `tests/validate.sh` grows from synthetic smoke into a
  conformance-vector + soak + encode-round-trip suite, driven by `make check`.
  Decode is bit-exact-gated; encode is PSNR+interop-gated.
- **CI:** build + clang-tidy + ASan on push (cloud); conformance + app matrix on
  a self-hosted ROCK 5B (manual/scheduled). Never claim a hardware result the
  CI didn't produce.
- **Static/dynamic analysis:** clang-tidy, `-Wall -Wextra -Werror`, ASan/UBSan
  on the test path, periodic Valgrind/TSan. Non-negotiable given the in-process
  browser threat model.
- **Security:** the driver hands a semi-trusted media process ioctl access to
  `/dev/mpp_service`, whose BSP-side input validation is below mainline. Treat
  every VA buffer as hostile input; fuzz the bitstream reconstructors. Document
  the sandbox trade-off honestly (it does not disappear — it moves).
- **Docs:** keep `DEVELOPMENT.md` (architecture), this roadmap (plan), and a
  `TESTING.md` (how to run the gates) current as the source of truth.

## Risks & open questions

- **External buffer group parity:** resolved for shipping H.264 and VP9 on the
  pinned MPP/ROCK 5B stack. HEVC must repeat the parity gate when its decode
  path lands; the internal-group ref-holding fallback remains zero-copy.
- **10-bit exactness:** resolved for the generated HEVC Main10 and VP9 Profile
  2 AFBC paths, pinned Main10 weighted-prediction and official VP9 Profile 2
  vectors, and the 24-frame HDR10 vector. RGA performs a pure NV15-to-P010
  repack and every gate is byte-exact. Static BT.2020/PQ HDR metadata survives
  the hardware frame path; broader HEVC conformance and app/display HDR
  presentation remain open.
- **Encode conformance:** encoders aren't spec-exact; the gate must be
  round-trip PSNR + interop, and depends on the kernel RKVENC2 hardening.
- **Sandbox upstreamability:** the Firefox RDD policy patch is small but must be
  re-verified per milestone; the Chromium aliasing sidestep depends on the GPU
  sandbox continuing to allow `ioctl` without arg inspection — verify against
  the shipping Chromium, don't assume.
- **MPP threading contract:** the dedicated-worker model is validated for the
  normal single-decoder H.264/VP9 matrix, nine simultaneous idle MPP contexts,
  and two active H.264/VP9 contexts in one process under normal, ASan/UBSan,
  and TSan builds.

## Status

- Phase 0: complete on `main`; the normal and sanitized full hardware gates are
  green after the Phase 2 slice on audited fixed kernel build `#4`.
- Phase 1: complete on `main`; object heap/object migrations, external-buffer
  zero-copy, worker/fence synchronization, module separation, two active
  decoders, sanitizer gates, and the multi-hour 4K resource soak are green.
- Phase 2: in progress; the first host reconstruction/routing slice is green,
  the fail-fast experimental HEVC Main hardware gate is 7/8 bit-exact, and the
  generated 48-frame Main10/Profile 2, pinned 256-frame Main10, official
  10-frame Profile 2, and 24-frame Main10 HDR10 AFBC-to-P010 gates are
  bit-exact. Static BT.2020/PQ HDR metadata is preserved. HEVC Main remains
  hidden on the direct-MPP TILES failure; both 10-bit profiles remain hidden
  while broader HEVC conformance and app/display validation are open.
- Phase 3: in progress; the stock GStreamer 1.28 `va` plugin system-memory
  gate is byte-exact for H.264, HEVC Main10, and VP9 Profiles 0/2. Split
  driver/config Debian packaging no longer weakens Firefox's sandbox globally.
  Display sinks, the other desktop apps, Firefox policy, and clean-image
  package validation remain open.
- Phase 4: in progress; experimental one-slice H.264 Main/High and HEVC Main
  encode pass FFmpeg and GStreamer CQP/CBR/VBR interoperability, parser, and
  PSNR gates normally and under ASan/UBSan. Both 96-frame encoder gates pass
  together with the shipping decode matrix. Broader inputs, WebRTC,
  multi-slice, and long encode soak remain open.
- Phase 5: planned.

Tracked in the ROCK 5B project as status **track 14** with the enablement
map and driver-review finding as the decision/evidence record.
