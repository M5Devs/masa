#!/bin/sh
#
# dist.sh - build Masa locally for every toolchain that is present and
# package the result under dist/.
#
# This is the "local independence" path: with nothing more than a host C
# compiler you get a native archive, and when the Windows (MinGW-w64) or
# Android (NDK) cross compilers are on PATH you get those too - no GitHub
# Actions or other hosted CI required.  Each target is written to
#   dist/masa-<version>-<os>-<arch>/
# alongside LICENSE and README.md, packaged as a .tar.gz and a .zip, and a
# dist/SHA256SUMS file is generated at the end.
#
# Copyright (C) 2026 Masa contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Usage: sh scripts/dist.sh   (or `make dist`)

set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

VERSION=$(sed -n 's/^#define MASA_VERSION "\(.*\)"/\1/p' include/masa.h)
if [ -z "$VERSION" ]; then
    echo "dist: cannot read MASA_VERSION from include/masa.h" >&2
    exit 1
fi

HOST_ARCH=$(uname -m)
DIST="$ROOT/dist"
CFLAGS="-std=c11 -Wall -Wextra -Werror -Iinclude"

# Core translation units, without the platform backend (added per target).
CORE_SRC="src/main.c src/cc_wrapper.c src/swift_wrapper.c src/sys_shims.c src/tool_shims.c src/sdk.c src/xcodeproj.c src/brew.c src/exec.c src/doctor.c"

ARCHIVES=""

# package <dir-name> : archive dist/<dir-name> as .tar.gz and .zip
package() {
    name=$1
    tar -czf "$DIST/$name.tar.gz" -C "$DIST" "$name"
    ARCHIVES="$ARCHIVES $name.tar.gz"

    if command -v zip >/dev/null 2>&1; then
        (cd "$DIST" && zip -qr "$name.zip" "$name")
    else
        # python3 is a fair assumption and its zipfile module is standard.
        python3 - "$DIST" "$name" <<'PY'
import os, sys, zipfile
dist, name = sys.argv[1], sys.argv[2]
base = os.path.join(dist, name)
with zipfile.ZipFile(os.path.join(dist, name + ".zip"), "w",
                     zipfile.ZIP_DEFLATED) as z:
    for root, _dirs, files in os.walk(base):
        for f in files:
            full = os.path.join(root, f)
            z.write(full, os.path.relpath(full, dist))
PY
    fi
    ARCHIVES="$ARCHIVES $name.zip"
    echo "dist: packaged $name.tar.gz and $name.zip"
}

# stage <dir-name> <binary-name> <binary-path>
stage() {
    dir="$DIST/$1"
    mkdir -p "$dir"
    cp "$3" "$dir/$2"
    cp LICENSE README.md "$dir/"
}

echo "dist: Masa $VERSION ($HOST_ARCH host)"

rm -rf "$DIST"
mkdir -p "$DIST"

# --- Native Linux (always) -------------------------------------------
NATIVE="masa-$VERSION-linux-$HOST_ARCH"
echo "dist: building native $NATIVE"
# shellcheck disable=SC2086
cc $CFLAGS $CORE_SRC src/platform_posix.c -o "$DIST/masa.native"
stage "$NATIVE" masa "$DIST/masa.native"
rm -f "$DIST/masa.native"
package "$NATIVE"

# --- Windows x86_64 (when MinGW-w64 is available) --------------------
if command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
    WIN="masa-$VERSION-windows-x86_64"
    echo "dist: building $WIN"
    # shellcheck disable=SC2086
    x86_64-w64-mingw32-gcc $CFLAGS $CORE_SRC src/platform_win.c \
        -o "$DIST/masa.win"
    stage "$WIN" masa.exe "$DIST/masa.win"
    rm -f "$DIST/masa.win"
    package "$WIN"
else
    echo "dist: skipping Windows (x86_64-w64-mingw32-gcc not on PATH)"
fi

# --- Android aarch64 (when the NDK compiler is configured) -----------
if [ -n "${ANDROID_CC:-}" ] && command -v "$ANDROID_CC" >/dev/null 2>&1; then
    ANDROID="masa-$VERSION-android-aarch64"
    echo "dist: building $ANDROID"
    # shellcheck disable=SC2086
    "$ANDROID_CC" $CFLAGS $CORE_SRC src/platform_posix.c \
        -o "$DIST/masa.android"
    stage "$ANDROID" masa "$DIST/masa.android"
    rm -f "$DIST/masa.android"
    package "$ANDROID"
else
    echo "dist: skipping Android (set ANDROID_CC to an NDK clang to include it)"
fi

# --- Checksums -------------------------------------------------------
# shellcheck disable=SC2086
(cd "$DIST" && sha256sum $ARCHIVES > SHA256SUMS)
echo "dist: wrote $DIST/SHA256SUMS"
cat "$DIST/SHA256SUMS"
