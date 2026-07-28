# rockchip-vaapi

VA-API driver for Rockchip RK3588 / RK3576 that bridges **libva** to
**librockchip-mpp** (MPP), enabling hardware-accelerated video decode in
applications such as Firefox.

**Author:** Eduardo García-Mádico Portabella — EGP Sistemas
**Contact:** woodyst@gmail.com
**License:** LGPL-2.1-or-later

## Fork status (yisding/rockchip-vaapi)

This fork continues the upstream v1.0.11 work. Changes so far are tested on a
ROCK 5B (RK3588, vendor MPP stack on a 6.18 kernel) with a software-vs-VAAPI
`framemd5` bit-exactness harness (`tests/validate.sh`). The full normal and
ASan/UBSan gates are green on the audited fixed kernel build. The Phase 1
object model, module split, zero-copy worker/fence architecture, concurrent
decoder gates, and two-hour 4K resource soak are also complete; see
[docs/TESTING.md](docs/TESTING.md):

- **Fixed multi-reference / B-frame H.264 corruption.** VA-API never passes
  the original PPS, and the reconstructed PPS hardcoded
  `num_ref_idx_*_default_active` to 1 — any stream whose slices rely on the
  PPS default (x264 `ref>1`) decoded mostly garbage (measured: 109–117 of
  120 frames wrong at `ref=4:bframes=3`). The driver now re-emits a PPS
  before every frame with defaults taken from the frame's own slice
  parameters. All six configurations of `ref∈{1,2,4,8} × bframes∈{0,2,3}`
  plus a 4K clip are now bit-exact.
- **Fixed nondeterministic VP9 frame drops.** `EndPicture` is non-blocking;
  when MPP's input queue filled, `decode_put_packet` failures were treated
  as fatal and ffmpeg silently dropped frames (measured: 38 of 120 packets
  rejected on some runs, 5/10 runs corrupt). A per-context worker now owns
  MPP submission and bounded output draining, then retries backpressured puts.
- **VP9 output routing by FIFO instead of PTS**, since a
  `show_existing_frame` repeat of a hidden altref can surface with the
  altref packet's PTS and desync every later frame.
- **Preserved hidden VP9 references.** FFmpeg resolves
  `show_existing_frame` packets internally, so MPP never receives the repeat
  that would expose a hidden decoded buffer. The driver now parses the hidden
  frame's refresh mask and submits a bounded one-byte repeat to MPP, routing
  that output back to the VA surface that FFmpeg may later reuse. Host parser,
  sanitizer, and Valgrind checks pass, and the formerly quarantined hardware
  vector is bit-exact on the audited fixed kernel.
- **Zero-copy external decode buffers.** After MPP reports its exact layout,
  each context allocates and commits a 24-buffer DRM pool. VA surfaces retain
  the returned frame and its backing dma-buf until reuse; export and readback
  use that buffer directly, with dma-buf CPU synchronization around readback.
  The old per-frame CPU copy is gone, and pool lifetime is shared with bound
  surfaces so context teardown does not orphan MPP allocations.
- **Honest surface fences.** Decode jobs carry unique route tokens and the
  target's generation fence. The worker signals completion through the
  surface condition variable; `vaSyncSurface2` honors zero, finite, and
  infinite timeouts, and stale late frames cannot overwrite a reused surface.
  Declared H.264 field pairs share one fence/route, matching MPP's first-field
  PTS behavior without weakening VP9 reuse isolation.
- **Pinned real conformance vectors and CI plumbing.** The gate now uses ITU-T
  H.264 and official libvpx VP9 vectors with payload checksums, and normal plus
  sanitized AArch64 builds are cross-compiled in CI.
- **Direct HEVC backend reduction tooling.** The remaining TILES failure has a
  libva-free MPP runner and a control-gated access-unit prefix reducer; current
  host analysis isolates the first candidate to an IDR plus one replacement-PPS
  P-picture transition. See
  [`docs/HEVC_TILES_BACKEND.md`](docs/HEVC_TILES_BACKEND.md).
- **Honest capability advertising.** HEVC reconstruction is under Phase 2
  validation; seven of eight gated Main vectors are bit-exact, but the profile
  remains hidden until every pinned HEVC case is either supported bit-exactly
  or has a documented fallback contract. HEVC Main10 has a separate opt-in
  gate whose generated 48-frame and pinned 256-frame MPP AFBC-to-RGA P010
  paths are bit-exact, but it remains hidden pending broader conformance and
  app/display HDR presentation. VP9 Profile 2 has generated and official
  libvpx P010-exact AFBC/RGA gates and remains hidden pending app validation.
  HEVC Main10 additionally has a 24-frame HDR10 gate
  proving byte-exact P010 output and preservation of BT.2020/PQ, mastering
  display, and content-light metadata. VP8 and the other unvalidated profiles
  are also not advertised, so
  applications fall back instead of receiving an unsafe format or decode path.
