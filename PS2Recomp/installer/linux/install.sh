#!/usr/bin/env bash
# S2Recomp installer for Linux. Run it from the unpacked release folder:
#
#   ./install.sh [--iso PATH] [--build fast|background|none] [--prefix DIR] [--yes] [--no-deps] [--steam|--no-steam] [--check]
#
# --steam adds S2Recomp to Steam without asking (otherwise it asks when Steam is installed).
# --check only reports the detected system and what would be installed, and changes nothing.
#
# Installs for the current user only (default ~/Games/S2Recomp, no root needed), which also
# works on SteamOS and other read-only systems, where the home folder is the only place that
# survives system updates. What else it does depends on the distro (from /etc/os-release):
#   SteamOS                 no package installs (read-only system); adds S2Recomp to Steam so it
#                           can be started from Game Mode
#   Bazzite, Fedora Atomic  no package installs (image-based); says what to layer if anything
#   and other image-based   is missing; offers to add it to Steam when Steam is installed
#   Debian, Ubuntu, Mint,   installs missing system libraries with apt / dnf / pacman / zypper
#   Fedora, Arch, openSUSE  (asks first, uses sudo)
# Then it copies the game files out of your own disc image into extracted/, where the launcher finds them.
# Last it builds the game (ps2EntryRunner) from those files on this PC: no game files and no game code
# come with S2Recomp.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
# ~/Games/<name>, like the other recomps; an install in the old default folder is updated where it is.
PREFIX="$HOME/Games/S2Recomp"
OLD_PREFIX="${XDG_DATA_HOME:-$HOME/.local/share}/s2recomp"
[ ! -e "$PREFIX" ] && [ -f "$OLD_PREFIX/S2RecompLauncher" ] && PREFIX="$OLD_PREFIX"
BIN_DIR="$HOME/.local/bin"
APPS_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
ISO=""
ASSUME_YES=0
DO_DEPS=1
DO_STEAM=1
CHECK_ONLY=0
BUILD=""
ARGS=("$@")

while [ $# -gt 0 ]; do
    case "$1" in
        --iso) ISO="$2"; shift 2 ;;
        --iso=*) ISO="${1#*=}"; shift ;;
        --prefix) PREFIX="$2"; shift 2 ;;
        --prefix=*) PREFIX="${1#*=}"; shift ;;
        --yes|-y) ASSUME_YES=1; shift ;;
        --no-deps) DO_DEPS=0; shift ;;
        --no-steam) DO_STEAM=0; shift ;;
        --steam) DO_STEAM=2; shift ;;
        --check) CHECK_ONLY=1; shift ;;
        --build) BUILD="$2"; shift 2 ;;
        --build=*) BUILD="${1#*=}"; shift ;;
        -h|--help) sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "Unknown option: $1 (see --help)"; exit 1 ;;
    esac
done

# The first install takes an hour or more, and a PC (a Steam Deck in particular) that sleeps meanwhile
# stops it, so this runs again under a logind inhibitor. The inhibitor ends with this script, even
# when it is killed, so the PC is never left unable to sleep. Where none works, it just says so.
AWAKE=0
if [ "$CHECK_ONLY" = 0 ] && [ -z "${S2R_AWAKE:-}" ]; then
    for t in systemd-inhibit elogind-inhibit; do
        if command -v "$t" >/dev/null 2>&1 && "$t" --what=sleep:idle --mode=block --who=S2Recomp --why=check true >/dev/null 2>&1; then
            S2R_AWAKE=1 exec "$t" --what=sleep:idle --mode=block --who="S2Recomp Setup" --why="Installing and building the game" \
                bash "$0" "${ARGS[@]}"
        fi
    done
fi
[ "${S2R_AWAKE:-}" = 1 ] && AWAKE=1

# On a terminal, colours; otherwise (the setup window reads the output) plain lines, plus "STEP <text>"
# and "PROGRESS <0-100>" lines for its status and progress bar.
TTY=0
[ -t 1 ] && TTY=1
if [ "$TTY" = 1 ]; then
    bold() { printf '\033[1m%s\033[0m\n' "$*"; }
    warn() { printf '\033[33m%s\033[0m\n' "$*"; }
    die() { printf '\033[31m%s\033[0m\n' "$*" >&2; exit 1; }
    step() { :; }
else
    bold() { echo "$*"; }
    warn() { echo "$*"; }
    die() { echo "ERROR $*"; exit 1; }
    step() { echo "STEP $*"; }
