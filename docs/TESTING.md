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
make test-fuzz
make lint
make check-firefox-rdd-patch
make check-package-install
shellcheck tests/*.sh
python3 -c "import ast,pathlib; ast.parse(pathlib.Path('tests/webrtc_peer.py').read_text())"
```

`make sanitize` builds the whole driver and runs its hardware-independent unit
tests under ASan and UBSan. LeakSanitizer is disabled for these runs because it
cannot operate reliably under the board's ptrace restrictions. The hardware
gate still loads the sanitized driver into FFmpeg and stops on the first ASan
or UBSan finding. `make test-valgrind` runs the same unit tests with full leak
checking and treats every reported leak kind or memory error as a failure. On
AArch64, install both `valgrind` and the matching `libc6-dbg`; Valgrind needs
the dynamic loader's symbols before it can start.

`tests/log_test` is part of `make test`, `make sanitize`, `make test-tsan`, and
`make test-valgrind`. It verifies severity filtering, JSON escaping and
one-record-per-line framing, nested init/finalize plus sink reopen, and 800
concurrent records from eight threads without interleaving.

`make check-firefox-rdd-patch` checks the committed policy contract without
network access. Pass an unpacked exact Firefox 153.0 source tree to validate
the two upstream source hashes and dry-run/apply the patch:

```sh
tests/check-firefox-rdd-patch.sh /path/to/firefox-153.0
```

`make check-package-install` builds both binary packages in the repository's
parent directory, runs Lintian, checks their metadata, payload modes, config
contents, AArch64 ELF dependencies, and linker hardening, then exercises
install, driver upgrade, config-only purge, reinstall, and final purge with
`dpkg`. Bubblewrap and Fakeroot keep all package and maintainer-script writes
inside an ignored isolated root with an empty package database. Dependency
resolution is deliberately forced in that empty database; the gate separately
checks the declared package dependencies and tests package lifecycle behavior,
not dependency installation or hardware decode. Install `dpkg-dev`,
`debhelper`, `lintian`, `bubblewrap`, `fakeroot`, and the normal build
dependencies before running it.

`make test-tsan` stress-tests concurrent object insertion, lookup, removal,
and refcounted destruction under ThreadSanitizer. The full two-decoder TSan
gate remains a Phase 1 exit requirement.

## Bitstream reconstructor fuzzing

`make test-fuzz` builds three libFuzzer targets against the reconstruction and
parsing code that consumes VA buffers — `tests/h264_fuzz.c`,
`tests/hevc_fuzz.c`, and `tests/vp9_fuzz.c` — with ASan, UBSan, and leak
detection enabled. The driver runs inside a browser media process, so every VA
picture/slice/IQ-matrix buffer is treated as hostile input: the harnesses fill
whole parameter structures from fuzzer bytes rather than only mutating
bitstreams, and they abort if a writer reports more bytes than the caller's
buffer could hold.

Each target runs in two stages. The committed seed corpus under
`tests/fuzz-seeds/<target>/` is replayed first as a deterministic regression
check, then a `FUZZ_RUNS`-long campaign (20,000 by default) runs from a scratch
copy of it. Crash and UBSan artifacts are written under `tests/.fuzz/`. Raise
the campaign length for a real hunt:

```sh
make test-fuzz FUZZ_RUNS=5000000
```

The seed corpora are coverage-minimized (`-merge=1`) outputs of longer
campaigns plus named reproducers for bugs the fuzzer found.
`tests/fuzz-seeds/hevc_fuzz/slice-rewrite-ctb-log2-shift` is the input that
first reached an out-of-range shift in the HEVC slice rewriter; the rewriter
now validates its own picture-parameter syntax bounds instead of trusting an
earlier parameter-set call to have rejected them.

libFuzzer needs Clang's sanitizer runtime archives, which Debian and Ubuntu
ship separately from the compiler. Install the package matching the Clang you
build with, for example:

```sh
sudo apt install clang libclang-rt-21-dev
```

Without it the link fails with `cannot find ... libclang_rt.fuzzer.a`. Use
`FUZZ_CC` to select a different Clang.

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
tiles, VPS IDs, WPP, and weighted prediction, and are all `vaapi` as of
2026-07-28. The
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

## Widening the pinned set

`tests/validate.sh` only runs what the manifest already pins.
`tests/sweep-hevc-conformance.sh` is the step before that: point it at a
directory of candidate streams and it reports which ones are bit-exact,
emitting manifest-ready rows for those.

```sh
mkdir -p ~/Code/tmp/hevc-sweep/vectors
# fetch candidates from https://fate-suite.ffmpeg.org/hevc-conformance/
make tests/hevc_mpp_repro
tests/sweep-hevc-conformance.sh ~/Code/tmp/hevc-sweep/vectors
```

Each candidate is classified in four steps: `ffprobe` rejects anything that is
not the profile under test, software decode establishes the reference, direct
MPP records whether the backend handles the original Annex-B stream at all,
and VA-API output is compared byte-for-byte against software.

Direct MPP is a classifier, not a gate. Its frame count legitimately differs
from the VA-API path on output-order streams, where libavcodec decides what is
output and MPP does not, so every candidate is still compared through VA-API.
The backend verdict only decides attribution: a VA-API failure on a stream the
backend also could not decode is reported as `backend`, and only a failure the
backend handled cleanly is reported as `driver`. A picture larger than the
advertised size constraints is reported as `unsup` -- refusing it is the
documented contract, and FFmpeg falls back to software.

The script never edits the manifest. Promoting a vector stays a decision a
maintainer makes after reading the report.

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
FFMPEG=/usr/bin/ffmpeg make check-hevc
FFMPEG=/usr/bin/ffmpeg make check-hevc-conformance-sweep
FFMPEG=/usr/bin/ffmpeg make check-hevc-main10-conformance-sweep
make check-hevc-tiles-backend
make probe-av1-platform
FFMPEG=/usr/bin/ffmpeg make check-hevc-main10-experimental
FFMPEG=/usr/bin/ffmpeg make check-hevc-main10-hdr-experimental
FFMPEG=/usr/bin/ffmpeg make check-vp9-profile2-experimental
FFMPEG=/usr/bin/ffmpeg make check-10bit-throughput-experimental
FFMPEG=/usr/bin/ffmpeg make check-gstreamer-va
FFMPEG=/usr/bin/ffmpeg make check-vlc-display
FFMPEG=/usr/bin/ffmpeg make check-mpv-display
FFMPEG=/usr/bin/ffmpeg make check-firefox-decode
FIREFOX=/path/to/patched/firefox FIREFOX_RDD_SANDBOX=enabled \
  FFMPEG=/usr/bin/ffmpeg make check-firefox-decode
FFMPEG=/usr/bin/ffmpeg make check-h264-encode-experimental
FFMPEG=/usr/bin/ffmpeg make check-hevc-encode-experimental
FFMPEG=/usr/bin/ffmpeg make check-multiplane-dmabuf-encode-experimental
FFMPEG=/usr/bin/ffmpeg make check-multiplane-dmabuf-encode-experimental-sanitize
FFMPEG=/usr/bin/ffmpeg make check-multislice-encode-experimental
FFMPEG=/usr/bin/ffmpeg make check-multislice-encode-experimental-sanitize
FFMPEG=/usr/bin/ffmpeg make check-webrtc-rtp-experimental
FFMPEG=/usr/bin/ffmpeg make check-webrtc-peer-experimental
make check-encode-soak-experimental
FFMPEG=/usr/bin/ffmpeg make check-encode-decode-concurrent
FFMPEG=/usr/bin/ffmpeg make check-encode-decode-same-process
FFMPEG=/usr/bin/ffmpeg make check-encode-decode-same-process-sanitize
FFMPEG=/usr/bin/ffmpeg make check-encode-decode-same-process-tsan
FFMPEG=/usr/bin/ffmpeg RISKY_VECTORS=run make check-conformance
FFMPEG=/usr/bin/ffmpeg RISKY_VECTORS=run make check-sanitize
```

`check-hevc` runs only the pinned HEVC vectors and fails fast with a bounded
FFmpeg timeout. All eight are bit-exact as of 2026-07-28, normally and with the
complete ASan/UBSan driver, and `VAProfileHEVCMain` is advertised by default.
SPS-backed references are consumed from the original slice syntax and
rematerialized from the current VA DPB state; explicit long-term entries are
reproduced from the stream unchanged; scaling matrices are carried in the
per-access-unit PPS.

`TILES_A_Cisco_2.bit` was the last holdout. Direct MPP decode of the original
Annex B stream reported `errinfo=1` from frame 1 onward, so the failure was
never attributable to the driver. `make check-hevc-tiles-backend` builds a
libva-free MPP runner and reduces the pinned stream by software-valid
access-unit prefix, requiring the known-good `PPS_A_qualcomm_7.bit` control to
decode cleanly first so a broken kernel/device stack cannot be mistaken for a
TILES-specific failure. On the current stack the control, the complete
100-frame vector, and the reduced two-picture core all decode cleanly, and the
vector is bit-exact through VA-API. The host-side packet, NAL, PPS-transition,
checksum, and kernel-rerun details are recorded in
[`HEVC_TILES_BACKEND.md`](HEVC_TILES_BACKEND.md).

`check-hevc-conformance-sweep` is the wide gate behind that claim. It fetches
every HEVC Main candidate in the FFmpeg FATE conformance suite into
`tests/vectors/hevc-sweep/` against pinned checksums and requires each one to
land in exactly the class `tests/hevc-sweep-vectors.tsv` records. It is slow --
budget well over an hour -- and is not part of `check`.

The pinned Main expectation is now 144 `exact`, 17 `skip`, two `unsup`, and
zero `backend` or `driver`. The complete installed-package sweep established
142 of those exact cases. The other two manifest rows,
`NUT_A_ericsson_4.bit` and `NUT_A_ericsson_5.bit`, are byte-identical
(SHA-256
`d87dcae6353a680ff1c816395b578afae3ed9f1a88b56b07a24e62333e0621b7`)
and were focused-verified with the fixed source stack:

- MPP `3381fd2c` returns 34/34 clean direct frames for both names after
  retaining RASL suppression but no longer suppressing valid RADL pictures;
- FFmpeg upstream fix `265d39e551` generates unavailable unused
  `ST_FOLL`/`LT_FOLL` references as HEVC 8.3.3 requires; and
- the combined VA-API run returns 34 hardware frames byte-exact against the
  34-frame software reference.

An installed stack lacking either fix is expected to diverge from the new
manifest. Package both source commits before treating
`make check-hevc-conformance-sweep` as a release result, then rerun all 163
candidates; the focused result does not substitute for that complete
regression. Missing-reference diagnostics alone are not a failure for this
stream because its unavailable entries are unused following pictures. The
required signals are the 34-frame count, zero MPP error/discard flags, EOS,
and byte-exact VA-API output.

`check-hevc-main10-conformance-sweep` is the same tool with `PROFILE=main10`,
comparing P010 output over the FATE Main10 candidates. Ten of the eleven real
Main10 streams are bit-exact. The exception is `WPP_D_ericsson_MAIN10_2.bit` at
64x240: the AFBC NV15-to-P010 repack every 10-bit surface depends on requires
RGA3, whose input and output active-width minimum is 68. The driver now returns
`VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED` at context creation, before MPP or
RGA setup. `make check-hevc-main10-narrow-fallback` requires FFmpeg to complete
all 48 frames through software fallback, with one context rejection, zero RGA
conversions, and no kernel `no core match`. Main10 stays unadvertised, so this
records the 10-bit boundary rather than gating a shipping profile.

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

`check-10bit-throughput-experimental` generates 240-frame 1920x1080 HEVC
Main10 and VP9 Profile 2 streams, downloads P010, and times the complete
VA-API decode plus AFBC-to-RGA conversion path. It requires at least 60 fps,
exact visible-frame count, no linear 10-bit fallback, and one audited
conversion per decoded output. HEVC requires exactly 240 decoded frames; VP9
allows additional hidden/reference outputs but requires conversions and
assigned frames to match. The measured runs are 261.38 fps for HEVC (240
visible/decoded) and 261.08 fps for VP9 (240 visible, 254 decoded).

`check-vlc-display` plays generated H.264 High, HEVC Main, and HEVC Main10
clips through stock VLC with `--avcodec-hw=vaapi` and requires that VLC select its VA-API
hardware decoder, name this driver, call `vaDeriveImage`, and produce at least
`MIN_FRAMES` external-pool frames with no driver error markers. It refuses to
run without `DISPLAY` or `WAYLAND_DISPLAY`: headless VLC reports "no hw decoder
modules matched", falls back to software, and never loads this driver, so a
headless pass would be evidence of nothing.

VLC's OpenGL VA-API converters derive an image from the decoded surface, take
its buffer handle as a DRM PRIME fd, and import that as an EGLImage. The driver
implements `vaDeriveImage` over the surface's own DMA-BUF and
`vaAcquireBufferHandle` over that buffer; `tests/driver_objects_test` covers
the layout, the aliasing (a write through the mapping is visible through a
second map), the DRM PRIME handle, and the refusal of other memory types,
normally and under ASan/UBSan and TSan. Completed driver-owned P010 and an
aligned provisional P010 converter probe are accepted; imported, stale, or
unaligned provisional P010 layouts fail closed. The measured display run
produces 118 H.264, 120 HEVC Main, and 120 HEVC Main10 external frames.

VLC destroys its decoder mid-stream at exit with the last pictures still in
flight. The driver reports that as a teardown-drain note and fails those
fences rather than waiting forever; it is not an error and the gate does not
treat it as one.

`check-mpv-display` first generates 20 H.264 High CIF frames at 352x288 and
plays the complete clip through stock mpv 0.41.0 with `--hwdec=vaapi`,
gpu-next, OpenGL, and the active Wayland context. It requires VA-API hardware decode,
the VAAPI NV12 video-output path, exactly one MPP info-change, at least 20
external-pool frames, and at least 20 352-to-384-byte RGA repacks. It rejects
Panfrost's `WSI pitch not properly aligned`, EGL mapping/import failures,
render failures, and hardware-frame downloads. The gate refuses to run without
a Wayland session because a headless pass cannot exercise EGL DMA-BUF import.

This narrow repack is an export/display compatibility path, not a decode-path
copy: MPP still decodes into its external pool, layouts already aligned to 64
bytes stay zero-copy through export, and only decoded driver-owned 8-bit NV12
surfaces with an incompatible pitch are repacked once per surface fence.
`tests/driver_objects_test` independently fills 352-stride NV12 luma/chroma,
repacks to 384 stride, and compares every active output byte. The gate then
plays a generated HEVC Main10 HDR10 clip and requires VA-API P010 output,
BT.2020/PQ input metadata, one AFBC conversion per frame, and successful EGL
presentation. Passing this proves the HDR-tagged P010 presentation path, not
physical HDR-monitor passthrough. The current GNOME session has no Wayland
`wl_output`; Mutter's `GetCurrentState` also returns empty physical-monitor
and logical-monitor arrays. The expanded gate is therefore
environment-blocked before either case; the earlier 8-bit H.264 result remains
the last completed mpv evidence.

`check-firefox-decode` selects H.264 High, HEVC Main, and/or HEVC Main10
through `FIREFOX_CASES` and plays them in stock Firefox with a throwaway
profile. It requires at least `MIN_FRAMES` external frames plus DMA-BUF
exports, with no driver error markers. `KEEP_WORK=1` preserves the exact page,
media, browser log, and driver log for boundary diagnosis. Like the VLC gate
it refuses to run headless.

By default it runs with `MOZ_DISABLE_RDD_SANDBOX=1` and says so in its own
output. The stock Firefox binary cannot reach `/dev/mpp_service`, `/dev/rga`
or `/dev/dma_heap` from a sandboxed RDD process; `contrib/firefox` holds
version-pinned 152.0.6 and 153.0 source patches that add exactly those broker
paths and ioctls, but they have to be applied to a Firefox source build. The
default mode therefore proves the decode and export path, not the sandbox
story. `FIREFOX_RDD_SANDBOX=enabled` instead removes the bypass and requires
the live RDD process to be present with Linux seccomp filter mode 2 before the
hardware evidence can pass. For Main10 it also enables Firefox's `Dmabuf` log
and requires the measured Panfrost `EGL_BAD_MATCH` followed by the patched
one-shot swapped-chroma retry.

Stock Firefox 153 completes the H.264 and HEVC Main cases. Its Main10 case
exports the standards-correct split P010 descriptor and reaches three hardware
frames before falling back: Firefox creates the luma `R16` EGL image, then
Panfrost rejects the chroma `GR1616` image with `EGL_BAD_MATCH`. The companion
P010 patches preserve that first attempt and retry Firefox's existing RG/GR
alternative once after a real creation failure. `check-firefox-rdd-patch`
hash-pins all three relevant upstream files per version, applies both the RDD
and P010 patches, and checks the retry contract. Source application and the
152.0.6 release-object compile pass; the full package/sandboxed playback result
is tracked separately.

Chromium is not gated. On this stack Chromium 150 cannot initialize a GL
context at all -- ANGLE reports "Could not create a backing OpenGL context" on
Mali-G610/Panfrost under both X11 and Wayland -- so its GPU process never
starts and no VA-API decode path is reachable. The same session runs accelerated
GL for VLC and Firefox, so this is a Chromium/ANGLE limitation rather than a
driver one, and no Chromium claim is made either way.

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

`check-rgb-dmabuf-encode-experimental` allocates a real linear BGRA DMA-BUF,
imports it with a DRM PRIME 2 descriptor through public libva, closes the
application descriptor fd, and updates the same surface for 48 H.264 High
frames. The gate requires exactly one accepted import, 48 RGA RGB-to-NV12
conversions, 48 MPP packets, standard FFmpeg decode, and at least 30 dB against
a software BGRA-to-YUV reference. The measured normal and full-driver
ASan/UBSan runs both produce 48/48 frames at 37.140921 dB. The object gate
separately checks fd lifetime, re-export identity, and rejection of VA-managed
RGB and multi-object RGB descriptors.

`check-multiplane-dmabuf-encode-experimental` imports linear NV12 with
separate luma and chroma DMA-BUF objects through public libva, closes the
application fds, and encodes 48 H.264 High frames. It requires an accurate
two-object re-export, one private normalization per frame, 48 MPP packets,
standard decode, and at least 40 dB against the reference; the measured result
is 50.683977 dB normally and under ASan/UBSan. Object tests cover the same
contract for P010 and reject too-small objects, nonzero plane offsets, and
non-linear modifiers.

`check-multislice-encode-experimental` supplies four contiguous equal-row
slices per H.264 macroblock picture and HEVC CTU picture. It requires 12
complete frames per codec, exactly four parser-visible slices per frame,
standard decoder acceptance, and driver audit of the requested MPP row split.
The sanitizer target repeats both paths with the full ASan/UBSan driver.

`check-webrtc-rtp-experimental` runs 120 direct-I420 H.264 frames through
`vah264enc`, `h264parse`, RTP payload/depay, and standard software decode. It
captures each RTP buffer, requires more than one packet per frame, enforces a
1,200-byte maximum packet size, checks exact frame count and High profile,
requires at least 35 dB PSNR, and audits one MPP packet plus one checked planar
upload per encoded frame. The measured run produces 604 RTP packets at
41.061795 dB. Its sanitizer target loads the full ASan/UBSan driver. This is a
WebRTC-compatible media-path gate; signaling and secure peer transport are
outside its claim.

`check-webrtc-peer-experimental` connects two local `webrtcbin` peers through
an in-process SDP offer/answer and trickle-ICE exchange. It requires connected
peer and ICE states, SDP fingerprints, connected DTLS-SRTP elements on both
peers, and an H.264 receiver pad before accepting the same 120-frame
`vah264enc` stream. The receiver depayloads and standard-decodes the resulting
Annex B stream; the outer gate checks exact High-profile frame count, at least
35 dB PSNR, and one MPP packet plus one checked I420 upload per frame.
`python3-gi`, `gir1.2-gst-plugins-bad-1.0`, and `gstreamer1.0-nice` are
additional test dependencies. `WEBRTC_DEPS_ROOT` can point at an extracted
arm64 package root during development. Running `tests/webrtc_peer.py` with
`--encoder openh264enc` is a transport-only diagnostic and is not hardware
encode evidence.

On 2026-07-29, the exact Ubuntu arm64 packages
`gir1.2-gst-plugins-bad-1.0 1.28.2-1ubuntu1.1` and
`gstreamer1.0-nice 0.1.23-2` were extracted under a development-only
`WEBRTC_DEPS_ROOT`. The 120-frame hardware peer gate passed at 41.061795 dB
both normally and with the full ASan/UBSan driver. SDP fingerprints, connected
peer/ICE/DTLS-SRTP state, 120 High-profile received access units, 120 MPP
packets, and checked I420 uploads were all required. This is full local
hardware WebRTC transport evidence, but not evidence that those two optional
test packages are installed system-wide.

`check-encode-soak-experimental` launches simultaneous live H.264 and HEVC
GStreamer pipelines at 30 fps for two hours by default. It samples combined
RSS/fd counts from the actual pipeline processes after warmup, requires bounded
span and no sustained growth, and audits exactly one MPP packet plus at least
one I420-to-NV12 upload per frame for both codecs. Shorter
`ENCODE_SOAK_SECONDS` values are explicitly smoke-only. The qualification run
completed the full 7,200 seconds and 216,000 frames per codec. Combined RSS
moved from 56,328 to 52,708 KiB with a 3,620 KiB total span and no growth; fd
count remained exactly 60. A 30-second ASan/UBSan smoke completed 900 frames
per codec. The gate also covers HEVC's visible 640x360 surface with an aligned
640x368 VA context.

`check-encode-decode-concurrent` runs 96-frame versions of both encoder gates
in parallel with the shipping synthetic decode matrix. It requires all three
processes to complete, providing a board-level overlap check between two MPP
encoder contexts and independent H.264/VP9 decode contexts.

`check-encode-decode-same-process` tightens that claim by creating two
hardware decoders and two hardware encoders in one public-libav process. Each
context must complete 120 frames, both decode workers must overlap, and both
encoders must emit 120 packets. Normal and ASan/UBSan runs retain shared filter
graphs and compare downloaded files. The TSan target uses independent simple
graphs, separate encode sources, and direct raw decode sinks so uninstrumented
libavfilter races do not mask driver races; it uses no TSan suppressions. All
three variants pass.

`make probe-mpp-main10-encode` is a bounded, libva-free diagnostic for the
current HEVC Main10 encoder blocker. It configures MPP format id 1 with the
compact 10-bit byte stride and requires the tested `vepu5xx_set_fmt`
unsupported-format result. It is not a passing Main10 encode claim; a changed
backend result is inconclusive until the full promotion gates in
[`HEVC_MAIN10_ENCODE_BACKEND.md`](HEVC_MAIN10_ENCODE_BACKEND.md) pass.

`make probe-av1-platform` is non-submitting Phase 0 discovery. It reports a
versioned key/value inventory of the public MPP AV1 capability, bound vendor
AV1 devices, `/dev/mpp_service` access, MPP readiness messages, and compressed
V4L2 OUTPUT formats. On the audited image it finds one bound
`rockchip,av1-decoder` endpoint and MPP API advertisement, but no `AV1F` V4L2
node and six generic MPP “driver is not ready” messages. The report therefore
ends with `hardware_decode_attempted=0`, `phase0_qualified=0`, and
`result=endpoint-present-unqualified`. `AV1_REQUIRE_ENDPOINT=1` can make an
endpoint-missing report fail for platform provisioning checks; it still does
not turn discovery into decode qualification.

The object-lifecycle gate crosses every former fixed-array ceiling, validates
all five typed handle namespaces and stale-handle rejection, and creates nine
simultaneous MPP decode contexts. It also checks immediate success for NV12
and P010 placeholder surfaces, validates composed P010 and split R16/GR1616
descriptors before decode, validates byte-exact linear P010 PRIME import and
readback in one- and two-object layouts plus packed-RGB PRIME import/re-export
and owned-fd lifetime, verifies
NV12/P010 `PutImage`/`GetImage` byte equality and coded-buffer segment mapping,
rejects inconsistent RT/pixel formats, checks zero-timeout behavior for a
pending fence, and checks failure signaling when that fence's context is
destroyed. Its sanitized and TSan variants apply ASan/UBSan and thread-race
checking to the complete lifecycle.

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
`RISKY_KERNEL_NOTES_SHA256`. The defaults name the current audited production
kernel, `6.18.40-ysp-rockchip64` with notes SHA-256
`db18acdddf7ba9de84590a5816911ed2d929643980057d639a90c2b1337d900c`;
a stale checkbox or environment variable therefore cannot enable the vector
on an older or merely same-version build. A future kernel must be audited
before advancing both variables.
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
`run_risky_vectors` confirmation may be enabled only when the runner matches
the exact audited release and notes fingerprint in `tests/validate.sh`; with it
false, the required quarantine intentionally fails the job.
Register the board as a self-hosted runner with the default `self-hosted`,
`linux`, and `ARM64` labels plus the custom `rk3588` label. GitHub documents
the label routing in its
[self-hosted runner guide](https://docs.github.com/en/actions/how-tos/manage-runners/self-hosted-runners/use-in-a-workflow).

Before confirming risky vectors, verify the board has the fixed kernel, the
driver build includes the hidden-reference bridge, `/usr/bin/ffmpeg` has
VA-API, and the build dependencies, `curl`, `unzip`, and `sha256sum` are
installed.

On 2026-07-29, installed `rockchip-vaapi 1.0.11+ysp5` was verified byte-for-byte
against its built deb payload on the production-shaped stack:
`6.18.40-ysp-rockchip64` (notes
`db18acdddf7ba9de84590a5816911ed2d929643980057d639a90c2b1337d900c`),
`librockchip-mpp1 1.5.0+git20260727.d8c6b88a`, and
`librga2 2.2.0+git20260725.26a50ef`. The installed-driver 64x240 Main10 gate
software-decoded all 48 frames after one up-front context refusal with zero
RGA submissions and zero kernel `no core match` messages. The complete pinned
conformance gate then passed, including the guarded VP9 hidden-reference
vector. These are installed-package correctness results; they do not substitute
for a genuinely clean-image install or the two-hour resource soaks.

The final `1.0.11+ysp6` driver/config packages build with the native Debian
toolchain and pass Lintian plus the isolated clean install, upgrade, config
purge, reinstall, and full-purge lifecycle. Against checksum-verified exact
Published MPP `3381fd2c` and FFmpeg `33a651a55b` binaries, the complete
risky-enabled shipping matrix is also green with the full ASan/UBSan driver:
all pinned vectors, the H.264 reference/B-frame and 4K matrix, five VP9
determinism runs, and VP8 software fallback. This is exact-package correctness
evidence; host installation and installed-payload identity remain separate.

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
