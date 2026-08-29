#!/usr/bin/env bash
# Cross-build the termux-va daemon with the Android NDK.
#
# The NDK version is PINNED to 29.0.14206865 - the same version used by the
# anland-termux project (app/build.gradle ndkVersion) so that all daemons in
# this ecosystem are built with one toolchain.  Override ANDROID_NDK_HOME to
# point at a different installation, but keep the version.
#
# Output: build/termux-va (aarch64 PIE, interpreter /system/bin/linker64).

set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUT_DIR="$REPO_DIR/build"

REQUIRED_NDK_VERSION="29.0.14206865"
API=29                 # Android 10+; AMediaFormat_getRect needs >= 28
TARGET_TRIPLE="aarch64-linux-android${API}"

if [[ -z "${ANDROID_NDK_HOME:-}" ]]; then
    echo "ANDROID_NDK_HOME is not set; expected NDK ${REQUIRED_NDK_VERSION}" >&2
    echo "e.g. export ANDROID_NDK_HOME=\$HOME/Android/Sdk/ndk/${REQUIRED_NDK_VERSION}" >&2
    exit 1
fi

if [[ -f "${ANDROID_NDK_HOME}/source.properties" ]]; then
    INSTALLED_VERSION="$(sed -n 's/^Pkg\.Revision *= *//p' "${ANDROID_NDK_HOME}/source.properties")"
    if [[ "$INSTALLED_VERSION" != "${REQUIRED_NDK_VERSION}"* ]]; then
        echo "WARNING: NDK ${INSTALLED_VERSION} found, project pins ${REQUIRED_NDK_VERSION}" >&2
        echo "         (same version as anland-termux app/build.gradle)" >&2
    fi
fi

CC="${ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/linux-x86_64/bin/${TARGET_TRIPLE}-clang"
if [[ ! -x "$CC" ]]; then
    echo "NDK clang not found: $CC" >&2
    exit 1
fi

mkdir -p "$OUT_DIR"

# -lmediandk provides both AMediaCodec_* and AMediaFormat_* symbols.
# bionic's pthread lives in libc, so no -lpthread is needed.
"$CC" \
    -O2 -Wall -Wextra -std=c11 \
    -I "$REPO_DIR/common" \
    -o "$OUT_DIR/termux-va" \
    "$REPO_DIR/daemon/termux-va.c" \
    -lmediandk -llog -landroid

echo "Built $OUT_DIR/termux-va"
