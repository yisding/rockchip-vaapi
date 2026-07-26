#!/bin/sh
# Experimental H.264 VA-API encode interoperability and quality gate.

set -eu

FFMPEG=${FFMPEG:-/usr/bin/ffmpeg}
FFPROBE=${FFPROBE:-/usr/bin/ffprobe}
FFMPEG_TIMEOUT=${FFMPEG_TIMEOUT:-120}
SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH='' cd -- "$SCRIPT_DIR/.." && pwd)
DRIVER_DIR=${DRIVER_DIR:-$REPO_ROOT}
RENDER_NODE=${RENDER_NODE:-/dev/dri/renderD128}
KEEP_WORK=${KEEP_WORK:-0}
WIDTH=${ENCODE_WIDTH:-320}
HEIGHT=${ENCODE_HEIGHT:-240}
FRAMES=${ENCODE_FRAMES:-48}
FPS=${ENCODE_FPS:-30}
MIN_PSNR=${ENCODE_MIN_PSNR:-35}

for command in "$FFMPEG" "$FFPROBE" timeout awk sed \
               gst-inspect-1.0 gst-launch-1.0; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "error: required command not found: $command" >&2
        exit 2
    fi
done
for value in "$FFMPEG_TIMEOUT" "$WIDTH" "$HEIGHT" "$FRAMES" "$FPS"; do
    case $value in
        ''|*[!0-9]*|0)
            echo "error: encode gate values must be positive integers" >&2
            exit 2
            ;;
    esac
done

WORK=$(mktemp -d "$REPO_ROOT/.test-work.h264-encode.XXXXXX") || exit 1
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

run_ffmpeg()
{
    timeout --kill-after=5s "$FFMPEG_TIMEOUT" "$FFMPEG" "$@"
}

reference=$WORK/reference.yuv
run_ffmpeg -nostdin -y -v error \
    -f lavfi -i "testsrc2=size=${WIDTH}x${HEIGHT}:rate=${FPS}" \
    -frames:v "$FRAMES" -pix_fmt yuv420p -f rawvideo "$reference"
expected_size=$((WIDTH * HEIGHT * 3 / 2 * FRAMES))
if [ "$(wc -c <"$reference")" -ne "$expected_size" ]; then
    echo "FAIL  H.264 encode reference size is invalid" >&2
    exit 1
fi

run_mode()
{
    mode=$1
    shift
    output=$WORK/$mode.h264
    decoded=$WORK/$mode.yuv
    driver_log=$WORK/$mode.driver.log
    ffmpeg_log=$WORK/$mode.ffmpeg.log

    RK_VAAPI_EXPERIMENTAL_ENCODE=h264 \
    LIBVA_DRIVER_NAME=rockchip \
    LIBVA_DRIVERS_PATH=$DRIVER_DIR \
    RK_VAAPI_LOG=$driver_log \
        run_ffmpeg -nostdin -y -v error \
        -vaapi_device "$RENDER_NODE" \
        -f lavfi -i "testsrc2=size=${WIDTH}x${HEIGHT}:rate=${FPS}" \
        -frames:v "$FRAMES" -vf 'format=nv12,hwupload' \
        -c:v h264_vaapi -profile:v high "$@" "$output" \
        >"$ffmpeg_log" 2>&1

    encoded_frames=$("$FFPROBE" -v error -count_frames -select_streams v:0 \
        -show_entries stream=nb_read_frames -of default=nw=1:nk=1 "$output")
    profile=$("$FFPROBE" -v error -select_streams v:0 \
        -show_entries stream=profile -of default=nw=1:nk=1 "$output")
    if [ "$encoded_frames" != "$FRAMES" ] || [ "$profile" != High ]; then
        echo "FAIL  H.264 $mode output identity: frames=$encoded_frames profile=$profile" >&2
        exit 1
    fi

    run_ffmpeg -nostdin -y -v error -c:v h264 -i "$output" \
        -frames:v "$FRAMES" -pix_fmt yuv420p -f rawvideo "$decoded"
    if [ "$(wc -c <"$decoded")" -ne "$expected_size" ]; then
        echo "FAIL  H.264 $mode decoded size is invalid" >&2
        exit 1
    fi

    psnr_log=$WORK/$mode.psnr.log
    run_ffmpeg -nostdin -v info \
        -s "${WIDTH}x${HEIGHT}" -pix_fmt yuv420p -framerate "$FPS" \
        -f rawvideo -i "$reference" -c:v h264 -i "$output" \
        -lavfi '[0:v][1:v]psnr' -frames:v "$FRAMES" -f null - \
        >"$psnr_log" 2>&1
    average=$(sed -n 's/.* average:\([^ ]*\).*/\1/p' "$psnr_log" | tail -1)
    if [ -z "$average" ] ||
       ! awk -v actual="$average" -v minimum="$MIN_PSNR" \
             'BEGIN { exit !(actual + 0 >= minimum + 0) }'; then
        echo "FAIL  H.264 $mode PSNR=$average minimum=$MIN_PSNR" >&2
        exit 1
    fi

    packets=$(grep -c 'encoder produced .* bytes' "$driver_log" || true)
    if [ "$packets" -ne "$FRAMES" ] ||
       grep -q 'encoder .*failed\|rejected' "$driver_log"; then
        echo "FAIL  H.264 $mode driver audit packets=$packets expected=$FRAMES" >&2
        exit 1
    fi
    bytes=$(wc -c <"$output")
    echo "ok    H.264 encode $mode $FRAMES frames profile=High PSNR=$average bytes=$bytes"
}

