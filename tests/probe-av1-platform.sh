#!/bin/sh
# Enumerate AV1 kernel/userspace endpoints without submitting decode work.

set -eu

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH='' cd -- "$SCRIPT_DIR/.." && pwd)
SYS_ROOT=${AV1_SYS_ROOT:-/sys}
DEV_ROOT=${AV1_DEV_ROOT:-/dev}
V4L2_CTL=${V4L2_CTL:-v4l2-ctl}
MPP_CAPS_PROBE=${MPP_CAPS_PROBE:-$SCRIPT_DIR/av1_mpp_caps}
PROBE_TIMEOUT=${AV1_PROBE_TIMEOUT:-5}
REQUIRE_ENDPOINT=${AV1_REQUIRE_ENDPOINT:-0}
KEEP_WORK=${KEEP_WORK:-0}

for command in uname awk sed grep tr readlink mktemp timeout; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "error: required command not found: $command" >&2
        exit 2
    fi
done
case $PROBE_TIMEOUT in
    ''|*[!0-9]*|0)
        echo "error: AV1_PROBE_TIMEOUT must be a positive integer" >&2
        exit 2
        ;;
esac
case $REQUIRE_ENDPOINT in
    0|1) ;;
    *)
        echo "error: AV1_REQUIRE_ENDPOINT must be 0 or 1" >&2
        exit 2
        ;;
esac

WORK=$(mktemp -d "$REPO_ROOT/.test-work.av1-platform.XXXXXX") || exit 1
# shellcheck disable=SC2329 # Invoked by the EXIT trap below.
cleanup()
{
    if [ "$KEEP_WORK" = 1 ]; then
        echo "work_files=$WORK"
    else
        rm -rf "$WORK"
    fi
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

normalize_file()
{
    tr '\000' ',' <"$1" | tr '\r\n' '  ' |
        sed 's/[|=]/_/g; s/,$//; s/  */ /g; s/^ //; s/ $//'
}

package_version()
{
    if command -v dpkg-query >/dev/null 2>&1; then
        dpkg-query -W -f='${Version}' "$1" 2>/dev/null || printf unknown
    else
        printf unknown
    fi
}

mpp_details=$WORK/mpp-details
v4l2_details=$WORK/v4l2-details
: >"$mpp_details"
: >"$v4l2_details"

mpp_service=$DEV_ROOT/mpp_service
mpp_present=0
mpp_readable=0
mpp_writable=0
[ -e "$mpp_service" ] && mpp_present=1
[ -r "$mpp_service" ] && mpp_readable=1
[ -w "$mpp_service" ] && mpp_writable=1

mpp_driver_dir=$SYS_ROOT/bus/platform/drivers/mpp_av1dec
mpp_bound_count=0
if [ -d "$mpp_driver_dir" ]; then
    for link in "$mpp_driver_dir"/*; do
        [ -L "$link" ] || continue
        target=$(readlink -f "$link") || continue
        case $target in
            "$SYS_ROOT"/devices/*) ;;
            *) continue ;;
        esac
        compatible=unknown
        status=unknown
        if [ -f "$target/of_node/compatible" ]; then
            compatible=$(normalize_file "$target/of_node/compatible")
        fi
        if [ -f "$target/of_node/status" ]; then
            status=$(normalize_file "$target/of_node/status")
        fi
        printf 'mpp_av1_device_%d=%s|compatible=%s|status=%s\n' \
            "$mpp_bound_count" "${link##*/}" "$compatible" "$status" \
            >>"$mpp_details"
        mpp_bound_count=$((mpp_bound_count + 1))
    done
fi

