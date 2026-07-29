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
# applied to a Firefox source build. The default gate therefore runs with
# MOZ_DISABLE_RDD_SANDBOX=1 and proves the decode path, not the sandbox story.
# Set FIREFOX_RDD_SANDBOX=enabled for a patched build; that mode unsets the
# bypass and requires the live RDD process to report seccomp filter mode 2.
#
# Environment:
#   FFMPEG FIREFOX DRIVER_DIR PLAY_SECONDS MIN_FRAMES FIREFOX_CASES
#   FIREFOX_RDD_SANDBOX KEEP_WORK DISPLAY WAYLAND_DISPLAY

set -eu

FFMPEG=${FFMPEG:-ffmpeg}
FIREFOX=${FIREFOX:-firefox}
PLAY_SECONDS=${PLAY_SECONDS:-30}
MIN_FRAMES=${MIN_FRAMES:-120}
FIREFOX_CASES=${FIREFOX_CASES:-h264,hevc,hevc-main10}
FIREFOX_RDD_SANDBOX=${FIREFOX_RDD_SANDBOX:-disabled}
KEEP_WORK=${KEEP_WORK:-0}
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
case $FIREFOX_RDD_SANDBOX in
    disabled)
        export MOZ_DISABLE_RDD_SANDBOX=1
        ;;
    enabled)
        unset MOZ_DISABLE_RDD_SANDBOX
        if ! command -v pgrep >/dev/null 2>&1; then
            echo "error: pgrep is required for the RDD sandbox audit" >&2
            exit 2
        fi
        ;;
    *)
        echo "error: FIREFOX_RDD_SANDBOX must be enabled or disabled" >&2
        exit 2
        ;;
esac
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
case ,${MOZ_LOG:-}, in
    *,Dmabuf:*)
        ;;
    *)
        MOZ_LOG=${MOZ_LOG:+$MOZ_LOG,}Dmabuf:4
        export MOZ_LOG
        ;;
esac

WORK=$(mktemp -d "$REPO_ROOT/.test-work.firefox-decode.XXXXXX") || exit 1
# shellcheck disable=SC2317,SC2329 # Invoked by the EXIT trap.
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

find_rdd_descendant()
{
    parent=$1
    for child in $(pgrep -P "$parent" 2>/dev/null || true); do
        process_name=$(
            (sed -n '1p' "/proc/$child/comm") 2>/dev/null || true
        )
        if [ "$process_name" = "RDD Process" ]; then
            echo "$child"
            return 0
        fi
        descendant=$(find_rdd_descendant "$child" || true)
        if [ -n "$descendant" ]; then
            echo "$descendant"
            return 0
        fi
    done
    return 1
}

audit_rdd_sandbox()
{
    root=$1
    attempts=0
    attempt_limit=$((PLAY_SECONDS * 10))
    while [ "$attempts" -lt "$attempt_limit" ]; do
        rdd_pid=$(find_rdd_descendant "$root" || true)
        if [ -n "$rdd_pid" ]; then
            seccomp=$(awk '$1 == "Seccomp:" { print $2 }' \
                           "/proc/$rdd_pid/status" 2>/dev/null || true)
            if [ "$seccomp" = 2 ]; then
                echo "note  RDD sandbox active (pid=$rdd_pid seccomp=$seccomp)"
                return 0
            fi
            if [ -n "$seccomp" ]; then
                echo "FAIL  RDD process is not seccomp-filtered " \
                     "(pid=$rdd_pid seccomp=$seccomp)"
                return 1
            fi
        fi
        attempts=$((attempts + 1))
        sleep 0.1
    done
    echo "FAIL  could not observe the sandboxed RDD process"
    return 1
}

play()
{
    label=$1
    encoder=$2
    profile=$3
    pixel_format=$4
    profiles=$5
    expected_format=$6
    case ",$FIREFOX_CASES," in
        *",$label,"*)
            ;;
        *)
            return
            ;;
    esac
    clip=$WORK/$label.mp4
    page=$WORK/$label.html
    driver_log=$WORK/$label.driver.log

    "$FFMPEG" -nostdin -y -v error -f lavfi \
        -i testsrc2=size=1280x720:rate=30:duration=8 \
        -c:v "$encoder" -profile:v "$profile" -pix_fmt "$pixel_format" \
        -movflags +faststart "$clip" >"$WORK/$label.encode.log" 2>&1
    cat >"$page" <<PAGE
<!doctype html><meta charset=utf-8><title>rockchip-vaapi</title>
<body style="margin:0;background:#000">
<video src="$label.mp4" autoplay muted loop playsinline
       width=1280 height=720></video>
PAGE

    # Firefox does not exit on its own; play for a fixed window and kill it.
    RK_VAAPI_EXPERIMENTAL_PROFILES=$profiles \
        RK_VAAPI_LOG=$driver_log \
        timeout --kill-after=5s "$PLAY_SECONDS" \
        "$FIREFOX" --profile "$WORK/profile" --new-instance \
        "file://$page" >"$WORK/$label.firefox.log" 2>&1 &
    browser_job=$!
    if [ "$FIREFOX_RDD_SANDBOX" = enabled ] &&
       ! audit_rdd_sandbox "$browser_job"; then
        FAIL=1
    fi
    if wait "$browser_job"; then
        :
    fi

    if [ ! -s "$driver_log" ]; then
        echo "FAIL  $label (Firefox never loaded this driver)"
        FAIL=1
        return
    fi

    frames=$(awk '/external=1/ &&
                  (/zero_copy=1/ || /converted_10bit=1/) { count++ }
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

    if [ "$expected_format" = P010 ]; then
        conversions=$(grep -c 'convert: NV15->P010.*afbc=1' \
                           "$driver_log" || true)
        p010_exports=$(grep -c \
            'ExportSurfaceHandle: surface=.*decoded=1.*10bit=1' \
            "$driver_log" || true)
        if [ "$conversions" -lt "$MIN_FRAMES" ] ||
           [ "$p010_exports" -lt 1 ] ||
           ! grep -q 'CreateContext: 10-bit output mode=AFBC_V2 profile=18' \
                   "$driver_log"; then
            echo "FAIL  $label (P010 audit conversions=$conversions exports=$p010_exports)"
            FAIL=1
            return
        fi
        if [ "$FIREFOX_RDD_SANDBOX" = enabled ] &&
           ! grep -q 'EGL error EGL_BAD_MATCH; retrying swapped chroma format' \
                   "$WORK/$label.firefox.log"; then
            echo "FAIL  $label (missing Panfrost swapped-chroma retry audit)"
            FAIL=1
            return
        fi
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
if [ "$FIREFOX_RDD_SANDBOX" = enabled ]; then
    echo "note  RDD sandbox required for this run"
else
    echo "note  RDD sandbox disabled for this run; see contrib/firefox"
fi
play h264 libx264 high yuv420p "" NV12
play hevc libx265 main yuv420p "" NV12
play hevc-main10 libx265 main10 yuv420p10le hevc-main10 P010

if [ "$FAIL" -ne 0 ]; then
    echo "FAILURES PRESENT"
    exit 1
fi
echo "ALL GREEN"
