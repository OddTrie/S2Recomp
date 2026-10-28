#include "ps2_runtime.h"
#include "games_database.h"
#if defined(PS2X_ENABLE_DEBUG_UI) && !defined(PLATFORM_VITA)
#include "ps2_debug_panel.h"
#endif

#ifdef _DEBUG
#include "ps2_log.h"
#endif

#include <iostream>
#include <string>
#include <filesystem>
#include <exception>
#include <algorithm>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <mutex>
#include <vector>
#include "runtime/ee_scheduler.h"

#if defined(__ANDROID__)
#include <android/log.h>
#include <unistd.h>
#include <thread>
#include <cstdio>
#include <cstring>
#endif

extern std::atomic<uint64_t> g_ps2xGameFrameCount; // EeScheduler.cpp: page flips (VBlanks with a new display buffer)
extern std::atomic<uint64_t> g_ps2xGsDrawCount;    // gs_frontend.cpp: primitives drawn
void ps2xStartSampler();                           // ps2_sampler.cpp: PS2X_PROF sampling profiler
int ps2xVuReplay(const char *path, int repeat);    // ps2_vu1_replay.cpp: --vu-replay <file> [repeat]
int ps2xVuGenerate(const char *recording, const char *outDir); // ps2_vu1_compiled.cpp: --vu-gen <file.vur> <dir>
void ps2xStartHostWatch(uint8_t *rdram);            // ps2_sampler.cpp: PS2X_HOST_WATCH write watchpoint
int ps2xGsReplay(const char *path, int bands, int passes); // gs_record_replay.cpp: --gs-replay <file> <bands> [passes]
int ps2xGsReplayCheck(const char *path, int bands, int rounds); // gs_record_replay.cpp: --gs-replay-check <file> <bands> [rounds]
int ps2xGsStats(const char *path); // gs_record_replay.cpp: --gs-stats <file>
int ps2xGsGpuCompare(const char *path, int every, int pngEvery, const char *outDir); // gs_record_replay.cpp
int ps2xVuRunDump(const char *path); // ps2_vu1_replay.cpp: --vu-run <vu1_run_*.bin>

namespace
{
#if defined(__ANDROID__)
    int g_logcatPipeFds[2]{-1, -1};
    std::thread g_logcatThread;

    void stopLogcatRedirect()
    {
        std::fflush(stdout);
        std::fflush(stderr);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        if (g_logcatPipeFds[1] >= 0)
        {
            close(g_logcatPipeFds[1]);
            g_logcatPipeFds[1] = -1;
        }
        if (g_logcatThread.joinable())
        {
            g_logcatThread.join();
        }
    }

    void redirectStdioToLogcat()
    {
        if (pipe(g_logcatPipeFds) != 0)
        {
            return;
        }

        setvbuf(stdout, nullptr, _IOLBF, 0);
        setvbuf(stderr, nullptr, _IONBF, 0);
        dup2(g_logcatPipeFds[1], STDOUT_FILENO);
        dup2(g_logcatPipeFds[1], STDERR_FILENO);

        g_logcatThread = std::thread([]()
                                     {
                                         FILE *reader = fdopen(g_logcatPipeFds[0], "r");
                                         if (!reader)
                                         {
                                             return;
                                         }
                                         char line[1024];
                                         while (fgets(line, sizeof(line), reader))
                                         {
                                             size_t len = std::strlen(line);
                                             if (len > 0 && line[len - 1] == '\n')
                                             {
                                                 line[len - 1] = '\0';
                                             }
                                             __android_log_write(ANDROID_LOG_INFO, "ps2x", line);
                                         }
                                         fclose(reader);
                                         g_logcatPipeFds[0] = -1;
                                     });
        if (std::atexit(stopLogcatRedirect) != 0)
        {
            close(STDOUT_FILENO);
            close(STDERR_FILENO);
            close(g_logcatPipeFds[1]);
            g_logcatPipeFds[1] = -1;
            if (g_logcatThread.joinable())
            {
                g_logcatThread.join();
            }
        }
    }
#endif

