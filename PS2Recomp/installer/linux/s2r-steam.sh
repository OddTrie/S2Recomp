#!/usr/bin/env bash
# Adds S2Recomp to Steam as a non-Steam game, so it starts from the library, Big Picture and Game Mode.
#
#   s2r-steam.sh [install dir]      (default: the folder this script is in)
#
# Exit codes: 0 added (or Steam is starting and adds it), 2 it has to be added by hand (the last line
# says how), 1 Steam was not found.
set -u

PREFIX="$(cd "${1:-$(dirname "$(readlink -f "$0")")}" && pwd)"
EXE="$PREFIX/S2RecompLauncher"
DESKTOP="${XDG_DATA_HOME:-$HOME/.local/share}/applications/s2recomp.desktop"

manual() { echo "In Steam: Games > Add a Non-Steam Game to My Library > Browse > $EXE"; exit 2; }
urlencode() {
    local LC_ALL=C s="$1" out="" c i
    for ((i = 0; i < ${#s}; i++)); do
        c="${s:i:1}"
        case "$c" in [a-zA-Z0-9/._~-]) out+="$c" ;; *) printf -v c '%%%02X' "'$c"; out+="$c" ;; esac
    done
    printf '%s' "$out"
}

# Already there (an update, or added before): adding again would make another library entry.
for vdf in "$HOME"/.steam/steam/userdata/*/config/shortcuts.vdf "$HOME"/.local/share/Steam/userdata/*/config/shortcuts.vdf \
           "$HOME"/.var/app/com.valvesoftware.Steam/.local/share/Steam/userdata/*/config/shortcuts.vdf; do
    [ -f "$vdf" ] && grep -aqF "$EXE" "$vdf" && { echo "S2Recomp is already in Steam."; exit 0; }
done

# SteamOS (Steam Deck, Desktop Mode with Steam running): its own tool.
if command -v steamos-add-to-steam >/dev/null 2>&1 && [ -f "$DESKTOP" ]; then
    steamos-add-to-steam "$DESKTOP" >/dev/null 2>&1 && { echo "Added S2Recomp to Steam."; exit 0; }
fi

if command -v steam >/dev/null 2>&1; then
    # Steam only takes an addnonsteamgame link while this file exists (it marks the request as local).
    f=/tmp/addnonsteamgamefile
    [ -e "$f" ] || ( umask 077; set -C; : >"$f" ) 2>/dev/null
    [ -f "$f" ] && [ ! -L "$f" ] && [ -O "$f" ] || manual
    url="steam://addnonsteamgame/$(urlencode "$EXE")"
    if pgrep -x steam >/dev/null 2>&1; then
        timeout 30 steam "$url" </dev/null >/dev/null 2>&1 && { echo "Added S2Recomp to Steam."; exit 0; }
        manual
    fi
    # Not running: started with the link, Steam adds the game once it has opened.
    setsid nohup steam "$url" </dev/null >/dev/null 2>&1 &
    echo "Steam is starting; it adds S2Recomp to your library once it is open."
    exit 0
fi

if command -v flatpak >/dev/null 2>&1 && [ -d "$HOME/.var/app/com.valvesoftware.Steam" ]; then
    # Flatpak Steam cannot take the link from outside its sandbox, and needs to be allowed to see the folder.
    flatpak override --user --filesystem="$PREFIX" com.valvesoftware.Steam >/dev/null 2>&1 &&
        echo "Flatpak Steam may now open $PREFIX."
    manual
fi

echo "Steam was not found. Install or open Steam, then try again."
exit 1
