#!/bin/sh
# Software-vs-VAAPI bit-exactness gate for rockchip-vaapi.
#
# H.264 and VP9 inverse transforms are spec-exact. A decoded-frame mismatch is
# therefore a correctness failure, not hardware tolerance.

set -u

FFMPEG=${FFMPEG:-ffmpeg}
SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH='' cd -- "$SCRIPT_DIR/.." && pwd)
DRIVER_DIR=${DRIVER_DIR:-$REPO_ROOT}
RENDER_NODE=${RENDER_NODE:-/dev/dri/renderD128}
VECTOR_DIR=${VECTOR_DIR:-$SCRIPT_DIR/vectors}
MANIFEST=${MANIFEST:-$SCRIPT_DIR/conformance-vectors.tsv}
TEST_SET=${TEST_SET:-all}
RISKY_VECTORS=${RISKY_VECTORS:-skip}
RISKY_KERNEL_RELEASE=${RISKY_KERNEL_RELEASE:-6.18.38-current-rockchip64}
RISKY_KERNEL_NOTES_SHA256=${RISKY_KERNEL_NOTES_SHA256:-db292410e58bd9c658a0b32b6fc7c7895f3ac4a349ae3c292c441e92e340690e}
ALLOW_QUARANTINE=${ALLOW_QUARANTINE:-0}
VP9_RUNS=${VP9_RUNS:-5}
KEEP_WORK=${KEEP_WORK:-0}

case $TEST_SET in
    all|conformance|synthetic) ;;
    *) echo "error: TEST_SET must be all, conformance, or synthetic" >&2; exit 2 ;;
esac
case $RISKY_VECTORS in
    skip|run) ;;
    *) echo "error: RISKY_VECTORS must be skip or run" >&2; exit 2 ;;
esac

# A typo or stale CI checkbox must not turn a conformance run into a kernel
# panic. Kernel-crash vectors are enabled only on the exact release and GNU
# notes fingerprint of the build whose RK3588 VP9 probability-table bounds fix
# was audited. The fingerprint distinguishes audited build #4 from vulnerable
# build #1, which deliberately shares the same release string. Future kernel
# builds must be reviewed and then named explicitly through both variables.
if [ "$RISKY_VECTORS" = run ] && [ "$TEST_SET" != synthetic ]; then
    running_kernel=$(uname -r) || exit 2
    running_notes_sha=$(sha256sum /sys/kernel/notes 2>/dev/null |
        awk '{print $1}')
    if [ "$running_kernel" != "$RISKY_KERNEL_RELEASE" ]; then
        echo "error: refusing kernel-crash vectors on $running_kernel" >&2
        echo "error: audited kernel release is $RISKY_KERNEL_RELEASE" >&2
        exit 2
    fi
    if [ -z "$running_notes_sha" ] ||
       [ "$running_notes_sha" != "$RISKY_KERNEL_NOTES_SHA256" ]; then
        echo "error: refusing kernel-crash vectors on unaudited kernel build" >&2
        echo "error: running kernel notes sha256 is ${running_notes_sha:-unavailable}" >&2
        echo "error: audited kernel notes sha256 is $RISKY_KERNEL_NOTES_SHA256" >&2
        exit 2
    fi
fi

export LIBVA_DRIVER_NAME=rockchip
export LIBVA_DRIVERS_PATH="$DRIVER_DIR"

WORK=$(mktemp -d "$REPO_ROOT/.test-work.validate.XXXXXX") || exit 1
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

FAIL=0
BLOCKED=0
INDEX=0

checksum()
{
    sha256sum "$1" | awk '{print $1}'
}

sw_md5()
{
    "$FFMPEG" -nostdin -y -v error -i "$1" -an -vf format=yuv420p \
        -f framemd5 "$2" >"$2.log" 2>&1
}

hw_md5()
{
    if [ "$3" = vaapi ]; then
        "$FFMPEG" -nostdin -y -v error -hwaccel vaapi \
            -hwaccel_output_format vaapi -vaapi_device "$RENDER_NODE" \
            -i "$1" -an \
            -vf 'hwdownload,format=nv12,format=yuv420p' \
            -f framemd5 "$2" >"$2.log" 2>&1
    else
        "$FFMPEG" -nostdin -y -v error -hwaccel vaapi \
            -vaapi_device "$RENDER_NODE" -i "$1" -an \
            -vf format=yuv420p -f framemd5 "$2" \
            >"$2.log" 2>&1
    fi
}

compare_clip()
{
    label=$1
    input=$2
    decode_path=$3
    INDEX=$((INDEX + 1))
    sw=$WORK/$INDEX.sw.md5
    hw=$WORK/$INDEX.hw.md5
    unexpected_hw=$WORK/$INDEX.unexpected-hw.md5

    if ! sw_md5 "$input" "$sw"; then
        echo "FAIL  $label (software reference decode errored)"
        tail -20 "$sw.log"
        FAIL=1
    elif [ "$decode_path" = software-fallback ] && \
         hw_md5 "$input" "$unexpected_hw" vaapi; then
        echo "FAIL  $label (VA-API unexpectedly accepted a fallback-only profile)"
        FAIL=1
    elif [ "$decode_path" = software-fallback ] && \
         ! grep -q 'Failed setup for format vaapi' "$unexpected_hw.log"; then
        echo "FAIL  $label (forced VA-API failed for an unexpected reason)"
        tail -20 "$unexpected_hw.log"
        FAIL=1
    elif ! hw_md5 "$input" "$hw" "$decode_path"; then
        echo "FAIL  $label ($decode_path decode errored)"
        tail -20 "$hw.log"
        FAIL=1
    elif cmp -s "$sw" "$hw"; then
        echo "ok    $label bit-exact ($decode_path)"
    else
        frames=$(grep -vc '^#' "$sw")
        differing=$(diff "$sw" "$hw" | grep -c '^<' || true)
        echo "FAIL  $label ($differing of $frames frames differ)"
        FAIL=1
    fi
}

