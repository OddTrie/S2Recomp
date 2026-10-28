#pragma once
// The Lobbies tab's EOS side: logs in with the same device ID as the game (ps2_netplay.cpp) and lists
// the lobbies hosts advertise. Every EOS call runs on its own thread; the UI only reads snapshots.
// Kept apart from launcher_main.cpp because EOS pulls in windows.h, which clashes with raylib.h.
#include <filesystem>
#include <string>
#include <vector>

namespace lobbies
{
    struct Entry
    {
        std::string owner; // host's product user ID: the long join code
        std::string code;  // host's short join code (8 characters, no dash); empty from older builds
        std::string name;
        int delay = 3;
        int slots = 0;
        bool mine = false; // hosted from this PC
    };

    bool available();                          // built with the EOS SDK
    void start(const std::filesystem::path &ini); // logs in once (no-op after the first call), then refreshes
    void refresh();
    bool busy();
    std::string status();
    bool failed();
    std::vector<Entry> list();
    void shutdown();
}
