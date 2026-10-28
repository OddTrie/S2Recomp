S2Recomp mods
=============

Each mod is a folder here. Turn mods on and off in the launcher (Mods tab) or with MODS.bat, or set
"enabled = true/false" in the mod's mod.ini. Changes take effect the next time the game starts.

In game:  F2 = Mods menu (settings of the code mods that are on)   F1 = debug panel (Mods tab lists every mod)


Mods that come with S2Recomp
----------------------------

  Save slots are built into the game: it asks which slot to use each time you load or save.
  Saves made with the old MultipleSaves mod are copied into the new slots on first start.


  HelloMod        (off)  The example to copy when you start a code mod: text over the game, a Mods menu
                         section, a hotkey (F6) and a setting saved in mod.ini.
  PerformanceHUD  (off)  Game and window frames per second in a corner (F8).
  MemoryTools     (off)  Search the game's memory for a value (money, needs...) and set or freeze it:
                         how you find the addresses a gameplay mod changes.

Their source is in each folder's src/ folder.


Kinds of mods
-------------

A mod folder can hold any of these, in any mix:

  mods/<MyMod>/mod.ini      name, version, author, description, enabled, priority, code (see below)
  mods/<MyMod>/files/...    files that replace (or add to) the game disc, laid out like the disc.
                            Example: files/DATA/FONTS.ARC replaces DATA/FONTS.ARC.
                            Upper/lower case does not matter.
  mods/<MyMod>/textures/    texture replacements: <hash>_<w>x<h>_<psm>.bin files, as written by
                            the texture dump (set PS2X_TEX_DUMP=<folder> before starting the game).
  mods/<MyMod>/mod.dll      a code mod: C or C++ that runs inside the game (see "Code mods").

mod.ini:

  name = My Mod
  version = 1.0
  author = You
  description = What it changes
  enabled = true
  priority = 0
  code = mod.dll            (only for code mods; mod.dll is the default)

"enabled = false" keeps a mod installed but switched off. A mod with a higher priority loads later
and wins when two mods replace the same file. Comments go on their own line, starting with ; or #.
A code mod can keep its own settings in mod.ini too (key = value lines).

Folders whose name starts with "_" or "." are ignored (_example shows the layout, _sdk is the code
mod kit). The console and run_log.txt list every mod loaded ("[modloader] ...") and every replaced
file the game opens. PS2X_MODS=<folder> loads mods from another folder; PS2X_MODS=0 turns mods off.


Code mods
---------

A code mod is a DLL the game loads at start-up. It can draw text and windows over the game, add a
section to the Mods menu (F2), read and write the game's memory, react to keys, keep settings in its
mod.ini and swap memory cards. Everything it can use is in _sdk/s2r_mod_api.h, explained there.

Make your first one:

  1. Install Visual Studio Community (free) or the Build Tools for Visual Studio, with
     "Desktop development with C++".
  2. Copy the HelloMod folder to a new folder here, e.g. MyMod. Change name = in its mod.ini and set
     enabled = true.
  3. Change src/hello_mod.c (rename it if you like; every .c and .cpp file in src/ is built).
  4. Close the game, then build:   _sdk\build_mod.bat MyMod     (or drag the MyMod folder onto
     build_mod.bat). _sdk\build_mod.bat all builds every mod here.
  5. Start the game. The console shows "[modloader] My Mod: code mod mod.dll running", and lines your
     mod logs as "[mod:My Mod] ...". F2 shows its menu section.

The functions a mod DLL exports (only S2R_ModInit is required):

  int  S2R_ModInit(const S2RModApi *api)  start-up, before the game runs. Return 0 to stay loaded.
  void S2R_ModFrame(void)                 every frame shown
  void S2R_ModMenu(const S2RUi *ui)       the mod's section of the Mods menu (F2)
  void S2R_ModHud(const S2RUi *ui)        every frame, to draw over the game
  void S2R_ModShutdown(void)              when the game closes

Tips:
  * Finding values: turn on MemoryTools, search for the number you see in game, change it in game,
    search again. Once you have the address, read or write it in your mod with api->read / api->write.
    PS2 numbers are little-endian, like the PC's, so a 4-byte value reads straight into a uint32_t.
  * The game runs on its own threads; your callbacks run on the window's thread. A value can change
    between two reads, and the game may overwrite what you write: write it every frame to keep it.
  * Free keys for hotkeys: F5-F9 and F12 (the game uses WASD, IJKL, the arrows, Space, Backspace,
    R, F, Q, E, Z, C, Shift, Ctrl, Enter, Esc and Tab). Two mods on the same key both react.
  * A crash in a code mod closes the game. If the game stops starting after you add a mod, turn it
    off (launcher or MODS.bat, or enabled = false in its mod.ini).
  * Share a mod as its folder: mod.ini, mod.dll and (please) src/. Only install code mods from people
    you trust: a mod.dll can do anything a program on your PC can.
