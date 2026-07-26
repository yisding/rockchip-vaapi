#!/bin/sh
# Run both encoder app gates concurrently with shipping decode workloads.

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
) >"$WORK/h264-encode.log" 2>&1 &
h264_encode_pid=$!

(
    ENCODE_FRAMES=${CONCURRENT_ENCODE_FRAMES:-96} \
    DRIVER_DIR=${DRIVER_DIR:-$REPO_ROOT} \
        "$SCRIPT_DIR/check-hevc-encode.sh"
) >"$WORK/hevc-encode.log" 2>&1 &
hevc_encode_pid=$!

(
    TEST_SET=synthetic \
    DRIVER_DIR=${DRIVER_DIR:-$REPO_ROOT} \
    FFMPEG=${FFMPEG:-/usr/bin/ffmpeg} \
        "$SCRIPT_DIR/validate.sh"
) >"$WORK/decode.log" 2>&1 &
decode_pid=$!

set +e
wait "$h264_encode_pid"
h264_encode_status=$?
wait "$hevc_encode_pid"
hevc_encode_status=$?
wait "$decode_pid"
decode_status=$?
set -e

if [ "$h264_encode_status" -ne 0 ] ||
   [ "$hevc_encode_status" -ne 0 ] ||
   [ "$decode_status" -ne 0 ]; then
    echo "FAIL  concurrent encode/decode: h264=$h264_encode_status hevc=$hevc_encode_status decode=$decode_status" >&2
    tail -80 "$WORK/h264-encode.log" >&2
    tail -80 "$WORK/hevc-encode.log" >&2
    tail -80 "$WORK/decode.log" >&2
    exit 1
fi

if ! grep -q 'H.264 VA-API encode gate passed' "$WORK/h264-encode.log" ||
   ! grep -q 'HEVC VA-API encode gate passed' "$WORK/hevc-encode.log" ||
   ! grep -q 'ALL GREEN' "$WORK/decode.log"; then
    echo "FAIL  concurrent encode/decode logs lack completion markers" >&2
    exit 1
fi

echo "ok    concurrent H.264/HEVC encode and shipping-profile decode gates passed"
