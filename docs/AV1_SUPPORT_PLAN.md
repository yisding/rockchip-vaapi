# AV1 decode support: feasibility, architecture, and implementation plan

Status: Phase 0 blocked; non-submitting platform probe implemented

Research snapshot: 2026-07-27

Scope: AV1 decode through `VAEntrypointVLD` on RK3588-class systems

## Executive decision

Do **not** re-advertise AV1 by sending the existing VA slice buffers through
MPP's normal packet decoder. That is the already-observed failure mode: libva
clients send parsed picture state and tile payloads, while MPP's public AV1
decoder expects a complete OBU stream and runs its own parser.

The recommended production design is:

1. Prove that the target kernel and device tree expose a working AV1 hardware
   endpoint before changing VA capabilities.
2. Add an AV1 picture translator to this driver. It must copy and validate the
   libva picture parameters, tile descriptors, tile bytes, and referenced
   surfaces into an immutable per-picture job.
3. Submit that job through a **supported parsed-picture MPP API** that associates
   CDF, segmentation, and motion-vector state with explicit decoded surfaces.
   Such an API does not exist in public MPP today, so it must be designed with
   Rockchip, prototyped, and either upstreamed or carried as a versioned
   dependency.
4. Keep the translator independent of MPP so a Linux stateless V4L2 backend can
   be added for the mainline RK3588 VPU981 driver. V4L2 is the preferred
   long-term mainline backend, but its current AV1 uAPI also asks for
   `refresh_frame_flags`, which stock libva does not supply.
5. Initially expose only `VAProfileAV1Profile0`, first for 8-bit NV12 and then
   for 10-bit P010. Keep Profiles 1 and 2, large-scale tiles, and any unvalidated
   feature hidden.
6. Keep AV1 behind a narrow experimental gate until platform probing,
   conformance, browser, synchronization, and soak gates all pass. Missing
   support must fail fast and fall back to software or another codec; it must
   never reproduce the current multi-second MPP parser stall.

An in-driver OBU writer is worthwhile as a diagnostic when the original frame
header is also available, but it is **not a complete production solution for
stock VA clients**. The current libva AV1 structure omits
`refresh_frame_flags`, and no synchronous driver algorithm can recover that
value for every stream.

This is a substantial cross-project feature, not a small codec switch. A
production MPP route is approximately four to eight engineer-months if
Rockchip cooperates on a parsed API and the hardware endpoint is made
available. A private client-plus-driver prototype could be much faster, but it
would not meet this project's stock-client goal.

## What “supported” means

AV1 may be enabled by default only when all of the following are true:

- Stock distro FFmpeg can decode AV1 through `-hwaccel vaapi` without a private
  bitstream patch.
- Firefox can decode, seek, change resolution, suspend, and recover from errors
  without disabling hardware decode for the session.
- Output matches a trusted software decoder on pinned conformance streams,
  including hidden reference frames and CDF updates.
- Both the ungrained reference picture and grained display picture are correct
  when film grain is requested.
- 8-bit output is correct as NV12 and 10-bit output is correct as P010,
  including dmabuf export, mapping, derive-image, and synchronization.
- Unsupported streams and unavailable hardware fail before a decode job is
  queued.
- Multiple contexts, mixed codecs, teardown during work, and multi-hour loops
  have no stale surfaces, deadlocks, fd leaks, or unbounded memory growth.
- The driver reports only capabilities proved by the selected runtime backend.

AV1 encode, 12-bit, 4:2:2, 4:4:4, scalable coding, protected content, and AV1
large-scale-tile mode are not part of the first production milestone.

## Current repository state

The repository already contains several useful pieces:

- `profile_to_coding()` in `src/context.c` maps `VAProfileAV1Profile0` and
  `VAProfileAV1Profile1` to `MPP_VIDEO_CodingAV1`.
- `decode_profile_supported()` in `src/rockchip_drv_video.c` deliberately does
  not advertise either profile.
- `rk_mpp_dec_build_job()` in `src/mpp_dec.c` has codec-specific reconstruction
  for AVC and HEVC, then sends other codecs through the generic packet path.
- The per-context worker, surface fences, external MPP buffer pool, dmabuf
  export, route objects, and 10-bit conversion machinery can be reused.
- The changelog records the important negative result: advertising AV1 made
  Firefox select it, MPP spent roughly three seconds per frame in its parser,
  and Firefox eventually disabled hardware decoding for the browser session.

The current data path is therefore:

```text
AV1 bitstream
  -> client AV1 parser (FFmpeg/Firefox media stack)
  -> VA picture parameters + tile descriptors + headerless tile data
  -> rockchip-vaapi generic packet path
  -> MPP AV1 packet parser, which expects sequence/frame/tile-group OBUs
  -> parse failure or severe stall
```

The desired data path is:

```text
client AV1 parser
  -> standard VA buffers
  -> validated RKAV1Picture + explicit referenced VA surfaces
  -> backend-neutral parsed-picture contract
       -> public MPP parsed AV1 submission
       or
       -> Linux V4L2 Request API submission
  -> current reference surface + optional film-grain display surface
  -> existing VA surface fence/export lifecycle
```

### Local RK3588 platform audit

The 2026-07-27 development machine was checked separately from the driver:

- RK3588 ROCK 5B
- kernel `6.18.40-ysp-rockchip64`
- `librockchip-mpp1`
  `1.5.0+git20260529.1375813c+ds-0ubuntu2~rk1`
- libva `2.23.0`
- FFmpeg `8.0.3+rockchip+git20260719`
- `/dev/mpp_service` exists
- the vendor kernel binds `mpp_av1dec` to
  `fdc70000.video-codec` (`rockchip,av1-decoder`, status `okay`)
- the only decoder V4L2 node found was
  `rockchip,rk3328-vpu-dec`, advertising parsed MPEG-2 and VP8 rather than AV1
- no `rockchip,rk3588-av1-vpu-dec`/VPU981 node was present
- MPP's public capability function advertised AV1 decode while emitting six
  generic client-driver readiness diagnostics; this does not prove that the
  bound AV1 endpoint can complete work
