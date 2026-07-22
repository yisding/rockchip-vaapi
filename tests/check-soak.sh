#!/bin/sh
# Long-lived 4K decode soak with RSS, fd, pool, and worker lifecycle audits.

set -eu

FFMPEG=${FFMPEG:-ffmpeg}
SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH='' cd -- "$SCRIPT_DIR/.." && pwd)
DRIVER_DIR=${DRIVER_DIR:-$REPO_ROOT}
RENDER_NODE=${RENDER_NODE:-/dev/dri/renderD128}
SOAK_SECONDS=${SOAK_SECONDS:-7200}
SOAK_SAMPLE_SECONDS=${SOAK_SAMPLE_SECONDS:-30}
SOAK_WARMUP_SECONDS=${SOAK_WARMUP_SECONDS:-60}
SOAK_RSS_SPAN_KB=${SOAK_RSS_SPAN_KB:-65536}
SOAK_RSS_GROWTH_KB=${SOAK_RSS_GROWTH_KB:-32768}
# MP4 stream-loop boundaries briefly reopen demux/decoder resources; the
# end-to-start check below still requires zero sustained fd growth.
SOAK_FD_SPAN=${SOAK_FD_SPAN:-32}
KEEP_WORK=${KEEP_WORK:-0}
WORK=$(mktemp -d "$REPO_ROOT/.test-work.soak.XXXXXX") || exit 1
LOG=$WORK/driver.log
SAMPLES=$WORK/samples.tsv
FFMPEG_PID=

case $SOAK_SECONDS:$SOAK_SAMPLE_SECONDS:$SOAK_WARMUP_SECONDS in
    *[!0-9:]*|:*|*::*)
        echo "error: soak durations must be non-negative integers" >&2
        exit 2
        ;;
esac
if [ "$SOAK_SECONDS" -eq 0 ] || [ "$SOAK_SAMPLE_SECONDS" -eq 0 ] ||
   [ "$SOAK_WARMUP_SECONDS" -ge "$SOAK_SECONDS" ]; then
    echo "error: require positive soak/sample durations and warmup < soak" >&2
    exit 2
fi

