#!/bin/sh
# Rootless clean-image install/upgrade/purge gate for the Debian package split.

set -eu

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH='' cd -- "$SCRIPT_DIR/.." && pwd)
BWRAP=${BWRAP:-bwrap}
DPKG=${DPKG:-dpkg}
DPKG_DEB=${DPKG_DEB:-dpkg-deb}
DPKG_QUERY=${DPKG_QUERY:-dpkg-query}
FAKEROOT=${FAKEROOT:-fakeroot-sysv}
READELF=${READELF:-readelf}
KEEP_WORK=${KEEP_WORK:-0}

if [ "$#" -ne 2 ]; then
    echo "usage: $0 DRIVER_DEB CONFIG_DEB" >&2
    exit 2
fi
DRIVER_DEB=$1
CONFIG_DEB=$2

for command in "$BWRAP" "$DPKG" "$DPKG_DEB" "$DPKG_QUERY" "$FAKEROOT" \
               "$READELF" awk cmp cp grep mktemp sed stat; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "error: required command not found: $command" >&2
        exit 2
    fi
done
for package in "$DRIVER_DEB" "$CONFIG_DEB"; do
    if [ ! -f "$package" ]; then
        echo "error: package not found: $package" >&2
        exit 2
    fi
done

WORK=$(mktemp -d "$REPO_ROOT/.test-work.package-install.XXXXXX") || exit 1
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

fail()
{
    echo "FAIL  package install gate: $*" >&2
    exit 1
}

field()
{
    "$DPKG_DEB" -f "$1" "$2"
}

driver_name=$(field "$DRIVER_DEB" Package)
config_name=$(field "$CONFIG_DEB" Package)
driver_version=$(field "$DRIVER_DEB" Version)
config_version=$(field "$CONFIG_DEB" Version)
driver_arch=$(field "$DRIVER_DEB" Architecture)
config_arch=$(field "$CONFIG_DEB" Architecture)
driver_depends=$(field "$DRIVER_DEB" Depends)
config_depends=$(field "$CONFIG_DEB" Depends)

[ "$driver_name" = rockchip-vaapi ] ||
    fail "driver Package field is $driver_name"
[ "$config_name" = rockchip-vaapi-config ] ||
    fail "config Package field is $config_name"
[ "$driver_version" = "$config_version" ] ||
    fail "package versions differ: $driver_version / $config_version"
[ "$driver_arch" = arm64 ] ||
    fail "driver Architecture is $driver_arch"
[ "$config_arch" = all ] ||
    fail "config Architecture is $config_arch"
printf '%s\n' "$driver_depends" |
    grep -Eq '(^|, )libva2( |,|$)' ||
    fail "driver does not depend on libva2"
printf '%s\n' "$driver_depends" |
    grep -Eq '(^|, )librockchip-mpp1( |,|$)' ||
    fail "driver does not depend on librockchip-mpp1"
printf '%s\n' "$driver_depends" |
    grep -Eq '(^|, )librga2( |,|$)' ||
    fail "driver does not depend on librga2"
printf '%s\n' "$config_depends" |
    grep -Fq "rockchip-vaapi (= $driver_version)" ||
    fail "config does not require the exact driver version"

mkdir -p "$WORK/packages" "$WORK/driver-payload" "$WORK/config-payload"
cp "$DRIVER_DEB" "$WORK/packages/driver.deb"
cp "$CONFIG_DEB" "$WORK/packages/config.deb"
"$DPKG_DEB" -x "$DRIVER_DEB" "$WORK/driver-payload"
"$DPKG_DEB" -x "$CONFIG_DEB" "$WORK/config-payload"

driver_path=usr/lib/aarch64-linux-gnu/dri/rockchip_drv_video.so
profile_path=etc/profile.d/rockchip-vaapi-config.sh
environment_path=etc/environment.d/61-rockchip-vaapi.conf
[ -f "$WORK/driver-payload/$driver_path" ] ||
    fail "driver payload path is missing"
[ "$(stat -c %a "$WORK/driver-payload/$driver_path")" = 644 ] ||
    fail "driver payload mode is not 0644"
[ "$(stat -c %a "$WORK/config-payload/$profile_path")" = 644 ] ||
    fail "profile.d payload mode is not 0644"
[ "$(stat -c %a "$WORK/config-payload/$environment_path")" = 644 ] ||
    fail "environment.d payload mode is not 0644"
cmp "$REPO_ROOT/debian/rockchip-vaapi-config.sh" \
    "$WORK/config-payload/$profile_path" >/dev/null ||
    fail "profile.d payload differs from its source"
cmp "$REPO_ROOT/debian/61-rockchip-vaapi.conf" \
    "$WORK/config-payload/$environment_path" >/dev/null ||
    fail "environment.d payload differs from its source"
if grep -ERq 'MOZ_DISABLE_RDD_SANDBOX|MOZ_ENABLE_WAYLAND|MOZ_X11_EGL' \
              "$WORK/config-payload/etc"; then
    fail "config payload contains browser or sandbox overrides"
fi
"$READELF" -h "$WORK/driver-payload/$driver_path" |
    grep -Fq 'Machine:                           AArch64' ||
    fail "driver payload is not AArch64 ELF"
