#!/bin/sh
# Verify the fixed HEVC TILES backend or reduce an older failing MPP build.
#
# The control stream must decode cleanly before a TILES result is accepted.
# This prevents a broken kernel/device stack from being mislabeled as a
# stream-specific MPP failure.

set -eu

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH='' cd -- "$SCRIPT_DIR/.." && pwd)
FFMPEG=${FFMPEG:-/usr/bin/ffmpeg}
FFPROBE=${FFPROBE:-/usr/bin/ffprobe}
REPRO=${REPRO:-$SCRIPT_DIR/hevc_mpp_repro}
INPUT=${1:-$SCRIPT_DIR/vectors/TILES_A_Cisco_2.bit}
OUTPUT_DIR=${2:-}
CONTROL=${CONTROL:-$SCRIPT_DIR/vectors/PPS_A_qualcomm_7.bit}
MAX_PREFIXES=${MAX_PREFIXES:-0}
ALLOW_UNPINNED=${ALLOW_UNPINNED:-0}
EXPECTED_RESULT=${EXPECTED_RESULT:-failure}

TILES_SHA256=eff78a401ecccc21d995345988f1be60ee76604cf10fa39d421c3e00668a94d6
CONTROL_SHA256=2382b56a09bc9bff001cc4b1e90826219c17324de944acce5dde490032322128

usage()
{
    echo "usage: $0 [TILES_A_Cisco_2.bit [OUTPUT_DIR]]" >&2
    echo "environment: CONTROL FFMPEG FFPROBE REPRO MAX_PREFIXES ALLOW_UNPINNED EXPECTED_RESULT" >&2
}

case ${1:-} in
    -h|--help)
        usage
        exit 0
        ;;
esac

for command in "$FFMPEG" "$FFPROBE" "$REPRO" sha256sum awk date grep wc uname; do
    if [ ! -x "$command" ] && ! command -v "$command" >/dev/null 2>&1; then
        echo "error: required command not found or executable: $command" >&2
        exit 2
    fi
done
if [ ! -f "$INPUT" ] || [ ! -f "$CONTROL" ]; then
    echo "error: input or control vector missing; run 'make fetch-vectors'" >&2
    exit 2
fi
case $MAX_PREFIXES in
    ''|*[!0-9]*) echo "error: MAX_PREFIXES must be a non-negative integer" >&2; exit 2 ;;
esac
case $ALLOW_UNPINNED in
    0|1) ;;
    *) echo "error: ALLOW_UNPINNED must be 0 or 1" >&2; exit 2 ;;
esac
case $EXPECTED_RESULT in
    fixed|failure) ;;
    *) echo "error: EXPECTED_RESULT must be fixed or failure" >&2; exit 2 ;;
esac

input_sha=$(sha256sum "$INPUT" | awk '{print $1}')
control_sha=$(sha256sum "$CONTROL" | awk '{print $1}')
if [ "$ALLOW_UNPINNED" != 1 ] && {
       [ "$input_sha" != "$TILES_SHA256" ] ||
       [ "$control_sha" != "$CONTROL_SHA256" ]; }; then
    echo "error: vector checksum mismatch" >&2
    echo "input:   $input_sha" >&2
    echo "control: $control_sha" >&2
    exit 2
fi

if [ -z "$OUTPUT_DIR" ]; then
    OUTPUT_DIR=$(mktemp -d "$REPO_ROOT/.test-work.hevc-tiles-backend.XXXXXX") ||
        exit 1
else
    mkdir -p "$OUTPUT_DIR"
    OUTPUT_DIR=$(CDPATH='' cd -- "$OUTPUT_DIR" && pwd)
fi

repro_sha=$(sha256sum "$REPRO" | awk '{print $1}')
if command -v pkg-config >/dev/null 2>&1; then
    mpp_pkgconfig_version=$(pkg-config --modversion rockchip_mpp 2>/dev/null ||
        echo unavailable)
else
    mpp_pkgconfig_version=unavailable
fi
if command -v dpkg-query >/dev/null 2>&1; then
    mpp_package_version=$(dpkg-query -W -f='${Version}' \
        librockchip-mpp1 2>/dev/null || echo unavailable)
else
    mpp_package_version=unavailable
fi
kernel_notes_sha=unavailable
if [ -r /sys/kernel/notes ]; then
    kernel_notes_sha=$(sha256sum /sys/kernel/notes | awk '{print $1}')
fi
{
    echo "date_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "kernel=$(uname -a)"
    echo "kernel_notes_sha256=$kernel_notes_sha"
    echo "input=$INPUT"
    echo "input_sha256=$input_sha"
    echo "control=$CONTROL"
    echo "control_sha256=$control_sha"
    echo "reproducer=$REPRO"
    echo "reproducer_sha256=$repro_sha"
    echo "mpp_pkgconfig_version=$mpp_pkgconfig_version"
    echo "mpp_package_version=$mpp_package_version"
} >"$OUTPUT_DIR/environment.txt"

packet_count()
{
    "$FFPROBE" -v error -select_streams v:0 -count_packets \
        -show_entries stream=nb_read_packets -of default=nk=1:nw=1 "$1"
}

