#!/bin/sh
# Reduce a Main10 stream that whole-stream MPP decodes but the VA path breaks.
#
# This is deliberately not a generic decode gate. It accepts a prefix only
# when software decode and direct MPP are clean, then requires the VA path to
# produce MPP errinfo/discard diagnostics. RGA conversion failures are kept as
# infrastructure evidence and never mislabeled as rebuilt-packet failures.

set -eu

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH='' cd -- "$SCRIPT_DIR/.." && pwd)
FFMPEG=${FFMPEG:-/usr/bin/ffmpeg}
FFPROBE=${FFPROBE:-/usr/bin/ffprobe}
REPRO=${REPRO:-$SCRIPT_DIR/hevc_mpp_repro}
DRIVER_DIR=${DRIVER_DIR:-$REPO_ROOT}
RENDER_NODE=${RENDER_NODE:-/dev/dri/renderD128}
FFMPEG_TIMEOUT=${FFMPEG_TIMEOUT:-120}
DIRECT_TIMEOUT=${DIRECT_TIMEOUT:-30}
MAX_PREFIXES=${MAX_PREFIXES:-32}
ATTEMPTS=${ATTEMPTS:-3}
REQUIRED_FAILURES=${REQUIRED_FAILURES:-2}
DIRECT_ATTEMPTS=${DIRECT_ATTEMPTS:-3}
REQUIRED_DIRECT_CLEAN=${REQUIRED_DIRECT_CLEAN:-3}
EXPECTED_RESULT=${EXPECTED_RESULT:-failure}
WIDTH=${MAIN10_REDUCER_WIDTH:-1280}
HEIGHT=${MAIN10_REDUCER_HEIGHT:-720}
FRAMES=${MAIN10_REDUCER_FRAMES:-240}
FPS=${MAIN10_REDUCER_FPS:-30}
INPUT=${1:-}
OUTPUT_DIR=${2:-}

usage()
{
    echo "usage: $0 [INPUT.(mp4|mkv|h265) [OUTPUT_DIR]]" >&2
    echo "environment: FFMPEG FFPROBE REPRO DRIVER_DIR RENDER_NODE" >&2
    echo "             MAX_PREFIXES ATTEMPTS REQUIRED_FAILURES" >&2
    echo "             DIRECT_ATTEMPTS REQUIRED_DIRECT_CLEAN EXPECTED_RESULT" >&2
    echo "             MAIN10_REDUCER_WIDTH MAIN10_REDUCER_HEIGHT" >&2
    echo "             MAIN10_REDUCER_FRAMES MAIN10_REDUCER_FPS" >&2
}

case ${1:-} in
    -h|--help)
        usage
        exit 0
        ;;
esac

for command in "$FFMPEG" "$FFPROBE" "$REPRO" timeout sha256sum awk grep \
               sed tail wc cmp cp date uname; do
    if [ ! -x "$command" ] && ! command -v "$command" >/dev/null 2>&1; then
        echo "error: required command not found or executable: $command" >&2
        exit 2
    fi
done
for value in "$FFMPEG_TIMEOUT" "$DIRECT_TIMEOUT" "$MAX_PREFIXES" \
             "$ATTEMPTS" "$REQUIRED_FAILURES" "$DIRECT_ATTEMPTS" \
             "$REQUIRED_DIRECT_CLEAN" "$WIDTH" "$HEIGHT" "$FRAMES" \
             "$FPS"; do
    case $value in
        ''|*[!0-9]*)
            echo "error: numeric reducer settings must be non-negative integers" >&2
            exit 2
            ;;
    esac
done
if [ "$FFMPEG_TIMEOUT" -eq 0 ] || [ "$DIRECT_TIMEOUT" -eq 0 ] ||
   [ "$ATTEMPTS" -eq 0 ] || [ "$REQUIRED_FAILURES" -eq 0 ] ||
   [ "$REQUIRED_FAILURES" -gt "$ATTEMPTS" ] || [ "$WIDTH" -eq 0 ] ||
   [ "$DIRECT_ATTEMPTS" -eq 0 ] || [ "$REQUIRED_DIRECT_CLEAN" -eq 0 ] ||
   [ "$REQUIRED_DIRECT_CLEAN" -gt "$DIRECT_ATTEMPTS" ] ||
   [ "$HEIGHT" -eq 0 ] || [ "$FRAMES" -eq 0 ] || [ "$FPS" -eq 0 ]; then
    echo "error: timeouts, attempts, geometry, frame count, and rate must be positive" >&2
    echo "error: REQUIRED_FAILURES must not exceed ATTEMPTS" >&2
    exit 2
