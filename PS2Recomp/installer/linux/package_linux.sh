#!/usr/bin/env bash
# Packs the build from build_linux.sh into S2Recomp-<version>-linux-x86_64.tar.gz:
#
#   S2Recomp-<version>-linux-x86_64/
#     install.sh  uninstall.sh  README.txt
#     files/  S2RecompLauncher  s2r_setup  s2recomp-disc.sh  build_game.sh  s2r-distro.sh  ps2_recomp
#             lib/ (FFmpeg for the game)  ffmpeg/ (FFmpeg to build against)  source/ (what the game is built from)
#             mods/  texpacks/  LICENSE.txt
#
# A release holds no game data and no game code: the player's PC builds ps2EntryRunner from their own
# disc (build_game.sh). The package is checked for both before it is made.
#
# Usage: package_linux.sh [version] [work dir] [output dir]
set -euo pipefail

VERSION="${1:-0.1.0}"
WORK="${2:-$HOME/s2r-build}"
SRC="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${3:-$SRC/installer/build/dist}"
BUILD="$WORK/build"
NAME="S2Recomp-$VERSION-linux-x86_64"
STAGE="$WORK/stage/$NAME"

[ -x "$BUILD/ps2xRuntime/S2RecompLauncher" ] && [ -x "$BUILD/ps2xRecomp/ps2_recomp" ] || { echo "Run build_linux.sh first."; exit 1; }

rm -rf "$STAGE"
mkdir -p "$STAGE/files/lib" "$OUT"
cp "$BUILD/ps2xRuntime/S2RecompLauncher" "$BUILD/s2r_setup" "$BUILD/ps2xRecomp/ps2_recomp" "$STAGE/files/"
strip --strip-unneeded "$STAGE/files/S2RecompLauncher" "$STAGE/files/s2r_setup" "$STAGE/files/ps2_recomp"
# FFmpeg's libraries for ps2EntryRunner, which the launcher starts with lib/ on LD_LIBRARY_PATH.
for lib in "$WORK"/ffmpeg/lib/lib*.so.*; do
    case "$lib" in *.so.[0-9]*.[0-9]*) continue ;; esac # copy each library once, under its soname
    cp -L "$lib" "$STAGE/files/lib/"
