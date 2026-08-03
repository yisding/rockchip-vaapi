#!/bin/sh
# Payload provenance gate: prove that the driver a package ships is the driver
# this source tree produces, independently of where it was built.
#
# The interesting failure this catches is not a corrupted download. It is a
# build that silently records its own directory in the binary: the Debian build
# links with -flto, LTO re-emits debug info for its own translation units at
# link time, and those units take DW_AT_comp_dir from the link command. If the
# link does not carry the -f*-prefix-map flags, two builds of identical source
# differ in their GNU build-id and therefore in their payload hash. A
# same-directory rebuild would still match, so the provenance claim would look
# sound while resting on an accident of where it was rebuilt.
#
# So this gate rebuilds the packaged driver twice, from a clean export of the
# commit, at two deliberately different paths, and requires one hash. When a
# reference driver exists (by default the installed one) it must carry that
# same hash.

set -eu

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH='' cd -- "$SCRIPT_DIR/.." && pwd)
GIT=${GIT:-git}
DPKG_DEB=${DPKG_DEB:-dpkg-deb}
DPKG_BUILDPACKAGE=${DPKG_BUILDPACKAGE:-dpkg-buildpackage}
READELF=${READELF:-readelf}
MULTIARCH=${MULTIARCH:-$(${CC:-gcc} -print-multiarch 2>/dev/null || echo aarch64-linux-gnu)}
DRIVER_PATH=usr/lib/$MULTIARCH/dri/rockchip_drv_video.so
# Empty disables the comparison; "none" is accepted for the same intent.
REFERENCE=${REFERENCE-/$DRIVER_PATH}
COMMIT=${COMMIT:-HEAD}
KEEP_WORK=${KEEP_WORK:-0}

for command in "$GIT" "$DPKG_DEB" "$DPKG_BUILDPACKAGE" "$READELF" \
               awk find grep mktemp sed sha256sum tar; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "error: required command not found: $command" >&2
        exit 2
    fi
done

fail()
{
    echo "FAIL  package provenance gate: $*" >&2
    exit 1
}

# Prints the first existing path matching the arguments, if any. dpkg-buildpackage
# names its own output, so the caller globs for it rather than knowing the name.
first_existing()
{
    for candidate in "$@"; do
        if [ -e "$candidate" ]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    return 1
}

WORK=$(mktemp -d "$REPO_ROOT/.test-work.package-provenance.XXXXXX") || exit 1
# shellcheck disable=SC2329 # Invoked by the EXIT trap below.
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

if ! "$GIT" -C "$REPO_ROOT" rev-parse --git-dir >/dev/null 2>&1; then
    fail "$REPO_ROOT is not a git checkout, so the payload has no commit"
fi
revision=$("$GIT" -C "$REPO_ROOT" rev-parse "$COMMIT") ||
    fail "cannot resolve $COMMIT"

# The gate speaks for a commit, so a tree that has drifted from it must say so
# rather than quietly certifying uncommitted work.
dirty=$("$GIT" -C "$REPO_ROOT" status --porcelain -- Makefile debian src tests)
if [ -n "$dirty" ]; then
    if [ "${ALLOW_DIRTY:-0}" = 1 ]; then
        echo "warn  packaged paths differ from $COMMIT; ALLOW_DIRTY=1 set"
    else
        printf '%s\n' "$dirty" >&2
        fail "packaged paths differ from $COMMIT; commit them or set ALLOW_DIRTY=1"
    fi
fi

