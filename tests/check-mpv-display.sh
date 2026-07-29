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

h264_clip=$WORK/h264-cif.mp4
h264_mpv_log=$WORK/h264.mpv.log
h264_driver_log=$WORK/h264.driver.log

"$FFMPEG" -nostdin -y -v error -f lavfi \
    -i "testsrc2=size=352x288:rate=25" -frames:v "$MIN_FRAMES" \
    -c:v libx264 -profile:v high -pix_fmt yuv420p "$h264_clip"

export LIBVA_DRIVER_NAME=rockchip
export LIBVA_DRIVERS_PATH="$DRIVER_DIR"

if ! RK_VAAPI_LOG=$h264_driver_log \
        timeout --kill-after=5s "$MPV_TIMEOUT" \
        "$MPV" --no-config --input-terminal=no \
        --input-default-bindings=no --osc=no --deinterlace=no \
        --hwdec=vaapi --hwdec-software-fallback=no --vo=gpu-next \
        --gpu-api=opengl --gpu-context=wayland --no-audio \
        --msg-level=all=info "$h264_clip" >"$h264_mpv_log" 2>&1; then
    echo "FAIL  mpv exited non-zero" >&2
    tail -40 "$h264_mpv_log" >&2
    exit 1
fi

if ! grep -q 'Using hardware decoding (vaapi)' "$h264_mpv_log" ||
   ! grep -q 'VO: \[gpu-next\] 352x288 vaapi\[nv12\]' "$h264_mpv_log"; then
    echo "FAIL  mpv did not select VA-API decode and gpu-next presentation" >&2
    grep -E 'Using .*decoding|VO:' "$h264_mpv_log" >&2 || true
    exit 1
fi

APP_ERRORS='WSI pitch not properly aligned|mapping VAAPI EGL image failed'
APP_ERRORS="$APP_ERRORS|Mapping hardware decoded surface failed"
APP_ERRORS="$APP_ERRORS|Failed rendering frame|HW-downloading from vaapi"
if grep -qE "$APP_ERRORS" "$h264_mpv_log"; then
    echo "FAIL  mpv/EGL presentation error" >&2
    grep -nE "$APP_ERRORS" "$h264_mpv_log" | head -10 >&2
    exit 1
fi

info_changes=$(grep -c 'assign_mpp_frame: info_change' \
                      "$h264_driver_log" || true)
decoded=$(grep -c 'assign_mpp_frame: surface=.*zero_copy=1.*external=1' \
                  "$h264_driver_log" || true)
repacks=$(grep -c 'convert: NV12 repack 352x288 352x288 -> 384x288' \
                  "$h264_driver_log" || true)
aligned_exports=$(grep -c \
    'ExportSurfaceHandle: surface=.*352x288 stride=384x288.*decoded=1' \
    "$h264_driver_log" || true)
if [ "$info_changes" -ne 1 ] || [ "$decoded" -lt "$MIN_FRAMES" ] ||
   [ "$repacks" -lt "$MIN_FRAMES" ] ||
   [ "$aligned_exports" -lt "$MIN_FRAMES" ] ||
   grep -q 'ExportSurfaceHandle: surface=.*352x288 stride=352x288.*decoded=1' \
           "$h264_driver_log" ||
   grep -qE 'RGA NV12 repack failed|external buffer mismatch|unsafe internal layout' \
           "$h264_driver_log"; then
    echo "FAIL  driver audit: info_changes=$info_changes decoded=$decoded repacks=$repacks aligned_exports=$aligned_exports" >&2
    exit 1
fi

echo "ok    mpv rendered $MIN_FRAMES H.264 VA-API frames through Panfrost EGL"
echo "ok    decoded=$decoded repacks=$repacks info_changes=$info_changes"