for library in librockchip_mpp.so.1 librga.so.2 libc.so.6; do
    "$READELF" -d "$WORK/driver-payload/$driver_path" |
        grep -Fq "Shared library: [$library]" ||
        fail "driver ELF does not require $library"
done
"$READELF" -d "$WORK/driver-payload/$driver_path" |
    grep -Fq '(FLAGS)              BIND_NOW' ||
    fail "driver ELF lacks immediate binding hardening"

ROOT=$WORK/root
mkdir -p "$ROOT/.packages" \
    "$ROOT/etc/profile.d" "$ROOT/etc/environment.d" \
    "$ROOT/usr/lib/aarch64-linux-gnu/dri" "$ROOT/usr/share/doc" \
    "$ROOT/var/lib/dpkg/updates" "$ROOT/var/lib/dpkg/info" \
    "$ROOT/var/lib/dpkg/triggers" "$ROOT/var/lib/dpkg/parts" \
    "$ROOT/var/log"
: >"$ROOT/var/lib/dpkg/status"
: >"$ROOT/var/lib/dpkg/available"
: >"$ROOT/etc/.clean-image-sentinel"
: >"$ROOT/etc/profile.d/.clean-image-sentinel"
: >"$ROOT/etc/environment.d/.clean-image-sentinel"
cp "$WORK/packages/driver.deb" "$ROOT/.packages/driver.deb"
cp "$WORK/packages/config.deb" "$ROOT/.packages/config.deb"

seed_legacy_files()
{
    printf '%s\n' 'export MOZ_DISABLE_RDD_SANDBOX=1' \
        >"$ROOT/etc/profile.d/rockchip-vaapi.sh"
    printf '%s\n' 'MOZ_DISABLE_RDD_SANDBOX=1' \
        >"$ROOT/etc/environment.d/60-rockchip-vaapi.conf"
}

run_dpkg()
{
    "$BWRAP" --ro-bind / / --proc /proc --dev /dev --unshare-all \
        --die-with-parent \
        --bind "$ROOT" /mnt \
        --bind "$ROOT/etc" /etc \
        /usr/bin/env PATH=/usr/sbin:/usr/bin:/sbin:/bin \
            FAKEROOTDONTTRYCHOWN=1 "$FAKEROOT" -- "$DPKG" \
            --root=/mnt --force-script-chrootless \
            --force-not-root --force-depends --force-bad-path "$@"
}

run_checked()
{
    label=$1
    shift
    if ! run_dpkg "$@" >"$WORK/$label.log" 2>&1; then
        sed -n '1,160p' "$WORK/$label.log" >&2
        fail "dpkg step failed: $label"
    fi
}

package_status()
{
    # shellcheck disable=SC2016 # dpkg-query expands this format expression.
    "$DPKG_QUERY" --admindir="$ROOT/var/lib/dpkg" \
        -W -f='${db:Status-Status}' "$1" 2>/dev/null || true
}

assert_installed()
{
    [ "$(package_status "$1")" = installed ] ||
        fail "$1 is not installed in the isolated database"
}

assert_not_installed()
{
    [ "$(package_status "$1")" != installed ] ||
        fail "$1 remains installed in the isolated database"
}

assert_no_legacy_files()
{
    [ ! -e "$ROOT/etc/profile.d/rockchip-vaapi.sh" ] &&
    [ ! -e "$ROOT/etc/environment.d/60-rockchip-vaapi.conf" ] ||
        fail "unsafe ysp2 environment files survived postinst"
}

seed_legacy_files
run_checked install -i /mnt/.packages/driver.deb \
    /mnt/.packages/config.deb
assert_installed rockchip-vaapi
assert_installed rockchip-vaapi-config
assert_no_legacy_files
[ -f "$ROOT/$driver_path" ] ||
    fail "driver is absent after isolated install"
cmp "$REPO_ROOT/debian/rockchip-vaapi-config.sh" \
    "$ROOT/$profile_path" >/dev/null ||
    fail "installed profile.d file changed"
cmp "$REPO_ROOT/debian/61-rockchip-vaapi.conf" \
    "$ROOT/$environment_path" >/dev/null ||
    fail "installed environment.d file changed"

seed_legacy_files
run_checked upgrade-driver -i /mnt/.packages/driver.deb
assert_no_legacy_files
assert_installed rockchip-vaapi
assert_installed rockchip-vaapi-config

run_checked purge-config --purge rockchip-vaapi-config
assert_not_installed rockchip-vaapi-config
assert_installed rockchip-vaapi
[ ! -e "$ROOT/$profile_path" ] && [ ! -e "$ROOT/$environment_path" ] ||
    fail "config files survived config-package purge"

run_checked reinstall-config -i /mnt/.packages/config.deb
assert_installed rockchip-vaapi-config
run_checked purge-all --purge rockchip-vaapi-config rockchip-vaapi
assert_not_installed rockchip-vaapi-config
assert_not_installed rockchip-vaapi
[ ! -e "$ROOT/$driver_path" ] &&
[ ! -e "$ROOT/$profile_path" ] &&
[ ! -e "$ROOT/$environment_path" ] ||
    fail "package-owned runtime files survived final purge"
assert_no_legacy_files

echo "ok    Debian clean install/upgrade/purge $driver_version (isolated root)"
