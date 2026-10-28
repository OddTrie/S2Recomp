#!/usr/bin/env bash
# Removes S2Recomp for the current user. The game files copied from the disc are removed; the
# memory cards (your saved games, extracted/mc0 and extracted/mc1) are kept unless you say otherwise or pass
# --all. Packages the installer added with your package manager are left installed.
set -eu

APP="$(dirname "$(readlink -f "$0")")"
APPS_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
ALL=0
[ "${1:-}" = "--all" ] && ALL=1

[ -f "$APP/S2RecompLauncher" ] || { echo "This does not look like an S2Recomp install: $APP"; exit 1; }

rm -f "$HOME/.local/bin/s2recomp" "$HOME/.local/bin/s2recomp-disc" "$HOME/.local/bin/s2recomp-build" "$APPS_DIR/s2recomp.desktop"
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$APPS_DIR" >/dev/null 2>&1 || true

if [ "$ALL" = 0 ] && [ -t 0 ] && { [ -d "$APP/extracted/mc0" ] || [ -d "$APP/extracted/mc1" ]; }; then
    read -r -p "Also delete your saved games (memory cards in $APP/extracted)? [y/N] " a
    case "$a" in [yY]*) ALL=1 ;; esac
fi

if [ "$ALL" = 1 ]; then
    rm -rf "$APP"
    echo "S2Recomp and your saved games are removed."
else
    # Keep only the memory cards.
    find "$APP" -mindepth 1 -maxdepth 1 ! -name extracted -exec rm -rf {} +
    [ ! -d "$APP/extracted" ] || find "$APP/extracted" -mindepth 1 -maxdepth 1 ! -name mc0 ! -name mc1 -exec rm -rf {} +
    rmdir "$APP/extracted" "$APP" 2>/dev/null || true
    [ -d "$APP" ] && echo "S2Recomp is removed. Your saved games are kept in $APP/extracted." || echo "S2Recomp is removed."
fi
echo "If you added it to Steam, remove that shortcut in Steam yourself."
command -v podman >/dev/null 2>&1 && podman image exists localhost/s2recomp-build:1 2>/dev/null &&
    echo "The build container image is still there; podman rmi localhost/s2recomp-build:1 removes it." || true
