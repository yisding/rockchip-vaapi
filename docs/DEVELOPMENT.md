# Developer documentation — rockchip-vaapi

**Author:** Eduardo García-Mádico Portabella — EGP Sistemas
**Contact:** woodyst@gmail.com

---

## Project overview

`rockchip-vaapi` is a VA-API 1.20 driver (`rockchip_drv_video.so`) that
bridges the `libva` API to the Rockchip MPP (Media Process Platform) library.
It was written from scratch because no open VA-API driver exists for the
Rockchip RK3588 SoC.

### Why a VA-API driver and not GStreamer or V4L2?

- **GStreamer**: Firefox 128+ has no GStreamer media backend. The code path
  (`dom/media/platforms/gstreamer/`) was removed; only the VA-API PDM remains.
- **V4L2**: Rockchip exposes a V4L2 interface, but it uses virtual M2M devices
  that are incompatible with Firefox's V4L2 backend (which expects real
  `V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE` M2M).
- **VA-API**: Firefox uses `libva` for hardware decode on Linux via its
  VA-API Platform Decode Module (PDM), running inside the RDD (Remote Data
  Decoder) sandboxed process.

---

## Source tree

```
rockchip-vaapi/
├── src/
│   ├── rockchip_drv_video.c   # Main driver: full VA-API vtable
│   ├── driver_internal.h      # Shared private object model and heap access
│   ├── buffer.c               # VA buffer and image object lifecycle
│   ├── buffer.h
│   ├── context.c              # Context lifecycle and MPP decode workers
│   ├── context.h
│   ├── convert.c              # RGA-backed NV15-to-P010 conversion
│   ├── convert.h
│   ├── export.c               # DRM PRIME 2 descriptor construction
│   ├── export.h
│   ├── log.c                  # Thread-safe driver logging
│   ├── log.h
│   ├── mpp_dec.c              # MPP pool, packet, routing, and worker backend
│   ├── mpp_dec.h
│   ├── mpp_enc.c              # Experimental synchronous MPP encode backend
│   ├── mpp_enc.h
│   ├── surface.c              # Surface lifecycle, fence waits, and readback
│   ├── surface.h
│   ├── h264.c                 # H.264 SPS/PPS Annex B reconstruction
│   ├── h264.h
│   ├── hevc.c                 # HEVC VPS/SPS/PPS Annex B reconstruction
│   ├── hevc.h
│   ├── frame_layout.c         # Checked NV12 sizing and frame copies
│   ├── frame_layout.h
│   ├── object_heap.c          # Dynamic generation-tagged VA handles
│   ├── object_heap.h
│   ├── vp9.c                  # VP9 header parsing/repeat construction
│   ├── vp9.h
│   └── bs.h                   # Header-only Exp-Golomb bitstream writer
├── tests/                     # Unit + conformance-vector harnesses
├── debian/                    # Debian packaging metadata
├── docs/
│   ├── DEVELOPMENT.md         # This file
│   ├── ROADMAP.md             # Production target and phased plan
│   └── TESTING.md             # Reproducible validation gates
├── Makefile
├── README.md
└── INSTALL.md
```

---

## Architecture

```
Firefox (RDD process)
    │
    │  VA-API calls (libva 1.20)
    ▼
rockchip_drv_video.so          ← this driver; owns the external decode pool
    │
    │  MppApi calls (librockchip-mpp1)
    ▼
Rockchip MPP
    │
    │  kernel ioctl
    ▼
RK3588 VPU (hardware)
    │
    │  retained external-pool DMA-BUF fd (PRIME 2)
    ▼
Firefox compositor (zero-copy from the exported surface onward)
```

### Object model

Phase 1 replaced all original fixed arrays with the dynamic heap in
`object_heap.c`. Configs, contexts, surfaces, buffers, and images each use a
distinct handle type. A `VAImageID` owns a separate backing `VABufferID`, so
the two namespaces no longer alias.

Heap handles encode a 4-bit object type, an 8-bit generation, and a 20-bit
slot index. Lookup rejects the wrong type and any stale generation. Slots grow
dynamically, and a slot is retired instead of wrapping its generation, so an
old handle cannot alias a later object. Objects are atomically reference-counted:
the driver holds `object_lock` only while acquiring/removing a heap entry, and
the acquired object remains alive after that short critical section.

