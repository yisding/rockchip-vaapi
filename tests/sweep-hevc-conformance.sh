#!/bin/sh
# Triage candidate HEVC Main conformance vectors for the pinned manifest.
#
# tests/validate.sh only runs vectors that are already pinned in
# tests/conformance-vectors.tsv. This script is the step before that: it takes
# a directory of candidate streams, rejects the ones that are not HEVC Main
# 8-bit 4:2:0, decodes each through direct MPP and then through VA-API, and
# reports which ones are bit-exact against software.
#
# Every candidate is also submitted to MPP directly, but only to attribute a
# failure. A VA-API failure on a stream the backend could not decode either is
# a backend finding and must not be laundered as a driver defect; the reverse
# does not hold, because the direct-MPP frame count legitimately differs on
# output-order streams where libavcodec, not MPP, decides what is output.
#
# Output is a TSV report plus manifest-ready rows for the bit-exact vectors.
# Nothing here edits the manifest; promoting a vector stays a human decision.
#
# Set EXPECTATIONS to a pinned manifest to run it as a gate instead of a
# survey: every candidate's class must match the manifest's decode_path column
# and the script exits non-zero on the first divergence in either direction.
#
# Usage:
#   tests/sweep-hevc-conformance.sh CANDIDATE_DIR [REPORT_DIR]
#
# PROFILE selects which HEVC profile is under test: "main" (8-bit 4:2:0, the
# default) or "main10". Main10 output is compared as P010, which is also what
# every consumer of this driver receives, because MPP hands back AFBC NV15 that
# RGA repacks.
#
# Environment:
#   FFMPEG FFPROBE REPRO RENDER_NODE FFMPEG_TIMEOUT DRIVER_DIR BASE_URL
#   EXPECTATIONS PROFILE

set -u

FFMPEG=${FFMPEG:-ffmpeg}
FFPROBE=${FFPROBE:-ffprobe}
FFMPEG_TIMEOUT=${FFMPEG_TIMEOUT:-300}
SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH='' cd -- "$SCRIPT_DIR/.." && pwd)
DRIVER_DIR=${DRIVER_DIR:-$REPO_ROOT}
REPRO=${REPRO:-$SCRIPT_DIR/hevc_mpp_repro}
RENDER_NODE=${RENDER_NODE:-/dev/dri/renderD128}
BASE_URL=${BASE_URL:-https://fate-suite.ffmpeg.org/hevc-conformance}
EXPECTATIONS=${EXPECTATIONS:-}
PROFILE=${PROFILE:-main}

case $PROFILE in
    main)
        WANT_PROFILE=Main
        WANT_PIX_FMT=yuv420p
        HW_FORMAT=nv12
        SW_FORMAT=yuv420p
        ;;
    main10)
        WANT_PROFILE="Main 10"
        WANT_PIX_FMT=yuv420p10le
        HW_FORMAT=p010le
        SW_FORMAT=yuv420p10le
        export RK_VAAPI_EXPERIMENTAL_PROFILES=hevc-main10
        ;;
    *)
        echo "error: PROFILE must be main or main10" >&2
        exit 2 ;;
esac

if [ $# -lt 1 ] || [ $# -gt 2 ]; then
    echo "usage: $0 CANDIDATE_DIR [REPORT_DIR]" >&2
    exit 2
fi
CANDIDATE_DIR=$1
REPORT_DIR=${2:-$REPO_ROOT/.test-work.hevc-sweep}

case $FFMPEG_TIMEOUT in
    ''|*[!0-9]*)
        echo "error: FFMPEG_TIMEOUT must be a non-negative integer" >&2
        exit 2 ;;
esac
if [ ! -d "$CANDIDATE_DIR" ]; then
    echo "error: $CANDIDATE_DIR is not a directory" >&2
    exit 2
fi
if [ ! -x "$REPRO" ]; then
    echo "error: $REPRO is missing; run 'make tests/hevc_mpp_repro'" >&2
    exit 2
fi
for tool in "$FFMPEG" "$FFPROBE"; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "error: $tool not found" >&2
        exit 2
    fi
done
# A non-VAAPI FFmpeg would report every vector as a driver failure.
if ! "$FFMPEG" -hide_banner -hwaccels 2>/dev/null |
     grep -qx '[[:space:]]*vaapi[[:space:]]*'; then
    echo "error: $FFMPEG was built without VAAPI support" >&2
    exit 2
