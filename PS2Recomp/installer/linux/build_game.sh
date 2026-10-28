#!/usr/bin/env bash
# Builds ps2EntryRunner on the player's PC from their own game files: the recompiler turns
# extracted/SLES_537.18 into C++ (source/PS2Recomp/ps2xRuntime/src/runner), then the compiler builds
# the runtime and that code. Nothing of the game is shipped; this is where the game code comes from.
# The compiled VU1 programs come from the same executable: ps2xVuGen reads their code at the places
# source/vu1_locations.toml names.
#
#   build_game.sh [--pace fast|background] [--yes] [--native|--container]
#
#   --pace fast        all processor threads (as many as the memory allows)
#   --pace background  half of them at the lowest priority, so the PC stays usable (asked when not given)
#   --yes              install missing build packages without asking
#   --prepare          only set up the build tools (packages or the container), then stop
#   --native, --container   force where it builds (default: native when the distro can provide a
#                      new enough compiler, else an Ubuntu 22.04 container with podman or docker;
#                      always the container on SteamOS and other read-only or image-based systems)
#
# Prints "STEP <text>" and "PROGRESS <0-100>" lines (on a terminal, a progress line instead).
# Exit codes: 0 done, 10 no build tools, 11 game files missing, 13 no container tool, 1 failed.
set -euo pipefail

APP="$(dirname "$(readlink -f "$0")")"
SRC="$APP/source"
ROOT="$SRC/PS2Recomp"
RUNTIME="$ROOT/ps2xRuntime"
WORK="$APP/build"
ELF="$APP/extracted/SLES_537.18"
LOG="$APP/build_log.txt"
IMAGE="localhost/s2recomp-build:2"

PACE="" ASSUME_YES=0 MODE="" INSIDE=0 JOBS="" PREPARE=0
ARGS=("$@")
while [ $# -gt 0 ]; do
    case "$1" in
        --pace) PACE="$2"; shift 2 ;;
        --yes|-y) ASSUME_YES=1; shift ;;
        --prepare) PREPARE=1; PACE=fast; shift ;;
        --native) MODE=native; shift ;;
        --container) MODE=container; shift ;;
        --inside) INSIDE=1; shift ;;        # internal: running inside the build container
        --jobs) JOBS="$2"; shift 2 ;;       # internal
        -h|--help) sed -n '2,19p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "Unknown option: $1 (see --help)"; exit 1 ;;
    esac
done

# The build takes an hour or more, and a PC (a Steam Deck in particular) that sleeps meanwhile stops
# it, so this runs again under a logind inhibitor that ends with it. Setup already holds one when it
# runs the build itself (S2R_AWAKE).
if [ "$INSIDE" = 0 ] && [ "$PREPARE" = 0 ] && [ -z "${S2R_AWAKE:-}" ]; then
    for t in systemd-inhibit elogind-inhibit; do
        if command -v "$t" >/dev/null 2>&1 && "$t" --what=sleep:idle --mode=block --who=S2Recomp --why=check true >/dev/null 2>&1; then
            S2R_AWAKE=1 exec "$t" --what=sleep:idle --mode=block --who="S2Recomp" --why="Building the game" \
                bash "$0" "${ARGS[@]}"
        fi
    done
fi

TTY=0
[ -t 1 ] && TTY=1
if [ "$INSIDE" = 0 ]; then
    mkdir -p "$WORK"
    exec > >(tee "$LOG") 2>&1
fi

step() { if [ "$TTY" = 1 ]; then printf '\n== %s\n' "$*"; else echo "STEP $*"; fi; }
progress() { if [ "$TTY" = 1 ]; then printf '\r   %3s%%' "$1"; else echo "PROGRESS $1"; fi; }
fail() { echo "$2" >&2; exit "$1"; }
ask() {
    [ "$ASSUME_YES" = 1 ] && return 0
    [ -t 0 ] || return 1
    local a
    read -r -p "$1 [Y/n] " a
    case "$a" in [nN]*) return 1 ;; *) return 0 ;; esac
}

[ -f "$ELF" ] || fail 11 "The game files are not set up yet ($ELF). Run s2recomp-disc first."

# ---- Pace ----
if [ -z "$PACE" ]; then
    PACE=fast
    if [ -t 0 ]; then
        read -r -p "Build fast (uses the whole PC) or in the background (PC stays usable)? [F/b] " a
        case "$a" in [bB]*) PACE=background ;; esac
    fi
