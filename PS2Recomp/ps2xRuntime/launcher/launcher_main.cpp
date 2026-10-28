// S2Recomp launcher: picks the settings the runtime reads from PS2X_* environment variables, manages
// the mods folder (mods/<mod>/mod.ini), and starts ps2EntryRunner.exe with them. Settings are kept in
// launcher.ini next to the executable; the game's console output goes to run_log.txt like RUN.bat's.

#include "launcher_lobbies.h"
#include "launcher_platform.h"

#include "raylib.h"
#include "imgui.h"
#include "rlImGui.h"
#include "imgui_impl_raylib.h"
#include "extras/IconsFontAwesome6.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <regex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

// The UI fonts, compiled in by CMake (launcher_fonts.cpp in the build folder).
extern unsigned char kFont_Roboto_Medium[];
extern const unsigned int kFont_Roboto_Medium_size;
extern unsigned char kFont_DroidSans[];
extern const unsigned int kFont_DroidSans_size;
#if PS2X_APP_ICON_PNG
// The window icon, also compiled in (app_icon_png.cpp). Windows takes it from the exe (assets/s2recomp.rc).
extern const unsigned char kAppIconPng[];
extern const unsigned int kAppIconPng_size;
#endif

namespace
{
    // ---------------------------------------------------------------------------------------------
    // Small helpers
    // ---------------------------------------------------------------------------------------------
    std::string trim(const std::string &s)
    {
        size_t a = 0, b = s.size();
        while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
            ++a;
        while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
            --b;
        return s.substr(a, b - a);
    }

    std::string lower(std::string s)
    {
        for (char &c : s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    bool truthy(const std::string &v)
    {
        const std::string l = lower(trim(v));
        return !(l == "0" || l == "false" || l == "no" || l == "off");
    }

    // key = value lines; ; and # start comment lines (the mod.ini format).
    std::map<std::string, std::string> readIni(const fs::path &path)
    {
        std::map<std::string, std::string> out;
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line))
        {
            const std::string t = trim(line);
            if (t.empty() || t[0] == ';' || t[0] == '#')
                continue;
            const size_t eq = t.find('=');
            if (eq == std::string::npos)
                continue;
            out[lower(trim(t.substr(0, eq)))] = trim(t.substr(eq + 1));
        }
        return out;
    }

