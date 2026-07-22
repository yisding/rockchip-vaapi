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
`framemd5` bit-exactness harness (`tests/validate.sh`). The full conformance
gate is currently blocked by the quarantined VP9 `show_existing_frame`
kernel/MPP path; see [docs/TESTING.md](docs/TESTING.md):

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
  rejected on some runs, 5/10 runs corrupt). Puts now drain output frames
  and retry, bounded. The synthetic VP9 regression is 10/10 bit-exact.
- **VP9 output routing by FIFO instead of PTS**, since a
  `show_existing_frame` repeat of a hidden altref can surface with the
  altref packet's PTS and desync every later frame.
- **Hardened decoded-surface copies.** MPP can return a much wider VP9 stride
  than the coded width. Surface allocation now reserves MPP's aligned layout,
  every NV12 copy is bounds-checked, and an unexpected layout becomes a VA
  decoding error instead of a DMA-BUF overflow and userspace segfault.
- **Pinned real conformance vectors and CI plumbing.** The gate now uses ITU-T
  H.264 and official libvpx VP9 vectors with payload checksums, and normal plus
  sanitized AArch64 builds are cross-compiled in CI.
- **Honest capability advertising.** HEVC (no header reconstruction —
  verified non-functional), VP8 (verified segfault), and the 10-bit
  profiles (MPP emits compact NV15, driver exported it as P010 — layout
  mismatch) are no longer advertised, so applications fall back to software
  instead of breaking. They can return as their decode paths get built.
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
- DRM PRIME 2 surface export (NV12 DMA-BUF; the current decode path copies
  from MPP into a permanent per-surface export buffer)
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
| H.264 | Main, High | safe conformance vectors bit-exact; full gate pending | scaling-list reconstruction included |
| H.264 | Constrained Baseline | not offered | pinned SVA vector is corrupt in MPP; software fallback |
| VP9 | Profile 0 | safe vectors under validation; full gate blocked | `show_existing_frame` quarantined until kernel + MPP fixes deploy |
| HEVC | — | not offered | needs VPS/SPS/PPS reconstruction |
| VP8 | — | not offered | crashes in the generic path; needs debugging |
| AV1 | — | not offered | VA-API hands headerless tile data; MPP needs full OBUs |
| 10-bit (High10, VP9 P2) | — | not offered | MPP outputs compact NV15; P010 export path pending |

Applications fall back to their software decoders for the codecs that are
not offered.

## Dependencies

Runtime:
- `libva2` (>= 2.0)
- `librockchip-mpp1`

Build:
- `libva-dev`
- `librockchip-mpp-dev`
- `pkg-config`, `gcc`

## Quick start

```bash
# Build and install
make
sudo make install

# Launch Firefox with hardware decode
LIBVA_DRIVER_NAME=rockchip \
LIBVA_DRIVERS_PATH=/usr/lib/aarch64-linux-gnu/dri \
MOZ_DISABLE_RDD_SANDBOX=1 \
firefox
```

In Firefox, also enable via `about:config`:

| Preference | Value |
|-----------|-------|
| `media.hardware-video-decoding.enabled` | `true` |
| `media.ffmpeg.vaapi.enabled` | `true` |
| `media.rdd-ffmpeg.enabled` | `true` |

## Verifying hardware decode

After starting Firefox and playing a video, check the driver log:

```bash
# Should show mpp_create OK, BeginPicture, EndPicture, ExportSurfaceHandle
LIBVA_DRIVER_NAME=rockchip MOZ_DISABLE_RDD_SANDBOX=1 firefox 2>&1 | grep rk-vaapi
```

You can also check VPU activity:

```bash
cat /sys/class/devfreq/*/cur_freq   # VPU frequency rises under load
```

## Permanent Firefox launcher

Create `/usr/local/bin/firefox-hw`:

```bash
#!/bin/sh
export LIBVA_DRIVER_NAME=rockchip
export LIBVA_DRIVERS_PATH=/usr/lib/aarch64-linux-gnu/dri
export MOZ_DISABLE_RDD_SANDBOX=1
exec /usr/bin/firefox "$@"
```

```bash
chmod +x /usr/local/bin/firefox-hw
```

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
