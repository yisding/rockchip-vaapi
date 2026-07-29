#!/bin/sh
# VLC hardware-decode gate, in a real display session.
#
# VLC's OpenGL VA-API converters are the whole point of this gate: they derive
# an image from the decoded surface, take its buffer handle as a DRM PRIME fd,
# and import that as an EGLImage. A headless run never reaches any of that --
# VLC reports "no hw decoder modules matched", falls back to software, and
# never loads this driver at all. Passing headlessly would therefore be
# evidence of nothing, so this script refuses to run without a display.
#
# Environment:
#   FFMPEG VLC DRIVER_DIR RENDER_NODE VLC_TIMEOUT DISPLAY WAYLAND_DISPLAY

set -eu

FFMPEG=${FFMPEG:-ffmpeg}
VLC=${VLC:-vlc}
VLC_TIMEOUT=${VLC_TIMEOUT:-120}
SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH='' cd -- "$SCRIPT_DIR/.." && pwd)
DRIVER_DIR=${DRIVER_DIR:-$REPO_ROOT}
RENDER_NODE=${RENDER_NODE:-/dev/dri/renderD128}
MIN_FRAMES=${MIN_FRAMES:-100}

case $VLC_TIMEOUT in
    ''|*[!0-9]*)
        echo "error: VLC_TIMEOUT must be a non-negative integer" >&2
        exit 2 ;;
esac
for tool in "$FFMPEG" "$VLC"; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "error: $tool not found" >&2
        exit 2
    fi
done
if [ ! -e "$RENDER_NODE" ]; then
    echo "error: render node $RENDER_NODE is missing" >&2
    exit 2
fi
if [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
    echo "error: no display session; set DISPLAY or WAYLAND_DISPLAY" >&2
    echo "error: headless VLC silently falls back to software decode" >&2
    exit 2
fi

export LIBVA_DRIVER_NAME=rockchip
export LIBVA_DRIVERS_PATH="$DRIVER_DIR"

WORK=$(mktemp -d "$REPO_ROOT/.test-work.vlc-display.XXXXXX") || exit 1
# shellcheck disable=SC2317,SC2329 # Invoked by the EXIT trap.
cleanup()
{
    rm -rf "$WORK"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

FAIL=0
# Deliberately not listed: the teardown-drain note. VLC stops mid-stream and
# destroys its decoder with the last pictures still in flight, which it never
# intends to consume; the driver reports that and fails their fences rather
# than waiting forever. The backend-stall warning IS listed, because that one
# means the hardware stopped responding during playback.
ERROR_MARKERS='decode failed|buffer mismatch|unsafe internal layout'
ERROR_MARKERS="$ERROR_MARKERS|has no pending route|produced no output for"
ERROR_MARKERS="$ERROR_MARKERS|pool exhausted|reconstruction failed"

generate()
{
    "$FFMPEG" -nostdin -y -v error -f lavfi \
        -i testsrc2=size=1280x720:rate=30:duration=4 \
        -c:v "$1" -profile:v "$2" -pix_fmt yuv420p "$3" \
        >"$WORK/generate.log" 2>&1
}

play()
{
    label=$1
    clip=$2
    log=$WORK/$label.vlc.log
    driver_log=$WORK/$label.driver.log

    if ! RK_VAAPI_LOG=$driver_log timeout --kill-after=5s "$VLC_TIMEOUT" \
            "$VLC" -I dummy --no-audio --avcodec-hw=vaapi --play-and-exit \
            -vvv "$clip" >"$log" 2>&1; then
        echo "FAIL  $label (VLC exited non-zero)"
        tail -20 "$log"
        FAIL=1
        return
    fi

    if ! grep -q 'using hw decoder module "vaapi"' "$log"; then
        echo "FAIL  $label (VLC did not select its VA-API hardware decoder)"
        grep -iE 'hw decoder|glconv' "$log" | tail -10
        FAIL=1
        return
    fi
    if ! grep -q 'Using Rockchip MPP VA-API Driver .* for hardware decoding' \
            "$log"; then
        echo "FAIL  $label (VLC did not report this driver for decoding)"
        FAIL=1
        return
    fi
    if ! grep -q 'rk_DeriveImage' "$driver_log"; then
        echo "FAIL  $label (no vaDeriveImage; the GL converter never bound)"
        FAIL=1
        return
    fi

    frames=$(awk '/zero_copy=1/ && /external=1/ { count++ }
                  END { print count + 0 }' "$driver_log")
    if [ "$frames" -lt "$MIN_FRAMES" ]; then
        echo "FAIL  $label (only $frames external frames, expected >= $MIN_FRAMES)"
        FAIL=1
        return
    fi

    bad=$(grep -cE "$ERROR_MARKERS" "$driver_log" || true)
    if [ "$bad" -ne 0 ]; then
        echo "FAIL  $label ($bad driver error markers)"
        grep -nE "$ERROR_MARKERS" "$driver_log" | head -5
        FAIL=1
        return
    fi

    echo "ok    $label hardware-decoded $frames frames through VLC"
}

echo "== VLC display-session hardware decode =="
generate libx264 high "$WORK/h264.mp4"
play h264 "$WORK/h264.mp4"
generate libx265 main "$WORK/hevc.mp4"
play hevc "$WORK/hevc.mp4"

if [ "$FAIL" -ne 0 ]; then
    echo "FAILURES PRESENT"
    exit 1
fi
echo "ALL GREEN"