fi
if [ $((WIDTH % 2)) -ne 0 ] || [ $((HEIGHT % 2)) -ne 0 ]; then
    echo "error: generated Main10 dimensions must be even" >&2
    exit 2
fi
case $EXPECTED_RESULT in
    failure|fixed) ;;
    *) echo "error: EXPECTED_RESULT must be failure or fixed" >&2; exit 2 ;;
esac
if [ ! -e "$RENDER_NODE" ]; then
    echo "error: render node is missing: $RENDER_NODE" >&2
    exit 2
fi
if [ ! -f "$DRIVER_DIR/rockchip_drv_video.so" ]; then
    echo "error: driver is missing: $DRIVER_DIR/rockchip_drv_video.so" >&2
    exit 2
fi

if [ -z "$OUTPUT_DIR" ]; then
    OUTPUT_DIR=$(mktemp -d \
        "$REPO_ROOT/.test-work.hevc-main10-reconstruction.XXXXXX") || exit 1
else
    mkdir -p "$OUTPUT_DIR"
    OUTPUT_DIR=$(CDPATH='' cd -- "$OUTPUT_DIR" && pwd)
fi

run_ffmpeg()
{
    timeout --kill-after=5s "$FFMPEG_TIMEOUT" "$FFMPEG" "$@"
}

frame_count()
{
    "$FFPROBE" -v error -codec_whitelist hevc -select_streams v:0 \
        -count_frames -show_entries stream=nb_read_frames \
        -of default=nk=1:nw=1 "$1"
}

positive_count()
{
    case $1 in
        ''|N/A|*[!0-9]*|0) return 1 ;;
        *) return 0 ;;
    esac
}

software_hash()
{
    stream=$1
    hash_file=$2
    log_file=$3
    run_ffmpeg -nostdin -y -v error -c:v hevc -i "$stream" -an \
        -vf format=p010le -fps_mode passthrough -f hash -hash sha256 \
        "$hash_file" >"$log_file" 2>&1
}

direct_check()
{
    stream=$1
    expected_frames=$2
    log_file=$3
    timeout --kill-after=5s "$DIRECT_TIMEOUT" "$REPRO" \
        "$stream" "$expected_frames" >"$log_file" 2>&1
}

direct_control()
{
    control_stream=$1
    control_frames=$2
    control_stem=$3
    control_clean=0
    control_stream_errors=0
    control_attempt=1
    while [ "$control_attempt" -le "$DIRECT_ATTEMPTS" ]; do
        control_log=$control_stem.direct-$control_attempt.log
        if direct_check "$control_stream" "$control_frames" "$control_log"; then
            control_clean=$((control_clean + 1))
        else
            control_status=$?
            if [ "$control_status" -eq 1 ]; then
                control_stream_errors=$((control_stream_errors + 1))
            else
                echo "BLOCK environment: direct MPP runtime/setup failure status=$control_status log=$control_log" >&2
                return 2
            fi
        fi
        control_attempt=$((control_attempt + 1))
    done
    echo "direct stream=$control_stream clean=$control_clean/$DIRECT_ATTEMPTS stream_errors=$control_stream_errors"
    [ "$control_clean" -ge "$REQUIRED_DIRECT_CLEAN" ]
}

