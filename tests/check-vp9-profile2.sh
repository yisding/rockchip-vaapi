#!/bin/sh
# Bit-exact VP9 Profile 2 -> MPP AFBC -> RGA P010 hardware gate.

set -eu

if [ -z "${FFMPEG:-}" ]; then
    if [ -x /usr/bin/ffmpeg ]; then
        FFMPEG=/usr/bin/ffmpeg
    else
        FFMPEG=ffmpeg
    fi
fi
if [ -z "${FFPROBE:-}" ]; then
    if [ -x /usr/bin/ffprobe ]; then
        FFPROBE=/usr/bin/ffprobe
    else
        FFPROBE=ffprobe
    fi
fi
FFMPEG_TIMEOUT=${FFMPEG_TIMEOUT:-120}
SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH='' cd -- "$SCRIPT_DIR/.." && pwd)
DRIVER_DIR=${DRIVER_DIR:-$REPO_ROOT}
RENDER_NODE=${RENDER_NODE:-/dev/dri/renderD128}
KEEP_WORK=${KEEP_WORK:-0}
WIDTH=${VP9_PROFILE2_WIDTH:-320}
HEIGHT=${VP9_PROFILE2_HEIGHT:-240}
FRAMES=${VP9_PROFILE2_FRAMES:-48}
FPS=${VP9_PROFILE2_FPS:-24}
PINNED_VECTOR=${VP9_PROFILE2_PINNED_VECTOR:-$SCRIPT_DIR/vectors/vp92-2-20-10bit-yuv420.webm}
PINNED_SHA256=c4b56b148d5039aa824fde3d4877dbd2604d0de7f77af96f4ba1ade537396a38
PINNED_CONVERSIONS=11

case $FFMPEG_TIMEOUT in
    ''|*[!0-9]*) echo "error: FFMPEG_TIMEOUT must be an integer" >&2; exit 2 ;;
esac
for value in "$WIDTH" "$HEIGHT" "$FRAMES" "$FPS"; do
    case $value in
        ''|*[!0-9]*|0)
            echo "error: Profile 2 dimensions, frame count, and rate must be positive integers" >&2
            exit 2
            ;;
    esac
done

if [ "$FFMPEG_TIMEOUT" != 0 ] && ! command -v timeout >/dev/null 2>&1; then
    echo "error: timeout is required when FFMPEG_TIMEOUT is non-zero" >&2
    exit 2
fi

WORK=$(mktemp -d "$REPO_ROOT/.test-work.vp9-profile2.XXXXXX") || exit 1
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

input=$WORK/profile2.webm
software=$WORK/software.p010
hardware=$WORK/hardware.p010
driver_log=$WORK/driver.log

run_ffmpeg -nostdin -y -v error \
    -f lavfi -i "testsrc2=size=${WIDTH}x${HEIGHT}:rate=${FPS}" \
    -frames:v "$FRAMES" -an -vf format=yuv420p10le \
    -c:v libvpx-vp9 -profile:v 2 -lossless 1 -row-mt 1 \
    -threads 4 -deadline good -cpu-used 4 "$input"

run_ffmpeg -nostdin -y -v error -i "$input" -an \
    -frames:v "$FRAMES" -pix_fmt p010le -fps_mode passthrough \
    -f rawvideo "$software"

RK_VAAPI_EXPERIMENTAL_PROFILES=vp9-profile2 \
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
    echo "FAIL  VP9 Profile 2 frame count/size: expected=$expected_size software=$software_size hardware=$hardware_size" >&2
    exit 1
fi
if ! cmp -s "$software" "$hardware"; then
    echo "FAIL  VP9 Profile 2 P010 output differs from software reference" >&2
    exit 1
fi

conversions=$(grep -c 'convert: NV15->P010.*afbc=1' "$driver_log" || true)
if [ "$conversions" -ne "$FRAMES" ] ||
   ! grep -q 'CreateContext: 10-bit output mode=AFBC_V2 profile=21' "$driver_log" ||
   grep -q 'external buffer mismatch\|decode failed\|afbc=0' "$driver_log"; then
    echo "FAIL  VP9 Profile 2 AFBC/RGA path audit failed (conversions=$conversions expected=$FRAMES)" >&2
    exit 1
