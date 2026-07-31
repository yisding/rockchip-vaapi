#!/bin/sh
# Validate the Rockchip RDD sandbox patch against its exact Firefox release.
# Each supported milestone pins both preimage hashes; a source tree that does
# not match is rejected rather than force-patched.

set -eu

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH='' cd -- "$SCRIPT_DIR/.." && pwd)
FIREFOX_VERSION=${FIREFOX_VERSION:-153.0}
PATCH=$REPO_ROOT/contrib/firefox/patches/firefox-$FIREFOX_VERSION-rdd-rockchip-vaapi.patch
SOURCE_ROOT=${1:-${FIREFOX_SOURCE_ROOT:-}}
FILTER=security/sandbox/linux/SandboxFilter.cpp
BROKER=security/sandbox/linux/broker/SandboxBrokerPolicyFactory.cpp
case $FIREFOX_VERSION in
    153.0)
        FILTER_SHA256=b1dae2499ba9589cc41454cf7f73c332c82ed9d6c13710c0448fdc9c7507e1e9
        BROKER_SHA256=3eefffdd817ddebea6d029e5403a1f1d9536c7b49ef86e5c553e1ab77e6bddcb
        ;;
    152.0.6)
        FILTER_SHA256=7a9c7b4e56b5ed0401998f42242bd576bff5461e85df271d42f73844a2bf9f47
        BROKER_SHA256=0bc000706b11d7dcf54c71f67bd1cb32d2214e939fbb67634e0bd0036b805af0
        ;;
    *)
        echo "error: no pinned patch for Firefox $FIREFOX_VERSION" >&2
        echo "error: rebase and remeasure before adding one" >&2
        exit 2
        ;;
esac
if [ ! -f "$PATCH" ]; then
    echo "error: missing patch $PATCH" >&2
    exit 2
fi
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
    echo "ok    Firefox $FIREFOX_VERSION RDD patch contract"
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

echo "ok    Firefox $FIREFOX_VERSION source hashes and RDD patch application"
