#!/bin/sh
# Triage candidate HEVC Main conformance vectors for the pinned manifest.
#
# tests/validate.sh only runs vectors that are already pinned in
# tests/conformance-vectors.tsv. This script is the step before that: it takes
# a directory of candidate streams, rejects the ones that are not HEVC Main
# 8-bit 4:2:0, decodes each through direct MPP and then through VA-API, and
# reports which ones are bit-exact against software.
#
# Direct MPP runs first on purpose. A vector that the backend itself cannot
# decode is a backend finding, not a driver finding, and must not be laundered
# through the VA-API gate as though the driver had been proven wrong.
#
# Output is a TSV report plus manifest-ready rows for the bit-exact vectors.
# Nothing here edits the manifest; promoting a vector stays a human decision.
#
# Usage:
#   tests/sweep-hevc-conformance.sh CANDIDATE_DIR [REPORT_DIR]
#
# Environment:
#   FFMPEG FFPROBE REPRO RENDER_NODE FFMPEG_TIMEOUT DRIVER_DIR BASE_URL JOBS

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

export LIBVA_DRIVER_NAME=rockchip
export LIBVA_DRIVERS_PATH="$DRIVER_DIR"
export RK_VAAPI_EXPERIMENTAL_PROFILES=hevc-main

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
# on it. Unequal bit depths and non-4:2:0 chroma are out of scope for Main.
classify()
{
    "$FFPROBE" -v error -select_streams v:0 \
        -show_entries stream=codec_name,profile,pix_fmt \
        -of default=nw=1:nk=1 "$1" 2>/dev/null | tr '\n' ' '
}

report()
{
    printf '%s\t%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$4" "$5" >>"$REPORT"
    printf '%-8s %-44s %s\n' "$1" "$2" "$5"
}

TOTAL=0
SKIPPED=0
BACKEND_FAILED=0
DRIVER_FAILED=0
UNSUPPORTED=0
EXACT=0

for input in "$CANDIDATE_DIR"/*.bit "$CANDIDATE_DIR"/*.bin; do
    [ -f "$input" ] || continue
    name=$(basename "$input")
    TOTAL=$((TOTAL + 1))

    info=$(classify "$input")
    codec=$(echo "$info" | awk '{print $1}')
    profile=$(echo "$info" | awk '{print $2}')
    pix_fmt=$(echo "$info" | awk '{print $3}')
    if [ "$codec" != hevc ] || [ "$profile" != Main ] ||
       [ "$pix_fmt" != yuv420p ]; then
        SKIPPED=$((SKIPPED + 1))
        report skip "$name" - - "not HEVC Main 8-bit 4:2:0 (${info:-unreadable})"
        continue
    fi

    sw=$WORK/sw.md5
    if ! run_ffmpeg -nostdin -y -v error -i "$input" -an -vf format=yuv420p \
            -f framemd5 "$sw" >"$WORK/sw.log" 2>&1; then
        SKIPPED=$((SKIPPED + 1))
        report skip "$name" - - "software reference decode errored"
        continue
    fi
    frames=$(grep -vc '^#' "$sw")

    # Direct MPP first: separate backend failures from driver failures.
    if "$REPRO" "$input" "$frames" >"$WORK/repro.log" 2>&1; then
        :
    else
        repro_status=$?
        BACKEND_FAILED=$((BACKEND_FAILED + 1))
        report backend "$name" "$frames" - \
            "direct MPP decode failed (exit $repro_status)"
        continue
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
            -vf "hwdownload,format=nv12,${crop}format=yuv420p" \
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
        DRIVER_FAILED=$((DRIVER_FAILED + 1))
        report driver "$name" "$frames" - "VA-API decode errored"
        continue
    fi
    if ! cmp -s "$sw" "$hw"; then
        differing=$(diff "$sw" "$hw" | grep -c '^<' || true)
        DRIVER_FAILED=$((DRIVER_FAILED + 1))
        report driver "$name" "$frames" "$differing" \
            "$differing of $frames frames differ"
        continue
    fi

    EXACT=$((EXACT + 1))
    sha=$(sha256sum "$input" | awk '{print $1}')
    report exact "$name" "$frames" 0 "bit-exact"
    printf 'hevc\t%s\t%s\t%s/%s\t%s\t-\t%s\tvaapi\tsafe\n' \
        "$name" "$name" "$BASE_URL" "$name" "$sha" "$sha" >>"$ROWS"
done

echo
echo "candidates=$TOTAL skipped=$SKIPPED backend-failed=$BACKEND_FAILED" \
     "unsupported=$UNSUPPORTED driver-failed=$DRIVER_FAILED bit-exact=$EXACT"
echo "report:        $REPORT"
echo "manifest rows: $ROWS"

[ "$DRIVER_FAILED" -eq 0 ]