    // Changes one key of an ini file, keeping every other line (comments included) as it was.
    void setIniKey(const fs::path &path, const std::string &key, const std::string &value)
    {
        std::vector<std::string> lines;
        {
            std::ifstream in(path);
            std::string line;
            while (std::getline(in, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                lines.push_back(line);
            }
        }
        bool found = false;
        for (std::string &line : lines)
        {
            const std::string t = trim(line);
            const size_t eq = t.find('=');
            if (!found && eq != std::string::npos && t[0] != ';' && t[0] != '#' && lower(trim(t.substr(0, eq))) == key)
            {
                line = key + " = " + value;
                found = true;
            }
        }
        if (!found)
            lines.push_back(key + " = " + value);
        std::ofstream out(path, std::ios::trunc);
        for (const std::string &line : lines)
            out << line << "\n";
    }

    // ---------------------------------------------------------------------------------------------
    // Settings
    // ---------------------------------------------------------------------------------------------
    const char *const kRenderers[] = {"Automatic (GPU when available)", "GPU", "CPU (exact reference, needs many cores)"};
    const char *const kRendererKeys[] = {"auto", "gpu", "cpu"};
    const char *const kAspects[] = {"4:3 (as on a TV)", "16:9", "Stretch to window", "Native (square pixels)"};
    const char *const kAspectKeys[] = {"4:3", "16:9", "stretch", "native"};
    const char *const kFilters[] = {"Sharp (crisp, even pixels)", "Smooth (bilinear)", "Nearest (blocky)"};
    const char *const kFilterKeys[] = {"sharp", "linear", "nearest"};
    const char *const kInterlace[] = {"Sharp", "Soft (TV blur)"};
    const char *const kInterlaceKeys[] = {"sharp", "tv"};
    const char *const kGlyphs[] = {"Follow the last device used", "Keyboard keys", "Xbox buttons", "PlayStation buttons"};
    const char *const kGlyphKeys[] = {"auto", "keyboard", "xbox", "playstation"};

    struct Settings
    {
        std::string elf;
        int renderer = 0;
        int scale = 3; // 0 = auto
        bool fullscreen = false;
        bool windowAuto = true;
        int windowW = 1280, windowH = 960;
        int aspect = 0;
        int filter = 0;
        bool vsync = true;
        bool framegen = true;
        int interlace = 0;
        bool hdTextures = true;
        bool branding = true;
        int glyphs = 0;
        bool audio = true;
        int volume = 50;
        bool rumble = true;
        bool steamInput = false;
        bool mods = true;
        bool fps60 = false;
        bool showConsole = false;
        int netMode = 0; // kNetModes
        std::string netName;
        std::string netCode;
        int netDelay = 3;
        bool deckScale = false; // the Steam Deck default (Auto) was applied once
    };

    const char *const kNetModes[] = {"Off", "Host  (you are player 1)", "Join  (you are player 2)"};
    const char *const kNetModeKeys[] = {"off", "host", "join"};

    int keyIndex(const std::string &v, const char *const *keys, int n, int fallback)
    {
        for (int i = 0; i < n; ++i)
            if (v == keys[i])
                return i;
        return fallback;
    }

    fs::path settingsPath() { return launcher::exeDir() / "launcher.ini"; }

    Settings loadSettings()
    {
        Settings s;
        const auto ini = readIni(settingsPath());
        auto get = [&](const char *k) -> const std::string * { auto it = ini.find(k); return it == ini.end() ? nullptr : &it->second; };
        auto getBool = [&](const char *k, bool &out) { if (auto *v = get(k)) out = truthy(*v); };
        auto getInt = [&](const char *k, int &out, int lo, int hi) { if (auto *v = get(k)) out = std::clamp(std::atoi(v->c_str()), lo, hi); };
        if (auto *v = get("elf")) s.elf = *v;
        if (auto *v = get("renderer")) s.renderer = keyIndex(*v, kRendererKeys, 3, 0);
        getInt("scale", s.scale, 0, 4);
        getBool("fullscreen", s.fullscreen);
        if (auto *v = get("window"))
        {
            int w = 0, h = 0;
            s.windowAuto = std::sscanf(v->c_str(), "%dx%d", &w, &h) != 2 || w < 320 || h < 224;
            if (!s.windowAuto)
                s.windowW = w, s.windowH = h;
        }
        if (auto *v = get("aspect")) s.aspect = keyIndex(*v, kAspectKeys, 4, 0);
        if (auto *v = get("filter")) s.filter = keyIndex(*v, kFilterKeys, 3, 0);
        getBool("vsync", s.vsync);
        getBool("framegen", s.framegen);
        if (auto *v = get("interlace")) s.interlace = keyIndex(*v, kInterlaceKeys, 2, 0);
        getBool("hd_textures", s.hdTextures);
        getBool("branding", s.branding);
        if (auto *v = get("glyphs")) s.glyphs = keyIndex(*v, kGlyphKeys, 4, 0);
        getBool("audio", s.audio);
        getInt("volume", s.volume, 0, 100);
        getBool("rumble", s.rumble);
        getBool("steam_input", s.steamInput);
        getBool("mods", s.mods);
        getBool("fps60", s.fps60);
        getBool("show_console", s.showConsole);
        if (auto *v = get("net_mode")) s.netMode = keyIndex(*v, kNetModeKeys, 3, 0);
        if (auto *v = get("net_name")) s.netName = *v;
        if (auto *v = get("net_code")) s.netCode = *v;
        getInt("net_delay", s.netDelay, 1, 10);
        getBool("deck_scale", s.deckScale);
        return s;
    }

    void saveSettings(const Settings &s)
    {
        std::ofstream out(settingsPath(), std::ios::trunc);
        auto b = [](bool v) { return v ? "true" : "false"; };
        out << "; S2Recomp launcher settings (written by S2RecompLauncher.exe)\n";
        out << "elf = " << s.elf << "\n";
        out << "renderer = " << kRendererKeys[s.renderer] << "\n";
        out << "scale = " << s.scale << "\n";
        out << "fullscreen = " << b(s.fullscreen) << "\n";
        out << "window = " << (s.windowAuto ? std::string("auto") : std::to_string(s.windowW) + "x" + std::to_string(s.windowH)) << "\n";
        out << "aspect = " << kAspectKeys[s.aspect] << "\n";
        out << "filter = " << kFilterKeys[s.filter] << "\n";
        out << "vsync = " << b(s.vsync) << "\n";
        out << "framegen = " << b(s.framegen) << "\n";
        out << "interlace = " << kInterlaceKeys[s.interlace] << "\n";
        out << "hd_textures = " << b(s.hdTextures) << "\n";
        out << "branding = " << b(s.branding) << "\n";
        out << "glyphs = " << kGlyphKeys[s.glyphs] << "\n";
        out << "audio = " << b(s.audio) << "\n";
        out << "volume = " << s.volume << "\n";
        out << "rumble = " << b(s.rumble) << "\n";
        out << "steam_input = " << b(s.steamInput) << "\n";
        out << "mods = " << b(s.mods) << "\n";
        out << "fps60 = " << b(s.fps60) << "\n";
        out << "show_console = " << b(s.showConsole) << "\n";
        out << "net_mode = " << kNetModeKeys[s.netMode] << "\n";
        out << "net_name = " << s.netName << "\n";
        out << "net_code = " << s.netCode << "\n";
        out << "net_delay = " << s.netDelay << "\n";
        out << "deck_scale = " << b(s.deckScale) << "\n";
    }

    // The PS2X_* variables the runtime reads (see the getenv calls in ps2xRuntime/src). Under gamescope (Steam
    // Deck Game Mode) every window fills the screen anyway, so the game always starts fullscreen there.
    std::vector<launcher::EnvVar> gameEnv(const Settings &s, bool gamescope)
    {
        std::vector<launcher::EnvVar> env;
        auto set = [&](const char *name, std::string value) { env.push_back({name, std::move(value), false}); };
        auto unset = [&](const char *name) { env.push_back({name, {}, true}); };
        auto b = [](bool v) { return std::string(v ? "1" : "0"); };
        if (s.renderer == 0) unset("PS2X_GS_GPU");
        else set("PS2X_GS_GPU", s.renderer == 1 ? "1" : "0");
        set("PS2X_GS_SCALE", s.scale == 0 ? std::string("auto") : std::to_string(s.scale));
        if (s.windowAuto || gamescope) unset("PS2X_WINDOW");
        else set("PS2X_WINDOW", std::to_string(s.windowW) + "x" + std::to_string(s.windowH));
        set("PS2X_FULLSCREEN", b(s.fullscreen || gamescope));
        set("PS2X_ASPECT", kAspectKeys[s.aspect]);
        set("PS2X_FILTER", kFilterKeys[s.filter]);
        set("PS2X_PRESENT_VSYNC", b(s.vsync));
        set("PS2X_FRAMEGEN", b(s.framegen));
        if (s.interlace == 0) unset("PS2X_GS_FLICKER");
        else set("PS2X_GS_FLICKER", "tv");
        set("PS2X_TEXPACK_HD", b(s.hdTextures));
        set("PS2X_BRANDING", b(s.branding));
        if (s.glyphs == 0) unset("PS2X_GLYPHS");
        else set("PS2X_GLYPHS", kGlyphKeys[s.glyphs]);
        set("PS2X_AUDIO", b(s.audio));
        set("PS2X_AUDIO_VOLUME", std::to_string(s.volume));
        set("PS2X_RUMBLE", b(s.rumble));
        set("PS2X_STEAM_INPUT", b(s.steamInput));
        if (s.mods) unset("PS2X_MODS");
        else set("PS2X_MODS", "0");
        set("PS2X_60FPS", b(s.fps60));
        if (s.netMode == 0)
        {
            unset("PS2X_NETPLAY");
            unset("PS2X_NETPLAY_CODE");
        }
        else
        {
            set("PS2X_NETPLAY", kNetModeKeys[s.netMode]);
            set("PS2X_NETPLAY_NAME", s.netName.empty() ? (s.netMode == 1 ? "Host" : "Guest") : s.netName);
            set("PS2X_NETPLAY_DELAY", std::to_string(s.netDelay));
            if (s.netMode == 2) set("PS2X_NETPLAY_CODE", s.netCode);
            else unset("PS2X_NETPLAY_CODE");
        }
        return env;
    }

    // ---------------------------------------------------------------------------------------------
    // Game files
    // ---------------------------------------------------------------------------------------------
    // A PS2 boot executable name such as SLES_537.18 or SLUS_212.65.
    bool isBootElfName(const std::string &name)
    {
        static const std::regex re(R"(^S[CL][A-Z]{2}_\d{3}\.\d{2}$)", std::regex::icase);
        return std::regex_match(name, re);
    }

    // A real PS2 executable, not the disc image or another file picked by mistake.
    bool isElfFile(const fs::path &p)
    {
        std::ifstream in(p, std::ios::binary);
        char magic[4] = {};
        return in.read(magic, 4) && std::memcmp(magic, "" "ELF", 4) == 0;
    }

    fs::path findElfIn(const fs::path &dir)
    {
        std::error_code ec;
        if (!fs::is_directory(dir, ec))
            return {};
        for (const auto &e : fs::directory_iterator(dir, ec))
            if (e.is_regular_file(ec) && isBootElfName(e.path().filename().string()))
                return e.path();
        return {};
    }

    // The extracted disc next to the launcher, or in an "extracted" folder up to six levels above it
    // (the development layout: <root>/PS2Recomp/build_win/ps2xRuntime/Release).
    fs::path findElf()
    {
        fs::path dir = launcher::exeDir();
        for (int up = 0; up < 7 && !dir.empty(); ++up)
        {
            if (fs::path p = findElfIn(dir); !p.empty())
                return p;
            if (fs::path p = findElfIn(dir / "extracted"); !p.empty())
                return p;
            const fs::path parent = dir.parent_path();
            if (parent == dir)
                break;
            dir = parent;
        }
        return {};
    }

    // Where the game runs and run_log.txt goes: the folder holding "extracted" (as RUN.bat does), else the ELF's.
    fs::path gameRoot(const fs::path &elf)
    {
        const fs::path dir = elf.parent_path();
        if (lower(dir.filename().string()) == "extracted")
            return dir.parent_path();
        return dir;
    }

    // ---------------------------------------------------------------------------------------------
    // Mods
    // ---------------------------------------------------------------------------------------------
    struct Mod
    {
        fs::path dir;
        std::string folder, name, version, author, description;
        bool enabled = true;
        int priority = 0;
        int files = 0, textures = 0;
        bool code = false; // a code mod (mod.dll, or "code = <file>" in mod.ini)
    };

    int countFiles(const fs::path &dir)
    {
        std::error_code ec;
        if (!fs::is_directory(dir, ec))
            return 0;
        int n = 0;
        for (auto it = fs::recursive_directory_iterator(dir, ec); it != fs::recursive_directory_iterator() && n < 100000; it.increment(ec))
            if (it->is_regular_file(ec))
                ++n;
        return n;
    }

    fs::path modsRoot() { return launcher::exeDir() / "mods"; }

    std::vector<Mod> scanMods()
    {
        std::vector<Mod> mods;
        std::error_code ec;
        for (const auto &e : fs::directory_iterator(modsRoot(), ec))
        {
            const std::string folder = e.path().filename().string();
            if (!e.is_directory(ec) || folder.empty() || folder[0] == '_' || folder[0] == '.')
                continue;
            Mod m;
            m.dir = e.path();
            m.folder = m.name = folder;
            const auto ini = readIni(e.path() / "mod.ini");
            auto get = [&](const char *k) { auto it = ini.find(k); return it == ini.end() ? std::string() : it->second; };
            if (!get("name").empty()) m.name = get("name");
            m.version = get("version");
            m.author = get("author");
            m.description = get("description");
            if (ini.count("enabled")) m.enabled = truthy(get("enabled"));
            if (ini.count("priority")) m.priority = std::atoi(get("priority").c_str());
            m.files = countFiles(e.path() / "files");
            m.textures = countFiles(e.path() / "textures");
            const std::string code = get("code").empty() ? "mod.dll" : get("code");
            m.code = fs::is_regular_file(e.path() / fs::u8path(code), ec);
            mods.push_back(std::move(m));
        }
        std::sort(mods.begin(), mods.end(), [](const Mod &a, const Mod &b) { return lower(a.name) < lower(b.name); });
        return mods;
    }

    // ---------------------------------------------------------------------------------------------
    // Building the game on this PC (installer/*/build_game.*, installed next to the launcher)
    // ---------------------------------------------------------------------------------------------
#ifdef _WIN32
    const char *const kRunnerName = "ps2EntryRunner.exe";
    fs::path buildScript() { return launcher::exeDir() / "build_game.ps1"; }
#else
    const char *const kRunnerName = "ps2EntryRunner";
    fs::path buildScript() { return launcher::exeDir() / "build_game.sh"; }
#endif
    fs::path runnerPath() { return launcher::exeDir() / kRunnerName; }

    // An S2Recomp update installs a new build script but leaves the game as it was until it is rebuilt.
    bool gameOutdated()
    {
        std::error_code ec1, ec2;
        const auto game = fs::last_write_time(runnerPath(), ec1), script = fs::last_write_time(buildScript(), ec2);
        return !ec1 && !ec2 && game < script;
    }

    struct BuildState
    {
        launcher::GameProcess *proc = nullptr;
        std::string step;
        float progress = 0.0f;
        double lastRead = 0.0;
        fs::path log;
    };

    // The last STEP / PROGRESS lines the build script wrote, and its last other line (its error, on failure).
    void readBuildLog(BuildState &b, std::string *lastOther)
    {
        std::ifstream in(b.log);
        std::string line;
        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.rfind("STEP ", 0) == 0)
                b.step = line.substr(5);
            else if (line.rfind("PROGRESS ", 0) == 0)
                b.progress = std::clamp(std::atoi(line.c_str() + 9), 0, 100) / 100.0f;
            else if (lastOther && !trim(line).empty())
                *lastOther = trim(line);
        }
    }

    // ---------------------------------------------------------------------------------------------
    // UI
    // ---------------------------------------------------------------------------------------------
    struct Ui
    {
        Settings s;
        std::vector<Mod> mods;
        fs::path elf;
        launcher::GameProcess *game = nullptr;
        std::string status;
        bool statusError = false;
        bool lastRunFailed = false;
        ImFont *body = nullptr;
        ImFont *title = nullptr;
        float dpi = 1.0f;
        int monitorW = 1920, monitorH = 1080;

        // Steam Deck / TV play: `couch` is a Steam Deck or Big Picture (larger, fullscreen launcher); `gamescope` is
        // Game Mode, where Explorer and file dialogs are no use, so the buttons that open them are hidden.
        bool couch = false;
        bool gamescope = false;

        // Controller state for this frame (see feedGamepad).
        bool padConnected = false;
        bool padPlay = false;    // Start pressed
        int padTabStep = 0;      // LB / RB pressed
        float padScroll = 0.0f;  // right stick, -1..1
        bool focusPlay = true;   // put the controller cursor on Play (at start and after the game exits)
        int tab = 0;             // the open tab
        int tabRequest = -1;     // a tab to switch to this frame

        BuildState build;
        bool askBuild = false;   // open the "Fast or in the background?" popup this frame
    };

    bool building(const Ui &ui) { return ui.build.proc != nullptr; }

    void startBuild(Ui &ui, bool background)
    {
        std::vector<std::string> args;
#ifdef _WIN32
        const char *sysRoot = std::getenv("SystemRoot");
        const fs::path exe = fs::path(sysRoot ? sysRoot : "C:\\Windows") / "System32" / "WindowsPowerShell" / "v1.0" / "powershell.exe";
        args = {"-NoProfile", "-ExecutionPolicy", "Bypass", "-File", buildScript().string(), "-Pace", background ? "background" : "fast"};
#else
        const fs::path exe = buildScript();
        args = {"--pace", background ? "background" : "fast"};
#endif
        ui.build = BuildState();
        ui.build.log = launcher::exeDir() / "build_output.txt";
        ui.build.step = "Starting";
        std::string error;
        ui.build.proc = launcher::startTool(exe, args, launcher::exeDir(), ui.build.log, error);
        if (!ui.build.proc)
        {
            ui.status = error;
            ui.statusError = true;
            return;
        }
        ui.status.clear();
        ui.statusError = false;
    }

    // Called every frame while a build runs: progress from its output, and the result once it ends.
    void pollBuild(Ui &ui)
    {
        if (!building(ui))
            return;
        uint32_t code = 0;
        const bool running = launcher::gameRunning(ui.build.proc, code);
        const double now = GetTime();
        if (running && now - ui.build.lastRead < 0.3)
            return;
        ui.build.lastRead = now;
        std::string lastOther;
        readBuildLog(ui.build, &lastOther);
        if (running)
            return;
        launcher::closeGame(ui.build.proc);
        ui.build.proc = nullptr;
        if (code == 0)
        {
            ui.status = "The game is built. Press Play.";
            ui.statusError = false;
            ui.focusPlay = true;
            return;
        }
        ui.statusError = true;
        switch (code)
        {
#ifdef _WIN32
        case 10: ui.status = "Microsoft's C++ tools are missing. Run S2Recomp Setup again: it offers to install them."; break;
#else
        case 10:
        case 13: ui.status = lastOther.empty() ? "The build tools are missing. Run s2recomp-build in a terminal." : lastOther; break;
#endif
        case 11: ui.status = "The game files are not set up yet: use Change game disc first."; break;
        default: ui.status = "Building failed: " + (lastOther.empty() ? std::string("see build_log.txt") : lastOther); break;
        }
    }

    // "Fast or in the background?" for the game build.
    void buildPopup(Ui &ui, const char *id)
    {
        if (ui.askBuild)
        {
            ImGui::OpenPopup(id);
            ui.askBuild = false;
        }
        ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 30.0f, 0));
        if (!ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove))
            return;
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 29.0f);
        ImGui::TextUnformatted("S2Recomp builds the game on this PC from your own disc: the recompiler turns it into C++ "
                               "and the compiler builds it. That takes about 30 minutes to a few hours, and about 5 GB.");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (ImGui::Button("Fast (whole PC)"))
        {
            startBuild(ui, false);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("In the background"))
        {
            startBuild(ui, true);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    const ImVec4 kAccent = ImVec4(0.33f, 0.78f, 0.36f, 1.0f);      // plumbob green
    const ImVec4 kAccentHover = ImVec4(0.40f, 0.86f, 0.42f, 1.0f);
    const ImVec4 kAccentActive = ImVec4(0.26f, 0.66f, 0.29f, 1.0f);
    const ImVec4 kMuted = ImVec4(0.62f, 0.66f, 0.70f, 1.0f);
    const ImVec4 kWarn = ImVec4(0.98f, 0.72f, 0.30f, 1.0f);

    const char *const kDiscordUrl = "https://discord.com/invite/YwAx7DrrFY";

    // ---------------------------------------------------------------------------------------------
    // Updates: the latest GitHub release's installer, downloaded and started (it updates S2Recomp and
    // rebuilds the game). The installers write version.txt next to the launcher; without it (a
    // developer build) there is no update check.
    // ---------------------------------------------------------------------------------------------
    const char *const kReleaseApi = "https://api.github.com/repos/OddTrie/S2Recomp/releases/latest";
#ifdef _WIN32
    const char *const kInstallerSuffix = "-windows-x64-setup.exe";
#else
    const char *const kInstallerSuffix = "-linux-x86_64.run";
#endif

    enum class UpdatePhase { Off, Checking, Current, Available, Downloading, Downloaded, Failed };

    struct UpdateState
    {
        std::atomic<UpdatePhase> phase{UpdatePhase::Off};
        std::atomic<uint64_t> got{0};
        // Written by the worker thread before it publishes the phase, read by the UI after.
        std::string current, latest, url, error;
        uint64_t size = 0;
        fs::path file;
        bool ask = false;  // open the confirm popup this frame
        bool quit = false; // the installer started: close the launcher
    };
    UpdateState gUpdate;

    // "v0.1.10" > "0.1.9".
    bool newerVersion(const std::string &a, const std::string &b)
    {
        auto parts = [](std::string v)
        {
            if (!v.empty() && (v[0] == 'v' || v[0] == 'V'))
                v.erase(0, 1);
            std::vector<long> out;
            size_t i = 0;
            while (i < v.size() && std::isdigit(static_cast<unsigned char>(v[i])))
            {
                out.push_back(std::strtol(v.c_str() + i, nullptr, 10));
                while (i < v.size() && std::isdigit(static_cast<unsigned char>(v[i])))
                    ++i;
                if (i < v.size() && v[i] == '.')
                    ++i;
            }
            return out;
        };
        std::vector<long> x = parts(a), y = parts(b);
        const size_t n = std::max(x.size(), y.size());
        x.resize(n), y.resize(n);
        return x > y;
    }

    // The string value of "key" at or after `from` in GitHub's JSON (no general parser needed).
    std::string jsonString(const std::string &json, const char *key, size_t from = 0, size_t *at = nullptr)
    {
        const std::string k = std::string("\"") + key + "\"";
        size_t p = json.find(k, from);
        if (p == std::string::npos)
            return {};
        if (at)
            *at = p;
        p += k.size();
        while (p < json.size() && (std::isspace(static_cast<unsigned char>(json[p])) || json[p] == ':'))
            ++p;
        if (p >= json.size() || json[p] != '"')
            return {};
        std::string out;
        for (++p; p < json.size() && json[p] != '"'; ++p)
        {
            if (json[p] == '\\' && p + 1 < json.size())
                ++p;
            out += json[p];
        }
        return out;
    }

    void startUpdateCheck()
    {
        std::ifstream in(launcher::exeDir() / "version.txt");
        std::string version;
        std::getline(in, version);
        version = trim(version);
        if (version.empty())
            return;
        gUpdate.current = version;
        gUpdate.phase = UpdatePhase::Checking;
        std::thread([]
        {
            std::string body, error;
            if (!launcher::httpGet(kReleaseApi, &body, {}, error))
            {
                gUpdate.phase = UpdatePhase::Failed; // offline: no update, no message
                return;
            }
            const std::string tag = jsonString(body, "tag_name");
            // The asset whose browser_download_url ends with this platform's installer name; its "size" comes earlier
            // in the same asset object.
            for (size_t at = 0, from = 0;; from = at + 1)
            {
                const std::string url = jsonString(body, "browser_download_url", from, &at);
                if (url.empty())
                    break;
                if (url.size() < std::strlen(kInstallerSuffix) ||
                    url.compare(url.size() - std::strlen(kInstallerSuffix), std::string::npos, kInstallerSuffix) != 0)
                    continue;
                const size_t sz = body.rfind("\"size\"", at);
                if (sz != std::string::npos)
                {
                    size_t p = sz + 6;
                    while (p < body.size() && (std::isspace(static_cast<unsigned char>(body[p])) || body[p] == ':'))
                        ++p;
                    gUpdate.size = std::strtoull(body.c_str() + p, nullptr, 10);
                }
                gUpdate.url = url;
                break;
            }
            gUpdate.latest = tag.empty() || tag[0] != 'v' ? tag : tag.substr(1);
            gUpdate.phase = !gUpdate.url.empty() && newerVersion(tag, gUpdate.current) ? UpdatePhase::Available : UpdatePhase::Current;
        }).detach();
    }

    void startUpdateDownload()
    {
        gUpdate.file = fs::temp_directory_path() / gUpdate.url.substr(gUpdate.url.rfind('/') + 1);
        gUpdate.got = 0;
        gUpdate.error.clear();
        gUpdate.phase = UpdatePhase::Downloading;
        std::thread([]
        {
            std::string error;
            bool ok = launcher::httpGet(gUpdate.url, nullptr, gUpdate.file, error, &gUpdate.got);
            std::error_code ec;
            if (ok && gUpdate.size && fs::file_size(gUpdate.file, ec) != gUpdate.size)
                ok = false, error = "The download is incomplete. Try again.";
            if (!ok)
            {
                fs::remove(gUpdate.file, ec);
                gUpdate.error = error;
                gUpdate.phase = UpdatePhase::Available; // offer it again, with the error shown
                return;
            }
            gUpdate.phase = UpdatePhase::Downloaded;
        }).detach();
    }

    float updateProgress()
    {
        if (!gUpdate.size)
            return 0.0f;
        std::error_code ec;
        uint64_t got = gUpdate.got;
        if (got == 0) // curl writes the file itself
        {
            const auto sz = fs::file_size(gUpdate.file, ec);
            got = ec ? 0 : sz;
        }
        return std::min(1.0f, static_cast<float>(got) / static_cast<float>(gUpdate.size));
    }

    // Called every frame: starts the downloaded installer and closes the launcher so it can be replaced.
    void pollUpdate(std::string &status, bool &statusError)
    {
        if (gUpdate.phase != UpdatePhase::Downloaded)
            return;
        std::string error;
        if (launcher::startDetached(gUpdate.file, {}, error))
        {
            gUpdate.quit = true;
            return;
        }
        status = error;
        statusError = true;
        gUpdate.phase = UpdatePhase::Available;
    }

    bool updateAvailable() { return gUpdate.phase == UpdatePhase::Available; }
    bool updating() { return gUpdate.phase == UpdatePhase::Downloading || gUpdate.phase == UpdatePhase::Downloaded; }

    void updatePopup(const char *id)
    {
        if (gUpdate.ask)
        {
            ImGui::OpenPopup(id);
            gUpdate.ask = false;
        }
        ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 30.0f, 0));
        if (!ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove))
            return;
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 29.0f);
        ImGui::Text("S2Recomp %s is available (you have %s).", gUpdate.latest.c_str(), gUpdate.current.c_str());
        ImGui::Spacing();
        ImGui::Text("The launcher downloads its setup (%.0f MB), closes, and starts it. Setup updates S2Recomp and "
                    "rebuilds the game; a rebuild only compiles what changed. Your saves, settings and mods are kept.",
                    static_cast<double>(gUpdate.size) / (1024.0 * 1024.0));
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (ImGui::Button(ICON_FA_DOWNLOAD "  Update"))
        {
            startUpdateDownload();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Release notes"))
            launcher::openPath(fs::path("https://github.com/OddTrie/S2Recomp/releases/latest"));
        ImGui::SameLine();
        if (ImGui::Button("Not now") || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    void applyStyle(float dpi)
    {
        ImGui::StyleColorsDark();
        ImGuiStyle &st = ImGui::GetStyle();
        st.WindowRounding = 0.0f;
        st.FrameRounding = 6.0f;
        st.GrabRounding = 6.0f;
        st.TabRounding = 6.0f;
        st.ChildRounding = 8.0f;
        st.PopupRounding = 6.0f;
        st.FramePadding = ImVec2(10, 6);
        st.ItemSpacing = ImVec2(10, 8);
        st.WindowPadding = ImVec2(20, 16);
        st.ScrollbarSize = 12.0f;
        ImVec4 *c = st.Colors;
        c[ImGuiCol_WindowBg] = ImVec4(0.086f, 0.098f, 0.114f, 1.0f);
        c[ImGuiCol_ChildBg] = ImVec4(0.118f, 0.133f, 0.153f, 1.0f);
        c[ImGuiCol_PopupBg] = ImVec4(0.118f, 0.133f, 0.153f, 0.98f);
        c[ImGuiCol_Border] = ImVec4(1, 1, 1, 0.07f);
        c[ImGuiCol_FrameBg] = ImVec4(0.17f, 0.19f, 0.22f, 1.0f);
        c[ImGuiCol_FrameBgHovered] = ImVec4(0.22f, 0.25f, 0.29f, 1.0f);
        c[ImGuiCol_FrameBgActive] = ImVec4(0.25f, 0.29f, 0.33f, 1.0f);
        c[ImGuiCol_Button] = ImVec4(0.19f, 0.22f, 0.25f, 1.0f);
        c[ImGuiCol_ButtonHovered] = ImVec4(0.25f, 0.29f, 0.33f, 1.0f);
        c[ImGuiCol_ButtonActive] = ImVec4(0.29f, 0.33f, 0.38f, 1.0f);
        c[ImGuiCol_Header] = ImVec4(0.20f, 0.36f, 0.22f, 1.0f);
        c[ImGuiCol_HeaderHovered] = ImVec4(0.24f, 0.44f, 0.26f, 1.0f);
        c[ImGuiCol_HeaderActive] = ImVec4(0.28f, 0.52f, 0.30f, 1.0f);
        c[ImGuiCol_Tab] = ImVec4(0.14f, 0.16f, 0.18f, 1.0f);
        c[ImGuiCol_TabHovered] = ImVec4(0.24f, 0.44f, 0.26f, 1.0f);
        c[ImGuiCol_TabSelected] = ImVec4(0.20f, 0.38f, 0.22f, 1.0f);
        c[ImGuiCol_TabSelectedOverline] = kAccent;
        c[ImGuiCol_CheckMark] = kAccent;
        c[ImGuiCol_SliderGrab] = kAccent;
        c[ImGuiCol_SliderGrabActive] = kAccentHover;
        c[ImGuiCol_Separator] = ImVec4(1, 1, 1, 0.08f);
        c[ImGuiCol_TableHeaderBg] = ImVec4(0.14f, 0.16f, 0.18f, 1.0f);
        c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.025f);
        c[ImGuiCol_TextDisabled] = kMuted;
        c[ImGuiCol_NavCursor] = kAccentHover;
        st.ScaleAllSizes(dpi);
        st.FontSizeBase = 17.0f;
        st.FontScaleDpi = dpi;
    }

    // The Sims' plumbob: two pyramids, lit from the upper left.
    void drawPlumbob(ImDrawList *dl, ImVec2 c, float h)
    {
        const float w = h * 0.36f, mid = h * 0.12f;
        const ImVec2 top(c.x, c.y - h * 0.5f), bottom(c.x, c.y + h * 0.5f);
        const ImVec2 l(c.x - w, c.y - mid), r(c.x + w, c.y - mid), f(c.x + w * 0.15f, c.y - mid + h * 0.06f);
        dl->AddTriangleFilled(top, l, f, IM_COL32(120, 230, 120, 255));
        dl->AddTriangleFilled(top, f, r, IM_COL32(70, 185, 80, 255));
        dl->AddTriangleFilled(l, bottom, f, IM_COL32(60, 170, 70, 255));
        dl->AddTriangleFilled(f, bottom, r, IM_COL32(35, 125, 50, 255));
    }

    float labelWidth() { return ImGui::GetFontSize() * 11.0f; }

    void rowLabel(const char *label)
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(labelWidth());
        ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * 22.0f));
    }

    void rowHelp(const char *text)
    {
        ImGui::SetCursorPosX(labelWidth());
        ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    bool comboRow(const char *label, int &value, const char *const *items, int n, const char *help = nullptr)
    {
        rowLabel(label);
        ImGui::PushID(label);
        const bool changed = ImGui::Combo("##v", &value, items, n);
        ImGui::PopID();
        if (help)
            rowHelp(help);
        return changed;
    }

    bool checkRow(const char *label, bool &value, const char *text, const char *help = nullptr)
    {
        rowLabel(label);
        ImGui::PushID(label);
        const bool changed = ImGui::Checkbox(text, &value);
        ImGui::PopID();
        if (help)
            rowHelp(help);
        return changed;
    }

    void sectionTitle(const char *text)
    {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
        ImGui::TextUnformatted(text);
        ImGui::PopStyleColor();
        ImGui::Separator();
        ImGui::Spacing();
    }

    // Window size and fullscreen (not shown in Game Mode, where the game is always fullscreen).
    void windowRows(Ui &ui)
    {
        Settings &s = ui.s;
        rowLabel("Window size");
        struct Preset { int w, h; };
        static const Preset kPresets[] = {{960, 720}, {1280, 960}, {1440, 1080}, {1600, 1200}, {1920, 1440}, {2560, 1920},
                                          {1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
        char current[48];
        if (s.windowAuto)
            std::snprintf(current, sizeof(current), "Fit to screen (default)");
        else
            std::snprintf(current, sizeof(current), "%d x %d", s.windowW, s.windowH);
        ImGui::BeginDisabled(s.fullscreen);
        if (ImGui::BeginCombo("##window", current))
        {
            if (ImGui::Selectable("Fit to screen (default)", s.windowAuto))
                s.windowAuto = true;
            for (const Preset &p : kPresets)
            {
                if (p.w > ui.monitorW || p.h > ui.monitorH)
                    continue;
                char name[48];
                std::snprintf(name, sizeof(name), "%d x %d  (%s)", p.w, p.h, p.w * 3 == p.h * 4 ? "4:3" : "16:9");
                if (ImGui::Selectable(name, !s.windowAuto && s.windowW == p.w && s.windowH == p.h))
                    s.windowAuto = false, s.windowW = p.w, s.windowH = p.h;
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        {
            char help[160];
            std::snprintf(help, sizeof(help), "Your screen is %d x %d. The window can still be resized while playing.", ui.monitorW, ui.monitorH);
            rowHelp(help);
        }
        checkRow("Fullscreen", s.fullscreen, "Start in borderless fullscreen", "F11 switches between window and fullscreen at any time.");
    }

    void tabDisplay(Ui &ui)
    {
        Settings &s = ui.s;
        sectionTitle("Resolution");
        const bool cpu = s.renderer == 2;
        rowLabel("Internal resolution");
        ImGui::BeginDisabled(cpu);
        {
            // Segmented Auto / 1x..4x picker (0 = auto).
            const float w = std::min(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * 22.0f) / 5.0f;
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 0));
            for (int i = 0; i <= 4; ++i)
            {
                const bool on = !cpu && s.scale == i;
                if (on)
                {
                    ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccentHover);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.04f, 0.10f, 0.05f, 1.0f));
                }
                char name[16];
                if (i == 0)
                    std::snprintf(name, sizeof(name), "Auto");
                else
                    std::snprintf(name, sizeof(name), "%dx", i);
                if (i > 0)
                    ImGui::SameLine();
                if (ImGui::Button(name, ImVec2(w - 2, 0)))
                    s.scale = i;
                if (on)
                    ImGui::PopStyleColor(3);
            }
            ImGui::PopStyleVar();
        }
        ImGui::EndDisabled();
        {
            char help[200];
            if (cpu)
                std::snprintf(help, sizeof(help), "The CPU renderer always draws at the original 640 x 512. Pick the GPU renderer to render sharper.");
            else if (s.scale == 0)
                std::snprintf(help, sizeof(help), "Up to 3x (1920 x 1536), lowered while your graphics card cannot keep up and raised again when it can.");
            else
                std::snprintf(help, sizeof(help), "%d x %d%s. How sharp the 3D is drawn; higher needs a stronger graphics card.", 640 * s.scale,
                              512 * s.scale, s.scale == 1 ? " (original)" : s.scale == 3 ? " (default)" : "");
            rowHelp(help);
        }

        if (ui.gamescope)
        {
            rowLabel("Screen");
            ImGui::TextUnformatted("Fullscreen");
            rowHelp("In Steam Deck Game Mode the game always fills the screen.");
        }
        else
            windowRows(ui);

        sectionTitle("Picture");
        comboRow("Aspect ratio", s.aspect, kAspects, 4, "4:3 is how the game was made. 16:9 is true widescreen: the game shows more to the sides instead of stretching. Stretch just pulls the 4:3 picture wider.");
        comboRow("Scaling filter", s.filter, kFilters, 3, "How the picture is scaled up to the window.");
        comboRow("Interlace merge", s.interlace, kInterlace, 2, "Sharp keeps full detail; Soft blends lines like a CRT TV did (GPU renderer).");
    }

    void tabGraphics(Ui &ui)
    {
        Settings &s = ui.s;
        sectionTitle("Renderer");
        comboRow("Renderer", s.renderer, kRenderers, 3,
                 "The GPU renderer is faster on every PC tested and allows higher resolutions. "
                 "The CPU renderer is pixel-exact but needs a fast many-core processor.");
        sectionTitle("Smoothness");
        checkRow("Frame generation", s.framegen, "Show the game's 30 FPS at ~60 FPS",
                 "Adds in-between frames. Needs at least 2 processor threads.");
        checkRow("VSync", s.vsync, "Sync to the monitor", "Recommended. Off uses a 60 FPS timer instead (may tear).");
        sectionTitle("Textures");
        const bool cpu = s.renderer == 2;
        ImGui::BeginDisabled(cpu);
        checkRow("HD textures", s.hdTextures, "Use high-resolution texture packs",
                 cpu ? "GPU renderer only." : "Sharper button icons and logo, and HD pictures from mods.");
        ImGui::EndDisabled();
        checkRow("Title logo", s.branding, "S2Recomp title logo", "The title-screen logo with the RECOMP ribbon. Off shows the original.");
    }

    void tabAudioInput(Ui &ui)
    {
        Settings &s = ui.s;
        sectionTitle("Audio");
        checkRow("Sound", s.audio, "Play sound");
        rowLabel("Volume");
        ImGui::BeginDisabled(!s.audio);
        ImGui::SliderInt("##volume", &s.volume, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp);
        ImGui::EndDisabled();
        rowHelp("The game mixes very loud, like a real PS2; 50% is a comfortable start.");
        sectionTitle("Controls");
        comboRow("Button icons", s.glyphs, kGlyphs, 4, "Which buttons the game shows in its hints.");
        checkRow("Vibration", s.rumble, "Controller rumble");
        checkRow("Steam Input", s.steamInput, "Use Steam's virtual controller",
                 "Leave off unless you set up a Steam Input layout; off reads your controller directly.");
    }

    void tabMods(Ui &ui)
    {
        Settings &s = ui.s;
        ImGui::Spacing();
        ImGui::Checkbox("Load mods", &s.mods);
        ImGui::SameLine();
        ImGui::TextDisabled(s.mods ? "Enabled mods below are loaded when the game starts." : "The game starts without any mods.");
        ImGui::Spacing();
        if (!ui.gamescope)
        {
            if (ImGui::Button(ICON_FA_FOLDER_OPEN "  Open mods folder"))
            {
                std::error_code ec;
                fs::create_directories(modsRoot(), ec);
                launcher::openPath(modsRoot());
            }
            ImGui::SameLine();
        }
        if (ImGui::Button(ICON_FA_ROTATE "  Refresh"))
            ui.mods = scanMods();
        if (ui.gamescope)
        {
            // No file manager in Game Mode: mods are copied in from Desktop Mode.
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("Add mods from Desktop Mode, in %s", modsRoot().string().c_str());
        }
        else
        {
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_FILE_LINES "  How to make a mod"))
                launcher::openPath(modsRoot() / "README.txt");
        }
        ImGui::Spacing();

        ImGui::BeginDisabled(!s.mods);
        if (ui.mods.empty())
        {
            ImGui::BeginChild("empty", ImVec2(0, ImGui::GetFontSize() * 6), ImGuiChildFlags_Borders);
            ImGui::Spacing();
            ImGui::TextWrapped("No mods installed yet.");
            ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
            ImGui::TextWrapped("Put each mod in its own folder inside the mods folder, then press Refresh. "
                               "A mod folder holds files/ (disc files to replace), textures/ and an optional mod.ini.");
            ImGui::PopStyleColor();
            ImGui::EndChild();
        }
        else if (ImGui::BeginTable("mods", ui.gamescope ? 3 : 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY |
                                                  ImGuiTableFlags_PadOuterX, ImVec2(0, ImGui::GetContentRegionAvail().y)))
        {
            const float fs_ = ImGui::GetFontSize();
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, fs_ * 2.2f);
            ImGui::TableSetupColumn("Mod", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Priority", ImGuiTableColumnFlags_WidthFixed, fs_ * 6.5f);
            if (!ui.gamescope)
                ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, fs_ * 2.4f);
            ImGui::TableHeadersRow();
            for (Mod &m : ui.mods)
            {
                ImGui::PushID(m.folder.c_str());
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (ImGui::Checkbox("##on", &m.enabled))
                    setIniKey(m.dir / "mod.ini", "enabled", m.enabled ? "true" : "false");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(m.name.c_str());
                if (!m.version.empty())
                {
                    ImGui::SameLine();
                    ImGui::TextDisabled("v%s", m.version.c_str());
                }
                if (!m.author.empty())
                {
                    ImGui::SameLine();
                    ImGui::TextDisabled("by %s", m.author.c_str());
                }
                ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
                ImGui::PushTextWrapPos(0.0f);
                if (!m.description.empty())
                    ImGui::TextUnformatted(m.description.c_str());
                ImGui::Text("%s%d file%s, %d texture%s  -  mods/%s", m.code ? "Code mod (F2 in game), " : "", m.files,
                            m.files == 1 ? "" : "s", m.textures, m.textures == 1 ? "" : "s", m.folder.c_str());
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputInt("##pri", &m.priority, 1, 10))
                    setIniKey(m.dir / "mod.ini", "priority", std::to_string(m.priority));
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                    ImGui::SetTooltip("Higher loads later and wins when two mods replace the same file.");
                if (!ui.gamescope)
                {
                    ImGui::TableNextColumn();
                    if (ImGui::Button(ICON_FA_FOLDER_OPEN))
                        launcher::openPath(m.dir);
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                        ImGui::SetTooltip("Open this mod's folder");
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::EndDisabled();
    }

    void tabAdvanced(Ui &ui)
    {
        Settings &s = ui.s;
        sectionTitle("Game files");
        rowLabel("Game executable");
        char buf[1024];
        std::snprintf(buf, sizeof(buf), "%s", ui.elf.empty() ? "(not found)" : ui.elf.string().c_str());
        // No Browse button in Game Mode, so the path gets the whole row there.
        ImGui::SetNextItemWidth(ui.gamescope ? ImGui::GetContentRegionAvail().x
                                             : std::min(ImGui::GetContentRegionAvail().x - ImGui::GetFontSize() * 6, ImGui::GetFontSize() * 22.0f));
        ImGui::InputText("##elf", buf, sizeof(buf), ImGuiInputTextFlags_ReadOnly);
        if (!ui.gamescope)
        {
            ImGui::SameLine();
            if (ImGui::Button("Browse..."))
            {
                const fs::path p = launcher::pickFile("Choose the game executable (SLES_537.18) from the extracted disc");
                if (!p.empty() && !isElfFile(p))
                    ui.status = "That is not the game executable: choose SLES_537.18 from the extracted disc, not the disc image.", ui.statusError = true;
                else if (!p.empty())
                    ui.elf = p, s.elf = p.string(), ui.status.clear(), ui.statusError = false;
            }
        }
        rowHelp(ui.gamescope ? "SLES_537.18 from the extracted game disc, found next to the launcher. Pick another copy from Desktop Mode."
                             : "SLES_537.18 from the extracted game disc. Found automatically when the disc is extracted next to the launcher.");
        if (!s.elf.empty())
        {
            ImGui::SetCursorPosX(labelWidth());
            if (ImGui::SmallButton("Find automatically"))
                s.elf.clear(), ui.elf = findElf();
        }

        std::error_code ec;
        if (fs::exists(buildScript(), ec))
        {
            sectionTitle("Game build");
            rowLabel("Game");
            ImGui::BeginDisabled(building(ui) || updating());
            if (updateAvailable())
            {
                const std::string label = std::string(ICON_FA_DOWNLOAD "  Update to ") + gUpdate.latest;
                if (ImGui::Button(label.c_str()))
                    gUpdate.ask = true;
            }
            else if (ImGui::Button(!fs::exists(runnerPath(), ec) ? ICON_FA_HAMMER "  Build the game"
                              : gameOutdated()              ? ICON_FA_HAMMER "  Rebuild the game (updated)"
                                                            : ICON_FA_HAMMER "  Rebuild the game"))
                ui.askBuild = true;
            ImGui::EndDisabled();
            rowHelp(updateAvailable() ? "A new S2Recomp is out. Its setup updates S2Recomp and rebuilds the game."
                                      : "Built on this PC from your disc. A rebuild only compiles what changed, after an S2Recomp update.");
        }

        sectionTitle("Community");
        rowLabel("Discord");
        if (ImGui::Button(ICON_FA_COMMENTS "  Join the S2Recomp Discord"))
            launcher::openPath(fs::path(kDiscordUrl));
        rowHelp("News, help, and co-op partners.");

        if (!ui.gamescope)
        {
            sectionTitle("Troubleshooting");
            checkRow("Console", s.showConsole, "Show the game's console window", "Its output is always saved to run_log.txt.");
            rowLabel("Folders");
            if (ImGui::Button(ICON_FA_FILE_LINES "  run_log.txt") && !ui.elf.empty())
                launcher::openPath(gameRoot(ui.elf) / "run_log.txt");
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_FOLDER_OPEN "  Memory card") && !ui.elf.empty())
                launcher::openPath(ui.elf.parent_path() / "mc0");
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_FOLDER_OPEN "  Launcher folder"))
                launcher::openPath(launcher::exeDir());
            ImGui::Spacing();
        }

        sectionTitle(ICON_FA_FLASK "  Experimental");
        checkRow("60 FPS patch", s.fps60, "Run the game at 60 FPS where your PC keeps up",
                 "Drops to 30 in heavy scenes instead of slowing down. Not playable yet: some animations run at "
                 "double speed at 60. Use frame generation for smooth play.");
        ImGui::Spacing();
        ImGui::SetCursorPosX(labelWidth());
        if (ImGui::Button("Restore default settings"))
        {
            const std::string elf = s.elf;
            s = Settings();
            s.elf = elf;
        }
    }

    // Online co-op (EOS P2P, see ps2_netplay.cpp). The runtime reads eos.ini next to the runner, i.e. the
    // launcher's folder; one dropped next to the game files is copied there on Play.
    fs::path eosIni(const Ui &ui)
    {
        std::error_code ec;
        const fs::path here = launcher::exeDir() / "eos.ini";
        if (fs::exists(here, ec))
            return here;
        if (!ui.elf.empty() && fs::exists(gameRoot(ui.elf) / "eos.ini", ec))
            return gameRoot(ui.elf) / "eos.ini";
        return {};
    }

    // The host's join code (its EOS id), saved by the game the first time it hosts. It stays the same on this PC.
    std::string hostCode()
    {
        std::ifstream in(launcher::exeDir() / "netplay_code.txt");
        std::string code;
        std::getline(in, code);
        while (!code.empty() && std::isspace(static_cast<unsigned char>(code.back())))
            code.pop_back();
        return code;
    }

    bool textRow(const char *label, std::string &value, const char *hint, const char *help = nullptr)
    {
        rowLabel(label);
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s", value.c_str());
        ImGui::PushID(label);
        const bool changed = ImGui::InputTextWithHint("##v", hint, buf, sizeof(buf));
        ImGui::PopID();
        if (changed)
            value = buf;
        if (help)
            rowHelp(help);
        return changed;
    }

    // Short join codes look like EOSS-DK18: typed in caps, the dash filling itself in after the first 4.
    // Anything longer (the old full product user ID) is left as typed.
    std::string formatCode(const std::string &in, bool grew)
    {
        std::string raw;
        for (unsigned char c : in)
            if (c != '-')
                raw += static_cast<char>(std::toupper(c));
        if (raw.size() > 8)
            return in;
        if (raw.size() > 4 || (raw.size() == 4 && grew))
            raw.insert(4, "-");
        return raw;
    }

    int codeInputCallback(ImGuiInputTextCallbackData *data)
    {
        int &prevLen = *static_cast<int *>(data->UserData);
        if (data->EventFlag == ImGuiInputTextFlags_CallbackCharFilter)
        {
            const ImWchar c = data->EventChar;
            if (c >= 'a' && c <= 'z')
                data->EventChar = static_cast<ImWchar>(c - 'a' + 'A');
            else if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-'))
                return 1;
            return 0;
        }
        const std::string before(data->Buf, data->BufTextLen);
        int typed = 0; // letters/digits before the cursor, so it lands in the same spot after the dash moves
        for (int i = 0; i < data->CursorPos; ++i)
            typed += data->Buf[i] != '-';
        const std::string after = formatCode(before, data->BufTextLen > prevLen);
        if (after != before)
        {
            data->DeleteChars(0, data->BufTextLen);
            data->InsertChars(0, after.c_str());
            int pos = 0;
            for (int n = 0; pos < static_cast<int>(after.size()) && (n < typed || after[pos] == '-'); ++pos)
                n += after[pos] != '-';
            data->CursorPos = data->SelectionStart = data->SelectionEnd = pos;
        }
        prevLen = data->BufTextLen;
        return 0;
    }

    void codeRow(const char *label, std::string &value, const char *hint)
    {
        rowLabel(label);
        static int prevLen = 0;
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s", value.c_str());
        ImGui::PushID(label);
        if (ImGui::InputTextWithHint("##v", hint, buf, sizeof(buf),
                                     ImGuiInputTextFlags_CallbackCharFilter | ImGuiInputTextFlags_CallbackEdit,
                                     codeInputCallback, &prevLen))
            value = buf;
        if (!ImGui::IsItemActive())
            prevLen = static_cast<int>(std::strlen(buf));
        ImGui::PopID();
    }

    void tabCoop(Ui &ui)
    {
        Settings &s = ui.s;
        sectionTitle(ICON_FA_USERS "  Online co-op");
        comboRow("Mode", s.netMode, kNetModes, 3,
                 "Two players in one game over the internet: the host plays controller 1, the friend who joins "
                 "plays controller 2. Both need the same game build and mods.");
        if (s.netMode == 0)
            return;
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), s.netMode == 1
            ? ICON_FA_CIRCLE_INFO "  The game holds at the start until your friend joins. Set Mode to Off to play alone."
            : ICON_FA_CIRCLE_INFO "  Start after the host: their game waits for you at the start.");

        textRow("Your name", s.netName, s.netMode == 1 ? "Host" : "Guest");
        if (s.netMode == 1)
        {
            rowLabel("Join code");
            const std::string code = hostCode();
            if (code.empty())
                ImGui::TextDisabled("Shown here after you host once");
            else
            {
                ImGui::TextUnformatted(code.c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton(ICON_FA_COPY "  Copy"))
                    ImGui::SetClipboardText(code.c_str());
            }
            rowHelp("Send it to your friend. It stays the same on this PC, so they only need it once. "
                    "The game also prints it in its console when you host.");
        }
        else
        {
            codeRow("Host's code", s.netCode, "XXXX-XXXX");
            ImGui::SetCursorPosX(labelWidth());
            if (!ui.gamescope)
            {
                if (ImGui::SmallButton(ICON_FA_PASTE "  Paste"))
                    if (const char *clip = ImGui::GetClipboardText())
                    {
                        s.netCode = clip;
                        s.netCode.erase(std::remove_if(s.netCode.begin(), s.netCode.end(), [](unsigned char c) { return std::isspace(c); }),
                                        s.netCode.end());
                        s.netCode = formatCode(s.netCode, true);
                    }
                ImGui::SameLine();
            }
            // Always there, so a mistyped code can be started over even where the keyboard can't erase.
            if (ImGui::SmallButton(ICON_FA_DELETE_LEFT "  Clear"))
                s.netCode.clear();
            ImGui::Spacing();
        }

        rowLabel("Input delay");
        ImGui::SliderInt("##delay", &s.netDelay, 1, 10, "%d frames");
        rowHelp("Both players must use the same value. Raise it if the game stutters on a slow connection.");

        sectionTitle("Epic Online Services");
        rowLabel("eos.ini");
        const fs::path ini = eosIni(ui);
        if (!ini.empty())
            ImGui::TextColored(kAccent, ICON_FA_CHECK "  Found");
        else
            ImGui::TextColored(kWarn, ICON_FA_TRIANGLE_EXCLAMATION "  Missing");
        if (!ui.gamescope)
        {
            ImGui::SameLine();
            if (ImGui::SmallButton("Choose..."))
            {
                const fs::path p = launcher::pickFile("Choose eos.ini");
                std::error_code ec;
                if (!p.empty())
                    fs::copy_file(p, launcher::exeDir() / "eos.ini", fs::copy_options::overwrite_existing, ec);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(ICON_FA_FOLDER_OPEN "  Launcher folder"))
                launcher::openPath(launcher::exeDir());
        }
        rowHelp(s.netMode == 1 ? "Your Epic Developer Portal IDs (see eos.ini.example). Give the same file to your friend, and only to them: it holds your client secret."
                               : "Get eos.ini from the host and put it in the launcher's folder.");
    }

    void play(Ui &ui)
    {
        const fs::path runner = runnerPath();
        std::error_code ec;
        if (!fs::exists(runner, ec))
        {
            ui.status = std::string(kRunnerName) + " is missing from the launcher's folder.";
            ui.statusError = true;
            return;
        }
        if (ui.elf.empty() || !fs::exists(ui.elf, ec))
        {
            ui.status = "Game files not found: choose SLES_537.18 under Advanced.";
            ui.statusError = true;
            return;
        }
        if (ui.s.netMode != 0)
        {
            const fs::path ini = eosIni(ui);
            if (ini.empty())
            {
                ui.status = "Co-op needs eos.ini in the launcher's folder (see the Co-op tab).";
                ui.statusError = true;
                return;
            }
            if (ini.parent_path() != launcher::exeDir())
                fs::copy_file(ini, launcher::exeDir() / "eos.ini", fs::copy_options::overwrite_existing, ec);
            if (gameOutdated())
            {
                // An old game cannot talk to an updated one (the host's save is not shared).
                ui.status = "S2Recomp was updated: rebuild the game before co-op.";
                ui.statusError = true;
                ui.askBuild = !building(ui);
                return;
            }
            if (ui.s.netMode == 2 && ui.s.netCode.empty())
            {
                ui.status = "Paste the host's join code in the Co-op tab first.";
                ui.statusError = true;
                return;
            }
        }
        saveSettings(ui.s);
        const fs::path root = gameRoot(ui.elf);
        const std::vector<launcher::EnvVar> env = gameEnv(ui.s, ui.gamescope);
        std::string error;
        ui.game = launcher::startGame(runner, ui.elf, root, env, root / "run_log.txt",
                                      ui.s.showConsole && !ui.gamescope, error);
        if (!ui.game)
        {
            ui.status = error;
            ui.statusError = true;
            launcher::showError("S2Recomp", error);
            return;
        }
        ui.status.clear();
        ui.statusError = false;
        SetWindowState(FLAG_WINDOW_HIDDEN);
    }

    // Lists the games hosted on this PC's local network (launcher_lobbies.cpp), so the other PC joins without a code.
    void tabLobbies(Ui &ui)
    {
        Settings &s = ui.s;
        sectionTitle(ICON_FA_NETWORK_WIRED "  LAN");
        const fs::path ini = eosIni(ui);
        if (!lobbies::available() || ini.empty())
        {
            ImGui::TextColored(kWarn, ICON_FA_TRIANGLE_EXCLAMATION "  %s",
                               lobbies::available() ? "LAN play needs eos.ini in the launcher's folder (see the Co-op tab)."
                                                    : lobbies::status().c_str());
            return;
        }
        lobbies::start(ini);

        textRow("Your name", s.netName, "Player", "Shown to others when you host.");
        rowLabel("Input delay");
        ImGui::SliderInt("##lobbydelay", &s.netDelay, 1, 10, "%d frames");
        rowHelp("Used when you host. Joining takes the host's value.");
        ImGui::SetCursorPosX(labelWidth());
        if (ImGui::Button(ICON_FA_SERVER "  Host on LAN"))
        {
            s.netMode = 1;
            play(ui);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(lobbies::busy());
        if (ImGui::Button(ICON_FA_ARROWS_ROTATE "  Refresh"))
            lobbies::refresh();
        ImGui::EndDisabled();
        rowHelp("Hosting starts the game; it waits at the start until someone on your network joins, then leaves the list. "
                "Both PCs still need internet for the Epic sign-in; the game traffic itself stays on your network.");

        sectionTitle("Games on your network");
        const std::string st = lobbies::status();
        if (lobbies::failed())
            ImGui::TextColored(kWarn, ICON_FA_TRIANGLE_EXCLAMATION "  %s", st.c_str());
        else
            ImGui::TextDisabled("%s", st.c_str());
        ImGui::Spacing();

        const std::vector<lobbies::Entry> list = lobbies::list();
        if (list.empty())
            return;
        if (ImGui::BeginTable("lobbies", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_PadOuterX))
        {
            ImGui::TableSetupColumn("Host", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Delay", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 6.0f);
            ImGui::TableSetupColumn("##join", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 6.0f);
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < list.size(); ++i)
            {
                const lobbies::Entry &e = list[i];
                ImGui::PushID(static_cast<int>(i));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(e.name.c_str());
                if (e.mine)
                {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(this PC)");
                }
                ImGui::TableNextColumn();
                ImGui::Text("%d frames", e.delay);
                ImGui::TableNextColumn();
                ImGui::BeginDisabled(e.mine);
                if (ImGui::SmallButton(ICON_FA_RIGHT_TO_BRACKET "  Join"))
                {
                    s.netMode = 2;
                    s.netCode = e.code.size() == 8 ? formatCode(e.code, true) : e.owner;
                    s.netDelay = e.delay;
                    play(ui);
                }
                ImGui::EndDisabled();
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    void drawUi(Ui &ui)
    {
        const ImGuiViewport *vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(vp->Size);
        ImGui::Begin("launcher", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                              ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
        const float fsz = ImGui::GetFontSize();

        // Header
        {
            ImDrawList *dl = ImGui::GetWindowDrawList();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float h = fsz * 2.6f;
            if (ui.padConnected)
            {
                const char *hint = "A  select     B  back     LB / RB  tabs     START  play";
                const ImVec2 sz = ImGui::CalcTextSize(hint);
                dl->AddText(ImVec2(p.x + ImGui::GetContentRegionAvail().x - sz.x, p.y + (h - sz.y) * 0.5f),
                            ImGui::GetColorU32(ImGuiCol_TextDisabled), hint);
            }
            drawPlumbob(dl, ImVec2(p.x + h * 0.32f, p.y + h * 0.5f), h * 0.95f);
            ImGui::SetCursorScreenPos(ImVec2(p.x + h * 0.8f, p.y - fsz * 0.1f));
            ImGui::PushFont(ui.title, ImGui::GetStyle().FontSizeBase * 1.9f);
            ImGui::TextUnformatted("S2Recomp");
            ImGui::PopFont();
            ImGui::SetCursorScreenPos(ImVec2(p.x + h * 0.8f, ImGui::GetCursorScreenPos().y - fsz * 0.35f));
            ImGui::TextDisabled("The Sims 2  -  PS2 recompiled for PC");
            ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + fsz * 0.4f));
        }

        // Tabs
        const float footerH = fsz * 3.6f;
        // NavFlattened: the controller moves straight between the tabs, the page and Play.
        ImGui::BeginChild("body", ImVec2(0, -footerH), ImGuiChildFlags_NavFlattened);
        if (ImGui::BeginTabBar("tabs"))
        {
            struct Tab { const char *name; void (*fn)(Ui &); };
            static const Tab kTabs[] = {
                {ICON_FA_DISPLAY "  Display", tabDisplay},
                {ICON_FA_IMAGE "  Graphics", tabGraphics},
                {ICON_FA_VOLUME_HIGH "  Audio & Input", tabAudioInput},
                {ICON_FA_PUZZLE_PIECE "  Mods", tabMods},
                {ICON_FA_USERS "  Co-op", tabCoop},
                {ICON_FA_NETWORK_WIRED "  LAN", tabLobbies},
                {ICON_FA_GEAR "  Advanced", tabAdvanced},
            };
            const int tabCount = static_cast<int>(std::size(kTabs));
            if (ui.padTabStep)
                ui.tabRequest = (ui.tab + ui.padTabStep + tabCount) % tabCount;
            for (int i = 0; i < tabCount; ++i)
            {
                if (ImGui::BeginTabItem(kTabs[i].name, nullptr, ui.tabRequest == i ? ImGuiTabItemFlags_SetSelected : 0))
                {
                    ui.tab = i;
                    // Inner margin so the page's text doesn't sit on the panel's edge.
                    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ImGui::GetFontSize() * 1.2f, ImGui::GetFontSize() * 0.6f));
                    ImGui::BeginChild("page", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened);
                    ImGui::PopStyleVar();
                    kTabs[i].fn(ui);
                    if (ui.padScroll != 0.0f)
                        ImGui::SetScrollY(ImGui::GetScrollY() + ui.padScroll * ImGui::GetFontSize() * 40.0f * ImGui::GetIO().DeltaTime);
                    ImGui::EndChild();
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
            ui.tabRequest = -1;
        }
        ImGui::EndChild();

        // Footer: status and Play
        ImGui::Separator();
        ImGui::Spacing();
        const float playW = fsz * 9.0f, playH = fsz * 2.2f;
        const float statusY = ImGui::GetCursorPosY();
        ImGui::SetCursorPosY(statusY + (playH - fsz) * 0.5f);
        std::error_code ec;
        const bool built = fs::exists(runnerPath(), ec);
        const bool canBuild = fs::exists(buildScript(), ec);
        if (building(ui))
        {
            ImGui::SetCursorPosY(statusY);
            ImGui::TextUnformatted(ui.build.step.c_str());
            char pct[16];
            std::snprintf(pct, sizeof(pct), "%d%%", static_cast<int>(ui.build.progress * 100.0f + 0.5f));
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, kAccent);
            ImGui::ProgressBar(ui.build.progress, ImVec2(ImGui::GetContentRegionMax().x - playW - fsz * 2, fsz * 0.9f), pct);
            ImGui::PopStyleColor();
        }
        else if (updating())
        {
            ImGui::SetCursorPosY(statusY);
            ImGui::Text("Downloading S2Recomp %s...", gUpdate.latest.c_str());
            const float f = updateProgress();
            char pct[16];
            std::snprintf(pct, sizeof(pct), "%d%%", static_cast<int>(f * 100.0f + 0.5f));
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, kAccent);
            ImGui::ProgressBar(f, ImVec2(ImGui::GetContentRegionMax().x - playW - fsz * 2, fsz * 0.9f), pct);
            ImGui::PopStyleColor();
        }
        else if (updateAvailable() && (built || !gUpdate.error.empty()))
        {
            ImGui::PushStyleColor(ImGuiCol_Text, gUpdate.error.empty() ? kAccent : kWarn);
            if (gUpdate.error.empty())
                ImGui::Text(ICON_FA_CIRCLE_DOWN "  S2Recomp %s is available.", gUpdate.latest.c_str());
            else
                ImGui::Text(ICON_FA_TRIANGLE_EXCLAMATION "  Update failed: %s", gUpdate.error.c_str());
            ImGui::PopStyleColor();
            ImGui::SameLine();
            if (ImGui::SmallButton(gUpdate.error.empty() ? "Update" : "Retry"))
                gUpdate.ask = true;
        }
        else if (!ui.status.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ui.statusError ? kWarn : kMuted);
            if (ui.statusError)
            {
                ImGui::TextUnformatted(ICON_FA_TRIANGLE_EXCLAMATION);
                ImGui::SameLine();
            }
            ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x - playW - fsz);
            ImGui::TextUnformatted(ui.status.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            if (ui.lastRunFailed && !ui.elf.empty() && !ui.gamescope)
            {
                ImGui::SameLine();
                if (ImGui::SmallButton("Open log"))
                    launcher::openPath(gameRoot(ui.elf) / "run_log.txt");
            }
        }
        else if (!built && canBuild)
            ImGui::TextDisabled("The game is not built yet: S2Recomp builds it on this PC from your disc.");
        else
        {
            const int enabledMods = static_cast<int>(std::count_if(ui.mods.begin(), ui.mods.end(), [](const Mod &m) { return m.enabled; }));
            const std::string res = ui.s.renderer == 2 ? "1x" : ui.s.scale == 0 ? "auto" : std::to_string(ui.s.scale) + "x";
            ImGui::TextDisabled("%s  -  %s  -  %s%s%s", ui.s.renderer == 2 ? "CPU" : "GPU", res.c_str(),
                                ui.s.fullscreen || ui.gamescope ? "fullscreen" : "windowed",
                                !ui.s.mods || enabledMods == 0 ? "" : enabledMods == 1 ? "  -  1 mod" : ("  -  " + std::to_string(enabledMods) + " mods").c_str(),
                                ui.s.netMode == 1 ? "  -  co-op host" : ui.s.netMode == 2 ? "  -  co-op join" : "");
        }
        ImGui::SetCursorPos(ImVec2(ImGui::GetContentRegionMax().x - playW, statusY));
        ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccentHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccentActive);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.04f, 0.10f, 0.05f, 1.0f));
        ImGui::PushFont(ui.body, ImGui::GetStyle().FontSizeBase * 1.2f); // the body font carries the icons
        if (ui.focusPlay && ui.padConnected)
        {
            // Controller users start on Play, so A starts the game.
            ImGui::SetKeyboardFocusHere();
            ImGui::SetNavCursorVisible(true);
            ui.focusPlay = false;
        }
        if (building(ui) || updating())
        {
            ImGui::BeginDisabled();
            ImGui::Button(updating() ? ICON_FA_DOWNLOAD "  UPDATING" : ICON_FA_HAMMER "  BUILDING", ImVec2(playW, playH));
            ImGui::EndDisabled();
        }
        else if (!built && canBuild && updateAvailable())
        {
            // Not built yet and a newer release is out: build that one instead.
            if (ImGui::Button(ICON_FA_DOWNLOAD "  UPDATE", ImVec2(playW, playH)) || ui.padPlay)
                gUpdate.ask = true;
        }
        else if (!built && canBuild)
        {
            if (ImGui::Button(ICON_FA_HAMMER "  BUILD GAME", ImVec2(playW, playH)) || ui.padPlay)
                ui.askBuild = true;
        }
        else if (ImGui::Button(ICON_FA_PLAY "  PLAY", ImVec2(playW, playH)) || ui.padPlay)
            play(ui);
        ImGui::PopFont();
        ImGui::PopStyleColor(4);
        buildPopup(ui, "Build the game");
        updatePopup("Update S2Recomp");
        ImGui::End();
    }

    // Controller input. rlImGui's own mapping swaps B with X and Start with Back, so beginFrame keeps it off the pad
    // and the buttons ImGui navigates with are fed here; LB / RB, Start and the right stick are the launcher's.
    void feedGamepad(Ui &ui)
    {
        ui.padPlay = false;
        ui.padTabStep = 0;
        ui.padScroll = 0.0f;
        ui.padConnected = IsGamepadAvailable(0);
        if (!ui.padConnected)
            return;
        ImGuiIO &io = ImGui::GetIO();
        struct Map { GamepadButton button; ImGuiKey key; };
        static const Map kMap[] = {
            {GAMEPAD_BUTTON_LEFT_FACE_UP, ImGuiKey_GamepadDpadUp},
            {GAMEPAD_BUTTON_LEFT_FACE_DOWN, ImGuiKey_GamepadDpadDown},
            {GAMEPAD_BUTTON_LEFT_FACE_LEFT, ImGuiKey_GamepadDpadLeft},
            {GAMEPAD_BUTTON_LEFT_FACE_RIGHT, ImGuiKey_GamepadDpadRight},
            {GAMEPAD_BUTTON_RIGHT_FACE_DOWN, ImGuiKey_GamepadFaceDown},   // A: press / open
            {GAMEPAD_BUTTON_RIGHT_FACE_RIGHT, ImGuiKey_GamepadFaceRight}, // B: back / close
            {GAMEPAD_BUTTON_RIGHT_FACE_LEFT, ImGuiKey_GamepadFaceLeft},   // X
            {GAMEPAD_BUTTON_RIGHT_FACE_UP, ImGuiKey_GamepadFaceUp},       // Y
        };
        for (const Map &m : kMap)
        {
            if (IsGamepadButtonPressed(0, m.button))
                io.AddKeyEvent(m.key, true);
            else if (IsGamepadButtonReleased(0, m.button))
                io.AddKeyEvent(m.key, false);
        }
        const float dead = 0.25f;
        auto stick = [&](GamepadAxis axis, ImGuiKey neg, ImGuiKey pos)
        {
            const float v = GetGamepadAxisMovement(0, axis);
            io.AddKeyAnalogEvent(neg, v < -dead, v < -dead ? -v : 0.0f);
            io.AddKeyAnalogEvent(pos, v > dead, v > dead ? v : 0.0f);
        };
        stick(GAMEPAD_AXIS_LEFT_X, ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight);
        stick(GAMEPAD_AXIS_LEFT_Y, ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown);
        if (IsGamepadButtonPressed(0, GAMEPAD_BUTTON_LEFT_TRIGGER_1))
            ui.padTabStep = -1;
        if (IsGamepadButtonPressed(0, GAMEPAD_BUTTON_RIGHT_TRIGGER_1))
            ui.padTabStep = 1;
        ui.padPlay = IsGamepadButtonPressed(0, GAMEPAD_BUTTON_MIDDLE_RIGHT);
        const float rs = GetGamepadAxisMovement(0, GAMEPAD_AXIS_RIGHT_Y);
        ui.padScroll = std::fabs(rs) > dead ? rs : 0.0f;
    }

    // Keys pressed and released within one frame: the Steam Deck's on-screen keyboard taps keys that fast. raylib's
    // key state never shows them down, so rlImGui misses them (letters still arrive as characters, but Backspace
    // didn't); its pressed-key queue still has them, so the text-editing ones are fed to ImGui as a tap here.
    void feedTappedKeys()
    {
        static const struct { int key; ImGuiKey imgui; } kKeys[] = {
            {KEY_BACKSPACE, ImGuiKey_Backspace}, {KEY_DELETE, ImGuiKey_Delete}, {KEY_ENTER, ImGuiKey_Enter},
            {KEY_KP_ENTER, ImGuiKey_KeypadEnter}, {KEY_TAB, ImGuiKey_Tab}, {KEY_ESCAPE, ImGuiKey_Escape},
            {KEY_LEFT, ImGuiKey_LeftArrow}, {KEY_RIGHT, ImGuiKey_RightArrow}, {KEY_UP, ImGuiKey_UpArrow},
            {KEY_DOWN, ImGuiKey_DownArrow}, {KEY_HOME, ImGuiKey_Home}, {KEY_END, ImGuiKey_End},
        };
        ImGuiIO &io = ImGui::GetIO();
        for (int key = GetKeyPressed(); key != 0; key = GetKeyPressed())
        {
            if (IsKeyDown(key)) // a normal press: rlImGui has it
                continue;
            for (const auto &k : kKeys)
                if (k.key == key)
                {
                    io.AddKeyEvent(k.imgui, true); // ImGui's event queue spreads the down/up over two frames
                    io.AddKeyEvent(k.imgui, false);
                }
        }
    }

    // rlImGuiBegin() with the controller fed by feedGamepad instead of rlImGui.
    void beginFrame(Ui &ui)
    {
        ImGuiIO &io = ImGui::GetIO();
        ImGui_ImplRaylib_NewFrame();
        io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
        ImGui_ImplRaylib_ProcessEvents();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
        feedTappedKeys();
        feedGamepad(ui);
        ImGui::NewFrame();
    }

    bool envSet(const char *name)
    {
        const char *v = std::getenv(name);
        return v && *v && std::string(v) != "0";
    }

    // Steam sets SteamDeck=1 on a Deck and SteamTenfoot=1 in Big Picture; Game Mode's gamescope shows in the Linux
    // environment, which Proton passes on. S2RECOMP_LAUNCHER_UI=desktop|couch|gamescope overrides the guess.
    void detectCouch(Ui &ui)
    {
        const char *desktop = std::getenv("XDG_CURRENT_DESKTOP");
        ui.gamescope = envSet("GAMESCOPE_WAYLAND_DISPLAY") || (desktop && lower(desktop) == "gamescope");
        ui.couch = ui.gamescope || envSet("SteamDeck") || envSet("SteamTenfoot");
        if (const char *mode = std::getenv("S2RECOMP_LAUNCHER_UI"))
        {
            const std::string m = lower(mode);
            ui.gamescope = m == "gamescope";
            ui.couch = ui.gamescope || m == "couch";
        }
    }

    // A Steam Deck: Steam's SteamDeck=1, or the board in the Linux DMI table (Jupiter = LCD, Galileo = OLED).
    bool isSteamDeck()
    {
        if (envSet("SteamDeck"))
            return true;
        std::ifstream f("/sys/devices/virtual/dmi/id/product_name");
        std::string name;
        return f && std::getline(f, name) && (name == "Jupiter" || name == "Galileo");
    }
}

