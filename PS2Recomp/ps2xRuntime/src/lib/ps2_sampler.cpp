// PS2X_PROF=<startSec>[,<seconds>]: a tiny sampling profiler (Windows only). After <startSec> it samples
// the instruction pointer of every other thread about 1000 times a second for <seconds> (default 20)
// and writes prof_samples.txt ("tid rva count", RVAs relative to the exe). Resolve the RVAs with the
// linker map file (ps2EntryRunner.map), e.g. with tools/prof_report.py.
// PS2X_PROF=key[,<seconds>]: sampling starts when F9 is pressed and ends at the next F9 press (or after
// <seconds>, default 60), so a scene that has to be played to can be captured. Each capture rewrites
// prof_samples.txt (the previous one becomes prof_samples_prev.txt). Nothing is allocated while a thread is suspended, so a sampled
// thread that holds the heap lock cannot deadlock the sampler.
// High-rate mode (E-cores, where the 1 ms sleep gives ~80 samples/s on a busy machine):
// PS2X_PROF_THREAD=<name prefix> samples only those threads ("main" = the thread that started the sampler, e.g. the
// --vu-replay thread); PS2X_PROF_US=<n> samples every n microseconds with a busy wait instead of Sleep(1);
// PS2X_PROF_CPU=<logical cpu> runs the sampler on that processor only and keeps every other thread off it (process
// default CPU sets), and ps2xHostCpu() does not count it, so `start /affinity` <game mask + that cpu> profiles the
// game as if the mask had no extra processor.
// PS2X_THREAD_CPU=<startSec>[,<seconds>]: no suspending; from <startSec> on, for every <seconds> window (default
// 10) until exit, prints each named thread's CPU time as a share of one logical processor ("[threads] 60-70 s: ...
// Vif1Worker 87% ..."), plus the process total and the usable logical processors: which thread saturates on a
// small machine.
#include "runtime/ps2_host_cpu.h"
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <timeapi.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "winmm.lib")

// ---------------------------------------------------------------------------------------------
// PS2X_HOST_WATCH=<guest addr>[,<startSec>]: hardware write watchpoint (x64 debug register DR0)
// on the 8 host bytes backing that guest RAM address, set on every thread. Every write is kept
// in a ring (value, host backtrace as exe RVAs); the ring is written to watch_log.txt each second.
// Catches writers of any kind (recompiled code, HLE, memcpy) with no slowdown elsewhere.
// ---------------------------------------------------------------------------------------------
void ps2xGuestWriteTag(int &thread, int &invocations, uint32_t &pc, uint32_t &sp, uint32_t &ra); // EeScheduler.cpp

namespace
{
    struct WatchHit
    {
        uint64_t seq;
        uint64_t value;
        DWORD tid;
        USHORT frames;
        int guestThread, guestInvocations;
        uint32_t guestPc, guestSp, guestRa;
        void *trace[20];
    };
    constexpr uint32_t kWatchRing = 256;
    WatchHit g_watchRing[kWatchRing];
    volatile LONG64 g_watchCount = 0;
    volatile uintptr_t g_watchHost = 0;

    LONG CALLBACK watchHandler(PEXCEPTION_POINTERS info)
    {
        if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || g_watchHost == 0)
            return EXCEPTION_CONTINUE_SEARCH;
        if ((info->ContextRecord->Dr6 & 1u) == 0u)
            return EXCEPTION_CONTINUE_SEARCH;
        info->ContextRecord->Dr6 = 0;
        const LONG64 seq = InterlockedIncrement64(&g_watchCount);
        WatchHit &hit = g_watchRing[static_cast<uint64_t>(seq) % kWatchRing];
        hit.seq = static_cast<uint64_t>(seq);
        hit.value = *reinterpret_cast<volatile uint64_t *>(g_watchHost);
        hit.tid = GetCurrentThreadId();
        ps2xGuestWriteTag(hit.guestThread, hit.guestInvocations, hit.guestPc, hit.guestSp, hit.guestRa);
        hit.frames = CaptureStackBackTrace(0, 20, hit.trace, nullptr);
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    void armThread(HANDLE thread, uintptr_t address)
    {
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (!GetThreadContext(thread, &ctx))
            return;
        if (ctx.Dr0 == address && (ctx.Dr7 & 1u))
            return;
        ctx.Dr0 = address;
        // L0 enable, R/W0 = 01 (write), LEN0 = 10 (8 bytes)
        ctx.Dr7 = (ctx.Dr7 & ~0x000F0003ull) | 0x1ull | (0x1ull << 16) | (0x2ull << 18);
        SetThreadContext(thread, &ctx);
    }
}

