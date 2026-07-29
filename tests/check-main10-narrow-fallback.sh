#!/bin/sh
# Require the 64-pixel Main10 RGA boundary to fail at VA context creation,
# fall back to software decode, and never reach the RGA conversion path.

set -eu

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH='' cd -- "$SCRIPT_DIR/.." && pwd)
DRIVER_DIR=${DRIVER_DIR:-$REPO_ROOT}
FFMPEG=${FFMPEG:-/usr/bin/ffmpeg}
FFPROBE=${FFPROBE:-/usr/bin/ffprobe}
RENDER_NODE=${RENDER_NODE:-/dev/dri/renderD128}
VECTOR=${MAIN10_NARROW_VECTOR:-$SCRIPT_DIR/vectors/hevc-main10-sweep/WPP_D_ericsson_MAIN10_2.bit}
KEEP_WORK=${KEEP_WORK:-0}

for tool in "$FFMPEG" "$FFPROBE" journalctl; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "FAIL  required tool is missing: $tool" >&2
        exit 2
    fi
done
if [ ! -x "$REPO_ROOT/rockchip_drv_video.so" ]; then
    echo "FAIL  driver is missing; run make first" >&2
    exit 2
fi
if [ ! -e "$RENDER_NODE" ]; then
    echo "FAIL  render node is missing: $RENDER_NODE" >&2
    exit 2
fi
if [ ! -f "$VECTOR" ]; then
    echo "FAIL  narrow Main10 vector is missing: $VECTOR" >&2
    exit 2
fi
if ! "$FFMPEG" -hide_banner -hwaccels 2>/dev/null |
     grep -qx '[[:space:]]*vaapi[[:space:]]*'; then
    echo "FAIL  FFmpeg lacks VA-API support" >&2
    exit 2
fi

WORK=$(mktemp -d "$REPO_ROOT/.test-work.main10-narrow.XXXXXX") || exit 1
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

driver_log=$WORK/driver.log
ffmpeg_log=$WORK/ffmpeg.log
kernel_log=$WORK/kernel.log
cursor=$(journalctl -k -n 0 --show-cursor --no-pager |
         sed -n 's/^-- cursor: //p')
if [ -z "$cursor" ]; then
    echo "FAIL  could not capture the kernel journal cursor" >&2
    exit 1
fi

if ! RK_VAAPI_EXPERIMENTAL_PROFILES=hevc-main10 \
     LIBVA_DRIVER_NAME=rockchip \
     LIBVA_DRIVERS_PATH=$DRIVER_DIR \
     RK_VAAPI_LOG=$driver_log \
     "$FFMPEG" -nostdin -v verbose -hwaccel vaapi \
        -vaapi_device "$RENDER_NODE" -i "$VECTOR" -an -f null - \
        >"$ffmpeg_log" 2>&1; then
    echo "FAIL  FFmpeg did not complete through software fallback" >&2
    exit 1
fi
journalctl -k --after-cursor "$cursor" --no-pager -o cat >"$kernel_log"

expected_frames=$(
    "$FFPROBE" -v error -count_frames -select_streams v:0 \
        -show_entries stream=nb_read_frames \
        -of default=nw=1:nk=1 "$VECTOR"
)
case $expected_frames in
    ''|*[!0-9]*|0)
        echo "FAIL  could not count vector frames" >&2
        exit 1
        ;;
esac

rejections=$(grep -c \
    'CreateContext: 10-bit AFBC conversion width=64 is below RGA3 minimum=68' \
    "$driver_log" || true)
conversions=$(grep -c 'convert: NV15->P010' "$driver_log" || true)
kernel_rejections=$(grep -c 'no core match' "$kernel_log" || true)
if [ "$rejections" -ne 1 ] || [ "$conversions" -ne 0 ] ||
   [ "$kernel_rejections" -ne 0 ] ||
   grep -q 'CreateContext: 10-bit output mode=AFBC_V2' "$driver_log" ||
   ! grep -q 'Failed setup for format vaapi: hwaccel initialisation returned error' \
       "$ffmpeg_log" ||
   ! grep -Eq "frame=[[:space:]]*$expected_frames([[:space:]]|$)" "$ffmpeg_log"; then
    echo "FAIL  narrow Main10 fallback audit: frames=$expected_frames " \
         "rejections=$rejections conversions=$conversions " \
         "kernel_no_core_match=$kernel_rejections" >&2
    exit 1
fi

printf 'ok    HEVC Main10 64px context refused up front; %s frames software-decoded; no RGA submission\n' \
    "$expected_frames"