fi
if [ ! -e "$RENDER_NODE" ]; then
    echo "error: render node $RENDER_NODE is missing" >&2
    exit 2
fi
if [ -n "$EXPECTATIONS" ] && [ ! -f "$EXPECTATIONS" ]; then
    echo "error: expectations manifest $EXPECTATIONS is missing" >&2
    exit 2
fi

export LIBVA_DRIVER_NAME=rockchip
export LIBVA_DRIVERS_PATH="$DRIVER_DIR"

mkdir -p "$REPORT_DIR" || exit 1
WORK=$(mktemp -d "$REPORT_DIR/work.XXXXXX") || exit 1
# shellcheck disable=SC2317,SC2329 # Invoked by the EXIT trap.
cleanup()
{
    rm -rf "$WORK"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

REPORT=$REPORT_DIR/report.tsv
ROWS=$REPORT_DIR/manifest-rows.tsv
: >"$REPORT"
: >"$ROWS"

run_ffmpeg()
{
    if [ "$FFMPEG_TIMEOUT" = 0 ]; then
        "$FFMPEG" "$@"
    else
        timeout --kill-after=5s "$FFMPEG_TIMEOUT" "$FFMPEG" "$@"
    fi
}

# Reject anything that is not the profile under test before spending a decode
# on it. Unequal luma/chroma bit depths and non-4:2:0 chroma are out of scope.
classify()
{
    "$FFPROBE" -v error -select_streams v:0 \
        -show_entries stream=codec_name,profile,pix_fmt \
        -of default=nw=1:nk=1 "$1" 2>/dev/null | tr '\n' '|'
}

# The manifest's decode_path column carries the expected class for each
# pinned vector. Unknown names and unexpected classes both fail: a vector that
# newly passes is as much a change to record as one that newly fails.
expected_class()
{
    [ -n "$EXPECTATIONS" ] || return 0
    awk -F'\t' -v want="$1" '$1 !~ /^#/ && $2 == want {print $8; found=1}
                             END {if (!found) print "unpinned"}' \
        "$EXPECTATIONS"
}

report()
{
    printf '%s\t%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$4" "$5" >>"$REPORT"
    printf '%-8s %-44s %s\n' "$1" "$2" "$5"

    [ -n "$EXPECTATIONS" ] || return 0
    want=$(expected_class "$2")
    if [ "$1" != "$want" ]; then
        printf 'MISMATCH %s: expected %s, got %s\n' "$2" "$want" "$1"
        UNEXPECTED=$((UNEXPECTED + 1))
    fi
}

# A VA-API failure on a stream direct MPP could not decode either is a backend
# finding. Only a failure the backend handled cleanly is attributable here.
report_failure()
{
    if [ "$5" = yes ]; then
        DRIVER_FAILED=$((DRIVER_FAILED + 1))
        report driver "$1" "$2" "$3" "$4"
    else
        BACKEND_FAILED=$((BACKEND_FAILED + 1))
        report backend "$1" "$2" "$3" "$4 (direct MPP also failed: $5)"
    fi
}

TOTAL=0
SKIPPED=0
BACKEND_FAILED=0
DRIVER_FAILED=0
UNSUPPORTED=0
EXACT=0
UNEXPECTED=0

for input in "$CANDIDATE_DIR"/*.bit "$CANDIDATE_DIR"/*.bin; do
    [ -f "$input" ] || continue
    name=$(basename "$input")
    TOTAL=$((TOTAL + 1))

    info=$(classify "$input")
    codec=${info%%|*}
    rest=${info#*|}
    profile=${rest%%|*}
    pix_fmt=$(echo "$rest" | cut -d'|' -f2)
    if [ "$codec" != hevc ] || [ "$profile" != "$WANT_PROFILE" ] ||
       [ "$pix_fmt" != "$WANT_PIX_FMT" ]; then
        SKIPPED=$((SKIPPED + 1))
        report skip "$name" - - \
            "not HEVC $WANT_PROFILE $WANT_PIX_FMT (${info:-unreadable})"
        continue
    fi

    sw=$WORK/sw.md5
    if ! run_ffmpeg -nostdin -y -v error -i "$input" -an \
            -vf "format=$SW_FORMAT" -f framemd5 "$sw" \
            >"$WORK/sw.log" 2>&1; then
        SKIPPED=$((SKIPPED + 1))
        report skip "$name" - - "software reference decode errored"
        continue
    fi
    frames=$(grep -vc '^#' "$sw")

    # Direct MPP is a classifier, not a gate. It reports how many frames the
    # backend emits for the original Annex-B stream, which differs from the
    # VA-API path for output-order cases -- libavcodec decides what to output
    # there, MPP does not. So record the result and still compare through
    # VA-API; the backend verdict only says whether a VA-API failure is ours.
    backend_ok=yes
    if "$REPRO" "$input" "$frames" >"$WORK/repro.log" 2>&1; then
        :
    else
        backend_ok="no(exit $?)"
    fi

    # libavcodec applies only the right/bottom crop to a hardware frame and
    # leaves the left/top conformance-window offset for the consumer, so
    # re-crop to the display rectangle before comparing.
    size=$("$FFPROBE" -v error -select_streams v:0 \
        -show_entries stream=width,height -of csv=p=0:s=x "$input" 2>/dev/null)
    crop=""
    case $size in
        [0-9]*x[0-9]*) crop="crop=${size%x*}:${size#*x}:in_w-${size%x*}:in_h-${size#*x}," ;;
    esac

    hw=$WORK/hw.md5
    if ! run_ffmpeg -nostdin -y -v error -hwaccel vaapi \
            -hwaccel_output_format vaapi -vaapi_device "$RENDER_NODE" \
            -i "$input" -an \
            -vf "hwdownload,format=$HW_FORMAT,${crop}format=$SW_FORMAT" \
            -f framemd5 "$hw" >"$WORK/hw.log" 2>&1; then
        # A picture larger than the advertised constraints is a correct,
        # documented refusal, not a decode failure. FFmpeg falls back.
        if grep -q 'Hardware does not support image size' "$WORK/hw.log"; then
            UNSUPPORTED=$((UNSUPPORTED + 1))
            report unsup "$name" "$frames" - \
                "$(grep -m1 'Hardware does not support image size' \
                    "$WORK/hw.log" | sed 's/.*\] //')"
            continue
        fi
        report_failure "$name" "$frames" - "VA-API decode errored" \
            "$backend_ok"
        continue
    fi
    if ! cmp -s "$sw" "$hw"; then
        differing=$(diff "$sw" "$hw" | grep -c '^<' || true)
        report_failure "$name" "$frames" "$differing" \
            "$differing of $frames frames differ" "$backend_ok"
        continue
    fi

    EXACT=$((EXACT + 1))
    sha=$(sha256sum "$input" | awk '{print $1}')
    report exact "$name" "$frames" 0 "bit-exact (direct-mpp=$backend_ok)"
    printf 'hevc\t%s\t%s\t%s/%s\t%s\t-\t%s\tvaapi\tsafe\n' \
        "$name" "$name" "$BASE_URL" "$name" "$sha" "$sha" >>"$ROWS"
done

echo
echo "candidates=$TOTAL skipped=$SKIPPED backend-failed=$BACKEND_FAILED" \
     "unsupported=$UNSUPPORTED driver-failed=$DRIVER_FAILED bit-exact=$EXACT"
echo "report:        $REPORT"
echo "manifest rows: $ROWS"

if [ -n "$EXPECTATIONS" ]; then
    # A pinned run must also cover every pinned vector, so a manifest entry
    # whose file is absent cannot pass unnoticed.
    pinned=$(awk -F'\t' '$1 !~ /^#/ && NF >= 8' "$EXPECTATIONS" | wc -l)
    if [ "$TOTAL" -ne "$pinned" ]; then
        echo "error: ran $TOTAL candidates for $pinned pinned vectors" >&2
        exit 1
    fi
    if [ "$UNEXPECTED" -ne 0 ]; then
        echo "error: $UNEXPECTED vectors diverged from the pinned manifest" >&2
        exit 1
    fi
    # The manifest is the verdict for a pinned run. A driver failure recorded
    # there is a known boundary, not a regression; an unrecorded one already
    # failed above.
    echo "ALL PINNED CLASSES MATCH"
    exit 0
fi

[ "$DRIVER_FAILED" -eq 0 ]