void ps2xStartHostWatch(uint8_t *rdram)
{
    const char *env = std::getenv("PS2X_HOST_WATCH");
    if (!env || !*env || !rdram)
        return;
    const uint32_t guest = static_cast<uint32_t>(std::strtoul(env, nullptr, 0)) & 0x01FFFFF8u;
    const char *comma = std::strchr(env, ',');
    const double startSec = comma ? std::atof(comma + 1) : 0.0;
    const uintptr_t host = reinterpret_cast<uintptr_t>(rdram) + guest;
    std::thread([host, startSec, guest]()
                {
        std::this_thread::sleep_for(std::chrono::duration<double>(startSec));
        AddVectoredExceptionHandler(1, watchHandler);
        g_watchHost = host;
        const DWORD self = GetCurrentThreadId();
        const DWORD pid = GetCurrentProcessId();
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        std::fprintf(stderr, "[watch] guest 0x%x (host %p) armed\n", guest, reinterpret_cast<void *>(host));
        for (;;)
        {
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            THREADENTRY32 te{};
            te.dwSize = sizeof(te);
            if (snap != INVALID_HANDLE_VALUE && Thread32First(snap, &te))
            {
                do
                {
                    if (te.th32OwnerProcessID != pid || te.th32ThreadID == self)
                        continue;
                    if (HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, te.th32ThreadID))
                    {
                        if (SuspendThread(h) != static_cast<DWORD>(-1))
                        {
                            armThread(h, host);
                            ResumeThread(h);
                        }
                        CloseHandle(h);
                    }
                } while (Thread32Next(snap, &te));
            }
            if (snap != INVALID_HANDLE_VALUE)
                CloseHandle(snap);

            // Dump the most recent writes (oldest first) so the log survives a crash or kill.
            const uint64_t count = static_cast<uint64_t>(g_watchCount);
            if (std::FILE *f = std::fopen("watch_log.txt", "w"))
            {
                std::fprintf(f, "# guest 0x%x, %llu writes\n", guest, static_cast<unsigned long long>(count));
                const uint64_t first = count > kWatchRing ? count - kWatchRing + 1 : 1;
                for (uint64_t s = first; s <= count; ++s)
                {
                    const WatchHit hit = g_watchRing[s % kWatchRing];
                    if (hit.seq != s)
                        continue;
                    std::fprintf(f, "%llu tid=%lu guest=th%d inv=%d pc=%x sp=%x ra=%x value=%016llx trace=", static_cast<unsigned long long>(hit.seq),
                                 static_cast<unsigned long>(hit.tid), hit.guestThread, hit.guestInvocations, hit.guestPc, hit.guestSp, hit.guestRa,
                                 static_cast<unsigned long long>(hit.value));
                    for (USHORT i = 0; i < hit.frames; ++i)
                        std::fprintf(f, " %llx", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(hit.trace[i]) - base));
                    std::fprintf(f, "\n");
                }
                std::fclose(f);
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        } })
        .detach();
}

// Copies up to `size` bytes of a (suspended) thread's stack starting at rsp, stopping at the end of
// the committed region. Returns the bytes copied.
static size_t copyStack(uintptr_t rsp, uint8_t *out, size_t size)
{
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<void *>(rsp), &info, sizeof(info)) || info.State != MEM_COMMIT)
        return 0;
    const uintptr_t end = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    const size_t n = std::min<size_t>(size, end - rsp);
    std::memcpy(out, reinterpret_cast<const void *>(rsp), n);
    return n;
}