fi
SUDO="${S2R_SUDO:-sudo}" # the setup window sets pkexec, which asks for the password in a window
ask() { # ask "question" -> 0 for yes; --yes answers yes, no terminal answers no
    [ "$ASSUME_YES" = 1 ] && return 0
    [ -t 0 ] || return 1
    local a
    read -r -p "$1 [Y/n] " a
    case "$a" in [nN]*) return 1 ;; *) return 0 ;; esac
}

PREFIX="$(realpath -m "$PREFIX")"
case "$PREFIX" in /|/usr|/usr/*|/etc|/etc/*|"$HOME") die "Refusing to install into $PREFIX; use a folder of its own." ;; esac
[ -f "$HERE/files/S2RecompLauncher" ] || die "Run install.sh from the unpacked S2Recomp folder (files/S2RecompLauncher is missing)."
[ "$(id -u)" != 0 ] || die "Run install.sh as your normal user, not with sudo; it asks for the password itself when it installs packages."

# ---- What system is this ----
# shellcheck source=files/s2r-distro.sh
. "$HERE/files/s2r-distro.sh"
bold "S2Recomp installer - ${PRETTY_NAME:-Linux} (${FAMILY})"

[ "$(uname -m)" = x86_64 ] || die "S2Recomp needs a 64-bit x86 PC (this is $(uname -m))."
grep -qw avx2 /proc/cpuinfo || die "S2Recomp needs a CPU with AVX2 (Intel Haswell / AMD Zen or newer)."

# ---- System libraries ----
# Everything else (FFmpeg, the C++ runtime) comes with S2Recomp. The launcher and game open the display
# and audio libraries itself, so they are checked by name as well as through ldd.
need_libs="libGL.so.1 libX11.so.6 libXrandr.so.2 libXi.so.6 libXcursor.so.1 libXinerama.so.1 libasound.so.2"
missing=""
ldcache="$( { ldconfig -p || /sbin/ldconfig -p; } 2>/dev/null || true)"
if [ -n "$ldcache" ]; then
    for lib in $need_libs; do
        grep -q "/$lib\$" <<<"$ldcache" || missing="$missing $lib"
    done
fi
missing="$missing $(LD_LIBRARY_PATH="$HERE/files/lib" ldd "$HERE/files/S2RecompLauncher" 2>/dev/null | awk '/not found/ {print $1}' | tr '\n' ' ' || true)"
missing="$(echo "$missing" | xargs -n1 2>/dev/null | sort -u | xargs)"

pkgs="" cmd=""
case "$FAMILY" in
    debian)
        asound=libasound2
        apt-cache show libasound2t64 >/dev/null 2>&1 && asound=libasound2t64 # Ubuntu 24.04+, Debian 13+
        pkgs="libgl1 libx11-6 libxrandr2 libxi6 libxcursor1 libxinerama1 $asound"
        cmd="$SUDO apt-get install -y $pkgs" ;;
    fedora)
        pkgs="mesa-libGL libX11 libXrandr libXi libXcursor libXinerama alsa-lib"
        cmd="$SUDO dnf install -y $pkgs" ;;
    arch)
        pkgs="libglvnd mesa libx11 libxrandr libxi libxcursor libxinerama alsa-lib"
        cmd="$SUDO pacman -S --needed --noconfirm $pkgs" ;;
    suse)
        pkgs="Mesa-libGL1 libX11-6 libXrandr2 libXi6 libXcursor1 libXinerama1 libasound2"
        cmd="$SUDO zypper --non-interactive install $pkgs" ;;
esac

if [ "$CHECK_ONLY" = 1 ]; then
    echo "System family: $FAMILY"
    echo "Missing libraries: ${missing:-none}"
    echo "Package command: ${cmd:-none (no package installs on this system)}"
    [ "$FAMILY" = steamos ] && echo "Steam: steamos-add-to-steam $(command -v steamos-add-to-steam >/dev/null 2>&1 && echo found || echo 'not found')"
    exit 0
fi

if [ -n "$missing" ]; then
    warn "Missing system libraries: $missing"
    if [ -n "$cmd" ] && [ "$DO_DEPS" = 1 ]; then
        if ask "Install them now with: $cmd ?"; then
            step "Installing system libraries"
            $cmd || die "Installing the packages failed. Install them yourself and run install.sh again."
        else
            warn "Not installing them. The game will not start until they are installed:"
            warn "  $cmd"
        fi
    else
        case "$FAMILY" in
            steamos)
                warn "SteamOS normally has all of these. Updating SteamOS (Settings > System) should bring them back;"
                warn "S2Recomp does not change the read-only system." ;;
            atomic)
                warn "This is an image-based system, so S2Recomp does not install packages. Layer them with"
                warn "  rpm-ostree install mesa-libGL libX11 libXrandr libXi libXcursor libXinerama alsa-lib"
                warn "and reboot, or run S2Recomp from a distrobox." ;;
            nixos)
                warn "On NixOS run S2Recomp through steam-run (or nix-ld): steam-run ~/.local/bin/s2recomp" ;;
            *)
                warn "Install these with your package manager, then start S2Recomp." ;;
        esac
    fi
fi

# ---- Program files ----
step "Copying S2Recomp"
bold "Installing to $PREFIX"
if [ "$AWAKE" = 1 ]; then echo "Keeping the PC awake until setup finishes."
else warn "Note: this PC cannot be kept awake automatically; keep it from sleeping until setup finishes."; fi
mkdir -p "$PREFIX" "$BIN_DIR" "$APPS_DIR"
cp -f "$HERE/files/S2RecompLauncher" "$HERE/files/s2r_setup" "$HERE/files/s2recomp-disc.sh" "$HERE/files/build_game.sh" \
    "$HERE/files/s2r-distro.sh" "$HERE/files/s2r-steam.sh" "$HERE/files/ps2_recomp" "$HERE/files/s2recomp.png" "$HERE/uninstall.sh" "$PREFIX/"
[ -f "$HERE/files/LICENSE.txt" ] && cp -f "$HERE/files/LICENSE.txt" "$PREFIX/"
[ -f "$HERE/files/version.txt" ] && cp -f "$HERE/files/version.txt" "$PREFIX/"
[ -f "$HERE/files/eos.ini" ] && cp -f "$HERE/files/eos.ini" "$PREFIX/"
rm -rf "${PREFIX:?}/lib" "${PREFIX:?}/ffmpeg" && cp -a "$HERE/files/lib" "$HERE/files/ffmpeg" "$PREFIX/"
# What the game is built from. Copied over the old one, so the code generated from the game files
# stays and an update only recompiles what changed.
mkdir -p "$PREFIX/source" && cp -a "$HERE/files/source/." "$PREFIX/source/"
# Mods and texture packs are the player's to change, so an update never overwrites them.
cp -an "$HERE/files/mods" "$HERE/files/texpacks" "$PREFIX/" 2>/dev/null || true
chmod +x "$PREFIX/S2RecompLauncher" "$PREFIX/s2r_setup" "$PREFIX/s2recomp-disc.sh" "$PREFIX/build_game.sh" "$PREFIX/ps2_recomp" "$PREFIX/s2r-steam.sh" "$PREFIX/uninstall.sh"
mkdir -p "$PREFIX/extracted"
ln -sf "$PREFIX/S2RecompLauncher" "$BIN_DIR/s2recomp"
ln -sf "$PREFIX/s2recomp-disc.sh" "$BIN_DIR/s2recomp-disc"
ln -sf "$PREFIX/build_game.sh" "$BIN_DIR/s2recomp-build"

cat >"$APPS_DIR/s2recomp.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=S2Recomp
Comment=The Sims 2 (PS2), natively recompiled
Exec="$PREFIX/S2RecompLauncher"
Path=$PREFIX
Icon=$PREFIX/s2recomp.png
Terminal=false
Categories=Game;
Actions=disc;mods;

[Desktop Action disc]
Name=Change game disc
Exec="$PREFIX/s2recomp-disc.sh"

[Desktop Action mods]
Name=Open mods folder
Exec=xdg-open "$PREFIX/mods"
EOF
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$APPS_DIR" >/dev/null 2>&1 || true

# ---- Game files from the player's disc ----
# Into extracted/ next to the launcher, where it finds them.
if [ -f "$PREFIX/extracted/SLES_537.18" ] && [ -z "$ISO" ]; then
    echo "Game files are already set up (run s2recomp-disc to change the disc)."
else
    if [ -z "$ISO" ] && [ -t 0 ]; then
        echo
        echo "S2Recomp needs your own copy of The Sims 2 for PlayStation 2 (PAL / Europe, SLES-53718)"
        echo "as a disc image (.iso). Leave this empty to do it later with s2recomp-disc."
        read -r -p "Disc image path: " ISO
        ISO="${ISO/#\~/$HOME}"
    fi
    if [ -n "$ISO" ]; then
        step "Copying the game files from the disc image"
        "$PREFIX/s2r_setup" --check "$ISO" >/dev/null || die "This disc image cannot be used (see above). Run install.sh --iso <file> again with the right one."
        if ! "$PREFIX/s2r_setup" "$ISO" "$PREFIX/extracted" | while read -r line; do
            case "$line" in
                PROGRESS*) if [ "$TTY" = 1 ]; then printf '\r  copying game files... %s%%' "${line#PROGRESS }"; else echo "$line"; fi ;;
                *) if [ "$TTY" = 1 ]; then printf '\r%s\n' "$line"; else echo "$line"; fi ;;
            esac
        done; then
            die "Copying the game files failed."
        fi
    fi
fi

# ---- Build the game from those files ----
# No game code comes with S2Recomp: build_game.sh recompiles the player's own executable and compiles
# it, natively or (SteamOS, image-based systems, old compilers) in a podman/docker container.
built=0
if [ -f "$PREFIX/extracted/SLES_537.18" ] && [ "$BUILD" != none ]; then
    if [ -z "$BUILD" ]; then
        BUILD=fast
        if [ "$ASSUME_YES" = 0 ] && [ -t 0 ]; then
            echo
            echo "S2Recomp builds the game on this PC from your disc: about 30 minutes to a few hours, and about 5 GB."
            read -r -p "Build it now [F]ast (whole PC), in the [b]ackground (PC stays usable), or [l]ater? " a
            case "$a" in [bB]*) BUILD=background ;; [lL]*) BUILD=none ;; esac
        fi
    fi
    yes_arg=()
    [ "$ASSUME_YES" = 1 ] && yes_arg=(--yes)
    case "$BUILD" in
        fast)
            if "$PREFIX/build_game.sh" --pace fast "${yes_arg[@]}"; then built=1; else warn "Building failed; s2recomp-build tries again (details in $PREFIX/build_log.txt)."; fi ;;
        background)
            # Packages and the container are set up first, while there is someone to answer questions.
            if "$PREFIX/build_game.sh" --prepare "${yes_arg[@]}"; then
                # It outlives setup, so it holds its own inhibitor rather than setup's.
                env -u S2R_AWAKE nohup setsid "$PREFIX/build_game.sh" --pace background --yes </dev/null >/dev/null 2>&1 &
                built=2
            else
                warn "The build tools could not be set up; s2recomp-build tries again."
            fi ;;
    esac
fi

# ---- Steam (library, Big Picture, Game Mode on the Steam Deck) ----
added_to_steam=0 steam_msg="" has_steam=0
if command -v steam >/dev/null 2>&1 || command -v steamos-add-to-steam >/dev/null 2>&1 ||
    [ -d "$HOME/.var/app/com.valvesoftware.Steam" ]; then has_steam=1; fi
if [ "$DO_STEAM" = 2 ] || { [ "$DO_STEAM" = 1 ] && [ "$has_steam" = 1 ] && ask "Add S2Recomp to Steam (library, Big Picture, Game Mode)?"; }; then
    step "Adding S2Recomp to Steam"
    if steam_msg="$("$PREFIX/s2r-steam.sh" "$PREFIX")"; then added_to_steam=1; fi
fi

echo
bold "S2Recomp is installed."
echo "  Start the launcher from your application menu (Games), or run: s2recomp"
case ":$PATH:" in *":$BIN_DIR:"*) ;; *) echo "  ($BIN_DIR is not on your PATH; the full command is $PREFIX/S2RecompLauncher)" ;; esac
if [ -n "$steam_msg" ]; then
    echo "$steam_msg" | sed 's/^/  Steam: /'
    [ "$added_to_steam" = 1 ] && [ "$FAMILY" = steamos ] && echo "  It is under Non-Steam in your library; switch to Game Mode to play it there."
elif [ "$has_steam" = 1 ]; then
    echo "  To add it to Steam later, run: $PREFIX/s2r-steam.sh"
fi
echo "  Mods: $PREFIX/mods    Uninstall: $PREFIX/uninstall.sh"
case "$built" in
    1) echo "  The game is built and ready to play." ;;
    2) echo "  The game is being built in the background ($PREFIX/build_log.txt); play once it has finished." ;;
    *) [ -x "$PREFIX/ps2EntryRunner" ] || warn "  The game is not built yet: run s2recomp-build (or use Build game in the launcher)." ;;
esac