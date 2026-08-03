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

**Follow-up (2026-07-28, the soak audit was broken):** `check-soak`,
`check-zero-copy` and `check-concurrent-decode` all counted external frames by
matching the literal string `zero_copy=1 external=1`, which stopped appearing
when `converted_10bit` was inserted between those fields on 2026-07-25. They
failed closed rather than passing wrongly, but none of them can have passed
since. With the audit repaired, a 1,800-second paced 4K soak completed 54,005
external frames with post-warmup RSS moving from 164,876 KiB to 165,344 KiB --
a 468 KiB span, against 47,844 KiB in the run recorded above -- and fd
head/tail medians both 54. The gate still reports it as a smoke run, because
the Phase 1 exit criterion is 7,200 seconds.

**Follow-up (2026-07-29, exact Published package root):** The repaired gate
completed the full 7,200-second exit duration against checksum-verified
Published MPP `3381fd2c` and FFmpeg `33a651a55b` packages extracted from the
live PPA. It decoded 216,005 external 4K frames; RSS moved from 169,248 KiB to
139,776 KiB with a 53,592 KiB transient span and no growth, while fd
head/tail medians moved from 57 to 54 with a bounded 29-fd span. This closes
the exact-package long-runtime gate independently of host installation.

**Follow-up (2026-07-28, three fixed ceilings removed):** Widening the HEVC
conformance evidence (Phase 2 below) exposed three limits in this core that
were not reachable from the H.264/VP9 matrix. All three are fixed and none was
a codec-specific problem:

- `RenderPicture` capped one picture at 64 buffers, so a legal many-slice
  stream became `VA_STATUS_ERROR_MAX_NUM_EXCEEDED`. The pending list now grows;
  the remaining ceiling only bounds hostile input.
- Context teardown cancelled in-flight decodes. VA surfaces belong to the
  display, not the context, and applications legitimately destroy a decode
  context on a sequence change and then sync surfaces the old context was still
  filling — `SLIST_B_Sony_8.bit` failed this way in three of six runs. Teardown
  now drains submitted work under a 3-second deadline, so a wedged backend
  still cannot block `vaDestroyContext`.
- The external buffer pool was a fixed 24 frames and cannot be sized up front:
  surfaces are created independently of the context, and FFmpeg passes no
  render targets to `vaCreateContext`. Frame-threaded FFmpeg allocated 29
  surfaces, every pool buffer ended up bound to a surface the application still
  held, and MPP waited forever for a free one — a hard deadlock that hung the
  process. The pool now grows on demand to a 64-frame ceiling and reports a
  real error instead of stalling if a consumer holds even that many.

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
`make check-hevc` (then `check-hevc-experimental`) runs only the pinned HEVC vectors
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
fallback and the risky VP9 vector blocked. HEVC Main stayed hidden at this
point; it was advertised on 2026-07-28 once the backend decoded TILES and the
wider sweep landed (see below).

**Progress (2026-07-27, TILES backend reduction tooling):** A libva-free
`tests/hevc_mpp_repro` now submits Annex-B HEVC directly to MPP, reports every
frame's `errinfo`/discard/EOS state, and uses distinct clean, stream-failure,
environment, and runtime exit classes. `tests/minimize-hevc-tiles.sh` gates on
a checksum-pinned known-good HEVC control before accepting the full TILES
failure, then tests software-valid access-unit prefixes and strips nonessential
NAL classes. Host analysis reduces the first candidate boundary to a
software-valid two-packet stream: VPS/SPS/PPS/IDR followed by a replacement PPS
and one P-picture. The PPS transition changes between two non-uniform 5×5 tile
layouts and toggles cross-tile filtering. Confirmation on a healthy backend is
pending: the 2026-07-27 KASAN kernel failed the known-good control with
`ENODEV`/`EIO`, and the reducer correctly stopped without attributing that
failure to TILES. See `docs/HEVC_TILES_BACKEND.md`.

**Progress (2026-07-28, TILES backend fixed and the sweep widened):** On the
current stack -- kernel `6.18.40-ysp-rockchip64` (notes
`db18acdddf7ba9de84590a5816911ed2d929643980057d639a90c2b1337d900c`,
package `6.18.40+rk3588av1fwport20260725-0ubuntu1~rk2`) with
`librockchip-mpp 1.5.0+git20260727.d8c6b88a` -- `make check-hevc-tiles-backend`
passes: the known-good control, the complete 100-frame TILES vector, and the
reduced two-picture same-ID PPS core all decode cleanly through direct MPP. The
`errinfo=1` that blocked `TILES_A_Cisco_2.bit` is gone. The experimental HEVC
Main gate is consequently **8/8 bit-exact**, deterministic across three runs
and green with the complete ASan/UBSan driver.

