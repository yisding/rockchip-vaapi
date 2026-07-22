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
    tests/fetch-vectors.sh tests/validate.sh
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

## Pinned conformance vectors

The manifest at `tests/conformance-vectors.tsv` pins both the downloaded file
and extracted payload SHA-256. It currently covers three ITU-T H.264 streams
(Constrained Baseline fallback, Main field-coded VA-API, and High VA-API with
scaling lists) and four official WebM/libvpx VP9 VA-API streams. The
`decode_path` column makes hardware expectations explicit; `vaapi` cases force
hardware-frame output so an accidental software fallback cannot turn the gate
green.

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
FFMPEG=/usr/bin/ffmpeg RISKY_VECTORS=run make check-conformance
FFMPEG=/usr/bin/ffmpeg RISKY_VECTORS=run make check-sanitize
```

The object-lifecycle gate crosses every former fixed-array ceiling, validates
all five typed handle namespaces and stale-handle rejection, and creates nine
simultaneous MPP decode contexts. It also checks immediate success for a
placeholder surface, zero-timeout behavior for a pending fence, and failure
signaling when that fence's context is destroyed. Its sanitized and TSan
variants apply ASan/UBSan and thread-race checking to the complete lifecycle.

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
`RISKY_KERNEL_NOTES_SHA256`. This distinguishes the audited fixed build `#3`
from vulnerable build `#1`, which both report
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

As of 2026-07-21, this board is booted into fixed kernel build `#3`, identified
by kernel-notes SHA-256
`5708409f759669c2ff6a9d32597acb452632ef658c57a1f2b75a981733d7559a`.
The pinned MPP revision already contains Rockchip's January 2026 parser
handling for `show_existing_frame`; official `develop` has no later VP9 parser
or buffer-slot change. On that boot, both the unquarantined normal conformance
gate and the full ASan/UBSan gate pass, including the hidden-reference vector,
the supplemental matrix, and five VP9 determinism runs. This closes the Phase
0 hardware gate.
