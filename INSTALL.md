# Installation guide — rockchip-vaapi

## System requirements

### Contiguous memory (CMA) — required for 4K

The RK3588 VPU decodes into **physically contiguous** memory (CMA —
Contiguous Memory Allocator), and the Mali GPU also draws compositing buffers
from the same CMA pool. A single 4K NV12 frame is ~12.5 MB and the VP9 decoder
keeps ~10 reference frames in flight (~125–200 MB), so 4K decode **plus** 4K GPU
compositing together need well over the default `cma=256M`.

With only 256 MB CMA, 4K playback in Firefox decodes correctly for a few
seconds and then dies with `NS_ERROR_DOM_MEDIA_FATAL_ERR` and falls back to
software — the VPU can no longer allocate a contiguous buffer once the GPU has
taken its share. (1080p frames are 4× smaller and never hit the limit.)

**Set `cma=512M`** (or higher) on the kernel command line and reboot:

- Armbian / mainline u-boot: edit `/boot/armbianEnv.txt`, add or change
  `extraargs=cma=512M` (append to any existing `extraargs`).
- extlinux: edit the `append` line in `/boot/extlinux/extlinux.conf`, add `cma=512M`.

Verify after reboot:

```bash
cat /proc/cmdline | tr ' ' '\n' | grep cma     # → cma=512M
grep CmaTotal /proc/meminfo                     # → CmaTotal: 524288 kB
```

> The driver's own per-surface buffers come from the *system* dma-heap (not CMA),
> so they are not the constraint; the CMA pressure is MPP's decode DPB plus the
> GPU compositor. See `docs/DEVELOPMENT.md` for details.

## Option A: Install from Debian package (recommended)

```bash
sudo apt install ./rockchip-vaapi_1.0.11+ysp3_arm64.deb
```

This installs `rockchip_drv_video.so` to
`/usr/lib/aarch64-linux-gnu/dri/` and registers the package with `dpkg`.
Install `rockchip-vaapi-config_1.0.11+ysp3_all.deb` as well to select this
driver and enable GStreamer's non-Intel vendor probe for all login sessions.
The config package does not weaken browser sandboxes. Purge both packages to
remove the config package's system-wide conffiles as well:

```bash
sudo apt purge rockchip-vaapi-config rockchip-vaapi
```

## Option B: Build and install from source

### 1. Install build dependencies

```bash
sudo apt install gcc pkg-config libva-dev librockchip-mpp-dev librga-dev
```

### 2. Build

```bash
cd /path/to/rockchip-vaapi
make
```

Expected output: `rockchip_drv_video.so` in the project root.

### 3. Install

```bash
sudo make install
# installs to /usr/lib/aarch64-linux-gnu/dri/
```

To install to a custom path:

```bash
sudo install -m 755 rockchip_drv_video.so /your/path/rockchip_drv_video.so
```

## Option C: Build your own Debian package

```bash
# Create orig tarball (exclude build artifacts and debian/)
tar --exclude='rockchip-vaapi/debian' \
    --exclude='rockchip-vaapi/*.so' \
    --exclude='rockchip-vaapi/src/*.o' \
    -czf rockchip-vaapi_1.0.11.orig.tar.gz rockchip-vaapi/

# Build binary package
cd rockchip-vaapi
dpkg-buildpackage -us -uc -b

# Build source package
dpkg-buildpackage -us -uc -S

# Build, lint, and exercise clean install/upgrade/purge in an isolated root
make check-package-install
```

## Verifying the installation

```bash
# Check the driver file is in place
ls -l /usr/lib/aarch64-linux-gnu/dri/rockchip_drv_video.so

# Verify VA-API sees the driver
LIBVA_DRIVER_NAME=rockchip \
LIBVA_DRIVERS_PATH=/usr/lib/aarch64-linux-gnu/dri \
vainfo
```

Expected `vainfo` output:

```
libva info: VA-API version 1.20.0
libva info: User environment variable requested driver 'rockchip'
libva info: Trying to open /usr/lib/aarch64-linux-gnu/dri/rockchip_drv_video.so
libva info: Found init function __vaDriverInit_1_20
...
VA profile VAProfileH264Main               : VAEntrypointVLD
VA profile VAProfileH264High               : VAEntrypointVLD
VA profile VAProfileVP9Profile0             : VAEntrypointVLD
...
```