// Name given with ThreadNaming::SetCurrentThreadName ("" if none).
static std::string threadName(HANDLE thread)
{
    using GetThreadDescriptionFn = HRESULT(WINAPI *)(HANDLE, PWSTR *);
    static const auto getThreadDescription = reinterpret_cast<GetThreadDescriptionFn>(
        reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"Kernel32.dll"), "GetThreadDescription")));
    std::string out;
    PWSTR name = nullptr;
    if (getThreadDescription && SUCCEEDED(getThreadDescription(thread, &name)) && name)
    {
        for (const wchar_t *c = name; *c; ++c)
            out.push_back(*c > 0x20 && *c < 0x7F ? static_cast<char>(*c) : '_');
        LocalFree(name);
    }
    return out;
}

// One capture: samples every other thread until `seconds` have passed or stop() returns true, then
// writes prof_samples.txt.
static DWORD g_samplerStarterTid = 0; // the thread that called ps2xStartSampler (PS2X_PROF_THREAD=main)

// PS2X_PROF_CPU: the logical processor reserved for the sampler, -1 if none.
static int profilerCpu()
{
    static const int cpu = [] { const char *e = std::getenv("PS2X_PROF_CPU"); return e && *e ? std::atoi(e) : -1; }();
    return cpu;
}

struct UsableCpuSet { ULONG id; BYTE core; BYTE efficiency; };
static std::vector<UsableCpuSet> usableCpuSets();

// PS2X_PROF_CPU: this thread on that processor alone, every other thread of the process on the rest.
static void pinSamplerThread()
{
    const int cpu = profilerCpu();
    if (cpu < 0)
        return;
    SetThreadAffinityMask(GetCurrentThread(), static_cast<DWORD_PTR>(1) << cpu);
    std::vector<ULONG> others;
    for (const auto &set : usableCpuSets()) // already without the profiler cpu
        others.push_back(set.id);
    if (!others.empty())
        SetProcessDefaultCpuSets(GetCurrentProcess(), others.data(), static_cast<ULONG>(others.size()));
    std::fprintf(stderr, "[prof] sampler on cpu %d, %zu cpu sets for the rest\n", cpu, others.size());
}

