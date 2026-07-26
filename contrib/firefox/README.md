# Firefox RDD sandbox policy

Firefox runs Linux VA-API decode in the sandboxed RDD process. The stock
Firefox 152.0.6 RDD broker permits `/dev/dri`, but it does not permit the
Rockchip MPP, RGA, or dma-heap paths. Its seccomp policy permits DRM and
DMA-BUF ioctl families, but not the MPP request or the RGA requests used by
this driver.

The patch in `patches/firefox-152.0.6-rdd-rockchip-vaapi.patch` is intended for
an arm64 Firefox source package. It keeps the RDD sandbox enabled. Broker
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

## Version contract

The patch is pinned to upstream Firefox `FIREFOX_152_0_6_RELEASE`. The two
preimage files must have these SHA-256 values:

```text
7a9c7b4e56b5ed0401998f42242bd576bff5461e85df271d42f73844a2bf9f47  security/sandbox/linux/SandboxFilter.cpp
0bc000706b11d7dcf54c71f67bd1cb32d2214e939fbb67634e0bd0036b805af0  security/sandbox/linux/broker/SandboxBrokerPolicyFactory.cpp
```

Validate an unpacked source tree before adding the patch to the distribution
package:

```sh
tests/check-firefox-rdd-patch.sh /path/to/firefox-152.0.6
```

For a Debian-format Firefox source package, copy the patch into
`debian/patches/`, append its filename to `debian/patches/series`, add a local
version suffix, and rebuild the binary package. Do not install this patch from
the `rockchip-vaapi` binary package: changing another package's source or
binary files would be unowned and would be lost on Firefox upgrades.

## Revalidation

Rebase and remeasure this patch for every Firefox, MPP, or librga update.
Reject a source tree whose hashes differ instead of forcing the patch. Validate
the rebuilt browser in a real Wayland or X11 session and confirm:

1. `MOZ_DISABLE_RDD_SANDBOX` is unset.
2. `about:support` reports hardware video decoding.
3. the driver log shows MPP decode and surface export.
4. the RDD process remains sandboxed.

The ioctl list above was measured on 2026-07-26 with Firefox 152.0.6,
librockchip-mpp 1.5.0, librga 2.2.0, and the audited RK3588 kernel. H.264
encode and HEVC Main10 decode/RGA gates used the same request set. Browser
playback remains a separate display-session validation gate.
