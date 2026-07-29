#!/bin/sh
# Prove simultaneous decode and encode contexts in one FFmpeg process.

set -eu

FFMPEG=${FFMPEG:-/usr/bin/ffmpeg}
FFPROBE=${FFPROBE:-/usr/bin/ffprobe}
FFMPEG_TIMEOUT=${FFMPEG_TIMEOUT:-180}
SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH='' cd -- "$SCRIPT_DIR/.." && pwd)
DRIVER_DIR=${DRIVER_DIR:-$REPO_ROOT}
RENDER_NODE=${RENDER_NODE:-/dev/dri/renderD128}
KEEP_WORK=${KEEP_WORK:-0}
FRAMES=${CONCURRENT_CONTEXT_FRAMES:-120}
FILTER_COMPLEX_THREADS=${FILTER_COMPLEX_THREADS:-4}
SAME_PROCESS_DECODE_OUTPUT_MODE=${SAME_PROCESS_DECODE_OUTPUT_MODE:-compare}
SAME_PROCESS_GRAPH_MODE=${SAME_PROCESS_GRAPH_MODE:-complex}
WORK=$(mktemp -d "$REPO_ROOT/.test-work.same-process.XXXXXX") || exit 1
DRIVER_LOG=$WORK/driver.log

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

for command in "$FFMPEG" "$FFPROBE" timeout cmp awk; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "error: required command not found: $command" >&2
        exit 2
    fi
done
case $FRAMES in
    ''|*[!0-9]*|0)
        echo "error: CONCURRENT_CONTEXT_FRAMES must be a positive integer" >&2
        exit 2
        ;;
esac
case $FILTER_COMPLEX_THREADS in
    ''|*[!0-9]*|0)
        echo "error: FILTER_COMPLEX_THREADS must be a positive integer" >&2
        exit 2
        ;;
esac
case $SAME_PROCESS_GRAPH_MODE in
    complex|simple)
        ;;
    *)
        echo "error: SAME_PROCESS_GRAPH_MODE must be complex or simple" >&2
        exit 2
        ;;
esac
case $SAME_PROCESS_DECODE_OUTPUT_MODE in
    compare)
        H264_HARDWARE_OUTPUT=$WORK/h264.hardware.nv12
        VP9_HARDWARE_OUTPUT=$WORK/vp9.hardware.nv12
        ;;
    discard)
        H264_HARDWARE_OUTPUT=/dev/null
        VP9_HARDWARE_OUTPUT=/dev/null
        ;;
    *)
        echo "error: SAME_PROCESS_DECODE_OUTPUT_MODE must be compare or discard" >&2
        exit 2
        ;;
esac

duration=$((FRAMES / 30))
if [ $((duration * 30)) -lt "$FRAMES" ]; then
    duration=$((duration + 1))
fi

run_ffmpeg()
{
    timeout --kill-after=5s "$FFMPEG_TIMEOUT" "$FFMPEG" "$@"
}

run_ffmpeg -nostdin -y -v error -f lavfi \
    -i "testsrc2=size=1280x720:rate=30" -frames:v "$FRAMES" \
    -c:v libx264 -profile:v high -x264-params ref=4:bframes=3 \
    -pix_fmt yuv420p "$WORK/decode.h264.mp4"
run_ffmpeg -nostdin -y -v error -f lavfi \
    -i "testsrc2=size=1280x720:rate=30" -frames:v "$FRAMES" \
    -c:v libvpx-vp9 -b:v 1M -pix_fmt yuv420p "$WORK/decode.vp9.webm"

run_ffmpeg -nostdin -y -v error -i "$WORK/decode.h264.mp4" \
    -frames:v "$FRAMES" -vf format=nv12 -c:v rawvideo \
    -f rawvideo "$WORK/h264.reference.nv12"
run_ffmpeg -nostdin -y -v error -i "$WORK/decode.vp9.webm" \
    -frames:v "$FRAMES" -vf format=nv12 -c:v rawvideo \
    -f rawvideo "$WORK/vp9.reference.nv12"

export LIBVA_DRIVER_NAME=rockchip
export LIBVA_DRIVERS_PATH="$DRIVER_DIR"
export RK_VAAPI_EXPERIMENTAL_ENCODE=h264,hevc