    void setupTerminateLogger() // to help on release build crashs
    {
        std::set_terminate([]()
                           {
                               std::cerr << "[terminate] unhandled exception" << std::endl;
                               const std::exception_ptr ep = std::current_exception();
                               if (ep)
                               {
                                   try
                                   {
                                       std::rethrow_exception(ep);
                                   }
                                   catch (const std::system_error &e)
                                   {
                                       std::cerr << "[terminate] std::system_error code=" << e.code().value()
                                                 << " category=" << e.code().category().name()
                                                 << " message=" << e.what() << std::endl;
                                   }
                                   catch (const std::exception &e)
                                   {
                                       std::cerr << "[terminate] std::exception: " << e.what() << std::endl;
                                   }
                                   catch (...)
                                   {
                                       std::cerr << "[terminate] non-std exception" << std::endl;
                                   }
                               }
                               std::abort(); });
    }


    // Headless diagnostics: PS2X_THREAD_DUMP=<seconds> prints EE thread/kernel state and
    // GS display registers to stderr every N seconds. Works without the ImGui debug UI,
    // so a cloud/CI agent can see where the guest is stuck.
    void startThreadDumpWatchdog(PS2Runtime &runtime)
    {
        const char *env = std::getenv("PS2X_THREAD_DUMP");
        if (!env || !*env)
            return;
        const int seconds = std::max(1, std::atoi(env));
        std::thread([&runtime, seconds]()
                    {
            static const char *kStatus[] = {"RUN", "READY", "WAIT", "WAITSUSP", "SUSP", "DORMANT"};
            static const char *kWait[] = {"-", "sleep", "sema", "evflag", "vsync", "ext", "mpeg"};
            for (int tick = 1;; ++tick)
            {
                std::this_thread::sleep_for(std::chrono::seconds(seconds));
                const EeKernelSnapshot snap = runtime.eeScheduler().snapshot();
                const GSRegisters &gs = runtime.memory().gs();
                std::fprintf(stderr, "[dump t=%ds] threads=%zu running=%d eeCycle=%llu pmode=0x%llx dispfb1=0x%llx display1=0x%llx dispfb2=0x%llx display2=0x%llx vsync=%llu\n",
                             tick * seconds, snap.threads.size(), snap.runningThreadId,
                             static_cast<unsigned long long>(snap.eeCycle),
                             static_cast<unsigned long long>(gs.pmode), static_cast<unsigned long long>(gs.dispfb1),
                             static_cast<unsigned long long>(gs.display1), static_cast<unsigned long long>(gs.dispfb2),
                             static_cast<unsigned long long>(gs.display2),
                             static_cast<unsigned long long>(gs.vsyncTick.load()));
                {
                    std::string q;
                    for (int id : snap.readyQueued)
                        q += " " + std::to_string(id);
                    std::fprintf(stderr, "  readyQueue:[%s ] pendingInvocations=%zu\n", q.c_str(), snap.pendingInvocations);
                    for (const std::string &line : snap.invocationThreads)
                        std::fprintf(stderr, "  invocationThread %s\n", line.c_str());
                }
                for (const EeThreadSnapshot &t : snap.threads)
                {
                    const unsigned st = static_cast<unsigned>(t.status);
                    const unsigned wr = static_cast<unsigned>(t.waitReason);
                    std::fprintf(stderr, "  th%-3d %-8s wait=%-6s id=%-3d pc=0x%08x ra=0x%08x sp=0x%08x entry=0x%08x prio=%d/%d\n",
                                 t.id, st < 6 ? kStatus[st] : "?", wr < 7 ? kWait[wr] : "?", t.waitId,
                                 t.pc, t.ra, t.sp, t.entry, t.currentPriority, t.initialPriority);
                    // Heuristic backtrace: code-looking words on the guest stack.
                    const uint8_t *rdram = runtime.memory().getRDRAM();
                    if (rdram && t.status != EeThreadStatus::Dormant && t.sp)
                    {
                        std::string bt;
                        int found = 0;
                        for (uint32_t off = 0; off < 0x400u && found < 12; off += 4u)
                        {
                            const uint32_t a = (t.sp + off) & 0x01FFFFFCu;
                            uint32_t w = 0;
                            std::memcpy(&w, rdram + a, 4);
                            if (w >= 0x00100008u && w < 0x00518648u && (w & 3u) == 0u)
                            {
                                char b[24];
                                std::snprintf(b, sizeof(b), " %x@+%x", w, off);
                                bt += b;
                                ++found;
                            }
                        }
                        std::fprintf(stderr, "        stack:%s\n", bt.c_str());
                    }
                }
                {
                    const ps2x::iop::DebugSnapshot iop = runtime.iopDebugSnapshot();
                    std::fprintf(stderr, "  iop: cycles=%llu modules=%u threads=%u rpcServers=%u\n",
                                 static_cast<unsigned long long>(iop.emulatorCycles), iop.emulatorLoadedModules,
                                 iop.emulatorThreads, iop.emulatorRpcServers);
                    for (const std::string &line : iop.diagnostics)
                        if (line.rfind("iop th", 0) == 0)
                            std::fprintf(stderr, "    %s\n", line.c_str());
                }
                // PS2X_PEEK=0xADDR[,0xADDR...] dumps 8 words of EE memory at each address.
                if (const char *peek = std::getenv("PS2X_PEEK"))
                {
                    const uint8_t *ram = runtime.memory().getRDRAM();
                    std::string list(peek);
                    size_t pos = 0;
                    while (ram && pos < list.size())
                    {
                        const size_t comma = list.find(',', pos);
                        const uint32_t addr = static_cast<uint32_t>(std::strtoul(list.substr(pos, comma - pos).c_str(), nullptr, 0)) & 0x01FFFFF0u;
                        uint32_t w[8];
                        std::memcpy(w, ram + addr, sizeof(w));
                        std::fprintf(stderr, "  peek 0x%08x: %08x %08x %08x %08x %08x %08x %08x %08x\n", addr, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
                        if (comma == std::string::npos)
                            break;
                        pos = comma + 1;
                    }
                }
                for (const EeSemaphoreSnapshot &sema : snap.semaphores)
                    if (sema.waiters)
                        std::fprintf(stderr, "  sema%-3d count=%d/%d waiters=%u\n", sema.id, sema.count, sema.maxCount, sema.waiters);
                std::fflush(stderr);
            } })
            .detach();
    }


    // -----------------------------------------------------------------------
    // Boot status monitor: every 10 s decide whether the game is drawing, starting up,
    // stuck (nothing at all changes) or possibly looping (threads busy, nothing else
    // changes). Prints one "[status]" line, appends it to status_log.txt next to the
    // game files and shows it in the window title. Turn off with PS2X_STATUS=0.
    // -----------------------------------------------------------------------
    std::mutex g_statusMutex;
    std::string g_statusText;
    bool g_statusDirty = false;

    extern "C" void SetWindowTitle(const char *title); // raylib (kept out of headers on purpose)

    struct StatusUiHooks
    {
#if defined(PS2X_ENABLE_DEBUG_UI) && !defined(PLATFORM_VITA)
        PS2DebugPanel *panel = nullptr;
#endif
        std::string baseTitle;
    };

    extern "C" int GetFPS(void); // raylib
}
const char *ps2PadMouseHint(); // ps2_pad.cpp
namespace
{

