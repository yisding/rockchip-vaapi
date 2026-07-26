#!/bin/sh
# GStreamer va plugin integration and byte-exact system-memory output gate.

set -eu

if [ -z "${FFMPEG:-}" ]; then
    if [ -x /usr/bin/ffmpeg ]; then
        FFMPEG=/usr/bin/ffmpeg
    else
        FFMPEG=ffmpeg
    fi
fi
FFMPEG_TIMEOUT=${FFMPEG_TIMEOUT:-180}
SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH='' cd -- "$SCRIPT_DIR/.." && pwd)
DRIVER_DIR=${DRIVER_DIR:-$REPO_ROOT}
KEEP_WORK=${KEEP_WORK:-0}

case $FFMPEG_TIMEOUT in
    ''|*[!0-9]*) echo "error: FFMPEG_TIMEOUT must be an integer" >&2; exit 2 ;;
esac
if [ "$FFMPEG_TIMEOUT" != 0 ] &&
   ! command -v timeout >/dev/null 2>&1; then
    echo "error: timeout is required when FFMPEG_TIMEOUT is non-zero" >&2
    exit 2
fi
for command in gst-inspect-1.0 gst-launch-1.0 "$FFMPEG"; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "error: required command not found: $command" >&2
        exit 2
    fi
done

WORK=$(mktemp -d "$REPO_ROOT/.test-work.gstreamer-va.XXXXXX") || exit 1
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

run_bounded()
{
    if [ "$FFMPEG_TIMEOUT" = 0 ]; then
        "$@"
    else
        timeout --kill-after=5s "$FFMPEG_TIMEOUT" "$@"
    fi
}

export GST_VA_ALL_DRIVERS=1
export GST_REGISTRY="$WORK/registry.bin"
export LIBVA_DRIVER_NAME=rockchip
export LIBVA_DRIVERS_PATH="$DRIVER_DIR"
export RK_VAAPI_EXPERIMENTAL_PROFILES=all

plugin_log=$WORK/gst-inspect-va.log
run_bounded gst-inspect-1.0 va >"$plugin_log" 2>&1
for element in vah264dec vah265dec vavp9dec; do
    if ! grep -q "^  $element:" "$plugin_log"; then
        echo "FAIL  GStreamer va plugin did not register $element" >&2
        tail -40 "$plugin_log" >&2
        exit 1
    fi
done

verify_vector()
{
    vector=$1
    expected=$2
    if [ ! -f "$vector" ]; then
        echo "FAIL  vector missing: $vector; run 'make fetch-vectors'" >&2
        exit 1
    fi
    actual=$(sha256sum "$vector" | awk '{print $1}')
    if [ "$actual" != "$expected" ]; then
        echo "FAIL  vector checksum mismatch: $vector" >&2
        exit 1
    fi
}

h264=$SCRIPT_DIR/vectors/HCAFR1_HHI.264
vp9p0=$SCRIPT_DIR/vectors/vp90-2-15-segkey.webm
vp9p2=$SCRIPT_DIR/vectors/vp92-2-20-10bit-yuv420.webm
main10=$SCRIPT_DIR/vectors/WP_A_MAIN10_Toshiba_3.bit
verify_vector "$h264" bc6b6fe1d3860c88eb6564125c20c3cd272240bf6a22b4fb93876c64cb1b6e83
verify_vector "$vp9p0" 2809d3c237383bcecffee926c624dbbccd2146331cc569344caabbd8b8141449
verify_vector "$vp9p2" c4b56b148d5039aa824fde3d4877dbd2604d0de7f77af96f4ba1ade537396a38
verify_vector "$main10" 3be359e9c70f56e478bb0c0710dcea112252541570f81856cc9f3dba3d988263

software_decode()
{
    name=$1
    input=$2
    pixel_format=$3
    run_bounded "$FFMPEG" -nostdin -y -v error -i "$input" -an \
        -pix_fmt "$pixel_format" -fps_mode passthrough -f rawvideo \
        "$WORK/$name.software.raw"
}

gstreamer_decode()
{
    name=$1
    expected_frames=$2
    expected_conversions=$3
    shift 3
    driver_log=$WORK/$name.driver.log
    pipeline_log=$WORK/$name.pipeline.log
    RK_VAAPI_LOG=$driver_log \
        run_bounded gst-launch-1.0 -q "$@" \
        ! filesink location="$WORK/$name.gstreamer.raw" \
        >"$pipeline_log" 2>&1

    if ! cmp -s "$WORK/$name.software.raw" \
                    "$WORK/$name.gstreamer.raw"; then
        echo "FAIL  GStreamer $name output differs from software" >&2
        exit 1
    fi

    assigned=$(grep -c 'assign_mpp_frame: surface=.*converted_10bit=' \
                       "$driver_log" || true)
    conversions=$(grep -c 'convert: NV15->P010.*afbc=1' \
                          "$driver_log" || true)
    if [ "$assigned" -ne "$expected_frames" ] ||
       [ "$conversions" -ne "$expected_conversions" ] ||
       grep -q 'assign_mpp_frame: stale\|external buffer mismatch\|decode failed\|afbc=0' \
               "$driver_log"; then
        echo "FAIL  GStreamer $name driver audit: assigned=$assigned expected=$expected_frames conversions=$conversions expected_conversions=$expected_conversions" >&2
        exit 1
    fi

    bytes=$(wc -c <"$WORK/$name.gstreamer.raw")
    echo "ok    GStreamer $name $expected_frames decoded frames byte-exact ($bytes bytes)"
}

software_decode h264 "$h264" nv12
gstreamer_decode h264 10 0 \
    filesrc location="$h264" ! h264parse ! vah264dec \
    ! video/x-raw,format=NV12

software_decode vp9-profile0 "$vp9p0" nv12
gstreamer_decode vp9-profile0 1 0 \
    filesrc location="$vp9p0" ! matroskademux ! vp9parse ! vavp9dec \
    ! video/x-raw,format=NV12

software_decode vp9-profile2 "$vp9p2" p010le
gstreamer_decode vp9-profile2 11 11 \
    filesrc location="$vp9p2" ! matroskademux ! vp9parse ! vavp9dec \
    ! video/x-raw,format=P010_10LE

software_decode hevc-main10 "$main10" p010le
gstreamer_decode hevc-main10 256 256 \
    filesrc location="$main10" ! h265parse ! vah265dec \
    ! video/x-raw,format=P010_10LE

echo "ok    GStreamer va app gate passed with GST_VA_ALL_DRIVERS=1"