- Packaging/build hygiene: `DESTDIR`/`PREFIX`/multiarch-aware Makefile,
  no `sudo` in `make install`, `make check` validation gate.

---

## What it does

The Rockchip RK3588 SoC includes a dedicated VPU capable of decoding H.264,
HEVC, VP9 and AV1 at up to 8K resolution. However, no vendor-supplied VA-API
driver exists for it. This project fills that gap by implementing the complete
`VADriverVTable` (VA-API 1.20) and forwarding decode work to the Rockchip MPP
library, which in turn uses the hardware VPU.

Key features:

- H.264 and VP9 hardware decode with byte-exact regression checking
- DRM PRIME 2 surface export directly from retained MPP external-pool DMA-BUFs
- GStreamer `va` app gate with byte-exact H.264, HEVC Main10, and VP9
  Profile 0/2 system-memory output
- Experimental H.264 High encode through `h264_vaapi` and GStreamer
  `vah264enc`, with CQP/CBR/VBR round-trip PSNR gates
- Compatible with Firefox 128+ (VA-API PDM path, RDD process)
- Implements the full VA-API 1.20 vtable (`__vaDriverInit_1_20`)

## Supported hardware

| SoC | Board (tested) |
|-----|---------------|
| RK3588 | Orange Pi 5 Plus; ROCK 5B (tested) |
| RK3588S | Orange Pi 5 (untested) |
| RK3576 | Likely compatible (untested) |

## Supported codecs

| Codec | Profile | Validated | Notes |
|-------|---------|-----------|-------|
| H.264 | Main, High | full normal + ASan/UBSan gates bit-exact | scaling-list reconstruction included |
| H.264 | Constrained Baseline | not offered | pinned SVA vector is corrupt in MPP; software fallback |
| VP9 | Profile 0 | full normal + ASan/UBSan gates bit-exact | hidden-reference vector included on audited kernel |
| VP9 | Profile 2 (under development) | not offered | generated 48-frame and official libvpx 10-frame gates are P010 bit-exact through MPP AFBC + RGA |
| HEVC | Main (under development) | not offered | gated hardware path has 7/8 pinned Main vectors bit-exact; MPP rejects the remaining TILES vector |
| HEVC | Main10 (under development) | not offered | generated, pinned weighted-prediction, and HDR10 gates are P010 bit-exact through MPP AFBC + RGA; BT.2020/PQ and static HDR metadata survive hardware decode |
| VP8 | — | not offered | crashes in the generic path; needs debugging |
| AV1 | — | not offered | VA-API hands headerless tile data; MPP needs full OBUs; see the [support plan](docs/AV1_SUPPORT_PLAN.md) and non-submitting platform probe |
| H.264 | High10 | not offered | profile-specific reconstruction and validation pending |

Applications fall back to their software decoders for the codecs that are
not offered.

Experimental encode is hidden by default. Setting
`RK_VAAPI_EXPERIMENTAL_ENCODE=h264`, `hevc`, or `h264,hevc` exposes
frame-level `VAEntrypointEncSlice` for H.264 Main/High and HEVC Main. NV12 is
the native MPP input; checked I420 and YV12 image uploads are converted into
that storage. Both paths cover one complete frame slice, MPP-generated headers,
CQP/CBR/VBR, FFmpeg interoperability, direct-I420 GStreamer
`vah264enc`/`vah265enc`, ASan/UBSan, and concurrent encode/decode. Imported
linear NV12 DMA-BUFs can be submitted directly, and single-object linear
RGBA/RGBX/BGRA/BGRX DMA-BUFs are converted through RGA into aligned NV12 before
encode. Linear P010 DMA-BUF import and byte-exact image readback are supported
as a surface contract, but P010 encode remains unadvertised because the
RK3588 MPP `vepu5xx` encoder HAL rejects its compact 10-bit input format; see
[`docs/HEVC_MAIN10_ENCODE_BACKEND.md`](docs/HEVC_MAIN10_ENCODE_BACKEND.md).
The native two-peer WebRTC harness now covers SDP offer/answer, trickle ICE,
DTLS-SRTP state, and H.264 media transfer; its software transport control
passes, while the combined `vah264enc` normal and sanitizer qualification
remains open. The separate hardware H.264 RTP pay/depay gate passes with
1,200-byte MTU enforcement. Multi-slice, tiled or multi-object imports, and
long encode soak also remain open. Paced dual-codec encode smoke runs pass
with flat post-warmup RSS/fd counts; the two-hour qualification run remains
open.

