#!/bin/sh
# Firefox hardware-decode gate, in a real display session.
#
# This is the browser threat model the driver is built for: the decode runs in
# Firefox's RDD process and the frames are exported as DMA-BUFs for the
# compositor. A headless run never gets there, so this script refuses to run
# without a display.
#
# Sandbox caveat: the stock Firefox binary cannot reach /dev/mpp_service,
# /dev/rga or /dev/dma_heap from a sandboxed RDD process. contrib/firefox holds
# a source patch that adds exactly those broker paths and ioctls, but it must be
# applied to a Firefox source build. This gate therefore runs with
# MOZ_DISABLE_RDD_SANDBOX=1 and proves the decode path, not the sandbox story.
# It records that in its output so the distinction cannot be lost.
#
# Environment:
#   FFMPEG FIREFOX DRIVER_DIR PLAY_SECONDS MIN_FRAMES DISPLAY WAYLAND_DISPLAY

set -eu

FFMPEG=${FFMPEG:-ffmpeg}
FIREFOX=${FIREFOX:-firefox}
PLAY_SECONDS=${PLAY_SECONDS:-30}
MIN_FRAMES=${MIN_FRAMES:-120}
SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH='' cd -- "$SCRIPT_DIR/.." && pwd)
DRIVER_DIR=${DRIVER_DIR:-$REPO_ROOT}
RENDER_NODE=${RENDER_NODE:-/dev/dri/renderD128}

for value in "$PLAY_SECONDS" "$MIN_FRAMES"; do
    case $value in
        ''|*[!0-9]*)
            echo "error: PLAY_SECONDS and MIN_FRAMES must be integers" >&2
            exit 2 ;;
    esac
done
for tool in "$FFMPEG" "$FIREFOX"; do
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
    echo "error: headless Firefox never reaches the hardware decode path" >&2
    exit 2
fi

export LIBVA_DRIVER_NAME=rockchip
export LIBVA_DRIVERS_PATH="$DRIVER_DIR"
export MOZ_DISABLE_RDD_SANDBOX=1

WORK=$(mktemp -d "$REPO_ROOT/.test-work.firefox-decode.XXXXXX") || exit 1
# shellcheck disable=SC2317,SC2329 # Invoked by the EXIT trap.
cleanup()
{
    rm -rf "$WORK"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

FAIL=0
ERROR_MARKERS='decode failed|buffer mismatch|unsafe internal layout'
ERROR_MARKERS="$ERROR_MARKERS|has no pending route|produced no output for"
ERROR_MARKERS="$ERROR_MARKERS|pool exhausted|reconstruction failed"

mkdir -p "$WORK/profile"
cat >"$WORK/profile/user.js" <<'PREFS'
user_pref("media.ffmpeg.vaapi.enabled", true);
user_pref("media.hardware-video-decoding.force-enabled", true);
user_pref("media.rdd-ffmpeg.enabled", true);
user_pref("gfx.webrender.all", true);
user_pref("browser.shell.checkDefaultBrowser", false);
user_pref("browser.startup.homepage_override.mstone", "ignore");
user_pref("datareporting.policy.dataSubmissionEnabled", false);
user_pref("toolkit.telemetry.enabled", false);
PREFS

play()
{
    label=$1
    encoder=$2
    profile=$3
    clip=$WORK/$label.mp4
    page=$WORK/$label.html
    driver_log=$WORK/$label.driver.log

    "$FFMPEG" -nostdin -y -v error -f lavfi \
        -i testsrc2=size=1280x720:rate=30:duration=8 \
        -c:v "$encoder" -profile:v "$profile" -pix_fmt yuv420p \
        -movflags +faststart "$clip" >"$WORK/$label.encode.log" 2>&1
    cat >"$page" <<PAGE
<!doctype html><meta charset=utf-8><title>rockchip-vaapi</title>
<body style="margin:0;background:#000">
<video src="$label.mp4" autoplay muted loop playsinline
       width=1280 height=720></video>
PAGE

    # Firefox does not exit on its own; play for a fixed window and kill it.
    RK_VAAPI_LOG=$driver_log timeout --kill-after=5s "$PLAY_SECONDS" \
        "$FIREFOX" --profile "$WORK/profile" --new-instance \
        "file://$page" >"$WORK/$label.firefox.log" 2>&1 || true

    if [ ! -s "$driver_log" ]; then
        echo "FAIL  $label (Firefox never loaded this driver)"
        FAIL=1
        return
    fi

    frames=$(awk '/zero_copy=1/ && /external=1/ { count++ }
                  END { print count + 0 }' "$driver_log")
    exports=$(grep -c 'ExportSurfaceHandle: surface' "$driver_log" || true)
    if [ "$frames" -lt "$MIN_FRAMES" ]; then
        echo "FAIL  $label (only $frames external frames, expected >= $MIN_FRAMES)"
        FAIL=1
        return
    fi
    if [ "$exports" -lt 1 ]; then
        echo "FAIL  $label (no DMA-BUF export; frames never reached the compositor)"
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

    echo "ok    $label hardware-decoded $frames frames, $exports DMA-BUF exports"
}

echo "== Firefox display-session hardware decode =="
echo "note  RDD sandbox disabled for this run; see contrib/firefox"
play h264 libx264 high
play hevc libx265 main

if [ "$FAIL" -ne 0 ]; then
    echo "FAILURES PRESENT"
    exit 1
fi
echo "ALL GREEN"
