#!/bin/sh
# Started by the .run once it has unpacked itself. On a desktop it opens the setup window (setup.py,
# Python 3 with Tk); otherwise, or with --cli, the terminal installer (install.sh).
#
#   S2Recomp-<version>-linux-x86_64.run [--gui | --cli [install.sh options]]
here="$(cd "$(dirname "$0")" && pwd)"
title="S2Recomp Setup"

error() {
    echo "$1" >&2
    if command -v zenity >/dev/null 2>&1; then zenity --error --title="$title" --text="$1" 2>/dev/null
    elif command -v kdialog >/dev/null 2>&1; then kdialog --title "$title" --error "$1" 2>/dev/null
    elif command -v notify-send >/dev/null 2>&1; then notify-send "$title" "$1"
    fi
}

[ "$(uname -m)" = x86_64 ] || { error "S2Recomp needs a 64-bit x86 PC (this is $(uname -m))."; exit 1; }

mode=gui
case "${1:-}" in
    --gui) shift ;;
    --cli) mode=cli; shift ;;
    -*) mode=cli ;; # install.sh options
esac
[ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] || mode=cli

if [ "$mode" = gui ]; then
    for py in python3 python3.13 python3.12 python3.11 python3.10 python3.9 python3.8; do
        command -v "$py" >/dev/null 2>&1 || continue
        "$py" -c 'import sys, tkinter; sys.exit(sys.version_info < (3, 8))' 2>/dev/null || continue
        "$py" "$here/setup.py"
        exit $?
    done
    echo "Python 3 with Tk (python3-tk / python3-tkinter / tk) was not found; using the terminal installer."
    no_tk=1
fi

# Terminal installer. Double-clicked (no terminal): open one and run the .run again in it.
if [ ! -t 0 ] && [ -z "${S2R_IN_TERMINAL:-}" ] && [ -n "${S2R_RUN:-}" ] && [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then
    for t in konsole gnome-terminal ptyxis kgx xfce4-terminal mate-terminal tilix lxterminal alacritty kitty foot xterm x-terminal-emulator; do
        command -v "$t" >/dev/null 2>&1 || continue
        case "$t" in
            gnome-terminal|ptyxis|kgx) "$t" -- env S2R_IN_TERMINAL=1 sh "$S2R_RUN" --cli "$@" ;;
            xfce4-terminal|mate-terminal) "$t" -x env S2R_IN_TERMINAL=1 sh "$S2R_RUN" --cli "$@" ;;
            tilix|lxterminal) "$t" -e "env S2R_IN_TERMINAL=1 sh '$S2R_RUN' --cli $*" ;;
            kitty|foot) "$t" env S2R_IN_TERMINAL=1 sh "$S2R_RUN" --cli "$@" ;;
            *) "$t" -e env S2R_IN_TERMINAL=1 sh "$S2R_RUN" --cli "$@" ;;
        esac && exit 0
    done
    msg="Install Python 3 with Tk (python3-tk) for the setup window, or open a terminal and run: sh \"$S2R_RUN\""
    [ -n "${no_tk:-}" ] || msg="Open a terminal and run: sh \"$S2R_RUN\""
    error "$msg"
    exit 1
fi

bash "$here/install.sh" "$@"
status=$?
if [ -n "${S2R_IN_TERMINAL:-}" ]; then
    echo
    printf 'Press Enter to close this window. '
    read -r _ || true
fi
exit $status
