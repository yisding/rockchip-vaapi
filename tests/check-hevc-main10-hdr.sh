#!/bin/sh
# HEVC Main10 HDR10 metadata and P010 exactness hardware gate.

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
WIDTH=${MAIN10_HDR_WIDTH:-320}
HEIGHT=${MAIN10_HDR_HEIGHT:-240}
FRAMES=${MAIN10_HDR_FRAMES:-24}
FPS=${MAIN10_HDR_FPS:-24}

case $FFMPEG_TIMEOUT in
    ''|*[!0-9]*) echo "error: FFMPEG_TIMEOUT must be an integer" >&2; exit 2 ;;
esac
for value in "$WIDTH" "$HEIGHT" "$FRAMES" "$FPS"; do
    case $value in
        ''|*[!0-9]*|0)
            echo "error: HDR dimensions, frame count, and rate must be positive integers" >&2
            exit 2
            ;;
    esac
done
if [ $((WIDTH % 2)) -ne 0 ] || [ $((HEIGHT % 2)) -ne 0 ]; then
    echo "error: HDR dimensions must be even for 4:2:0 video" >&2
    exit 2
fi
if [ "$FFMPEG_TIMEOUT" != 0 ] &&
   ! command -v timeout >/dev/null 2>&1; then
    echo "error: timeout is required when FFMPEG_TIMEOUT is non-zero" >&2
    exit 2
fi

WORK=$(mktemp -d "$REPO_ROOT/.test-work.main10-hdr.XXXXXX") || exit 1
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

input=$WORK/hdr10.mkv
software=$WORK/software.p010
hardware=$WORK/hardware.p010
software_log=$WORK/software-showinfo.log
hardware_log=$WORK/hardware-showinfo.log
driver_log=$WORK/driver.log

run_ffmpeg -nostdin -y -v error \
    -f lavfi -i "testsrc2=size=${WIDTH}x${HEIGHT}:rate=${FPS}" \
    -frames:v "$FRAMES" -an -vf format=yuv420p10le \
    -c:v libx265 -preset veryfast -crf 22 \
    -x265-params "profile=main10:keyint=${FPS}:min-keyint=${FPS}:scenecut=0:hdr10=1:repeat-headers=1:colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc:master-display=G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(10000000,1):max-cll=1000,400:log-level=error" \
    "$input"

run_ffmpeg -nostdin -y -v info -i "$input" -an \
    -frames:v "$FRAMES" -vf 'format=p010le,showinfo' \
    -pix_fmt p010le -fps_mode passthrough -f rawvideo "$software" \
    2>"$software_log"

RK_VAAPI_EXPERIMENTAL_PROFILES=hevc-main10 \
LIBVA_DRIVER_NAME=rockchip \
LIBVA_DRIVERS_PATH=$DRIVER_DIR \
RK_VAAPI_LOG=$driver_log \
run_ffmpeg -nostdin -y -v info \
    -hwaccel vaapi -hwaccel_output_format vaapi \
    -vaapi_device "$RENDER_NODE" -i "$input" -an \
    -frames:v "$FRAMES" -vf 'hwdownload,format=p010le,showinfo' \
    -pix_fmt p010le -fps_mode passthrough -f rawvideo "$hardware" \
    2>"$hardware_log"

expected_size=$((WIDTH * HEIGHT * 3 * FRAMES))
software_size=$(wc -c <"$software")
hardware_size=$(wc -c <"$hardware")
if [ "$software_size" -ne "$expected_size" ] ||
   [ "$hardware_size" -ne "$expected_size" ]; then
    echo "FAIL  Main10 HDR frame count/size: expected=$expected_size software=$software_size hardware=$hardware_size" >&2
    exit 1
fi
if ! cmp -s "$software" "$hardware"; then
    echo "FAIL  Main10 HDR P010 output differs from software reference" >&2
    exit 1
fi

conversions=$(grep -c 'convert: NV15->P010.*afbc=1' "$driver_log" || true)
if [ "$conversions" -ne "$FRAMES" ] ||
   ! grep -q 'CreateContext: 10-bit output mode=AFBC_V2 profile=18' "$driver_log" ||
   grep -q 'external buffer mismatch\|decode failed\|afbc=0' "$driver_log"; then
    echo "FAIL  Main10 HDR AFBC/RGA path audit failed (conversions=$conversions expected=$FRAMES)" >&2
    exit 1
fi

assert_metadata()
{
    log=$1
    label=$2
    color_count=$(grep -c 'color_range:tv color_space:bt2020nc color_primaries:bt2020 color_trc:smpte2084' "$log" || true)
    mastering_count=$(grep -c 'side data - Mastering display metadata: has_primaries:1 has_luminance:1 r(0.6800,0.3200) g(0.2650,0.6900) b(0.1500 0.0600) wp(0.3127, 0.3290) min_luminance=0.000100, max_luminance=1000.000000' "$log" || true)
    light_count=$(grep -c 'side data - Content light level metadata: MaxCLL=1000, MaxFALL=400' "$log" || true)
    if [ "$color_count" -ne "$FRAMES" ] ||
       [ "$mastering_count" -ne "$FRAMES" ] ||
       [ "$light_count" -ne "$FRAMES" ]; then
        echo "FAIL  $label HDR metadata: color=$color_count mastering=$mastering_count light=$light_count expected=$FRAMES" >&2
        exit 1
    fi
}

assert_metadata "$software_log" software
assert_metadata "$hardware_log" hardware

echo "ok    HEVC Main10 HDR10 ${WIDTH}x${HEIGHT} $FRAMES frames P010 bit-exact with BT.2020/PQ metadata"
