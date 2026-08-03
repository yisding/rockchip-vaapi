#!/bin/sh
# Repeated exactness discriminator for small HEVC Main10 AFBC-to-P010 RGA jobs.

set -eu

FFMPEG=${FFMPEG:-/usr/bin/ffmpeg}
FFMPEG_TIMEOUT=${FFMPEG_TIMEOUT:-120}
SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH='' cd -- "$SCRIPT_DIR/.." && pwd)
DRIVER_DIR=${DRIVER_DIR:-$REPO_ROOT}
RENDER_NODE=${RENDER_NODE:-/dev/dri/renderD128}
KEEP_WORK=${KEEP_WORK:-0}
FRAMES=${RGA_REPEAT_FRAMES:-48}
FPS=${RGA_REPEAT_FPS:-24}
SMALL_RUNS=${RGA_REPEAT_SMALL_RUNS:-10}
CONTROL_RUNS=${RGA_REPEAT_CONTROL_RUNS:-2}
CONCURRENT_ROUNDS=${RGA_REPEAT_CONCURRENT_ROUNDS:-2}
CONCURRENT_WORKERS=${RGA_REPEAT_CONCURRENT_WORKERS:-4}

for command in "$FFMPEG" timeout journalctl awk cmp grep sed; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "error: required command not found: $command" >&2
        exit 2
    fi
done
for value in "$FFMPEG_TIMEOUT" "$FRAMES" "$FPS" "$SMALL_RUNS" \
             "$CONCURRENT_WORKERS"; do
    case $value in
        ''|*[!0-9]*|0)
            echo "error: timeout, frames, rate, small runs, and workers must be positive integers" >&2
            exit 2
            ;;
    esac
done
for value in "$CONTROL_RUNS" "$CONCURRENT_ROUNDS"; do
    case $value in
        ''|*[!0-9]*)
            echo "error: control runs and concurrent rounds must be non-negative integers" >&2
            exit 2
            ;;
    esac
done
if [ ! -x "$DRIVER_DIR/rockchip_drv_video.so" ]; then
    echo "error: driver is missing from $DRIVER_DIR; run make first" >&2
    exit 2
fi
if [ ! -e "$RENDER_NODE" ]; then
    echo "error: render node is missing: $RENDER_NODE" >&2
    exit 2
fi
if ! "$FFMPEG" -hide_banner -hwaccels 2>/dev/null |
     grep -qx '[[:space:]]*vaapi[[:space:]]*'; then
    echo "error: FFmpeg lacks VA-API support" >&2
    exit 2
fi

WORK=$(mktemp -d "$REPO_ROOT/.test-work.rga-repeat.XXXXXX") || exit 1
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
    timeout --kill-after=5s "$FFMPEG_TIMEOUT" "$FFMPEG" "$@"
}

generate_case()
{
    label=$1
    geometry=$2
    input=$WORK/$label.mkv
    reference=$WORK/$label.reference.framemd5

    run_ffmpeg -nostdin -y -v error -f lavfi \
        -i "testsrc2=size=${geometry}:rate=${FPS}" \
        -frames:v "$FRAMES" -an -vf format=yuv420p10le \
        -c:v libx265 -preset ultrafast -crf 28 \
        -x265-params "profile=main10:keyint=${FPS}:min-keyint=${FPS}:scenecut=0:bframes=0:log-level=error" \
        "$input" >"$WORK/$label.generate.log" 2>&1
    run_ffmpeg -nostdin -y -v error -i "$input" -an \
        -frames:v "$FRAMES" -vf format=p010le -pix_fmt p010le \
        -fps_mode passthrough -f framemd5 "$reference" \
        >"$WORK/$label.reference.log" 2>&1

    reference_frames=$(awk '!/^#/ { count++ } END { print count + 0 }' \
        "$reference")
    if [ "$reference_frames" -ne "$FRAMES" ]; then
        echo "FAIL  $label software reference has $reference_frames frames, expected $FRAMES" >&2
        exit 1
    fi
}