    // Once per second: game FPS (VBlanks in which the GS drew something), emulated VSync rate
    // (60/s is full speed) and the host window's FPS.
    std::string g_fpsText;

    bool updateFpsText(PS2Runtime &runtime)
    {
        using clock = std::chrono::steady_clock;
        static auto lastTime = clock::now();
        static uint64_t lastVsync = runtime.memory().gs().vsyncTick.load(std::memory_order_relaxed);
        static uint64_t lastFrames = g_ps2xGameFrameCount.load(std::memory_order_relaxed);

        const auto now = clock::now();
        const double seconds = std::chrono::duration<double>(now - lastTime).count();
        if (seconds < 1.0)
            return false;

        const uint64_t vsync = runtime.memory().gs().vsyncTick.load(std::memory_order_relaxed);
        const uint64_t frames = g_ps2xGameFrameCount.load(std::memory_order_relaxed);
        const double vsyncRate = static_cast<double>(vsync - lastVsync) / seconds;
        const double frameRate = static_cast<double>(frames - lastFrames) / seconds;
        lastTime = now;
        lastVsync = vsync;
        lastFrames = frames;

        char text[128];
        std::snprintf(text, sizeof(text), "FPS %.0f  |  VSync %.0f/60 (%.0f%% speed)  |  window %d fps",
                      frameRate, vsyncRate, vsyncRate * 100.0 / 60.0, GetFPS());
        g_fpsText = text;
        // PS2X_FPS_LOG=1: the same per-second line on the console (the [status] line averages 10 s, hiding dips).
        static const bool logFps = [] { const char *e = std::getenv("PS2X_FPS_LOG"); return e && *e && *e != '0'; }();
        if (logFps)
        {
            static const auto start = now;
            std::fprintf(stderr, "[fps] %6.1f s  %s\n", std::chrono::duration<double>(now - start).count(), text);
        }
        return true;
    }