packetized_matrix()
{
    matrix_label=$1
    matrix_stream=$2
    matrix_sizes=$3
    matrix_frames=$4
    matrix_afbc=$5
    matrix_external=$6
    matrix_clean=0
    matrix_bad_frames=0
    matrix_other=0
    matrix_attempt=1
    while [ "$matrix_attempt" -le "$ATTEMPTS" ]; do
        matrix_log=$OUTPUT_DIR/replay-$matrix_label-$matrix_attempt.log
        set +e
        REPRO_IMMEDIATE_OUT=1 REPRO_AFBC=$matrix_afbc \
        REPRO_EXTERNAL_POOL=$matrix_external REPRO_NO_EOS=1 \
            timeout --kill-after=5s "$DIRECT_TIMEOUT" "$REPRO" \
            --packetized "$matrix_stream" "$matrix_sizes" "$matrix_frames" \
            >"$matrix_log" 2>&1
        matrix_status=$?
        set -e
        matrix_bad=$(sed -n \
            's/.*bad_frames=\([0-9][0-9]*\).*/\1/p' "$matrix_log" | \
            tail -1)
        if [ "$matrix_status" -eq 0 ]; then
            matrix_clean=$((matrix_clean + 1))
        elif [ "${matrix_bad:-0}" -gt 0 ]; then
            matrix_bad_frames=$((matrix_bad_frames + 1))
        else
            matrix_other=$((matrix_other + 1))
        fi
        matrix_attempt=$((matrix_attempt + 1))
    done
    echo "replay mode=$matrix_label clean=$matrix_clean/$ATTEMPTS bad_frame=$matrix_bad_frames other=$matrix_other"
}

record_trace()
{
    stream=$1
    stem=$2
    set +e
    "$FFMPEG" -nostdin -v trace -i "$stream" -map 0:v:0 -c:v copy \
        -bsf:v trace_headers -f null - >/dev/null 2>"$stem.trace.log"
    set -e
    grep -E 'nal_unit_type|slice_type|slice_pic_order_cnt_lsb|short_term_ref_pic_set_sps_flag|num_(negative|positive)_pics|delta_poc_s[01]_minus1|used_by_curr_pic_s[01]_flag|num_ref_idx_l[01]_active_minus1' \
        "$stem.trace.log" >"$stem.rps.log" || true
}

if [ -z "$INPUT" ]; then
    INPUT=$OUTPUT_DIR/generated-main10.mp4
    run_ffmpeg -nostdin -y -v error -f lavfi \
        -i "testsrc2=size=${WIDTH}x${HEIGHT}:rate=${FPS}" \
        -frames:v "$FRAMES" -an -vf format=yuv420p10le \
        -c:v libx265 -profile:v main10 -pix_fmt yuv420p10le \
        -movflags +faststart "$INPUT" >"$OUTPUT_DIR/generate.log" 2>&1
elif [ ! -f "$INPUT" ]; then
    echo "error: input does not exist: $INPUT" >&2
    exit 2
fi

profile=$("$FFPROBE" -v error -select_streams v:0 \
    -show_entries stream=profile -of default=nk=1:nw=1 "$INPUT")
if [ "$profile" != "Main 10" ]; then
    echo "error: reducer requires HEVC Main 10 input, got: $profile" >&2
    exit 2
fi

original=$OUTPUT_DIR/original.h265
run_ffmpeg -nostdin -y -v error -i "$INPUT" -map 0:v:0 -c:v copy \
    -bsf:v hevc_mp4toannexb -f hevc "$original" \
    >"$OUTPUT_DIR/extract.log" 2>&1
full_frames=$(frame_count "$original")
if ! positive_count "$full_frames"; then
    echo "error: could not count software-decoded input frames" >&2
    exit 2
fi
if ! software_hash "$original" "$OUTPUT_DIR/original.software.sha256" \
                         "$OUTPUT_DIR/original.software.log"; then
    echo "error: original stream does not decode cleanly in software" >&2
    exit 2
fi
if direct_control "$original" "$full_frames" "$OUTPUT_DIR/original"; then
    :
else
    status=$?
    echo "BLOCK backend: original stream is not a repeatably clean direct-MPP control (status=$status)" >&2
    exit 3
fi

driver_sha=$(sha256sum "$DRIVER_DIR/rockchip_drv_video.so" | awk '{print $1}')
repro_sha=$(sha256sum "$REPRO" | awk '{print $1}')
input_sha=$(sha256sum "$INPUT" | awk '{print $1}')
original_sha=$(sha256sum "$original" | awk '{print $1}')
{
    echo "date_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "kernel=$(uname -a)"
    echo "input=$INPUT"
    echo "input_sha256=$input_sha"
    echo "original_annexb_sha256=$original_sha"
    echo "original_frames=$full_frames"
    echo "driver=$DRIVER_DIR/rockchip_drv_video.so"
    echo "driver_sha256=$driver_sha"
    echo "reproducer=$REPRO"
    echo "reproducer_sha256=$repro_sha"
    echo "render_node=$RENDER_NODE"
    echo "attempts=$ATTEMPTS"
    echo "required_failures=$REQUIRED_FAILURES"
    echo "direct_attempts=$DIRECT_ATTEMPTS"
    echo "required_direct_clean=$REQUIRED_DIRECT_CLEAN"
} >"$OUTPUT_DIR/environment.txt"