# Two build roots whose absolute paths differ in both content and length, so a
# leak of either one is visible as a hash difference.
short_root=$WORK/a
long_root=$WORK/second-build-root-with-a-longer-name
build_one()
{
    root=$1
    label=$2
    mkdir -p "$root/source"
    "$GIT" -C "$REPO_ROOT" archive --format=tar "$revision" |
        tar -x -C "$root/source" ||
        fail "cannot export $COMMIT into $root"
    if [ "${ALLOW_DIRTY:-0}" = 1 ]; then
        # Certify what is on disk, not what is committed. Only tracked and
        # newly added files: the ignored ones are build output and the
        # hundred-megabyte vector cache, neither of which is packaged.
        "$GIT" -C "$REPO_ROOT" ls-files -z --cached --others \
            --exclude-standard -- Makefile debian src tests |
            tar -C "$REPO_ROOT" --null -T - -cf - |
            tar -x -C "$root/source"
    fi
    ( cd "$root/source" && "$DPKG_BUILDPACKAGE" -us -uc -b ) \
        >"$WORK/$label.log" 2>&1 ||
        { sed -n '1,80p' "$WORK/$label.log" >&2
          fail "package build failed at $root"; }
    deb=$(first_existing "$root"/rockchip-vaapi_*_*.deb) ||
        fail "no driver package was produced at $root"
    "$DPKG_DEB" -x "$deb" "$root/payload" ||
        fail "cannot extract $deb"
    [ -f "$root/payload/$DRIVER_PATH" ] ||
        fail "package has no $DRIVER_PATH"
}

build_one "$short_root" build-short
build_one "$long_root" build-long

hash_of()
{
    sha256sum "$1" | awk '{print $1}'
}

short_hash=$(hash_of "$short_root/payload/$DRIVER_PATH")
long_hash=$(hash_of "$long_root/payload/$DRIVER_PATH")

if [ "$short_hash" != "$long_hash" ]; then
    echo "  $short_root -> $short_hash" >&2
    echo "  $long_root  -> $long_hash" >&2
    fail "the payload depends on the build directory, so its hash cannot" \
         "attest to source"
fi

# A hash that is stable across two paths would still be worth little if the
# build merely stopped recording paths it kept using. Check the binary and its
# detached debug info directly.
for root in "$short_root" "$long_root"; do
    if grep -aqF "$root" "$root/payload/$DRIVER_PATH"; then
        fail "the driver payload built at $root embeds that build directory"
    fi
    dbgsym=$(first_existing "$root"/rockchip-vaapi-dbgsym_*.ddeb \
                            "$root"/rockchip-vaapi-dbgsym_*.deb) || continue
    "$DPKG_DEB" -x "$dbgsym" "$root/dbgsym" || fail "cannot extract $dbgsym"
    debug=$(find "$root/dbgsym" -name '*.debug' | head -1)
    [ -n "$debug" ] || continue
    # Debian compresses debug sections, so search the decompressed copy.
    if command -v objcopy >/dev/null 2>&1 &&
       objcopy --decompress-debug-sections "$debug" "$root/debug.raw" \
           2>/dev/null; then
        debug=$root/debug.raw
    fi
    if grep -aqF "$root" "$debug"; then
        fail "the debug info built at $root embeds that build directory"
    fi
done

"$READELF" -h "$short_root/payload/$DRIVER_PATH" |
    grep -Fq 'Machine:                           AArch64' ||
    fail "rebuilt driver is not AArch64 ELF"

reference_note='no reference driver compared'
case "${REFERENCE:-}" in
    ''|none)
        ;;
    *)
        if [ ! -f "$REFERENCE" ]; then
            fail "reference driver not found: $REFERENCE"
        fi
        reference_hash=$(hash_of "$REFERENCE")
        if [ "$reference_hash" != "$short_hash" ]; then
            echo "  rebuilt   $short_hash" >&2
            echo "  reference $reference_hash ($REFERENCE)" >&2
            fail "$REFERENCE was not built from $COMMIT"
        fi
        reference_note="matches $REFERENCE"
        ;;
esac

built_deb=$(first_existing "$short_root"/rockchip-vaapi_*_*.deb) ||
    fail "the rebuilt driver package disappeared"
version=$("$DPKG_DEB" -f "$built_deb" Version)
short_revision=$(printf '%.12s' "$revision")
echo "ok    package provenance $version from $short_revision" \
     "sha256 $short_hash ($reference_note)"