main10_clip=$WORK/hevc-main10-hdr.mkv
main10_mpv_log=$WORK/hevc-main10-hdr.mpv.log
main10_driver_log=$WORK/hevc-main10-hdr.driver.log
"$FFMPEG" -nostdin -y -v error -f lavfi \
    -i "testsrc2=size=320x240:rate=25" -frames:v "$MIN_FRAMES" \
    -vf format=yuv420p10le -c:v libx265 -profile:v main10 \
    -pix_fmt yuv420p10le \
    -x265-params "profile=main10:keyint=25:min-keyint=25:scenecut=0:hdr10=1:repeat-headers=1:colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc:master-display=G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(10000000,1):max-cll=1000,400:log-level=error" \
    "$main10_clip"

# mpv, not the shell, expands the property placeholders.
# shellcheck disable=SC2016
if ! RK_VAAPI_EXPERIMENTAL_PROFILES=hevc-main10 \
        RK_VAAPI_LOG=$main10_driver_log \
        timeout --kill-after=5s "$MPV_TIMEOUT" \
        "$MPV" --no-config --input-terminal=no \
        --input-default-bindings=no --osc=no --deinterlace=no \
        --hwdec=vaapi --hwdec-software-fallback=no --vo=gpu-next \
        --gpu-api=opengl --gpu-context=wayland --no-audio \
        --term-playing-msg='RK_VAAPI_HDR pixfmt=${video-params/pixelformat} primaries=${video-params/primaries} gamma=${video-params/gamma} matrix=${video-params/colormatrix}' \
        --msg-level=all=info "$main10_clip" >"$main10_mpv_log" 2>&1; then
    echo "FAIL  mpv Main10 exited non-zero" >&2
    tail -40 "$main10_mpv_log" >&2
    exit 1
fi

if ! grep -q 'Using hardware decoding (vaapi)' "$main10_mpv_log" ||
   ! grep -qE 'VO: \[gpu-next\] 320x240 vaapi\[p010(le)?\]' \
            "$main10_mpv_log" ||
   ! grep -qE 'RK_VAAPI_HDR .*primaries=bt.2020 .*gamma=pq .*matrix=bt.2020-ncl' \
            "$main10_mpv_log"; then
    echo "FAIL  mpv did not present Main10 P010 with BT.2020/PQ metadata" >&2
    grep -E 'Using .*decoding|VO:|RK_VAAPI_HDR' "$main10_mpv_log" >&2 ||
        true
    exit 1
fi
if grep -qE "$APP_ERRORS" "$main10_mpv_log"; then
    echo "FAIL  mpv Main10/EGL presentation error" >&2
    grep -nE "$APP_ERRORS" "$main10_mpv_log" | head -10 >&2
    exit 1
fi

main10_info_changes=$(grep -c 'assign_mpp_frame: info_change' \
                           "$main10_driver_log" || true)
main10_decoded=$(grep -c \
    'assign_mpp_frame: surface=.*converted_10bit=1.*external=1' \
    "$main10_driver_log" || true)
main10_conversions=$(grep -c 'convert: NV15->P010.*afbc=1' \
                          "$main10_driver_log" || true)
main10_exports=$(grep -c \
    'ExportSurfaceHandle: surface=.*decoded=1.*10bit=1' \
    "$main10_driver_log" || true)
if [ "$main10_info_changes" -ne 1 ] ||
   [ "$main10_decoded" -lt "$MIN_FRAMES" ] ||
   [ "$main10_conversions" -lt "$MIN_FRAMES" ] ||
   [ "$main10_exports" -lt "$MIN_FRAMES" ] ||
   ! grep -q 'CreateContext: 10-bit output mode=AFBC_V2 profile=18' \
           "$main10_driver_log" ||
   grep -qE 'external buffer mismatch|unsafe internal layout|RGA .*failed' \
           "$main10_driver_log"; then
    echo "FAIL  Main10 driver audit: info_changes=$main10_info_changes decoded=$main10_decoded conversions=$main10_conversions exports=$main10_exports" >&2
    exit 1
fi

echo "ok    mpv rendered $MIN_FRAMES HEVC Main10 HDR10 P010 frames through Panfrost EGL"
echo "ok    Main10 decoded=$main10_decoded conversions=$main10_conversions exports=$main10_exports"
echo "note  this proves BT.2020/PQ input presentation, not HDR-monitor passthrough"
echo "ALL GREEN"