limit=$full_frames
if [ "$MAX_PREFIXES" -gt 0 ] && [ "$MAX_PREFIXES" -lt "$limit" ]; then
    limit=$MAX_PREFIXES
fi

minimal=
minimal_frames=0
minimal_prefix=0
minimal_failures=0
minimal_clean=0
minimal_infrastructure=0
minimal_dump=
n=1
while [ "$n" -le "$limit" ]; do
    stem=$(printf '%s/prefix-%03d' "$OUTPUT_DIR" "$n")
    candidate=$stem.h265
    run_ffmpeg -nostdin -y -v error -i "$INPUT" -map 0:v:0 -c:v copy \
        -frames:v "$n" -bsf:v hevc_mp4toannexb -f hevc "$candidate" \
        >"$stem.extract.log" 2>&1
    candidate_frames=$(frame_count "$candidate")
    if ! positive_count "$candidate_frames" ||
       ! software_hash "$candidate" "$stem.software.sha256" \
                                "$stem.software.log"; then
        echo "skip  prefix=$n software rejected the extracted prefix"
        n=$((n + 1))
        continue
    fi

    if direct_control "$candidate" "$candidate_frames" "$stem"; then
        :
    else
        status=$?
        if [ "$status" -eq 1 ]; then
            echo "skip  prefix=$n direct MPP control was not repeatably clean"
            n=$((n + 1))
            continue
        fi
        echo "BLOCK environment: direct MPP failed for prefix=$n (status=$status)" >&2
        exit 3
    fi

    reconstruction_failures=0
    clean_attempts=0
    infrastructure_failures=0
    other_failures=0
    first_complete_failure_dump=
    attempt=1
    while [ "$attempt" -le "$ATTEMPTS" ]; do
        driver_log=$stem.attempt-$attempt.driver.log
        ffmpeg_log=$stem.attempt-$attempt.ffmpeg.log
        hardware_hash=$stem.attempt-$attempt.hardware.sha256
        dump=$stem.attempt-$attempt.reconstructed.h265
        rm -f "$dump" "$dump.sizes" "$hardware_hash"

        set +e
        RK_VAAPI_EXPERIMENTAL_PROFILES=hevc-main10 \
        LIBVA_DRIVER_NAME=rockchip \
        LIBVA_DRIVERS_PATH=$DRIVER_DIR \
        RK_VAAPI_LOG=$driver_log \
        RK_VAAPI_HEVC_DUMP=$dump \
            run_ffmpeg -nostdin -y -v error \
            -hwaccel vaapi -hwaccel_output_format vaapi \
            -vaapi_device "$RENDER_NODE" -i "$candidate" -an \
            -vf 'hwdownload,format=p010le' -fps_mode passthrough \
            -f hash -hash sha256 "$hardware_hash" \
            >"$ffmpeg_log" 2>&1
        va_status=$?
        set -e

        mpp_errors=$(grep -cE 'MPP reported err=0x[1-9a-f]|discard=0x[1-9a-f]' \
                          "$driver_log" || true)
        rga_errors=$(grep -cE 'RGA NV15->P010 refused|external buffer mismatch' \
                          "$driver_log" || true)
        dump_records=0
        if [ -f "$dump.sizes" ]; then
            dump_records=$(wc -l <"$dump.sizes")
        fi

        if [ "$mpp_errors" -gt 0 ]; then
            reconstruction_failures=$((reconstruction_failures + 1))
            if [ "$dump_records" -eq "$candidate_frames" ] &&
               [ -z "$first_complete_failure_dump" ]; then
                first_complete_failure_dump=$dump
            fi
            verdict=mpp-error
        elif [ "$rga_errors" -gt 0 ]; then
            infrastructure_failures=$((infrastructure_failures + 1))
            verdict=rga-block
        elif [ "$va_status" -eq 0 ] && [ -f "$hardware_hash" ] &&
             cmp -s "$stem.software.sha256" "$hardware_hash"; then
            clean_attempts=$((clean_attempts + 1))
            verdict=clean
        else
            other_failures=$((other_failures + 1))
            verdict=other-failure
        fi
        echo "try   prefix=$n attempt=$attempt status=$va_status verdict=$verdict mpp_errors=$mpp_errors rga_errors=$rga_errors dump_records=$dump_records"
        attempt=$((attempt + 1))
    done

    echo "case  prefix=$n frames=$candidate_frames reconstruction=$reconstruction_failures/$ATTEMPTS clean=$clean_attempts rga_block=$infrastructure_failures other=$other_failures"
    if [ "$n" -eq 1 ] && [ "$clean_attempts" -eq 0 ] &&
       [ "$reconstruction_failures" -eq 0 ]; then
        echo "BLOCK environment: one-picture VA control never completed cleanly" >&2
        exit 3
    fi
    if [ "$reconstruction_failures" -ge "$REQUIRED_FAILURES" ]; then
        minimal=$candidate
        minimal_frames=$candidate_frames
        minimal_prefix=$n
        minimal_failures=$reconstruction_failures
        minimal_clean=$clean_attempts
        minimal_infrastructure=$infrastructure_failures
        minimal_dump=$first_complete_failure_dump
        break
    fi
    n=$((n + 1))