frame_count()
{
    "$FFPROBE" -v error -codec_whitelist hevc -select_streams v:0 -count_frames \
        -show_entries stream=nb_read_frames -of default=nk=1:nw=1 "$1"
}

software_check()
{
    "$FFMPEG" -nostdin -v error -c:v hevc -i "$1" -an -f null - \
        >"$2" 2>&1
}

positive_count()
{
    case $1 in
        ''|N/A|*[!0-9]*|0) return 1 ;;
        *) return 0 ;;
    esac
}

backend_check()
{
    stream=$1
    expected=$2
    log=$3
    set +e
    "$REPRO" "$stream" "$expected" >"$log" 2>&1
    status=$?
    set -e
    return "$status"
}

record_stream_details()
{
    details_stream=$1
    "$REPRO" --inspect "$details_stream" >"$OUTPUT_DIR/nal-inventory.txt" \
        2>"$OUTPUT_DIR/nal-inspect-runtime.log"
    "$FFMPEG" -nostdin -v trace -i "$details_stream" -map 0:v:0 -c:v copy \
        -bsf:v trace_headers -f null - >/dev/null \
        2>"$OUTPUT_DIR/trace-headers.log"
    grep -E 'Video Parameter Set|Sequence Parameter Set|Picture Parameter Set|Slice Segment Header|tiles_enabled_flag|entropy_coding_sync_enabled_flag|num_tile_(columns|rows)_minus1|uniform_spacing_flag|column_width_minus1|row_height_minus1|loop_filter_across_tiles_enabled_flag|slice_pic_parameter_set_id|slice_type|slice_pic_order_cnt_lsb' \
        "$OUTPUT_DIR/trace-headers.log" >"$OUTPUT_DIR/tile-layouts.txt" || true
}

control_frames=$(frame_count "$CONTROL")
if ! positive_count "$control_frames"; then
    echo "error: could not count control frames" >&2
    exit 2
fi
if backend_check "$CONTROL" "$control_frames" "$OUTPUT_DIR/control-mpp.log"; then
    :
else
    status=$?
    echo "BLOCK environment: known-good HEVC control did not decode cleanly (status=$status)" >&2
    echo "log: $OUTPUT_DIR/control-mpp.log" >&2
    exit 3
fi

total_packets=$(packet_count "$INPUT")
full_frames=$(frame_count "$INPUT")
for value in "$total_packets" "$full_frames"; do
    if ! positive_count "$value"; then
        echo "error: could not count input packets/frames" >&2
        exit 2
    fi
done

if backend_check "$INPUT" "$full_frames" "$OUTPUT_DIR/full-mpp.log"; then
    if [ "$EXPECTED_RESULT" = failure ]; then
        echo "NOT_REPRODUCED: full TILES vector decoded cleanly" >&2
        exit 1
    fi

    fixed_prefix="$OUTPUT_DIR/fixed-prefix-002.h265"
    fixed_core="$OUTPUT_DIR/fixed-core.h265"
    "$FFMPEG" -nostdin -y -v error -i "$INPUT" -map 0:v:0 -c:v copy \
        -frames:v 2 -f hevc "$fixed_prefix"
    "$FFMPEG" -nostdin -y -v error -i "$fixed_prefix" -map 0:v:0 -c:v copy \
        -bsf:v 'filter_units=remove_types=35|36|37|38|39|40' \
        -f hevc "$fixed_core"
    if ! software_check "$fixed_core" "$OUTPUT_DIR/fixed-core-software.log"; then
        echo "error: fixed two-picture core failed software decode" >&2
        exit 2
    fi
    fixed_frames=$(frame_count "$fixed_core")
    if ! positive_count "$fixed_frames" || [ "$fixed_frames" -ne 2 ]; then
        echo "error: fixed core has $fixed_frames frames, expected 2" >&2
        exit 2
    fi
    if backend_check "$fixed_core" "$fixed_frames" "$OUTPUT_DIR/fixed-core-mpp.log"; then
        :
    else
        status=$?
        if [ "$status" -eq 1 ]; then
            echo "FAIL: full TILES passed but the same-ID PPS core failed" >&2
            exit 1
        fi
        echo "BLOCK environment: fixed core hit MPP runtime/setup failure (status=$status)" >&2
        exit 3
    fi

    record_stream_details "$fixed_core"
    fixed_sha=$(sha256sum "$fixed_core" | awk '{print $1}')
    fixed_size=$(wc -c <"$fixed_core")
    {
        echo "HEVC TILES direct-MPP fixed-backend report"
        cat "$OUTPUT_DIR/environment.txt"
        echo "expected_result=$EXPECTED_RESULT"
        echo "input_packets=$total_packets"
        echo "input_frames=$full_frames"
        echo "control_frames=$control_frames"
        echo "fixed_core_file=${fixed_core##*/}"
        echo "fixed_core_frames=$fixed_frames"
        echo "fixed_core_size=$fixed_size"
        echo "fixed_core_sha256=$fixed_sha"
        echo
        echo "The known-good control, complete TILES vector, and two-picture"
        echo "same-ID PPS core all decoded cleanly through direct MPP."
    } >"$OUTPUT_DIR/report.txt"
    echo "ok    full TILES vector: $full_frames frames clean"
    echo "ok    same-ID PPS core: $fixed_frames frames clean ($fixed_size bytes)"
    echo "ok    report: $OUTPUT_DIR/report.txt"
    exit 0
