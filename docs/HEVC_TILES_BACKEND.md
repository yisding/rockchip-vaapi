# HEVC TILES direct-MPP backend reproducer

This note records the evidence and repeatable reduction procedure for the sole
non-green HEVC Main conformance vector, `TILES_A_Cisco_2.bit`. The reproducer
intentionally links only to `librockchip_mpp`: it does not initialize libva or
load `rockchip_drv_video.so`.

## Current status

The full pinned vector is 1920x1080 HEVC Main with 100 packets / 100
software-decoded frames:

```
size:   484767 bytes
sha256: eff78a401ecccc21d995345988f1be60ee76604cf10fa39d421c3e00668a94d6
```

Historical runs on the audited fixed kernel reported `errinfo=1` from the first
frame in both direct MPP decode and the VA reconstruction path. An old direct
sample invocation nevertheless wrote exactly one linear 1920x1080 NV12 frame
(3,110,400 bytes); because that output was not accompanied by a self-describing
command/result log, it remains supporting evidence rather than the acceptance
gate for a minimized reproducer.

The new control-first reducer could not perform hardware minimization on
2026-07-27 because the booted
`6.18.40-video-rewrite-kasan-rockchip64 #6` kernel was already known to have
MPP device issues. With `librockchip-mpp1`
`1.5.0+git20260529.1375813c+ds-0ubuntu2~rk1`, MPP reported clients 1, 3, 4,
12, 13, 18, and 19 as not ready. The checksum-pinned, known-good
`PPS_A_qualcomm_7.bit` control then produced `errinfo=1` on all 81 frames;
the EOS frame was also marked discarded. Buffer attaches failed with
`ENODEV`, and `MPP_IOC_CFG_V1` polls failed with `EIO`. The kernel notes hash
for that run was
`52a54705a400bc125be7a95fbdf140fe7b5cd6d6989c47010fce32a17e103f93`.
The reducer correctly stopped with `BLOCK environment` before testing TILES.
No stream-specific conclusion should be drawn from that boot.

## Host-side reduction facts

FFmpeg stream-copy prefixes remain valid, independently software-decodable
Annex-B streams:

| Prefix | Bytes | SHA-256 | NAL inventory |
|---|---:|---|---|
| packet 1 | 73,227 | `af9b8b0295ba1c96911823fec6289418dc469236062507c5195cda19b7e7ad4b` | VPS, SPS, PPS, IDR_W_RADL |
| packets 1–2 | 75,498 | `172e302ff89afc8c802b80a0db70a876d68ef584ee2e83d017a032ea0f65e870` | VPS, SPS, PPS, IDR_W_RADL, PPS, TRAIL_R |
| normalized six-NAL core | 75,496 | `54a4defe45864fb743c209d7bd30a36c0b9c0d5bc4bf959e800bdd4820367802` | same six NAL units |

Packet 1 contains a 73,138-byte IDR NAL. Packet 2 is only 2,271 bytes: a
15-byte replacement PPS plus a 2,248-byte P-picture NAL. Both prefixes decode
to exactly one and two frames respectively with FFmpeg's software HEVC
decoder.

Running FFmpeg's `filter_units` removal for AUD, EOS, EOB, filler, and
prefix/suffix SEI leaves the same six semantic NAL units and normalizes two
bytes of Annex-B padding. The resulting 75,496-byte core remains a valid
two-frame software stream. It becomes the preferred handoff only if the
fixed-kernel direct runner confirms that it preserves the same backend failure.

The initial PPS enables a non-uniform 5×5 tile grid:

```
column_width_minus1 = [3, 9, 4, 5]
row_height_minus1   = [7, 2, 1, 2]
loop_filter_across_tiles_enabled_flag = 1
```

Before packet 2's P-picture, the stream replaces PPS ID 0 with another
non-uniform 5×5 layout:

```
column_width_minus1 = [10, 4, 5, 3]
row_height_minus1   = [6, 2, 0, 0]
loop_filter_across_tiles_enabled_flag = 0
```

This makes the two-packet stream the first candidate to test on a healthy
backend: it isolates the first dynamic tile-layout/PPS transition while
retaining the reference IDR required by the P-picture. It is not yet called
the confirmed minimum because the new direct-MPP runner has not executed on a
healthy kernel.

Changing tile dimensions in the PPS without re-encoding the slice payload is
not a valid semantic reduction: tile boundaries alter CABAC entry points and
coding-tree-block traversal. Once the exact two-packet failure is confirmed,
further semantic discrimination requires purpose-encoded streams that vary one
of grid dimensions, uniform spacing, PPS replacement, or cross-tile filtering
at a time.

## Tools

Build the direct backend runner:

```bash
make tests/hevc_mpp_repro
```

Inspect an Annex-B stream without opening MPP devices:

```bash
tests/hevc_mpp_repro --inspect INPUT.h265
```

Run one direct decode when the expected output count is known:

```bash
tests/hevc_mpp_repro INPUT.h265 EXPECTED_FRAMES
```

Its exit statuses are deliberately machine-readable:

| Status | Meaning |
|---:|---|
| 0 | clean decode, EOS observed, exact frame count, no error flags |
| 1 | bad/missing output; stream-specific only after a clean control |
| 2 | invalid input or invocation |
| 3 | MPP/device setup unavailable |
| 4 | MPP runtime/transport API failure |

Run the reducer after fetching the pinned vectors:

```bash
make fetch-vectors
make tests/hevc_mpp_repro
tests/minimize-hevc-tiles.sh \
  tests/vectors/TILES_A_Cisco_2.bit \
  .hevc-probe/tiles-backend-fixed-kernel
```

`make check-hevc-tiles-backend` runs the same reducer with a generated work
directory. The explicit output directory is preferable for a handoff because
it retains:

- the known-good control log;
- an environment manifest retained even when the control blocks the run;
- the full-vector direct-MPP log;
- each software-valid prefix and its direct-MPP log;
- a version with AUD/EOS/EOB/filler/SEI NAL classes removed when that preserves
  the failure;
- a NAL inventory, `trace_headers` output, extracted tile layouts, checksums,
  and a summary report.

The reducer accepts a TILES failure only after the checksum-pinned
`PPS_A_qualcomm_7.bit` control decodes cleanly in the same invocation. A broken
kernel therefore blocks reduction rather than generating a false reproducer.

## Fixed-kernel acceptance sequence

1. Confirm the control returns status 0.
2. Confirm the complete TILES vector returns status 1, not status 3 or 4.
3. Let the reducer test prefixes in order. Based on prior output, packet 1 is
   expected to pass and packets 1–2 are expected to reproduce.
4. Confirm the stripped six-NAL core remains software-valid and returns status
   1 through direct MPP.
5. Preserve the generated report and logs with the kernel build identity and
   MPP package/version.
6. Use the two-PPS/two-frame stream as the MPP/HAL/kernel handoff input before
   generating semantic tile-layout variants.
