#!/bin/sh
# Paced dual-codec encoder soak with process-resource and driver-log audits.

set -eu

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH='' cd -- "$SCRIPT_DIR/.." && pwd)
DRIVER_DIR=${DRIVER_DIR:-$REPO_ROOT}
SOAK_SECONDS=${ENCODE_SOAK_SECONDS:-7200}
SAMPLE_SECONDS=${ENCODE_SOAK_SAMPLE_SECONDS:-30}
WARMUP_SECONDS=${ENCODE_SOAK_WARMUP_SECONDS:-60}
RSS_SPAN_KB=${ENCODE_SOAK_RSS_SPAN_KB:-65536}
RSS_GROWTH_KB=${ENCODE_SOAK_RSS_GROWTH_KB:-32768}
FD_SPAN=${ENCODE_SOAK_FD_SPAN:-16}
FPS=${ENCODE_SOAK_FPS:-30}
WIDTH=${ENCODE_SOAK_WIDTH:-640}
HEIGHT=${ENCODE_SOAK_HEIGHT:-360}
KEEP_WORK=${KEEP_WORK:-0}
WORK=$(mktemp -d "$REPO_ROOT/.test-work.encode-soak.XXXXXX") || exit 1
SAMPLES=$WORK/samples.tsv
H264_PID=
HEVC_PID=

case $SOAK_SECONDS:$SAMPLE_SECONDS:$WARMUP_SECONDS:$FPS:$WIDTH:$HEIGHT in
    *[!0-9:]*|:*|*::*)
        echo "error: encode soak values must be non-negative integers" >&2
        exit 2
        ;;
esac
if [ "$SOAK_SECONDS" -eq 0 ] || [ "$SAMPLE_SECONDS" -eq 0 ] ||
   [ "$WARMUP_SECONDS" -ge "$SOAK_SECONDS" ] || [ "$FPS" -eq 0 ] ||
   [ "$WIDTH" -eq 0 ] || [ "$HEIGHT" -eq 0 ]; then
    echo "error: require positive dimensions/durations and warmup < soak" >&2
    exit 2
fi