else
    full_status=$?
    if [ "$full_status" -ne 1 ]; then
        echo "BLOCK environment: full vector hit MPP runtime/setup failure (status=$full_status)" >&2
        exit 3
    fi
fi

limit=$total_packets
if [ "$MAX_PREFIXES" -gt 0 ] && [ "$MAX_PREFIXES" -lt "$limit" ]; then
    limit=$MAX_PREFIXES
fi

minimal=
minimal_packets=0
n=1
while [ "$n" -le "$limit" ]; do
    candidate=$(printf '%s/prefix-%03d.h265' "$OUTPUT_DIR" "$n")
    "$FFMPEG" -nostdin -y -v error -i "$INPUT" -map 0:v:0 -c:v copy \
        -frames:v "$n" -f hevc "$candidate"
    sw_log=$(printf '%s/prefix-%03d-software.log' "$OUTPUT_DIR" "$n")
    if ! software_check "$candidate" "$sw_log"; then
        echo "skip  prefix=$n software decode rejected truncated stream"
        n=$((n + 1))
        continue
    fi

    candidate_frames=$(frame_count "$candidate")
    if ! positive_count "$candidate_frames"; then
        echo "error: could not count frames in prefix $n" >&2
        exit 2
    fi
    direct_log=$(printf '%s/prefix-%03d-mpp.log' "$OUTPUT_DIR" "$n")
    if backend_check "$candidate" "$candidate_frames" "$direct_log"; then
        echo "clean prefix=$n frames=$candidate_frames"
    else
        status=$?
        if [ "$status" -eq 1 ]; then
            echo "repro prefix=$n frames=$candidate_frames"
            minimal=$candidate
            minimal_packets=$n
            break
        fi
        echo "BLOCK environment: prefix=$n hit MPP runtime/setup failure (status=$status)" >&2
        exit 3
    fi
    n=$((n + 1))
done

if [ -z "$minimal" ]; then
    echo "error: no failing prefix found in first $limit packets" >&2
    exit 1
fi

core="$OUTPUT_DIR/minimal-core.h265"
"$FFMPEG" -nostdin -y -v error -i "$minimal" -map 0:v:0 -c:v copy \
    -bsf:v 'filter_units=remove_types=35|36|37|38|39|40' -f hevc "$core"
core_status=2
core_software_ok=0
if software_check "$core" "$OUTPUT_DIR/minimal-core-software.log"; then
    core_software_ok=1
    core_frames=$(frame_count "$core")
    if positive_count "$core_frames"; then
        if backend_check "$core" "$core_frames" "$OUTPUT_DIR/minimal-core-mpp.log"; then
            core_status=0
        else
            core_status=$?
        fi
    else
        echo "error: could not count frames in stripped prefix" >&2
        exit 2
    fi
fi
if [ "$core_status" -eq 1 ]; then
    minimized="$core"
elif [ "$core_software_ok" -eq 1 ] && [ "$core_status" -gt 1 ]; then
    echo "BLOCK environment: stripped prefix hit MPP runtime/setup failure (status=$core_status)" >&2
    exit 3
else
    minimized="$minimal"
fi

record_stream_details "$minimized"

minimized_sha=$(sha256sum "$minimized" | awk '{print $1}')
minimized_size=$(wc -c <"$minimized")
minimal_size=$(wc -c <"$minimal")
minimized_name=${minimized##*/}

{
    echo "HEVC TILES direct-MPP minimization report"
    cat "$OUTPUT_DIR/environment.txt"
    echo "expected_result=$EXPECTED_RESULT"
    echo "input_packets=$total_packets"
    echo "input_frames=$full_frames"
    echo "control_frames=$control_frames"
    echo "first_failing_prefix_packets=$minimal_packets"
    echo "first_failing_prefix_size=$minimal_size"
    echo "minimized_file=$minimized_name"
    echo "minimized_size=$minimized_size"
    echo "minimized_sha256=$minimized_sha"
    echo "nonessential_nals_removed=$([ "$minimized" = "$core" ] && echo yes || echo no)"
    echo
    echo "The known-good control decoded cleanly before this result was accepted."
    echo "The minimized stream decodes with FFmpeg's software HEVC decoder and"
    echo "returns hevc_mpp_repro status=stream-error through direct MPP."
} >"$OUTPUT_DIR/report.txt"

echo "ok    first failing prefix: $minimal_packets packet(s)"
echo "ok    minimized stream: $minimized ($minimized_size bytes)"
echo "ok    report: $OUTPUT_DIR/report.txt"
if [ "$EXPECTED_RESULT" = fixed ]; then
    echo "FAIL: fixed backend required, but TILES still reproduces" >&2
    exit 1
fi
