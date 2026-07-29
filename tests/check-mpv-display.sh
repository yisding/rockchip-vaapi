#!/bin/sh
# mpv VA-API decode and Wayland/EGL presentation gate.

set -eu

FFMPEG=${FFMPEG:-ffmpeg}
MPV=${MPV:-mpv}
MPV_TIMEOUT=${MPV_TIMEOUT:-60}
MIN_FRAMES=${MIN_FRAMES:-20}
SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH='' cd -- "$SCRIPT_DIR/.." && pwd)
DRIVER_DIR=${DRIVER_DIR:-$REPO_ROOT}
RENDER_NODE=${RENDER_NODE:-/dev/dri/renderD128}

for value in "$MPV_TIMEOUT" "$MIN_FRAMES"; do
    case $value in
        ''|*[!0-9]*)
            echo "error: timeouts and frame counts must be integers" >&2
            exit 2 ;;
    esac
done
for tool in "$FFMPEG" "$MPV" timeout; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "error: required command not found: $tool" >&2
        exit 2
    fi
done
if [ ! -e "$RENDER_NODE" ]; then
    echo "error: render node $RENDER_NODE is missing" >&2
    exit 2
fi
if [ -z "${WAYLAND_DISPLAY:-}" ]; then
    echo "error: no Wayland session; set WAYLAND_DISPLAY" >&2
    echo "error: this gate must exercise Panfrost EGL DMA-BUF import" >&2
    exit 2
fi

WORK=$(mktemp -d "$REPO_ROOT/.test-work.mpv-display.XXXXXX") || exit 1
# shellcheck disable=SC2317,SC2329 # Invoked by the EXIT trap.
cleanup()
{
    rm -rf "$WORK"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

clip=$WORK/h264-cif.mp4
mpv_log=$WORK/mpv.log
driver_log=$WORK/driver.log

"$FFMPEG" -nostdin -y -v error -f lavfi \
    -i "testsrc2=size=352x288:rate=25" -frames:v "$MIN_FRAMES" \
    -c:v libx264 -profile:v high -pix_fmt yuv420p "$clip"

export LIBVA_DRIVER_NAME=rockchip
export LIBVA_DRIVERS_PATH="$DRIVER_DIR"

if ! RK_VAAPI_LOG=$driver_log timeout --kill-after=5s "$MPV_TIMEOUT" \
        "$MPV" --no-config --input-terminal=no \
        --input-default-bindings=no --osc=no --deinterlace=no \
        --hwdec=vaapi --hwdec-software-fallback=no --vo=gpu-next \
        --gpu-api=opengl --gpu-context=wayland --no-audio \
        --msg-level=all=info "$clip" >"$mpv_log" 2>&1; then
    echo "FAIL  mpv exited non-zero" >&2
    tail -40 "$mpv_log" >&2
    exit 1
fi

if ! grep -q 'Using hardware decoding (vaapi)' "$mpv_log" ||
   ! grep -q 'VO: \[gpu-next\] 352x288 vaapi\[nv12\]' "$mpv_log"; then
    echo "FAIL  mpv did not select VA-API decode and gpu-next presentation" >&2
    grep -E 'Using .*decoding|VO:' "$mpv_log" >&2 || true
    exit 1
fi

APP_ERRORS='WSI pitch not properly aligned|mapping VAAPI EGL image failed'
APP_ERRORS="$APP_ERRORS|Mapping hardware decoded surface failed"
APP_ERRORS="$APP_ERRORS|Failed rendering frame|HW-downloading from vaapi"
if grep -qE "$APP_ERRORS" "$mpv_log"; then
    echo "FAIL  mpv/EGL presentation error" >&2
    grep -nE "$APP_ERRORS" "$mpv_log" | head -10 >&2
    exit 1
fi

info_changes=$(grep -c 'assign_mpp_frame: info_change' "$driver_log" || true)
decoded=$(grep -c 'assign_mpp_frame: surface=.*zero_copy=1.*external=1' \
                  "$driver_log" || true)
repacks=$(grep -c 'convert: NV12 repack 352x288 352x288 -> 384x288' \
                  "$driver_log" || true)
aligned_exports=$(grep -c \
    'ExportSurfaceHandle: surface=.*352x288 stride=384x288.*decoded=1' \
    "$driver_log" || true)
if [ "$info_changes" -ne 1 ] || [ "$decoded" -lt "$MIN_FRAMES" ] ||
   [ "$repacks" -lt "$MIN_FRAMES" ] ||
   [ "$aligned_exports" -lt "$MIN_FRAMES" ] ||
   grep -q 'ExportSurfaceHandle: surface=.*352x288 stride=352x288.*decoded=1' \
           "$driver_log" ||
   grep -qE 'RGA NV12 repack failed|external buffer mismatch|unsafe internal layout' \
           "$driver_log"; then
    echo "FAIL  driver audit: info_changes=$info_changes decoded=$decoded repacks=$repacks aligned_exports=$aligned_exports" >&2
    exit 1
fi

echo "ok    mpv rendered $MIN_FRAMES H.264 VA-API frames through Panfrost EGL"
echo "ok    decoded=$decoded repacks=$repacks info_changes=$info_changes"
echo "ALL GREEN"