`driver_internal.h` is private to the driver translation units. It centralizes
the shared object layouts and short heap-acquire helpers without exposing them
as public ABI. Buffer/image ownership now lives in `buffer.c`; logging uses a
reference-counted sink guarded by one mutex, so independent decode workers
cannot interleave records and the final VA display closes the file cleanly.
Every record carries realtime nanoseconds, PID/TID, severity, source, line,
function, and message. `RK_VAAPI_LOG_LEVEL` filters five levels and
`RK_VAAPI_LOG_FORMAT=json` selects newline-delimited, control-character-safe
JSON; the default text format preserves existing audit substrings. DRM PRIME 2
descriptor construction is isolated in `export.c`; it synchronizes pending
surfaces through the narrow interface in `surface.h` before duplicating and
describing the active DMA-BUF. Surface allocation, teardown, status/fence
waits, and DMA-BUF-synchronized image readback now live together in
`surface.c`. Context creation/destruction, picture lifecycle, and render-target
ownership are isolated in `context.c`. `mpp_dec.c` owns packet construction,
external-pool management, frame routing, submission/draining, and the dedicated
worker. The main translation unit now owns capability/configuration policy,
stubs, initialization, and vtable wiring.

---

## VA-API vtable (`VADriverVTable`)

The entry point `__vaDriverInit_1_20` fills all 50+ function pointers. The
functions that actually do meaningful work are:

| Function | Role |
|----------|------|
| `rk_CreateConfig` | Validate profile/entrypoint; allocate `RKConfig` |
| `rk_CreateContext` | `mpp_create` + `mpp_init`; retain render-target hints; allocate `RKContext` |
| `rk_CreateSurfaces2` | Validate NV12/P010 attributes; allocate format-aware `RKSurface` objects and pre-decode placeholder DMA-BUFs |
| `rk_CreateBuffer` | Allocate a dynamic, stale-safe `RKBuffer` object and copy caller data |
| `rk_BeginPicture` | Reset the render target and advance its surface fence |
| `rk_RenderPicture` | Collect buffer IDs into `pending[]` and snapshot H.264/HEVC picture and IQ state |
| `rk_EndPicture` | Build an owned packet and enqueue it to the context worker |
| `rk_QuerySurfaceAttrs` | Report settable NV12/P010 + `DRM_PRIME_2` support (critical for Firefox) |
| `rk_ExportSurfaceHandle` | Return `VADRMPRIMESurfaceDescriptor` with `dup()`'d DMABUF fd |
| `rk_SyncSurface` / `rk_SyncSurface2` | Wait on the surface fence indefinitely or for the requested timeout |
| `rk_Terminate` | Destroy MPP contexts; close fds; `free(RKDriver)` |

All other vtable slots are filled with stubs that return `VA_STATUS_SUCCESS`
(or `VA_STATUS_ERROR_UNIMPLEMENTED` where appropriate) to satisfy libva's
initialization checks.

---

## H.264 decode pipeline

### The SPS/PPS problem

VA-API decouples parameter parsing from decoding. Firefox (via FFmpeg) parses
the H.264 bitstream and extracts the SPS and PPS as C structs
(`VAPictureParameterBufferH264`, `VAIQMatrixBufferH264`), then passes them via
`vaRenderPicture`. It does **not** pass the original binary NALUs.

MPP's `decode_put_packet` requires an Annex B bitstream with SPS/PPS parameter
sets. The driver must therefore **reconstruct** the binary parameter sets from
the parsed VA structs. It sends an SPS on the first frame and IDRs, and a PPS
before every frame so inferred reference counts and scaling lists stay exact.

### SPS/PPS reconstruction (`h264.c`)

`h264_write_sps()` and `h264_write_pps()` encode the structs back to binary
using Exp-Golomb coding:

1. A `BSWriter` (from `bs.h`) writes individual fields as `u(n)`, `ue(v)` or
   `se(v)` elements into a temporary `raw[]` buffer.
2. The NAL header byte is written first (`0x67` for SPS, `0x68` for PPS).
3. High-profile SPS includes `chroma_format_idc` and bit-depth fields.
4. `emulation_prevent()` scans the raw bytes and inserts `0x03` bytes before
   any `0x000001` or `0x000002` sequences (required by the H.264 spec).
5. Scaling lists from `VAIQMatrixBufferH264` are emitted explicitly in the PPS
   in H.264 zig-zag scan order.
