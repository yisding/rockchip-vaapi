#!/bin/sh
# Detect the RK3588 MPP/vepu5xx compact 10-bit encoder-input blocker.

set -eu

MPP_ENC_TEST=${MPP_ENC_TEST:-mpi_enc_test}
PROBE_TIMEOUT=${PROBE_TIMEOUT:-15}
KEEP_WORK=${KEEP_WORK:-0}
WIDTH=${PROBE_WIDTH:-320}
HEIGHT=${PROBE_HEIGHT:-240}
SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH='' cd -- "$SCRIPT_DIR/.." && pwd)

for command in "$MPP_ENC_TEST" timeout grep tail mktemp; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "error: required command not found: $command" >&2
        exit 2
    fi
done
for value in "$PROBE_TIMEOUT" "$WIDTH" "$HEIGHT"; do
    case $value in
        ''|*[!0-9]*|0)
            echo "error: probe values must be positive integers" >&2
            exit 2
            ;;
    esac
done
if [ $((WIDTH % 2)) -ne 0 ] || [ $((HEIGHT % 2)) -ne 0 ]; then
    echo "error: 4:2:0 probe dimensions must be even" >&2
    exit 2
fi

PIXEL_STRIDE=$(((WIDTH + 63) / 64 * 64))
BYTE_STRIDE=$((PIXEL_STRIDE * 5 / 4))
VERTICAL_STRIDE=$(((HEIGHT + 15) / 16 * 16))
WORK=$(mktemp -d "$REPO_ROOT/.test-work.main10-mpp-probe.XXXXXX") ||
    exit 1
# shellcheck disable=SC2329 # Invoked by the EXIT trap below.
cleanup()
{
    if [ "$KEEP_WORK" = 1 ]; then
        echo "work files retained in $WORK"
    else
        rm -rf "$WORK"
    fi
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

set +e
LC_ALL=C timeout --signal=TERM --kill-after=2s "${PROBE_TIMEOUT}s" \
    "$MPP_ENC_TEST" \
    -w "$WIDTH" -h "$HEIGHT" \
    -hstride "$BYTE_STRIDE" -vstride "$VERTICAL_STRIDE" \
    -f 1 -t h265 -n 1 -o "$WORK/output.hevc" \
    >"$WORK/probe.log" 2>&1
probe_status=$?
set -e

if grep -Eq 'vepu5xx_set_fmt unsupport frame format (0x)?1([^0-9]|$)' \
        "$WORK/probe.log"; then
    echo "blocked: MPP vepu5xx rejects MPP_FMT_YUV420SP_10BIT"
    echo "geometry: ${WIDTH}x${HEIGHT} stride=${BYTE_STRIDE}x${VERTICAL_STRIDE}"
    exit 0
fi

echo "inconclusive: the known vepu5xx rejection was not observed" >&2
echo "mpi_enc_test status: $probe_status" >&2
tail -40 "$WORK/probe.log" >&2
exit 1
