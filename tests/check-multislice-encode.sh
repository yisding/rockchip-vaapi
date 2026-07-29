#!/bin/sh
# Equal-row H.264/HEVC multi-slice VA-API encode interoperability gate.

set -eu

FFMPEG=${FFMPEG:-/usr/bin/ffmpeg}
FFPROBE=${FFPROBE:-/usr/bin/ffprobe}
FFMPEG_TIMEOUT=${FFMPEG_TIMEOUT:-120}
SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH='' cd -- "$SCRIPT_DIR/.." && pwd)
DRIVER_DIR=${DRIVER_DIR:-$REPO_ROOT}
RENDER_NODE=${RENDER_NODE:-/dev/dri/renderD128}
KEEP_WORK=${KEEP_WORK:-0}
WIDTH=320
HEIGHT=240
FRAMES=12
REQUESTED_SLICES=3

for command in "$FFMPEG" "$FFPROBE" timeout grep sed; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "error: required command not found: $command" >&2
        exit 2
    fi
done

WORK=$(mktemp -d "$REPO_ROOT/.test-work.multislice.XXXXXX") || exit 1
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

run_codec()
{
    name=$1
    encoder=$2
    decoder=$3
    profile=$4
    extension=$5
    header_pattern=$6
    output=$WORK/output.$extension
    decoded=$WORK/decoded.$name.yuv
    driver_log=$WORK/driver.$name.log
    encode_log=$WORK/encode.$name.log
    trace_log=$WORK/trace.$name.log

    RK_VAAPI_EXPERIMENTAL_ENCODE=$name \
    LIBVA_DRIVER_NAME=rockchip \
    LIBVA_DRIVERS_PATH=$DRIVER_DIR \
    RK_VAAPI_LOG=$driver_log \
        timeout --kill-after=5s "$FFMPEG_TIMEOUT" "$FFMPEG" \
        -nostdin -y -v error -vaapi_device "$RENDER_NODE" \
        -f lavfi -i "testsrc2=size=${WIDTH}x${HEIGHT}:rate=30" \
        -frames:v "$FRAMES" -vf 'format=nv12,hwupload' \
        -c:v "$encoder" -profile:v "$profile" -rc_mode CQP -qp 24 \
        -slices "$REQUESTED_SLICES" "$output" >"$encode_log" 2>&1

    frames=$("$FFPROBE" -v error -count_frames -select_streams v:0 \
        -show_entries stream=nb_read_frames -of default=nw=1:nk=1 "$output")
    if [ "$frames" != "$FRAMES" ]; then
        echo "FAIL  $name multi-slice frame count=$frames expected=$FRAMES" >&2
        exit 1
    fi

    timeout --kill-after=5s "$FFMPEG_TIMEOUT" "$FFMPEG" \
        -nostdin -y -v error -c:v "$decoder" -i "$output" \
        -frames:v "$FRAMES" -pix_fmt yuv420p -f rawvideo "$decoded"
    expected_bytes=$((WIDTH * HEIGHT * 3 * FRAMES / 2))
    if [ "$(wc -c <"$decoded")" -ne "$expected_bytes" ]; then
        echo "FAIL  $name multi-slice decoded byte count is invalid" >&2
        exit 1
    fi

    timeout --kill-after=5s "$FFMPEG_TIMEOUT" "$FFMPEG" \
        -nostdin -v trace -i "$output" -map 0:v:0 -c:v copy \
        -bsf:v trace_headers -f null - >/dev/null 2>"$trace_log"
    slice_headers=$(grep -c "$header_pattern" "$trace_log" || true)
    split_frames=$(grep -c \
        'encoder slice split count=4 .* span=' "$driver_log" || true)
    packets=$(grep -c 'encoder produced .* bytes' "$driver_log" || true)
    if [ "$slice_headers" -ne $((FRAMES * 4)) ] ||
       [ "$split_frames" -ne "$FRAMES" ] ||
       [ "$packets" -ne "$FRAMES" ] ||
       grep -Eq 'encoder .*failed|config rejected' "$driver_log"; then
        echo "FAIL  $name multi-slice audit headers=$slice_headers split_frames=$split_frames packets=$packets" >&2
        exit 1
    fi

    echo "ok    $name multi-slice encode $FRAMES frames, 4 equal-row slices/frame"
}

# FFmpeg rounds the requested three slices to four for the advertised
# power-of-two-row contract at this geometry.
run_codec h264 h264_vaapi h264 high h264 'Slice Header'
run_codec hevc hevc_vaapi hevc main hevc 'Slice Segment Header'

echo "ok    H.264/HEVC VA-API multi-slice gate passed"