done
for lib in "$STAGE"/files/lib/*.so.*; do patchelf --set-rpath '$ORIGIN' "$lib"; done
# Online co-op: the EOS SDK library (the launcher and the game load it from lib/) and the Epic Online
# Services product's client credentials (eos.ini, read by the Lobbies tab and the game).
EOS_SDK="$SRC/../../EOS/SDK"
[ -f "$EOS_SDK/Include/eos_sdk.h" ] && [ -f "$EOS_SDK/Bin/libEOSSDK-Linux-Shipping.so" ] || { echo "The EOS SDK was not found at $EOS_SDK."; exit 1; }
readelf -d "$BUILD/ps2xRuntime/S2RecompLauncher" | grep -q libEOSSDK || { echo "The launcher was built without EOS: run build_linux.sh again."; exit 1; }
cp "$EOS_SDK/Bin/libEOSSDK-Linux-Shipping.so" "$STAGE/files/lib/"
# The build's RUNPATH also names the SDK folder on the build machine; installed, it is only lib/.
patchelf --set-rpath '$ORIGIN/lib' "$STAGE/files/S2RecompLauncher"
EOS_INI=""
for f in "$SRC/../eos.ini" "$SRC/build_win/ps2xRuntime/Release/eos.ini"; do [ -f "$f" ] && { EOS_INI="$f"; break; }; done
if [ -n "$EOS_INI" ]; then cp "$EOS_INI" "$STAGE/files/eos.ini"; else echo "Warning: no eos.ini; online co-op will not work."; fi
# FFmpeg's headers and libraries to build the game against (build_game.sh fixes the .pc prefix).
cp -a "$WORK/ffmpeg" "$STAGE/files/ffmpeg"
rm -rf "$STAGE/files/ffmpeg/share"
sed -i -e 's|^prefix=.*|prefix=${pcfiledir}/../..|' -e 's|^libdir=.*|libdir=${prefix}/lib|' \
    -e 's|^includedir=.*|includedir=${prefix}/include|' "$STAGE"/files/ffmpeg/lib/pkgconfig/*.pc
cp -r "$SRC/ps2xRuntime/mods" "$SRC/ps2xRuntime/texpacks" "$STAGE/files/"
cp "$SRC/LICENSE" "$STAGE/files/LICENSE.txt"
cp "$SRC/ps2xRuntime/assets/s2recomp.png" "$STAGE/files/"
cp "$SRC/installer/linux/s2recomp-disc.sh" "$SRC/installer/linux/build_game.sh" "$SRC/installer/linux/s2r-distro.sh" "$STAGE/files/"
cp "$SRC/installer/linux/install.sh" "$SRC/installer/linux/uninstall.sh" "$SRC/installer/linux/README.txt" "$STAGE/"

# What the player's PC builds the game from: the runtime's source without any generated game code,
# the libraries it uses (so no git is needed), and the game's config.
B="$STAGE/files/source"
mkdir -p "$B/PS2Recomp" "$B/deps"
cp "$SRC/CMakeLists.txt" "$SRC/LICENSE" "$B/PS2Recomp/"
tar -C "$SRC" -cf - --exclude=.git --exclude='ps2xRuntime/src/runner/*' --exclude='ps2xRuntime/src/lib/vu/vu1gen/*' \
    --exclude=ps2xRuntime/vita --exclude='ps2_recompiled_*.h' ps2xRuntime ps2xIOP | tar -C "$B/PS2Recomp" -xf -
mkdir -p "$B/PS2Recomp/ps2xRuntime/src/runner" "$B/PS2Recomp/ps2xRuntime/src/lib/vu/vu1gen"
# No compiled VU1 programs: build_game.sh generates them from the player's executable (ps2xVuGen,
# vu1_locations.toml); until then, the registry --vu-gen writes for an empty folder. Their PGO profile
# for the clang++ that compiles them (src/lib/vu/pgo/vu1_linux.profdata) comes with the source.
cp "$SRC/installer/vu1_registry_empty.cpp" "$B/PS2Recomp/ps2xRuntime/src/lib/vu/vu1gen/vu1_registry.cpp"
for dep in raylib imgui rlimgui sdl2; do
    mkdir -p "$B/deps/$dep"
    tar -C "$BUILD/_deps/$dep-src" -cf - --exclude=.git --exclude=./examples --exclude=./projects . | tar -C "$B/deps/$dep" -xf -
done
cp "$SRC/../sims2_config.toml" "$SRC/../vu1_locations.toml" "$B/"
# The EOS C SDK's headers and library, so the game built on the player's PC has online co-op.
mkdir -p "$B/EOS/SDK/Bin"
cp -r "$EOS_SDK/Include" "$B/EOS/SDK/"
cp "$EOS_SDK/Bin/libEOSSDK-Linux-Shipping.so" "$B/EOS/SDK/Bin/"

sed -i 's/\r$//' "$STAGE"/*.sh "$STAGE"/files/*.sh "$STAGE/README.txt"
chmod +x "$STAGE"/*.sh "$STAGE"/files/*.sh "$STAGE/files/S2RecompLauncher" "$STAGE/files/s2r_setup" "$STAGE/files/ps2_recomp"

if find "$STAGE" \( -iname 'ps2EntryRunner*' -o -iname 'SLES_*' -o -iname 'SYSTEM.CNF' -o -iname '*.iso' \
        -o -iname 'IOPRP*.IMG' -o -iname '*.NGH' -o -name 'sub_[0-9A-F]*_0x*.cpp' -o -name 'entry_*_0x*.cpp' \
        -o -name 'vu1p_*.cpp' -o -name '*.vur' -o -name '*.profdata' -o -name 'ps2_recompiled_*.h' \) \
        ! -path '*/src/lib/vu/pgo/vu1_linux.profdata' | grep .; then
    echo "The package must not contain the runner, game files or game code (above)."
    exit 1
fi

# The newest glibc symbol needed decides the oldest distro it runs on.
echo "Needs glibc $(objdump -T "$STAGE/files/S2RecompLauncher" "$STAGE/files/s2r_setup" "$STAGE/files/ps2_recomp" "$STAGE"/files/lib/*.so.* |
    grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)"

tar -C "$WORK/stage" -czf "$OUT/$NAME.tar.gz" "$NAME"
echo "Built $OUT/$NAME.tar.gz ($(du -h "$OUT/$NAME.tar.gz" | cut -f1))"