run_mode cqp -rc_mode CQP -qp 24
run_mode cbr -rc_mode CBR -b:v 1M -maxrate 1M
run_mode vbr -rc_mode VBR -b:v 1M -maxrate 2M

gst_output=$WORK/gstreamer.h264
gst_decoded=$WORK/gstreamer.yuv
gst_driver_log=$WORK/gstreamer.driver.log
export GST_VA_ALL_DRIVERS=1
export GST_REGISTRY=$WORK/gstreamer-registry.bin
export LIBVA_DRIVER_NAME=rockchip
export LIBVA_DRIVERS_PATH=$DRIVER_DIR
export RK_VAAPI_EXPERIMENTAL_ENCODE=h264
if ! timeout --kill-after=5s "$FFMPEG_TIMEOUT" gst-inspect-1.0 va |
     grep -q '^  vah264enc:'; then
    echo "FAIL  GStreamer va plugin did not register vah264enc" >&2
    exit 1
fi
RK_VAAPI_LOG=$gst_driver_log \
    timeout --kill-after=5s "$FFMPEG_TIMEOUT" gst-launch-1.0 -e -q \
    filesrc location="$reference" \
    ! rawvideoparse format=i420 width="$WIDTH" height="$HEIGHT" \
        framerate="$FPS/1" \
    ! videoconvert ! video/x-raw,format=NV12 \
    ! vah264enc rate-control=cqp qpi=24 qpp=24 key-int-max="$FPS" \
    ! filesink location="$gst_output"

gst_frames=$("$FFPROBE" -v error -count_frames -select_streams v:0 \
    -show_entries stream=nb_read_frames -of default=nw=1:nk=1 "$gst_output")
gst_profile=$("$FFPROBE" -v error -select_streams v:0 \
    -show_entries stream=profile -of default=nw=1:nk=1 "$gst_output")
if [ "$gst_frames" != "$FRAMES" ] || [ "$gst_profile" != High ]; then
    echo "FAIL  GStreamer H.264 identity: frames=$gst_frames profile=$gst_profile" >&2
    exit 1
fi
run_ffmpeg -nostdin -y -v error -c:v h264 -i "$gst_output" \
    -frames:v "$FRAMES" -pix_fmt yuv420p -f rawvideo "$gst_decoded"
if [ "$(wc -c <"$gst_decoded")" -ne "$expected_size" ]; then
    echo "FAIL  GStreamer H.264 decoded size is invalid" >&2
    exit 1
fi
gst_psnr_log=$WORK/gstreamer.psnr.log
run_ffmpeg -nostdin -v info \
    -s "${WIDTH}x${HEIGHT}" -pix_fmt yuv420p -framerate "$FPS" \
    -f rawvideo -i "$reference" -c:v h264 -i "$gst_output" \
    -lavfi '[0:v][1:v]psnr' -frames:v "$FRAMES" -f null - \
    >"$gst_psnr_log" 2>&1
gst_average=$(sed -n 's/.* average:\([^ ]*\).*/\1/p' \
                      "$gst_psnr_log" | tail -1)
gst_packets=$(grep -c 'encoder produced .* bytes' "$gst_driver_log" || true)
if [ -z "$gst_average" ] || [ "$gst_packets" -ne "$FRAMES" ] ||
   ! awk -v actual="$gst_average" -v minimum="$MIN_PSNR" \
         'BEGIN { exit !(actual + 0 >= minimum + 0) }' ||
   grep -q 'encoder .*failed\|rejected' "$gst_driver_log"; then
    echo "FAIL  GStreamer H.264 audit packets=$gst_packets PSNR=$gst_average" >&2
    exit 1
fi
echo "ok    GStreamer vah264enc $FRAMES frames profile=High PSNR=$gst_average"

echo "ok    H.264 VA-API encode gate passed"
