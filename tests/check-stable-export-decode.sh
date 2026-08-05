#!/bin/sh
# Reproduce Chromium's persistent NativePixmap contract: export before decode,
# retain the DMA-BUF, and require every later frame to appear in that storage.

set -eu

if [ -z "${FFMPEG:-}" ]; then
    if [ -x /usr/bin/ffmpeg ]; then
        FFMPEG=/usr/bin/ffmpeg
    else
        FFMPEG=ffmpeg
    fi
fi

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH='' cd -- "$SCRIPT_DIR/.." && pwd)
DRIVER_DIR=${DRIVER_DIR:-$REPO_ROOT}
FRAMES=${STABLE_EXPORT_FRAMES:-24}
WORK=$(mktemp -d "$REPO_ROOT/.test-work.stable-export.XXXXXX") || exit 1
LOG=$WORK/driver.log
INPUT=$WORK/stable-export.ivf

# shellcheck disable=SC2317,SC2329 # Invoked by the EXIT trap.
cleanup()
{
    rm -rf "$WORK"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

case $FRAMES in
    ''|*[!0-9]*|0)
        echo "error: STABLE_EXPORT_FRAMES must be a positive integer" >&2
        exit 2
        ;;
esac

for command in "$FFMPEG" "$SCRIPT_DIR/va_stable_export_decode" awk grep; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "error: required command not found: $command" >&2
        exit 2
    fi
done

"$FFMPEG" -nostdin -y -v error \
    -f lavfi -i "nullsrc=size=352x288:rate=24" \
    -frames:v "$FRAMES" -an \
    -vf "geq=lum='mod(N*8,256)':cb=128:cr=128,format=yuv420p" \
    -c:v libvpx-vp9 -profile:v 0 -lossless 1 -row-mt 1 \
    -threads 4 -deadline good -cpu-used 4 -f ivf "$INPUT"

LIBVA_DRIVER_NAME=rockchip \
LIBVA_DRIVERS_PATH=$DRIVER_DIR \
RK_VAAPI_LOG=$LOG \
VA_TEST_PREEXPORT=1 \
    "$SCRIPT_DIR/va_stable_export_decode" "$INPUT"

established=$(grep -c 'established stable NV12' "$LOG" || true)
refreshed=$(grep -c 'refreshed stable NV12 export' "$LOG" || true)
assigned=$(grep -c 'stable_export_copy=1' "$LOG" || true)
bad=$(awk '
    /stable export copy failed/ || /decode failed/ ||
    /has no pending route/ || /submission failed/ || /output wait failed/ {
        count++
    }
    END { print count + 0 }
' "$LOG")

if [ "$established" -ne 8 ] || [ "$refreshed" -ne "$FRAMES" ] ||
   [ "$assigned" -ne "$FRAMES" ] || [ "$bad" -ne 0 ]; then
    echo "FAIL  stable export audit: established=$established refreshed=$refreshed assigned=$assigned bad=$bad" >&2
    exit 1
fi

echo "stable pre-decode export gate: OK ($FRAMES VP9 frames through 8 retained DMA-BUFs)"