mpp_api_av1_decode=not-run
mpp_api_result=not-run
mpp_not_ready_messages=0
if [ -x "$MPP_CAPS_PROBE" ]; then
    if timeout --kill-after=1s "$PROBE_TIMEOUT" "$MPP_CAPS_PROBE" \
        >"$WORK/mpp-caps" 2>"$WORK/mpp-caps.log"; then
        mpp_api_av1_decode=$(sed -n \
            's/^mpp_api_av1_decode=//p' "$WORK/mpp-caps" | tail -1)
        mpp_api_result=$(sed -n \
            's/^mpp_api_result=//p' "$WORK/mpp-caps" | tail -1)
        [ -n "$mpp_api_av1_decode" ] || mpp_api_av1_decode=invalid-output
        [ -n "$mpp_api_result" ] || mpp_api_result=invalid-output
    else
        mpp_api_av1_decode=probe-error
        mpp_api_result=probe-error
    fi
    mpp_not_ready_messages=$(grep -c 'driver is not ready' \
        "$WORK/mpp-caps.log" || true)
fi

v4l2_node_count=0
v4l2_av1_count=0
v4l2_ctl_available=0
command -v "$V4L2_CTL" >/dev/null 2>&1 && v4l2_ctl_available=1
for sys_node in "$SYS_ROOT"/class/video4linux/video*; do
    [ -e "$sys_node" ] || continue
    node_name=${sys_node##*/}
    device=$DEV_ROOT/$node_name
    card=unknown
    formats=unavailable
    if [ -f "$sys_node/name" ]; then
        card=$(normalize_file "$sys_node/name")
    fi
    format_log=$WORK/formats-"$node_name"
    if [ "$v4l2_ctl_available" -eq 1 ] && [ -e "$device" ] &&
       timeout --kill-after=1s "$PROBE_TIMEOUT" "$V4L2_CTL" \
           -d "$device" --list-formats-out-ext >"$format_log" 2>&1; then
        formats=$(sed -n "s/.*\\[[0-9][0-9]*\\]: '\\([^']*\\)'.*/\\1/p" \
            "$format_log" |
            awk 'BEGIN { separator = "" }
                 { printf "%s%s", separator, $0; separator = "," }
                 END { if (separator == "") printf "none" }')
    fi
    printf 'v4l2_node_%d=%s|card=%s|output_formats=%s\n' \
        "$v4l2_node_count" "$device" "$card" "$formats" >>"$v4l2_details"
    if [ "$formats" != unavailable ] &&
       printf '%s\n' "$formats" | grep -Eq '(^|,)AV1F(,|$)'; then
        printf 'v4l2_av1_device_%d=%s|card=%s\n' \
            "$v4l2_av1_count" "$device" "$card" >>"$v4l2_details"
        v4l2_av1_count=$((v4l2_av1_count + 1))
    fi
    v4l2_node_count=$((v4l2_node_count + 1))
done

selected_track=none
result=endpoint-missing
if [ "$v4l2_av1_count" -gt 0 ]; then
    selected_track=v4l2
    result=endpoint-present-unqualified
elif [ "$mpp_bound_count" -gt 0 ]; then
    selected_track=mpp
    result=endpoint-present-unqualified
fi

printf '%s\n' \
    'probe_format=rockchip-vaapi-av1-platform-v1' \
    "kernel_release=$(uname -r)" \
    "machine=$(uname -m)" \
    "mpp_package_version=$(package_version librockchip-mpp1)" \
    "libva_package_version=$(package_version libva2)" \
    "mpp_service=$mpp_service" \
    "mpp_service_present=$mpp_present" \
    "mpp_service_readable=$mpp_readable" \
    "mpp_service_writable=$mpp_writable" \
    "mpp_av1_bound_count=$mpp_bound_count"
cat "$mpp_details"
printf '%s\n' \
    "mpp_api_av1_decode=$mpp_api_av1_decode" \
    "mpp_api_result=$mpp_api_result" \
    "mpp_not_ready_messages=$mpp_not_ready_messages" \
    "v4l2_ctl_available=$v4l2_ctl_available" \
    "v4l2_node_count=$v4l2_node_count" \
    "v4l2_av1_count=$v4l2_av1_count"
cat "$v4l2_details"
printf '%s\n' \
    "selected_track=$selected_track" \
    'hardware_decode_attempted=0' \
    'phase0_qualified=0' \
    "result=$result"

if [ "$REQUIRE_ENDPOINT" -eq 1 ] && [ "$result" = endpoint-missing ]; then
    exit 1
fi
