#!/bin/bash
# Cross-compile an SDL2 program for the R36S (ArkOS, aarch64, glibc 2.30).
# Usage: ./build.sh <project-dir>    -> builds <project-dir>/*.c into <project-dir>/<name>
# Needs: clang + lld on the host; toolchain/sysroot = Debian buster arm64 headers/libs.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SYSROOT=$HERE/toolchain/sysroot
DIR=$(cd "${1:?usage: build.sh <project-dir>}" && pwd)
NAME=$(basename "$DIR")

clang --target=aarch64-linux-gnu --sysroot="$SYSROOT" -fuse-ld=lld \
    --gcc-toolchain="$SYSROOT/usr" -mcpu=cortex-a35 -O2 -Wall \
    -I"$SYSROOT/usr/include/SDL2" -D_REENTRANT \
    "$DIR"/*.c -o "$DIR/$NAME" \
    -lSDL2 -lm -Wl,--allow-shlib-undefined

echo "built $DIR/$NAME"
