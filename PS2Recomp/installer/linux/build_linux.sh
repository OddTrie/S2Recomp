#!/usr/bin/env bash
# Builds the Linux release of S2Recomp: the launcher (S2RecompLauncher), the recompiler (ps2_recomp, which the
# player's PC runs on their own game files), s2r_setup and a minimal shared
# FFmpeg 7.1 (the same release the Windows build downloads; ps2EntryRunner loads it from lib/). Then run
# package_linux.sh to make the tarball. ps2EntryRunner is not part of a release; WITH_RUNNER=1 builds it too.
#
# Made for Ubuntu 22.04 (older glibc, so the result runs on most current distros). Needs:
#   sudo add-apt-repository ppa:ubuntu-toolchain-r/test
#   sudo apt install build-essential g++-13 git pkg-config ninja-build curl nasm zlib1g-dev \
#     libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxext-dev libgl1-mesa-dev \
#     libwayland-dev libxkbcommon-dev libudev-dev libasound2-dev libpulse-dev libdbus-1-dev patchelf
#   pip install cmake   (3.20 or newer)
#
# Usage: build_linux.sh [work dir]   (default ~/s2r-build; the source is the PS2Recomp folder this script is in)
set -euo pipefail

SRC="$(cd "$(dirname "$0")/../.." && pwd)"
WORK="${1:-$HOME/s2r-build}"
FFMPEG_VERSION=7.1
FFMPEG_PREFIX="$WORK/ffmpeg"
BUILD="$WORK/build"
JOBS="${JOBS:-$(nproc)}"
export CC="${CC:-gcc-13}" CXX="${CXX:-g++-13}"

mkdir -p "$WORK"

if [ ! -f "$FFMPEG_PREFIX/lib/pkgconfig/libavcodec.pc" ]; then
    echo "== FFmpeg $FFMPEG_VERSION (MPEG-2 video decoder only)"
    cd "$WORK"
    [ -f "ffmpeg-$FFMPEG_VERSION.tar.xz" ] || curl -fL -o "ffmpeg-$FFMPEG_VERSION.tar.xz" "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz"
    rm -rf "ffmpeg-$FFMPEG_VERSION" && tar xf "ffmpeg-$FFMPEG_VERSION.tar.xz"
    cd "ffmpeg-$FFMPEG_VERSION"
    ./configure --prefix="$FFMPEG_PREFIX" --cc="$CC" --enable-shared --disable-static --enable-pic \
        --disable-programs --disable-doc --disable-network --disable-autodetect --disable-everything \
        --disable-avdevice --disable-avfilter \
        --enable-decoder=mpeg2video --enable-parser=mpegvideo --enable-swscale --enable-swresample
    make -j"$JOBS"
    make install
fi

echo "== S2RecompLauncher"
export PKG_CONFIG_PATH="$FFMPEG_PREFIX/lib/pkgconfig"
# libstdc++ is linked in: g++-13's is newer than the one older distros ship, and a bundled copy
# could clash with the GL driver's.
cmake -S "$SRC" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc" \
    -DPS2X_BUILD_ANALYZER=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF -DPS2X_BUILD_RECOMP=ON
cmake --build "$BUILD" --target S2RecompLauncher ps2_recomp -j"$JOBS"
if [ "${WITH_RUNNER:-0}" = 1 ]; then
    echo "== ps2EntryRunner"
    cmake --build "$BUILD" --target ps2EntryRunner -j"$JOBS"
fi

echo "== s2r_setup"
"$CXX" -std=c++17 -O2 -Wall -Wextra -static-libstdc++ -static-libgcc \
    "$SRC/installer/s2r_setup.cpp" -o "$BUILD/s2r_setup"

echo "Built: $BUILD/ps2xRuntime/S2RecompLauncher and $BUILD/s2r_setup"
