# HEVC Main10 encode backend boundary

## Status

HEVC Main10 encode is not advertised. Linear P010 is a supported VA surface
and DRM PRIME 2 import/readback contract, but it is not a supported rkvenc2
encoder input on the tested RK3588 MPP stack.

The distinction matters:

- P010 is the public linear 16-bit-container format used by VA clients.
- `MPP_FMT_YUV420SP_10BIT` is Rockchip's compact 10-bit layout, not P010.
- RGA can convert P010 to that compact layout.
- The RK3588 MPP `vepu5xx` encoder HAL rejects the compact format before a
  frame can be encoded.

The driver therefore continues to expose only progressive 8-bit HEVC Main
encode behind `RK_VAAPI_EXPERIMENTAL_ENCODE=hevc`. There is no
`hevc-main10` encode opt-in.

## Reproduction

Run the bounded, libva-free backend probe on the board:

```sh
make probe-mpp-main10-encode
```

The probe asks the installed `mpi_enc_test` to configure one HEVC frame with
format id 1 (`MPP_FMT_YUV420SP_10BIT`), a 320-pixel / 400-byte compact stride,
and a 240-row vertical stride. On the tested stack it reports:

```text
mpp_enc: set prep cfg w:h [320:240] stride [400:240] fmt 1
vepu5xx_common: vepu5xx_set_fmt unsupport frame format 1
blocked: MPP vepu5xx rejects MPP_FMT_YUV420SP_10BIT
```

The measured package was
`librockchip-mpp1 1.5.0+git20260529.1375813c+ds-0ubuntu2~rk1`.
The probe treats the exact rejection as its expected diagnostic. If that
message disappears, it fails as inconclusive so a real Main10 encode,
bitstream-parse, decode, and P010 quality gate must be run before capabilities
change.

An earlier driver experiment reached the same boundary after an RGA
P010-to-compact conversion. FFmpeg selected `VAProfileHEVCMain10`, RGA
completed the conversion, MPP logged the rejection above, and no output packet
was produced. That experimental capability and submission path were removed
rather than leaving an opt-in that could time out during teardown.

## Upstream evidence

Rockchip MPP defines `MPP_FMT_YUV420SP_10BIT` as a Rockchip-specific compact
format without P010's per-pixel gap:

- <https://github.com/rockchip-linux/mpp/blob/develop/inc/mpp_frame.h>

In the encoder HAL's `vepu5xx_yuv_cfg`, the entry for that format maps to
`VEPU5xx_FMT_BUTT`. `vepu5xx_set_fmt()` returns `MPP_NOK` and logs the
unsupported-format error for such entries:

- <https://github.com/rockchip-linux/mpp/blob/develop/mpp/hal/rkenc/common/vepu5xx_common.c>

Main10 encode can move from backend-blocked to driver work only after the
target MPP/HAL accepts an RK3588 10-bit input layout. At that point the driver
still needs a fail-fast P010 conversion/submission path plus FFmpeg,
GStreamer, parser, decode-quality, sanitizer, and long-soak qualification.
