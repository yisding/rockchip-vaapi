# Testing and conformance

The release gate compares every decoded Y and UV byte from Rockchip VA-API
against FFmpeg's software decoder. H.264 and VP9 inverse transforms are
specified exactly, so a mismatch is a driver failure.

## Host checks

Run the unit tests, the complete sanitized build, and static analysis with:

```sh
make test
make sanitize
make test-tsan
make test-valgrind
make lint
shellcheck tests/check-concurrent-decode.sh tests/check-zero-copy.sh \
    tests/check-hevc-main10.sh tests/check-hevc-main10-hdr.sh \
    tests/check-soak.sh tests/fetch-vectors.sh tests/validate.sh
```

`make sanitize` builds the whole driver and runs its hardware-independent unit
tests under ASan and UBSan. LeakSanitizer is disabled for these runs because it
cannot operate reliably under the board's ptrace restrictions. The hardware
gate still loads the sanitized driver into FFmpeg and stops on the first ASan
or UBSan finding. `make test-valgrind` runs the same unit tests with full leak
checking and treats every reported leak kind or memory error as a failure. On
AArch64, install both `valgrind` and the matching `libc6-dbg`; Valgrind needs
the dynamic loader's symbols before it can start.

`make test-tsan` stress-tests concurrent object insertion, lookup, removal,
and refcounted destruction under ThreadSanitizer. The full two-decoder TSan
gate remains a Phase 1 exit requirement.

When changing driver-wide compile wiring, also build the instrumented driver
artifacts directly:

```sh
make tests/.tsan-driver/rockchip_drv_video.so
```

## Pinned conformance vectors

The manifest at `tests/conformance-vectors.tsv` pins both the downloaded file
and extracted payload SHA-256. It currently covers three ITU-T H.264 streams
(Constrained Baseline fallback, Main field-coded VA-API, and High VA-API with
scaling lists), eight FFmpeg FATE HEVC Main streams, one FATE HEVC Main10
weighted-prediction stream, one official WebM/libvpx VP9 Profile 2 10-bit
stream, and four official WebM/libvpx VP9 Profile 0 VA-API streams.
The HEVC cases exercise long-term references, PPS syntax, RPS, scaling lists,
tiles, VPS IDs, WPP, and weighted prediction; they remain
`software-fallback` by default until the Phase 2 hardware gates close. The
`decode_path` column makes hardware expectations explicit; `vaapi` cases force
hardware-frame output so an accidental software fallback cannot turn the gate
green.

`tests/hevc_test` independently round-trips the reconstructed Main/Main10
syntax, scaling-list scan order, current short/long-term references, tile
layout, and slice PPS IDs. It also exhaustively feeds every repeated-byte short
prefix through the slice parser and checks that incomplete or unrepresentable
VA state is rejected. For an additional parser outside the driver, emit either
header bundle and run FFmpeg's `trace_headers` bitstream filter:

```sh
tests/hevc_test --emit-headers > main.h265
tests/hevc_test --emit-main10-headers > main10.h265
ffmpeg -f hevc -i main10.h265 -c copy -bsf:v trace_headers -f null /dev/null
```

Header-only streams can make the null muxer report missing picture dimensions;
the validation evidence is successful VPS/SPS/PPS parsing with the expected
profile, bit depth, and scaling-list fields.

Fetch or verify them without using `/tmp`:

```sh
make fetch-vectors
```

Archives remain under the ignored `tests/vectors/.downloads/` cache. Vector
payloads are also ignored; only their URLs, member names, checksums, codec, and
risk classification are committed.

## ROCK 5B hardware gate

On a ROCK 5B with the vendor MPP stack and a VA-capable system FFmpeg:

```sh
make clean all test
make fetch-vectors
make check-driver-objects
make check-driver-objects-sanitize
make check-driver-objects-tsan
FFMPEG=/usr/bin/ffmpeg make check-zero-copy
FFMPEG=/usr/bin/ffmpeg make check-zero-copy-sanitize
FFMPEG=/usr/bin/ffmpeg make check-concurrent-decode
FFMPEG=/usr/bin/ffmpeg make check-concurrent-decode-sanitize
FFMPEG=/usr/bin/ffmpeg make check-concurrent-decode-tsan
FFMPEG=/usr/bin/ffmpeg make check-soak
FFMPEG=/usr/bin/ffmpeg make check-hevc-experimental
FFMPEG=/usr/bin/ffmpeg make check-hevc-main10-experimental
FFMPEG=/usr/bin/ffmpeg make check-hevc-main10-hdr-experimental
FFMPEG=/usr/bin/ffmpeg make check-vp9-profile2-experimental
FFMPEG=/usr/bin/ffmpeg make check-gstreamer-va
FFMPEG=/usr/bin/ffmpeg make check-h264-encode-experimental
FFMPEG=/usr/bin/ffmpeg make check-hevc-encode-experimental
FFMPEG=/usr/bin/ffmpeg make check-webrtc-rtp-experimental
make check-encode-soak-experimental
FFMPEG=/usr/bin/ffmpeg make check-encode-decode-concurrent
FFMPEG=/usr/bin/ffmpeg RISKY_VECTORS=run make check-conformance
FFMPEG=/usr/bin/ffmpeg RISKY_VECTORS=run make check-sanitize
```

`check-hevc-experimental` sets
`RK_VAAPI_EXPERIMENTAL_PROFILES=hevc-main` through the validator, runs only
the pinned HEVC vectors, and fails fast with a bounded FFmpeg timeout. It is a
Phase 2 development gate, not a release gate: HEVC remains unadvertised until
this target is bit-exact. On 2026-07-26 the current forced-hardware boundary is
seven bit-exact vectors: `LTRPSPS_A_Qualcomm_1.bit`,
`PPS_A_qualcomm_7.bit`, `RPS_A_docomo_4.bit`, `SLIST_A_Sony_4.bit`,
`VPSID_A_VIDYO_2.bit`, `WPP_A_ericsson_MAIN_2.bit`, and
`WP_A_Toshiba_3.bit`. SPS-backed references are consumed from the original
slice syntax and rematerialized from the current VA DPB state; scaling matrices
are carried in the per-access-unit PPS. `TILES_A_Cisco_2.bit` is the sole
failure. Direct MPP decode of the original Annex B stream and the reconstructed
VA-API path both report `errinfo=1` from frame 1 onward, so the driver maps
errored/discarded MPP frames to `VA_STATUS_ERROR_DECODING_ERROR` instead of
returning corrupt output.

`check-hevc-main10-experimental` generates 48 Main10 frames at 320x240 and also
runs the checksum-pinned FATE `WP_A_MAIN10_Toshiba_3.bit` weighted-prediction
vector (416x240, 256 frames). It forces the hidden `hevc-main10` profile,
downloads P010, and requires byte-for-byte equality with software decode for
both inputs. It also requires one MPP AFBC-to-RGA conversion per frame and
rejects linear fallback, buffer mismatch, or decode failure. Both cases are
bit-exact on the tested 2026-07-25 kernel/librga stack. MPP AFBC is mandatory
here because VDPU383's linear NV15 stride is not always RGA-expressible; the
conversion also applies MPP's AFBC crop offset. Main10 stays hidden until
broader conformance and HDR playback checks pass.

`check-hevc-main10-hdr-experimental` generates 24 Main10 HDR10 frames at
320x240. It requires P010 byte equality with software decode, one audited
AFBC-to-RGA conversion per frame, and matching per-frame limited-range
BT.2020 non-constant-luminance color, BT.2020 primaries, SMPTE ST 2084 (PQ),
mastering-display, and MaxCLL/MaxFALL metadata. This proves that libavcodec's
original-stream VUI/SEI metadata survives the VA hardware-frame path even
though the private SPS reconstructed for MPP has no VUI. It does not replace
the Firefox/mpv display-presentation gate.

`check-vp9-profile2-experimental` generates a lossless 48-frame VP9 Profile 2
stream at 320x240 and runs the checksum-pinned official WebM/libvpx
`vp92-2-20-10bit-yuv420.webm` vector (160x90, 10 displayed frames). It forces
the hidden `vp9-profile2` profile, downloads P010, and requires byte-for-byte
equality with software decode. Like Main10, it requires AFBC NV15-to-P010
conversion and rejects linear fallback, buffer mismatch, or decode failure.
The official vector additionally audits its 11 decoded frames, including one
hidden/reference output. This validates the VP9 uncompressed-header parser and
profile-matched hidden-reference handling in addition to the shared 10-bit
conversion/export path. The profile remains hidden pending app validation.

