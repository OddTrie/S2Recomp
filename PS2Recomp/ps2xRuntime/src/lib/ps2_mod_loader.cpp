#include "runtime/ps2_mod_loader.h"
#include "Kernel/Stubs/MemoryCard.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

// raylib's keyboard state (raylib.h clashes with windows.h, so only these two are declared).
extern "C" bool IsKeyPressed(int key);
extern "C" bool IsKeyDown(int key);

namespace ps2x::mods
{
    namespace
    {
        std::filesystem::path exeDir()
        {
#if defined(_WIN32)
            wchar_t buf[MAX_PATH];
            const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
            if (n > 0 && n < MAX_PATH)
                return std::filesystem::path(buf).parent_path();
#endif
            return std::filesystem::current_path();
        }

        std::string lower(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(),
                           [](unsigned char c)
                           { return static_cast<char>(std::tolower(c)); });
            return value;
        }

        std::string trim(std::string_view value)
        {
            size_t begin = 0, end = value.size();
            while (begin < end && std::isspace(static_cast<unsigned char>(value[begin])))
                ++begin;
            while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])))
                --end;
            return std::string(value.substr(begin, end - begin));
        }

        // "DATA\\Fonts.arc;1" -> "data/fonts.arc"
        std::string pathKey(std::string_view path)
        {
            std::string key(path);
            std::replace(key.begin(), key.end(), '\\', '/');
            if (const size_t semicolon = key.find(';'); semicolon != std::string::npos)
                key.erase(semicolon);
            if (const size_t colon = key.find(':'); colon != std::string::npos)
                key.erase(0, colon + 1);
            while (!key.empty() && (key.front() == '/' || std::isspace(static_cast<unsigned char>(key.front()))))
                key.erase(key.begin());
            while (!key.empty() && std::isspace(static_cast<unsigned char>(key.back())))
                key.pop_back();
            return lower(std::move(key));
        }

        bool parseBool(const std::string &value, bool fallback)
        {
            const std::string v = lower(value);
            if (v == "1" || v == "true" || v == "yes" || v == "on")
                return true;
            if (v == "0" || v == "false" || v == "no" || v == "off")
                return false;
            return fallback;
        }

        void readManifest(ModInfo &mod)
        {
            std::ifstream in(mod.dir / "mod.ini");
            std::string line;
            while (in && std::getline(in, line))
            {
                if (line.rfind("\xEF\xBB\xBF", 0) == 0) // UTF-8 BOM (Notepad, PowerShell)
                    line.erase(0, 3);
                const std::string text = trim(line);
                if (text.empty() || text[0] == '#' || text[0] == ';' || text[0] == '[')
                    continue;
                const size_t eq = text.find('=');
                if (eq == std::string::npos)
                    continue;
                const std::string key = lower(trim(std::string_view(text).substr(0, eq)));
                const std::string value = trim(std::string_view(text).substr(eq + 1));
                if (key == "name")
                    mod.name = value;
                else if (key == "version")
                    mod.version = value;
                else if (key == "author")
                    mod.author = value;
                else if (key == "description")
                    mod.description = value;
                else if (key == "enabled")
                    mod.enabled = parseBool(value, mod.enabled);
                else if (key == "priority")
                    mod.priority = std::atoi(value.c_str());
                else if (key == "code")
                    mod.code = value;
            }
            std::error_code ec;
            if (mod.code.empty() && std::filesystem::is_regular_file(mod.dir / "mod.dll", ec))
                mod.code = "mod.dll";
        }

        struct State
        {
            std::filesystem::path root;
            std::vector<ModInfo> mods;
            std::unordered_map<std::string, std::pair<std::filesystem::path, size_t>> files; // key -> (host file, mod index)
            std::unordered_set<std::string> reported;
            std::mutex mutex;
        };

        State &state()
        {
            static State s;
            static std::once_flag once;
            std::call_once(once, []
            {
                State &out = s;
                out.root = exeDir() / "mods";
                if (const char *e = std::getenv("PS2X_MODS"); e && *e)
                    out.root = (e[0] == '0' && e[1] == 0) ? std::filesystem::path() : std::filesystem::path(e);
                std::error_code ec;
                if (out.root.empty() || !std::filesystem::is_directory(out.root, ec))
                    return;

                for (const auto &entry : std::filesystem::directory_iterator(out.root, ec))
                {
                    const std::string folder = entry.path().filename().string();
                    if (!entry.is_directory() || folder.empty() || folder[0] == '_' || folder[0] == '.')
                        continue;
                    if (folder == "MultipleSaves") // retired: save slots are built in now (MemoryCard.cpp)
                        continue;
                    ModInfo mod;
                    mod.folder = folder;
                    mod.name = folder;
                    mod.dir = entry.path();
                    readManifest(mod);
                    out.mods.push_back(std::move(mod));
                }
                std::sort(out.mods.begin(), out.mods.end(), [](const ModInfo &a, const ModInfo &b)
                          { return a.priority != b.priority ? a.priority < b.priority : lower(a.folder) < lower(b.folder); });

                for (size_t index = 0; index < out.mods.size(); ++index)
                {
                    ModInfo &mod = out.mods[index];
                    const std::filesystem::path filesDir = mod.dir / "files";
                    if (std::filesystem::is_directory(filesDir, ec))
                    {
                        for (const auto &entry : std::filesystem::recursive_directory_iterator(
                                 filesDir, std::filesystem::directory_options::skip_permission_denied, ec))
                        {
                            if (!entry.is_regular_file())
                                continue;
                            ++mod.fileCount;
                            if (mod.enabled)
                                out.files[pathKey(std::filesystem::relative(entry.path(), filesDir, ec).generic_string())] = {entry.path(), index};
                        }
                    }
                    const std::filesystem::path texDir = mod.dir / "textures";
                    if (std::filesystem::is_directory(texDir, ec))
                    {
                        for (const auto &entry : std::filesystem::directory_iterator(texDir, ec))
                            mod.textureCount += entry.path().extension() == ".bin" ? 1u : 0u;
                    }
                    std::fprintf(stderr, "[modloader] %s %s%s%s (priority %d): %zu files, %zu textures%s%s\n",
                                 mod.enabled ? "loaded" : "disabled", mod.name.c_str(),
                                 mod.version.empty() ? "" : " v", mod.version.c_str(),
                                 mod.priority, mod.fileCount, mod.textureCount,
                                 mod.code.empty() ? "" : ", code ", mod.code.c_str());
                }
                std::fprintf(stderr, "[modloader] %zu mods in %s, %zu disc files replaced\n",
                             out.mods.size(), out.root.string().c_str(), out.files.size());
            });
            return s;
        }
    }

    std::vector<ModInfo> mods()
    {
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        return s.mods;
    }

    std::filesystem::path modsRoot()
    {
        return state().root;
    }

    bool overrideCdPath(std::string_view relativePath, std::filesystem::path &hostPath)
    {
        State &s = state();
        if (s.files.empty())
            return false;
        const std::string key = pathKey(relativePath);
        const auto found = s.files.find(key);
        if (found == s.files.end())
            return false;
        hostPath = found->second.first;
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.reported.insert(key).second)
        {
            ModInfo &mod = s.mods[found->second.second];
            ++mod.overridesUsed;
            std::fprintf(stderr, "[modloader] %s -> %s (%s)\n", key.c_str(), hostPath.string().c_str(), mod.name.c_str());
        }
        return true;
    }

    std::vector<std::filesystem::path> textureDirs()
    {
        std::vector<std::filesystem::path> dirs;
        for (const ModInfo &mod : state().mods)
        {
            if (mod.enabled && mod.textureCount != 0u)
                dirs.push_back(mod.dir / "textures");
        }
        return dirs;
    }
}