HEVC and 10-bit profiles are intentionally hidden in normal operation while
their remaining conformance and display gates are open.

## Configuring Firefox

### Environment variables

| Variable | Value | Reason |
|----------|-------|--------|
| `LIBVA_DRIVER_NAME` | `rockchip` | Selects this driver |
| `LIBVA_DRIVERS_PATH` | `/usr/lib/aarch64-linux-gnu/dri` | Driver search path |

`rockchip-vaapi-config` installs those variables system-wide and also sets
`GST_VA_ALL_DRIVERS=1`. Otherwise, set them only for the application being
tested.

Firefox's RDD sandbox must permit the MPP and dma-heap device operations used
by the driver. A distribution Firefox build needs an appropriate sandbox
policy; this repository does not install one. `MOZ_DISABLE_RDD_SANDBOX=1`
can be used for a short per-process diagnosis, but must not be configured
globally or in a permanent launcher.

### about:config (required)

Open `about:config` in Firefox and set:

| Key | Value |
|-----|-------|
| `media.hardware-video-decoding.enabled` | `true` |
| `media.ffmpeg.vaapi.enabled` | `true` |
| `media.rdd-ffmpeg.enabled` | `true` |

## Troubleshooting

**`vainfo` reports "driver not found"**
Verify `LIBVA_DRIVER_NAME=rockchip` and that the `.so` exists at
`/usr/lib/aarch64-linux-gnu/dri/rockchip_drv_video.so`.

**Firefox still uses SWDEC**
Check `about:support` → Media → Video Decoder. If it shows `FFmpegVideo`,
hardware decode is active. If it shows `Softpipe` or similar, verify the three
`about:config` keys, the driver environment, and the distribution's RDD
sandbox policy. Use a one-off run with `MOZ_DISABLE_RDD_SANDBOX=1` only to
isolate a sandbox-policy failure.

**`/dev/dri` permission denied in RDD process**
Add your user to the `video` and `render` groups:
```bash
sudo usermod -aG video,render $USER
```
Then log out and back in (or use `newgrp video`).

**No frames decoded / black screen**
Enable verbose logging by setting `RK_VAAPI_LOG` to a file path:
```bash
mkdir -p "$HOME/.local/state"
LIBVA_DRIVER_NAME=rockchip \
RK_VAAPI_LOG="$HOME/.local/state/rockchip-vaapi.log" firefox
tail -f "$HOME/.local/state/rockchip-vaapi.log" |
    grep -E "assigned|TIMEOUT|ERROR|failed"
```
Look for errors after `BeginPicture` or `EndPicture`. Missing SPS/PPS or
MPP decode errors will appear there. Without `RK_VAAPI_LOG`, the driver
produces no output (logging is disabled by default for performance).

**4K plays for a few seconds then falls back to software
(`NS_ERROR_DOM_MEDIA_FATAL_ERR`)**
This is CMA exhaustion. The driver log will show frames decoded normally
(`copied=1`, no errors) for ~75 frames then a fatal error with no driver-side
cause — MPP cannot report a failed contiguous allocation through libva.
Increase CMA to `cma=512M` (see *System requirements* above).

Note: on some RK3588 boards the DTB hardcodes the CMA region with a fixed
physical address (`reg` property), which overrides any `cma=` command-line
parameter. If `grep CmaTotal /proc/meminfo` still shows 262144 kB after adding
`cma=512M`, patch the DTB directly:

```bash
sudo cp /boot/dtb/rockchip/rk3588-orangepi-5-plus.dtb \
        /boot/dtb/rockchip/rk3588-orangepi-5-plus.dtb.bak
sudo fdtput -t x /boot/dtb/rockchip/rk3588-orangepi-5-plus.dtb \
        /reserved-memory/cma reg 0x00 0x10000000 0x00 0x20000000
```

Then reboot and verify `grep CmaTotal /proc/meminfo` → `524288 kB`.