run_case()
{
    label=$1
    run_id=$2
    input=$WORK/$label.mkv
    reference=$WORK/$label.reference.framemd5
    hardware=$WORK/$label.$run_id.hardware.framemd5
    driver_log=$WORK/$label.$run_id.driver.log
    ffmpeg_log=$WORK/$label.$run_id.ffmpeg.log

    if ! RK_VAAPI_EXPERIMENTAL_PROFILES=hevc-main10 \
         LIBVA_DRIVER_NAME=rockchip \
         LIBVA_DRIVERS_PATH=$DRIVER_DIR \
         RK_VAAPI_LOG=$driver_log \
         run_ffmpeg -nostdin -y -v error \
            -hwaccel vaapi -hwaccel_output_format vaapi \
            -vaapi_device "$RENDER_NODE" -i "$input" -an \
            -frames:v "$FRAMES" -vf 'hwdownload,format=p010le' \
            -pix_fmt p010le -fps_mode passthrough -f framemd5 "$hardware" \
            >"$ffmpeg_log" 2>&1; then
        echo "FAIL  $label $run_id FFmpeg hardware decode failed" >&2
        tail -20 "$ffmpeg_log" >&2 || true
        return 1
    fi

    conversions=$(grep -c 'convert: NV15->P010.*afbc=1' "$driver_log" || true)
    assigned=$(grep -c \
        'assign_mpp_frame: surface=.*converted_10bit=1.*external=1' \
        "$driver_log" || true)
    canceled=$(grep -c \
        'assign_mpp_frame: output canceled.*converted_10bit=1' \
        "$driver_log" || true)
    hardware_frames=$(awk '!/^#/ { count++ } END { print count + 0 }' \
        "$hardware")
    if [ "$hardware_frames" -ne "$FRAMES" ] ||
       [ "$conversions" -ne "$FRAMES" ] ||
       [ "$assigned" -ne "$FRAMES" ] ||
       [ "$canceled" -ne 0 ] ||
       ! cmp -s "$reference" "$hardware" ||
       ! grep -q 'CreateContext: 10-bit output mode=AFBC_V2 profile=18' \
            "$driver_log" ||
       grep -Eq 'external buffer mismatch|decode failed|afbc=0|RGA .*failed' \
            "$driver_log"; then
        echo "FAIL  $label $run_id exactness/audit frames=$hardware_frames conversions=$conversions assigned=$assigned canceled=$canceled" >&2
        return 1
    fi

    echo "ok    $label $run_id: $FRAMES P010 frames exact"
}

generate_case small-320x240 320x240
generate_case small-416x240 416x240
generate_case control-1280x720 1280x720

cursor=$(journalctl -k -n 0 --show-cursor --no-pager |
         sed -n 's/^-- cursor: //p')
if [ -z "$cursor" ]; then
    echo "FAIL  could not capture the kernel journal cursor" >&2
    exit 1
fi

iteration=1
while [ "$iteration" -le "$SMALL_RUNS" ]; do
    run_case small-320x240 "sequential-$iteration"
    run_case small-416x240 "sequential-$iteration"
    iteration=$((iteration + 1))
done

iteration=1
while [ "$iteration" -le "$CONTROL_RUNS" ]; do
    run_case control-1280x720 "sequential-$iteration"
    iteration=$((iteration + 1))
done

round=1
while [ "$round" -le "$CONCURRENT_ROUNDS" ]; do
    pids=
    worker=1
    while [ "$worker" -le "$CONCURRENT_WORKERS" ]; do
        if [ $((worker % 2)) -eq 0 ]; then
            label=small-416x240
        else
            label=small-320x240
        fi
        run_case "$label" "concurrent-${round}-worker-$worker" &
        pids="$pids $!"
        worker=$((worker + 1))
    done
    round_failed=0
    for pid in $pids; do
        if ! wait "$pid"; then
            round_failed=1
        fi
    done
    if [ "$round_failed" -ne 0 ]; then
        echo "FAIL  concurrent round $round failed" >&2
        exit 1
    fi
    round=$((round + 1))
done

kernel_log=$WORK/kernel.log
journalctl -k --after-cursor "$cursor" --no-pager -o cat >"$kernel_log"
kernel_error_re='BUG:|Oops:|KASAN:|kernel panic|IOMMU.*(fault|page fault)|rga.*(no core match|failed|failure|time.?out|mmu.*fault)'
if grep -Eiq "$kernel_error_re" "$kernel_log"; then
    echo "FAIL  RGA/IOMMU/kernel-fatal signature appeared during repeat gate" >&2
    grep -Ein "$kernel_error_re" "$kernel_log" >&2 || true
    exit 1
fi

small_runs=$((SMALL_RUNS * 2 + CONCURRENT_ROUNDS * CONCURRENT_WORKERS))
total_runs=$((small_runs + CONTROL_RUNS))
total_frames=$((total_runs * FRAMES))
echo "ok    repeated small-geometry RGA gate: $small_runs small runs, $CONTROL_RUNS control runs, $total_frames exact frames, clean kernel journal"
