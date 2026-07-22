#!/bin/sh
# Prove that two active VA-API decode contexts are correct in one process.

set -eu

FFMPEG=${FFMPEG:-ffmpeg}
SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH='' cd -- "$SCRIPT_DIR/.." && pwd)
DRIVER_DIR=${DRIVER_DIR:-$REPO_ROOT}
RENDER_NODE=${RENDER_NODE:-/dev/dri/renderD128}
KEEP_WORK=${KEEP_WORK:-0}
WORK=$(mktemp -d "$REPO_ROOT/.test-work.concurrent.XXXXXX") || exit 1
LOG=$WORK/driver.log

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

export LIBVA_DRIVER_NAME=rockchip
export LIBVA_DRIVERS_PATH="$DRIVER_DIR"

generate()
{
    output=$1
    shift
    "$FFMPEG" -nostdin -y -v error -f lavfi \
        -i testsrc2=size=1280x720:rate=30:duration=4 \
        "$@" -pix_fmt yuv420p "$WORK/$output" \
        >"$WORK/$output.log" 2>&1
}

software_md5()
{
    "$FFMPEG" -nostdin -y -v error -i "$1" -an \
        -vf format=yuv420p -f framemd5 "$2" >"$2.log" 2>&1
}

generate h264.mp4 -c:v libx264 -profile:v high \
    -x264-params ref=4:bframes=3
generate vp9.webm -c:v libvpx-vp9 -b:v 1M
software_md5 "$WORK/h264.mp4" "$WORK/h264.sw.md5"
software_md5 "$WORK/vp9.webm" "$WORK/vp9.sw.md5"

# Both inputs are processed by this single FFmpeg process. The driver-log peak
# below proves that both context workers overlap. TSan downloads NV12 directly
# to rawvideo sinks, avoiding unrelated FFmpeg swscale/frame-hash pipelines;
# normal and ASan/UBSan runs retain the bit-exact output comparison.
case ${CONCURRENT_OUTPUT_MODE:-md5} in
    md5)
        RK_VAAPI_LOG=$LOG LD_PRELOAD=${HW_LD_PRELOAD:-} \
            "$FFMPEG" -nostdin -y -v error \
            -hwaccel vaapi -hwaccel_output_format vaapi \
            -vaapi_device "$RENDER_NODE" -threads 1 -i "$WORK/h264.mp4" \
            -hwaccel vaapi -hwaccel_output_format vaapi \
            -vaapi_device "$RENDER_NODE" -threads 1 -i "$WORK/vp9.webm" \
            -filter_complex_threads 2 \
            -filter_complex \
                '[0:v:0]hwdownload,format=nv12,format=yuv420p[h264];[1:v:0]hwdownload,format=nv12,format=yuv420p[vp9]' \
            -map '[h264]' -an -f framemd5 "$WORK/h264.hw.md5" \
            -map '[vp9]' -an -f framemd5 "$WORK/vp9.hw.md5" \
            >"$WORK/hardware.log" 2>&1

        if ! cmp -s "$WORK/h264.sw.md5" "$WORK/h264.hw.md5"; then
            echo "error: concurrent H.264 decode differs from software" >&2
            exit 1
        fi
        if ! cmp -s "$WORK/vp9.sw.md5" "$WORK/vp9.hw.md5"; then
            echo "error: concurrent VP9 decode differs from software" >&2
            exit 1
        fi
        ;;
    download)
        RK_VAAPI_LOG=$LOG LD_PRELOAD=${HW_LD_PRELOAD:-} \
            "$FFMPEG" -nostdin -y -v error \
            -hwaccel vaapi -hwaccel_output_format vaapi \
            -vaapi_device "$RENDER_NODE" -threads 1 -i "$WORK/h264.mp4" \
            -hwaccel vaapi -hwaccel_output_format vaapi \
            -vaapi_device "$RENDER_NODE" -threads 1 -i "$WORK/vp9.webm" \
            -filter_complex_threads 2 \
            -filter_complex \
                '[0:v:0]hwdownload,format=nv12[h264];[1:v:0]hwdownload,format=nv12[vp9]' \
            -map '[h264]' -an -threads 1 -c:v rawvideo -f rawvideo /dev/null \
            -map '[vp9]' -an -threads 1 -c:v rawvideo -f rawvideo /dev/null \
            >"$WORK/hardware.log" 2>&1
        ;;
    *)
        echo "error: CONCURRENT_OUTPUT_MODE must be md5 or download" >&2
        exit 2
        ;;
esac

ready_count=$(awk '/external_group: ready/ { count++ } END { print count + 0 }' "$LOG")
destroyed_count=$(awk '/external_group: destroyed/ { count++ } END { print count + 0 }' "$LOG")
frame_count=$(awk '/zero_copy=1 external=1/ { count++ } END { print count + 0 }' "$LOG")
worker_started=$(awk '/decode worker: started/ { count++ } END { print count + 0 }' "$LOG")
worker_stopped=$(awk '/decode worker: stopped/ { count++ } END { print count + 0 }' "$LOG")
peak_workers=$(awk '
    /decode worker: started/ { active++; if (active > peak) peak = active }
    /decode worker: stopped/ { active-- }
    END { print peak + 0 }
' "$LOG")
expected_frames=$(awk '
    !/^#/ { count++ }
    END { print count + 0 }
' "$WORK/h264.sw.md5" "$WORK/vp9.sw.md5")
bad_count=$(awk '
    /mode=internal-fallback/ || /zero_copy=0/ || /copied=1/ ||
    /external buffer mismatch/ || /unsafe internal layout/ ||
    /has no pending route/ || /submission failed/ ||
    /output wait failed/ || /SyncSurface: .*draining MPP/ { count++ }
    END { print count + 0 }
' "$LOG")

if [ "$ready_count" -ne 2 ] || [ "$destroyed_count" -ne 2 ]; then
    echo "error: expected two clean external-pool lifecycles; ready=$ready_count destroyed=$destroyed_count" >&2
    exit 1
fi
if [ "$worker_started" -ne 2 ] || [ "$worker_stopped" -ne 2 ] ||
   [ "$peak_workers" -lt 2 ]; then
    echo "error: workers did not overlap cleanly; started=$worker_started stopped=$worker_stopped peak=$peak_workers" >&2
    exit 1
fi
if [ "$frame_count" -ne "$expected_frames" ]; then
    echo "error: expected $expected_frames external frames, observed $frame_count" >&2
    exit 1
fi
if [ "$bad_count" -ne 0 ]; then
    echo "error: decode fallback or ownership failure found in driver log" >&2
    exit 1
fi

echo "concurrent decode gate: OK (2 contexts, $frame_count frames, peak workers=$peak_workers)"