## Dependencies

Runtime:
- `libva2` (>= 2.0)
- `librockchip-mpp1`
- `librga2`

Build:
- `libva-dev`
- `librockchip-mpp-dev`
- `librga-dev`
- `pkg-config`, `gcc`

Native WebRTC peer gate:
- `python3-gi`
- `gir1.2-gst-plugins-bad-1.0`
- `gstreamer1.0-nice`

## Quick start

```bash
# Build and install
make
sudo make install

# Select the driver for one process
LIBVA_DRIVER_NAME=rockchip vainfo

# GStreamer's va plugin also needs its non-Intel vendor override
LIBVA_DRIVER_NAME=rockchip GST_VA_ALL_DRIVERS=1 gst-inspect-1.0 vah264dec
```

The Debian build produces a driver package and a separate, optional
`rockchip-vaapi-config` package. The config package selects the driver and sets
`GST_VA_ALL_DRIVERS=1` system-wide; it does not change browser sandbox or
display-backend settings.

Firefox additionally needs a distribution sandbox policy that permits the RDD
process to use Rockchip MPP, RGA, and dma-heap devices. A version-pinned
Firefox 152.0.6 source patch and validator are provided in
[`contrib/firefox`](contrib/firefox/README.md). Disabling the RDD sandbox is
only appropriate as a short, per-process diagnostic because it broadens the
attack surface for untrusted media. With a suitable policy, enable:

| Preference | Value |
|-----------|-------|
| `media.hardware-video-decoding.enabled` | `true` |
| `media.ffmpeg.vaapi.enabled` | `true` |
| `media.rdd-ffmpeg.enabled` | `true` |

## Verifying hardware decode

After starting Firefox and playing a video, check the driver log:

```bash
mkdir -p "$HOME/.local/state"
RK_VAAPI_LOG="$HOME/.local/state/rockchip-vaapi.log" \
LIBVA_DRIVER_NAME=rockchip firefox
tail -f "$HOME/.local/state/rockchip-vaapi.log"
```

Each default text record includes realtime nanoseconds, process/thread IDs,
severity, source file, line, and function while preserving the human-readable
message. `RK_VAAPI_LOG_LEVEL` accepts `error`, `warning`, `info` (default),
`debug`, or `trace`. Set `RK_VAAPI_LOG_FORMAT=json` for newline-delimited JSON:

```bash
RK_VAAPI_LOG="$HOME/.local/state/rockchip-vaapi.jsonl" \
RK_VAAPI_LOG_LEVEL=debug RK_VAAPI_LOG_FORMAT=json \
LIBVA_DRIVER_NAME=rockchip firefox
```

You can also check VPU activity:

```bash
cat /sys/class/devfreq/*/cur_freq   # VPU frequency rises under load
```

For sandbox diagnosis only, `MOZ_DISABLE_RDD_SANDBOX=1` can establish whether
policy is the blocker. Do not place it in `/etc/environment`,
`/etc/profile.d`, or a permanent browser launcher.

## Development

See [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) for architecture and VA-API/MPP
internals, [docs/TESTING.md](docs/TESTING.md) for the reproducible test gates,
and [docs/ROADMAP.md](docs/ROADMAP.md) for the production target design and
phased plan (decode core → HEVC + 10-bit → hardening → encode).

## AI-assisted development

This driver was designed and implemented with the assistance of
**Claude Sonnet 4.6** (model ID: `claude-sonnet-4-6`), an AI model developed
by Anthropic. Total interactive development time: approximately **3–4 hours**
across two sessions (24 April 2026).

The AI assisted with: architecture design, VA-API vtable implementation,
H.264 Annex B SPS/PPS reconstruction via Exp-Golomb encoding, MPP API
integration, DMABUF/DRM PRIME 2 surface export, and iterative debugging of
Firefox integration issues.

All code was reviewed, tested, and validated on real hardware by
Eduardo García-Mádico Portabella — EGP Sistemas.

Fork development (July 2026) continued the AI-assisted approach with
**Claude Fable 5** (`claude-fable-5`, Anthropic): full-source review, the
correctness fixes and validation gate described in *Fork status*, all
hardware-validated on a ROCK 5B.
