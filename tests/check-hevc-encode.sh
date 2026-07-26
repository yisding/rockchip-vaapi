#!/bin/sh
# Experimental HEVC Main VA-API encode interoperability and quality gate.

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

WORK=$(mktemp -d "$REPO_ROOT/.test-work.hevc-encode.XXXXXX") || exit 1
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

reject_parser_errors()
{
    log=$1
    if grep -Eiq 'outside the valid range|error while decoding|invalid (cu|nal|data)|missing reference|corrupt' "$log"; then
        echo "FAIL  HEVC parser reported an invalid bitstream" >&2
        tail -40 "$log" >&2
        exit 1
    fi
}

reference=$WORK/reference.yuv
run_ffmpeg -nostdin -y -v error \
    -f lavfi -i "testsrc2=size=${WIDTH}x${HEIGHT}:rate=${FPS}" \
    -frames:v "$FRAMES" -pix_fmt yuv420p -f rawvideo "$reference"
expected_size=$((WIDTH * HEIGHT * 3 * FRAMES / 2))
if [ "$(wc -c <"$reference")" -ne "$expected_size" ]; then
    echo "FAIL  HEVC encode reference size is invalid" >&2
    exit 1
fi

run_mode()
{
    mode=$1
    upload_format=$2
    shift
    shift
    output=$WORK/$mode.hevc
    decoded=$WORK/$mode.yuv
    driver_log=$WORK/$mode.driver.log
    ffmpeg_log=$WORK/$mode.ffmpeg.log
    probe_log=$WORK/$mode.probe.log
    decode_log=$WORK/$mode.decode.log

    RK_VAAPI_EXPERIMENTAL_ENCODE=hevc \
    LIBVA_DRIVER_NAME=rockchip \
    LIBVA_DRIVERS_PATH=$DRIVER_DIR \
    RK_VAAPI_LOG=$driver_log \
        run_ffmpeg -nostdin -y -v error \
        -vaapi_device "$RENDER_NODE" \
        -f lavfi -i "testsrc2=size=${WIDTH}x${HEIGHT}:rate=${FPS}" \
        -frames:v "$FRAMES" -vf "format=$upload_format,hwupload" \
        -c:v hevc_vaapi -profile:v main "$@" "$output" \
        >"$ffmpeg_log" 2>&1

    encoded_frames=$("$FFPROBE" -v error -count_frames -select_streams v:0 \
        -show_entries stream=nb_read_frames -of default=nw=1:nk=1 \
        "$output" 2>"$probe_log")
    profile=$("$FFPROBE" -v error -select_streams v:0 \
        -show_entries stream=profile -of default=nw=1:nk=1 \
        "$output" 2>>"$probe_log")
    reject_parser_errors "$probe_log"
    if [ "$encoded_frames" != "$FRAMES" ] || [ "$profile" != Main ]; then
        echo "FAIL  HEVC $mode output identity: frames=$encoded_frames profile=$profile" >&2
        exit 1
    fi

    run_ffmpeg -nostdin -y -v warning -c:v hevc -i "$output" \
        -frames:v "$FRAMES" -pix_fmt yuv420p -f rawvideo "$decoded" \
        >"$decode_log" 2>&1
    reject_parser_errors "$decode_log"
    if [ "$(wc -c <"$decoded")" -ne "$expected_size" ]; then
        echo "FAIL  HEVC $mode decoded size is invalid" >&2
        exit 1
    fi

    psnr_log=$WORK/$mode.psnr.log
    run_ffmpeg -nostdin -v info \
        -s "${WIDTH}x${HEIGHT}" -pix_fmt yuv420p -framerate "$FPS" \
        -f rawvideo -i "$reference" -c:v hevc -i "$output" \
        -lavfi '[0:v][1:v]psnr' -frames:v "$FRAMES" -f null - \
        >"$psnr_log" 2>&1
    reject_parser_errors "$psnr_log"
    average=$(sed -n 's/.* average:\([^ ]*\).*/\1/p' "$psnr_log" | tail -1)
    if [ -z "$average" ] ||
       ! awk -v actual="$average" -v minimum="$MIN_PSNR" \
             'BEGIN { exit !(actual + 0 >= minimum + 0) }'; then
        echo "FAIL  HEVC $mode PSNR=$average minimum=$MIN_PSNR" >&2
        exit 1
    fi

    packets=$(grep -c 'encoder produced .* bytes' "$driver_log" || true)
    conversions=$(grep -c 'PutImage: I420->NV12' "$driver_log" || true)
    if [ "$packets" -ne "$FRAMES" ] ||
       { [ "$upload_format" = yuv420p ] &&
         [ "$conversions" -lt "$FRAMES" ]; } ||
       ! grep -q 'HEVC encoder sequence .*va_ctu=64' "$driver_log" ||
       grep -q 'encoder .*failed\|rejected' "$driver_log"; then
        echo "FAIL  HEVC $mode driver audit packets=$packets expected=$FRAMES" >&2
        exit 1
    fi
    bytes=$(wc -c <"$output")
    echo "ok    HEVC encode $mode $FRAMES frames profile=Main PSNR=$average bytes=$bytes"
}

