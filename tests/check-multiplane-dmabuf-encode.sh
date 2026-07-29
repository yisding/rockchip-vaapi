#!/bin/sh
# Two-object linear NV12 DRM PRIME import -> contiguous NV12 -> H.264 gate.

set -eu

FFMPEG=${FFMPEG:-/usr/bin/ffmpeg}
FFPROBE=${FFPROBE:-/usr/bin/ffprobe}
FFMPEG_TIMEOUT=${FFMPEG_TIMEOUT:-120}
SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH='' cd -- "$SCRIPT_DIR/.." && pwd)
DRIVER_DIR=${DRIVER_DIR:-$REPO_ROOT}
KEEP_WORK=${KEEP_WORK:-0}
WIDTH=320
HEIGHT=240
FRAMES=48
FPS=30
MIN_PSNR=${MULTIPLANE_ENCODE_MIN_PSNR:-35}

for command in "$FFMPEG" "$FFPROBE" timeout awk sed; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "error: required command not found: $command" >&2
        exit 2
    fi
done

WORK=$(mktemp -d "$REPO_ROOT/.test-work.multiplane.XXXXXX") || exit 1
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

output=$WORK/two-object-nv12.h264
source=$WORK/source.nv12
decoded=$WORK/decoded.yuv
driver_log=$WORK/driver.log
probe_log=$WORK/probe.log

RK_VAAPI_EXPERIMENTAL_ENCODE=h264 \
LIBVA_DRIVER_NAME=rockchip \
LIBVA_DRIVERS_PATH=$DRIVER_DIR \
RK_VAAPI_LOG=$driver_log \
    timeout --kill-after=5s "$FFMPEG_TIMEOUT" \
    "$SCRIPT_DIR/va_multiplane_dmabuf_encode" "$output" "$source" \
    >"$probe_log" 2>&1

encoded_frames=$("$FFPROBE" -v error -count_frames -select_streams v:0 \
    -show_entries stream=nb_read_frames -of default=nw=1:nk=1 "$output")
if [ "$encoded_frames" != "$FRAMES" ]; then
    echo "FAIL  two-object DMA-BUF frame count=$encoded_frames" >&2
    exit 1
fi

timeout --kill-after=5s "$FFMPEG_TIMEOUT" "$FFMPEG" -nostdin -y -v error \
    -c:v h264 -i "$output" -frames:v "$FRAMES" \
    -pix_fmt yuv420p -f rawvideo "$decoded"

psnr_log=$WORK/psnr.log
timeout --kill-after=5s "$FFMPEG_TIMEOUT" "$FFMPEG" -nostdin -v info \
    -s "${WIDTH}x${HEIGHT}" -pix_fmt nv12 -framerate "$FPS" \
    -f rawvideo -i "$source" \
    -s "${WIDTH}x${HEIGHT}" -pix_fmt yuv420p -framerate "$FPS" \
    -f rawvideo -i "$decoded" \
    -lavfi '[0:v][1:v]psnr' -frames:v "$FRAMES" -f null - \
    >"$psnr_log" 2>&1
average=$(sed -n 's/.* average:\([^ ]*\).*/\1/p' "$psnr_log" | tail -1)

imports=$(grep -c 'CreateSurfaces: imported NV12 .* objects=2 ' \
               "$driver_log" || true)
normalizations=$(grep -c \
    'surface: normalized two-object NV12 .* into contiguous' \
    "$driver_log" || true)
packets=$(grep -c 'encoder produced .* bytes' "$driver_log" || true)
if [ "$imports" -ne 1 ] || [ "$normalizations" -ne "$FRAMES" ] ||
   [ "$packets" -ne "$FRAMES" ] || [ -z "$average" ] ||
   ! awk -v actual="$average" -v minimum="$MIN_PSNR" \
         'BEGIN { exit !(actual + 0 >= minimum + 0) }' ||
   grep -Eq 'encoder .*failed|config rejected' "$driver_log"; then
    echo "FAIL  two-object audit imports=$imports normalizations=$normalizations packets=$packets PSNR=$average" >&2
    exit 1
fi

echo "ok    two-object linear NV12 DMA-BUF encode $FRAMES frames PSNR=$average"