    void applyStatusTitle(PS2Runtime &runtime, StatusUiHooks &hooks)
    {
        bool changed = updateFpsText(runtime);
        std::string text;
        {
            std::lock_guard<std::mutex> lock(g_statusMutex);
            changed = changed || g_statusDirty;
            g_statusDirty = false;
            text = g_statusText;
        }
        if (!changed)
            return;
        std::string title = hooks.baseTitle;
        if (!g_fpsText.empty())
            title += "  |  " + g_fpsText;
        if (const char *hint = ps2PadMouseHint(); *hint)
            title += std::string("  |  ") + hint;
        if (!text.empty())
            title += "  |  " + text;
        SetWindowTitle(title.c_str());
    }

    uint64_t hashBytes(const std::vector<uint8_t> &bytes)
    {
        uint64_t h = 1469598103934665603ull;
        for (size_t i = 0; i < bytes.size(); ++i)
        {
            h ^= bytes[i];
            h *= 1099511628211ull;
        }
        return h;
    }

    void startStatusMonitor(PS2Runtime &runtime)
    {
        const char *env = std::getenv("PS2X_STATUS");
        if (env && *env == '0')
            return;
        std::thread([&runtime]()
                    {
            using clock = std::chrono::steady_clock;
            constexpr int kIntervalSeconds = 10;
            const auto start = clock::now();
            std::string lastWaitSignature;
            uint64_t lastFrameHash = 0;
            bool everDrew = false;
            int unchangedSeconds = 0;
            int busyUnchangedSeconds = 0;
            std::FILE *log = std::fopen("status_log.txt", "w");
            for (;;)
            {
                std::this_thread::sleep_for(std::chrono::seconds(kIntervalSeconds));
                const int elapsed = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(clock::now() - start).count());

                {
                    // PS2X_VRAM_DUMP=<sec>[,<sec>...]: write the raw 4 MB GS memory to vram_<elapsed>.bin
                    // at the first status tick at or after each time (racy snapshot, for offline inspection).
                    static std::vector<int> dumpTimes = [] {
                        std::vector<int> t;
                        if (const char *e = std::getenv("PS2X_VRAM_DUMP"))
                            for (const char *p = e; *p;) { t.push_back(std::atoi(p)); while (*p && *p != ',') ++p; if (*p) ++p; }
                        return t;
                    }();
                    for (int &t : dumpTimes)
                    {
                        if (t >= 0 && elapsed >= t)
                        {
                            t = -1;
                            if (const uint8_t *vram = runtime.memory().getGSVRAM())
                                if (std::FILE *f = std::fopen(("vram_" + std::to_string(elapsed) + ".bin").c_str(), "wb"))
                                {
                                    std::fwrite(vram, 1, 4u * 1024u * 1024u, f);
                                    std::fclose(f);
                                }
                        }
                    }
                }
                // Picture: hash of what is on screen, and whether it is all black.
                std::vector<uint8_t> pixels;
                uint32_t w = 0, h = 0;
                bool black = true;
                uint64_t frameHash = 0;
                if (runtime.gs().copyLatchedHostPresentationFrame(pixels, w, h) && !pixels.empty())
                {
                    // A few stray pixels (seen in Sims 2) still count as black: need >0.5% lit.
                    size_t lit = 0;
                    const size_t total = pixels.size() / 4;
                    for (size_t i = 0; i + 3 < pixels.size(); i += 4)
                        if (pixels[i] > 8 || pixels[i + 1] > 8 || pixels[i + 2] > 8)
                            ++lit;
                    black = lit * 200 < total;
                    frameHash = hashBytes(pixels);
                    if (std::getenv("PS2X_STATUS_DUMP"))
                    {
                        if (std::FILE *f = std::fopen(("status_frame_" + std::to_string(elapsed) + ".rgba").c_str(), "wb"))
                        {
                            std::fprintf(f, "%u %u\n", w, h);
                            std::fwrite(pixels.data(), 1, pixels.size(), f);
                            std::fclose(f);
                        }
                    }
                }
                const bool frameChanged = frameHash != lastFrameHash;
                lastFrameHash = frameHash;
                if (!black)
                    everDrew = true;

                // Threads: what everyone is waiting on (running threads count as "busy").
                const EeKernelSnapshot snap = runtime.eeScheduler().snapshot();
                std::string waitSignature;
                std::string waitingList;
                std::string busyList;
                int alive = 0;
                for (const EeThreadSnapshot &t : snap.threads)
                {
                    if (t.status == EeThreadStatus::Dormant)
                        continue;
                    ++alive;
                    char item[64];
                    if (t.status == EeThreadStatus::Running || t.status == EeThreadStatus::Ready)
                    {
                        std::snprintf(item, sizeof(item), " th%d@0x%x", t.id, t.pc);
                        busyList += item;
                        waitSignature += "B" + std::to_string(t.id) + ";";
                    }
                    else
                    {
                        std::snprintf(item, sizeof(item), "%d:%u:%d:%x;", t.id, static_cast<unsigned>(t.waitReason), t.waitId, t.pc);
                        waitSignature += item;
                        static const char *kWait[] = {"-", "sleep", "sema", "evflag", "vsync", "ext", "mpeg"};
                        const unsigned wr = static_cast<unsigned>(t.waitReason);
                        std::snprintf(item, sizeof(item), " th%d(%s %d)", t.id, wr < 7 ? kWait[wr] : "?", t.waitId);
                        waitingList += item;
                    }
                }
                const ps2x::iop::DebugSnapshot iop = runtime.iopDebugSnapshot();
                for (const std::string &line : iop.diagnostics)
                    if (line.rfind("iop th", 0) == 0 && line.find("READY") == std::string::npos && line.find(" RUN ") == std::string::npos)
                        waitSignature += line.substr(0, 24) + ";";

                const bool threadsChanged = waitSignature != lastWaitSignature;
                lastWaitSignature = waitSignature;
                const bool anyBusy = !busyList.empty();

                if (frameChanged || threadsChanged)
                    unchangedSeconds = 0;
                else
                    unchangedSeconds += kIntervalSeconds;
                if (anyBusy && !frameChanged && !threadsChanged)
                    busyUnchangedSeconds += kIntervalSeconds;
                else
                    busyUnchangedSeconds = 0;

                char text[512];
                if (!black && frameChanged)
                    std::snprintf(text, sizeof(text), "RUNNING - drawing frames");
                else if (!black && unchangedSeconds < 60)
                    std::snprintf(text, sizeof(text), "RUNNING - showing a still picture");
                else if (anyBusy && busyUnchangedSeconds >= 60)
                    std::snprintf(text, sizeof(text), "LOOPING? - threads busy but nothing else changed for %ds (busy:%s)", busyUnchangedSeconds, busyList.c_str());
                else if (!anyBusy && unchangedSeconds >= 30)
                    std::snprintf(text, sizeof(text), "STUCK - nothing changed for %ds (waiting:%s)", unchangedSeconds, waitingList.c_str());
                else if (!everDrew && elapsed >= 90)
                    std::snprintf(text, sizeof(text), "NO PICTURE - game is alive (%d threads) but nothing on screen after %ds: graphics problem, not a hang", alive, elapsed);
                else
                    std::snprintf(text, sizeof(text), "RUNNING - %s, %d game threads", everDrew ? "picture went black" : "starting up, no picture yet", alive);

                char line[640];
                std::snprintf(line, sizeof(line), "[status] %02d:%02d %s  [%s, gs draws %llu, game frames %llu]", elapsed / 60, elapsed % 60, text,
                              g_fpsText.c_str(), static_cast<unsigned long long>(g_ps2xGsDrawCount.load(std::memory_order_relaxed)),
                              static_cast<unsigned long long>(g_ps2xGameFrameCount.load(std::memory_order_relaxed)));
                std::fprintf(stdout, "%s\n", line);
                std::fflush(stdout);
                if (log)
                {
                    std::fprintf(log, "%s\n", line);
                    std::fflush(log);
                }
                {
                    std::lock_guard<std::mutex> lock(g_statusMutex);
                    g_statusText = text;
                    g_statusDirty = true;
                }
            } })
            .detach();
    }