run_conformance()
{
    echo "== Pinned conformance vectors =="
    tab=$(printf '\t')
    while IFS="$tab" read -r codec output download url download_sha member payload_sha decode_path risk <&3; do
        case $codec in
            ''|'#'*) continue ;;
        esac
        : "$download" "$url" "$download_sha" "$member"
        input=$VECTOR_DIR/$output
        if [ ! -f "$input" ]; then
            echo "FAIL  $codec/$output (missing; run 'make fetch-vectors')"
            FAIL=1
            continue
        fi
        actual=$(checksum "$input")
        if [ "$actual" != "$payload_sha" ]; then
            echo "FAIL  $codec/$output (payload checksum mismatch)"
            FAIL=1
            continue
        fi
        if [ "$risk" != safe ] && [ "$RISKY_VECTORS" != run ]; then
            echo "BLOCK $codec/$output ($risk; set RISKY_VECTORS=run only on a fixed kernel)"
            BLOCKED=1
            continue
        fi
        case $decode_path in
            vaapi|software-fallback) ;;
            *) echo "FAIL  $codec/$output (invalid decode path $decode_path)"; FAIL=1; continue ;;
        esac
        compare_clip "$codec/$output" "$input" "$decode_path"
    done 3<"$MANIFEST"
}

generate()
{
    output=$1
    shift
    "$FFMPEG" -nostdin -y -v error -f lavfi \
        -i testsrc2=size=1280x720:rate=30:duration=4 \
        "$@" -pix_fmt yuv420p "$WORK/$output" >"$WORK/$output.log" 2>&1
}

run_synthetic()
{
    echo "== Supplemental H.264 reference/B-frame matrix =="
    for cfg in ref=1:bframes=2 ref=2:bframes=0 ref=2:bframes=3 \
               ref=4:bframes=3 ref=8:bframes=0 ref=8:bframes=3; do
        clip="h264_$(printf %s "$cfg" | tr '=:' '__').mp4"
        if generate "$clip" -c:v libx264 -profile:v high -x264-params "$cfg"; then
            compare_clip "h264 $cfg" "$WORK/$clip" vaapi
        else
            echo "FAIL  h264 $cfg (test clip generation errored)"
            FAIL=1
        fi
    done

    echo "== Supplemental 4K H.264 =="
    if "$FFMPEG" -nostdin -y -v error -f lavfi \
        -i testsrc2=size=3840x2160:rate=30:duration=4 \
        -c:v libx264 -x264-params ref=3:bframes=2 -pix_fmt yuv420p \
        "$WORK/h264_4k.mp4" >"$WORK/h264_4k.mp4.log" 2>&1; then
        compare_clip "h264 4K ref=3:bframes=2" "$WORK/h264_4k.mp4" vaapi
    else
        echo "FAIL  h264 4K (test clip generation errored)"
        FAIL=1
    fi

    echo "== Supplemental VP9 determinism (x$VP9_RUNS) =="
    if generate vp9.webm -c:v libvpx-vp9 -b:v 1M; then
        i=1
        while [ "$i" -le "$VP9_RUNS" ]; do
            compare_clip "vp9 run $i" "$WORK/vp9.webm" vaapi
            i=$((i + 1))
        done
    else
        echo "FAIL  vp9 (test clip generation errored)"
        FAIL=1
    fi

    echo "== Unadvertised-codec software fallback =="
    if generate vp8.webm -c:v libvpx -b:v 1M && \
       "$FFMPEG" -nostdin -y -v error -hwaccel vaapi -vaapi_device "$RENDER_NODE" \
           -i "$WORK/vp8.webm" -an -f null - >"$WORK/vp8-fallback.log" 2>&1; then
        echo "ok    vp8 software fallback decodes"
    else
        echo "FAIL  vp8 software fallback"
        FAIL=1
    fi
}

case $TEST_SET in
    all)         run_conformance; run_synthetic ;;
    conformance) run_conformance ;;
    synthetic)   run_synthetic ;;
esac

if [ "$BLOCKED" -ne 0 ] && [ "$ALLOW_QUARANTINE" != 1 ]; then
    echo "BLOCKED REQUIRED VECTORS"
    FAIL=1
fi

if [ "$FAIL" -eq 0 ]; then
    if [ "$BLOCKED" -ne 0 ]; then
        echo "SAFE SUBSET GREEN; FULL GATE STILL BLOCKED"
    else
        echo "ALL GREEN"
    fi
else
    echo "FAILURES PRESENT"
fi
exit "$FAIL"