`tests/sweep-hevc-conformance.sh` then widened the evidence from the eight
pinned vectors to all 163 HEVC Main candidates in the FFmpeg FATE conformance
suite. It found nine failures, of which one was a comparison artifact and four
were fixed driver limits; see the Phase 1 note below. After those fixes the
sweep is **134 bit-exact, zero driver failures**. The remainder are 17
candidates that are not Main 8-bit 4:2:0, `PICSIZE_A_Bossen_1.bit` at
1056x8440 which the driver correctly refuses against its advertised
7680x4320 constraint, and streams direct MPP cannot decode either.
`make check-hevc-conformance-sweep` pins every class in
`tests/hevc-sweep-vectors.tsv` and fails on divergence in either direction.

**Progress (2026-07-29, NUT random-access and RPS fixes):**
`NUT_A_ericsson_4.bit` and `NUT_A_ericsson_5.bit` are not independent
payloads: both are 302,142 bytes with SHA-256
`d87dcae6353a680ff1c816395b578afae3ed9f1a88b56b07a24e62333e0621b7`.
The stream contains 36 VCL access units and should output 34 pictures after
two RASL pictures are suppressed.

Direct MPP at `d8c6b88a` returned only 27 pictures because its random-access
gate suppressed every non-IRAP picture below the IRAP POC, incorrectly
discarding seven valid RADL pictures along with the two RASL pictures. MPP
`3381fd2c` removes that broad condition while retaining the explicit RASL
test. Both sample names then return 34 clean direct-MPP frames, zero
error/discard flags, and EOS.

The YSP FFmpeg 8.0/8.1 lines had a separate software-side failure: unavailable
references in the unused `ST_FOLL`/`LT_FOLL` sets were made fatal unless
corrupt output was requested. Upstream FFmpeg fix `265d39e551` implements the
HEVC 8.3.3 requirement to generate those following pictures and removes the
global FATE `output_corrupt` workaround. It is present in YSP's maintained
`ffmpeg-80@ab675f19cf`, `ffmpeg-81@629f4968d2`, and package line
`fix/rkmpp-output-timeout@33a651a55b`.

A focused source-stack VA-API run with both fixes produced 34 software and 34
hardware frames with byte-identical per-frame MD5s. The manifest therefore
advances both FATE names from `backend` to `exact`: the source expectation is
now **144 exact, 17 non-Main skips, two size-contract refusals, and zero
backend or driver failures**.

