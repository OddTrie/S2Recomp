#pragma once

// Mod loader: folders under "mods" next to the executable (PS2X_MODS=<dir> picks another folder,
// "0" turns mods off). Each mod is a folder:
//
//   mods/<ModFolder>/mod.ini      optional: name, version, author, description, enabled, priority
//   mods/<ModFolder>/files/...    replaces (or adds) disc files; files/DATA/FONTS.ARC shadows
//                                 cdrom0:\DATA\FONTS.ARC. Matching ignores case.
//   mods/<ModFolder>/textures/    texture-pack files (<hash>_<w>x<h>_<psm>.bin, see gs_texture_replace.cpp)
//   mods/<ModFolder>/mod.dll      code mod (or "code = <file>" in mod.ini): see runtime/s2r_mod_api.h
//
// Mods load by priority (low to high, then folder name); when two mods ship the same file, the one
// loaded later wins. Folders whose name starts with '_' or '.' are skipped.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/s2r_mod_api.h"

namespace ps2x::mods
{
    struct ModInfo
    {
        std::string folder;
        std::string name;
        std::string version;
        std::string author;
        std::string description;
        std::filesystem::path dir;
        bool enabled = true;
        int priority = 0;
        size_t fileCount = 0;    // files under files/
        size_t textureCount = 0; // .bin files under textures/
        size_t overridesUsed = 0; // files the game has opened from this mod
        std::string code;         // code mod DLL file name, empty when the mod has none
        std::string codeStatus;   // "running", "not loaded: ..." (code mods only)
    };

    // Every mod found, enabled or not, in load order.
    std::vector<ModInfo> mods();
    std::filesystem::path modsRoot();

    // relativePath is a disc path without the device ("DATA/FONTS.ARC", '\\' or '/', ";1" allowed).
    // Returns true with the mod's host file when an enabled mod replaces it.
    bool overrideCdPath(std::string_view relativePath, std::filesystem::path &hostPath);

    // textures/ folders of enabled mods, in load order.
    std::vector<std::filesystem::path> textureDirs();

    // Code mods (s2r_mod_api.h), all on the host thread: started before the game thread, once per
    // frame shown, inside the debug UI's ImGui frame (ps2_debug_panel.cpp fills the S2RUi), at exit.
    void startCodeMods(uint8_t *rdram);
    void frameCodeMods();
    bool codeModsHaveUi();
    void drawCodeModsMenu(const S2RUi &ui); // the Mods menu (F2): each mod's S2R_ModMenu
    void drawCodeModsHud(const S2RUi &ui);  // every frame: S2R_ModHud
    void shutdownCodeMods();

    // Short on-screen messages from api->notify, newest last, with their age in seconds.
    struct Notice
    {
        std::string text;
        double age = 0.0;
    };
    std::vector<Notice> notices();

    // The host UI (ImGui) is using the keyboard or mouse: the pad ignores them meanwhile.
    void setUiCapture(bool keyboard, bool mouse);
    bool uiCapturesKeyboard();
    bool uiCapturesMouse();
}