template <typename StopFn>
static void sampleThreads(double seconds, StopFn stop)
{
    timeBeginPeriod(1);
    static const char *onlyThread = std::getenv("PS2X_PROF_THREAD");
    static const int intervalUs = [] { const char *e = std::getenv("PS2X_PROF_US"); return e && *e ? std::atoi(e) : 0; }();
    LARGE_INTEGER qpcFreq{};
    QueryPerformanceFrequency(&qpcFreq);
    const DWORD self = GetCurrentThreadId();
    const DWORD pid = GetCurrentProcessId();
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    const uintptr_t imageSize = nt->OptionalHeader.SizeOfImage;

    std::unordered_map<uint64_t, uint32_t> hits;          // (tid << 32) | rva
    std::unordered_map<DWORD, uint32_t> outside, inside; // samples outside / inside the exe
    std::unordered_map<DWORD, std::string> names;
    static uint8_t stackCopy[8192];
    std::unordered_map<uint64_t, uint32_t> waits, waits2; // (tid << 32) | rva of the 1st / 2nd frame in the exe
    static const bool waitsOn = [] { const char *e = std::getenv("PS2X_PROF_WAITS"); return e && *e && *e != '0'; }();
    std::vector<std::pair<DWORD, HANDLE>> threads;
    hits.reserve(1u << 16);

    const auto t0 = std::chrono::steady_clock::now();
    uint32_t iteration = 0;
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < seconds && !stop())
    {
        if ((iteration++ % 200u) == 0u)
        {
            for (auto &t : threads)
                CloseHandle(t.second);
            threads.clear();
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            THREADENTRY32 te{};
            te.dwSize = sizeof(te);
            if (snap != INVALID_HANDLE_VALUE && Thread32First(snap, &te))
            {
                do
                {
                    if (te.th32OwnerProcessID != pid || te.th32ThreadID == self)
                        continue;
                    if (HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID))
                    {
                        if (names.find(te.th32ThreadID) == names.end())
                            names[te.th32ThreadID] = threadName(h);
                        const std::string &name = names[te.th32ThreadID];
                        if (onlyThread && *onlyThread &&
                            !(std::strcmp(onlyThread, "main") == 0 ? te.th32ThreadID == g_samplerStarterTid
                                                                   : name.rfind(onlyThread, 0) == 0))
                        {
                            CloseHandle(h);
                            continue;
                        }
                        threads.push_back({te.th32ThreadID, h});
                    }
                } while (Thread32Next(snap, &te));
            }
            if (snap != INVALID_HANDLE_VALUE)
                CloseHandle(snap);
        }
        for (auto &t : threads)
        {
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_CONTROL;
            if (SuspendThread(t.second) == static_cast<DWORD>(-1))
                continue;
            const BOOL ok = GetThreadContext(t.second, &ctx);
            // PS2X_PROF_WAITS: copy the top of the stack while suspended (a plain copy, no locks:
            // unwinding a suspended thread can deadlock on a lock it holds) and scan it below.
            size_t stackBytes = 0;
            if (ok && waitsOn && !(ctx.Rip >= base && ctx.Rip < base + imageSize))
                stackBytes = copyStack(static_cast<uintptr_t>(ctx.Rsp), stackCopy, sizeof(stackCopy));
            ResumeThread(t.second);
            if (!ok)
                continue;
            const uintptr_t rip = static_cast<uintptr_t>(ctx.Rip);
            if (rip >= base && rip < base + imageSize)
            {
                ++hits[(static_cast<uint64_t>(t.first) << 32) | static_cast<uint32_t>(rip - base)];
                ++inside[t.first];
            }
            else
            {
                ++outside[t.first];
                // Waiting in the OS: the first two values on the stack that point into the exe are
                // (mostly) return addresses of the innermost exe frames: "w" and "v" lines.
                int found = 0;
                for (size_t off = 0; off + 8 <= stackBytes && found < 2; off += 8)
                {
                    uint64_t value = 0;
                    std::memcpy(&value, stackCopy + off, 8);
                    if (value >= base && value < base + imageSize)
                    {
                        ++(found == 0 ? waits : waits2)[(static_cast<uint64_t>(t.first) << 32) | static_cast<uint32_t>(value - base)];
                        ++found;
                    }
                }
            }
        }
        if (intervalUs > 0)
        {
            LARGE_INTEGER now{}, until{};
            QueryPerformanceCounter(&until);
            until.QuadPart += qpcFreq.QuadPart * intervalUs / 1000000;
            do
            {
                YieldProcessor();
                QueryPerformanceCounter(&now);
            } while (now.QuadPart < until.QuadPart);
        }
        else
            Sleep(1);
    }
    timeEndPeriod(1);
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    MoveFileExA("prof_samples.txt", "prof_samples_prev.txt", MOVEFILE_REPLACE_EXISTING);
    if (std::FILE *f = std::fopen("prof_samples.txt", "w"))
    {
        std::fprintf(f, "# seconds %.2f\n", elapsed);
        for (auto &kv : inside)
            std::fprintf(f, "# thread %lu inside=%u outside=%u name=%s\n", static_cast<unsigned long>(kv.first), kv.second,
                         outside[kv.first], names[kv.first].c_str());
        for (auto &kv : outside)
            if (inside.find(kv.first) == inside.end())
                std::fprintf(f, "# thread %lu inside=0 outside=%u name=%s\n", static_cast<unsigned long>(kv.first), kv.second,
                             names[kv.first].c_str());
        for (auto &kv : hits)
            std::fprintf(f, "%lu %x %u\n", static_cast<unsigned long>(kv.first >> 32), static_cast<uint32_t>(kv.first), kv.second);
        for (auto &kv : waits) // PS2X_PROF_WAITS=1: where threads wait (first frame inside the exe)
            std::fprintf(f, "w%lu %x %u\n", static_cast<unsigned long>(kv.first >> 32), static_cast<uint32_t>(kv.first), kv.second);
        for (auto &kv : waits2)
            std::fprintf(f, "v%lu %x %u\n", static_cast<unsigned long>(kv.first >> 32), static_cast<uint32_t>(kv.first), kv.second);
        std::fclose(f);
    }
    for (auto &t : threads)
        CloseHandle(t.second);
    std::fprintf(stderr, "[prof] wrote prof_samples.txt (%.1f s)\n", elapsed);
}

