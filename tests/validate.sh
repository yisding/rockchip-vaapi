#!/bin/sh
# validate.sh — software-vs-VAAPI bit-exactness gate for rockchip-vaapi.
#
# H.264 and VP9 inverse transforms are spec-exact, so a correct hardware
# decode must produce byte-identical planes to software decode. Any
# difference is a driver bug, not "hardware tolerance".
#
# Requirements: ffmpeg with vaapi + libx264 + libvpx, Rockchip MPP hardware
# (/dev/mpp_service), and a DRM render node.
#
# Environment overrides:
#   FFMPEG       ffmpeg binary            (default: ffmpeg)
#   DRIVER_DIR   dir with the built .so   (default: repo root)
#   RENDER_NODE  DRM node for vaInitialize (default: /dev/dri/renderD128)
#   VP9_RUNS     repeat count for the VP9 determinism check (default: 5)
#
# Exit status: 0 = all green, 1 = any failure.

set -u

FFMPEG=${FFMPEG:-ffmpeg}
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DRIVER_DIR=${DRIVER_DIR:-$SCRIPT_DIR/..}
RENDER_NODE=${RENDER_NODE:-/dev/dri/renderD128}
VP9_RUNS=${VP9_RUNS:-5}

export LIBVA_DRIVER_NAME=rockchip
export LIBVA_DRIVERS_PATH=$DRIVER_DIR

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
FAIL=0

gen() { # gen <out> <codec-args...>
    out=$1; shift
    "$FFMPEG" -y -v error -f lavfi \
        -i testsrc2=size=1280x720:rate=30:duration=4 \
        "$@" -pix_fmt yuv420p "$WORK/$out" 2>/dev/null
}

sw_md5() { # sw_md5 <in> <out>
    "$FFMPEG" -y -v error -i "$WORK/$1" -an -vf format=yuv420p \
        -f framemd5 "$WORK/$2" 2>/dev/null
}

hw_md5() { # hw_md5 <in> <out>  (returns ffmpeg status)
    "$FFMPEG" -y -v error -hwaccel vaapi -vaapi_device "$RENDER_NODE" \
        -i "$WORK/$1" -an -vf format=yuv420p \
        -f framemd5 "$WORK/$2" 2>/dev/null
}

check() { # check <label> <clip>
    sw_md5 "$2" "$1.sw"
    if ! hw_md5 "$2" "$1.hw"; then
        echo "FAIL  $1 (hardware decode errored)"
        FAIL=1
    elif cmp -s "$WORK/$1.sw" "$WORK/$1.hw"; then
        echo "ok    $1 bit-exact"
    else
        n=$(grep -vc '^#' "$WORK/$1.sw")
        d=$("$FFMPEG" -v error 2>/dev/null; diff "$WORK/$1.sw" "$WORK/$1.hw" | grep -c '^<')
        echo "FAIL  $1 ($d of $n frames differ)"
        FAIL=1
    fi
}

echo "== H.264 reference/B-frame matrix =="
for cfg in ref=1:bframes=2 ref=2:bframes=0 ref=2:bframes=3 \
           ref=4:bframes=3 ref=8:bframes=0 ref=8:bframes=3; do
    clip="h264_$(printf %s "$cfg" | tr '=:' '__').mp4"
    gen "$clip" -c:v libx264 -profile:v high -x264-params "$cfg"
    check "h264 $cfg" "$clip"
done

echo "== 4K H.264 =="
"$FFMPEG" -y -v error -f lavfi \
    -i testsrc2=size=3840x2160:rate=30:duration=4 \
    -c:v libx264 -x264-params ref=3:bframes=2 -pix_fmt yuv420p \
    "$WORK/h264_4k.mp4" 2>/dev/null
check "h264 4K ref=3:bframes=2" h264_4k.mp4

echo "== VP9 determinism (x$VP9_RUNS) =="
gen vp9.webm -c:v libvpx-vp9 -b:v 1M
sw_md5 vp9.webm vp9.sw
i=1
while [ "$i" -le "$VP9_RUNS" ]; do
    if ! hw_md5 vp9.webm "vp9.hw$i" || ! cmp -s "$WORK/vp9.sw" "$WORK/vp9.hw$i"; then
        echo "FAIL  vp9 run $i"
        FAIL=1
    else
        echo "ok    vp9 run $i bit-exact"
    fi
    i=$((i + 1))
done

echo "== Unadvertised codecs fall back to software =="
gen vp8.webm -c:v libvpx -b:v 1M
if "$FFMPEG" -y -v error -hwaccel vaapi -vaapi_device "$RENDER_NODE" \
     -i "$WORK/vp8.webm" -an -f null - 2>/dev/null; then
    echo "ok    vp8 (software fallback decodes)"
else
    echo "FAIL  vp8 fallback"
    FAIL=1
fi

[ "$FAIL" -eq 0 ] && echo "ALL GREEN" || echo "FAILURES PRESENT"
exit "$FAIL"