int main()
{
    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT | FLAG_MSAA_4X_HINT);
    InitWindow(820, 640, "S2Recomp Launcher");
    SetExitKey(KEY_NULL);

    Ui ui;
    detectCouch(ui);
    ui.dpi = std::max(1.0f, GetWindowScaleDPI().y);
    const int monitor = GetCurrentMonitor();
    ui.monitorW = GetMonitorWidth(monitor);
    ui.monitorH = GetMonitorHeight(monitor);
    if (ui.couch)
    {
        // Fills the screen with the 820 x 640 layout scaled up to it (1.25x on a Steam Deck's 1280 x 800).
        ui.dpi = std::max(ui.dpi, std::clamp(std::min(ui.monitorW / 820.0f, ui.monitorH / 640.0f), 1.0f, 2.5f));
        SetWindowState(FLAG_WINDOW_UNDECORATED);
        SetWindowSize(ui.monitorW, ui.monitorH);
        SetWindowPosition(0, 0);
    }
    else
    {
        const int w = static_cast<int>(820 * ui.dpi), h = static_cast<int>(640 * ui.dpi);
        SetWindowSize(w, h);
        SetWindowMinSize(static_cast<int>(640 * ui.dpi), static_cast<int>(480 * ui.dpi));
        SetWindowPosition((ui.monitorW - w) / 2, (ui.monitorH - h) / 2);
    }
