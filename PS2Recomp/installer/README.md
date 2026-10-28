# Installers

Release packages for Windows and Linux. They contain **no game data and no game code**: the player's
PC builds the game from their own disc. Both packaging scripts refuse to build if a runner, a disc
file, generated code (`sub_*.cpp`, `vu1p_*.cpp`, `ps2_recompiled_*.h`), a VU1 recording or a profile
other than the VU1 programs' PGO profile (Windows only; counts per function, no code) ends up in the
package.

What a package holds: the launcher (`S2RecompLauncher`, `ps2xRuntime/launcher/`), the recompiler
(`ps2_recomp`, built here), the runtime's source without generated code, the libraries it uses
(raylib, Dear ImGui, rlImGui, SDL2: no git needed), FFmpeg, `sims2_config.toml`, `vu1_locations.toml`
and the scripts.

On the player's PC:

1. `s2r_setup` checks the disc image (`SLES_537.18` must be the exact build) and copies it into `extracted/`.
2. The build tools are made available: on Windows Microsoft's C++ tools with clang-cl (an existing
   Visual Studio 2022 or newer with them; one without gets them added; none: Setup downloads Visual
   Studio Build Tools 2022), each only after the player says yes. On Linux the distro's packages, or
   a podman/docker container on SteamOS, image-based systems and old compilers.
3. `build_game` runs the recompiler on the player's `SLES_537.18` and compiles the game, Fast or in
   the background (the player chooses). About 30 minutes on a fast PC, hours on a Steam Deck. A
   rebuild only compiles what changed.
   Before the game, `ps2xVuGen` writes the developer's compiled VU1 programs (`vu1gen/`) from the
   player's `SLES_537.18`: `vu1_locations.toml` holds only where each program's code lies in it (file
   offsets, start pcs, signatures), and the generator checks every signature. A different game
   version leaves the VU1 interpreter running everything. On Windows the VU1 sources build with
   clang-cl and the shipped `vu1.profdata`, like the developer build.

The recompiler must reproduce the developer build exactly: `r5900_decoder.cpp` takes the VU0 macro
dest mask from bits 21-24, and `sims2_config.toml` lists the jump table of `sub_002AF0F8`. With both,
the regenerated `src/runner` matches the shipped one (13224 of 13225 files byte-identical apart from
line endings; `sub_002AF0F8` only orders its cases by address). `ps2xVuGen` reproduces the developer's
`vu1gen/` byte for byte. After the developer regenerates `vu1gen/`, `ps2xRuntime/tools/vu1_locations.py`
rewrites `vu1_locations.toml` (next to `sims2_config.toml`).

| File | What it is |
| --- | --- |
| `s2r_setup.cpp` | Disc image (ISO 9660) or disc folder → `extracted/` copier and disc check, both platforms |
| `vu1_registry_empty.cpp` | The VU1 registry `--vu-gen` writes for no programs; the starting `vu1gen/` |
| `../../vu1_locations.toml` | Where the compiled VU1 programs' code lies in `SLES_537.18`, for `ps2xVuGen` |
| `windows/S2Recomp.iss` | Inno Setup script for the setup .exe: disc page, Visual Studio check/install, build page |
| `windows/build_game.ps1` | Builds the game on the player's PC (also "Build game" in the Start menu and the launcher) |
| `windows/s2r_disc.cpp` | `S2Recomp Disc Setup.exe`, the "Change game disc" shortcut |
| `windows/package_windows.ps1` | Builds the Windows setup .exe |
| `linux/build_linux.sh` | Builds FFmpeg 7.1 (MPEG-2 decoder only), the launcher, the recompiler and s2r_setup |
| `linux/package_linux.sh` | Packs that build into the release .tar.gz |
| `linux/install.sh` | The player's installer: per-user, distro-aware, then builds the game |
| `linux/build_game.sh` | Builds the game on the player's PC, natively or in a container |
| `linux/s2r-distro.sh` | Distro detection shared by the two scripts above |
| `linux/s2recomp-disc.sh`, `linux/uninstall.sh` | Disc setup and uninstaller that get installed |

Output goes to `installer/build/dist/`.

## Windows

Needs Visual Studio (C++ workload) and Inno Setup 6 (`winget install JRSoftware.InnoSetup`).
Build with `BUILD_AND_RUN.bat` first (it builds S2RecompLauncher.exe and ps2_recomp.exe too), then:

```
powershell -ExecutionPolicy Bypass -File PS2Recomp\installer\windows\package_windows.ps1 -Version 0.1.0
```

Unattended install: `S2Recomp-0.1.0-windows-x64-setup.exe /VERYSILENT /ISO="C:\path\to\disc.iso" /BUILD=fast`
(silent installs never install Visual Studio).

## Linux

Build on Ubuntu 22.04 (its older glibc lets the result run on current distros, SteamOS included);
on Windows that is `wsl --install -d Ubuntu-22.04`. Install the packages listed at the top of
`build_linux.sh`, then:

```
PS2Recomp/installer/linux/build_linux.sh ~/s2r-build
PS2Recomp/installer/linux/package_linux.sh 0.1.0 ~/s2r-build
```

The build tree must be on the Linux file system (not `/mnt/c`), or it is several times slower.
