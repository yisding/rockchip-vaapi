#!/bin/sh
# Full two-peer SDP/ICE/DTLS/SRTP gate for the experimental VA H.264 encoder.

set -eu

FFMPEG=${FFMPEG:-/usr/bin/ffmpeg}
FFPROBE=${FFPROBE:-/usr/bin/ffprobe}
PYTHON=${PYTHON:-/usr/bin/python3}
TIMEOUT=${WEBRTC_TIMEOUT:-120}
PEER_TIMEOUT=${WEBRTC_PEER_TIMEOUT:-90}
SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH='' cd -- "$SCRIPT_DIR/.." && pwd)
DRIVER_DIR=${DRIVER_DIR:-$REPO_ROOT}
WEBRTC_DEPS_ROOT=${WEBRTC_DEPS_ROOT:-}
KEEP_WORK=${KEEP_WORK:-0}
WIDTH=${WEBRTC_WIDTH:-640}
HEIGHT=${WEBRTC_HEIGHT:-360}
FRAMES=${WEBRTC_FRAMES:-120}
FPS=${WEBRTC_FPS:-30}
MTU=${WEBRTC_MTU:-1200}
MIN_PSNR=${WEBRTC_MIN_PSNR:-35}

for command in "$FFMPEG" "$FFPROBE" "$PYTHON" timeout awk sed wc \
               gst-inspect-1.0; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "error: required command not found: $command" >&2
        exit 2
    fi
done
for value in "$TIMEOUT" "$PEER_TIMEOUT" "$WIDTH" "$HEIGHT" "$FRAMES" \
             "$FPS" "$MTU"; do
    case $value in
        ''|*[!0-9]*|0)
            echo "error: WebRTC peer values must be positive integers" >&2
            exit 2
            ;;
    esac
done
if [ $((WIDTH % 2)) -ne 0 ] || [ $((HEIGHT % 2)) -ne 0 ]; then
    echo "error: WebRTC I420 dimensions must be even" >&2
    exit 2
fi

WORK=$(mktemp -d "$REPO_ROOT/.test-work.webrtc-peer.XXXXXX") || exit 1
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

if [ -n "$WEBRTC_DEPS_ROOT" ]; then
    typelib_dir=$WEBRTC_DEPS_ROOT/usr/lib/aarch64-linux-gnu/girepository-1.0
    plugin_dir=$WEBRTC_DEPS_ROOT/usr/lib/aarch64-linux-gnu/gstreamer-1.0
    if [ ! -f "$typelib_dir/GstWebRTC-1.0.typelib" ] ||
       [ ! -f "$plugin_dir/libgstnice.so" ]; then
        echo "error: WEBRTC_DEPS_ROOT lacks GstWebRTC typelib or nice plugin" >&2
        exit 2
    fi
    export GI_TYPELIB_PATH="$typelib_dir${GI_TYPELIB_PATH:+:$GI_TYPELIB_PATH}"
    export GST_PLUGIN_PATH="$plugin_dir${GST_PLUGIN_PATH:+:$GST_PLUGIN_PATH}"
fi

if ! "$PYTHON" -c \
    "import gi; gi.require_version('GstWebRTC','1.0'); from gi.repository import GstWebRTC" \
    >/dev/null 2>&1; then
    echo "error: GstWebRTC introspection is required" >&2
    echo "hint: install gir1.2-gst-plugins-bad-1.0" >&2
    exit 2
fi

reference=$WORK/reference.yuv
output=$WORK/received.h264
decoded=$WORK/decoded.yuv
audit=$WORK/transport.json
peer_log=$WORK/peer.log
driver_log=$WORK/driver.log
probe_log=$WORK/probe.log
decode_log=$WORK/decode.log

timeout --kill-after=5s "$TIMEOUT" "$FFMPEG" -nostdin -y -v error \
    -f lavfi -i "testsrc2=size=${WIDTH}x${HEIGHT}:rate=${FPS}" \
    -frames:v "$FRAMES" -pix_fmt yuv420p -f rawvideo "$reference"