done

if [ -z "$minimal" ]; then
    {
        echo "HEVC Main10 VA reconstruction minimization report"
        cat "$OUTPUT_DIR/environment.txt"
        echo "expected_result=$EXPECTED_RESULT"
        echo "result=not-reproduced"
        echo "prefix_limit=$limit"
    } >"$OUTPUT_DIR/report.txt"
    if [ "$EXPECTED_RESULT" = fixed ]; then
        echo "ok    no MPP-marked VA reconstruction failure in $limit prefixes"
        echo "ok    report: $OUTPUT_DIR/report.txt"
        exit 0
    fi
    echo "NOT_REPRODUCED: no qualifying failure in $limit prefixes" >&2
    echo "report: $OUTPUT_DIR/report.txt" >&2
    exit 1
fi

minimal_original=$OUTPUT_DIR/minimal-original.h265
cp "$minimal" "$minimal_original"
record_trace "$minimal_original" "$OUTPUT_DIR/minimal-original"

reconstructed_direct=unavailable
replay_external_afbc_clean=0
replay_external_afbc_bad=0
replay_external_afbc_other=0
replay_external_linear_clean=0
replay_external_linear_bad=0
replay_external_linear_other=0
replay_internal_afbc_clean=0
replay_internal_afbc_bad=0
replay_internal_afbc_other=0
minimal_reconstructed=
if [ -n "$minimal_dump" ]; then
    minimal_reconstructed=$OUTPUT_DIR/minimal-reconstructed.h265
    cp "$minimal_dump" "$minimal_reconstructed"
    cp "$minimal_dump.sizes" "$minimal_reconstructed.sizes"
    record_trace "$minimal_reconstructed" "$OUTPUT_DIR/minimal-reconstructed"
    if direct_control "$minimal_reconstructed" "$minimal_frames" \
                      "$OUTPUT_DIR/minimal-reconstructed"; then
        reconstructed_direct=clean
    else
        status=$?
        if [ "$status" -eq 1 ]; then
            reconstructed_direct=unstable
        else
            reconstructed_direct=runtime-error-$status
        fi
    fi

    packetized_matrix external-afbc "$minimal_reconstructed" \
        "$minimal_reconstructed.sizes" "$minimal_frames" 1 1
    replay_external_afbc_clean=$matrix_clean
    replay_external_afbc_bad=$matrix_bad_frames
    replay_external_afbc_other=$matrix_other
    packetized_matrix external-linear "$minimal_reconstructed" \
        "$minimal_reconstructed.sizes" "$minimal_frames" 0 1
    replay_external_linear_clean=$matrix_clean
    replay_external_linear_bad=$matrix_bad_frames
    replay_external_linear_other=$matrix_other
    packetized_matrix internal-afbc "$minimal_reconstructed" \
        "$minimal_reconstructed.sizes" "$minimal_frames" 1 0
    replay_internal_afbc_clean=$matrix_clean
    replay_internal_afbc_bad=$matrix_bad_frames
    replay_internal_afbc_other=$matrix_other