run_mode cqp nv12 -rc_mode CQP -qp 24
run_mode cbr nv12 -rc_mode CBR -b:v 1M -maxrate 1M
run_mode vbr nv12 -rc_mode VBR -b:v 1M -maxrate 2M
run_mode i420 yuv420p -rc_mode CQP -qp 24

gst_output=$WORK/gstreamer.hevc
gst_decoded=$WORK/gstreamer.yuv
gst_driver_log=$WORK/gstreamer.driver.log
gst_probe_log=$WORK/gstreamer.probe.log
gst_decode_log=$WORK/gstreamer.decode.log
export GST_VA_ALL_DRIVERS=1
export GST_REGISTRY="$WORK/gstreamer-registry.bin"
export LIBVA_DRIVER_NAME=rockchip
export LIBVA_DRIVERS_PATH="$DRIVER_DIR"
export RK_VAAPI_EXPERIMENTAL_ENCODE=hevc
if ! timeout --kill-after=5s "$FFMPEG_TIMEOUT" gst-inspect-1.0 va |
     grep -q '^  vah265enc:'; then
    echo "FAIL  GStreamer va plugin did not register vah265enc" >&2
    exit 1
fi
RK_VAAPI_LOG=$gst_driver_log \
    timeout --kill-after=5s "$FFMPEG_TIMEOUT" gst-launch-1.0 -e -q \
    filesrc location="$reference" \
    ! rawvideoparse format=i420 width="$WIDTH" height="$HEIGHT" \
        framerate="$FPS/1" \
    ! video/x-raw,format=I420 \
    ! vah265enc rate-control=cqp qpi=24 qpp=24 key-int-max="$FPS" \
    ! filesink location="$gst_output"

gst_frames=$("$FFPROBE" -v error -count_frames -select_streams v:0 \
    -show_entries stream=nb_read_frames -of default=nw=1:nk=1 \
    "$gst_output" 2>"$gst_probe_log")
gst_profile=$("$FFPROBE" -v error -select_streams v:0 \
    -show_entries stream=profile -of default=nw=1:nk=1 \
    "$gst_output" 2>>"$gst_probe_log")
reject_parser_errors "$gst_probe_log"
if [ "$gst_frames" != "$FRAMES" ] || [ "$gst_profile" != Main ]; then
    echo "FAIL  GStreamer HEVC identity: frames=$gst_frames profile=$gst_profile" >&2
    exit 1
fi
run_ffmpeg -nostdin -y -v warning -c:v hevc -i "$gst_output" \
    -frames:v "$FRAMES" -pix_fmt yuv420p -f rawvideo "$gst_decoded" \
    >"$gst_decode_log" 2>&1
reject_parser_errors "$gst_decode_log"
if [ "$(wc -c <"$gst_decoded")" -ne "$expected_size" ]; then
    echo "FAIL  GStreamer HEVC decoded size is invalid" >&2
    exit 1
fi
gst_psnr_log=$WORK/gstreamer.psnr.log
run_ffmpeg -nostdin -v info \
    -s "${WIDTH}x${HEIGHT}" -pix_fmt yuv420p -framerate "$FPS" \
    -f rawvideo -i "$reference" -c:v hevc -i "$gst_output" \
    -lavfi '[0:v][1:v]psnr' -frames:v "$FRAMES" -f null - \
    >"$gst_psnr_log" 2>&1
reject_parser_errors "$gst_psnr_log"
gst_average=$(sed -n 's/.* average:\([^ ]*\).*/\1/p' \
                      "$gst_psnr_log" | tail -1)
gst_packets=$(grep -c 'encoder produced .* bytes' "$gst_driver_log" || true)
gst_conversions=$(grep -c 'PutImage: I420->NV12' "$gst_driver_log" || true)
if [ -z "$gst_average" ] || [ "$gst_packets" -ne "$FRAMES" ] ||
   [ "$gst_conversions" -lt "$FRAMES" ] ||
   ! grep -q 'HEVC encoder sequence .*va_ctu=64' "$gst_driver_log" ||
   ! awk -v actual="$gst_average" -v minimum="$MIN_PSNR" \
         'BEGIN { exit !(actual + 0 >= minimum + 0) }' ||
   grep -q 'encoder .*failed\|rejected' "$gst_driver_log"; then
    echo "FAIL  GStreamer HEVC audit packets=$gst_packets PSNR=$gst_average" >&2
    exit 1
fi
echo "ok    GStreamer vah265enc $FRAMES frames profile=Main PSNR=$gst_average"

echo "ok    HEVC VA-API encode gate passed"