// ---------------------------------------------------------------------------------------------
// Code mods (s2r_mod_api.h). Everything here runs on the host thread.
// ---------------------------------------------------------------------------------------------
namespace ps2x::mods
{
    namespace
    {
        struct CodeMod
        {
            size_t index = 0; // into State::mods
            std::string dir;  // UTF-8
            std::string name;
            std::filesystem::path ini;
            std::map<std::string, std::string> values; // mod.ini, lower-case keys
            void *library = nullptr;
            S2RModFrameFn frame = nullptr;
            S2RModUiFn menu = nullptr;
            S2RModUiFn hud = nullptr;
            S2RModShutdownFn shutdown = nullptr;
            S2RModApi api{};
        };

        std::deque<CodeMod> g_codeMods; // a deque: the api structs handed out must not move
        CodeMod *g_current = nullptr;   // the mod whose callback is running (ini_get etc. are per mod)
        uint8_t *g_rdram = nullptr;
        std::string g_folderText;
        std::atomic<bool> g_uiKeyboard{false};
        std::atomic<bool> g_uiMouse{false};

        struct TimedNotice
        {
            std::string text;
            std::chrono::steady_clock::time_point when;
        };
        std::mutex g_noticeMutex;
        std::deque<TimedNotice> g_notices;
        constexpr double kNoticeSeconds = 4.0;

