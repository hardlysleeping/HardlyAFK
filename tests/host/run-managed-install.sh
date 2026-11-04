#!/bin/sh
# Linux host proof, using the same converter linked by the iOS app.
set -eu
: "${TMPDIR:?Set TMPDIR to an owned scratch directory}"
archive=${1:?Usage: sh tests/host/run-managed-install.sh alpine.tar.gz}
work=$(mktemp -d "$TMPDIR/afk-managed-build.XXXXXX")
trap 'rm -rf "$work"' EXIT
expected=9bf70a7f18ea44094cbb5f70c58f9af129c8214745743db0e68e5502cc2ce773
actual=$(sha256sum "$archive" | cut -d ' ' -f 1)
[ "$actual" = "$expected" ] || { printf '%s\n' 'Alpine fixture checksum mismatch'; exit 1; }
${CC:-cc} -std=gnu11 -DGUEST_ARM64 -I. -Wall -Wextra -Werror -g \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -ffunction-sections -fdata-sections \
    tools/afk_managed_install.c tools/fakefs.c util/fchdir.c \
    core/environment/afk_env_catalog.c core/environment/afk_env_store.c \
    core/environment/afk_ios_boot.c core/distro/afk_distro_catalog.c \
    core/distro/afk_sha256.c tests/host/afk_managed_install_test.c \
    -Wl,--gc-sections -lsqlite3 -larchive -lpthread -o "$work/test"
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$work/test" "$archive"