# shellcheck disable=SC2317,SC2329 # Invoked by traps.
cleanup()
{
    if [ -n "$FFMPEG_PID" ] && kill -0 "$FFMPEG_PID" 2>/dev/null; then
        kill "$FFMPEG_PID" 2>/dev/null || true
        wait "$FFMPEG_PID" 2>/dev/null || true
    fi
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

echo "generating reusable 4K H.264 soak clip"
"$FFMPEG" -nostdin -y -v error -f lavfi \
    -i testsrc2=size=3840x2160:rate=30:duration=4 \
    -c:v libx264 -profile:v high -x264-params ref=4:bframes=3 \
    -pix_fmt yuv420p "$WORK/soak-4k.mp4" \
    >"$WORK/generate.log" 2>&1

echo "starting paced ${SOAK_SECONDS}s 4K decode soak"
RK_VAAPI_LOG=$LOG "$FFMPEG" -nostdin -y -v error \
    -stream_loop -1 -re -hwaccel vaapi -hwaccel_output_format vaapi \
    -vaapi_device "$RENDER_NODE" -threads 1 -i "$WORK/soak-4k.mp4" \
    -t "$SOAK_SECONDS" -an -vf 'hwdownload,format=nv12' \
    -threads 1 -c:v rawvideo -f rawvideo /dev/null \
    >"$WORK/hardware.log" 2>&1 &
FFMPEG_PID=$!
START_TIME=$(date +%s)

while kill -0 "$FFMPEG_PID" 2>/dev/null; do
    sleep "$SOAK_SAMPLE_SECONDS"
    if ! kill -0 "$FFMPEG_PID" 2>/dev/null; then
        break
    fi
    now=$(date +%s)
    elapsed=$((now - START_TIME))
    rss_kb=$(awk '/^VmRSS:/ { print $2; found=1 } END { if (!found) print 0 }' \
        "/proc/$FFMPEG_PID/status")
    fd_count=$(find "/proc/$FFMPEG_PID/fd" -mindepth 1 -maxdepth 1 \
        2>/dev/null | wc -l)
    printf '%s\t%s\t%s\n' "$elapsed" "$rss_kb" "$fd_count" >>"$SAMPLES"
    echo "soak sample: elapsed=${elapsed}s rss=${rss_kb}KiB fds=$fd_count"
done

set +e
wait "$FFMPEG_PID"
ffmpeg_status=$?
set -e
FFMPEG_PID=
if [ "$ffmpeg_status" -ne 0 ]; then
    echo "error: soak FFmpeg exited with status $ffmpeg_status" >&2
    tail -40 "$WORK/hardware.log" >&2
    exit 1
fi

sample_count=$(awk -v warmup="$SOAK_WARMUP_SECONDS" \
    '$1 >= warmup { count++ } END { print count + 0 }' "$SAMPLES")
if [ "$sample_count" -lt 3 ]; then
    echo "error: only $sample_count post-warmup resource samples" >&2
    exit 1
fi

rss_first=$(awk -v warmup="$SOAK_WARMUP_SECONDS" \
    '$1 >= warmup { print $2; exit }' "$SAMPLES")
rss_last=$(awk -v warmup="$SOAK_WARMUP_SECONDS" \
    '$1 >= warmup { value=$2 } END { print value + 0 }' "$SAMPLES")
rss_min=$(awk -v warmup="$SOAK_WARMUP_SECONDS" \
    '$1 >= warmup && (!seen || $2 < value) { value=$2; seen=1 } END { print value + 0 }' \
    "$SAMPLES")
rss_max=$(awk -v warmup="$SOAK_WARMUP_SECONDS" \
    '$1 >= warmup && (!seen || $2 > value) { value=$2; seen=1 } END { print value + 0 }' \
    "$SAMPLES")
fd_head_median=$(awk -v warmup="$SOAK_WARMUP_SECONDS" '
    $1 >= warmup && count < 3 { values[count++]=$3 }
    END {
        min=max=values[0]; sum=0
        for (i=0; i < 3; i++) {
            if (values[i] < min) min=values[i]
            if (values[i] > max) max=values[i]
            sum += values[i]
        }
        print sum - min - max
    }
' "$SAMPLES")
fd_tail_median=$(awk -v warmup="$SOAK_WARMUP_SECONDS" '
    $1 >= warmup { values[count % 3]=$3; count++ }
    END {
        min=max=values[0]; sum=0
        for (i=0; i < 3; i++) {
            if (values[i] < min) min=values[i]
            if (values[i] > max) max=values[i]
            sum += values[i]
        }
        print sum - min - max
    }
' "$SAMPLES")
fd_min=$(awk -v warmup="$SOAK_WARMUP_SECONDS" \
    '$1 >= warmup && (!seen || $3 < value) { value=$3; seen=1 } END { print value + 0 }' \
    "$SAMPLES")
fd_max=$(awk -v warmup="$SOAK_WARMUP_SECONDS" \
    '$1 >= warmup && (!seen || $3 > value) { value=$3; seen=1 } END { print value + 0 }' \
    "$SAMPLES")

rss_span=$((rss_max - rss_min))
rss_growth=$((rss_last - rss_first))
fd_span=$((fd_max - fd_min))
fd_growth=$((fd_tail_median - fd_head_median))
if [ "$rss_span" -gt "$SOAK_RSS_SPAN_KB" ] ||
   [ "$rss_growth" -gt "$SOAK_RSS_GROWTH_KB" ]; then
    echo "error: RSS is not flat: first=$rss_first last=$rss_last min=$rss_min max=$rss_max KiB" >&2
    exit 1
fi
if [ "$fd_span" -gt "$SOAK_FD_SPAN" ] || [ "$fd_growth" -gt 0 ]; then
    echo "error: fd count is not flat: head_median=$fd_head_median tail_median=$fd_tail_median min=$fd_min max=$fd_max" >&2
    exit 1
fi

ready_count=$(awk '/external_group: ready/ { count++ } END { print count + 0 }' "$LOG")
destroyed_count=$(awk '/external_group: destroyed/ { count++ } END { print count + 0 }' "$LOG")
frame_count=$(awk '/zero_copy=1 external=1/ { count++ } END { print count + 0 }' "$LOG")
worker_started=$(awk '/decode worker: started/ { count++ } END { print count + 0 }' "$LOG")
worker_stopped=$(awk '/decode worker: stopped/ { count++ } END { print count + 0 }' "$LOG")
bad_count=$(awk '
    /mode=internal-fallback/ || /zero_copy=0/ || /copied=1/ ||
    /external buffer mismatch/ || /unsafe internal layout/ ||
    /has no pending route/ || /submission failed/ ||
    /output wait failed/ || /SyncSurface: .*draining MPP/ ||
    /stale surface=/ { count++ }
    END { print count + 0 }
' "$LOG")
minimum_frames=$((SOAK_SECONDS * 25))

if [ "$ready_count" -eq 0 ] || [ "$destroyed_count" -ne "$ready_count" ] ||
   [ "$worker_started" -ne "$ready_count" ] ||
   [ "$worker_stopped" -ne "$worker_started" ]; then
    echo "error: soak lifecycle mismatch: pools=$ready_count/$destroyed_count workers=$worker_started/$worker_stopped" >&2
    exit 1
fi
if [ "$frame_count" -lt "$minimum_frames" ]; then
    echo "error: expected at least $minimum_frames external frames, observed $frame_count" >&2
    exit 1
fi
if [ "$bad_count" -ne 0 ]; then
    echo "error: fallback, stale route, or ownership failure found in soak log" >&2
    exit 1
fi

echo "soak resources: RSS first=$rss_first last=$rss_last span=${rss_span}KiB; fds head_median=$fd_head_median tail_median=$fd_tail_median span=$fd_span"
if [ "$SOAK_SECONDS" -lt 7200 ]; then
    echo "soak smoke: OK (${SOAK_SECONDS}s, $frame_count frames); Phase 1 requires at least 7200s"
else
    echo "multi-hour 4K soak gate: OK (${SOAK_SECONDS}s, $frame_count frames)"
fi