        const std::chrono::steady_clock::time_point g_start = std::chrono::steady_clock::now();

        std::string vformat(const char *fmt, va_list args)
        {
            char buffer[1024];
            const int n = std::vsnprintf(buffer, sizeof(buffer), fmt ? fmt : "", args);
            return n < 0 ? std::string() : std::string(buffer, std::min<size_t>(static_cast<size_t>(n), sizeof(buffer) - 1));
        }

        // UTF-8 text of a path (u8string() is a std::u8string from C++20 on).
        std::string utf8(const std::filesystem::path &path)
        {
            const auto text = path.u8string();
            return std::string(text.begin(), text.end());
        }

        const char *currentName() { return g_current ? g_current->name.c_str() : "mod"; }

        void readValues(CodeMod &mod)
        {
            mod.values.clear();
            std::ifstream in(mod.ini);
            std::string line;
            while (in && std::getline(in, line))
            {
                if (line.rfind("\xEF\xBB\xBF", 0) == 0)
                    line.erase(0, 3);
                const std::string text = trim(line);
                if (text.empty() || text[0] == '#' || text[0] == ';' || text[0] == '[')
                    continue;
                const size_t eq = text.find('=');
                if (eq != std::string::npos)
                    mod.values[lower(trim(std::string_view(text).substr(0, eq)))] = trim(std::string_view(text).substr(eq + 1));
            }
        }

        void apiLog(const char *fmt, ...)
        {
            va_list args;
            va_start(args, fmt);
            const std::string text = vformat(fmt, args);
            va_end(args);
            std::fprintf(stderr, "[mod:%s] %s\n", currentName(), text.c_str());
        }

        void apiNotify(const char *fmt, ...)
        {
            va_list args;
            va_start(args, fmt);
            const std::string text = vformat(fmt, args);
            va_end(args);
            std::fprintf(stderr, "[mod:%s] (notice) %s\n", currentName(), text.c_str());
            std::lock_guard<std::mutex> lock(g_noticeMutex);
            g_notices.push_back({text, std::chrono::steady_clock::now()});
            while (g_notices.size() > 6)
                g_notices.pop_front();
        }

        const char *apiIniGet(const char *key, const char *fallback)
        {
            if (!g_current || !key)
                return fallback;
            const auto found = g_current->values.find(lower(key));
            return found == g_current->values.end() ? fallback : found->second.c_str();
        }

        // Replaces the "key = ..." line (or appends one), keeping the rest of mod.ini as it is.
        int apiIniSet(const char *key, const char *value)
        {
            if (!g_current || !key || trim(key).empty())
                return 0;
            const std::string name = trim(key);
            const std::string wanted = lower(name);
            std::vector<std::string> lines;
            {
                std::ifstream in(g_current->ini);
                std::string line;
                while (in && std::getline(in, line))
                    lines.push_back(line);
            }
            const std::string newLine = name + " = " + (value ? value : "");
            bool replaced = false;
            for (std::string &line : lines)
            {
                std::string text = line;
                if (text.rfind("\xEF\xBB\xBF", 0) == 0)
                    text.erase(0, 3);
                text = trim(text);
                if (text.empty() || text[0] == '#' || text[0] == ';')
                    continue;
                const size_t eq = text.find('=');
                if (eq != std::string::npos && lower(trim(std::string_view(text).substr(0, eq))) == wanted)
                {
                    line = newLine;
                    replaced = true;
                    break;
                }
            }
            if (!replaced)
                lines.push_back(newLine);
            std::ofstream out(g_current->ini, std::ios::trunc);
            for (const std::string &line : lines)
                out << line << "\n";
            if (!out)
                return 0;
            g_current->values[wanted] = value ? value : "";
            return 1;
        }

        uint8_t *ramPointer(uint32_t address, uint32_t size)
        {
            const uint32_t offset = address & 0x1FFFFFFFu; // kseg0/kseg1/uncached mirrors -> physical
            if (!g_rdram || offset >= 0x02000000u || size > 0x02000000u - offset)
                return nullptr;
            return g_rdram + offset;
        }

