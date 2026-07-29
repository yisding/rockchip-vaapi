#!/bin/sh
# Measure sustained 1080p60 HEVC Main10 and VP9 Profile 2 decode + RGA P010.

set -eu

FFMPEG=${FFMPEG:-/usr/bin/ffmpeg}
FFMPEG_TIMEOUT=${FFMPEG_TIMEOUT:-600}
SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH='' cd -- "$SCRIPT_DIR/.." && pwd)
DRIVER_DIR=${DRIVER_DIR:-$REPO_ROOT}
RENDER_NODE=${RENDER_NODE:-/dev/dri/renderD128}
KEEP_WORK=${KEEP_WORK:-0}
WIDTH=${THROUGHPUT_WIDTH:-1920}
HEIGHT=${THROUGHPUT_HEIGHT:-1080}
FRAMES=${THROUGHPUT_FRAMES:-240}
FPS=${THROUGHPUT_RATE:-60}
MIN_FPS=${THROUGHPUT_MIN_FPS:-60}

for command in "$FFMPEG" timeout awk date; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "error: required command not found: $command" >&2
        exit 2
    fi
done
for value in "$WIDTH" "$HEIGHT" "$FRAMES" "$FPS" "$MIN_FPS"; do
    case $value in
        ''|*[!0-9]*|0)
            echo "error: throughput geometry/rate values must be positive integers" >&2
            exit 2
            ;;
    esac
done
if [ $((WIDTH % 2)) -ne 0 ] || [ $((HEIGHT % 2)) -ne 0 ]; then
    echo "error: throughput dimensions must be even" >&2
    exit 2
fi

WORK=$(mktemp -d "$REPO_ROOT/.test-work.10bit-throughput.XXXXXX") || exit 1
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

hevc=$WORK/main10.mkv
vp9=$WORK/profile2.webm
run_ffmpeg -nostdin -y -v error -f lavfi \
    -i "testsrc2=size=${WIDTH}x${HEIGHT}:rate=${FPS}" \
    -frames:v "$FRAMES" -an -vf format=yuv420p10le \
    -c:v libx265 -preset ultrafast -crf 28 \
    -x265-params "profile=main10:keyint=${FPS}:min-keyint=${FPS}:scenecut=0:log-level=error" \
    "$hevc"
run_ffmpeg -nostdin -y -v error -f lavfi \
    -i "testsrc2=size=${WIDTH}x${HEIGHT}:rate=${FPS}" \
    -frames:v "$FRAMES" -an -vf format=yuv420p10le \
    -c:v libvpx-vp9 -profile:v 2 -b:v 8M -row-mt 1 -threads 8 \
    -deadline realtime -cpu-used 8 -g "$FPS" "$vp9"

run_case()
{
    label=$1
    token=$2
    input=$3
    expected_profile=$4
    conversion_policy=$5
    driver_log=$WORK/$label.driver.log
    ffmpeg_log=$WORK/$label.ffmpeg.log
    progress=$WORK/$label.progress

    start_ns=$(date +%s%N)
    RK_VAAPI_EXPERIMENTAL_PROFILES=$token \
    LIBVA_DRIVER_NAME=rockchip \
    LIBVA_DRIVERS_PATH=$DRIVER_DIR \
    RK_VAAPI_LOG=$driver_log \
        run_ffmpeg -nostdin -v error \
        -hwaccel vaapi -hwaccel_output_format vaapi \
        -vaapi_device "$RENDER_NODE" -i "$input" -an \
        -frames:v "$FRAMES" -vf 'hwdownload,format=p010le' \
        -pix_fmt p010le -fps_mode passthrough -progress "$progress" \
        -f null - >"$ffmpeg_log" 2>&1
    end_ns=$(date +%s%N)

    elapsed_ns=$((end_ns - start_ns))
    measured_fps=$(awk -v frames="$FRAMES" -v ns="$elapsed_ns" \
        'BEGIN { printf "%.2f", frames * 1000000000 / ns }')
    conversions=$(grep -c 'convert: NV15->P010.*afbc=1' "$driver_log" || true)
    assigned=$(grep -c \
        'assign_mpp_frame: surface=.*converted_10bit=1.*external=1' \
        "$driver_log" || true)
    output_frames=$(sed -n 's/^frame=//p' "$progress" | tail -1)
    case $conversion_policy in
        exact)
            conversion_count_ok=false
            if [ "$conversions" -eq "$FRAMES" ]; then
                conversion_count_ok=true
            fi
            ;;
        allow-hidden)
            conversion_count_ok=false
            if [ "$conversions" -ge "$FRAMES" ]; then
                conversion_count_ok=true
            fi
            ;;
        *)
            echo "error: invalid conversion policy: $conversion_policy" >&2
            exit 2
            ;;
    esac
    if [ "$output_frames" != "$FRAMES" ] ||
       [ "$assigned" -ne "$conversions" ] ||
       [ "$conversion_count_ok" != true ] ||
       ! grep -q "10-bit output mode=AFBC_V2 profile=$expected_profile" \
            "$driver_log" ||
       ! awk -v actual="$measured_fps" -v minimum="$MIN_FPS" \
            'BEGIN { exit !(actual + 0 >= minimum + 0) }' ||
       grep -Eq 'external buffer mismatch|decode failed|afbc=0|RGA .*failed' \
            "$driver_log"; then
        echo "FAIL  $label throughput frames=$output_frames conversions=$conversions assigned=$assigned fps=$measured_fps minimum=$MIN_FPS" >&2
        exit 1
    fi

    echo "ok    $label ${WIDTH}x${HEIGHT} P010 throughput ${measured_fps} fps ($output_frames visible, $conversions decoded)"
}

run_case hevc-main10 hevc-main10 "$hevc" 18 exact
run_case vp9-profile2 vp9-profile2 "$vp9" 21 allow-hidden

echo "ok    both 10-bit decode/RGA paths sustain at least ${MIN_FPS} fps"
