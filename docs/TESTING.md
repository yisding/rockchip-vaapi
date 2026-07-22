# Testing and conformance

The release gate compares every decoded Y and UV byte from Rockchip VA-API
against FFmpeg's software decoder. H.264 and VP9 inverse transforms are
specified exactly, so a mismatch is a driver failure.

## Host checks

Run the unit tests, the complete sanitized build, and static analysis with:

```sh
make test
make sanitize
make lint
shellcheck tests/fetch-vectors.sh tests/validate.sh
```

`make sanitize` builds the whole driver and runs its hardware-independent unit
tests under ASan and UBSan. LeakSanitizer is disabled for these runs because it
cannot operate reliably under the board's ptrace restrictions. The hardware
gate still loads the sanitized driver into FFmpeg and stops on the first ASan
or UBSan finding.

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
FFMPEG=/usr/bin/ffmpeg RISKY_VECTORS=run make check-conformance
FFMPEG=/usr/bin/ffmpeg RISKY_VECTORS=run make check-sanitize
```

Do not set `RISKY_VECTORS=run` until the kernel's VP9 probability-table bounds
fix is installed and the board has booted that kernel. The
`vp90-2-10-show-existing-frame2.webm` stream can otherwise panic the RK3588 VPU
driver. Omitting the variable quarantines that stream, but the full gate exits
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

- native unit tests, ASan/UBSan tests, ShellCheck, and clang-tidy;
- an AArch64 cross-build of the normal and sanitized drivers against Rockchip
  MPP commit `1375813cbbae5ad6861b166475dd8fb672183220`.

The on-board gate is a manual `workflow_dispatch` job. Its separate
`run_risky_vectors` confirmation must remain false until both VP9 fixes are
deployed; with it false, the required quarantine intentionally fails the job.
Register the board as a self-hosted runner with the default `self-hosted`,
`linux`, and `ARM64` labels plus the custom `rk3588` label. GitHub documents
the label routing in its
[self-hosted runner guide](https://docs.github.com/en/actions/how-tos/manage-runners/self-hosted-runners/use-in-a-workflow).

Before confirming risky vectors, verify the board has the fixed kernel, an MPP
userspace fix for `show_existing_frame` reference accounting,
`/usr/bin/ffmpeg` with VA-API, the build dependencies, `curl`, `unzip`, and
`sha256sum`.

As of 2026-07-21, this board is still booted into the vulnerable kernel and the
pinned MPP revision still rejects the `show_existing_frame` reference counts.
The full Phase 0 gate is therefore blocked, not passed.
