# Firefox RDD sandbox patches

Firefox runs Linux VA-API decode in the sandboxed RDD process. Its RDD broker
permits `/dev/dri`, but not the Rockchip MPP, RGA, or dma-heap paths. Its
seccomp policy permits DRM and DMA-BUF ioctl families, but not the MPP request
or the RGA requests used by this driver.

`patches/` holds one RDD patch per pinned Firefox source revision; the current
milestones are 153.0 and 153.0.1, whose two patched files are byte-identical,
and 152.0.6 is kept for older trees. Each patch is intended for an arm64
Firefox source package and keeps the sandbox enabled. Broker
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

The former P010 chroma-retry patches were based on an exporter misdiagnosis and
have been removed. The driver emitted `0x36315247` (`GR16`) in a DRM-format
field, while `DRM_FORMAT_GR1616` is `0x32335247` (`GR32`). Mesa correctly
reported the unknown fourcc as `EGL_BAD_MATCH`; the request never reached
Panfrost. The fix belongs in this driver, and stock Firefox should import the
correct split-P010 descriptor without a swapped-format retry.

## Version contract

Each patch is pinned to an upstream Firefox release tag by the SHA-256 of its
two preimage files. A tree that does not match is rejected instead of
being force-patched.

`FIREFOX_153_0_RELEASE` and `FIREFOX_153_0_1_RELEASE` (shared preimages):

```text
b1dae2499ba9589cc41454cf7f73c332c82ed9d6c13710c0448fdc9c7507e1e9  security/sandbox/linux/SandboxFilter.cpp
3eefffdd817ddebea6d029e5403a1f1d9536c7b49ef86e5c553e1ab77e6bddcb  security/sandbox/linux/broker/SandboxBrokerPolicyFactory.cpp
```

`FIREFOX_152_0_6_RELEASE`:

```text
7a9c7b4e56b5ed0401998f42242bd576bff5461e85df271d42f73844a2bf9f47  security/sandbox/linux/SandboxFilter.cpp
0bc000706b11d7dcf54c71f67bd1cb32d2214e939fbb67634e0bd0036b805af0  security/sandbox/linux/broker/SandboxBrokerPolicyFactory.cpp
```

Validate an unpacked source tree before adding the patch to the distribution
package. `FIREFOX_VERSION` selects the milestone and defaults to 153.0.1. The
153.0.1 selector intentionally reuses the 153.0 patch because both preimages
have the hashes above:

```sh
tests/check-firefox-rdd-patch.sh /path/to/firefox-153.0.1
FIREFOX_VERSION=153.0 tests/check-firefox-rdd-patch.sh /path/to/firefox-153.0
FIREFOX_VERSION=152.0.6 tests/check-firefox-rdd-patch.sh /path/to/firefox-152.0.6
```

For a Debian-format Firefox source package, copy the version-matched patch into
`debian/patches/`, append its filename to `debian/patches/series`, add a local
version suffix, and rebuild the binary package. Do not install this patch from
the `rockchip-vaapi` binary package: changing another package's
source or binary files would be unowned and would be lost on Firefox upgrades.
Mozilla builds are incremental: an interrupted build can be resumed from the
same configured source tree with `./mach build -j2`. Do not remove its object
directory between the build and the sandbox-enabled runtime gate.

## Revalidation

Rebase and remeasure this patch for every Firefox, MPP, or librga update.
Reject a source tree whose hashes differ instead of forcing the patch. Validate
the rebuilt browser in a real Wayland or X11 session and confirm:

1. `MOZ_DISABLE_RDD_SANDBOX` is unset.
2. `about:support` reports hardware video decoding.
3. HEVC Main10 remains on VA-API after the first frame and the DMA-BUF log
   records plane 1 as `format=0x32335247` with zero-copy texture creation.
4. the driver log shows MPP decode and P010 surface export.
5. the RDD process remains sandboxed.

The ioctl list above was measured on 2026-07-26 with Firefox 152.0.6,
librockchip-mpp 1.5.0, librga 2.2.0, and the audited RK3588 kernel. H.264
encode and HEVC Main10 decode/RGA gates used the same request set.

The 153.0 patch is a rebase of that measurement, not a new one. It was verified
to apply cleanly to `FIREFOX_153_0_RELEASE` and
`FIREFOX_153_0_1_RELEASE` and to produce byte-identical sources to applying the
152.0.6 patch. Neither 153.0 release permitted these paths or requests already.
The request set itself is inherited from the 152.0.6 measurement and has not
yet been remeasured against a patched 153.x build. `make
check-firefox-decode` exercises the stock decode path with the sandbox
disabled by default. `FIREFOX_RDD_SANDBOX=enabled` removes that bypass and
requires the live RDD process to report Linux seccomp filter mode 2. The prior
stock Firefox Main10 fallback was caused by the driver's invalid split-P010
fourcc. Rebuilding the driver and rerunning the sandbox-enabled display gate
remain required to validate the corrected end-to-end path.
