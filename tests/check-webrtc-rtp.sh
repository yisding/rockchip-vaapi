#!/bin/sh
# WebRTC-compatible H.264 RTP packetization from the experimental VA encoder.

set -eu

FFMPEG=${FFMPEG:-/usr/bin/ffmpeg}
FFPROBE=${FFPROBE:-/usr/bin/ffprobe}
TIMEOUT=${WEBRTC_TIMEOUT:-120}
SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH='' cd -- "$SCRIPT_DIR/.." && pwd)
DRIVER_DIR=${DRIVER_DIR:-$REPO_ROOT}
RENDER_NODE=${RENDER_NODE:-/dev/dri/renderD128}
KEEP_WORK=${KEEP_WORK:-0}
WIDTH=${WEBRTC_WIDTH:-640}
HEIGHT=${WEBRTC_HEIGHT:-360}
FRAMES=${WEBRTC_FRAMES:-120}
FPS=${WEBRTC_FPS:-30}
MTU=${WEBRTC_MTU:-1200}
MIN_PSNR=${WEBRTC_MIN_PSNR:-35}

for command in "$FFMPEG" "$FFPROBE" timeout awk sed find wc \
               gst-inspect-1.0 gst-launch-1.0; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "error: required command not found: $command" >&2
        exit 2
    fi
done
for value in "$TIMEOUT" "$WIDTH" "$HEIGHT" "$FRAMES" "$FPS" "$MTU"; do
    case $value in
        ''|*[!0-9]*|0)
            echo "error: WebRTC gate values must be positive integers" >&2
            exit 2
            ;;
    esac
done

WORK=$(mktemp -d "$REPO_ROOT/.test-work.webrtc-rtp.XXXXXX") || exit 1
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
    timeout --kill-after=5s "$TIMEOUT" "$FFMPEG" "$@"
}

reference=$WORK/reference.yuv
output=$WORK/roundtrip.h264
decoded=$WORK/decoded.yuv
driver_log=$WORK/driver.log
gst_log=$WORK/gstreamer.log
probe_log=$WORK/probe.log
decode_log=$WORK/decode.log

run_ffmpeg -nostdin -y -v error \
    -f lavfi -i "testsrc2=size=${WIDTH}x${HEIGHT}:rate=${FPS}" \
    -frames:v "$FRAMES" -pix_fmt yuv420p -f rawvideo "$reference"
expected_size=$((WIDTH * HEIGHT * 3 * FRAMES / 2))
if [ "$(wc -c <"$reference")" -ne "$expected_size" ]; then
    echo "FAIL  WebRTC reference size is invalid" >&2
    exit 1
fi

export GST_VA_ALL_DRIVERS=1
export GST_REGISTRY="$WORK/gstreamer-registry.bin"
export LIBVA_DRIVER_NAME=rockchip
export LIBVA_DRIVERS_PATH="$DRIVER_DIR"
export RK_VAAPI_EXPERIMENTAL_ENCODE=h264
for element in vah264enc webrtcbin h264parse rtph264pay rtph264depay; do
    if ! gst-inspect-1.0 --exists "$element"; then
        echo "FAIL  required WebRTC/RTP element is unavailable: $element" >&2
        exit 1
    fi
done

RK_VAAPI_LOG=$driver_log \
    timeout --kill-after=5s "$TIMEOUT" gst-launch-1.0 -e -q \
    filesrc location="$reference" \
    ! rawvideoparse format=i420 width="$WIDTH" height="$HEIGHT" \
        framerate="$FPS/1" \
    ! video/x-raw,format=I420 \
    ! vah264enc rate-control=cbr bitrate=1000 key-int-max="$FPS" \
    ! h264parse config-interval=-1 \
    ! video/x-h264,stream-format=byte-stream,alignment=au,profile=high \
    ! rtph264pay aggregate-mode=zero-latency config-interval=-1 \
        mtu="$MTU" pt=96 \
    ! application/x-rtp,media=video,encoding-name=H264,clock-rate=90000,payload=96 \
    ! tee name=rtp \
    rtp. ! queue ! multifilesink location="$WORK/rtp-%05d.bin" \
    rtp. ! queue ! rtph264depay \
    ! h264parse \
    ! video/x-h264,stream-format=byte-stream,alignment=au \
    ! filesink location="$output" \
    >"$gst_log" 2>&1

rtp_packets=$(find "$WORK" -maxdepth 1 -name 'rtp-*.bin' -type f |
              wc -l)
max_packet=$(find "$WORK" -maxdepth 1 -name 'rtp-*.bin' -type f \
                 -printf '%s\n' |
             awk 'BEGIN { max = 0 } $1 > max { max = $1 } END { print max }')
if [ "$rtp_packets" -le "$FRAMES" ] || [ "$max_packet" -gt "$MTU" ]; then
    echo "FAIL  RTP packetization packets=$rtp_packets max=$max_packet mtu=$MTU" >&2
    exit 1
fi

encoded_frames=$("$FFPROBE" -v error -count_frames -select_streams v:0 \
    -show_entries stream=nb_read_frames -of default=nw=1:nk=1 \
    "$output" 2>"$probe_log")
profile=$("$FFPROBE" -v error -select_streams v:0 \
    -show_entries stream=profile -of default=nw=1:nk=1 \
    "$output" 2>>"$probe_log")
if [ "$encoded_frames" != "$FRAMES" ] || [ "$profile" != High ]; then
    echo "FAIL  RTP round-trip identity frames=$encoded_frames profile=$profile" >&2
    exit 1
fi

run_ffmpeg -nostdin -y -v warning -c:v h264 -i "$output" \
    -frames:v "$FRAMES" -pix_fmt yuv420p -f rawvideo "$decoded" \
    >"$decode_log" 2>&1
if grep -Eiq 'error while decoding|invalid (nal|data)|missing reference|corrupt' \
              "$probe_log" "$decode_log" ||
   [ "$(wc -c <"$decoded")" -ne "$expected_size" ]; then
    echo "FAIL  RTP round-trip did not decode cleanly" >&2
    exit 1
fi

psnr_log=$WORK/psnr.log
run_ffmpeg -nostdin -v info \
    -s "${WIDTH}x${HEIGHT}" -pix_fmt yuv420p -framerate "$FPS" \
    -f rawvideo -i "$reference" -c:v h264 -i "$output" \
    -lavfi '[0:v][1:v]psnr' -frames:v "$FRAMES" -f null - \
    >"$psnr_log" 2>&1
average=$(sed -n 's/.* average:\([^ ]*\).*/\1/p' "$psnr_log" | tail -1)
packets=$(grep -c 'encoder produced .* bytes' "$driver_log" || true)
conversions=$(grep -c 'PutImage: I420->NV12' "$driver_log" || true)
if [ -z "$average" ] || [ "$packets" -ne "$FRAMES" ] ||
   [ "$conversions" -lt "$FRAMES" ] ||
   ! awk -v actual="$average" -v minimum="$MIN_PSNR" \
         'BEGIN { exit !(actual + 0 >= minimum + 0) }' ||
   grep -q 'encoder .*failed\|rejected' "$driver_log"; then
    echo "FAIL  WebRTC RTP audit packets=$packets conversions=$conversions PSNR=$average" >&2
    exit 1
fi

echo "ok    WebRTC-compatible H.264 RTP $FRAMES frames packets=$rtp_packets max_packet=$max_packet PSNR=$average"
