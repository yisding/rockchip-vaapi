#!/bin/sh
# Validate the Firefox 152.0.6 Rockchip RDD sandbox source patch.

set -eu

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH='' cd -- "$SCRIPT_DIR/.." && pwd)
PATCH=$REPO_ROOT/contrib/firefox/patches/firefox-152.0.6-rdd-rockchip-vaapi.patch
SOURCE_ROOT=${1:-${FIREFOX_SOURCE_ROOT:-}}
FILTER=security/sandbox/linux/SandboxFilter.cpp
BROKER=security/sandbox/linux/broker/SandboxBrokerPolicyFactory.cpp
FILTER_SHA256=7a9c7b4e56b5ed0401998f42242bd576bff5461e85df271d42f73844a2bf9f47
BROKER_SHA256=0bc000706b11d7dcf54c71f67bd1cb32d2214e939fbb67634e0bd0036b805af0

for command in grep patch sha256sum mktemp cp mkdir; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "error: required command not found: $command" >&2
        exit 2
    fi
done

require_patch_text()
{
    if ! grep -Fq -- "$1" "$PATCH"; then
        echo "error: Firefox patch is missing: $1" >&2
        exit 1
    fi
}

require_patch_text '--- a/security/sandbox/linux/SandboxFilter.cpp'
require_patch_text '--- a/security/sandbox/linux/broker/SandboxBrokerPolicyFactory.cpp'
require_patch_text 'kRockchipMppConfigV1 = 0x40047601'
require_patch_text 'kRockchipRgaGetDriverVersion'
require_patch_text '0x801c7201'
require_patch_text 'kRockchipRgaGetHwVersion = 0x80907202'
require_patch_text 'kRockchipRgaBlitSync = 0x5017'
require_patch_text 'policy->AddPath(rdwr, "/dev/mpp_service")'
require_patch_text 'policy->AddPath(rdwr, "/dev/rga")'
require_patch_text 'policy->AddTree(rdwr, "/dev/dma_heap")'

if grep -Fq 'MOZ_DISABLE_RDD_SANDBOX' "$PATCH"; then
    echo "error: Firefox patch must not disable the RDD sandbox" >&2
    exit 1
fi

if [ -z "$SOURCE_ROOT" ]; then
    echo "ok    Firefox 152.0.6 RDD patch contract"
    echo "note  pass a Firefox source root to verify hashes and patch application"
    exit 0
fi

check_source()
{
    relative=$1
    expected=$2
    source=$SOURCE_ROOT/$relative

    if [ ! -f "$source" ]; then
        echo "error: missing Firefox source file: $source" >&2
        exit 2
    fi
    actual=$(sha256sum "$source" | awk '{print $1}')
    if [ "$actual" != "$expected" ]; then
        echo "error: Firefox source hash mismatch: $relative" >&2
        echo "error: expected $expected" >&2
        echo "error: actual   $actual" >&2
        exit 1
    fi
}

check_source "$FILTER" "$FILTER_SHA256"
check_source "$BROKER" "$BROKER_SHA256"

WORK=$(mktemp -d "$REPO_ROOT/.test-work.firefox-rdd.XXXXXX") || exit 1
cleanup()
{
    rm -rf "$WORK"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

mkdir -p "$WORK/$(dirname "$FILTER")" "$WORK/$(dirname "$BROKER")"
cp "$SOURCE_ROOT/$FILTER" "$WORK/$FILTER"
cp "$SOURCE_ROOT/$BROKER" "$WORK/$BROKER"

patch --batch --forward --dry-run -d "$WORK" -p1 <"$PATCH"
patch --batch --forward -d "$WORK" -p1 <"$PATCH"

grep -Fq 'request == kRockchipMppConfigV1' "$WORK/$FILTER"
grep -Fq 'request == kRockchipRgaBlitSync' "$WORK/$FILTER"
grep -Fq 'policy->AddPath(rdwr, "/dev/mpp_service")' "$WORK/$BROKER"
grep -Fq 'policy->AddTree(rdwr, "/dev/dma_heap")' "$WORK/$BROKER"

echo "ok    Firefox 152.0.6 source hashes and RDD patch application"