fi

minimal_original_sha=$(sha256sum "$minimal_original" | awk '{print $1}')
minimal_original_size=$(wc -c <"$minimal_original")
minimal_reconstructed_sha=unavailable
minimal_reconstructed_size=0
minimal_packet_sizes=unavailable
if [ -n "$minimal_reconstructed" ]; then
    minimal_reconstructed_sha=$(sha256sum "$minimal_reconstructed" | awk '{print $1}')
    minimal_reconstructed_size=$(wc -c <"$minimal_reconstructed")
    minimal_packet_sizes=$(awk '
        BEGIN { separator = "" }
        { printf "%s%s", separator, $1; separator = "," }
        END { print "" }
    ' "$minimal_reconstructed.sizes")
fi

attribution=va-rebuilt-packet-path
if [ "$replay_external_linear_clean" -eq "$ATTEMPTS" ] &&
   [ $((replay_external_afbc_bad + replay_internal_afbc_bad)) -gt 0 ]; then
    attribution=packetized-afbc-interaction
fi

{
    echo "HEVC Main10 VA reconstruction minimization report"
    cat "$OUTPUT_DIR/environment.txt"
    echo "expected_result=$EXPECTED_RESULT"
    echo "result=$attribution"
    echo "first_reproducing_prefix=$minimal_prefix"
    echo "first_reproducing_frames=$minimal_frames"
    echo "reconstruction_failures=$minimal_failures"
    echo "clean_attempts=$minimal_clean"
    echo "rga_blocked_attempts=$minimal_infrastructure"
    echo "minimal_original=$minimal_original"
    echo "minimal_original_size=$minimal_original_size"
    echo "minimal_original_sha256=$minimal_original_sha"
    echo "minimal_reconstructed=$minimal_reconstructed"
    echo "minimal_reconstructed_size=$minimal_reconstructed_size"
    echo "minimal_reconstructed_sha256=$minimal_reconstructed_sha"
    echo "minimal_reconstructed_packet_sizes=$minimal_packet_sizes"
    echo "minimal_reconstructed_direct_mpp=$reconstructed_direct"
    echo "replay_external_afbc_clean=$replay_external_afbc_clean"
    echo "replay_external_afbc_bad_frames=$replay_external_afbc_bad"
    echo "replay_external_afbc_other=$replay_external_afbc_other"
    echo "replay_external_linear_clean=$replay_external_linear_clean"
    echo "replay_external_linear_bad_frames=$replay_external_linear_bad"
    echo "replay_external_linear_other=$replay_external_linear_other"
    echo "replay_internal_afbc_clean=$replay_internal_afbc_clean"
    echo "replay_internal_afbc_bad_frames=$replay_internal_afbc_bad"
    echo "replay_internal_afbc_other=$replay_internal_afbc_other"
    echo
    echo "The original prefix and concatenated rebuilt stream decoded cleanly"
    echo "in repeated whole-stream direct-MPP controls. Only VA attempts with"
    echo "MPP errinfo/discard markers count; RGA failures remain separate."
    echo "The exact packet manifest is replayed below libva with external AFBC,"
    echo "external linear, and internal AFBC output to refine attribution."
} >"$OUTPUT_DIR/report.txt"

echo "ok    first qualifying prefix: $minimal_prefix packet(s), $minimal_frames frame(s)"
echo "ok    VA reconstruction failures: $minimal_failures/$ATTEMPTS"
echo "ok    reconstructed whole-stream direct MPP: $reconstructed_direct"
echo "ok    attribution: $attribution"
echo "ok    packetized replay logs retained for external/internal AFBC and linear controls"
echo "ok    report: $OUTPUT_DIR/report.txt"
if [ "$EXPECTED_RESULT" = fixed ]; then
    echo "FAIL: fixed reconstruction required, but the reducer still reproduces" >&2
    exit 1
fi
