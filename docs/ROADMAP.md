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
- **10-bit:** MPP emits compact **NV15**. Convert **NV15→P010 via RGA**
  (`convert.c`, librga im2d) for universal app compatibility — this reuses the
  kernel-side RGA NV15→P010 path validated in the ROCK 5B forward-port (patches
  `0048`/`0049`). Export P010 as R16+GR1616 split (Firefox) or composed P010
  (mpv/GStreamer). Investigate **direct NV15 dmabuf export**
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

### Phase 0 — Baseline & harness  ✅ mostly done (~1 wk remaining)

Fork, build against the ROCK 5B stack, the original `framemd5` gate, and the
three correctness fixes (PPS ref-counts, VP9 backpressure, VP9 PTS routing) are
**done** (`ysp/cleanup`). The remaining Phase 0 implementation also landed:

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
- The complete driver has an **ASan/UBSan build**; the safe conformance subset
  is green with that instrumented driver loaded into FFmpeg.
- The **CI skeleton** runs AArch64 cross-builds, sanitizer unit tests,
  ShellCheck, and clang-tidy on every push. The on-board hardware gate is a
  guarded manual self-hosted job.

Phase 0 remains open until the fixed VP9 kernel is booted, MPP's
`show_existing_frame` reference-accounting fix is installed, and the
unquarantined conformance + sanitizer gate passes. A skipped required vector
is deliberately reported as blocked, never green.

**Gate:** conformance-vector decode bit-exact for the shipping profiles; gate
green under ASan; CI builds and lints on push.

### Phase 1 — Architectural renovation (decode core)  (~2–3 wk)

The foundation everything else builds on. No new codecs — restructure.

- Split the monolith into the module layout above; introduce the object heap.
- Implement the **external-buffer-group zero-copy model** and delete the
  per-frame memcpy.
- Implement the **per-context worker + per-surface fence** sync model; delete
  polling.
- Add the driver lock; make **two concurrent decode contexts** correct.

**Gate:** H.264 + VP9 still conformance-vector bit-exact; **no per-frame copy**
(verify via perf counters / memory bandwidth); clean under ASan **and** TSan;
two simultaneous decoders in one process decode correctly; multi-hour 4K soak
with flat memory and no fd leaks.

### Phase 2 — HEVC decode + 10-bit / HDR  (~2–3 wk)

- **HEVC decode:** reconstruct VPS/SPS/PPS from `VAPictureParameterBufferHEVC`
  + `VASliceParameterBufferHEVC`, including **scaling lists** from
  `VAIQMatrixBufferHEVC` (HEVC scaling data *is* in the VA buffers, so this is
  spec-honest, unlike the H.264 PoC shortcuts). Main first, then Main10.
- **10-bit path:** NV15→P010 via RGA (`convert.c`); wire P010 export for all
  consumers.
- Backfill **H.264 spec-honesty**: honor `VAIQMatrixBufferH264` scaling
  matrices, derive level from the stream instead of hardcoding 5.1.

**Gate:** HEVC Main bit-exact vs software on conformance vectors; HEVC Main10 /
VP9 P2 validated (PSNR-bounded, since RGA P010 conversion is not
transform-exact — or NV15-space bit-exact if direct export lands); HDR HEVC
plays correctly in Firefox and mpv on-device.

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

- **External buffer group parity:** confirm `MPP_DEC_SET_EXT_BUF_GROUP` behaves
  for H.264/HEVC/VP9 on our MPP build the way `libv4l-rkmpp` relies on. If a
  codec misbehaves, fall back to a committed internal group with ref-holding
  (still zero-copy) for that codec. *Resolve early in Phase 1.*
- **10-bit exactness:** RGA NV15→P010 is a conversion, so 10-bit can't be
  transform-bit-exact against a P010 software reference; decide between a PSNR
  bound vs. NV15-space bit-exact comparison vs. landing direct NV15 export.
- **Encode conformance:** encoders aren't spec-exact; the gate must be
  round-trip PSNR + interop, and depends on the kernel RKVENC2 hardening.
- **Sandbox upstreamability:** the Firefox RDD policy patch is small but must be
  re-verified per milestone; the Chromium aliasing sidestep depends on the GPU
  sandbox continuing to allow `ioctl` without arg inspection — verify against
  the shipping Chromium, don't assume.
- **MPP threading contract:** validate that one `mpi` per context with a
  dedicated worker is the supported concurrency model for many simultaneous
  contexts.

## Status

- Phase 0: in progress on `ysp/cleanup` (fixes + gate done; conformance
  vectors, ASan, CI remain).
- Phases 1–5: planned.

Tracked in the ROCK 5B project as status **track 14** with the enablement
map and driver-review finding as the decision/evidence record.