The complete 163-vector sweep was then rerun against an isolated extraction of
the exact Published arm64 packages: MPP
`1.5.0+git20260729.3381fd2c+ds-0ubuntu1~rk1` and FFmpeg
`7:8.0.3+rockchip+git20260729.33a651a55b-0ubuntu1~rk1`. Loader inspection
confirmed that FFmpeg, the direct-MPP classifier, and this driver all resolved
the extracted MPP library. The result matches every pinned class: **144 exact,
17 skips, two size refusals, zero backend failures, and zero driver failures**.
The report SHA-256 is
`39c68fdf82773fb9bde47dbcde74abc3ea49271905b203aad1a1c81f1452cf89`.
The full shipping-profile matrix is also green on the same packages with the
audited kernel-crash vector enabled by its exact release and notes
fingerprint. System-wide installation remains a package-lifecycle gate, not a
decode-correctness gap. The root-cause record is
[in the YSP findings repository](https://github.com/yisding/rock-5b-ysp/blob/main/findings/2026-07-29-hevc-nut-radl-and-unused-rps-reference-fixes.md).

**Progress (2026-07-28, Main10 conformance widened):** `PROFILE=main10` runs
the same sweep over the FATE Main10 candidates with P010 comparison, pinned in
`tests/hevc-main10-sweep-vectors.tsv` and re-runnable as `make
check-hevc-main10-conformance-sweep`. Ten of the eleven real Main10 streams are
bit-exact, including the `DBLK_A_MAIN10_VIXS` and `WPP_A_ericsson_MAIN10`
vectors that direct-backend triage had rejected on 2026-07-26.

It found one driver defect and one hardware boundary. `init_qp_minus26` was
bounded at -26, which is the 8-bit range: the floor is -(26 + QpBdOffsetY) and
QpBdOffsetY grows with luma bit depth (7.4.3.3.1), so legal Main10 streams were
rejected as unreconstructable -- `INITQP_B_Sony_1.bit` among them, now
bit-exact. `WPP_D_ericsson_MAIN10_2.bit` at 64x240 is the remaining
hardware-path exception: the AFBC NV15-to-P010 repack that every 10-bit
surface depends on requires RGA3, whose input and output active-width minimum
is 68. The driver now returns `VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED` at
context creation, so FFmpeg falls back before MPP or RGA setup rather than
failing mid-decode.

Main10 stays unadvertised. Decode correctness is now well evidenced, but the
narrow-picture hardware path remains unavailable and HDR display presentation
is still unvalidated.

**Progress (2026-07-29, 10-bit throughput):**
`make check-10bit-throughput-experimental` measures the complete 1920x1080
hardware path rather than timing the decoder backend in isolation. HEVC Main10
completed 240 visible and decoded frames at 261.38 fps. VP9 Profile 2 completed
240 visible frames, 254 decoded frames, and 254 audited AFBC-to-RGA
conversions at 261.08 fps; the additional outputs are legitimate hidden
references. This closes the throughput question without changing the
profiles' default-hidden status.

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
Virtual-output HDR-tagged P010 presentation now passes in Firefox and mpv;
physical HDR-monitor passthrough remains an app/display-system gate.

**Progress (2026-07-26, P010 consumer contract):** Surface creation now
validates the requested RT format and `VASurfaceAttribPixelFormat`, records
NV12 or P010 before decode, and sizes the placeholder DMA-BUF for its declared
linear layout. A pre-decode P010 export therefore reports P010 rather than
NV12 and is valid in both composed P010 and split R16/GR1616 forms. The
driver-object gate checks both descriptors normally and under ASan/UBSan; the
HDR Main10 and shipping-profile hardware regressions remain green.

**Progress (2026-07-29, P010 derive/display consumers):**
`vaDeriveImage` now accepts completed driver-owned linear P010 surfaces and
the 64-pixel/16-row-aligned provisional P010 layout used by VLC's converter
probe. It still refuses imported P010, compressed backing, and stale or
unaligned provisional layouts. Stock VLC hardware-decodes and presents
H.264 High, HEVC Main, and HEVC Main10. Firefox 153 hardware-decodes H.264 and
HEVC Main, while the first recorded stock Firefox Main10 run reached three
hardware frames and then fell back at its
plane-1 EGL import. Source inspection later identified the producer defect:
the driver exported `0x36315247` (`GR16`) instead of
`DRM_FORMAT_GR1616` (`0x32335247`, `GR32`), so Mesa correctly rejected an
unknown fourcc before reaching Panfrost. The corrected ysp7 exporter has since
reached successful plane-1 zero-copy EGL import in both Firefox's RDD and
parent renderer. Later decode failure is below that EGL boundary.

**Gate:** ✅ HEVC Main bit-exact vs software on conformance vectors (8/8 pinned;
144/163 complete exact-PPA-package sweep, 17 profile skips, two size refusals,
zero backend/driver failures) and advertised by default;
✅ HEVC Main10 / VP9 P2 bit-exact after P010 repacking and measured above
260 fps at 1080p; ✅ all five codec/profile cases presented by VLC, Firefox,
and mpv on a virtual output; ✅ Firefox/Panfrost GR1616 import in both
processes; physical HDR presentation remains open.

### Phase 3 — Production hardening & the app matrix  (~2–3 wk)

Make it real software on real apps.

- **Firefox:** ship a proper **RDD sandbox policy patch** for the MPP/RGA
  requests and brokered Rockchip device paths as a packaged, documented
  alternative to `MOZ_DISABLE_RDD_SANDBOX=1`.
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

**Progress (2026-07-26, VLC environment probe):** Stock VLC 3.0.23 includes
its `vaapi`/`vaapi_drm` decoder plugins and libavcodec selects VAAPI as an
available H.264 output format. In the current headless login, however, dummy
video output supplies no hardware decoder device; VLC reports `no hw decoder
modules matched`, falls back to software, and never loads this driver. There is
no Wayland/X11 socket in the session. A valid VLC gate therefore requires a
real display/DRM session; headless playback success is not hardware evidence.

**Progress (2026-07-28, VLC display session closed):** The 2026-07-26 probe
found VLC loading no hardware decoder in a headless login. With a real GNOME
Wayland session and its Xwayland display, the actual blocker turned out to be
the driver, not the environment: VLC's OpenGL VA-API converter derives an image
from the decoded surface and imports its buffer handle as an EGLImage, and both
`vaDeriveImage` and `vaAcquireBufferHandle` were stubs. VLC therefore dropped
`glconv_vaapi_x11`, reported "no hw decoder modules matched", and fell back to
software after creating 38 surfaces through this driver.

`vaDeriveImage` now returns a VAImage that aliases the surface's own linear
NV12/P010 DMA-BUF, with mapping bracketed by dma-buf CPU synchronization, and
`vaAcquireBufferHandle` exports that buffer as DRM PRIME. Imported RGB and AFBC
layouts fail closed because they are not describable as a VAImage. Stock VLC
3.0.23 now reports `using hw decoder module "vaapi"` and `Using Rockchip MPP
VA-API Driver 0.1 for hardware decoding`, and `make check-vlc-display` gates
H.264 High and HEVC Main playback on that plus external-pool frame counts and
a clean driver log. The gate refuses to run headless.

Building it also closed two decode-path gaps. MPP's display reordering was
holding finished H.264 pictures for later input even though this driver routes
outputs back to their surface by token, so immediate output is now enabled for
H.264 as well as HEVC -- validated against the full conformance, 1,440-frame
zero-copy, and two-context concurrent gates. Teardown now also signals
end-of-stream to MPP and stops as soon as MPP marks the stream ended, instead
of always waiting out its deadline.

**Progress (2026-07-28, Firefox and Chromium rows):** `make
check-firefox-decode` plays generated H.264 High and HEVC Main clips in stock
Firefox 153.0 and requires external-pool frames plus DMA-BUF exports with a
clean driver log; both codecs pass with hundreds of frames and zero error
markers. It runs with `MOZ_DISABLE_RDD_SANDBOX=1` and reports that in its own
output, so it closes the decode/export row and not the sandbox row. The
hash-pinned RDD source patch in `contrib/firefox` has been rebased onto
`FIREFOX_153_0_RELEASE` -- it applies cleanly, produces sources byte-identical
to applying the 152.0.6 patch, and 153.0 was confirmed not to permit any of
those paths or requests already. The ioctl set is inherited from the 152.0.6
measurement rather than remeasured, which needs a Firefox source build.

**Progress (2026-08-01, Chromium 151 boundary):** The Chromium 150 ANGLE
failure is gone. Chromium 151 creates an accelerated ANGLE OpenGL ES context
over Mali-G610/Panfrost on Wayland and plays the generated H.264 page. The
remaining blocker is earlier than this driver: its live GPU report has an
empty hardware-decoding profile list, media-internals selects
`FFmpegVideoDecoder` with `kIsPlatformVideoDecoder=false`, and the Rockchip
driver is never loaded. The arm64 binary contains its V4L2 decoder backend but
not the libva implementation. The available Hantro V4L2 node advertises only
MPEG-2 and VP8 compressed input, so it cannot substitute for MPP on the v1
codec set. Runtime feature flags can turn the GPU feature label on but cannot
add the missing backend/profiles. Stock Chromium VA-API therefore requires a
distro build with libva enabled; the proposed device-node alias cannot solve
this binary.

**Progress (2026-07-29, mpv/Panfrost presentation slice):** Stock mpv 0.41.0
selects VA-API decode and gpu-next's Wayland/OpenGL presentation for H.264.
The first real probe exposed a driver/display contract gap: MPP emitted the
352x288 CIF frame with a 352-byte linear NV12 pitch, which Panfrost refused as
`WSI pitch not properly aligned`; every EGL import and render failed even
though hardware decode itself succeeded. Forcing a wider layout inside MPP
was rejected because it caused a stream info-change on every frame.

The driver now retains MPP's normal external-pool zero-copy decode and, only
when exporting a decoded driver-owned 8-bit NV12 surface whose pitch is not
64-byte aligned, uses RGA to repack the active image into a cached aligned
DMA-BUF once per surface fence. Compatible H.264, HEVC, and VP9 layouts remain
direct exports. The hardware object test proves every active luma/chroma byte
survives a 352-to-384-byte repack. `make check-mpv-display` then renders a
complete generated 20-frame H.264 High CIF clip through mpv with one MPP
info-change, 20 external frames, 20 repacks, no hardware-frame download, and
no pitch, EGL-import, mapping, or render error. This closes the 8-bit mpv
presentation slice; 10-bit/HDR presentation is still open.

**Progress (2026-07-27, packaging slice):** Debian packaging now declares its
RGA build dependency and produces separate `rockchip-vaapi` and
`rockchip-vaapi-config` packages. The optional config package selects the
driver and enables GStreamer's supported vendor override, but does not alter
browser sandboxes or display backends. Upgrading the driver removes legacy
ysp2 environment files that globally disabled Firefox's RDD sandbox. A
Lintian-backed isolated-root gate now validates package metadata, payloads,
clean install, driver upgrade, config-only purge, reinstall, and full purge
against an empty package database. Fresh-image hardware decode remains open.

**Progress (2026-07-29, installed ysp5 gate):** Source commit `491533e` adds
the up-front sub-68-pixel 10-bit context refusal and matching RGA pre-submit
guard; package commit `4e6e99b` versions it as `1.0.11+ysp5`. The driver and
config packages pass their isolated install/upgrade/purge gate and are now
installed on the board. The installed shared object's SHA-256 is byte-identical
to the deb payload. Against the exact production kernel/notes fingerprint, the
64x240 Main10 case software-decodes all 48 frames after one context refusal
with zero RGA submissions or kernel `no core match`, and the complete pinned
conformance gate passes including the guarded VP9 hidden-reference vector.
This closes the stale-installed-driver gap; a genuinely clean-image install is
still separate evidence.

**Progress (2026-07-26, Firefox RDD policy source slice):** The exact Firefox
152.0.6 RDD policy was audited and a hash-pinned distribution source patch now
adds broker access for existing `/dev/mpp_service`, `/dev/rga`, and
`/dev/dma_heap` nodes while preserving the sandbox. Seccomp permits only the
MPP v1 command and three RGA requests measured across the H.264 encode and
HEVC Main10 decode/RGA gates. Firefox already allows DMA-BUF ioctls and, on
arm64, dma-heap's `'H'` ioctl family through its Tegra policy; the missing
piece was the dma-heap broker path. Patch application is checked against exact
upstream source hashes. Building/installing the Firefox package and validating
RDD playback in a real display session remain open.

**Progress (2026-07-30, P010 exporter correction):** The split-P010 exporter
now uses `DRM_FORMAT_*` macros rather than hand-written fourcc literals. This
changes chroma from the invalid `0x36315247` to
`DRM_FORMAT_GR1616` (`0x32335247`) and corrects the test that had preserved the
bad value. The speculative Firefox GR/RG retry patches and their contract gate
were retired; the display gate now requires the correct exported fourcc and a
successful plane-1 zero-copy import. Installed `1.0.11+ysp7` reaches that
success in both the Firefox RDD process and parent renderer. Playback later
fails when MPP marks a decoded Main10 frame bad, so the GR1616/Mesa question is
closed independently of the remaining decoder failure.

**Progress (2026-07-31, Main10 failure reducer):** The new control-gated prefix
reducer retained Firefox-style cases with two and six access units in separate
runs. In both, the original prefix and exact concatenated driver output were
clean in three of three whole-stream direct-MPP controls while the VA path
produced MPP bad-frame markers in two of three attempts. For the six-packet
case, exact-boundary replay below libva was clean in three of three
external-linear runs but emitted bad frames in one of three external-AFBC and
three of three internal-AFBC runs. The final two-packet run was clean in all
three replay modes. This rules out an always-invalid reconstructed stream and
shows a stateful MPP packetized/session interaction, with AFBC implicated in
one run but not deterministically isolated. No individual parameter-set
rewrite is yet implicated. RGA multi-SG refusals are classified separately and
never count toward the reducer result.

**Progress (2026-08-01, complete virtual-output app matrix):** Mutter 50's
headless virtual monitor supplies a real Wayland output backed by the board's
Panthor/Panfrost GBM renderer; enabling its Xwayland server also gives VLC 3 a
valid X11 EGL target. The expanded gates exercise H.264 High, HEVC Main, VP9
Profile 0, HEVC Main10, and VP9 Profile 2 rather than only their earlier
subsets. Stock mpv 0.41 presents 20 frames of every case through VA-API and
Panfrost, including the CIF NV12 repack, both P010 paths, and BT.2020/PQ-tagged
Main10 input. Stock VLC 3.0.23 presents 120 external-pool frames of every case.
Stock Firefox 153.0.1, with its RDD sandbox explicitly disabled for this
decode-only diagnostic, produces at least 350 external-pool frames and 696
DMA-BUF exports per case; Main10 and Profile 2 both use corrected GR1616
zero-copy imports without the earlier stateful failure.

This is genuine Wayland/Xwayland, EGL and DMA-BUF presentation evidence, but a
virtual monitor cannot validate a connector, display link, EDID, compositor
HDR mode, or physical panel. Mutter reports no physical or logical monitors on
this host, so physical HDR passthrough remains open. The Firefox RDD patch's
sandbox-enabled runtime result is tracked separately from the unsandboxed app
matrix.

**Progress (2026-07-27, structured logging):** The opt-in driver log now emits
single-record structured text or newline-delimited JSON with realtime
nanoseconds, PID/TID, severity, source, line, function, and an escaped message.
Five severity levels are runtime-filtered while the default text message
remains compatible with existing hardware audit greps. The sink is
reference-counted across VA displays and closes/reopens cleanly. Dedicated
normal, ASan/UBSan, TSan, and Valgrind tests cover filtering, JSON control
characters, nested lifecycle, and 800 concurrent records without interleaving.

**Gate:** the app matrix passes on-device — ✅ stock FFmpeg and GStreamer
`va`; ✅ VLC, Firefox, and mpv virtual-output presentation for H.264 High, HEVC
Main, VP9 Profiles 0/2, and HEVC Main10; ⬜ physical HDR-monitor presentation;
⬜ Chromium (arm64 distro binary lacks its libva backend); ✅ conformance suite green; clean
soak; ✅ `.deb` + config packages install, upgrade and purge cleanly in an
isolated root, ⬜ enabling HW decode from a genuinely clean image is untested.

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
initially exposed one full-frame slice only; planar upload support is recorded
below. At that point P010 encode, full WebRTC, multi-slice, and long encode
soak remained open; the later progress entries close all but the
backend-blocked P010 encode path.

**Progress (2026-07-27, P010 contract and Main10 backend boundary):** Linear
P010 DRM PRIME 2 surfaces enforce canonical two-plane offsets, even 4:2:0
dimensions, byte-pitch divisibility, pixel width, object capacity, and owned-fd
lifetime. The original one-object gate covers byte-exact P010 upload/readback
and imported-surface readback; the later two-object gate applies the same
checks to separate luma/chroma objects. This does not advertise P010 encode. A
driver experiment converted P010 through RGA and reached MPP as
`MPP_FMT_YUV420SP_10BIT`, where RK3588's `vepu5xx_set_fmt` rejected format 1
and produced no packet. The official MPP `vepu5xx` format table likewise maps
that format to its unsupported sentinel. `make probe-mpp-main10-encode`
reproduces the backend result without libva; promotion now explicitly depends
on backend support plus Main10 interoperability, quality, sanitizer, and soak
gates. See
[`HEVC_MAIN10_ENCODE_BACKEND.md`](HEVC_MAIN10_ENCODE_BACKEND.md).

**Progress (2026-07-26, planar encoder input):** Encode configs now advertise
NV12 plus I420/YV12 system-memory upload formats. Explicit three-plane
pitches/offsets are capacity-checked, planar chroma is interleaved into the
native NV12 DMA-BUF under CPU synchronization, and `vaGetImage` reverses the
mapping byte-exactly. Stock FFmpeg direct `yuv420p` upload produces the same
CQP stream/PSNR as NV12 for both H.264 and HEVC. GStreamer now feeds I420
directly to `vah264enc`/`vah265enc` without `videoconvert`; normal,
ASan/UBSan, and expanded 96-frame concurrent encode/decode gates pass.

**Progress (2026-07-26, imported DMA-BUF input):** Encode configs now
advertise RGBA/RGBX/BGRA/BGRX when RGA is linked. `vaCreateSurfaces2` accepts
one-object linear DRM PRIME 2 descriptors with exact dimensions, checked
pitches/capacity, canonical zero-offset packed RGB or canonical NV12 planes,
and an owned fd duplicate. Compatible NV12 is submitted directly; packed RGB
is converted into aligned NV12 by RGA before every encode. A public-libva BGRA
gate closes the application's descriptor fd, encodes and standard-decodes
48/48 H.264 High frames at 37.14 dB, and requires exactly 48 RGA conversions
and MPP packets normally and under ASan/UBSan. At that point multi-object and
non-linear modifier layouts remained unsupported; the later two-object linear
YUV gate closes the former, while non-linear layouts still fail closed.

**Progress (2026-07-27, native WebRTC peer transport):** The earlier
120-frame direct-I420 `vah264enc` RTP path still produces 604 packets at a
strict 1,200-byte maximum and round-trips every High-profile frame at
41.06 dB normally and under ASan/UBSan. A new two-`webrtcbin` gate adds
in-process SDP offer/answer, trickle ICE, DTLS-SRTP state auditing, dynamic
sender attachment after the peers connect, H.264 depayload, and standard
decode/PSNR checks. An independent 12-frame OpenH264 transport control
completed 12/12 access units, exchanged 28 candidates in each direction, and
reported connected DTLS-SRTP elements on both peers. At that point the
combined `vah264enc` normal and sanitizer qualification was still open; the
transport-only control was not hardware evidence.

**Progress (2026-07-29, hardware WebRTC peers closed):** The missing
`GstWebRTC-1.0` typelib and `libgstnice.so` plugin were supplied through the
gate's non-system `WEBRTC_DEPS_ROOT` path by extracting the matching Ubuntu
arm64 packages `gir1.2-gst-plugins-bad-1.0 1.28.2-1ubuntu1.1` and
`gstreamer1.0-nice 0.1.23-2`. The combined two-peer hardware run now passes:
120 direct-I420 frames are encoded by `vah264enc`, traverse offer/answer,
trickle ICE, DTLS, and SRTP, arrive as 120 High-profile access units, decode
cleanly, and measure 41.061795 dB. The identical gate passes with the full
ASan/UBSan driver. This replaces the transport-only control with hardware
evidence; no system package installation is claimed.

**Progress (2026-07-26, encode soak smoke):** The new paced soak exposed and
closed a GStreamer HEVC geometry mismatch: a 640x360 visible I420 surface is
paired with a 640x368 aligned VA context/sequence. The driver now accepts only
that exact 16-pixel ceiling and configures MPP prep/frame geometry from the
visible surface. A targeted 640x360 five-path HEVC gate passes afterward.
Concurrent live H.264+HEVC smoke then completed 1,800 frames per codec over 60
seconds with post-warmup RSS fixed at 58,792 KiB and fds fixed at 60. A
30-second full-driver ASan/UBSan run completed 900 frames per codec. The
default two-hour qualification run was still open at that point and is closed
by the later 2026-07-29 result.

**Progress (2026-07-29, multi-object, multi-slice, concurrency, and soak):**
Linear NV12 imports may now use separate luma and chroma DMA-BUF objects. The
driver validates each object's canonical zero offset, pitch, modifier, and
capacity, normalizes privately with DMA synchronization, and exports an
accurate two-object descriptor. A public-libva H.264 gate completes 48/48
frames at 50.683977 dB normally and under ASan/UBSan; undersized, nonzero
offset, and non-linear descriptors fail closed.

H.264 and HEVC encode configs advertise equal-row multi-slice modes. The
driver validates contiguous full rows with equal slice heights except for a
smaller final remainder, then programs MPP's macroblock/CTU split. Normal and
ASan/UBSan gates produce 12 frames with exactly four parser-clean slices per
frame for both codecs.

A single public-libav process now runs two hardware decoders and two hardware
encoders concurrently. Each context completes 120 frames; normal and
ASan/UBSan runs compare downloaded output, while TSan uses independent simple
graphs and raw sinks to avoid unrelated FFmpeg filter races. All three pass
with two decode workers overlapping and 240 encode packets.

Finally, the full 7,200-second H.264+HEVC encode soak completed 216,000 frames
per codec. Combined RSS moved from 56,328 to 52,708 KiB (3,620 KiB total span,
no growth), and fd count stayed exactly 60. This supersedes the earlier
60-second smoke and closes the long encode qualification.

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
  `make test-fuzz` covers the H.264, HEVC, and VP9 reconstructors with
  libFuzzer under ASan/UBSan, filling whole parameter structures from fuzzer
  bytes rather than only mutating bitstreams. It found an out-of-range shift in
  the HEVC slice rewriter, which had trusted an earlier parameter-set call to
  have bounded the picture parameters; both entry points now validate their own
  syntax bounds. A coverage-minimized seed corpus plus named reproducers is
  replayed as a deterministic regression check before every campaign.
- **Docs:** keep `DEVELOPMENT.md` (architecture), this roadmap (plan), and a
  `TESTING.md` (how to run the gates) current as the source of truth.

## Risks & open questions

- **External buffer group parity:** resolved for shipping H.264, VP9, and HEVC
  on the pinned MPP/ROCK 5B stack. The exact Published MPP/FFmpeg package root
  passes the complete HEVC sweep and the normal plus ASan/UBSan shipping
  matrices; the internal-group ref-holding fallback remains zero-copy.
- **10-bit exactness:** resolved for the generated HEVC Main10 and VP9 Profile
  2 AFBC paths, pinned Main10 weighted-prediction and official VP9 Profile 2
  vectors, and the 24-frame HDR10 vector. RGA performs a pure NV15-to-P010
  repack and every gate is byte-exact. Static BT.2020/PQ HDR metadata survives
  the hardware frame path. Broad HEVC conformance and virtual-output app
  presentation are green; physical HDR-monitor passthrough remains open.
- **Encode conformance:** encoders aren't spec-exact; the gate must be
  round-trip PSNR + interop, and depends on the kernel RKVENC2 hardening.
- **Sandbox upstreamability:** the Firefox RDD source patch is hash-pinned and
  request-specific but must be rebased and remeasured per milestone -- it is
  pinned to 153.0/153.0.1 with 152.0.6 kept alongside, and the 153.x rebase
  inherits rather than remeasures the ioctl set. Chromium's proposed aliasing
  sidestep cannot help the current arm64 distro binary because that binary has
  no libva backend.
- **MPP threading contract:** the dedicated-worker model is validated for the
  normal single-decoder H.264/VP9 matrix, nine simultaneous idle MPP contexts,
  two active H.264/VP9 contexts, and a same-process two-decode/two-encode
  workload under normal, ASan/UBSan, and TSan builds.

## Status

- Phase 0: complete on `main`; the normal and sanitized full hardware gates are
  green after the Phase 2 slice on audited fixed kernel build `#4`, and again
  through the current source on the audited production
  `6.18.41-ysp-rockchip64` kernel (notes SHA-256 `6388dd294ff782a438a3a1e03d2c21f033998566d048cc6feecdd315aa2250f8`).
- Phase 1: complete on `main`; object heap/object migrations, external-buffer
  zero-copy, worker/fence synchronization, module separation, two active
  decoders, sanitizer gates, and the multi-hour 4K resource soak are green.
- Phase 2: **HEVC Main is complete and shipping.** `VAProfileHEVCMain` is
  advertised by default as of 2026-07-28: all eight pinned vectors are
  bit-exact normally and under ASan/UBSan. The complete sweep with the exact
  Published MPP `3381fd2c` and FFmpeg `33a651a55b` arm64 packages has 144 of
  163 candidates bit-exact, 17 profile skips, two advertised-size refusals,
  and zero backend or driver failures. Both byte-identical NUT names return
  34 exact frames. System installation remains a package-lifecycle gate. The
  direct-MPP TILES failure is gone on the current stack and
  `TILES_A_Cisco_2.bit` is bit-exact. The remainder of the phase is 10-bit: the
  generated 48-frame Main10/Profile 2, pinned 256-frame Main10, official
  10-frame Profile 2, and 24-frame Main10 HDR10 AFBC-to-P010 gates are
  bit-exact, 1080p throughput exceeds 260 fps for both codecs, and static
  BT.2020/PQ HDR metadata is preserved. Both 10-bit profiles stay hidden until
  physical HDR and release/distribution qualification are complete.
- Phase 3: in progress; five app-matrix consumers now pass on-device. Stock FFmpeg
  is the conformance gate itself, the stock GStreamer 1.28 `va` plugin
  system-memory gate is byte-exact for H.264, HEVC Main10, and VP9 Profiles
  0/2. Stock VLC 3.0.23, Firefox 153.0.1, and mpv 0.41.0 hardware-decode and
  present H.264 High, HEVC Main, VP9 Profiles 0/2, and HEVC Main10 with clean
  driver logs (`check-vlc-display`, `check-firefox-decode`,
  `check-mpv-display`) on a Mutter virtual monitor. The mpv H.264 CIF case also
  requires the selective 352-to-384-byte NV12 export repack. The display gates
  refuse to run without a real or virtual output; virtual presentation is not
  physical HDR-monitor proof.
  Split driver/config Debian packaging no longer weakens Firefox's sandbox
  globally, structured leveled text/JSON logging is lifecycle-, sanitizer-,
  thread-, and leak-tested, and clean-image package lifecycle validation is
  green, and the Firefox RDD source patch is rebased and hash-pinned to 153.0.
  `vaDeriveImage` and `vaAcquireBufferHandle` are implemented over the
  surface's own DMA-BUF, which is what unblocked VLC; completed linear P010
  and aligned provisional converter probes are accepted, while imported,
  still-compressed, stale-layout, imported-RGB, and encoder-input surfaces
  fail closed. The first recorded stock Firefox Main10 fallback was the
  driver's invalid `GR16` literal, not a Panfrost GR1616 limitation. Installed
  ysp7 proves the corrected GR1616 import in both Firefox processes; the next
  failure was a stateful MPP-marked Main10 boundary covered by the reducer;
  the expanded Firefox run is now stable across all five cases.
  Open: sandbox-enabled Firefox playback is awaiting the patched-build runtime
  result; Chromium 151 now has accelerated Panfrost GL but its distro arm64
  binary lacks the libva backend and reports no hardware profiles; physical
  HDR-monitor presentation and fresh-image hardware decode remain untested.
  Installed `1.0.11+ysp7` carries the corrected exporter.
- Phase 4: complete for the deliberately advertised experimental scope;
  H.264 Main/High and HEVC Main
  encode pass FFmpeg and GStreamer CQP/CBR/VBR interoperability, parser, and
  PSNR gates normally and under ASan/UBSan. Both 96-frame encoder gates pass
  together with the shipping decode matrix. Checked I420/YV12 uploads are
  normalized to native NV12 and pass direct FFmpeg/GStreamer gates. Linear
  packed-RGB DMA-BUF import passes a public-libva RGA conversion gate. Linear
  P010 import/readback is byte-exact, but Main10 encode is backend-blocked by
  MPP `vepu5xx` rejecting its compact 10-bit input format. The native two-peer
  `vah264enc` WebRTC gate covers SDP/ICE/DTLS/SRTP and passes 120 frames at
  41.061795 dB normally and with the full ASan/UBSan driver.
  Separate-object linear NV12 import passes public-libva normal and sanitizer
  gates; tiled/non-linear imports still fail closed. Equal-row H.264/HEVC
  multi-slice produces exactly four parser-clean slices per frame normally and
  under ASan/UBSan. A same-process two-decode/two-encode workload passes
  normally, under ASan/UBSan, and under TSan. The full two-hour dual-codec
  encode soak completes 216,000 frames per codec with flat fds and no RSS
  growth. H.264 WebRTC-compatible RTP packetization is green. Its two missing
  GStreamer test packages were supplied from an extracted arm64 package root
  rather than installed system-wide.
- Phase 5: in progress; the `1.0.11+ysp8` driver/config binaries pass build,
  Lintian, and the isolated package lifecycle, and `1.0.11+ysp7` is installed
  on the host. A genuinely fresh-image hardware run, final Firefox sandbox
  runtime proof, tag, GitHub Release, and PPA publication remain.

Tracked in the ROCK 5B project as status **track 14** with the enablement
map and driver-review finding as the decision/evidence record.
