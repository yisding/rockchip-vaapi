#!/bin/sh
# Validate the Rockchip RDD sandbox and Panfrost P010 source patches against
# their exact Firefox release. Each supported milestone pins all three
# preimage hashes; a source tree that does not match is rejected rather than
# force-patched.

set -eu

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH='' cd -- "$SCRIPT_DIR/.." && pwd)
FIREFOX_VERSION=${FIREFOX_VERSION:-153.0}
PATCH=$REPO_ROOT/contrib/firefox/patches/firefox-$FIREFOX_VERSION-rdd-rockchip-vaapi.patch
P010_PATCH=$REPO_ROOT/contrib/firefox/patches/firefox-$FIREFOX_VERSION-panfrost-p010-chroma-retry.patch
SOURCE_ROOT=${1:-${FIREFOX_SOURCE_ROOT:-}}
FILTER=security/sandbox/linux/SandboxFilter.cpp
BROKER=security/sandbox/linux/broker/SandboxBrokerPolicyFactory.cpp
DMABUF=widget/gtk/DMABufSurface.cpp
case $FIREFOX_VERSION in
    153.0)
        FILTER_SHA256=b1dae2499ba9589cc41454cf7f73c332c82ed9d6c13710c0448fdc9c7507e1e9
        BROKER_SHA256=3eefffdd817ddebea6d029e5403a1f1d9536c7b49ef86e5c553e1ab77e6bddcb
        DMABUF_SHA256=2c44aa0a1597ec57cd597055d78b5aaecb645b3af33169fb6439fb37b04434df
        ;;
    152.0.6)
        FILTER_SHA256=7a9c7b4e56b5ed0401998f42242bd576bff5461e85df271d42f73844a2bf9f47
        BROKER_SHA256=0bc000706b11d7dcf54c71f67bd1cb32d2214e939fbb67634e0bd0036b805af0
        DMABUF_SHA256=e4ea08b2da7c1e21df520620f873eb1f3db180d71dee26872e28eb7456ad8777
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
if [ ! -f "$P010_PATCH" ]; then
    echo "error: missing patch $P010_PATCH" >&2
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

if ! grep -Fq -- '--- a/widget/gtk/DMABufSurface.cpp' "$P010_PATCH" ||
   ! grep -Fq 'retrying swapped chroma format' "$P010_PATCH"; then
    echo "error: Firefox P010 patch is missing its EGL retry contract" >&2
    exit 1
fi

if grep -Fq 'MOZ_DISABLE_RDD_SANDBOX' "$PATCH" "$P010_PATCH"; then
    echo "error: Firefox patch must not disable the RDD sandbox" >&2
    exit 1
fi

if [ -z "$SOURCE_ROOT" ]; then
    echo "ok    Firefox $FIREFOX_VERSION RDD/P010 patch contract"
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
check_source "$DMABUF" "$DMABUF_SHA256"

WORK=$(mktemp -d "$REPO_ROOT/.test-work.firefox-rdd.XXXXXX") || exit 1
cleanup()
{
    rm -rf "$WORK"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

mkdir -p "$WORK/$(dirname "$FILTER")" "$WORK/$(dirname "$BROKER")" \
    "$WORK/$(dirname "$DMABUF")"
cp "$SOURCE_ROOT/$FILTER" "$WORK/$FILTER"
cp "$SOURCE_ROOT/$BROKER" "$WORK/$BROKER"
cp "$SOURCE_ROOT/$DMABUF" "$WORK/$DMABUF"

patch --batch --forward --dry-run -d "$WORK" -p1 <"$PATCH"
patch --batch --forward -d "$WORK" -p1 <"$PATCH"
patch --batch --forward --dry-run -d "$WORK" -p1 <"$P010_PATCH"
patch --batch --forward -d "$WORK" -p1 <"$P010_PATCH"

grep -Fq 'request == kRockchipMppConfigV1' "$WORK/$FILTER"
grep -Fq 'request == kRockchipRgaBlitSync' "$WORK/$FILTER"
grep -Fq 'policy->AddPath(rdwr, "/dev/mpp_service")' "$WORK/$BROKER"
grep -Fq 'policy->AddTree(rdwr, "/dev/dma_heap")' "$WORK/$BROKER"
grep -Fq 'retrying swapped chroma format' "$WORK/$DMABUF"

echo "ok    Firefox $FIREFOX_VERSION source hashes and RDD/P010 patch application"