# shellcheck disable=SC2317,SC2329 # Invoked by traps.
cleanup()
{
    for pid in "$H264_PID" "$HEVC_PID"; do
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
        fi
    done
    if [ "$KEEP_WORK" = 1 ]; then
        echo "work files retained in $WORK"
    else
        rm -rf "$WORK"
    fi
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

export GST_VA_ALL_DRIVERS=1
export GST_REGISTRY="$WORK/gstreamer-registry.bin"
export LIBVA_DRIVER_NAME=rockchip
export LIBVA_DRIVERS_PATH="$DRIVER_DIR"
export RK_VAAPI_EXPERIMENTAL_ENCODE=h264,hevc
gst-inspect-1.0 --exists vah264enc
gst-inspect-1.0 --exists vah265enc

frames=$((SOAK_SECONDS * FPS))
RK_VAAPI_LOG=$WORK/h264.driver.log gst-launch-1.0 -e -q \
    videotestsrc is-live=true num-buffers="$frames" pattern=smpte \
    ! "video/x-raw,format=I420,width=$WIDTH,height=$HEIGHT,framerate=$FPS/1" \
    ! vah264enc rate-control=cbr bitrate=1000 key-int-max="$FPS" \
    ! fakesink sync=false >"$WORK/h264.log" 2>&1 &
H264_PID=$!
RK_VAAPI_LOG=$WORK/hevc.driver.log gst-launch-1.0 -e -q \
    videotestsrc is-live=true num-buffers="$frames" pattern=smpte \
    ! "video/x-raw,format=I420,width=$WIDTH,height=$HEIGHT,framerate=$FPS/1" \
    ! vah265enc rate-control=cbr bitrate=1000 key-int-max="$FPS" \
    ! fakesink sync=false >"$WORK/hevc.log" 2>&1 &
HEVC_PID=$!
start=$(date +%s)

while kill -0 "$H264_PID" 2>/dev/null ||
      kill -0 "$HEVC_PID" 2>/dev/null; do
    sleep "$SAMPLE_SECONDS"
    rss=0
    fds=0
    for pid in "$H264_PID" "$HEVC_PID"; do
        if kill -0 "$pid" 2>/dev/null; then
            value=$(awk '/^VmRSS:/ { print $2; found=1 }
                         END { if (!found) print 0 }' "/proc/$pid/status")
            rss=$((rss + value))
            value=$(find "/proc/$pid/fd" -mindepth 1 -maxdepth 1 \
                        2>/dev/null | wc -l)
            fds=$((fds + value))
        fi
    done
    elapsed=$(($(date +%s) - start))
    printf '%s\t%s\t%s\n' "$elapsed" "$rss" "$fds" >>"$SAMPLES"
    echo "encode soak sample: elapsed=${elapsed}s rss=${rss}KiB fds=$fds"
done

set +e
wait "$H264_PID"
h264_status=$?
wait "$HEVC_PID"
hevc_status=$?
set -e
H264_PID=
HEVC_PID=
if [ "$h264_status" -ne 0 ] || [ "$hevc_status" -ne 0 ]; then
    echo "FAIL  encode soak pipeline status h264=$h264_status hevc=$hevc_status" >&2
    tail -40 "$WORK/h264.log" >&2
    tail -40 "$WORK/hevc.log" >&2
    tail -80 "$WORK/h264.driver.log" >&2
    tail -80 "$WORK/hevc.driver.log" >&2
    exit 1
fi

sample_count=$(awk -v warmup="$WARMUP_SECONDS" \
    '$1 >= warmup && $2 > 0 { count++ } END { print count + 0 }' "$SAMPLES")
if [ "$sample_count" -lt 3 ]; then
    echo "FAIL  only $sample_count post-warmup resource samples" >&2
    exit 1
fi

# shellcheck disable=SC2046 # Split the eight scalar awk fields into $1..$8.
set -- $(awk -v warmup="$WARMUP_SECONDS" '
    $1 >= warmup && $2 > 0 {
        if (!seen) {
            rss_first=rss_min=rss_max=$2
            fd_first=fd_min=fd_max=$3
            seen=1
        }
        rss_last=$2
        fd_last=$3
        if ($2 < rss_min) rss_min=$2
        if ($2 > rss_max) rss_max=$2
        if ($3 < fd_min) fd_min=$3
        if ($3 > fd_max) fd_max=$3
    }
    END {
        print rss_first, rss_last, rss_min, rss_max,
              fd_first, fd_last, fd_min, fd_max
    }
' "$SAMPLES")
rss_first=$1
rss_last=$2
rss_min=$3
rss_max=$4
fd_first=$5
fd_last=$6
fd_min=$7
fd_max=$8
rss_span=$((rss_max - rss_min))
rss_growth=$((rss_last - rss_first))
fd_span=$((fd_max - fd_min))
fd_growth=$((fd_last - fd_first))
if [ "$rss_span" -gt "$RSS_SPAN_KB" ] ||
   [ "$rss_growth" -gt "$RSS_GROWTH_KB" ] ||
   [ "$fd_span" -gt "$FD_SPAN" ] || [ "$fd_growth" -gt 0 ]; then
    echo "FAIL  encode soak resources rss=$rss_first/$rss_last/$rss_min/$rss_max fds=$fd_first/$fd_last/$fd_min/$fd_max" >&2
    exit 1
fi

for codec in h264 hevc; do
    driver_log=$WORK/$codec.driver.log
    packets=$(grep -c 'encoder produced .* bytes' "$driver_log" || true)
    conversions=$(grep -c 'PutImage: I420->NV12' "$driver_log" || true)
    if [ "$packets" -ne "$frames" ] || [ "$conversions" -lt "$frames" ] ||
       grep -q 'encoder .*failed\|rejected' "$driver_log"; then
        echo "FAIL  $codec soak audit packets=$packets conversions=$conversions expected=$frames" >&2
        exit 1
    fi
done

echo "encode soak resources: RSS first=$rss_first last=$rss_last span=${rss_span}KiB; fds first=$fd_first last=$fd_last span=$fd_span"
if [ "$SOAK_SECONDS" -lt 7200 ]; then
    echo "encode soak smoke: OK (${SOAK_SECONDS}s, $frames frames/codec); qualification requires 7200s"
else
    echo "dual-codec encode soak gate: OK (${SOAK_SECONDS}s, $frames frames/codec)"
fi