        int apiRead(uint32_t address, void *out, uint32_t size)
        {
            const uint8_t *src = ramPointer(address, size);
            if (!src || !out)
                return 0;
            std::memcpy(out, src, size);
            return 1;
        }

        int apiWrite(uint32_t address, const void *data, uint32_t size)
        {
            uint8_t *dst = ramPointer(address, size);
            if (!dst || !data)
                return 0;
            std::memcpy(dst, data, size);
            return 1;
        }

        int apiKeyPressed(int key) { return !uiCapturesKeyboard() && IsKeyPressed(key) ? 1 : 0; }
        int apiKeyDown(int key) { return !uiCapturesKeyboard() && IsKeyDown(key) ? 1 : 0; }

        const char *apiMemcardFolder(int port)
        {
            g_folderText = utf8(ps2_stubs::memoryCardFolder(port));
            return g_folderText.c_str();
        }

        int apiSetMemcardFolder(int port, const char *folder)
        {
            std::filesystem::path path;
            if (folder && *folder)
                path = std::filesystem::u8path(folder);
            return ps2_stubs::setMemoryCardFolder(port, path) ? 1 : 0;
        }

        int apiMemcardBusy() { return ps2_stubs::memoryCardBusy() ? 1 : 0; }

        uint64_t apiGameFrames();

        double apiSeconds()
        {
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
        }

        void *openLibrary(const std::filesystem::path &file, std::string &error)
        {
#if defined(_WIN32)
            // Altered search path: DLLs the mod ships next to its own are found too.
            HMODULE module = LoadLibraryExW(file.wstring().c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (!module)
                error = "Windows error " + std::to_string(GetLastError());
            return module;
#else
            void *module = dlopen(file.string().c_str(), RTLD_NOW | RTLD_LOCAL);
            if (!module)
                error = dlerror();
            return module;
#endif
        }

        void *findSymbol(void *library, const char *name)
        {
#if defined(_WIN32)
            return reinterpret_cast<void *>(GetProcAddress(static_cast<HMODULE>(library), name));
#else
            return dlsym(library, name);
#endif
        }

        void closeLibrary(void *library)
        {
#if defined(_WIN32)
            FreeLibrary(static_cast<HMODULE>(library));
#else
            dlclose(library);
#endif
        }

        struct CurrentMod
        {
            explicit CurrentMod(CodeMod &mod) : previous(g_current) { g_current = &mod; }
            ~CurrentMod() { g_current = previous; }
            CodeMod *previous;
        };

        void setCodeStatus(size_t index, const std::string &status)
        {
            State &s = state();
            std::lock_guard<std::mutex> lock(s.mutex);
            s.mods[index].codeStatus = status;
        }
    }
}

extern std::atomic<uint64_t> g_ps2xGameFrameCount;

namespace ps2x::mods
{
    namespace
    {
        uint64_t apiGameFrames()
        {
            return g_ps2xGameFrameCount.load(std::memory_order_relaxed);
        }
    }