    std::filesystem::path getExecutablePath(int argc, char *argv[])
    {
        if (argc >= 2 && argv[1] && argv[1][0] != '\0')
        {
            std::cout << "Using argv boot path" << std::endl;
            return std::filesystem::path(argv[1]);
        }
#if defined(PS2X_DEFAULT_BOOT_ELF)
        std::cout << "Using default boot file" << std::endl;
        const std::filesystem::path configuredPath = std::filesystem::path(PS2X_DEFAULT_BOOT_ELF);
#if defined(PLATFORM_VITA)
        return configuredPath;
#endif
        if (configuredPath.is_absolute())
        {
            return configuredPath;
        }
        return (std::filesystem::current_path() / configuredPath).lexically_normal();
#else
        throw std::runtime_error("Unable to determine executable path. Pass the guest ELF as argv[1] or define PS2X_DEFAULT_BOOT_ELF.");
#endif
    }
}

int main(int argc, char *argv[])
{
#if defined(__ANDROID__)
    redirectStdioToLogcat();
#endif
    setupTerminateLogger();
    ps2xStartSampler();
    if (argc >= 3 && std::strcmp(argv[1], "--vu-run") == 0)
        return ps2xVuRunDump(argv[2]);
    if (argc >= 4 && std::strcmp(argv[1], "--gs-replay") == 0)
        return ps2xGsReplay(argv[2], std::atoi(argv[3]), argc >= 5 ? std::max(1, std::atoi(argv[4])) : 1);
    if (argc >= 4 && std::strcmp(argv[1], "--gs-replay-check") == 0)
        return ps2xGsReplayCheck(argv[2], std::atoi(argv[3]), argc >= 5 ? std::max(1, std::atoi(argv[4])) : 4);
    if (argc >= 3 && std::strcmp(argv[1], "--gs-gpu-compare") == 0)
        return ps2xGsGpuCompare(argv[2], argc >= 4 ? std::atoi(argv[3]) : 2000, argc >= 5 ? std::atoi(argv[4]) : 25, argc >= 6 ? argv[5] : ".");
    if (argc >= 3 && std::strcmp(argv[1], "--gs-stats") == 0)
        return ps2xGsStats(argv[2]);
    if (argc >= 3 && std::strcmp(argv[1], "--vu-replay") == 0)
        return ps2xVuReplay(argv[2], argc >= 4 ? std::max(1, std::atoi(argv[3])) : 1);
    if (argc >= 4 && std::strcmp(argv[1], "--vu-gen") == 0)
        return ps2xVuGenerate(argv[2], argv[3]);

    try
    {
        std::filesystem::path pathObj = getExecutablePath(argc, argv);

        std::string filePathStr = pathObj.string();
        std::string elfName = pathObj.filename().string();

        // Project name only: the game's own title is a trademark, so it is not shown.
        std::string windowTitle = "S2Recomp | " + elfName;

        PS2Runtime runtime;
        // UI hooks: the debug panel (when built) plus the boot-status window title.
        static StatusUiHooks uiHooks;
        uiHooks.baseTitle = windowTitle;
#if defined(PS2X_ENABLE_DEBUG_UI) && !defined(PLATFORM_VITA)
        // This hook is to prevent leak rlimgui deps to recompiler etc
        PS2DebugPanel debugPanel;
        uiHooks.panel = &debugPanel;
#endif
        runtime.setDebugUiCallbacks(
            [](PS2Runtime &rt, void *userData)
            {
                (void)rt;
                (void)userData;
#if defined(PS2X_ENABLE_DEBUG_UI) && !defined(PLATFORM_VITA)
                static_cast<StatusUiHooks *>(userData)->panel->initialize();
#endif
            },
            [](PS2Runtime &rt, void *userData)
            {
                (void)rt;
                auto *hooks = static_cast<StatusUiHooks *>(userData);
#if defined(PS2X_ENABLE_DEBUG_UI) && !defined(PLATFORM_VITA)
                hooks->panel->draw(rt);
#endif
                applyStatusTitle(rt, *hooks);
            },
            [](PS2Runtime &rt, void *userData)
            {
                (void)rt;
                (void)userData;
#if defined(PS2X_ENABLE_DEBUG_UI) && !defined(PLATFORM_VITA)
                static_cast<StatusUiHooks *>(userData)->panel->shutdown();
#endif
            },
            &uiHooks);
        if (!runtime.initialize(windowTitle.c_str()))
        {
            std::cerr << "Failed to initialize PS2 runtime" << std::endl;
            return 1;
        }
        ps2xStartHostWatch(runtime.memory().getRDRAM());

        if (!runtime.loadELF(filePathStr))
        {
            std::cerr << "Failed to load ELF file: " << filePathStr << std::endl;
            return 1;
        }

        startThreadDumpWatchdog(runtime);
        startStatusMonitor(runtime);
        runtime.run();

#ifdef _DEBUG
        ps2_log::print_saved_location();
#endif
        std::cout.flush();
        std::cerr.flush();
        std::_Exit(0);
    }
    catch (const std::exception &e)
    {
        std::cerr << "[main] fatal exception: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "[main] fatal exception: unknown" << std::endl;
    }

    std::cout.flush();
    std::cerr.flush();
    std::_Exit(1);
}
