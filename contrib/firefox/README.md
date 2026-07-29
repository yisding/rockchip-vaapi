# Firefox RDD sandbox and Panfrost P010 patches

Firefox runs Linux VA-API decode in the sandboxed RDD process. Its RDD broker
permits `/dev/dri`, but not the Rockchip MPP, RGA, or dma-heap paths. Its
seccomp policy permits DRM and DMA-BUF ioctl families, but not the MPP request
or the RGA requests used by this driver.

`patches/` holds two patches per pinned Firefox release; the current milestone
is 153.0 and 152.0.6 is kept for older trees. Each pair is intended for an
arm64 Firefox source package. The RDD patch keeps the sandbox enabled. Broker
permissions are added only when the corresponding device node or directory
exists, and seccomp permits only the requests observed on the validated ROCK
5B stack:

| Device | Request | Purpose |
|---|---:|---|
| `/dev/mpp_service` | `0x40047601` | MPP v1 command transport |
| `/dev/rga` | `0x801c7201` | RGA driver version |
| `/dev/rga` | `0x80907202` | RGA hardware version |
| `/dev/rga` | `0x5017` | legacy synchronous RGA blit |

Firefox already permits the DMA-BUF `'b'` ioctl family. On arm64, its existing
Tegra policy also permits the `'H'` family used by dma-heap allocation, but the
broker still needs access to `/dev/dma_heap`.

The second patch closes an independent P010 presentation failure. Firefox
already retries GR/RG chroma formats when EGL modifier enumeration reports the
first format as unsupported. Panfrost advertises linear `GR1616` but rejects
the actual chroma-plane `eglCreateImage` call with `EGL_BAD_MATCH`; Firefox
then abandons HEVC Main10 VA-API after the first frame. The patch preserves the
standards-correct `GR1616` first attempt and retries Firefox's existing swapped
format once only after real image creation fails.

## Version contract

Each patch pair is pinned to an upstream Firefox release tag by the SHA-256 of
all three preimage files. A tree that does not match is rejected instead of
being force-patched.

`FIREFOX_153_0_RELEASE`:

```text
b1dae2499ba9589cc41454cf7f73c332c82ed9d6c13710c0448fdc9c7507e1e9  security/sandbox/linux/SandboxFilter.cpp
3eefffdd817ddebea6d029e5403a1f1d9536c7b49ef86e5c553e1ab77e6bddcb  security/sandbox/linux/broker/SandboxBrokerPolicyFactory.cpp
2c44aa0a1597ec57cd597055d78b5aaecb645b3af33169fb6439fb37b04434df  widget/gtk/DMABufSurface.cpp
```

`FIREFOX_152_0_6_RELEASE`:

```text
7a9c7b4e56b5ed0401998f42242bd576bff5461e85df271d42f73844a2bf9f47  security/sandbox/linux/SandboxFilter.cpp
0bc000706b11d7dcf54c71f67bd1cb32d2214e939fbb67634e0bd0036b805af0  security/sandbox/linux/broker/SandboxBrokerPolicyFactory.cpp
e4ea08b2da7c1e21df520620f873eb1f3db180d71dee26872e28eb7456ad8777  widget/gtk/DMABufSurface.cpp
```

Validate an unpacked source tree before adding the patch to the distribution
package. `FIREFOX_VERSION` selects the milestone and defaults to 153.0:

```sh
tests/check-firefox-rdd-patch.sh /path/to/firefox-153.0
FIREFOX_VERSION=152.0.6 tests/check-firefox-rdd-patch.sh /path/to/firefox-152.0.6
```

For a Debian-format Firefox source package, copy both version-matched patches
into `debian/patches/`, append their filenames to `debian/patches/series`, add
a local version suffix, and rebuild the binary package. Do not install these
patches from the `rockchip-vaapi` binary package: changing another package's
source or binary files would be unowned and would be lost on Firefox upgrades.

## Revalidation

Rebase and remeasure this patch for every Firefox, MPP, or librga update.
Reject a source tree whose hashes differ instead of forcing the patch. Validate
the rebuilt browser in a real Wayland or X11 session and confirm:

1. `MOZ_DISABLE_RDD_SANDBOX` is unset.
2. `about:support` reports hardware video decoding.
3. HEVC Main10 remains on VA-API after the first frame and the DMA-BUF log
   records the one-shot swapped-chroma retry after Panfrost's `EGL_BAD_MATCH`.
4. the driver log shows MPP decode and P010 surface export.
5. the RDD process remains sandboxed.

The ioctl list above was measured on 2026-07-26 with Firefox 152.0.6,
librockchip-mpp 1.5.0, librga 2.2.0, and the audited RK3588 kernel. H.264
encode and HEVC Main10 decode/RGA gates used the same request set.

The 153.0 patch is a rebase of that measurement, not a new one. It was verified
to apply cleanly to `FIREFOX_153_0_RELEASE` and to produce byte-identical
sources to applying the 152.0.6 patch, and 153.0 was confirmed not to permit
any of these paths or requests already. The request set itself is inherited
from the 152.0.6 measurement and has not been remeasured against a patched
153.0 build, because that needs a Firefox source build. `make
check-firefox-decode` exercises the stock decode path with the sandbox
disabled by default. `FIREFOX_RDD_SANDBOX=enabled` removes that bypass and
requires the live RDD process to report Linux seccomp filter mode 2. Stock
Firefox 153.0 proves the H.264/HEVC Main paths but falls back after the first
Main10 frame at the Panfrost `GR1616` import boundary; completing both the
sandbox and P010 claims requires the rebuilt package in that enabled mode.