static void startThreadCpuReport();

void ps2xStartSampler()
{
    startThreadCpuReport();
    const char *env = std::getenv("PS2X_PROF");
    if (!env || !*env)
        return;
    g_samplerStarterTid = GetCurrentThreadId();
    const char *comma = std::strchr(env, ',');
    if (std::strncmp(env, "key", 3) == 0)
    {
        const double seconds = comma ? std::atof(comma + 1) : 60.0;
        std::fprintf(stderr, "[prof] press F9 to start sampling, F9 again to stop (at most %.0f s)\n", seconds);
        std::thread([seconds]()
                    {
            // Low bit of GetAsyncKeyState: pressed since the previous call (whichever window has focus).
            const auto f9Pressed = []() { return (GetAsyncKeyState(VK_F9) & 1) != 0; };
            f9Pressed();
            for (;;)
            {
                while (!f9Pressed())
                    Sleep(20);
                std::fprintf(stderr, "[prof] sampling started, F9 to stop\n");
                uint32_t polls = 0;
                sampleThreads(seconds, [&]() { return (++polls % 16u) == 0u && f9Pressed(); });
            } })
            .detach();
        return;
    }
    const double startSec = std::atof(env);
    const double seconds = comma ? std::atof(comma + 1) : 20.0;

    std::thread([startSec, seconds]()
                {
        pinSamplerThread();
        std::this_thread::sleep_for(std::chrono::duration<double>(startSec));
        sampleThreads(seconds, []() { return false; }); })
        .detach();
}

// The CPU sets of the logical processors in the process affinity mask (group 0), so a process started with
// `start /affinity` or Task Manager's affinity sees only those.
static std::vector<UsableCpuSet> usableCpuSets()
{
    std::vector<UsableCpuSet> sets;
    DWORD_PTR processMask = 0, systemMask = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask) || processMask == 0)
        processMask = ~static_cast<DWORD_PTR>(0);
    ULONG length = 0;
    GetSystemCpuSetInformation(nullptr, 0, &length, GetCurrentProcess(), 0);
    std::vector<uint8_t> buffer(length);
    auto *info = reinterpret_cast<SYSTEM_CPU_SET_INFORMATION *>(buffer.data());
    if (length == 0 || !GetSystemCpuSetInformation(info, length, &length, GetCurrentProcess(), 0))
        return sets;
    for (ULONG offset = 0; offset < length;)
    {
        const auto *entry = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION *>(buffer.data() + offset);
        if (entry->Type == CpuSetInformation && entry->CpuSet.Group == 0 &&
            entry->CpuSet.LogicalProcessorIndex < sizeof(DWORD_PTR) * 8 &&
            ((processMask >> entry->CpuSet.LogicalProcessorIndex) & 1u) != 0 &&
            static_cast<int>(entry->CpuSet.LogicalProcessorIndex) != profilerCpu())
            sets.push_back({entry->CpuSet.Id, entry->CpuSet.CoreIndex, entry->CpuSet.EfficiencyClass});
        offset += entry->Size;
    }
    return sets;
}

