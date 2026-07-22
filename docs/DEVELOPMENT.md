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
│   ├── export.c               # DRM PRIME 2 descriptor construction
│   ├── export.h
│   ├── log.c                  # Thread-safe driver logging
│   ├── log.h
│   ├── mpp_dec.c              # MPP pool, packet, routing, and worker backend
│   ├── mpp_dec.h
│   ├── surface.c              # Surface lifecycle, fence waits, and readback
│   ├── surface.h
│   ├── h264.c                 # H.264 SPS/PPS Annex B reconstruction
│   ├── h264.h
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
`pthread_once`-initialized sink and stdio stream locking so independent decode
workers cannot interleave log records. DRM PRIME 2 descriptor construction is
isolated in `export.c`; it synchronizes pending surfaces through the narrow
interface in `surface.h` before duplicating and describing the active DMA-BUF.
Surface allocation, teardown, status/fence waits, and DMA-BUF-synchronized
image readback now live together in `surface.c`. Context creation/destruction,
picture lifecycle, and render-target ownership are isolated in `context.c`.
`mpp_dec.c` owns packet construction, external-pool management, frame routing,
submission/draining, and the dedicated worker. The main translation unit now
owns capability/configuration policy, stubs, initialization, and vtable wiring.

---

## VA-API vtable (`VADriverVTable`)

The entry point `__vaDriverInit_1_20` fills all 50+ function pointers. The
functions that actually do meaningful work are:

| Function | Role |
|----------|------|
| `rk_CreateConfig` | Validate profile/entrypoint; allocate `RKConfig` |
| `rk_CreateContext` | `mpp_create` + `mpp_init`; retain render-target hints; allocate `RKContext` |
| `rk_CreateSurfaces2` | Allocate `RKSurface` objects and pre-decode placeholder DMA-BUFs |
| `rk_CreateBuffer` | Allocate a dynamic, stale-safe `RKBuffer` object and copy caller data |
| `rk_BeginPicture` | Reset the render target and advance its surface fence |
| `rk_RenderPicture` | Collect buffer IDs into `pending[]` and snapshot H.264 parameters |
| `rk_EndPicture` | Build an owned packet and enqueue it to the context worker |
| `rk_QuerySurfaceAttrs` | Report NV12 + `DRM_PRIME_2` support (critical for Firefox) |
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
6. The worker drains `mpi->decode_get_frame`; H.264 resolves the output token
   after display reordering, while VP9 uses its ordered route queue.
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
deadline expires. Context teardown joins the worker and fails any remaining
queued or routed fences before destroying MPP.

---

## DRM PRIME 2 surface export

Firefox calls `vaExportSurfaceHandle` with
`VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2` to get a zero-copy handle to the
surface's currently bound decode buffer (or its placeholder before decode).

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
allocation remains only so pre-decode PRIME capability probes can succeed.

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

## Extending to other codecs

To add HEVC/VP9/AV1 decode, implement a `do_hevc_decode()` (etc.) function
analogous to `do_h264_decode()`:

1. In `rk_EndPicture`, dispatch on `ctx->coding`:
   ```c
   if (ctx->coding == MPP_VIDEO_CodingHEVC)
       return do_hevc_decode(d, ctx, target);
   ```
2. HEVC/VP9/AV1 use `VAPictureParameterBufferHEVC`, `VASliceParameterBufferHEVC`,
   etc. The header reconstruction challenge is codec-specific:
   - HEVC: reconstruct VPS/SPS/PPS NALUs (more complex than H.264)
   - VP9/AV1: no NAL structure; MPP accepts raw IVF/OBU frames directly

3. Update `profile_supported`, `rk_QueryConfigProfiles`, and the conformance
   manifest together. The driver currently advertises only validated H.264
   Main/High and VP9 Profile 0 decode.

---

## Firefox process model

Firefox decodes video in the **RDD (Remote Data Decoder)** sandboxed process.
The VA-API driver runs inside RDD. By default, RDD's seccomp sandbox blocks
`/dev/dri` device access, which prevents MPP from opening the VPU. The
workaround is `MOZ_DISABLE_RDD_SANDBOX=1`.

For a hardened deployment, the correct long-term fix is to add `/dev/dri` and
Rockchip's `/dev/mpp_service` to Firefox's RDD sandbox allow-list (requires
patching Firefox's sandbox policy).

---

## Known limitations

- H.264 SPS reconstruction uses `level_idc=51` (5.1) unconditionally. The
  actual level is not exposed by `VAPictureParameterBufferH264`; 5.1 is safe
  for all content up to 4K@60fps.
- `num_ref_idx_l0/l1_default` in PPS is hardcoded to 0 (= 1 reference).
  Multi-reference B-frame content may require this to match the stream.
- Maximum 64 surfaces / 8 concurrent decode contexts. Increase `MAX_SURFACES`
  and `MAX_CONTEXTS` if needed.
- Only H.264 header reconstruction is implemented. HEVC/VP9/AV1 decode paths
  call `do_h264_decode` as a stub; full implementation is pending.

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