expected_size=$((WIDTH * HEIGHT * 3 * FRAMES / 2))
if [ "$(wc -c <"$reference")" -ne "$expected_size" ]; then
    echo "FAIL  WebRTC peer reference size is invalid" >&2
    exit 1
fi

export GST_VA_ALL_DRIVERS=1
export GST_REGISTRY="$WORK/gstreamer-registry.bin"
export LIBVA_DRIVER_NAME=rockchip
export LIBVA_DRIVERS_PATH="$DRIVER_DIR"
export RK_VAAPI_EXPERIMENTAL_ENCODE=h264
if ! gst-inspect-1.0 va | grep -q '^  vah264enc:'; then
    echo "FAIL  GStreamer va plugin did not register vah264enc" >&2
    exit 1
fi
for element in vah264enc webrtcbin nicesrc nicesink dtlssrtpenc dtlssrtpdec \
               srtpenc srtpdec h264parse rtph264pay rtph264depay; do
    if ! gst-inspect-1.0 --exists "$element"; then
        echo "FAIL  required WebRTC peer element is unavailable: $element" >&2
        echo "hint: install gstreamer1.0-nice for nicesrc/nicesink" >&2
        exit 1
    fi
done

RK_VAAPI_LOG=$driver_log \
    timeout --kill-after=5s "$TIMEOUT" "$PYTHON" \
    "$SCRIPT_DIR/webrtc_peer.py" \
    --input "$reference" --output "$output" --audit "$audit" \
    --width "$WIDTH" --height "$HEIGHT" --frames "$FRAMES" --fps "$FPS" \
    --mtu "$MTU" --timeout "$PEER_TIMEOUT" \
    >"$peer_log" 2>&1

encoded_frames=$("$FFPROBE" -v error -count_frames -select_streams v:0 \
    -show_entries stream=nb_read_frames -of default=nw=1:nk=1 \
    "$output" 2>"$probe_log")
profile=$("$FFPROBE" -v error -select_streams v:0 \
    -show_entries stream=profile -of default=nw=1:nk=1 \
    "$output" 2>>"$probe_log")
if [ "$encoded_frames" != "$FRAMES" ] || [ "$profile" != High ]; then
    echo "FAIL  WebRTC peer identity frames=$encoded_frames profile=$profile" >&2
    exit 1
fi

timeout --kill-after=5s "$TIMEOUT" "$FFMPEG" -nostdin -y -v warning \
    -c:v h264 -i "$output" -frames:v "$FRAMES" \
    -pix_fmt yuv420p -f rawvideo "$decoded" >"$decode_log" 2>&1
if grep -Eiq 'error while decoding|invalid (nal|data)|missing reference|corrupt' \
              "$probe_log" "$decode_log" ||
   [ "$(wc -c <"$decoded")" -ne "$expected_size" ]; then
    echo "FAIL  WebRTC peer output did not decode cleanly" >&2
    exit 1
fi

psnr_log=$WORK/psnr.log
timeout --kill-after=5s "$TIMEOUT" "$FFMPEG" -nostdin -v info \
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
   grep -q 'encoder .*failed\|rejected' "$driver_log" ||
   ! grep -q 'both-peers-connected' "$peer_log" ||
   ! grep -q "transport-complete frames=$FRAMES" "$peer_log" ||
   ! grep -q '"offer_has_fingerprint": true' "$audit" ||
   ! grep -q '"answer_has_fingerprint": true' "$audit" ||
   ! grep -q '"state": "connected"' "$audit"; then
    echo "FAIL  WebRTC peer audit packets=$packets conversions=$conversions PSNR=$average" >&2
    tail -80 "$peer_log" >&2
    exit 1
fi

echo "ok    WebRTC H.264 peer $FRAMES frames SDP/ICE/DTLS/SRTP PSNR=$average"
