#!/bin/sh
# Run the H.264 encoder app gate concurrently with shipping decode workloads.

set -eu

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH='' cd -- "$SCRIPT_DIR/.." && pwd)
KEEP_WORK=${KEEP_WORK:-0}
WORK=$(mktemp -d "$REPO_ROOT/.test-work.encode-decode.XXXXXX") || exit 1

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

(
    ENCODE_FRAMES=${CONCURRENT_ENCODE_FRAMES:-96} \
    DRIVER_DIR=${DRIVER_DIR:-$REPO_ROOT} \
        "$SCRIPT_DIR/check-h264-encode.sh"
) >"$WORK/encode.log" 2>&1 &
encode_pid=$!

(
    TEST_SET=synthetic \
    DRIVER_DIR=${DRIVER_DIR:-$REPO_ROOT} \
    FFMPEG=${FFMPEG:-/usr/bin/ffmpeg} \
        "$SCRIPT_DIR/validate.sh"
) >"$WORK/decode.log" 2>&1 &
decode_pid=$!

set +e
wait "$encode_pid"
encode_status=$?
wait "$decode_pid"
decode_status=$?
set -e

if [ "$encode_status" -ne 0 ] || [ "$decode_status" -ne 0 ]; then
    echo "FAIL  concurrent encode/decode: encode=$encode_status decode=$decode_status" >&2
    tail -80 "$WORK/encode.log" >&2
    tail -80 "$WORK/decode.log" >&2
    exit 1
fi

if ! grep -q 'H.264 VA-API encode gate passed' "$WORK/encode.log" ||
   ! grep -q 'ALL GREEN' "$WORK/decode.log"; then
    echo "FAIL  concurrent encode/decode logs lack completion markers" >&2
    exit 1
fi

echo "ok    concurrent H.264 encode and shipping-profile decode gates passed"
