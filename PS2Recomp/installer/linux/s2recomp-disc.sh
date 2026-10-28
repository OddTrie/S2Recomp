#!/usr/bin/env bash
# Copies the game files from the player's own disc image into extracted/ next to S2RecompLauncher,
# where the launcher finds them.
#
#   s2recomp-disc [disc.iso]   (asks for the image when none is given)
set -u

APP="$(dirname "$(readlink -f "$0")")"
GAME="$APP/extracted"

say() {
    # From a desktop entry there is no terminal, so messages go to a dialog when one is available.
    if [ ! -t 1 ] && command -v kdialog >/dev/null 2>&1; then
        kdialog --title S2Recomp --msgbox "$1"
    elif [ ! -t 1 ] && command -v zenity >/dev/null 2>&1; then
        zenity --info --title S2Recomp --no-wrap --text "$1"
    else
        echo "$1"
    fi
}

iso="${1:-}"
if [ -z "$iso" ]; then
    if [ -t 0 ]; then
        read -r -p "Path to your The Sims 2 (PS2, Europe) disc image (.iso): " iso
        iso="${iso/#\~/$HOME}"
    elif command -v kdialog >/dev/null 2>&1; then
        iso="$(kdialog --title "Choose your The Sims 2 (PS2, Europe) disc image" --getopenfilename "$HOME" "*.iso *.ISO|Disc images")"
    elif command -v zenity >/dev/null 2>&1; then
        iso="$(zenity --file-selection --title "Choose your The Sims 2 (PS2, Europe) disc image" --file-filter "Disc images | *.iso *.ISO")"
    fi
fi
if [ -z "$iso" ]; then
    say "No disc image chosen. S2Recomp needs your own copy of The Sims 2 for PlayStation 2 (PAL / Europe, SLES-53718)."
    exit 1
fi

if [ -t 1 ]; then
    "$APP/s2r_setup" "$iso" "$GAME" | grep -v '^PROGRESS'
    exit "${PIPESTATUS[0]}"
fi
if command -v zenity >/dev/null 2>&1; then
    # zenity reads the percentages; the error text is kept for the message.
    out="$( { "$APP/s2r_setup" "$iso" "$GAME" 2>&1 1>&3 | cat; } 3> >(sed -un 's/^PROGRESS //p' |
        zenity --progress --auto-close --no-cancel --title S2Recomp --text "Copying the game files..."))"
else
    out="$("$APP/s2r_setup" "$iso" "$GAME" 2>&1 >/dev/null)"
fi
if [ ! -f "$GAME/SLES_537.18" ] || [ -n "$out" ]; then
    say "The game files could not be copied:

$out"
    exit 1
fi
say "The game files are ready. Start S2Recomp from the launcher."