- a tiny valid low-overhead AV1
  stream submitted with `mpi_dec_test` produced no frame and did not terminate
- MPP logged that `VPU_CLIENT_AV1DEC` was not ready

This proves only the state of this board image, not a limitation of RK3588
silicon. It does establish a Phase 0 blocker: driver work cannot be qualified
on this image until either the vendor MPP AV1 path completes a bounded
known-answer decode or the mainline VPU981 node is enabled and qualified.

`make probe-av1-platform` now records this boundary without submitting a
bitstream. Its versioned key/value report inventories the kernel, MPP/libva
packages, `/dev/mpp_service` access, bound `mpp_av1dec` devices, public MPP AV1
capability result, readiness diagnostics, and every V4L2 OUTPUT format. The
report always states `hardware_decode_attempted=0` and
`phase0_qualified=0`; an endpoint-present result is discovery evidence only,
not a decode claim.

Mainline Linux has an RK3588 VPU981 stateless AV1 implementation in
[`rockchip_vpu981_hw_av1_dec.c`](https://github.com/torvalds/linux/blob/master/drivers/media/platform/verisilicon/rockchip_vpu981_hw_av1_dec.c).
Its currently declared formats are AV1 Main-profile input with native tiled
8/10-bit surfaces and NV12/NV15/P010 post-processing, up to the sizes returned
by the device—currently 4K in the upstream format table. Runtime enumeration,
not the SoC marketing maximum, must determine what this driver advertises.

## The interface mismatch

### What FFmpeg actually sends

Upstream FFmpeg parses the sequence header, frame header, tile-group header,
and tile sizes before calling VA-API:

- [`av1dec.c`](https://github.com/FFmpeg/FFmpeg/blob/master/libavcodec/av1dec.c)
  calls the hardware `start_frame` callback with the original frame OBU but
  later calls `decode_slice` with `raw_tile_group->tile_data` only.
- [`vaapi_av1.c`](https://github.com/FFmpeg/FFmpeg/blob/master/libavcodec/vaapi_av1.c)
  converts the parsed state into `VADecPictureParameterBufferAV1` and one
  `VASliceParameterBufferAV1` per tile. The slice offsets point into the
  tile-data portion after the tile-group header.
- FFmpeg still has `refresh_frame_flags`, the original OBU, sequence details,
  and render dimensions internally, but the stock libva call does not transmit
  all of them.
- `show_existing_frame` is resolved in FFmpeg by returning the saved reference
  frame; no normal VA decode submission is made for that header.

This behavior is correct for a parsed/stateless hardware interface. It is not
compatible with feeding the supplied slice bytes to a stateful OBU parser.

### What public MPP expects

Rockchip MPP advertises AV1 in its packet decoder and contains mature AV1
parser/HAL code:

- [`av1d_api.c`](https://github.com/rockchip-linux/mpp/blob/develop/mpp/codec/dec/av1/av1d_api.c)
  forces stream splitting and parses OBU headers.
- [`av1d_parser.c`](https://github.com/rockchip-linux/mpp/blob/develop/mpp/codec/dec/av1/av1d_parser.c)
  parses sequence, frame, and tile OBUs, updates the DPB and CDF state, and
  constructs an internal `DXVA_PicParams_AV1`.
- [`av1d_syntax.h`](https://github.com/rockchip-linux/mpp/blob/develop/mpp/common/av1d_syntax.h)
  defines that internal picture syntax.
- [`hal_av1d_vdpu383.c`](https://github.com/rockchip-linux/mpp/blob/develop/mpp/hal/rkdec/av1d/hal_av1d_vdpu383.c)
  consumes the parsed structure and explicit tile/reference buffers.

That internal boundary demonstrates that parsed-picture submission is
technically natural for MPP. It is not a supported public ABI: `rk_mpi.h`
exposes packets, frames, and tasks, but no stable AV1 syntax-injection control.
Including MPP internal headers or calling the HAL directly would tie this
driver to private structure layouts, buffer-slot rules, and kernel ioctls.

### What libva loses

The current upstream
[`va_dec_av1.h`](https://github.com/intel/libva/blob/master/va/va_dec_av1.h)
provides most effective picture state: bit depth, current and display
surfaces, reference surface map, reference indices, tile geometry, frame
tools, quantization, segmentation, loop filters, CDEF, restoration, global
motion, and film grain. The API explicitly describes support as 8/10-bit
4:2:0.

It does **not** provide a `refresh_frame_flags` member, even though a nearby
comment tells applications to set it for switch frames. It also lacks a
complete sequence header, reference order-hint array, render dimensions,
`current_frame_id`, `skip_mode_frame`, and the original frame/tile-group OBU
headers.

The decisive omission is `refresh_frame_flags`:

- MPP's parser uses it to replace DPB slots and copy the decoded CDF,
  segmentation, and other reference state.
- Linux's `v4l2_ctrl_av1_frame` explicitly requires it and the RK3588 VPU981
  driver uses it to store post-decode CDF state.
- The current frame's VA reference map describes the map **before** refresh.
  A later picture may reveal some of the update by referring to the current
  surface, but that is too late for `vaSyncSurface`, end-of-stream, flush,
  seek, and error paths.

One-frame lookahead is therefore only a diagnostic experiment. It violates
normal VA completion semantics, adds latency, cannot finish the last frame,
and is ambiguous when duplicate or unused slots exist. Setting all refresh
bits, setting none, or choosing an arbitrary slot works only on restricted
streams and will eventually corrupt reference/CDF state.

### Information inventory

| Information | Stock VA AV1 | Can the driver recover it? | Required action |
| --- | --- | --- | --- |
| Profile, bit depth, subsampling | Yes | Direct | Validate Profile 0 and 4:2:0 |
| Effective per-frame coding tools | Mostly | Direct/canonicalize | Translate and range-check |
| Current ungrained and display surfaces | Yes | Direct | Hold both through completion |
| Eight reference surfaces and seven named refs | Yes | Direct | Acquire strong surface references |
| Reference dimensions/type/order state | Partial | Store when each VA surface is decoded | Add per-surface AV1 state |
| Tile rows, columns, sizes, offsets, payload | Yes | Direct; some size-width details can be inferred | Copy into dynamic tile list |
| Tile-group OBU header | No | Some fields are deprecated in slice params; exact original bytes are gone | Do not require it in parsed backend |
| Complete sequence header | No | Some omitted flags can be represented by effective frame values | Backend contract must use effective state |
| Render size/current frame ID | No | Render size sometimes defaults to frame size; frame ID is mainly error detection | Extension if a backend strictly needs them |
| Reference order-hint array | No | Store order hints with surfaces | Populate from per-surface state |
| `refresh_frame_flags` | No | Not synchronously or unambiguously | Use surface-keyed backend state or extend libva |
| Original sequence/frame OBUs | No | Impossible | Optional client extension only |
| HDR metadata OBUs | Not in this decode buffer | Available through other client metadata paths | Treat separately from pixel decode |

The [AV1 specification](https://aomediacodec.github.io/av1-spec/) is the
authority for OBU, uncompressed-header, tile-group, reference refresh, and CDF
semantics.

## Options considered

| Option | Stock clients | Correctness potential | Maintenance | Time to first frame | Production verdict |
| --- | --- | --- | --- | --- | --- |
| Public MPP parsed-picture API with surface-keyed state | Yes | High | Medium if upstreamed | Medium | **Recommended primary** |
| V4L2 stateless AV1 backend plus a surface-keyed state solution | Yes | High | High initially, low once upstream | Medium/high | **Recommended mainline alternative** |
| Upstream libva AV1 v2/companion state plus client changes | Only after ecosystem adoption | High | Low after adoption | High | Strategic standards work |
| Private VA buffer carrying original headers/state | Patched clients only | High | High | Low/medium | Useful bring-up path |
| Reconstruct complete OBUs from current VA buffers | Yes in theory | Low with current API | High | Low | Diagnostic, not production |
| Call MPP internal parser/HAL interfaces directly | Yes | Medium | Very high | Low/medium | Reject for release |
| Embed FFmpeg CBS, dav1d, or libaom parser | No missing data restored | Low | High | Medium | Not a solution by itself |
| Fork `libva-v4l2-request` | Yes after major work | Medium/high | High | Medium/high | Architectural reference only |
| Use native V4L2 clients instead of VA | No for Firefox/VA-only apps | High | Outside this driver | Low where supported | Deployment fallback |
| Delegate AV1 to a second VA driver | Not transparently | Depends on driver | High integration cost | Medium | No useful libva chaining model |
| Vulkan Video or another native client API | No for VA-only apps | Depends on platform | Outside this driver | Unknown | Ecosystem alternative, not this feature |
| Software AV1 hidden behind VA-API | Yes | High | Unnecessary duplication | Low | Reject; use the client's decoder |
| Keep AV1 hidden/software fallback | Yes | Correct but slower | Low | Already done | Required until gates pass |

### Option 1: public parsed-picture MPP API

This best fits the current project because its surface allocation, worker,
fence, output routing, and conversion paths already use MPP. The MPP AV1 HAL
already consumes a parsed picture structure close to libva's.

The public API must not simply expose the private `DXVA_PicParams_AV1` struct.
It should provide:

- a versioned, size-tagged public AV1 picture structure;
- explicit destination and reference frame handles;
- effective frame, tile, filter, restoration, global-motion, and film-grain
  state;
- tile payload plus a checked array of tile offset/size/row/column entries;
- a backend-owned AV1 state token associated with every decoded reference
  frame;
- selection of the primary reference state by explicit frame token;
- output of new CDF/segmentation/motion state attached to the current frame;
- asynchronous task completion and an error result before the frame becomes
  referenceable;
- runtime feature/version discovery.

Attaching state to decoded frames removes the need for the VA driver to know
which abstract AV1 DPB slots were refreshed. Future VA calls explicitly name
the surfaces they use. MPP can load the state belonging to the primary
reference surface and write new state next to the current destination.

The first prototype can live on a pinned MPP branch, but the driver must probe
the ABI and fail closed when it is absent. A permanent distro release should
either use an upstream MPP API or package a clearly versioned MPP dependency
with conformance evidence.

### Option 2: Linux stateless V4L2 backend

Linux added stable AV1 stateless controls and the RK3588 VPU981 driver in the
6.5 era. The
[Request API](https://www.kernel.org/doc/html/latest/userspace-api/media/mediactl/request-api.html)
associates per-frame controls and the coded OUTPUT buffer with a media request,
while the decoded CAPTURE buffer is queued separately. The
[stateless codec controls](https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/ext-ctrls-codec-stateless.html)
cover sequence, frame, tile-group entries, and film grain. Collabora's
[implementation report](https://www.collabora.com/news-and-blog/news-and-events/video-codecs-adding-av1-stateless-video-decoder-support-to-linux.html)
describes the Rockchip/MediaTek work and conformance testing.

Advantages:

- documented upstream kernel ABI;
- no dependency on MPP's packet parser or private HAL;
- native NV12/P010 post-processing in the current VPU981 driver;
- code can be tested against GStreamer's
  [`v4l2slav1dec`](https://gstreamer.freedesktop.org/documentation/v4l2codecs/v4l2slav1dec.html)
  and Chromium's stateless implementation;
- a separate backend could later serve other stateless codecs or SoCs.

Blockers:

- the development board currently has no VPU981 AV1 node;
- V4L2 requires more header state than stock libva supplies, especially
  `refresh_frame_flags`;
- the existing kernel driver indexes some CDF state by AV1 DPB slot, so a
  userspace-only surface map cannot transparently replace the missing flag;
- implementing requests, queue ownership, format negotiation, stream-on/off,
  resolution changes, timestamp-to-surface reference mapping, and dmabuf
  lifetime is a new backend rather than a small adapter;
- kernel AV1 fixes continue to matter, so the minimum supported kernel must be
  pinned to a reviewed, patched stable release.

There are two credible ways to close the state gap:

1. extend libva and clients to transmit the complete V4L2 frame state; or
2. propose a V4L2/kernel-driver mode in which persistent decode state is keyed
   to explicit reference timestamps/capture buffers rather than refreshed
   abstract slots.

Until one is accepted, this remains the mainline architecture target rather
than an immediately implementable stock-client backend.

### Option 3: extend libva and clients

A standards-first route would add a versioned picture-state companion buffer
or a new AV1 picture-parameter version containing:

- `refresh_frame_flags`;
- the omitted sequence feature flags and maximum dimensions;
- render dimensions and current frame ID;
- reference order hints;
- `skip_mode_frame` and tile-size-byte information;
- optionally the original sequence/frame/tile-group headers for stateful
  backends.

Silently placing private values in `va_reserved[]` is not acceptable.
Reserved storage may make an ABI-compatible upstream design possible, but the
layout, capability negotiation, and zero-initialized old-client behavior must
be agreed in libva. FFmpeg can populate the data because it still owns all of
it before `start_frame`/`decode_slice`.

This is the cleanest way to map VA to the existing V4L2 uAPI or reconstruct
OBUs, but it becomes useful to general desktop users only after libva, FFmpeg,
GStreamer, Firefox, and relevant distro versions adopt it. It should be
pursued in parallel as an ecosystem fix, not made the only short-term route.

### Option 4: private client extension

A vendor/private VA buffer could carry one of:

- the original low-overhead sequence and frame/tile-group OBUs;
- `refresh_frame_flags` plus the missing parsed state; or
- a complete versioned copy of a neutral `RKAV1Picture`.

Patching FFmpeg to emit it would quickly prove MPP OBU reconstruction or
parsed submission. It is valuable for hardware bring-up and comparing
backends. It is not a release solution because Firefox, stock FFmpeg,
GStreamer, and other distro clients will not emit it.

If used, assign an explicitly private type/UUID and hard version/size checks.
Never reinterpret standard `va_reserved` fields without negotiation.

### Option 5: in-driver OBU reconstruction

The mechanical work is feasible:

- write low-overhead OBU headers and LEB128 sizes;
- cache and regenerate a canonical sequence header on sequence change;
- serialize the uncompressed frame header;
- recreate tile-group headers, byte alignment, and tile-size fields;
- append tile payloads in tile order;
- preserve temporal/spatial IDs where supplied;
- feed complete temporal units to the existing MPP worker.

This would resemble the H.264/HEVC reconstruction already in the repository,
but AV1's reference refresh and CDF state make it categorically different.
Without the missing header values the writer can only generate a different
stream that happens to decode some samples. A lookahead prototype should be
kept in a test tool, never behind an advertised profile.

### Option 6: direct MPP HAL/private ioctl use

The internal MPP parsed structure and AV1 HAL offer a tempting shortcut.
Reject it for production because it would:

- couple the VA driver to uninstalled, private MPP headers;
- duplicate parser-owned buffer slots, CDF tables, segmentation maps, motion
  fields, error recovery, and fast-mode rules;
- depend on private kernel client behavior;
- expand the browser media-process attack surface with register-level code;
- break on ordinary MPP package upgrades.

Private HAL calls are acceptable only in an isolated proof used to design the
public API, not in `librockchip_drv_video.so`.

### Option 7: add another parser library

FFmpeg CBS, dav1d, libgav1, and libaom can parse original AV1 streams. They
cannot reconstruct bytes or syntax that the VA client never sends. Embedding
one adds license, versioning, security, and duplicate-parser cost without
solving the boundary. A parser becomes useful only with the private/or
standardized original-header extension from Options 3 or 4.

### Option 8: reuse `libva-v4l2-request`

Bootlin's
[`libva-v4l2-request`](https://github.com/bootlin/libva-v4l2-request) is a
useful example of a VA driver organized around media requests, but its public
repository predates the stable AV1 uAPI and supports older codecs. Reusing its
architecture or request helpers may save design time; adopting it wholesale
does not supply AV1 translation, current surface semantics, or this project's
MPP interoperability.

### Option 9: native client paths and software fallback

GStreamer can use `v4l2slav1dec`, and Chromium has current stateless V4L2 code
that has been tested on RK3588. These paths retain the original parser state
and therefore do not encounter the VA information loss. They are sensible
deployment recommendations while VA AV1 is under development, but they do
not serve Firefox, mpv/FFmpeg VAAPI, or other VA-only consumers.

The default driver behavior must remain “AV1 not advertised” until a
production route passes every gate.

A stateful V4L2 endpoint advertising `V4L2_PIX_FMT_AV1` would not by itself
fix this VA driver. Like public MPP, a stateful decoder consumes the complete
AV1 bitstream that the VA boundary has already discarded. It becomes useful
only with an original-bitstream client extension or with clients using V4L2
directly.

### Option 10: delegate AV1 to another VA driver

It may appear attractive to keep MPP for existing profiles and send AV1 to a
modernized `libva-v4l2-request` driver. In normal operation libva loads one
driver for a display; it does not compose profile lists, contexts, surfaces,
and exports from two independent backends. A user could select a separate
driver for a separate process, but would lose this driver's other profiles and
would still face the AV1 state gap.

The viable version of this idea is the backend-neutral design in this
document: one VA driver owns its objects and selects MPP or V4L2 internally.
A separate experimental V4L2 VA driver can be useful for development, but
transparent per-codec driver chaining should not be a project dependency.

### Option 11: Vulkan Video and other client-native APIs

FFmpeg and some media stacks can use APIs other than VA-API when a matching
Rockchip kernel/userspace implementation exists. Vulkan Video, direct V4L2,
and a future browser-native backend may eventually be cleaner than translating
through VA. They do not implement the requested Firefox/general VA capability,
and the availability of Vulkan decode on RK3588 must be proved rather than
assumed.

Document these as deployment alternatives. Do not add a Vulkan-to-VA bridge
to this driver: it would introduce another state translation and surface
ownership layer without repairing the missing VA inputs.

### Option 12: software decode behind VA-API

Calling dav1d/libaom from the VA driver could make `vainfo` appear to support
AV1, but would add a second software decoder inside the browser media process,
misrepresent hardware capability, complicate sandboxing, and usually perform
worse than the client's existing optimized fallback. Keep software decoding
in the client.

## Recommended driver architecture

### Keep parsing separate from submission

Add a backend-neutral AV1 module before either MPP or V4L2 integration:

```text
src/av1.h                 validated public neutral structures
src/av1.c                 VA buffer ingestion, validation, state derivation
src/av1_mpp.c             public parsed-MPP adapter
src/av1_v4l2.c            optional Request API adapter
src/dec_backend.h         backend capabilities and submit/reset/destroy hooks
tests/av1_params_test.c    libva-to-neutral mapping and rejection tests
tests/av1_replay.c         capture/replay harness without a browser
```

Do not start by generalizing all existing codecs. Add the smallest backend
contract needed by AV1, then extract shared queue/surface code only when the
second backend demonstrates a real common abstraction.

### Core objects

`RKAV1Picture`

- one immutable snapshot per `EndPicture`;
- all scalar picture parameters copied from
  `VADecPictureParameterBufferAV1`;
- normalized effective sequence/frame flags;
- checked tile geometry and a dynamic tile array;
- copied tile bytes or refcounted immutable VA buffers;
- current ungrained and optional display surface;
- eight acquired reference surfaces plus seven reference indices;
- profile, bit depth, output format, coded/upscaled dimensions, and a sequence
  generation number.

`RKAV1Tile`

- payload offset and size using `size_t`;
- tile row, column, and raster tile index;
- source-buffer identity where multiple slice-data buffers are used;
- optional tile-group identity for validation/debug capture.

`RKAV1SurfaceState`

- attached to the lifetime/generation of a VA surface, not just its numeric ID;
- valid only after its decode fence succeeds;
- order hint, dimensions, bit depth, frame type, and effective reference
  metadata saved when decoded;
- backend-owned reference/CDF/segmentation/motion token;
- ungrained reference image handle;
- reset generation so stale state cannot cross flush or sequence reset.

`RKAV1Job`

- owns the immutable picture and all strong surface references;
- carries the existing destination fence and route token;
- has explicit cancellation and failure cleanup;
- is destroyed only after the backend has stopped touching every tile,
  reference, and output buffer.

### VA call lifecycle

1. `CreateConfig`
   - allow AV1 Profile 0 only if the selected backend's runtime probe passes;
   - validate `VAEntrypointVLD`;
   - accept only the backend-proved RT formats.
2. `CreateContext`
   - create/reset an AV1 sequence generation;
   - initialize the selected backend and its state-token pool;
   - refuse unsupported dimensions before returning success.
3. `BeginPicture`
   - acquire the destination surface and create the normal surface fence;
   - clear all per-picture AV1 collection state.
4. `RenderPicture`
   - accept exactly one valid AV1 picture parameter buffer;
   - accept any legal number and grouping of AV1 slice-parameter and
     slice-data buffers;
   - copy or strongly reference data immediately because the client may
     destroy VA buffers after the call;
   - reject codec-inappropriate buffer types rather than silently ignoring
     malformed input.
5. `EndPicture`
   - normalize and validate the complete tile coverage;
   - acquire all reference and display surfaces;
   - resolve per-surface reference state;
   - build an immutable job and enqueue it;
   - return a real error synchronously if required information/backend
     capability is missing.
6. Worker
   - submit parsed state and buffers;
   - route hidden and displayed frames by explicit job token, not FIFO/PTS
     guesses;
   - commit `RKAV1SurfaceState` only after successful hardware completion;
   - signal the destination and display fences, or fail both atomically.
7. Flush/destroy
   - cancel or drain jobs;
   - reset sequence generation and backend reference state;
   - drop all surface/token references after hardware is idle.

### Tile ingestion and validation

The implementation must not assume one VA slice buffer, one tile group, or a
fixed number of pending buffers.

- Walk every element of every `VASliceParameterBufferAV1`.
- Match each parameter array with the correct slice-data buffer according to
  VA submission order and flags.
- Use checked addition for `slice_data_offset + slice_data_size`.
- Require row/column within the declared `tile_rows`/`tile_cols`.
- Reject duplicate tiles, missing tiles at `EndPicture`, overlaps, impossible
  tile counts, and invalid zero sizes.
- Cap total tile bytes, tile count, and allocation growth to device/API
  maxima. The current upstream VPU981 code uses a 128-tile hardware maximum;
  query or explicitly version the MPP maximum.
- Preserve size-field bytes where a backend input layout requires them, but
  pass explicit payload offsets to parsed backends.
- Treat deprecated `tg_start`/`tg_end` only as consistency hints. Do not make
  correctness depend on a deprecated field.
- Fuzz this entire path with arbitrary buffer counts, sizes, offsets, and
  truncated data.

### Reference, hidden-frame, and CDF state

AV1 output order is not decode order. Hidden frames can become references and
later be returned by `show_existing_frame`.

- Extend the token-route model to all AV1 submitted pictures.
- Do not use the generic FIFO route for AV1.
- A hidden frame still completes and commits surface state even when it has no
  immediate display.
- A client-resolved `show_existing_frame` normally produces no VA job. Retain
  the referenced surface normally so the client can output it.
- If another client sends a show-existing form through VA, reject it unless
  the libva contract for that submission is demonstrated and tested.
- Never make a surface referenceable until its fence succeeds.
- A failed frame must not update any persistent surface state.
- The backend must load primary-reference CDF/segmentation/motion state from
  the exact referenced surface token and store current state with the current
  surface token.

This surface-keyed rule is the architectural requirement that lets stock VA
calls work without `refresh_frame_flags`.

### Film grain and two-output semantics

`VADecPictureParameterBufferAV1` can specify:

- `current_frame`: ungrained decoded reference picture; and
- `current_display_picture`: grained display output.

FFmpeg uses a temporary surface for the first when it asks the driver to apply
film grain. The backend must prove one of these implementations:

1. hardware produces both an ungrained reference and a grained display
   surface;
2. hardware retains ungrained reference state internally and exports/copies it
   to `current_frame` while producing the display surface; or
3. hardware decodes ungrained, then a validated RGA/GPU/CPU grain stage creates
   the display surface without modifying the reference.

Returning the same grained pixels for both surfaces is incorrect and will
poison future references. If the selected backend cannot implement two-output
semantics, reject `apply_grain` while experimental and keep AV1 unadvertised by
default. “Ignore grain” is acceptable only when the client explicitly asks to
export film-grain side data instead of applying it.

### 8-bit and 10-bit output

Profile 0 covers both 8-bit and 10-bit 4:2:0 in the libva AV1 API, so profile
alone cannot choose one RT format.

- Report both `VA_RT_FORMAT_YUV420` and `VA_RT_FORMAT_YUV420_10` only after
  both pass runtime probing.
- Accept NV12 for 8-bit and P010 for 10-bit surface creation.
- Select output depth from the picture parameter's `bit_depth_idx`, validate
  it against the surface format, and reject mid-picture mismatches.
- For MPP, validate the exact native/tiled/AFBC format on every supported
  hardware generation. Reuse the existing NV15/AFBC-to-P010 path only when
  its layout and synchronization are proven for AV1.
- For V4L2 VPU981, prefer the device's P010 post-processor format when exposed;
  do not assume a format that was not returned by enumeration.
- Treat a bit-depth change like a resolution change: drain/reset, renegotiate
  buffers, and advance the sequence generation.
- Verify full 10-bit sample values, strides, padding, modifiers, and dmabuf
  descriptors—not only visible 8-bit luma.

### Capability and runtime probing

Add an `RKAV1BackendCaps` populated at driver initialization or first AV1
query:

- backend kind and ABI version;
- Profile 0 support;
- 8/10-bit support;
- maximum coded width/height and tile count;
- NV12/P010 and modifier support;
- super-resolution, restoration, warped motion, screen-content/intrabc, and
  film-grain support;
- large-scale-tile support;
- whether two-output film grain is correct;
- whether surface-keyed state is supported.

For MPP, a successful `mpp_create`/`mpp_init` is not enough. Probe the parsed
API version and AV1 hardware client without submitting work that can hang.
For V4L2, enumerate `V4L2_PIX_FMT_AV1_FRAME`, verify
`V4L2_BUF_CAP_SUPPORTS_REQUESTS`, query all mandatory controls, enumerate
capture formats/frame sizes, and make a bounded known-answer submission.

Capability policy:

- add a narrow `RK_VAAPI_EXPERIMENTAL_PROFILES=av1-profile0` path for bring-up;
- never list AV1 merely because headers define `MPP_VIDEO_CodingAV1`;
- advertise `VAConfigAttribDecAV1Features.lst_support = 0` initially;
- keep Profiles 1 and 2 absent;
- return `VA_STATUS_ERROR_UNSUPPORTED_PROFILE` or
  `VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED` immediately when appropriate;
- quarantine a backend after a timeout/device error and fail later calls fast
  until a new context/reset proves recovery.

## Phased implementation

Each phase has an exit gate. Do not begin default capability work before the
earlier gates are closed.

### Phase 0 — make one hardware endpoint real

Tasks:

- Choose and document the first platform track:
  - vendor kernel + MPP `VPU_CLIENT_AV1DEC`; or
  - mainline-style kernel + `rockchip,rk3588-av1-vpu-dec`.
- Fix/enable the matching kernel config, device-tree node, clocks, IOMMU,
  reserved memory, permissions, and sandbox device access.
- Pin a reviewed kernel branch with all relevant VPU981/MPP-service fixes.
- Pin MPP source/package revision for the MPP track.
- Enumerate V4L2 formats/controls or MPP capabilities in a checked-in probe
  script.
- Decode 8-bit and 10-bit known-answer streams outside VA with a hard timeout.
- Record kernel, DT compatible, MPP commit, formats, dimensions, output
  checksums, and logs.

Exit gate:

- 100 consecutive raw hardware decodes complete without a timeout;
- decoded checksums match software;
- both reset-after-error and process teardown work;
- at least one 10-bit sample succeeds if 10-bit is in the first release.

If neither hardware route passes, stop. A VA integration cannot repair a
missing kernel device.

### Phase 1 — freeze the input boundary

Tasks:

- Add a debug-only VA capture facility that writes a versioned, bounded record
  of AV1 picture params, slice params, tile bytes, surface relationships, and
  call order.
- Capture stock FFmpeg, Firefox, and GStreamer submissions for the same pinned
  clips.
- Build `RKAV1Picture`, `RKAV1Tile`, and `RKAV1SurfaceState`.
- Implement pure validation/mapping unit tests without hardware.
- Produce an explicit MPP parsed-API RFC and a libva AV1-gap report using the
  captures.
- Confirm with real clients whether any legal buffer order differs from
  FFmpeg's current order.

Exit gate:

- every field in the neutral picture has a documented VA source, stored
  per-surface source, canonical derivation, or explicit “unavailable” status;
- replay is deterministic;
- malformed captures cannot cause out-of-bounds access or excessive
  allocation.

### Phase 2A — MPP parsed-picture proof (recommended)

Tasks:

- Implement a minimal versioned MPP parsed AV1 API on a pinned branch.
- Reuse MPP's buffer-slot and AV1 HAL code, but bypass its OBU parser.
- Replace DPB-slot-only persistent state with backend state associated with
  explicit frame handles.
- Submit destination/reference buffers, normalized syntax, tiles, and
  per-surface state tokens.
- Return output state only after hardware completion.
- Add a standalone replay program that does not load libva.
- Compare results with MPP's normal OBU parser on a kernel image where that
  path works.

Exit gate:

- replay decodes key, inter, hidden-reference, multi-tile, and 10-bit pictures;
- output matches both software and normal MPP decoding;
- no internal MPP header is included by `rockchip-vaapi`;
- public API version/feature probing and error recovery are demonstrated;
- Rockchip either accepts the API direction or the project explicitly decides
  to own a pinned MPP fork.

### Phase 2B — V4L2 Request proof (parallel alternative)

Tasks:

- Enable the VPU981 node and verify it with an existing stateless client.
- Build a standalone media-request replay tool using the neutral picture.
- Map every libva field to V4L2 sequence/frame/tile/film-grain controls.
- Document all values that cannot be populated from stock VA.
- Prototype either the libva companion-state extension or a surface-keyed
  kernel state design.
- Test MMAP and dmabuf capture ownership, request reinitialization, stream
  reset, and resolution changes.

Exit gate:

- the replay tool passes the same picture set as Phase 2A;
- the `refresh_frame_flags` solution is explicit and reviewable—no inference
  heuristic;
- request fds, queue buffers, and capture surfaces have proven lifetimes;
- upstream maintainers have been approached before a private uAPI is frozen.

At the end of Phase 2, select the first production backend. Do not maintain two
unfinished hardware paths simultaneously.

### Phase 3 — integrate Profile 0 8-bit

Tasks:

- Wire AV1 buffer capture/normalization into `RenderPicture` and `EndPicture`.
- Add AV1 token routes and strong reference-surface ownership.
- Integrate the selected parsed backend with the existing worker and fences.
- Implement NV12 surface binding/export/map/derive-image paths.
- Handle flush, context destroy, destination reuse, hidden frames, client
  `show_existing_frame`, errors, and sequence changes.
- Add the narrow experimental capability gate and backend diagnostics.

Exit gate:

- pinned 8-bit feature vectors are bit-exact;
- stock FFmpeg and at least one browser complete decode, seek, and teardown;
- unavailable hardware fails in milliseconds, not seconds;
- H.264, HEVC, and VP9 regression tests remain unchanged.

### Phase 4 — 10-bit and complex tools

Tasks:

- Add P010/native-10-bit output negotiation and conversion as required.
- Validate super-resolution, restoration, CDEF, global/warped motion,
  segmentation, screen content/intrabc, multiple tile groups, and CDF update
  combinations.
- Implement and validate the two-surface film-grain path.
- Handle HDR color metadata and export separately from decoder syntax.
- Validate mid-stream size and depth changes.

Exit gate:

- all supported 10-bit vectors are sample-exact before grain;
- film-grain output matches the chosen reference implementation;
- ungrained reference reuse remains exact after grained display;
- P010 export works in Firefox, FFmpeg, GStreamer, and a DRM/Wayland consumer.

### Phase 5 — conformance, fuzzing, and robustness

Build a checksum-pinned corpus from AOM/AV1 conformance sources and small
locally generated clips. Cover at least:

- key, inter, intra-only, and switch frames;
- hidden frames and show-existing sequences;
- every reference name, duplicate references, order-hint wrap, primary-ref
  changes, and all refresh patterns;
- CDF update disabled/enabled and frame-end update disabled;
- one tile, maximum practical tiles, uniform/non-uniform layout, multiple tile
  groups, and truncated tile data;
- segmentation update-map/data combinations;
- CDEF and all restoration modes;
- super-resolution denominators;
- global/warped motion and reference-frame MVs;
- lossless, high precision MVs, compound/skip modes;
- screen-content tools and intrabc;
- 8-bit and 10-bit 4:2:0;
- film grain update/reuse and separate display surface;
- resolution/bit-depth changes, flush, seek, EOS, corrupt headers, and device
  errors.

Test layers:

- pure parameter-mapping unit tests;
- capture/replay tests;
- software-versus-hardware `framemd5`/raw-plane comparisons;
- libFuzzer/AFL++ target for AV1 VA buffers and tile arrays;
- ASan/UBSan and Valgrind;
- TSan or targeted race tests for surface/job teardown;
- two and four concurrent AV1 contexts;
- concurrent AV1 + H.264/HEVC/VP9/encode contexts;
- multi-hour 4K loop with fd, RSS, dma-buf, and kernel error monitoring.

Exit gate:

- zero unexpected mismatches on the pinned supported corpus;
- every unsupported feature fails before hardware submission;
- no sanitizer finding, kernel warning, hang, or resource trend;
- an error in one context does not poison other contexts.

### Phase 6 — application and sandbox qualification

Qualify:

- stock FFmpeg AV1 VAAPI;
- Firefox release and ESR media/RDD processes;
- Chromium/Electron VA path where enabled;
- GStreamer `vaav1dec`;
- mpv and VLC over their distro FFmpeg/libva builds;
- Wayland and X11 presentation;
- browser sandbox access to the exact MPP or media/video device nodes.

Test normal playback, rapid seek, tab close during decode, suspend/resume,
background throttling, encrypted-site fallback behavior without implementing
protected decode, and repeated decoder creation.

Exit gate:

- no application-specific patch is required;
- sandbox policy is documented and least-privilege;
- failures fall back without globally disabling otherwise working hardware
  codecs.

### Phase 7 — staged release

1. Ship the experimental Profile 0 gate to developers.
2. Collect backend/version/device telemetry through explicit logs, not silent
   phone-home reporting.
3. Publish the exact tested kernel, MPP, libva, FFmpeg, and vector revisions.
4. Enable Profile 0 by default only on an allowlist of proved backend ABI and
   device combinations.
5. Expand the allowlist after hardware CI passes.
6. Keep a runtime kill switch for newly discovered kernel/MPP regressions.
7. Revisit large-scale tiles and additional hardware generations only as
   separate conformance-qualified increments.

## Upstream work plan

### Rockchip MPP RFC

Prepare a small standalone RFC before writing the full VA adapter:

- show the exact standard VA buffers from FFmpeg;
- show that the existing public MPP path unconditionally parses OBUs;
- map the VA/V4L2 fields to MPP's internal AV1 picture syntax;
- propose versioned public parsed submission and surface state tokens;
- include 8/10-bit, film-grain two-output, async completion, external buffers,
  and reset requirements;
- ask which RK3588 kernel client/device-tree combination Rockchip supports;
- request a non-hanging capability query for `VPU_CLIENT_AV1DEC`.

### libva RFC

Report the API inconsistency separately:

- the switch-frame comment references `refresh_frame_flags`, but the structure
  has no such member;
- V4L2 and stateful reconstruction need data the API drops;
- propose a companion buffer or versioned AV1 picture extension;
- specify old-client zero behavior and a capability bit;
- provide an FFmpeg producer patch and at least two driver consumers before
  standardizing original-OBU transport.

### Linux media RFC

If V4L2 becomes the selected backend:

- discuss whether reference-associated state can replace slot refresh for
  parsed APIs that identify reference frames explicitly;
- otherwise use the libva extension rather than inventing an unupstreamable
  ioctl;
- include conformance results and verify all fixes required by the VPU981
  driver;
- use the media Request API exactly as documented.

### Client patches

Client patches are acceptable for diagnostics and standards development, but
the release gate remains stock-client compatibility. FFmpeg should be the
first producer for any proposed libva extension because its AV1 parser already
owns every missing field.

## Risk register

| Risk | Impact | Mitigation / stop condition |
| --- | --- | --- |
| No AV1 hardware node/client on target image | Total blocker | Phase 0 raw known-answer test; stop driver work if it fails |
| Rockchip declines a public parsed API | High | Decide explicitly between a maintained pinned MPP fork and V4L2 investment |
| Missing libva state proves needed beyond CDF | High | Field inventory and capture/replay before backend integration |
| Surface-keyed state cannot be fitted into MPP HAL | High | Prove in standalone Phase 2A; do not fall back to guessed refresh bits |
| V4L2 refresh semantics require uAPI/kernel change | High | Upstream RFC or libva extension; no private release ioctl |
| Film grain cannot produce two correct surfaces | High | Keep feature/profile experimental; never alias grained reference |
| 10-bit layout/modifier mismatch | High | Enumerate formats, raw P010 checksum tests, modifier-aware export |
| Hardware/MPP accepts input but silently corrupts rare tools | High | Feature-stratified conformance and fail-closed capability allowlist |
| Browser timeout disables all hardware decode | High | bounded probe, fast synchronous rejection, backend quarantine |
| MPP/kernel upgrades break ABI or output | High | versioned feature probe, pinned CI matrix, runtime kill switch |
| Tile offset/count bug becomes browser-process memory corruption | Critical | checked arithmetic, strict caps, fuzzing, sanitizers |
| State survives flush or surface-handle reuse | Critical | generation-tagged per-surface state and atomic reset |
| New backend regresses existing codecs | Medium | separate AV1 path and full existing codec regression gate |
| Carrying two backends doubles unfinished work | Medium | choose one production backend after Phase 2 proofs |

## Rough effort and sequencing

These are engineering ranges, not release promises:

| Work | MPP-first estimate | V4L2-first estimate |
| --- | ---: | ---: |
| Platform enablement and reproducible probes | 2–4 weeks | 2–5 weeks |
| Input capture, neutral model, validation | 2–4 weeks | 2–4 weeks |
| Backend/API prototype | 6–12 weeks | 8–16 weeks |
| Driver integration and 8-bit output | 4–8 weeks | 5–9 weeks |
| 10-bit, film grain, complex tools | 4–8 weeks | 4–8 weeks |
| Conformance, applications, hardening | 6–12 weeks | 6–12 weeks |

Some work overlaps, but upstream review and hardware access dominate elapsed
time. The fastest credible sequence is one engineer on the neutral
translator/capture harness while another MPP or kernel maintainer implements
the parsed backend. Without that backend cooperation, schedule risk is high.

## Immediate next actions

1. Fix the RK3588 board image so one raw AV1 hardware path passes Phase 0.
2. Use the checked-in bounded platform probe as the Phase 0 report, then add
   known-answer submission only after the endpoint can reset and terminate
   reliably.
3. Implement the debug-only VA AV1 capture/replay format.
4. Build the neutral AV1 picture and pure mapping tests.
5. Send the MPP parsed-API RFC with a minimal standalone proof.
6. Open the libva API-gap discussion with an FFmpeg field map.
7. Choose MPP-first or V4L2-first only after the two Phase 2 proof results.
8. Keep AV1 absent from normal `vainfo` output in the meantime.

## Source index

Primary implementation/specification sources used for this plan:

- [AOMedia AV1 specification](https://aomediacodec.github.io/av1-spec/)
- [libva AV1 decode API](https://github.com/intel/libva/blob/master/va/va_dec_av1.h)
- [FFmpeg AV1 decoder](https://github.com/FFmpeg/FFmpeg/blob/master/libavcodec/av1dec.c)
- [FFmpeg AV1 VAAPI adapter](https://github.com/FFmpeg/FFmpeg/blob/master/libavcodec/vaapi_av1.c)
- [Rockchip MPP repository](https://github.com/rockchip-linux/mpp)
- [Rockchip MPP AV1 parser](https://github.com/rockchip-linux/mpp/blob/develop/mpp/codec/dec/av1/av1d_parser.c)
- [Rockchip MPP AV1 internal syntax](https://github.com/rockchip-linux/mpp/blob/develop/mpp/common/av1d_syntax.h)
- [Linux media Request API](https://www.kernel.org/doc/html/latest/userspace-api/media/mediactl/request-api.html)
- [Linux stateless codec controls](https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/ext-ctrls-codec-stateless.html)
- [Linux RK3588 VPU981 AV1 driver](https://github.com/torvalds/linux/blob/master/drivers/media/platform/verisilicon/rockchip_vpu981_hw_av1_dec.c)
- [GStreamer stateless AV1 decoder](https://gstreamer.freedesktop.org/documentation/v4l2codecs/v4l2slav1dec.html)
- [Chromium stateless V4L2 implementation](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/media/gpu/v4l2/)
- [Bootlin libva V4L2 Request driver](https://github.com/bootlin/libva-v4l2-request)