6. A 4-byte Annex B start code (`00 00 00 01`) is prepended to the output.

### Packet assembly and worker dispatch

For each `EndPicture` call the driver:

1. Finds the `VAPictureParameterBufferH264` and all `VASliceDataBuffer` blobs
   in `pending[]`.
2. Prepends the SPS on the first frame and IDRs, and a reconstructed PPS on
   every frame.
3. Prepends `00 00 00 01` start codes to each slice NALU.
4. Copies everything into an owned decode job carrying the target surface and
   its current fence, then returns from `EndPicture` after queueing the job.
5. The per-context worker assigns a unique token, calls
   `mpi->decode_put_packet`, and handles input backpressure by draining output
   through MPP's bounded blocking wait before retrying.
6. The worker drains `mpi->decode_get_frame`; H.264 and HEVC resolve the output
   token after display reordering, while VP9 uses its ordered route queue.
7. Validates the returned external buffer index and layout, binds the frame
   and backing-buffer reference to the logical VA surface, then signals
   `surface->cond`. No decoded pixels are copied.

### Worker and surface-fence model

Each `RKContext` creates one worker after MPP initialization. Runtime packet
submission, output draining, info-change acknowledgement, and external-group
configuration occur on that worker; VA calling threads never poll MPP. The
output wait uses MPP's 20 ms blocking timeout so shutdown and newly queued
jobs remain responsive without `usleep` loops.

`BeginPicture` advances a monotonically increasing fence on the target
surface. Every job and frame-route record carries that fence. A late frame for
a surface that has already been reused is therefore released instead of being
bound to the newer picture. The exception is a declared H.264 field pair:
FFmpeg submits the two fields through separate Begin/Render/End sequences on
one surface, while MPP emits the completed frame with the first field's PTS.
The second field therefore shares the first field's fence and route token.
This continuation is gated by `field_pic_flag`; applying it to VP9 surface
reuse can expose an older hidden frame before the newer visible frame arrives.

On successful binding or asynchronous failure, the worker broadcasts
`surface->cond`. `vaSyncSurface` waits indefinitely as required by libva;
`vaSyncSurface2` returns `VA_STATUS_ERROR_TIMEDOUT` when its nanosecond
deadline expires.

Because `vaSyncSurface` has no timeout, a backend that stops responding would
otherwise hang the calling media process forever. The worker tracks how long it
has had frames outstanding with no output at all, and after ten seconds fails
every pending route so each waiting caller gets
`VA_STATUS_ERROR_DECODING_ERROR` and can fall back. Frames MPP delivers
afterwards find no route and are dropped safely.

Teardown does **not** cancel in-flight work. VA surfaces belong to the display,
not the context: applications legitimately destroy a decode context on a
sequence change and then sync surfaces the old context was still filling.
`rk_mpp_dec_stop` therefore puts the worker into a drain, signals end-of-stream
to MPP so codecs holding pictures release them, and stops as soon as MPP marks
the stream ended. A one-second deadline bounds the drain so a wedged backend
cannot block `vaDestroyContext`; anything still outstanding then has its fence
failed. A player that stops mid-stream routinely lands here, and that is
reported rather than treated as a fault.

H.264 and HEVC both run MPP with immediate output. Their outputs are routed
back to their surface by token, so MPP's own display reordering buys the driver
nothing and costs it latency plus pictures stranded whenever an application
stops feeding the decoder. VP9 keeps reordering because its routing is the
submission-order FIFO.

---

## DRM PRIME 2 surface export and `vaDeriveImage`

Not every consumer uses `vaExportSurfaceHandle`. VLC's OpenGL VA-API
converters derive an image from the decoded surface, take that image's buffer
handle as a DRM PRIME fd, and import it as an EGLImage; while `vaDeriveImage`
was a stub VLC dropped its hardware decoder module entirely and fell back to
software after creating surfaces through this driver.