`check-gstreamer-va` rescans GStreamer 1.28's `va` plugin against the local
driver, requires registration of `vah264dec`, `vah265dec`, and `vavp9dec`,
then negotiates system-memory raw output for four pinned cases. H.264 High,
VP9 Profile 0, VP9 Profile 2, and HEVC Main10 output is byte-identical to
FFmpeg software decode; driver logs additionally require every expected MPP
frame and every 10-bit AFBC-to-RGA conversion with no stale route or fallback.
GStreamer's VA plugin rejects unrecognized vendor strings by default, so this
gate sets its supported `GST_VA_ALL_DRIVERS=1` override. This is an app/plugin
and readback result, not yet a DMABuf display-sink or HDR-presentation result.

`check-h264-encode-experimental` generates 48 deterministic 320x240 frames and
forces stock FFmpeg `h264_vaapi` through the hidden H.264 encoder in CQP, CBR,
and VBR modes. Every output must be High profile, contain exactly 48 frames,
decode with the standard H.264 decoder, exceed 35 dB average PSNR, and have one
audited MPP packet per input frame. The same source then passes through
GStreamer 1.28 `vah264enc` after a fresh plugin scan with
`GST_VA_ALL_DRIVERS=1`, with the same frame/profile/PSNR checks. The sanitizer
target loads the complete ASan/UBSan driver for all five app paths.

`check-hevc-encode-experimental` applies the same five-path gate to HEVC Main
with `hevc_vaapi` and `vah265enc`. It additionally rejects decoder/parser
warnings, verifies the advertised 64x64 CTU contract, and requires parser-clean
standard HEVC decode. The measured CQP/CBR/VBR PSNR values are 45.19, 44.46,
and 40.91 dB; GStreamer CQP measures 45.31 dB. Its sanitizer target loads the
complete ASan/UBSan driver for all five paths.

Both encode gates also run a direct FFmpeg I420 upload at CQP and feed
GStreamer I420 directly into the VA encoder without `videoconvert`. Driver-log
audits require at least one checked `I420->NV12` upload per encoded frame. The
planar stream matches the native-NV12 CQP PSNR exactly: 48.495713 dB for H.264
and 45.191850 dB for HEVC. The object gate separately verifies byte-exact I420
and YV12 `vaPutImage`/`vaGetImage` round trips, plane layouts, odd-size
rejection, and conflicting-format rejection.

`check-webrtc-rtp-experimental` runs 120 direct-I420 H.264 frames through
`vah264enc`, `h264parse`, RTP payload/depay, and standard software decode. It
captures each RTP buffer, requires more than one packet per frame, enforces a
1,200-byte maximum packet size, checks exact frame count and High profile,
requires at least 35 dB PSNR, and audits one MPP packet plus one checked planar
upload per encoded frame. The measured run produces 604 RTP packets at
41.061795 dB. Its sanitizer target loads the full ASan/UBSan driver. This is a
WebRTC-compatible media-path gate; signaling and secure peer transport are
outside its claim.

`check-encode-soak-experimental` launches simultaneous live H.264 and HEVC
GStreamer pipelines at 30 fps for two hours by default. It samples combined
RSS/fd counts from the actual pipeline processes after warmup, requires bounded
span and no sustained growth, and audits exactly one MPP packet plus at least
one I420-to-NV12 upload per frame for both codecs. Shorter
`ENCODE_SOAK_SECONDS` values are explicitly smoke-only. The measured 60-second
normal smoke completed 1,800 frames per codec with 0 KiB post-warmup RSS span
and zero fd growth; a 30-second ASan/UBSan smoke completed 900 frames per
codec. The gate also covers HEVC's visible 640x360 surface with an aligned
640x368 VA context.

`check-encode-decode-concurrent` runs 96-frame versions of both encoder gates
in parallel with the shipping synthetic decode matrix. It requires all three
processes to complete, providing a board-level overlap check between two MPP
encoder contexts and independent H.264/VP9 decode contexts.