const Ps2xHostCpu &ps2xHostCpu()
{
    static const Ps2xHostCpu cpu = [] {
        Ps2xHostCpu c;
        const std::vector<UsableCpuSet> sets = usableCpuSets();
        if (sets.empty())
        {
            c.logical = c.physical = c.fast = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
            return c;
        }
        BYTE best = 0;
        for (const UsableCpuSet &set : sets)
            best = std::max(best, set.efficiency);
        std::vector<BYTE> cores, fastCores;
        for (const UsableCpuSet &set : sets)
        {
            if (std::find(cores.begin(), cores.end(), set.core) == cores.end())
                cores.push_back(set.core);
            if (set.efficiency == best && std::find(fastCores.begin(), fastCores.end(), set.core) == fastCores.end())
                fastCores.push_back(set.core);
        }
        c.logical = static_cast<int>(sets.size());
        c.physical = static_cast<int>(cores.size());
        c.fast = static_cast<int>(fastCores.size());
        std::fprintf(stderr, "[cpu] %d logical processors, %d physical cores (%d of the fastest class) usable\n",
                     c.logical, c.physical, c.fast);
        return c;
    }();
    return cpu;
}

// The calling thread (the VIF1/VU1 worker, the frame-rate limit in 3D scenes) gets one whole
// fastest-class physical core (both hyperthreads) and every other thread of the process defaults
// to the remaining logical processors, so no band or spinning feed thread shares the worker's core
// (neighbourhood pan low 23-24 -> 25-26 on a 14900K). On by default with 12+ usable physical cores
// (cores outside the process affinity mask don't count and are never picked); PS2X_VU1_OWN_CORE=0 off,
// =1 on. Timing only.
void ps2xReserveCoreForCurrentThread()
{
    const char *env = std::getenv("PS2X_VU1_OWN_CORE");
    if (env && *env == '0')
        return;
    const bool forced = env && *env && *env != '0';
    const std::vector<UsableCpuSet> sets = usableCpuSets();
    if (sets.size() < 4)
        return;
    BYTE best = 0;
    for (const UsableCpuSet &set : sets)
        best = std::max(best, set.efficiency);
    // The second fastest-class core (core 0 takes most interrupts and system work).
    std::vector<BYTE> fastCores;
    for (const UsableCpuSet &set : sets)
        if (set.efficiency == best && std::find(fastCores.begin(), fastCores.end(), set.core) == fastCores.end())
            fastCores.push_back(set.core);
    if (!forced && ps2xHostCpu().physical < 12)
        return;
    const BYTE core = fastCores.size() > 1 ? fastCores[1] : fastCores[0];
    std::vector<ULONG> mine, others;
    for (const UsableCpuSet &set : sets)
        (set.core == core ? mine : others).push_back(set.id);
    if (others.empty())
        return;
    // One logical processor for the worker; its sibling stays empty.
    SetThreadSelectedCpuSets(GetCurrentThread(), mine.data(), 1);
    SetProcessDefaultCpuSets(GetCurrentProcess(), others.data(), static_cast<ULONG>(others.size()));
    std::fprintf(stderr, "[cpu] VU1 worker on core %u (cpu set %lu), %zu sets for the rest\n", (unsigned)core, mine[0], others.size());
}