`vaDeriveImage` returns a `VAImage` that aliases the surface's own linear NV12
DMA-BUF rather than a fresh allocation: pitches and offsets are the surface's
real layout, `data_size` is the image extent rather than the whole allocation
(MPP's decode buffers reserve codec side data past the picture),
`vaMapBuffer` mmaps that buffer with dma-buf CPU access brackets, and
`vaAcquireBufferHandle` exports it as `DRM_PRIME`. The image holds a reference
on the surface, so it stays valid after the caller drops theirs.

A `VAImage` fixes its pitches once, but a surface's layout is only final after
decode binds the real frame -- and consumers legitimately derive during pool
setup, before that. Two rules keep the alias honest:

- **10-bit surfaces are refused.** Their placeholder is sized for the declared
  linear P010, while the decoded frame arrives as AFBC NV15 and RGA repacks it
  at MPP's stride; the two disagree. Consumers fall back to `vaGetImage`, whose
  readback resolves the layout per call.
- **Encoder-input surfaces are refused.** A surface created with
  `VASurfaceAttribUsageHint` set to `ENCODER` is written, not read: its content
  arrives through `vaPutImage`, which validates pitches and offsets and
  interleaves planar chroma into native NV12. Offering a derived alias made
  GStreamer's encoder negotiate NV12 and then write its I420 source straight
  through it, which encoded at 10.5 dB PSNR.
- **Map and acquire re-resolve the surface.** If the pitch or chroma offset has
  changed since the image was derived, the call fails rather than reading
  pixels through the wrong geometry. If only the buffer changed, the stale
  mapping is dropped and the current buffer is mapped instead.

Imported RGB and still-compressed AFBC layouts are refused for the same reason,
as is a request for any memory type other than DRM PRIME.

Firefox calls `vaExportSurfaceHandle` with
`VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2` to get a zero-copy handle to the
surface's currently bound decode buffer (or its placeholder before decode).
The surface's requested RT/pixel format is recorded at creation, so a
pre-decode 10-bit capability probe receives a sufficiently sized P010 object
with either a composed P010 layer or split R16/GR1616 layers. Decode later
replaces the placeholder with the retained NV12 frame or converted P010
backing buffer without changing that consumer-facing format contract.

`rk_ExportSurfaceHandle` returns a `VADRMPRIMESurfaceDescriptor`:

```c
desc->fourcc    = VA_FOURCC_NV12;
desc->width     = surface->width;
desc->height    = surface->height;
desc->num_layers = 2;
// Layer 0: Y plane
desc->layers[0].drm_format = DRM_FORMAT_R8;
desc->layers[0].object_index[0] = 0;
desc->layers[0].offset[0] = 0;
desc->layers[0].pitch[0]  = hstride;
// Layer 1: UV plane
desc->layers[1].drm_format = DRM_FORMAT_GR88;
desc->layers[1].object_index[0] = 0;
desc->layers[1].offset[0] = hstride * vstride;
desc->layers[1].pitch[0]  = hstride;
// Object (shared DMABUF)
desc->objects[0].fd             = dup(mpp_buffer_get_fd(active_buffer));
desc->objects[0].size           = mpp_buffer_get_size(active_buffer);
desc->objects[0].drm_format_modifier = DRM_FORMAT_MOD_LINEAR;
```

The `dup()` is essential: Firefox closes the exported fd when done. The
surface retains the `MppFrame`; for an external buffer it also retains the
driver-owned backing `MppBuffer`, because MPP's `EXT_DMA` wrapper borrows the
fd rather than closing it. Context and bound surfaces share a refcounted
decode-pool object, so group teardown occurs only after the final frame and
backing reference is returned; no MPP orphan group is left until process exit.

`vaGetImage` reads through the retained backing object and brackets CPU access
with `DMA_BUF_IOCTL_SYNC` start/end operations. Without that ownership
transition, the direct VPU-written mapping produced an intermittent stale
frame even though routing and buffer indices were correct.

### 10-bit NV15-to-P010 conversion

MPP reports 10-bit 4:2:0 output as compact NV15
(`MPP_FMT_YUV420SP_10BIT`). Apps expect P010, so `assign_mpp_frame` converts
NV15 from the external decode pool into a driver-owned P010 buffer via
`rk_convert_nv15_to_p010`. HEVC Main10 and VP9 Profile 2 contexts request
`MPP_FRAME_FBC_AFBC_V2`: VDPU383's normal linear 10-bit byte stride is not
always a whole, 64-aligned number of compact pixels and cannot be passed to
librga honestly. AFBC input uses `mpp_frame_get_fbc_hdr_stride()` as its pixel
stride, `IM_AFBC16x16_MODE`, and MPP's crop offsets as the source rectangle.
Linear NV15 remains fail-closed unless byte stride, pixel stride, alignment,
and buffer bounds are all mutually consistent.

RK3588 AFBC input can run only on RGA3, whose vendor table requires both input
and output active widths to be at least 68. `rk_CreateContext` therefore
returns `VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED` for a narrower HEVC Main10
or VP9 Profile 2 context before configuring MPP. The conversion helper repeats
the same geometry guard before allocating its destination or calling librga,
so a later crop/info-change mismatch still cannot reach `/dev/rga`.

The converted P010 buffer is stored as `surface->backing_buf` and the source
`MppFrame` is released back to MPP after the surface is signaled. Export and
image readback always prefer `backing_buf`, which lets the 10-bit path expose
P010 without retaining compact NV15 as the public surface memory. If librga is
not available or conversion fails, the frame is not bound and the surface fence
is failed; the driver must not report success with compact NV15 described as
P010.

### Why `vaQuerySurfaceAttributes` matters

This is the function Firefox calls **before** attempting hardware decode to
check whether the driver supports DRM PRIME 2. If it returns 0 attributes
or omits `VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2`, Firefox silently falls back
to software decode. The driver must return:

```c
// Pixel format
attrs[0].type  = VASurfaceAttribPixelFormat;
attrs[0].value.value.i = VA_FOURCC_NV12;
// Memory type: both VA and DRM PRIME 2
attrs[1].type  = VASurfaceAttribMemoryType;
attrs[1].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_VA
                       | VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
// Max dimensions
attrs[2].type  = VASurfaceAttribMaxWidth;
attrs[2].value.value.i = 7680;
attrs[3].type  = VASurfaceAttribMaxHeight;
attrs[3].value.value.i = 4320;
```

---

## Memory model and the CMA constraint (4K)

There are three driver/decoder allocations in play:

| Buffer | Allocated by | Backing memory | Must be contiguous? |
|--------|--------------|----------------|---------------------|
| Per-surface placeholder (`priv_buf`) | driver, at surface creation | MPP DRM allocator | platform-dependent |
| 24-buffer decode pool | driver, after MPP info-change | MPP DRM allocator, committed as `EXT_DMA` | VPU-compatible dma-buf |
| GPU compositor textures | Mali / Mesa in the application | GPU allocator | platform-dependent |

MPP writes directly into the 24-buffer context pool. `assign_mpp_frame` keeps
MPP's display-frame reference on the VA surface, so MPP cannot recycle that
buffer while the application may still display it. Surface reuse drops the
old frame, backing, and shared-pool references. A separate per-surface
allocation remains only so pre-decode PRIME capability probes can succeed. It
uses the declared NV12/P010 format and is sized for the exported linear layout
in addition to MPP's conservative alignment.

The exact physical placement of the DRM allocations is platform-dependent.
At 4K, the decode pool and GPU compositor can still put substantial pressure
on the board's CMA/IOMMU resources, so deployment sizing and the Phase 1
memory/fd soak gate remain important.

**Symptom of CMA exhaustion (important — it does not look like a driver bug):**
the 4K context acknowledges `info_change`, decodes frames cleanly
(`zero_copy=1`, no `TIMEOUT`, no VA error), then Firefox reports
`NS_ERROR_DOM_MEDIA_FATAL_ERR` with **no driver-side error at all**. MPP cannot
surface a failed contiguous allocation through the libva API, so from the
driver's point of view every call succeeded. The standalone `va_barcode_test`
decodes the same 4K stream fine because it does no GPU compositing and therefore
never contends for CMA.

**Fix:** raise CMA to `cma=512M` (or more) on the kernel command line. This is a
platform/deployment setting, not a driver change. See `INSTALL.md`.

Contrast this with AV1, which is a genuine *parse* failure: AV1 never
acknowledges `info_change` and decodes zero frames, because VA-API hands MPP
headerless tile data instead of a full OBU bitstream. CMA exhaustion is the
opposite — decode works, then a resource runs out.

`make probe-av1-platform` deliberately stops before decode submission. It
distinguishes the public MPP AV1 capability, a bound vendor `mpp_av1dec`
platform device, and a stateless V4L2 `AV1F` endpoint, then reports Phase 0 as
unqualified regardless of discovery. The neutral parsed-picture architecture,
missing libva state, and promotion gates are documented in
[`AV1_SUPPORT_PLAN.md`](AV1_SUPPORT_PLAN.md).

### External-pool surface binding

On the first info-change frame, the context allocates 24 conservative-size DRM
buffers and commits their fds to an MPP external group with stable indices.
`assign_mpp_frame` validates the returned index/fd pair and binds that frame to
the intended VA surface. This fixes the old aliasing bug without a CPU copy:

- **Buffer aliasing ("saltando frames")**: the retained `MppFrame` prevents
  reuse while the VA surface is live; the retained backing buffer keeps the
  borrowed external fd valid even if the context is destroyed first.
- **Hidden-reference export**: MPP normally withholds VP9 `show_frame=0`
  frames from `decode_get_frame`. The driver parses the refresh mask and sends
  a minimal `show_existing_frame` repeat, causing MPP to expose the completed
  hidden reference so it can be bound to the correct logical VA surface.

The pool grows on demand to a 64-frame ceiling. It cannot be sized correctly up
front: VA surfaces are created independently of the context and FFmpeg passes
no render targets to `vaCreateContext`, so the driver never learns how many
surfaces a consumer intends to hold. Frame-threaded FFmpeg allocated 29
surfaces for a large-DPB HEVC stream, every pool buffer ended up bound to a
surface the application still held, and MPP waited forever for a free one --
a hard deadlock. When the worker sees MPP refuse input while no drain can free
a buffer, it commits more; at the ceiling it reports a real error instead of
stalling.

## `bs.h` — Exp-Golomb bitstream writer

A header-only, zero-dependency, inline bitstream writer:

```c
BSWriter bs;
bs_init(&bs, buf, buf_size);

bs_write(&bs, value, n_bits);   // write n_bits of value
bs_write1(&bs, bit);            // write single bit
bs_write_ue(&bs, val);          // write unsigned Exp-Golomb (ue(v))
bs_write_se(&bs, val);          // write signed Exp-Golomb (se(v))
bs_rbsp_trailing(&bs);          // write RBSP trailing bits (1 + zeros to byte boundary)
size_t n = bs_bytes(&bs);       // number of bytes written
```

Exp-Golomb encoding for `ue(v)`:

```
val++ → find bit length k of val → write k zeros → write val in k+1 bits
```

---

## HEVC decode pipeline (under validation)

`hevc.c` reconstructs Annex B VPS/SPS/PPS units from the picture and IQ
buffers, including Main/Main10 profile-tier-level syntax, scaling-list scan
order, tiles, and the current short/long-term reference set. Scaling matrices
are emitted in the PPS because `VAIQMatrixBufferHEVC` is picture state; the SPS
enables scaling lists without freezing one picture's matrices as sequence
state. Slice NALs are parsed far enough to recover their PPS ID, consume any
SPS-table selection syntax using the VA-provided table counts, and rewrite
explicit RPS state from the current `ReferenceFrames[]` DPB view. Malformed and
unsupported state fails before a worker job is queued.

`mpp_dec.c` emits a PPS before every access unit so redefined picture state is
visible to MPP. It regenerates VPS/SPS bytes for each picture, but only prepends
them when they differ from the last successfully queued sequence bundle. This
propagates genuine sequence changes without resetting MPP's DPB at every CRA,
where following RASL pictures may still need pre-CRA references. The assembler
preserves already-prefixed slices and adds a start code to bare NALs. HEVC
shares H.264's token-based output routing because both codecs may reorder
display output.

This path is intentionally not advertised yet. Adding a profile requires
changing `profile_supported`, its surface attributes, and the conformance
manifest together only after the on-device bit-exact gate passes. VP9 Profile
0 remains the other shipping path. Profile 2 has an experimental AFBC/P010
gate; AV1 remains future work.

`VAProfileHEVCMain` is advertised by default as of 2026-07-28. Unflagged
`ReferenceFrames[]` entries are preserved as follow references, current
short-term references are explicitly materialized after the original SPS-table
syntax is consumed, and explicit long-term entries are reproduced from the
stream rather than rebuilt -- `ReferenceFrames[]` carries no ordering, and
RefPicSetLtCurr order decides the initial reference list. All eight pinned
vectors are bit-exact, as are 142 of the 163 HEVC Main candidates in the FATE
conformance suite, with zero driver failures. The remainder are two streams MPP
itself cannot decode and two pictures beyond the advertised 7680x4320
constraint; all four fail closed. Errored or discarded MPP frames still become
decode failures rather than bound corrupt output.

---

## H.264 and HEVC encode pipeline (experimental)

`RK_VAAPI_EXPERIMENTAL_ENCODE=h264`, `hevc`, or `h264,hevc` adds
`VAEntrypointEncSlice` to H.264 Main/High and HEVC Main without changing
default capabilities. The encoder config stores the selected CQP, CBR, or VBR
mode. `vaRenderPicture` snapshots codec-specific sequence, picture, slice, and
supported misc parameters before applications can destroy those VA buffers;
`mpp_enc.c` maps them to common MPP prep/rate-control plus H.264 or HEVC keys.
HEVC advertises RK3588 MPP's native 64x64 CTU, 8x8 minimum coding-block, and
4x4-to-32x32 transform contract so applications do not guess 32x32 CTUs.
GStreamer may create a HEVC context rounded up to 16 pixels while retaining a
smaller visible input surface. That pairing is accepted only when the context
is the exact 16-pixel ceiling; MPP prep/frame dimensions use the visible
surface, while the VA sequence may use either visible or aligned dimensions.

System-memory upload uses `vaCreateImage` plus `vaPutImage`. The latter performs
a checked NV12/P010 copy, or interleaves I420/YV12 chroma into native NV12,
with explicit DMA-BUF CPU synchronization. Per-plane pitches, offsets, and
capacities are validated before every row access; `vaGetImage` performs the
inverse planar conversion. Planar formats are advertised only on 8-bit encode
configs. `vaEndPicture` submits the normalized NV12 buffer to rkvenc2, waits for
one MPP packet, and publishes it through a `VACodedBufferSegment`. MPP emits
H.264 SPS/PPS or HEVC VPS/SPS/PPS on each IDR. Coded-buffer overflow fails
closed.

`vaCreateSurfaces2` also implements DRM PRIME 2 import for application
surfaces. The accepted contract is deliberately narrow: one linear object, one
composed layer, exact visible dimensions, zero packed-RGB offset, and checked
pitches and object capacity. Linear NV12 and P010 require canonical Y/UV
offsets; P010 pitches are byte pitches and are converted to the surface's pixel
stride only after divisibility and width checks. For the currently advertised
8-bit encode paths, NV12 is submitted directly. RGBA, RGBX, BGRA, and BGRX use
an owned duplicate of the application fd and one synchronous RGA conversion
into the driver's aligned NV12 buffer per encoded frame.
Multi-object, tiled/modifier, undersized, mismatched, and non-DMA-BUF
descriptors fail during surface creation. Imported surfaces reject
`vaPutImage`, and imported RGB is advertised only when RGA was linked.

The current boundary remains progressive 8-bit input, one complete frame-level
macroblock/CTU slice, MPP-managed references, and synchronous completion. P010
encode, packed application headers, B-frames, multi-slice, and WebRTC encode
are not advertised. This also avoids exercising the kernel's
historically vulnerable multi-slice FIFO path. P010 surface import/readback is
not a Main10 encode claim: the tested MPP `vepu5xx` HAL rejects
`MPP_FMT_YUV420SP_10BIT`. The direct diagnostic and promotion criteria are in
[`HEVC_MAIN10_ENCODE_BACKEND.md`](HEVC_MAIN10_ENCODE_BACKEND.md).

`check-rgb-dmabuf-encode-experimental` imports a real BGRA DMA-BUF through the
public libva API, closes the descriptor fd after surface creation, converts 48
frames through RGA, encodes H.264 High, and standard-decodes the output. It
requires one import, conversion, and MPP packet audit at the expected
boundaries and measures 37.14 dB against a software BGRA-to-YUV reference.

`check-webrtc-rtp-experimental` carries a direct-I420 `vah264enc` stream
through `h264parse`, `rtph264pay`, and `rtph264depay`, captures every RTP
packet, and standard-decodes the resulting Annex B stream. This verifies the
WebRTC-compatible H.264 payload boundary and MTU behavior, but it does not
perform SDP/ICE/DTLS/SRTP negotiation with a peer.

`check-webrtc-peer-experimental` creates a sending and receiving `webrtcbin`
in one process. The harness exchanges the SDP offer/answer and trickled ICE
candidates directly, waits for both peer connections, then replaces the
negotiation-only RTP source with an `appsrc`/encoder/payloader bin on the
already negotiated transceiver. The receiving peer dynamically links
H.264 depayload/parser/output elements. Completion requires connected
DTLS-SRTP elements on both peers as well as exact access-unit, decode, PSNR,
I420-upload, and MPP-packet audits. `openh264enc` is available only as a
transport diagnostic; the Make target always selects `vah264enc`.

`check-encode-soak-experimental` holds live H.264 and HEVC GStreamer encoder
contexts open together at 30 fps. It samples the actual `gst-launch` process
RSS/fd counts and audits exact MPP packet counts plus checked I420 uploads. The
default two-hour run is the qualification gate; shorter durations are reported
only as smoke coverage.

---

## Debian package lifecycle

`make check-package-install` builds the driver and optional config packages,
runs Lintian, validates their metadata and payloads, and uses Bubblewrap plus
Fakeroot to exercise `dpkg` install, upgrade, config-only purge, reinstall, and
full purge against an empty isolated package database. It forces dependencies
only inside that database, after checking the packages declare the expected
MPP, RGA, libva, and exact-version driver relationships. The test does not
alter the host package database or install either package on the host.

---

## Firefox process model

Firefox decodes video in the **RDD (Remote Data Decoder)** sandboxed process.
The VA-API driver runs inside RDD. Its seccomp policy can block the Rockchip
MPP ioctl family and dma-heap allocation operations needed by this driver.
`MOZ_DISABLE_RDD_SANDBOX=1` is useful only as a short per-process diagnostic;
configuring it globally exposes untrusted media decoding without the RDD
seccomp boundary.

For a hardened deployment, a distribution Firefox sandbox policy must permit
the required MPP, RGA, and dma-heap operations in RDD. The driver and optional
config package deliberately do not weaken that sandbox. A source-package patch
for Firefox 153.0 is pinned and validated under `contrib/firefox`; it adds
the missing broker paths only on systems where those nodes exist and
allowlists the four MPP/RGA requests measured on the audited stack.

---

## Known limitations

- VA-API does not preserve the original H.264 `level_idc`; the driver derives
  the lowest Annex A level supported by the available frame/DPB constraints.
  Bitrate and frame-rate distinctions cannot be recovered from this buffer.
- HEVC Main is advertised. Two FATE conformance streams -- `NUT_A_ericsson_4`
  and `_5` -- cannot be decoded by MPP itself and fail closed, as do pictures
  beyond the advertised 7680x4320 constraint.
- HEVC Main10 has bit-exact generated 48-frame and pinned FATE 256-frame AFBC
  NV15-to-P010 development gates.
  VP9 Profile 2 has a separate bit-exact 48-frame gate through the same
  conversion path. Both remain unadvertised until broader conformance and HDR
  gates pass. VP8 and AV1 are also unadvertised.
- H.264 and HEVC encode are experimental and restricted to full-frame
  NV12/I420/YV12 uploads normalized to NV12 with one slice. They are exposed
  only through
  `RK_VAAPI_EXPERIMENTAL_ENCODE`.

---

## AI-assisted development notice

This driver was designed and implemented through interactive pair programming
with **Claude Sonnet 4.6** (model ID: `claude-sonnet-4-6`), a large language
model developed by Anthropic.

**Development timeline:** 24 April 2026
**Estimated interactive development time:** ~3–4 hours across two sessions

**AI contributions:**
- Initial architecture design (VA-API vtable layout, object model, ID namespacing)
- Complete `rockchip_drv_video.c` implementation (~1,000 lines)
- H.264 Annex B SPS/PPS reconstruction algorithm (`h264.c`, `bs.h`)
- DRM PRIME 2 / DMABUF surface export implementation
- Iterative debugging: `MPP_FRAME_FLAG_EOS` compatibility, STUB macro issues,
  `max_subpic_formats` libva init requirement, `rk_QuerySurfaceAttrs`
  returning correct attributes for Firefox
- Debian package structure (`debian/`)

**Human contributions:**
- Problem identification and system analysis (RK3588 hardware context)
- Live testing on Orange Pi 5 Plus hardware
- Firefox `about:config` configuration and environment variable tuning
- Validation that video plays correctly with hardware acceleration active
- Code review and approval at each iteration
- License, authorship, and packaging decisions

All code was validated on real hardware by Eduardo García-Mádico Portabella —
EGP Sistemas.
