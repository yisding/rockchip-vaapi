#!/bin/sh
# Bit-exact HEVC Main10 -> MPP AFBC -> RGA P010 hardware gate.

set -eu

if [ -z "${FFMPEG:-}" ]; then
    if [ -x /usr/bin/ffmpeg ]; then
        FFMPEG=/usr/bin/ffmpeg
    else
        FFMPEG=ffmpeg
    fi
fi
FFMPEG_TIMEOUT=${FFMPEG_TIMEOUT:-120}
SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH='' cd -- "$SCRIPT_DIR/.." && pwd)
DRIVER_DIR=${DRIVER_DIR:-$REPO_ROOT}
RENDER_NODE=${RENDER_NODE:-/dev/dri/renderD128}
KEEP_WORK=${KEEP_WORK:-0}
WIDTH=${MAIN10_WIDTH:-320}
HEIGHT=${MAIN10_HEIGHT:-240}
FRAMES=${MAIN10_FRAMES:-48}
FPS=${MAIN10_FPS:-24}

case $FFMPEG_TIMEOUT in
    ''|*[!0-9]*) echo "error: FFMPEG_TIMEOUT must be an integer" >&2; exit 2 ;;
esac
for value in "$WIDTH" "$HEIGHT" "$FRAMES" "$FPS"; do
    case $value in
        ''|*[!0-9]*|0)
            echo "error: Main10 dimensions, frame count, and rate must be positive integers" >&2
            exit 2
            ;;
    esac
done

if [ "$FFMPEG_TIMEOUT" != 0 ] && ! command -v timeout >/dev/null 2>&1; then
    echo "error: timeout is required when FFMPEG_TIMEOUT is non-zero" >&2
    exit 2
fi

WORK=$(mktemp -d "$REPO_ROOT/.test-work.main10.XXXXXX") || exit 1
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
    if [ "$FFMPEG_TIMEOUT" = 0 ]; then
        "$FFMPEG" "$@"
    else
        timeout --kill-after=5s "$FFMPEG_TIMEOUT" "$FFMPEG" "$@"
    fi
}

input=$WORK/main10.mkv
software=$WORK/software.p010
hardware=$WORK/hardware.p010
driver_log=$WORK/driver.log

run_ffmpeg -nostdin -y -v error \
    -f lavfi -i "testsrc2=size=${WIDTH}x${HEIGHT}:rate=${FPS}" \
    -frames:v "$FRAMES" -an -vf format=yuv420p10le \
    -c:v libx265 -preset veryfast -crf 22 \
    -x265-params "profile=main10:keyint=${FPS}:min-keyint=${FPS}:scenecut=0:log-level=error" \
    "$input"

run_ffmpeg -nostdin -y -v error -i "$input" -an \
    -frames:v "$FRAMES" -pix_fmt p010le -fps_mode passthrough \
    -f rawvideo "$software"

RK_VAAPI_EXPERIMENTAL_PROFILES=hevc-main10 \
LIBVA_DRIVER_NAME=rockchip \
LIBVA_DRIVERS_PATH=$DRIVER_DIR \
RK_VAAPI_LOG=$driver_log \
run_ffmpeg -nostdin -y -v error \
    -hwaccel vaapi -hwaccel_output_format vaapi \
    -vaapi_device "$RENDER_NODE" -i "$input" -an \
    -frames:v "$FRAMES" -vf 'hwdownload,format=p010le' \
    -pix_fmt p010le -fps_mode passthrough -f rawvideo "$hardware"

expected_size=$((WIDTH * HEIGHT * 3 * FRAMES))
software_size=$(wc -c <"$software")
hardware_size=$(wc -c <"$hardware")
if [ "$software_size" -ne "$expected_size" ] ||
   [ "$hardware_size" -ne "$expected_size" ]; then
    echo "FAIL  Main10 frame count/size: expected=$expected_size software=$software_size hardware=$hardware_size" >&2
    exit 1
fi
if ! cmp -s "$software" "$hardware"; then
    echo "FAIL  Main10 P010 output differs from software reference" >&2
    exit 1
fi

conversions=$(grep -c 'convert: NV15->P010.*afbc=1' "$driver_log" || true)
if [ "$conversions" -ne "$FRAMES" ] ||
   ! grep -q 'CreateContext: Main10 output mode=AFBC_V2' "$driver_log" ||
   grep -q 'external buffer mismatch\\|decode failed\\|afbc=0' "$driver_log"; then
    echo "FAIL  Main10 AFBC/RGA path audit failed (conversions=$conversions expected=$FRAMES)" >&2
    exit 1
fi

echo "ok    HEVC Main10 ${WIDTH}x${HEIGHT} $FRAMES frames bit-exact (MPP AFBC -> RGA P010)"