fi

echo "ok    VP9 Profile 2 ${WIDTH}x${HEIGHT} $FRAMES frames bit-exact (MPP AFBC -> RGA P010)"

if [ ! -f "$PINNED_VECTOR" ]; then
    echo "FAIL  pinned VP9 Profile 2 vector missing; run 'make fetch-vectors'" >&2
    exit 1
fi
actual_sha=$(sha256sum "$PINNED_VECTOR" | awk '{print $1}')
if [ "$actual_sha" != "$PINNED_SHA256" ]; then
    echo "FAIL  pinned VP9 Profile 2 vector checksum mismatch" >&2
    exit 1
fi

pinned_software=$WORK/pinned-software.p010
pinned_hardware=$WORK/pinned-hardware.p010
pinned_log=$WORK/pinned-driver.log
run_ffmpeg -nostdin -y -v error -i "$PINNED_VECTOR" -an \
    -pix_fmt p010le -fps_mode passthrough -f rawvideo "$pinned_software"
RK_VAAPI_EXPERIMENTAL_PROFILES=vp9-profile2 \
LIBVA_DRIVER_NAME=rockchip \
LIBVA_DRIVERS_PATH=$DRIVER_DIR \
RK_VAAPI_LOG=$pinned_log \
run_ffmpeg -nostdin -y -v error \
    -hwaccel vaapi -hwaccel_output_format vaapi \
    -vaapi_device "$RENDER_NODE" -i "$PINNED_VECTOR" -an \
    -vf 'hwdownload,format=p010le' -pix_fmt p010le \
    -fps_mode passthrough -f rawvideo "$pinned_hardware"

dimensions=$("$FFPROBE" -v error -select_streams v:0 \
    -show_entries stream=width,height -of csv=s=x:p=0 "$PINNED_VECTOR")
case $dimensions in
    *x*) ;;
    *) echo "FAIL  could not determine pinned Profile 2 dimensions" >&2; exit 1 ;;
esac
pinned_width=${dimensions%x*}
pinned_height=${dimensions#*x}
for value in "$pinned_width" "$pinned_height"; do
    case $value in
        ''|*[!0-9]*|0)
            echo "FAIL  pinned Profile 2 dimensions are invalid: $dimensions" >&2
            exit 1
            ;;
    esac
done
pinned_frame_size=$((pinned_width * pinned_height * 3))
pinned_software_size=$(wc -c <"$pinned_software")
pinned_hardware_size=$(wc -c <"$pinned_hardware")
if [ "$pinned_frame_size" -eq 0 ] ||
   [ $((pinned_software_size % pinned_frame_size)) -ne 0 ]; then
    echo "FAIL  pinned Profile 2 software output has an invalid size" >&2
    exit 1
fi
pinned_frames=$((pinned_software_size / pinned_frame_size))
if [ "$pinned_frames" -eq 0 ] ||
   [ "$pinned_hardware_size" -ne "$pinned_software_size" ] ||
   ! cmp -s "$pinned_software" "$pinned_hardware"; then
    echo "FAIL  pinned Profile 2 P010 output differs from software reference" >&2
    exit 1
fi

pinned_conversions=$(grep -c 'convert: NV15->P010.*afbc=1' "$pinned_log" || true)
if [ "$pinned_conversions" -ne "$PINNED_CONVERSIONS" ] ||
   ! grep -q 'CreateContext: 10-bit output mode=AFBC_V2 profile=21' "$pinned_log" ||
   grep -q 'external buffer mismatch\|decode failed\|afbc=0' "$pinned_log"; then
    echo "FAIL  pinned Profile 2 AFBC/RGA audit failed (conversions=$pinned_conversions expected=$PINNED_CONVERSIONS)" >&2
    exit 1
fi

echo "ok    VP9 Profile 2 vp92-2-20-10bit-yuv420.webm ${pinned_width}x${pinned_height} $pinned_frames displayed frames bit-exact ($pinned_conversions decoded frames)"