case $SAME_PROCESS_GRAPH_MODE in
    complex)
        RK_VAAPI_LOG=$DRIVER_LOG LD_PRELOAD=${HW_LD_PRELOAD:-} \
            run_ffmpeg -nostdin -y -v error \
            -readrate 1 -hwaccel vaapi -hwaccel_output_format vaapi \
                -vaapi_device "$RENDER_NODE" -threads 1 \
                -i "$WORK/decode.h264.mp4" \
            -readrate 1 -hwaccel vaapi -hwaccel_output_format vaapi \
                -vaapi_device "$RENDER_NODE" -threads 1 \
                -i "$WORK/decode.vp9.webm" \
            -re -f lavfi \
                -i "testsrc2=size=320x240:rate=30:duration=$duration" \
            -filter_complex_threads "$FILTER_COMPLEX_THREADS" -filter_complex \
                "[0:v]hwdownload,format=nv12[h264dec]; \
                 [1:v]hwdownload,format=nv12[vp9dec]; \
                 [2:v]split=2[h264src][hevcsrc]; \
                 [h264src]format=nv12,hwupload[h264enc]; \
                 [hevcsrc]format=nv12,hwupload[hevcenc]" \
            -map '[h264dec]' -frames:v "$FRAMES" -an -threads 1 \
                -c:v rawvideo -f rawvideo "$H264_HARDWARE_OUTPUT" \
            -map '[vp9dec]' -frames:v "$FRAMES" -an -threads 1 \
                -c:v rawvideo -f rawvideo "$VP9_HARDWARE_OUTPUT" \
            -map '[h264enc]' -frames:v "$FRAMES" -an \
                -c:v h264_vaapi -profile:v high -rc_mode CQP -qp 24 \
                "$WORK/encoded.h264" \
            -map '[hevcenc]' -frames:v "$FRAMES" -an \
                -c:v hevc_vaapi -profile:v main -rc_mode CQP -qp 24 \
                "$WORK/encoded.hevc" >"$WORK/ffmpeg.log" 2>&1
        ;;
    simple)
        RK_VAAPI_LOG=$DRIVER_LOG LD_PRELOAD=${HW_LD_PRELOAD:-} \
            run_ffmpeg -nostdin -y -v error \
            -readrate 1 -hwaccel vaapi -hwaccel_output_format vaapi \
                -vaapi_device "$RENDER_NODE" -threads 1 \
                -i "$WORK/decode.h264.mp4" \
            -readrate 1 -hwaccel vaapi -hwaccel_output_format vaapi \
                -vaapi_device "$RENDER_NODE" -threads 1 \
                -i "$WORK/decode.vp9.webm" \
            -re -f lavfi \
                -i "testsrc2=size=320x240:rate=30:duration=$duration" \
            -re -f lavfi \
                -i "testsrc2=size=320x240:rate=30:duration=$duration" \
            -filter_threads 1 \
            -map 0:v -frames:v "$FRAMES" -an -threads 1 \
                -vf hwdownload,format=nv12 \
                -c:v rawvideo -f rawvideo "$H264_HARDWARE_OUTPUT" \
            -map 1:v -frames:v "$FRAMES" -an -threads 1 \
                -vf hwdownload,format=nv12 \
                -c:v rawvideo -f rawvideo "$VP9_HARDWARE_OUTPUT" \
            -map 2:v -frames:v "$FRAMES" -an \
                -vf format=nv12,hwupload \
                -c:v h264_vaapi -profile:v high -rc_mode CQP -qp 24 \
                "$WORK/encoded.h264" \
            -map 3:v -frames:v "$FRAMES" -an \
                -vf format=nv12,hwupload \
                -c:v hevc_vaapi -profile:v main -rc_mode CQP -qp 24 \
                "$WORK/encoded.hevc" >"$WORK/ffmpeg.log" 2>&1
        ;;
esac

if [ "$SAME_PROCESS_DECODE_OUTPUT_MODE" = compare ]; then
    if ! cmp -s "$WORK/h264.reference.nv12" "$H264_HARDWARE_OUTPUT"; then
        echo "FAIL  same-process H.264 decode differs from software" >&2
        exit 1
    fi
    if ! cmp -s "$WORK/vp9.reference.nv12" "$VP9_HARDWARE_OUTPUT"; then
        echo "FAIL  same-process VP9 decode differs from software" >&2
        exit 1
    fi
fi

for encoded in "$WORK/encoded.h264" "$WORK/encoded.hevc"; do
    encoded_frames=$("$FFPROBE" -v error -count_frames -select_streams v:0 \
        -show_entries stream=nb_read_frames -of default=nw=1:nk=1 "$encoded")
    if [ "$encoded_frames" != "$FRAMES" ]; then
        echo "FAIL  same-process output $encoded has $encoded_frames frames" >&2
        exit 1
    fi
    run_ffmpeg -nostdin -v error -i "$encoded" -f null -
done

worker_started=$(awk '/decode worker: started/ { count++ }
                      END { print count + 0 }' "$DRIVER_LOG")
worker_stopped=$(awk '/decode worker: stopped/ { count++ }
                      END { print count + 0 }' "$DRIVER_LOG")
peak_workers=$(awk '
    /decode worker: started/ { active++; if (active > peak) peak = active }
    /decode worker: stopped/ { active-- }
    END { print peak + 0 }
' "$DRIVER_LOG")
decoded=$(awk '/zero_copy=1/ && /external=1/ { count++ }
               END { print count + 0 }' "$DRIVER_LOG")
encoded=$(awk '/encoder produced .* bytes/ { count++ }
               END { print count + 0 }' "$DRIVER_LOG")
overlap_packets=$(awk '
    /decode worker: started/ { active++ }
    /encoder produced .* bytes/ && active >= 2 { count++ }
    /decode worker: stopped/ { active-- }
    END { print count + 0 }
' "$DRIVER_LOG")
bad=$(awk '
    /mode=internal-fallback/ || /zero_copy=0/ || /copied=1/ ||
    /external buffer mismatch/ || /unsafe internal layout/ ||
    /has no pending route/ || /submission failed/ ||
    /output wait failed/ || /encoder .*failed/ || /config rejected/ {
        count++
    }
    END { print count + 0 }
' "$DRIVER_LOG")

if [ "$worker_started" -ne 2 ] || [ "$worker_stopped" -ne 2 ] ||
   [ "$peak_workers" -lt 2 ] || [ "$decoded" -ne $((FRAMES * 2)) ] ||
   [ "$encoded" -ne $((FRAMES * 2)) ] ||
   [ "$overlap_packets" -lt "$FRAMES" ] || [ "$bad" -ne 0 ]; then
    echo "FAIL  same-process audit workers=$worker_started/$worker_stopped peak=$peak_workers decoded=$decoded encoded=$encoded overlap=$overlap_packets bad=$bad" >&2
    exit 1
fi

echo "ok    same-process contexts: two decoders + two encoders, $FRAMES frames each"
echo "ok    peak decode workers=$peak_workers encode/decode overlap packets=$overlap_packets"
