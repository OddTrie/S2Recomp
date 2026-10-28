# Sourced by install.sh and build_game.sh: which kind of Linux this is, from /etc/os-release.
# Sets FAMILY to steamos, atomic (Bazzite, Fedora Silverblue/Kinoite and other image-based systems),
# nixos, debian, fedora, arch, suse or unknown, and PRETTY_NAME. S2R_OS_RELEASE overrides the file (testing).
# shellcheck shell=bash

ID="" ID_LIKE="" VARIANT_ID="" PRETTY_NAME="Linux"
OS_RELEASE="${S2R_OS_RELEASE:-/etc/os-release}"
if [ -r "$OS_RELEASE" ]; then
    # shellcheck disable=SC1090
    . "$OS_RELEASE"
fi
_ids=" ${ID:-} ${ID_LIKE:-} "
FAMILY=unknown
case "$_ids" in
    *" steamos "*) FAMILY=steamos ;;
    *" nixos "*) FAMILY=nixos ;;
esac
if [ "$FAMILY" = unknown ]; then
    if { [ -z "${S2R_OS_RELEASE:-}" ] && [ -e /run/ostree-booted ]; } || [ "${ID:-}" = bazzite ] ||
        [ "${VARIANT_ID:-}" = silverblue ] || [ "${VARIANT_ID:-}" = kinoite ]; then
        FAMILY=atomic
    else
        case "$_ids" in
            *" debian "*|*" ubuntu "*) FAMILY=debian ;;
            *" fedora "*|*" rhel "*|*" centos "*) FAMILY=fedora ;;
            *" arch "*) FAMILY=arch ;;
            *" suse "*|*" opensuse "*|*" opensuse-tumbleweed "*) FAMILY=suse ;;
        esac
    fi
fi
unset _ids
export FAMILY PRETTY_NAME
