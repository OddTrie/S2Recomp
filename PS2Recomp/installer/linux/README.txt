S2Recomp for Linux
==================

S2Recomp builds The Sims 2 (PS2) as a native PC game on your own computer, from your own disc.
This package holds the launcher, the recompiler and the source the game is built from. It contains
no game files and no game code.

You need your own copy of The Sims 2 for PlayStation 2, PAL / Europe release (SLES-53718),
as a disc image (.iso), and about 7 GB of free disk space.

Install
-------
Double-click S2Recomp-<version>-linux-x86_64.run (or run: sh S2Recomp-<version>-linux-x86_64.run).
The setup window opens: choose your disc image, how to build, and whether to add S2Recomp to Steam,
then press Install. (If your file manager opens it as text, right-click > Properties > Permissions >
"Allow executing as program" first, or run it with sh.)

The setup window needs Python 3 with Tk (python3-tk), which most desktops have. Without it, or with
--cli, the terminal installer runs instead:

    sh S2Recomp-<version>-linux-x86_64.run --cli --iso "/path/to/The Sims 2.iso"

It installs for your user only (into ~/Games/S2Recomp) and then:

  1. copies the game files from your disc image;
  2. gets the build tools: on Debian/Ubuntu, Fedora, Arch and openSUSE it installs them with your
     package manager (it asks first). On SteamOS, Bazzite and other read-only or image-based
     systems, and where the compiler is too old, it builds in a podman (or docker) container
     instead and changes nothing on the system (the container downloads about 1 GB once);
  3. builds the game: you choose Fast (whole PC) or in the background (PC stays usable). This
     takes about 30 minutes to a few hours, depending on the PC.

Steam Deck / SteamOS
--------------------
Switch to Desktop Mode, double-click the .run and keep "Add to Steam" ticked in the setup window,
then go back to Game Mode once it has finished: it is in
your library under Non-Steam, and the launcher fills the screen and works with the Deck's controls.
Nothing on the read-only system is changed, and the install survives SteamOS updates. Building
on the Deck takes a few hours; In the background lets you keep using it.

Play
----
Start S2Recomp from your application menu (Games), from Steam, or run: s2recomp
If the game is not built yet, the launcher's BUILD GAME button builds it.
Change the disc image: s2recomp-disc        Build again: s2recomp-build
Mods go in ~/Games/S2Recomp/mods; the launcher's Mods tab turns them on and off.

Update
------
Download the new S2Recomp-<version>-linux-x86_64.run and run it the same way. Keep the same install
folder; the button reads Update and you don't need to choose the disc image again. It replaces the
launcher and the game's source, keeps your saves, settings and mods, and rebuilds only what changed.
Terminal: sh S2Recomp-<version>-linux-x86_64.run --cli
On the Steam Deck, do this in Desktop Mode; the Steam shortcut keeps working.

Uninstall
---------
    ~/Games/S2Recomp/uninstall.sh

Your saved games (memory cards) are kept unless you say otherwise.

Options (terminal installer: sh S2Recomp-<version>-linux-x86_64.run --cli [options])
-------
--build fast|background|none   build now, in the background, or later
--prefix DIR   install somewhere else
--yes          answer yes to every question
--no-deps      never install packages for the launcher
--steam / --no-steam   add it to Steam without asking / do not offer to
--check        only show what would be done on this system