fi
if [ -z "$JOBS" ]; then
    # About 2 GB of memory per compile job.
    threads="$(nproc)"
    mem_gb="$(awk '/MemAvailable/ { printf "%d", $2 / 1048576 }' /proc/meminfo)"
    JOBS="$threads"
    [ "$PACE" = background ] && JOBS=$(( threads / 2 ))
    [ "$JOBS" -gt $(( mem_gb / 2 )) ] && JOBS=$(( mem_gb / 2 ))
    [ "$JOBS" -lt 1 ] && JOBS=1
    echo "Using $JOBS parallel jobs ($PACE, $threads threads, $mem_gb GB free memory)"
fi
if [ "$PACE" = background ] && [ "$INSIDE" = 0 ]; then
    renice -n 19 $$ >/dev/null 2>&1 || true
    command -v ionice >/dev/null 2>&1 && ionice -c 3 -p $$ >/dev/null 2>&1 || true
fi

# ---- The build itself (natively, or inside the container) ----
version_major() { "$1" -dumpversion 2>/dev/null | cut -d. -f1; }

pick_compiler() {
    CXX="" CC=""
    for c in g++-14 g++-13 g++ clang++-19 clang++-18 clang++-17 clang++-16 clang++; do
        command -v "$c" >/dev/null 2>&1 || continue
        v="$(version_major "$c")"
        case "$c" in
            g++*) [ "${v:-0}" -ge 13 ] && { CXX="$c"; CC="${c/g++/gcc}"; return 0; } ;;
            clang++*) [ "${v:-0}" -ge 16 ] && { CXX="$c"; CC="${c/clang++/clang}"; return 0; } ;;
        esac
    done
    return 1
}