The object-lifecycle gate crosses every former fixed-array ceiling, validates
all five typed handle namespaces and stale-handle rejection, and creates nine
simultaneous MPP decode contexts. It also checks immediate success for NV12
and P010 placeholder surfaces, validates composed P010 and split R16/GR1616
descriptors before decode, verifies NV12 `PutImage`/`GetImage` byte equality
and coded-buffer segment mapping, rejects inconsistent RT/pixel formats,
checks zero-timeout behavior for a pending fence, and checks failure signaling
when that fence's context is destroyed. Its sanitized and TSan variants apply
ASan/UBSan and thread-race checking to the complete lifecycle.

The zero-copy gate runs the synthetic H.264 reference/B-frame matrix, 4K
decode, and five VP9 runs while auditing the driver log. It requires at least
one external group and external frame, requires every created pool to be
destroyed by normal VA teardown, and rejects internal fallback, a
per-frame copy marker, an unknown buffer index/fd, or an unsafe layout. Its
worker audit also requires one clean start/stop pair per decode context and
rejects the former caller-side MPP drain marker. Its sanitized variant loads
the complete ASan/UBSan driver for the same audit.
Readback uses explicit dma-buf CPU synchronization; the zero-copy and full
conformance gates are therefore visibility/coherency regressions, not only
routing checks.

The concurrent-decode gate opens H.264 and VP9 hardware decoders in one
FFmpeg process, requires both driver workers to overlap, and compares all 240
output frames against software references. It also audits two clean external
pool lifecycles and rejects every zero-copy fallback/error marker. The
ASan/UBSan and TSan variants load fully instrumented drivers for the same
single-process workload. FFmpeg uses one decoder thread per input in this
gate: concurrency comes from the two VA contexts and their dedicated workers,
while avoiding unrelated races in FFmpeg's uninstrumented internal frame
threading. The TSan variant downloads NV12 directly to rawvideo sinks,
avoiding FFmpeg's unrelated swscale/frame-hash races without any TSan
suppression; normal and ASan/UBSan runs perform the bit-exact readback.

The soak gate generates one reusable 4K H.264 clip, loops it in a paced
single FFmpeg process for two hours, and downloads every NV12 frame. After a
60-second warmup it samples `/proc/<pid>/status` and `/proc/<pid>/fd` every 30
seconds, requiring bounded RSS variation/growth and no fd growth. FFmpeg
recreates the VA decoder at each four-second stream-loop boundary, so the same
run also requires every external pool and worker to have a matching teardown.
It requires at least 25 frames per second and no fallback, stale route, or
ownership marker. `SOAK_SECONDS`,
`SOAK_SAMPLE_SECONDS`, and `SOAK_WARMUP_SECONDS` support short harness smoke
runs, but durations below 7,200 seconds are explicitly not a Phase 1 exit
result. RSS/fd thresholds can be overridden for diagnosis; release evidence
uses the committed defaults.

The pinned `CABREF3_Sand_D.264` case is also the worker/fence regression for
field pictures: its two submissions per VA surface must share one fence and
MPP route. `vp90-2-20-big_superframe-01.webm` is the counterexample—VP9 reuse
must advance the fence so an older hidden output cannot signal the newer
picture ready. Both behaviors are required for the full gate to be bit-exact.

Do not set `RISKY_VECTORS=run` until the kernel's VP9 probability-table bounds
fix is installed and the board has booted that kernel. The
`vp90-2-10-show-existing-frame2.webm` stream can otherwise panic the RK3588 VPU
driver. The harness additionally requires both the exact running release and
the SHA-256 of `/sys/kernel/notes` to match `RISKY_KERNEL_RELEASE` and
`RISKY_KERNEL_NOTES_SHA256`. This distinguishes the current audited fixed
build `#4` from vulnerable build `#1`, which both report
`6.18.38-current-rockchip64`; a stale checkbox or environment variable cannot
enable the vector on the older build. A future kernel must be audited before
passing both its release and notes fingerprint through those variables.
Omitting `RISKY_VECTORS` quarantines the stream, but the full gate exits
non-zero so a skipped required vector can never be reported as a pass.

For diagnosis on a vulnerable boot, the safe subset is:

```sh
FFMPEG=/usr/bin/ffmpeg make check-safe
FFMPEG=/usr/bin/ffmpeg make check-sanitize-safe
```

This command is not a release gate. It prints `SAFE SUBSET GREEN; FULL GATE
STILL BLOCKED` only if all non-quarantined vectors pass.

The full synthetic reference/B-frame and repeatability matrix remains as a
supplemental regression suite:

```sh
FFMPEG=/usr/bin/ffmpeg make check-synthetic
```

Use `RENDER_NODE`, `DRIVER_DIR`, `VECTOR_DIR`, or `KEEP_WORK=1` to override the
render node, driver location, vector directory, or cleanup behavior.

## CI split

GitHub Actions runs two hardware-independent jobs on every push and pull
request:

- native unit tests, ASan/UBSan and Valgrind tests, ShellCheck, and clang-tidy;
- an AArch64 cross-build of the normal and sanitized drivers against Rockchip
  MPP commit `1375813cbbae5ad6861b166475dd8fb672183220`.

The on-board gate is a manual `workflow_dispatch` job. Its separate
`run_risky_vectors` confirmation must remain false until the VP9 kernel fix is
booted and the driver hidden-reference bridge is ready to validate; with it
false, the required quarantine intentionally fails the job.
Register the board as a self-hosted runner with the default `self-hosted`,
`linux`, and `ARM64` labels plus the custom `rk3588` label. GitHub documents
the label routing in its
[self-hosted runner guide](https://docs.github.com/en/actions/how-tos/manage-runners/self-hosted-runners/use-in-a-workflow).

Before confirming risky vectors, verify the board has the fixed kernel, the
driver build includes the hidden-reference bridge, `/usr/bin/ffmpeg` has
VA-API, and the build dependencies, `curl`, `unzip`, and `sha256sum` are
installed.

On 2026-07-21, this board was booted into fixed kernel build `#3`, identified
by kernel-notes SHA-256
`5708409f759669c2ff6a9d32597acb452632ef658c57a1f2b75a981733d7559a`.
The pinned MPP revision already contains Rockchip's January 2026 parser
handling for `show_existing_frame`; official `develop` has no later VP9 parser
or buffer-slot change. On that boot, both the unquarantined normal conformance
gate and the full ASan/UBSan gate pass, including the hidden-reference vector,
the supplemental matrix, and five VP9 determinism runs. This closes the Phase
0 hardware gate.

On 2026-07-22, the board moved to forward-port kernel build `#4`
(`Pd222-C4ad2`, patches `0001`–`0058` less `0012`), identified by kernel-notes
SHA-256
`db292410e58bd9c658a0b32b6fc7c7895f3ac4a349ae3c292c441e92e340690e`;
the booted vmlinuz md5 matches the deb payload. The build-`#4` audit: its tail
is a superset of build `#3`'s and carries the proven root-cause fix for the
risky-vector hard-lock (`0058`, clientless `RELEASE_FD` guard) plus the `0055`
register-translation bounds check and `0053`/`0054` hardening, and the `0058`
deterministic reproducer and `0057` cross reproducer passed on this exact boot
with a zero-flagged kernel journal. On that basis the risky fingerprint
defaults were advanced to build `#4`, and the full gate ladder passed on it:
host checks, all three object-lifecycle gates, both zero-copy gates, all three
concurrent-decode gates, and the risky-enabled normal plus ASan/UBSan
conformance gates — every vector bit-exact including
`vp90-2-10-show-existing-frame2.webm` and the five VP9 determinism runs, with
zero fatal kernel-journal signatures across the window. Boundary: build `#4`
is a KASAN debug build and the run shared the board with a kernel compile, so
this closes correctness gates only; the two-hour 4K soak (a performance claim)
was not re-run and stands on build `#3`'s Phase 1 evidence pending a
production rebuild of the same tail. The generic procedure now lives in the
ysp repo's `kernel-drivers/docs/kernel-validation-runbook.md`.

Earlier on 2026-07-22, the Phase 1 exit gates passed on build `#3`. Two
active H.264/VP9 contexts in one FFmpeg process produced 240/240 external
frames with overlapping workers and bit-exact normal plus ASan/UBSan output;
the suppression-free complete-driver TSan variant also consumed all 240
downloaded frames cleanly. The paced 4K soak then ran for 7,200 seconds and
216,005 external frames. Post-warmup RSS was 191,288 KiB initially and 203,512
KiB finally with a 47,844 KiB span; fd head/tail medians were 55/55 with a
24-fd transient span, and every pool/worker lifecycle matched. The complete
risky-enabled Phase 0 normal and ASan/UBSan gates were green again afterward.