// One PS2X_THREAD_CPU window: [threads] line for the next `seconds`, labelled from `startSec`.
static void reportThreadCpu(double startSec, double seconds)
{
    const auto cpuTime = [](HANDLE h) -> uint64_t {
        FILETIME c, e, k, u;
        if (!GetThreadTimes(h, &c, &e, &k, &u))
            return 0;
        return ((static_cast<uint64_t>(k.dwHighDateTime) << 32) | k.dwLowDateTime) +
               ((static_cast<uint64_t>(u.dwHighDateTime) << 32) | u.dwLowDateTime);
    };
    const auto kernelTime = [](HANDLE h) -> uint64_t {
        FILETIME c, e, k, u;
        if (!GetThreadTimes(h, &c, &e, &k, &u))
            return 0;
        return (static_cast<uint64_t>(k.dwHighDateTime) << 32) | k.dwLowDateTime;
    };
    struct T { HANDLE h; std::string name; uint64_t start; uint64_t kernelStart; };
    std::vector<T> threads;
    const DWORD pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    if (snap != INVALID_HANDLE_VALUE && Thread32First(snap, &te))
    {
        do
        {
            if (te.th32OwnerProcessID == pid)
                if (HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID))
                    threads.push_back({h, threadName(h), cpuTime(h), kernelTime(h)});
        } while (Thread32Next(snap, &te));
    }
    if (snap != INVALID_HANDLE_VALUE)
        CloseHandle(snap);
    const auto t0 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
    const double wall100ns = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() * 1e7;
    // GSBand0..n are summed; so are unnamed (driver, CRT) threads.
    std::vector<std::pair<std::string, std::pair<double, int>>> groups;
    double total = 0.0;
    std::string kernelLine; // named threads' kernel-mode share (wakes, waits, heap growth: the OS part of their CPU)
    for (T &t : threads)
    {
        const double share = static_cast<double>(cpuTime(t.h) - t.start) / wall100ns;
        const double kernel = static_cast<double>(kernelTime(t.h) - t.kernelStart) / wall100ns;
        CloseHandle(t.h);
        if (!t.name.empty() && t.name.rfind("GSBand", 0) != 0 && share >= 0.005)
        {
            char kb[96];
            std::snprintf(kb, sizeof(kb), " %s %.0f%%", t.name.c_str(), kernel * 100.0);
            kernelLine += kb;
        }
        total += share;
        std::string key = t.name.empty() ? "(unnamed)" : t.name;
        if (key.rfind("GSBand", 0) == 0)
            key = "GSBand*";
        auto it = std::find_if(groups.begin(), groups.end(), [&](const auto &g) { return g.first == key; });
        if (it == groups.end())
            groups.push_back({key, {share, 1}});
        else
        {
            it->second.first += share;
            ++it->second.second;
        }
    }
    std::sort(groups.begin(), groups.end(), [](const auto &a, const auto &b) { return a.second.first > b.second.first; });
    std::string line;
    char buf[96];
    for (const auto &g : groups)
    {
        if (g.second.first < 0.005)
            continue;
        if (g.second.second > 1)
            std::snprintf(buf, sizeof(buf), " %s %dx %.0f%%", g.first.c_str(), g.second.second, g.second.first * 100.0);
        else
            std::snprintf(buf, sizeof(buf), " %s %.0f%%", g.first.c_str(), g.second.first * 100.0);
        line += buf;
    }
    std::fprintf(stderr, "[threads] %.0f-%.0f s: total %.0f%% of %d logical |%s\n", startSec, startSec + seconds,
                 total * 100.0, ps2xHostCpu().logical, line.c_str());
    std::fprintf(stderr, "[threads-kernel] %.0f-%.0f s:%s\n", startSec, startSec + seconds, kernelLine.c_str());
}

// PS2X_THREAD_CPU (see the top of the file).
static void startThreadCpuReport()
{
    const char *env = std::getenv("PS2X_THREAD_CPU");
    if (!env || !*env)
        return;
    const double startSec = std::atof(env);
    const char *comma = std::strchr(env, ',');
    const double seconds = std::max(1.0, comma ? std::atof(comma + 1) : 10.0);
    std::thread([startSec, seconds]()
                {
        std::this_thread::sleep_for(std::chrono::duration<double>(startSec));
        for (double from = startSec;; from += seconds)
            reportThreadCpu(from, seconds); })
        .detach();
}
#else
#include <algorithm>
#include <cstdint>
#include <thread>
const Ps2xHostCpu &ps2xHostCpu()
{
    static const Ps2xHostCpu cpu = [] {
        Ps2xHostCpu c;
        c.logical = c.physical = c.fast = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
        return c;
    }();
    return cpu;
}
void ps2xStartSampler() {}
void ps2xStartHostWatch(uint8_t *) {}
void ps2xReserveCoreForCurrentThread() {}
#endif