sync_dir() { # sync_dir <from> <to>: identical files keep their time stamps, so they are not compiled again
    mkdir -p "$2"
    if command -v rsync >/dev/null 2>&1; then
        rsync -r -c --delete "$1/" "$2/"
        return
    fi
    local f n
    for f in "$1"/*; do
        n="$2/$(basename "$f")"
        cmp -s "$f" "$n" || cp -f "$f" "$n"
    done
    for f in "$2"/*; do
        [ -e "$1/$(basename "$f")" ] || rm -f "$f"
    done
}

do_build() {
    pick_compiler || fail 10 "No C++ compiler new enough (GCC 13 or Clang 16) was found."
    export CC CXX
    echo "Compiler: $CXX $("$CXX" -dumpversion)"

    step "Recompiling the game"
    progress 0
    local gen="$WORK/generated"
    rm -rf "$gen"
    sed -e "s|^input *=.*|input = \"$ELF\"|" -e "s|^output *=.*|output = \"$gen/\"|" \
        "$SRC/sims2_config.toml" >"$WORK/recomp.toml"
    "$APP/ps2_recomp" "$WORK/recomp.toml" >"$WORK/recomp.log" 2>&1 || fail 1 "The recompiler failed (see $WORK/recomp.log)."
    # The headers are included everywhere: only replaced when their content changed.
    for h in ps2_recompiled_functions.h ps2_recompiled_stubs.h; do
        cmp -s "$gen/$h" "$RUNTIME/include/$h" || cp -f "$gen/$h" "$RUNTIME/include/$h"
        rm -f "$gen/$h"
    done
    sync_dir "$gen" "$RUNTIME/src/runner"

    step "Configuring"
    # The FFmpeg that comes with S2Recomp (the game loads the same libraries from lib/).
    # Point its .pc files at this folder (the package makes them relative already; this also covers
    # an install whose files were written by an older version).
    sed -i -e "s|^prefix=.*|prefix=$APP/ffmpeg|" -e 's|^libdir=.*|libdir=${prefix}/lib|' \
        -e 's|^includedir=.*|includedir=${prefix}/include|' "$APP"/ffmpeg/lib/pkgconfig/*.pc
    export PKG_CONFIG_PATH="$APP/ffmpeg/lib/pkgconfig"
    # pkg-config results are cached: a cache made with FFmpeg somewhere else (moved install, or the
    # paths fixed above) is dropped. The compiled files stay.
    if [ -f "$WORK/cmake/CMakeCache.txt" ] && grep -q '^FFMPEG_INCLUDE_DIRS' "$WORK/cmake/CMakeCache.txt" &&
        ! grep -q "^FFMPEG_INCLUDE_DIRS.*=$APP/ffmpeg/include" "$WORK/cmake/CMakeCache.txt"; then
        rm -f "$WORK/cmake/CMakeCache.txt"
    fi
    cmake -S "$ROOT" -B "$WORK/cmake" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc" \
        -DPS2X_BUILD_RECOMP=OFF -DPS2X_BUILD_ANALYZER=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF \
        -DPS2X_ENABLE_AGRESSIVE_LOGS=OFF -DPS2X_ENABLE_IOP_RPC_TRACE=OFF \
        -DFETCHCONTENT_FULLY_DISCONNECTED=ON \
        -DFETCHCONTENT_SOURCE_DIR_RAYLIB="$SRC/deps/raylib" -DFETCHCONTENT_SOURCE_DIR_IMGUI="$SRC/deps/imgui" \
        -DFETCHCONTENT_SOURCE_DIR_RLIMGUI="$SRC/deps/rlimgui" -DFETCHCONTENT_SOURCE_DIR_SDL2="$SRC/deps/sdl2" \
        -DPS2X_EOS_SDK_DIR="$SRC/EOS/SDK" \
        >"$WORK/configure.log" 2>&1 || fail 1 "Configuring the build failed (see $WORK/configure.log)."

    # VU1 programs: their code from the player's executable -> vu1gen/*.cpp, generated next to the
    # build and synced (unchanged files are not compiled again). Without them (another version of the
    # game) the VU1 interpreter runs everything.
    step "Compiling the VU1 programs"
    cmake --build "$WORK/cmake" --target ps2xVuGen >"$WORK/vugen.log" 2>&1 ||
        fail 1 "Building the VU1 program generator failed (see $WORK/vugen.log)."
    rm -rf "$WORK/vu1gen"
    if "$WORK/cmake/ps2xRuntime/ps2xVuGen" "$SRC/vu1_locations.toml" "$APP/extracted" "$WORK/vu1gen" >>"$WORK/vugen.log" 2>&1; then
        sync_dir "$WORK/vu1gen" "$RUNTIME/src/lib/vu/vu1gen"
    else
        echo "The VU1 programs could not be generated from this game version; the VU1 interpreter runs them."
    fi

    step "Compiling the game (this takes a while)"
    local last=-1
    set +e
    cmake --build "$WORK/cmake" --target ps2EntryRunner -j "$JOBS" 2>&1 | while IFS= read -r line; do
        if [[ "$line" =~ ^\[([0-9]+)/([0-9]+)\] ]]; then
            pct=$(( BASH_REMATCH[1] * 100 / BASH_REMATCH[2] ))
            if [ "$pct" != "$last" ]; then progress "$pct"; last="$pct"; fi
        elif [[ "$line" == *error* || "$line" == FAILED* ]]; then
            echo "$line"
        fi
    done
    local rc="${PIPESTATUS[0]}"
    set -e
    [ "$TTY" = 1 ] && echo
    [ "$rc" = 0 ] || fail 1 "Compiling failed (see $LOG)."

    step "Installing the game"
    cp -f "$WORK/cmake/ps2xRuntime/ps2EntryRunner" "$APP/ps2EntryRunner.new"
    mv -f "$APP/ps2EntryRunner.new" "$APP/ps2EntryRunner"
    progress 100
    [ "$TTY" = 1 ] && echo
    echo "The game is built: $APP/ps2EntryRunner"
}

if [ "$INSIDE" = 1 ]; then
    do_build
    exit 0
fi

# ---- Where to build ----
# shellcheck source=s2r-distro.sh
. "$APP/s2r-distro.sh"

native_ready() {
    pick_compiler || return 1
    command -v cmake >/dev/null 2>&1 && command -v ninja >/dev/null 2>&1 && command -v pkg-config >/dev/null 2>&1 || return 1
    local cv
    cv="$(cmake --version | awk 'NR == 1 { print $3 }')"
    [ "$(printf '%s\n3.21\n' "$cv" | sort -V | head -1)" = 3.21 ] || return 1
    local h
    for h in X11/Xlib.h X11/extensions/Xrandr.h X11/extensions/Xinerama.h X11/Xcursor/Xcursor.h X11/extensions/XInput2.h GL/gl.h; do
        [ -f "/usr/include/$h" ] || return 1
    done
}

install_build_packages() {
    local cmd=""
    case "$FAMILY" in
        debian) cmd="${S2R_SUDO:-sudo} apt-get install -y build-essential g++ clang cmake ninja-build pkg-config rsync libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxext-dev libgl1-mesa-dev libwayland-dev libxkbcommon-dev libudev-dev libasound2-dev libpulse-dev" ;;
        fedora) cmd="${S2R_SUDO:-sudo} dnf install -y gcc-c++ clang cmake ninja-build pkgconf-pkg-config rsync libX11-devel libXrandr-devel libXinerama-devel libXcursor-devel libXi-devel libXext-devel mesa-libGL-devel wayland-devel libxkbcommon-devel systemd-devel alsa-lib-devel pulseaudio-libs-devel" ;;
        arch) cmd="${S2R_SUDO:-sudo} pacman -S --needed --noconfirm base-devel clang cmake ninja pkgconf rsync libx11 libxrandr libxinerama libxcursor libxi libxext mesa wayland libxkbcommon systemd-libs alsa-lib libpulse" ;;
        suse) cmd="${S2R_SUDO:-sudo} zypper --non-interactive install gcc-c++ clang cmake ninja pkg-config rsync libX11-devel libXrandr-devel libXinerama-devel libXcursor-devel libXi-devel libXext-devel Mesa-libGL-devel wayland-devel libxkbcommon-devel systemd-devel alsa-devel libpulse-devel" ;;
        *) return 1 ;;
    esac
    echo "Building needs a C++ compiler and some development packages."
    ask "Install them now with: $cmd ?" || return 1
    [ "$FAMILY" = debian ] && { ${S2R_SUDO:-sudo} apt-get update || true; }
    $cmd
}

container_tool() {
    if command -v podman >/dev/null 2>&1; then echo podman; return; fi
    if command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then echo docker; return; fi
    return 1
}

# Sets TOOL (podman or docker, plus its options in TOOL_ARGS) and makes sure the build image exists.
container_image() {
    TOOL_ARGS=()
    # Rootless podman puts containers in cgroups through the user's systemd bus; without one (an ssh
    # or su session) it has to manage them itself.
    if [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ] && [ ! -S "/run/user/$(id -u)/bus" ]; then
        TOOL_ARGS=(--cgroup-manager=cgroupfs)
    fi
    TOOL="$(container_tool)" || {
        case "$FAMILY" in
            steamos) fail 13 "SteamOS normally comes with podman; it was not found. Update SteamOS (Settings > System), then run this again." ;;
            atomic) fail 13 "Install podman (it usually comes with image-based systems), then run this again." ;;
            *) fail 13 "Building here needs podman or docker (or a new enough compiler: GCC 13 or Clang 16). Install one, then run this again." ;;
        esac
    }
    [ "$TOOL" = podman ] || TOOL_ARGS=()
    if ! "$TOOL" "${TOOL_ARGS[@]}" image exists "$IMAGE" >/dev/null 2>&1 && ! "$TOOL" "${TOOL_ARGS[@]}" image inspect "$IMAGE" >/dev/null 2>&1; then
        step "Preparing the build container (once; downloads about 1 GB)"
        "$TOOL" "${TOOL_ARGS[@]}" build -t "$IMAGE" -f - "$WORK" <<'EOF'
FROM docker.io/library/ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y software-properties-common && \
    add-apt-repository -y ppa:ubuntu-toolchain-r/test && apt-get update && \
    apt-get install -y g++-13 clang-15 ninja-build pkg-config rsync python3-pip \
      libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxext-dev libgl1-mesa-dev \
      libwayland-dev libxkbcommon-dev libudev-dev libasound2-dev libpulse-dev && \
    pip3 install --no-cache-dir cmake==3.31.6 && rm -rf /var/lib/apt/lists/*
EOF
    fi
}

build_in_container() {
    container_image
    step "Building in the container"
    local user_args=(--userns=keep-id --security-opt label=disable)
    [ "$TOOL" = docker ] && user_args=(--user "$(id -u):$(id -g)")
    "$TOOL" "${TOOL_ARGS[@]}" run --rm "${user_args[@]}" -e HOME=/tmp -v "$APP:$APP" -w "$APP" "$IMAGE" \
        bash "$APP/build_game.sh" --inside --pace "$PACE" --jobs "$JOBS"
}

if [ -z "$MODE" ]; then
    case "$FAMILY" in
        steamos|atomic|nixos) MODE=container ;;
        *)
            if native_ready; then
                MODE=native
            elif install_build_packages && native_ready; then
                MODE=native
            else
                MODE=container
            fi ;;
    esac
fi
echo "Building on ${PRETTY_NAME:-Linux}: $MODE"
if [ "$PREPARE" = 1 ]; then
    [ "$MODE" != container ] || container_image
    echo "The build tools are ready."
    exit 0
fi
if [ "$MODE" = native ]; then
    do_build
else
    build_in_container
fi