#if PS2X_APP_ICON_PNG
    Image icon = LoadImageFromMemory(".png", kAppIconPng, static_cast<int>(kAppIconPng_size));
    SetWindowIcon(icon);
    UnloadImage(icon);
#endif

    rlImGuiBeginInitImGui();
    applyStyle(ui.dpi);
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    // The compiled-in fonts; the icon font (added by rlImGuiEndInitImGui) merges into the last font added.
    {
        ImFontConfig cfg;
        cfg.FontDataOwnedByAtlas = false;
        ui.title = io.Fonts->AddFontFromMemoryTTF(kFont_Roboto_Medium, static_cast<int>(kFont_Roboto_Medium_size), 17.0f, &cfg);
        ui.body = io.Fonts->AddFontFromMemoryTTF(kFont_DroidSans, static_cast<int>(kFont_DroidSans_size), 17.0f, &cfg);
        io.FontDefault = ui.body;
    }
    rlImGuiEndInitImGui();

    ui.s = loadSettings();
    // The Deck's GPU cannot draw the 3x default at a playable rate (~4 FPS in the neighbourhood pan): switch an
    // untouched 3x to Auto once (1x, up to 2x while there is room). A scale picked afterwards stays.
    if (!ui.s.deckScale && isSteamDeck())
    {
        if (ui.s.scale == 3)
            ui.s.scale = 0;
        ui.s.deckScale = true;
        saveSettings(ui.s);
    }
    if (!ui.s.elf.empty() && FileExists(ui.s.elf.c_str()) && isElfFile(ui.s.elf))
        ui.elf = ui.s.elf;
    else
        ui.elf = findElf();
    if (ui.elf.empty())
        ui.status = "Game files not found: choose SLES_537.18 under Advanced.", ui.statusError = true;
    ui.mods = scanMods();
    startUpdateCheck();

    while (!WindowShouldClose() && !gUpdate.quit)
    {
        if (ui.game)
        {
            uint32_t code = 0;
            if (launcher::gameRunning(ui.game, code))
            {
                PollInputEvents();
                WaitTime(0.2);
                continue;
            }
            launcher::closeGame(ui.game);
            ui.game = nullptr;
            ui.lastRunFailed = code != 0;
            ui.status.clear();
            ui.statusError = false;
            if (ui.lastRunFailed)
            {
                char text[160];
                std::snprintf(text, sizeof(text), "The game stopped with an error (exit code 0x%X). run_log.txt has the details.", code);
                ui.status = text;
                ui.statusError = true;
            }
            ui.mods = scanMods();
            ui.focusPlay = true;
            ClearWindowState(FLAG_WINDOW_HIDDEN);
        }
        pollBuild(ui);
        pollUpdate(ui.status, ui.statusError);

        if (IsFileDropped())
        {
            // A dropped SLES_xxx.xx picks the game executable.
            FilePathList dropped = LoadDroppedFiles();
            for (unsigned i = 0; i < dropped.count; ++i)
                if (isBootElfName(fs::path(dropped.paths[i]).filename().string()))
                    ui.elf = dropped.paths[i], ui.s.elf = dropped.paths[i], ui.status.clear(), ui.statusError = false;
            UnloadDroppedFiles(dropped);
        }

        BeginDrawing();
        ClearBackground(Color{22, 25, 29, 255});
        beginFrame(ui);
        drawUi(ui);
        rlImGuiEnd();
        EndDrawing();
    }

    saveSettings(ui.s);
    lobbies::shutdown();
    rlImGuiShutdown();
    CloseWindow();
    return 0;
}