    void startCodeMods(uint8_t *rdram)
    {
        static bool started = false;
        if (started)
            return;
        started = true;
        g_rdram = rdram;
        State &s = state();
        for (size_t index = 0; index < s.mods.size(); ++index)
        {
            const ModInfo &info = s.mods[index];
            if (info.code.empty())
                continue;
            if (!info.enabled)
            {
                setCodeStatus(index, "off");
                continue;
            }
            const std::filesystem::path file = info.dir / std::filesystem::u8path(info.code);
            std::string error;
            void *library = openLibrary(file, error);
            if (!library)
            {
                std::fprintf(stderr, "[modloader] %s: cannot load %s (%s)\n", info.name.c_str(), file.string().c_str(), error.c_str());
                setCodeStatus(index, "not loaded: " + error);
                continue;
            }
            const auto init = reinterpret_cast<S2RModInitFn>(findSymbol(library, "S2R_ModInit"));
            if (!init)
            {
                std::fprintf(stderr, "[modloader] %s: %s has no S2R_ModInit\n", info.name.c_str(), info.code.c_str());
                setCodeStatus(index, "not loaded: no S2R_ModInit");
                closeLibrary(library);
                continue;
            }

            CodeMod &mod = g_codeMods.emplace_back();
            mod.index = index;
            mod.dir = utf8(info.dir);
            mod.name = info.name;
            mod.ini = info.dir / "mod.ini";
            mod.library = library;
            mod.frame = reinterpret_cast<S2RModFrameFn>(findSymbol(library, "S2R_ModFrame"));
            mod.menu = reinterpret_cast<S2RModUiFn>(findSymbol(library, "S2R_ModMenu"));
            mod.hud = reinterpret_cast<S2RModUiFn>(findSymbol(library, "S2R_ModHud"));
            mod.shutdown = reinterpret_cast<S2RModShutdownFn>(findSymbol(library, "S2R_ModShutdown"));
            readValues(mod);

            S2RModApi &api = mod.api;
            api.version = S2R_MOD_API_VERSION;
            api.size = sizeof(S2RModApi);
            api.mod_dir = mod.dir.c_str();
            api.mod_name = mod.name.c_str();
            api.log = apiLog;
            api.ini_get = apiIniGet;
            api.ini_set = apiIniSet;
            api.ram = rdram;
            api.ram_size = 0x02000000u;
            api.read = apiRead;
            api.write = apiWrite;
            api.key_pressed = apiKeyPressed;
            api.key_down = apiKeyDown;
            api.memcard_folder = apiMemcardFolder;
            api.set_memcard_folder = apiSetMemcardFolder;
            api.memcard_busy = apiMemcardBusy;
            api.game_frames = apiGameFrames;
            api.seconds = apiSeconds;
            api.notify = apiNotify;

            int result = 0;
            {
                CurrentMod current(mod);
                result = init(&mod.api);
            }
            if (result != 0)
            {
                std::fprintf(stderr, "[modloader] %s: S2R_ModInit returned %d, unloaded\n", info.name.c_str(), result);
                setCodeStatus(index, "not loaded: S2R_ModInit returned " + std::to_string(result));
                closeLibrary(library);
                g_codeMods.pop_back();
                continue;
            }
            std::fprintf(stderr, "[modloader] %s: code mod %s running\n", info.name.c_str(), info.code.c_str());
            setCodeStatus(index, "running");
        }
    }

    void frameCodeMods()
    {
        for (CodeMod &mod : g_codeMods)
        {
            if (mod.frame)
            {
                CurrentMod current(mod);
                mod.frame();
            }
        }
    }

    bool codeModsHaveUi()
    {
        if (!g_codeMods.empty())
            return true; // the Mods menu lists every code mod
        std::lock_guard<std::mutex> lock(g_noticeMutex);
        return !g_notices.empty();
    }

    void drawCodeModsMenu(const S2RUi &ui)
    {
        if (g_codeMods.empty())
        {
            ui.text_wrapped("No code mods are running. A code mod is a mod folder with a mod.dll in it; "
                            "mods/_sdk has examples.");
            return;
        }
        int id = 0;
        for (CodeMod &mod : g_codeMods)
        {
            ui.push_id(id++);
            if (ui.collapsing_header(mod.name.c_str()))
            {
                if (mod.menu)
                {
                    CurrentMod current(mod);
                    mod.menu(&ui);
                }
                else
                {
                    ui.text_disabled("This mod has no settings here.");
                }
            }
            ui.pop_id();
        }
    }

    void drawCodeModsHud(const S2RUi &ui)
    {
        for (CodeMod &mod : g_codeMods)
        {
            if (mod.hud)
            {
                CurrentMod current(mod);
                mod.hud(&ui);
            }
        }
    }

    void shutdownCodeMods()
    {
        for (CodeMod &mod : g_codeMods)
        {
            if (mod.shutdown)
            {
                CurrentMod current(mod);
                mod.shutdown();
            }
        }
        // The DLLs stay loaded: the process is ending, and a mod may have left a thread running.
        g_codeMods.clear();
    }

    std::vector<Notice> notices()
    {
        std::vector<Notice> out;
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(g_noticeMutex);
        while (!g_notices.empty() && std::chrono::duration<double>(now - g_notices.front().when).count() > kNoticeSeconds)
            g_notices.pop_front();
        for (const TimedNotice &notice : g_notices)
            out.push_back({notice.text, std::chrono::duration<double>(now - notice.when).count()});
        return out;
    }

    void setUiCapture(bool keyboard, bool mouse)
    {
        g_uiKeyboard.store(keyboard, std::memory_order_relaxed);
        g_uiMouse.store(mouse, std::memory_order_relaxed);
    }

    bool uiCapturesKeyboard() { return g_uiKeyboard.load(std::memory_order_relaxed); }
    bool uiCapturesMouse() { return g_uiMouse.load(std::memory_order_relaxed); }
}
