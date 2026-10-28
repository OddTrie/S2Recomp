#include <mutex>
#include <unordered_set>
#include <string>
#include <vector>
#include "ps2_runtime.h"
#include "runtime/ps2_netplay.h"
#include "ps2_log.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"
#include "game_overrides.h"
#include "ps2_runtime_macros.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ee_scheduler.h"
#include "ThreadNaming.h"
#include "Kernel/Stubs/Audio.h"
#include "Kernel/Stubs/GS.h"
#include "Kernel/Stubs/MPEG.h"
#include "ps2_host_backend.h"
#include "ps2_iop_host.h"
#include "ps2x/iop/iop_subsystem.h"
#include "runtime/ps2_spu2_audio.h"
#include "runtime/ps2_framegen.h"
#include "runtime/gs/gs_gpu_backend.h"
#include "runtime/ps2_host_cpu.h"
#include "runtime/ps2_mod_loader.h"

#include <iostream>
#include <stdexcept>
#include <fstream>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <chrono>
#include <atomic>
#include <thread>
#include <unordered_map>
#include <sstream>

// Present on the host monitor's VSync (set in initialize, PS2X_PRESENT_VSYNC=0 turns it off).
static bool s_presentVsync = false;

#if !defined(PLATFORM_VITA) && !defined(PLATFORM_ANDROID)
// Borderless fullscreen (F11, PS2X_FULLSCREEN).
static void toggleFullscreen()
{
    ToggleBorderlessWindowed();
#if defined(_WIN32)
    // The GPU driver turns a borderless OpenGL window exactly the monitor's size into exclusive fullscreen, which
    // OBS Display Capture records as black. One row taller (off the bottom edge) keeps it an ordinary window.
    if (IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE))
    {
        const int monitor = GetCurrentMonitor();
        SetWindowSize(GetMonitorWidth(monitor), GetMonitorHeight(monitor) + 1);
    }
#endif
}
#endif

#if PS2X_APP_ICON_PNG
// assets/s2recomp.png, compiled in by CMake (app_icon_png.cpp). Windows takes the icon from the exe instead.
extern const unsigned char kAppIconPng[];
extern const unsigned int kAppIconPng_size;
#endif

void ps2PadPollHost(); // ps2_pad.cpp: latch host button presses once per host frame

namespace ps2_stubs
{
    void resetSifState();
}

#define ELF_MAGIC 0x464C457F // "\x7FELF" in little endian
#define ET_EXEC 2            // Executable file
#define EM_MIPS 8            // MIPS architecture
#define PT_LOAD 1            // Loadable segment

static constexpr int FB_WIDTH = 640;
static constexpr int FB_HEIGHT = 512;
static constexpr int DEFAULT_DISPLAY_HEIGHT = 448;
static constexpr uint32_t DEFAULT_FB_SIZE = FB_WIDTH * FB_HEIGHT * 4;
static constexpr uint32_t DEFAULT_FB_ADDR = (PS2_RAM_SIZE - DEFAULT_FB_SIZE - 0x10000u);
#if defined(PLATFORM_VITA)
static constexpr int HOST_WINDOW_WIDTH = 960;
static constexpr int HOST_WINDOW_HEIGHT = 544;
#else
// 2x the 640x448 picture at 4:3; shrunk to fit the monitor after the window opens.
static constexpr int HOST_WINDOW_WIDTH = 1280;
static constexpr int HOST_WINDOW_HEIGHT = 960;
#endif
struct ElfHeader
{
    uint32_t magic;
    uint8_t elf_class;
    uint8_t endianness;
    uint8_t version;
    uint8_t os_abi;
    uint8_t abi_version;
    uint8_t padding[7];
    uint16_t type;
    uint16_t machine;
    uint32_t version2;
    uint32_t entry;
    uint32_t phoff;
    uint32_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
};

struct ProgramHeader
{
    uint32_t type;
    uint32_t offset;
    uint32_t vaddr;
    uint32_t paddr;
    uint32_t filesz;
    uint32_t memsz;
    uint32_t flags;
    uint32_t align;
};

namespace
{
    constexpr uint32_t kGuestHeapDefaultBase = 0x00100000u;
    constexpr uint32_t kGuestHeapDefaultAlignment = 16u;
    constexpr uint32_t kGuestHeapSafetyPad = 0x1000u;
    constexpr uint32_t kGuestHeapHardLimit = 0x01F00000u;

    constexpr uint32_t COP0_CAUSE_EXCCODE_MASK = 0x0000007Cu;
    constexpr uint32_t COP0_CAUSE_BD = 0x80000000u;
    constexpr uint32_t COP0_STATUS_EXL = 0x00000002u;
    constexpr uint32_t COP0_STATUS_BEV = 0x00400000u;
    constexpr uint32_t EXCEPTION_VECTOR_GENERAL = 0x80000080u;
    constexpr uint32_t EXCEPTION_VECTOR_TLB_REFILL = 0x80000000u;
    constexpr uint32_t EXCEPTION_VECTOR_BOOT = 0xBFC00200u;

    struct DispatchHistory
    {
        std::array<uint32_t, 64> pcs{};
        uint32_t next = 0u;
        bool wrapped = false;
    };

    thread_local DispatchHistory g_dispatchHistory;

    bool computeFileCrc32(const std::string &path, uint32_t &crcOut)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            return false;
        }

        static const std::array<uint32_t, 256> table = []
        {
            std::array<uint32_t, 256> values{};
            for (uint32_t i = 0; i < values.size(); ++i)
            {
                uint32_t value = i;
                for (uint32_t bit = 0; bit < 8; ++bit)
                {
                    value = (value & 1u) ? (0xEDB88320u ^ (value >> 1u)) : (value >> 1u);
                }
                values[i] = value;
            }
            return values;
        }();

        uint32_t crc = 0xFFFFFFFFu;
        std::array<uint8_t, 16 * 1024> buffer{};
        while (file.good())
        {
            file.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize count = file.gcount();
            for (std::streamsize i = 0; i < count; ++i)
            {
                crc = table[(crc ^ buffer[static_cast<size_t>(i)]) & 0xFFu] ^ (crc >> 8u);
            }
        }
        if (file.bad())
        {
            return false;
        }
        crcOut = ~crc;
        return true;
    }

    void pushDispatchPc(uint32_t pc)
    {
        DispatchHistory &h = g_dispatchHistory;
        h.pcs[h.next] = pc;
        h.next = (h.next + 1u) % static_cast<uint32_t>(h.pcs.size());
        if (h.next == 0u)
        {
            h.wrapped = true;
        }
    }

}

// PS2X_DISPATCH_RING=1: ring of recent dispatcher events (debug for "resumed at a stale pc" bugs),
// printed with every missing-target report. Kinds: C = guest call/jump via dispatchGuestBranch
// (a=target, b=source), R = it returned (a=target, b=result, pc=ctx->pc after), S = scheduler
// resumes a thread (a=pc), X = scheduler's call returned (pc=ctx->pc after).
extern volatile int g_ps2xRunningGuestThread; // EeScheduler.cpp
namespace
{
    struct DispatchEvent
    {
        char kind;
        int thread;
        uint32_t a, b, pc, sp, ra;
    };
    constexpr uint32_t kDispatchRingSize = 4096;
    DispatchEvent g_dispatchRing[kDispatchRingSize];
    uint32_t g_dispatchRingNext = 0;
    uint64_t g_dispatchRingCount = 0;
}

// Continuation requests issued on this thread: dispatcher/checkpoint returns that ask the caller
// chain to unwind and continue at ctx->pc (see dispatchGuestBranch).
thread_local uint64_t t_dispatchContinuations = 0;

// Read once at startup (it was a function-local static: a guard check on every dispatch).
static const bool g_dispatchRingOn = [] { const char *e = std::getenv("PS2X_DISPATCH_RING"); return e && *e && *e != '0'; }();

bool ps2xDispatchRingOn()
{
    return g_dispatchRingOn;
}

void ps2xDispatchEvent(char kind, uint32_t a, uint32_t b, const R5900Context *ctx)
{
    DispatchEvent &e = g_dispatchRing[g_dispatchRingNext];
    g_dispatchRingNext = (g_dispatchRingNext + 1u) % kDispatchRingSize;
    ++g_dispatchRingCount;
    e.kind = kind;
    e.thread = g_ps2xRunningGuestThread;
    e.a = a;
    e.b = b;
    e.pc = ctx ? ctx->pc : 0u;
    e.sp = ctx ? getRegU32(ctx, 29) : 0u;
    e.ra = ctx ? getRegU32(ctx, 31) : 0u;
}

static void dumpDispatchRing(uint32_t events)
{
    if (!ps2xDispatchRingOn())
        return;
    const uint32_t count = static_cast<uint32_t>(std::min<uint64_t>(g_dispatchRingCount, std::min(events, kDispatchRingSize)));
    std::fprintf(stderr, "[dring] last %u dispatcher events (oldest first):\n", count);
    for (uint32_t i = 0; i < count; ++i)
    {
        const DispatchEvent &e = g_dispatchRing[(g_dispatchRingNext + kDispatchRingSize - count + i) % kDispatchRingSize];
        std::fprintf(stderr, "[dring] %c th%d a=%x b=%x pc=%x sp=%x ra=%x\n", e.kind, e.thread, e.a, e.b, e.pc, e.sp, e.ra);
    }
}

namespace
{
    std::string formatDispatchHistory()
    {
        const DispatchHistory &h = g_dispatchHistory;
        const uint32_t count = h.wrapped ? static_cast<uint32_t>(h.pcs.size()) : h.next;
        if (count == 0u)
        {
            return "(empty)";
        }

        std::ostringstream oss;
        bool first = true;
        for (uint32_t i = 0u; i < count; ++i)
        {
            const uint32_t idx = (h.next + h.pcs.size() - count + i) % static_cast<uint32_t>(h.pcs.size());
            if (!first)
            {
                oss << " -> ";
            }
            first = false;
            oss << "0x" << std::hex << h.pcs[idx];
        }
        return oss.str();
    }

    uint32_t selectExceptionVector(const R5900Context *ctx, bool tlbRefill)
    {
        if (ctx->cop0_status & COP0_STATUS_BEV)
        {
            return EXCEPTION_VECTOR_BOOT;
        }
        return tlbRefill ? EXCEPTION_VECTOR_TLB_REFILL : EXCEPTION_VECTOR_GENERAL;
    }

    void seedVu0IdleSuccess(R5900Context *ctx)
    {
        if (!ctx)
        {
            return;
        }

        ctx->vu0_clip_flags = 0;
        ctx->vu0_clip_flags2 = 0;
        ctx->vu0_mac_flags = 0;
        ctx->vu0_status = 0;
        ctx->vu0_q = 1.0f;
        ctx->vu0_r = _mm_castsi128_ps(_mm_set1_epi32(0x3F800000));
        ctx->vu0_vpu_stat = 0;
        ctx->vu0_vpu_stat2 = 0;
    }

    void copyVu0ContextToState(const R5900Context *ctx, VU1State &state)
    {
        std::memset(&state, 0, sizeof(state));

        for (uint32_t i = 0; i < 32u; ++i)
        {
            _mm_storeu_ps(state.vf[i], ctx->vu0_vf[i]);
        }
        for (uint32_t i = 0; i < 16u; ++i)
        {
            state.vi[i] = static_cast<int16_t>(ctx->vi[i]);
        }

        _mm_storeu_ps(state.acc, ctx->vu0_acc);
        state.q = ctx->vu0_q;
        state.p = ctx->vu0_p;
        state.i = ctx->vu0_i;
        alignas(16) uint32_t rWords[4]{};
        _mm_storeu_si128(reinterpret_cast<__m128i *>(rWords), _mm_castps_si128(ctx->vu0_r));
        state.r = 0x3F800000u | (rWords[0] & 0x007FFFFFu);
        state.pc = ctx->vu0_pc;
        state.mac = ctx->vu0_mac_flags;
        state.clip = ctx->vu0_clip_flags;
        state.status = ctx->vu0_status;
        state.itop = ctx->vu0_itop;
        state.dBitEnabled = (ctx->vu0_fbrst & (1u << 2)) != 0u;
        state.tBitEnabled = (ctx->vu0_fbrst & (1u << 3)) != 0u;

        state.vf[0][0] = 0.0f;
        state.vf[0][1] = 0.0f;
        state.vf[0][2] = 0.0f;
        state.vf[0][3] = 1.0f;
        state.vi[0] = 0;
    }

    void copyVu0StateToContext(const VU1State &state, R5900Context *ctx)
    {
        for (uint32_t i = 0; i < 32u; ++i)
        {
            ctx->vu0_vf[i] = _mm_loadu_ps(state.vf[i]);
        }
        for (uint32_t i = 0; i < 16u; ++i)
        {
            ctx->vi[i] = static_cast<uint16_t>(state.vi[i]);
        }

        ctx->vu0_acc = _mm_loadu_ps(state.acc);
        ctx->vu0_q = state.q;
        ctx->vu0_p = state.p;
        ctx->vu0_i = state.i;
        ctx->vu0_r = _mm_castsi128_ps(_mm_set1_epi32(static_cast<int32_t>(state.r)));
        ctx->vu0_mac_flags = state.mac;
        ctx->vu0_clip_flags = state.clip;
        ctx->vu0_clip_flags2 = state.clip;
        ctx->vu0_status = static_cast<uint16_t>(state.status);
        ctx->vu0_itop = state.itop;
        ctx->vu0_pc = state.pc;
        ctx->vu0_tpc = state.pc;
        ctx->vu0_vpu_stat = (ctx->vu0_vpu_stat & 0xFF00u) | (state.stoppedByD ? (1u << 1) : 0u) | (state.stoppedByT ? (1u << 2) : 0u);
        ctx->vu0_vpu_stat2 = 0;

        ctx->vu0_vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
        ctx->vi[0] = 0;
    }

    void raiseCop0Exception(R5900Context *ctx, uint32_t exceptionCode, bool tlbRefill = false)
    {
        if (ctx->in_delay_slot)
        {
            ctx->cop0_epc = ctx->branch_pc;
            ctx->cop0_cause = (ctx->cop0_cause & ~COP0_CAUSE_EXCCODE_MASK) |
                              ((exceptionCode << 2) & COP0_CAUSE_EXCCODE_MASK) |
                              COP0_CAUSE_BD;
        }
        else
        {
            ctx->cop0_epc = ctx->pc;
            ctx->cop0_cause = (ctx->cop0_cause & ~(COP0_CAUSE_EXCCODE_MASK | COP0_CAUSE_BD)) |
                              ((exceptionCode << 2) & COP0_CAUSE_EXCCODE_MASK);
        }

        ctx->cop0_status |= COP0_STATUS_EXL;
        ctx->pc = selectExceptionVector(ctx, tlbRefill);
        ctx->in_delay_slot = false;
    }

    std::filesystem::path normalizeAbsolutePath(const std::filesystem::path &path)
    {
        if (path.empty())
        {
            return {};
        }

#if defined(PLATFORM_VITA)
        const std::string generic = path.generic_string();
        const std::size_t colon = generic.find(':');
        if (colon != std::string::npos && colon != 0u)
        {
            const std::size_t slash = generic.find_first_of("/\\");
            if (slash == std::string::npos || colon < slash)
            {
                return path.lexically_normal();
            }
        }
#endif

        std::error_code ec;
        const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
        if (ec)
        {
            return path.lexically_normal();
        }
        return absolute.lexically_normal();
    }

    PS2Runtime::IoPaths &runtimeIoPaths()
    {
        static PS2Runtime::IoPaths paths = []()
        {
            PS2Runtime::IoPaths defaults;
            std::error_code ec;
            const std::filesystem::path cwd = std::filesystem::current_path(ec);
            defaults.elfDirectory = ec ? std::filesystem::path(".") : cwd.lexically_normal();
            defaults.hostRoot = defaults.elfDirectory;
            defaults.cdRoot = defaults.elfDirectory;
            defaults.mcRoot = defaults.elfDirectory / "mc0";
            return defaults;
        }();

        return paths;
    }

    std::string readGuestPrintableString(const uint8_t *rdram, uint32_t addr, size_t maxLen)
    {
        std::string out;
        if (!rdram || maxLen == 0)
        {
            return out;
        }

        out.reserve(std::min<size_t>(maxLen, 64));
        for (size_t i = 0; i < maxLen; ++i)
        {
            const char ch = static_cast<char>(rdram[(addr + static_cast<uint32_t>(i)) & PS2_RAM_MASK]);
            if (ch == '\0')
            {
                break;
            }
            if (ch >= 0x20 && ch < 0x7F)
            {
                out.push_back(ch);
            }
            else
            {
                out.push_back('.');
            }
        }
        return out;
    }
}

static void UploadFrame(Texture2D &tex, PS2Runtime *rt, uint32_t &outWidth, uint32_t &outHeight)
{
    static uint64_t s_lastPresentationTick = std::numeric_limits<uint64_t>::max();
    static bool s_hasLatchedInitialFrame = false;
    static uint32_t s_lastDisplayFbp = std::numeric_limits<uint32_t>::max();
    static uint32_t s_lastSourceFbp = std::numeric_limits<uint32_t>::max();
    static bool s_lastPreferred = false;
    static uint32_t s_lastWidth = 0u;
    static uint32_t s_lastHeight = 0u;
    static bool s_hasUploadedFrame = false;
    static std::vector<uint8_t> s_scratch;
    static std::vector<uint8_t> s_uploadBuffer(DEFAULT_FB_SIZE, 0u);

    const uint64_t currentTick = rt->eeScheduler().currentVSyncTick();
    const bool needsLatch = !s_hasLatchedInitialFrame || currentTick != s_lastPresentationTick;
    if (needsLatch)
    {
        rt->gs().latchHostPresentationFrame();
        s_lastPresentationTick = currentTick;
        s_hasLatchedInitialFrame = true;
    }
    else if (s_hasUploadedFrame)
    {
        outWidth = (s_lastWidth != 0u) ? s_lastWidth : FB_WIDTH;
        outHeight = (s_lastHeight != 0u) ? s_lastHeight : DEFAULT_DISPLAY_HEIGHT;
        return;
    }

    s_scratch.clear();
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t displayFbp = 0u;
    uint32_t sourceFbp = 0u;
    bool usedPreferredDisplaySource = false;
    if (!rt->gs().copyLatchedHostPresentationFrame(s_scratch,
                                                   width,
                                                   height,
                                                   &displayFbp,
                                                   &sourceFbp,
                                                   &usedPreferredDisplaySource))
    {
        Image blank = GenImageColor(FB_WIDTH, FB_HEIGHT, MAGENTA);
        UpdateTexture(tex, blank.data);
        ps2x_framegen::reset();
        UnloadImage(blank);
        outWidth = FB_WIDTH;
        outHeight = DEFAULT_DISPLAY_HEIGHT;
        s_lastWidth = outWidth;
        s_lastHeight = outHeight;
        s_hasUploadedFrame = true;
        return;
    }

    PS2_IF_AGRESSIVE_LOGS({
        static uint32_t s_uploadDebugCount = 0u;
        if (s_uploadDebugCount < 128u ||
            displayFbp != s_lastDisplayFbp ||
            sourceFbp != s_lastSourceFbp ||
            usedPreferredDisplaySource != s_lastPreferred ||
            width != s_lastWidth ||
            height != s_lastHeight)
        {
            std::cout << "[frame:upload] idx=" << s_uploadDebugCount
                      << " tick=" << currentTick
                      << " displayFbp=" << displayFbp
                      << " sourceFbp=" << sourceFbp
                      << " size=" << width << "x" << height
                      << " preferred=" << static_cast<uint32_t>(usedPreferredDisplaySource ? 1u : 0u)
                      << std::endl;
        }
        ++s_uploadDebugCount;
    });
    s_lastDisplayFbp = displayFbp;
    s_lastSourceFbp = sourceFbp;
    s_lastPreferred = usedPreferredDisplaySource;
    s_lastWidth = width;
    s_lastHeight = height;

    std::fill(s_uploadBuffer.begin(), s_uploadBuffer.end(), 0u);
    if (!s_scratch.empty() && width != 0u && height != 0u)
    {
        const uint32_t copyWidth = std::min<uint32_t>(width, FB_WIDTH);
        const uint32_t copyHeight = std::min<uint32_t>(height, FB_HEIGHT);
        const size_t srcRowBytes = static_cast<size_t>(width) * 4u;
        const size_t dstRowBytes = static_cast<size_t>(FB_WIDTH) * 4u;
        const size_t copyRowBytes = static_cast<size_t>(copyWidth) * 4u;
        for (uint32_t y = 0; y < copyHeight; ++y)
        {
            const size_t srcOffset = static_cast<size_t>(y) * srcRowBytes;
            const size_t dstOffset = static_cast<size_t>(y) * dstRowBytes;
            if (srcOffset + copyRowBytes > s_scratch.size() ||
                dstOffset + copyRowBytes > s_uploadBuffer.size())
            {
                break;
            }
            std::memcpy(s_uploadBuffer.data() + dstOffset, s_scratch.data() + srcOffset, copyRowBytes);
        }
    }

    if (ps2x_framegen::enabled())
    {
        // Frame generation keeps its own texture ring and picks what to show in ps2x_framegen::present().
        ps2x_framegen::submit(s_uploadBuffer.data(), width, height);
    }
    else
    {
        UpdateTexture(tex, s_uploadBuffer.data());
    }
    outWidth = width;
    outHeight = height;
    s_hasUploadedFrame = true;
}

// ---------------------------------------------------------------------------
// Runtime-private guest memory.
//
// HLE code needs guest-visible scratch memory (GIF packets, SIF command packets)
// and guest stacks for queued invocations (interrupt handlers, RPC callbacks).
// Originally the scratch heap shared the game's heap range ([_end, 0x1F00000))
// and invocation stacks were carved from the top of RAM. Both collide with games
// that run their own newlib sbrk/malloc over "the rest of RAM" (EndOfHeap) and
// with a main thread stack placed at the end of RAM. EA's Sims 2 does exactly
// that and its heap init then overwrote its own .data.
//
// Retail programs load at 0x00100000; 0x00080000-0x00100000 is kernel space on
// real hardware that no game touches, so the runtime keeps its own allocations
// there. Invocation stacks always live there; HLE scratch moves there once the
// game shows it manages its own heap (it calls EndOfHeap).
// ---------------------------------------------------------------------------
namespace
{
    constexpr uint32_t kPrivateHeapBase = 0x00080000u;
    constexpr uint32_t kPrivateHeapEnd = 0x000C0000u;
    constexpr uint32_t kPrivateStackFloor = 0x000C0000u;
    constexpr uint32_t kPrivateStackTop = 0x00100000u;

    struct PrivateArena
    {
        std::mutex mutex;
        bool regionUsable = false;   // the loaded program leaves 0x80000-0x100000 free
        bool heapActive = false;     // HLE scratch allocations go here
        uint32_t stackTop = kPrivateStackTop;
        struct Block
        {
            uint32_t base;
            uint32_t size;
            bool free;
        };
        std::vector<Block> blocks{{kPrivateHeapBase, kPrivateHeapEnd - kPrivateHeapBase, true}};

        uint32_t allocate(uint32_t size, uint32_t alignment)
        {
            if (size == 0u)
                size = 1u;
            alignment = std::max<uint32_t>(alignment, 16u);
            size = (size + 15u) & ~15u;
            for (size_t i = 0; i < blocks.size(); ++i)
            {
                Block block = blocks[i];
                if (!block.free)
                    continue;
                const uint32_t aligned = (block.base + alignment - 1u) & ~(alignment - 1u);
                const uint32_t pad = aligned - block.base;
                if (pad + size > block.size)
                    continue;
                std::vector<Block> replacement;
                if (pad)
                    replacement.push_back({block.base, pad, true});
                replacement.push_back({aligned, size, false});
                if (block.size > pad + size)
                    replacement.push_back({aligned + size, block.size - pad - size, true});
                blocks.erase(blocks.begin() + static_cast<std::ptrdiff_t>(i));
                blocks.insert(blocks.begin() + static_cast<std::ptrdiff_t>(i), replacement.begin(), replacement.end());
                return aligned;
            }
            return 0u;
        }

        bool owns(uint32_t address) const
        {
            const uint32_t physical = address & 0x1FFFFFFFu;
            return physical >= kPrivateHeapBase && physical < kPrivateHeapEnd;
        }

        void release(uint32_t address)
        {
            const uint32_t physical = address & 0x1FFFFFFFu;
            for (size_t i = 0; i < blocks.size(); ++i)
            {
                if (blocks[i].base == physical && !blocks[i].free)
                {
                    blocks[i].free = true;
                    // coalesce neighbours
                    for (size_t j = 0; j + 1 < blocks.size();)
                    {
                        if (blocks[j].free && blocks[j + 1].free && blocks[j].base + blocks[j].size == blocks[j + 1].base)
                        {
                            blocks[j].size += blocks[j + 1].size;
                            blocks.erase(blocks.begin() + static_cast<std::ptrdiff_t>(j + 1));
                        }
                        else
                            ++j;
                    }
                    return;
                }
            }
        }
    };

    PrivateArena &privateArena()
    {
        static PrivateArena arena;
        return arena;
    }
}

void ps2RuntimeActivatePrivateHeap()
{
    PrivateArena &arena = privateArena();
    std::lock_guard<std::mutex> lock(arena.mutex);
    if (arena.regionUsable && !arena.heapActive)
    {
        arena.heapActive = true;
        std::cerr << "[runtime] game manages its own heap; HLE scratch memory moved to 0x80000-0xC0000" << std::endl;
    }
}

PS2Runtime::PS2Runtime()
{
    m_iopHost = std::make_unique<PS2IopHostAdapter>(*this);
    m_iopSubsystem = std::make_unique<ps2x::iop::IopSubsystem>(*m_iopHost);
    ps2AttachIopSubsystemForEeRpc(m_iopSubsystem.get());

    m_eeScheduler = std::make_unique<EeScheduler>(*this);

    // Assign rather than memset: R5900Context's constructor zeroes itself and
    // then applies the COP0 reset values, which a memset here would discard.
    m_cpuContext = R5900Context{};

    // R0 is always zero in MIPS
    m_cpuContext.r[0] = _mm_set1_epi32(0);
    m_cpuContext.vu0_vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
    m_cpuContext.vu0_q = 1.0f;
    m_cpuContext.vu0_r = _mm_castsi128_ps(_mm_set1_epi32(0x3F800000));

    // Stack pointer (SP) and global pointer (GP) will be set by the loaded ELF

    m_loadedModules.clear();
    m_guestHeapBlocks.clear();
    m_guestHeapBase = kGuestHeapDefaultBase;
    m_guestHeapEnd = kGuestHeapDefaultBase;
    m_guestHeapLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    m_guestHeapSuggestedBase = kGuestHeapDefaultBase;
    m_guestHeapConfigured = false;
    m_asyncCallbackStackFloor = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    m_asyncCallbackStackTop = PS2_RAM_SIZE;
}

void PS2Runtime::setDebugUiCallbacks(DebugUiCallback initCallback,
                                     DebugUiCallback drawCallback,
                                     DebugUiCallback shutdownCallback,
                                     void *userData)
{
    if (m_debugUiInitialized && m_debugUiShutdownCallback)
    {
        m_debugUiShutdownCallback(*this, m_debugUiUserData);
        m_debugUiInitialized = false;
    }

    m_debugUiInitCallback = initCallback;
    m_debugUiDrawCallback = drawCallback;
    m_debugUiShutdownCallback = shutdownCallback;
    m_debugUiUserData = userData;
}

PS2Runtime::~PS2Runtime()
{
    try
    {
        requestStop();
        m_iopSubsystem.reset();
        m_iopHost.reset();
#if defined(PLATFORM_VITA)
        m_audioBackend.stopAll();
        m_audioBackend.setAudioReady(false);
#else
        ps2x_audio::stopSpu2Output();
        if (IsAudioDeviceReady())
        {
            CloseAudioDevice();
            m_audioBackend.setAudioReady(false);
        }
#endif
        if (m_debugUiInitialized && m_debugUiShutdownCallback)
        {
            m_debugUiShutdownCallback(*this, m_debugUiUserData);
            m_debugUiInitialized = false;
        }

        if (IsWindowReady())
        {
            ps2xGsGpuHostShutdown();
            CloseWindow();
        }

        m_loadedModules.clear();
    }
    catch (const std::exception &e)
    {
        std::cerr << "[~PS2Runtime] cleanup exception: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "[~PS2Runtime] cleanup exception: unknown" << std::endl;
    }
}

ps2x::iop::ModuleLoadResult PS2Runtime::loadIopModule(std::string_view path, const void *arguments, uint32_t argumentSize)
{
    auto scope = m_iopHost->enterCall(nullptr, m_memory.getRDRAM());
    return m_iopSubsystem->loadModule(path, arguments, argumentSize);
}

ps2x::iop::ModuleLoadResult PS2Runtime::loadIopModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize)
{
    auto scope = m_iopHost->enterCall(nullptr, m_memory.getRDRAM());
    return m_iopSubsystem->loadModuleBuffer(guestAddress, arguments, argumentSize);
}

bool PS2Runtime::stopIopModule(int32_t moduleId, int32_t *result)
{
    auto scope = m_iopHost->enterCall(nullptr, m_memory.getRDRAM());
    return m_iopSubsystem->stopModule(moduleId, result);
}

ps2x::iop::RpcAbi PS2Runtime::selectIopRpcAbi(const ps2x::iop::RpcAbiRequest &request) const
{
    return m_iopSubsystem->selectRpcAbi(request);
}

bool PS2Runtime::canBindIopRpc(uint32_t sid) const noexcept
{
    return m_iopSubsystem->canBindRpc(sid);
}

ps2x::iop::RpcResult PS2Runtime::handleIopRpc(uint8_t *rdram, R5900Context *ctx, ps2x::iop::RpcRequest request)
{
    auto scope = m_iopHost->enterCall(ctx, rdram);
    request.callToken = scope.token();
    return m_iopSubsystem->handleRpc(request);
}

void PS2Runtime::notifyIopSifTransfer(uint8_t *rdram, const ps2x::iop::SifTransfer &transfer)
{
    auto scope = m_iopHost->enterCall(nullptr, rdram);
    m_iopSubsystem->onSifTransfer(transfer);
}

void PS2Runtime::advanceIopEeCycles(uint64_t eeCycles) noexcept
{
    m_iopSubsystem->runEeCycles(eeCycles);
}

void PS2Runtime::resetIop()
{
    m_iopSubsystem->reset();
}

ps2x::iop::DebugSnapshot PS2Runtime::iopDebugSnapshot() const
{
    return m_iopSubsystem->debugSnapshot();
}

uint32_t PS2Runtime::allocateIopMemory(uint32_t size, uint32_t alignment)
{
    return m_iopSubsystem ? m_iopSubsystem->allocateMemory(size, alignment) : 0u;
}

bool PS2Runtime::freeIopMemory(uint32_t address)
{
    return m_iopSubsystem && m_iopSubsystem->freeMemory(address);
}

bool PS2Runtime::readIopMemory(uint32_t address, void *destination, size_t size) const
{
    return m_iopSubsystem && m_iopSubsystem->readMemory(address, destination, size);
}

bool PS2Runtime::writeIopMemory(uint32_t address, const void *source, size_t size)
{
    return m_iopSubsystem && m_iopSubsystem->writeMemory(address, source, size);
}

bool PS2Runtime::zeroIopMemory(uint32_t address, size_t size)
{
    return m_iopSubsystem && m_iopSubsystem->zeroMemory(address, size);
}

bool PS2Runtime::isIopMemoryRange(uint32_t address, size_t size) const
{
    return m_iopSubsystem && m_iopSubsystem->isMemoryRange(address, size);
}

bool ps2xOnVif1AsyncWorker(); // ps2_memory.cpp
std::unique_ptr<GSRasterBackend> ps2xMaybeRecordGsBackend(std::unique_ptr<GSRasterBackend> backend); // gs_record_replay.cpp
void ps2xGsFeedSetProcessor(std::function<void(const uint8_t *, uint32_t)> fn); // ps2_memory.cpp
bool ps2xGsFeedSubmit(const uint8_t *data, uint32_t size);                      // ps2_memory.cpp
bool ps2xGsFeedSubmitOwned(std::vector<uint8_t> &data);                         // ps2_memory.cpp
void ps2xVu1TimingEnterVu();                                                    // ps2_memory.cpp: PS2X_VU1_TIMING
void ps2xVu1TimingLeave();

// The GS renderer (after InitWindow, before the game runs). The GPU renderer is the default: it is
// never slower in the measurements (4 E-cores to the i9-14900K, menu/pan/shower/lot) and needs ~2
// cores instead of up to ~10. PS2X_GS_GPU=0 forces the CPU renderer (the bit-exact reference),
// PS2X_GS_GPU=1 the GPU. If GL cannot run the GPU renderer, the CPU renderer stays.
static void selectGsRenderer(GS &gs)
{
    const char *env = std::getenv("PS2X_GS_GPU");
    const bool forcedCpu = env && *env == '0';
    const bool forcedGpu = env && *env && *env != '0';
    const int cpus = ps2xHostCpu().logical;
    if (forcedCpu)
    {
        std::fprintf(stderr, "[gs] renderer: CPU (PS2X_GS_GPU=0)\n");
        return;
    }
    if (!ps2xGsGpuAvailable())
    {
        std::fprintf(stderr, "[gs] renderer: CPU (built without GL support)\n");
        return;
    }
    ps2xGsGpuHostInit(false);
    std::unique_ptr<GSRasterBackend> gpu = ps2xMakeGsGpuBackend();
    std::string reason;
    if (!ps2xGsGpuWaitReady(10000, &reason))
    {
        gpu.reset();
        ps2xGsGpuHostShutdown();
        std::fprintf(stderr, "[gs] GPU renderer unavailable (%s), using CPU renderer\n", reason.c_str());
        std::fprintf(stderr, "[gs] renderer: CPU (fallback, %d CPUs)\n", cpus);
        return;
    }
    gs.setRasterBackend(ps2xMaybeRecordGsBackend(std::move(gpu)));
    std::fprintf(stderr, "[gs] renderer: GPU (%s, %d CPUs)\n", forcedGpu ? "PS2X_GS_GPU=1" : "auto", cpus);
}

bool PS2Runtime::syncCoreSubsystems()
{
    uint8_t *const rdram = m_memory.getRDRAM();
    uint8_t *const gsVram = m_memory.getGSVRAM();
    if (!rdram || !gsVram)
    {
        return false;
    }

    if (m_boundRdram == rdram && m_boundGSVram == gsVram)
    {
        return true;
    }

    m_gs.init(gsVram, static_cast<uint32_t>(PS2_GS_VRAM_SIZE), &m_memory.gs());
    // On the async VIF1 worker the packets go on to the GS feed thread (ps2_memory.cpp).
    ps2xGsFeedSetProcessor([this](const uint8_t *data, uint32_t size)
                           { m_gs.processGIFPacket(data, size); });
    m_gifArbiter.setProcessPacketFn([this](const uint8_t *data, uint32_t size)
                                    {
                                        if (!ps2xGsFeedSubmit(data, size))
                                            m_gs.processGIFPacket(data, size); });
    m_gifArbiter.setTakePacketFn([](std::vector<uint8_t> &data) { return ps2xGsFeedSubmitOwned(data); });
    m_memory.setGifArbiter(&m_gifArbiter);
    m_memory.setVu1MscalCallback([this](uint32_t startPC, uint32_t top, uint32_t itop)
                                 {
                                     if (ps2xOnVif1AsyncWorker())
                                     {
                                         // Async VIF1 (ps2_memory.cpp): the EE context belongs to the EE thread.
                                         // D/T-bit halts (FBRST DE/TE, debug only) are not honoured here.
                                         m_vu1.state().dBitEnabled = false;
                                         m_vu1.state().tBitEnabled = false;
                                         ps2xVu1TimingEnterVu();
                                         m_vu1.execute(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                                                       m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                                                       m_gs, &m_memory, startPC, top, itop, 65536);
                                         ps2xVu1TimingLeave();
                                         return;
                                     }
                                     R5900Context *cpuContext = m_eeScheduler ? m_eeScheduler->currentContext() : nullptr;
                                     if (!cpuContext)
                                     {
                                         cpuContext = &m_cpuContext;
                                     }
                                     m_vu1.state().dBitEnabled =
                                         (cpuContext->vu0_fbrst & (1u << 10)) != 0u;
                                     m_vu1.state().tBitEnabled =
                                         (cpuContext->vu0_fbrst & (1u << 11)) != 0u;
                                     m_vu1.execute(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                                                   m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                                                   m_gs, &m_memory, startPC, top, itop, 65536);
                                     cpuContext->vu0_vpu_stat =
                                         (cpuContext->vu0_vpu_stat & ~0x0600u) |
                                         (m_vu1.state().stoppedByD ? 0x0200u : 0u) |
                                         (m_vu1.state().stoppedByT ? 0x0400u : 0u); });
    m_memory.setVu1MscntCallback([this](uint32_t top, uint32_t itop)
                                 {
                                     if (ps2xOnVif1AsyncWorker())
                                     {
                                         m_vu1.state().dBitEnabled = false;
                                         m_vu1.state().tBitEnabled = false;
                                         ps2xVu1TimingEnterVu();
                                         m_vu1.resume(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                                                      m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                                                      m_gs, &m_memory, top, itop, 65536);
                                         ps2xVu1TimingLeave();
                                         return;
                                     }
                                     R5900Context *cpuContext = m_eeScheduler ? m_eeScheduler->currentContext() : nullptr;
                                     if (!cpuContext)
                                     {
                                         cpuContext = &m_cpuContext;
                                     }
                                     m_vu1.state().dBitEnabled =
                                         (cpuContext->vu0_fbrst & (1u << 10)) != 0u;
                                     m_vu1.state().tBitEnabled =
                                         (cpuContext->vu0_fbrst & (1u << 11)) != 0u;
                                     m_vu1.resume(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                                                  m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                                                  m_gs, &m_memory, top, itop, 65536);
                                     cpuContext->vu0_vpu_stat =
                                         (cpuContext->vu0_vpu_stat & ~0x0600u) |
                                         (m_vu1.state().stoppedByD ? 0x0200u : 0u) |
                                         (m_vu1.state().stoppedByT ? 0x0400u : 0u); });
    resetIop();
    m_vu0.reset();
    m_vu1.reset();

    m_boundRdram = rdram;
    m_boundGSVram = gsVram;
    return true;
}

bool PS2Runtime::initialize(const char *title)
{
    try
    {
        if (!m_memory.initialize())
        {
            std::cerr << "Failed to initialize PS2 memory" << std::endl;
            return false;
        }

        if (!syncCoreSubsystems())
        {
            std::cerr << "Failed to bind runtime core subsystems" << std::endl;
            return false;
        }
#if defined(PLATFORM_VITA)
        InitWindow(HOST_WINDOW_WIDTH, HOST_WINDOW_HEIGHT, title); // raylib vita does not support audio
#else
        // Present on the monitor's VSync; the emulated VBlank locks onto it (EeScheduler.cpp), so every
        // game frame stays on screen equally long. PS2X_PRESENT_VSYNC=0 = the old 60 FPS sleep timer.
        const char *presentVsyncEnv = std::getenv("PS2X_PRESENT_VSYNC");
        s_presentVsync = !(presentVsyncEnv && *presentVsyncEnv == '0');
        // PS2X_NO_FOCUS=1: open without taking keyboard focus (test runs next to another game).
        const char *noFocusEnv = std::getenv("PS2X_NO_FOCUS");
        const bool noFocus = noFocusEnv && *noFocusEnv && *noFocusEnv != '0';
        SetConfigFlags(FLAG_WINDOW_RESIZABLE | (s_presentVsync ? FLAG_VSYNC_HINT : 0) | (noFocus ? FLAG_WINDOW_UNFOCUSED : 0));
        InitWindow(HOST_WINDOW_WIDTH, HOST_WINDOW_HEIGHT, title);
#if PS2X_APP_ICON_PNG
        {
            Image icon = LoadImageFromMemory(".png", kAppIconPng, static_cast<int>(kAppIconPng_size));
            SetWindowIcon(icon);
            UnloadImage(icon);
        }
#endif
        selectGsRenderer(m_gs); // GPU GS renderer unless PS2X_GS_GPU=0 or GL is not up to it
        InitAudioDevice();
        m_audioBackend.setAudioReady(IsAudioDeviceReady());
        ps2x_audio::startSpu2Output(IsAudioDeviceReady());
#endif
        SetExitKey(KEY_NULL); // Esc is a game key (Start, pause); closing the window still exits
        SetTargetFPS(s_presentVsync ? 0 : 60);
#if !defined(PLATFORM_VITA)
        {
            // Fit the default window into ~85% of the monitor height, keeping 4:3.
            // PS2X_WINDOW=<w>x<h> asks for a window size instead (the launcher's "Window size");
            // PS2X_FULLSCREEN=1 starts in borderless fullscreen (F11 still toggles it).
            // PS2X_MONITOR=<n> opens the window on monitor n (1-based, raylib/GLFW order; the startup log
            // lists them as "[present] monitor n: WxH at (x,y)").
            int monitor = GetCurrentMonitor();
            const int monitorCount = GetMonitorCount();
            if (const char *env = std::getenv("PS2X_MONITOR"); env && *env)
            {
                for (int i = 0; i < monitorCount; ++i)
                {
                    const Vector2 pos = GetMonitorPosition(i);
                    std::fprintf(stderr, "[present] monitor %d: %dx%d at (%d,%d)\n", i + 1, GetMonitorWidth(i),
                                 GetMonitorHeight(i), static_cast<int>(pos.x), static_cast<int>(pos.y));
                }
                const int n = std::atoi(env);
                if (n >= 1 && n <= monitorCount)
                {
                    monitor = n - 1;
                    std::fprintf(stderr, "[present] window on monitor %d (PS2X_MONITOR)\n", n);
                }
                else
                    std::fprintf(stderr, "[present] PS2X_MONITOR=%s ignored: %d monitors\n", env, monitorCount);
            }
            const Vector2 monitorPos = GetMonitorPosition(monitor);
            const int monitorX = static_cast<int>(monitorPos.x), monitorY = static_cast<int>(monitorPos.y);
            const int maxHeight = static_cast<int>(GetMonitorHeight(monitor) * 0.85f);
            int reqWidth = 0, reqHeight = 0;
            if (const char *env = std::getenv("PS2X_WINDOW"); env && *env)
                std::sscanf(env, "%dx%d", &reqWidth, &reqHeight);
            if (reqWidth >= 320 && reqHeight >= 224)
            {
                SetWindowSize(reqWidth, reqHeight);
                SetWindowPosition(monitorX + std::max(0, (GetMonitorWidth(monitor) - reqWidth) / 2),
                                  monitorY + std::max(0, (GetMonitorHeight(monitor) - reqHeight) / 2));
                std::fprintf(stderr, "[present] window %dx%d (PS2X_WINDOW)\n", reqWidth, reqHeight);
            }
            else if (maxHeight > 0 && maxHeight < HOST_WINDOW_HEIGHT)
            {
                const int height = std::max(DEFAULT_DISPLAY_HEIGHT, maxHeight);
                const int width = height * 4 / 3;
                SetWindowSize(width, height);
                SetWindowPosition(monitorX + (GetMonitorWidth(monitor) - width) / 2, monitorY + (GetMonitorHeight(monitor) - height) / 2);
            }
            else if (monitor != GetCurrentMonitor())
            {
                SetWindowPosition(monitorX + (GetMonitorWidth(monitor) - GetScreenWidth()) / 2,
                                  monitorY + (GetMonitorHeight(monitor) - GetScreenHeight()) / 2);
            }
#if !defined(PLATFORM_ANDROID)
            if (const char *env = std::getenv("PS2X_FULLSCREEN"); env && *env && *env != '0')
                toggleFullscreen();
#endif
        }
#endif
        if (m_debugUiInitCallback)
        {
            m_debugUiInitCallback(*this, m_debugUiUserData);
            m_debugUiInitialized = true;
        }

        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Failed to initialize PS2 runtime: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "Failed to initialize PS2 runtime: unknown exception" << std::endl;
    }

    return false;
}

// PS2X_60FPS (opt-in, off by default; not bit-exact with 30 FPS runs): The Sims 2 (SLES-53718) shows a
// new frame at most every 2nd VBlank. Its VBlank-start INTC handler 0x3EAE78 (via 0x3EB188) does
//   n = ++disp->0x94; if (disp->0x8C < 2) return;           // fewer than 2 frames queued: nothing to show
//   if (*0x553BC8 ? CSR.FIELD : n < 2) return;              // the 2-VBlank gate
//   flip (DISPFB), disp->0x8C--, disp->0x94 = 0, ...
// The wrapper below makes the n < 2 test pass from the first VBlank after a flip, so a queued frame is
// shown on the next VBlank.
//   PS2X_60FPS=1 (adaptive): only while the machine keeps up. In heavy scenes a frame takes longer than one
//     VBlank, the EE then waits for the VIF1 worker at each VBlank and the emulated VBlank rate (and with it
//     the game's VBlank-driven timers) falls to the frame rate: slow motion. So every 30 VBlanks the gate
//     looks at the host time those VBlanks took: at 60 it falls back to the game's own 30 FPS gate as soon
//     as the VBlank rate drops below 57/s. At 30 it tries 60 again after a few full-speed windows in which
//     both the EE thread and the VIF1/VU1 worker spent less than kUpBusyMs of host time per game frame.
//     That cost doesn't predict 60 well (the EE and the worker partly wait for each other: in the menu 60
//     sags at ~12 ms each, the quiet lot view holds it at ~13.5), so going up is a probe: it only rules
//     out frames that cannot fit a VBlank. A fall under load within 3 s of going up doubles the full-speed
//     windows needed before the next try, up to 16 (8 s); a failed probe costs half a second at ~90% speed.
//   PS2X_60FPS=always: every VBlank, whatever the cost (the original experiment).
//   PS2X_60FPS_LOG=1: one [60fps] line per 30-VBlank window (rate, frames, busy ms per frame, mode).
extern std::atomic<int64_t> g_ps2xEeWaitNs;        // EeScheduler.cpp: EE thread waiting (host ns)
extern std::atomic<int64_t> g_ps2xEeVblWorkerWaitNs; // EeScheduler.cpp: of that, the VBlank wait for the VIF1 worker
double ps2xDrainStatsTake(char *buf, size_t size);    // ps2_memory.cpp: EE drain waits since the last call
int64_t ps2xVu1IdleNsNow();                         // ps2_memory.cpp: VIF1 worker idle so far (host ns)
double ps2xVif1BacklogMs();                         // ps2_memory.cpp: estimated worker ms for what is queued (-1: n/a)
extern std::atomic<uint64_t> g_ps2xGameFrameCount; // EeScheduler.cpp: page flips
namespace
{
    PS2Runtime::RecompiledFunction s_sims2VblankOriginal = nullptr;

    struct Sims2FpsGate
    {
        static constexpr int kWindow = 30;        // VBlanks per decision (0.5 s at full speed)
        static constexpr double kDownHz = 57.0;   // at 60: VBlank rate below this -> 30
        static constexpr double kUpMinHz = 58.5;  // at 30: must run at full speed...
        static constexpr double kUpBusyMs = 16.0; // ...and frames that could fit a VBlank (host ms per frame, EE and VU1)
        bool adaptive = true;
        bool log = false;
        bool at60 = true;
        int vblanks = -1; // -1: no window yet
        std::chrono::steady_clock::time_point windowStart{};
        std::chrono::steady_clock::time_point wentUp{};
        int64_t eeWait0 = 0, vuIdle0 = 0, vblWait0 = 0;
        uint64_t frames0 = 0;
        int goodWindows = 0;
        int needGood = 2;
        // PS2X_60FPS_HOLD=<ms> (default 14, 0 = off): at 60, a VBlank at which the VIF1 worker still has
        // more than this much queued work (estimate: ps2xVif1BacklogMs) does not force the flip, so that
        // frame stays up two VBlanks and VU1 catches up before the EE would have to wait for it. Frames
        // slower than a VBlank then give 40-55 FPS at full speed (a mix of 1- and 2-VBlank frames)
        // instead of slow motion until the window drops to 30.
        // PS2X_60FPS_HOLD_WAIT=<ms> (default 0 = off): also hold when the EE already had to wait that long
        // for the worker at this VBlank. Measured worse with 2 (flyover 49-51 FPS vs 52 without).
        double holdMs = 14.0;
        double holdWaitMs = 0.0;
        int64_t vblWaitLast = 0;
        int held = 0;            // flips held in this window
        double backlogSum = 0.0; // for the log
        bool holdNow = false;

        void startWindow(std::chrono::steady_clock::time_point now)
        {
            vblanks = 0;
            windowStart = now;
            eeWait0 = g_ps2xEeWaitNs.load(std::memory_order_relaxed);
            vblWait0 = g_ps2xEeVblWorkerWaitNs.load(std::memory_order_relaxed);
            vuIdle0 = ps2xVu1IdleNsNow();
            frames0 = g_ps2xGameFrameCount.load(std::memory_order_relaxed);
        }

        // Once per VBlank (fresh handler entry), before the game's gate runs.
        void onVblank()
        {
            const auto now = std::chrono::steady_clock::now();
            holdNow = false;
            if (holdMs > 0.0 && at60)
            {
                const double backlog = ps2xVif1BacklogMs();
                static const bool dbg = [] { const char *e = std::getenv("PS2X_60FPS_HOLD_LOG"); return e && *e && *e != '0'; }();
                if (dbg)
                {
                    const int64_t vw = g_ps2xEeVblWorkerWaitNs.load(std::memory_order_relaxed);
                    static int64_t vwLast = 0;
                    std::fprintf(stderr, "[hold] t %.3f vblwait %.2f ms frames %llu\n",
                                 std::chrono::duration<double>(now.time_since_epoch()).count(), (vw - vwLast) / 1.0e6,
                                 (unsigned long long)g_ps2xGameFrameCount.load(std::memory_order_relaxed));
                    vwLast = vw;
                }
                const int64_t vw = g_ps2xEeVblWorkerWaitNs.load(std::memory_order_relaxed);
                const double waitedMs = static_cast<double>(vw - vblWaitLast) / 1.0e6;
                vblWaitLast = vw;
                holdNow = backlog > holdMs || (holdWaitMs > 0.0 && waitedMs > holdWaitMs);
                backlogSum += backlog > 0.0 ? backlog : 0.0;
                if (holdNow)
                    ++held;
            }
            if (vblanks < 0)
            {
                startWindow(now);
                wentUp = now;
                return;
            }
            if (++vblanks < kWindow)
                return;
            const double hostMs = std::chrono::duration<double, std::milli>(now - windowStart).count();
            const double eeWaitMs = static_cast<double>(g_ps2xEeWaitNs.load(std::memory_order_relaxed) - eeWait0) / 1.0e6;
            const double vuIdleMs = static_cast<double>(ps2xVu1IdleNsNow() - vuIdle0) / 1.0e6;
            const uint64_t frames = g_ps2xGameFrameCount.load(std::memory_order_relaxed) - frames0;
            const double hz = hostMs > 0.0 ? kWindow * 1000.0 / hostMs : 0.0;
            const double eeMs = frames ? std::max(0.0, hostMs - eeWaitMs) / static_cast<double>(frames) : 0.0;
            const double vuMs = frames ? std::max(0.0, hostMs - vuIdleMs) / static_cast<double>(frames) : 0.0;
            const char *change = nullptr;
            if (at60)
            {
                if (hz < kDownHz)
                {
                    at60 = false;
                    goodWindows = 0;
                    // Back off only after a quick fall under load (not a loading hitch: few flips or
                    // little work per frame).
                    const bool quick = now - wentUp < std::chrono::seconds(3) && frames >= 8 && std::max(eeMs, vuMs) >= 10.0;
                    needGood = quick ? std::min(needGood * 2, 16) : 2;
                    change = "-> 30";
                }
            }
            else if (frames >= 8) // loading screens and other phases without flips: no decision
            {
                const bool room = hz >= kUpMinHz && eeMs < kUpBusyMs && vuMs < kUpBusyMs;
                goodWindows = room ? goodWindows + 1 : 0;
                if (goodWindows >= needGood)
                {
                    at60 = true;
                    wentUp = now;
                    change = "-> 60";
                }
            }
            if (log || change)
            {
                std::fprintf(stderr, "[60fps] %.1f VBlank/s, %llu frames, busy/frame EE %.1f VU1 %.1f ms, %s (next up after %d good), held %d, backlog %.1f ms\n",
                             hz, static_cast<unsigned long long>(frames), eeMs, vuMs,
                             change ? change : (at60 ? "at 60" : "at 30"), needGood, held, backlogSum / kWindow);
            }
            if (log)
            {
                // Where the EE's non-busy and hidden-wait time went, ms per frame: VBlank wait for the VIF1
                // worker, pacing (the rest of the EE wait), and mid-frame drains (counted in EE busy above).
                char sites[256];
                const double drainMs = ps2xDrainStatsTake(sites, sizeof(sites));
                const double vblMs = static_cast<double>(g_ps2xEeVblWorkerWaitNs.load(std::memory_order_relaxed) - vblWait0) / 1.0e6;
                const double f = frames ? static_cast<double>(frames) : 1.0;
                std::fprintf(stderr, "[drain] per frame: vblwait %.2f pace %.2f drain %.2f ms |%s\n", vblMs / f,
                             std::max(0.0, eeWaitMs - vblMs) / f, drainMs / f, sites);
            }
            held = 0;
            backlogSum = 0.0;
            startWindow(now);
        }
    } s_sims2FpsGate;

    void sims2Vblank60(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == 0x3EAE78u) // fresh entry, not a resume inside the handler
        {
            bool flipNow = true;
            if (s_sims2FpsGate.adaptive)
            {
                s_sims2FpsGate.onVblank();
                flipNow = s_sims2FpsGate.at60 && !s_sims2FpsGate.holdNow;
            }
            const uint32_t sinceFlip = (GPR_U32(ctx, 4) + 0x94u) & 0x1FFFFFCu;
            uint32_t n = 0;
            std::memcpy(&n, rdram + sinceFlip, sizeof(n));
            if (flipNow && n == 0u)
            {
                n = 1u;
                std::memcpy(rdram + sinceFlip, &n, sizeof(n));
            }
        }
        s_sims2VblankOriginal(rdram, ctx, runtime);
    }

    void applySims2FrameRateOverride(PS2Runtime &runtime, uint8_t *rdram)
    {
        const char *env = std::getenv("PS2X_60FPS");
        if (!env || *env == '\0' || *env == '0' || !rdram)
        {
            return;
        }
        // Match the handler's code before patching (other games / builds keep their own pacing).
        struct Word { uint32_t address, value; };
        static constexpr Word kExpected[] = {
            {0x3EAEB0u, 0x8E02008Cu}, // lw    v0, 0x8C(s0)
            {0x3EAEB4u, 0x24840001u}, // addiu a0, a0, 1
            {0x3EAEC0u, 0xAE040094u}, // sw    a0, 0x94(s0)
            {0x3EAEF4u, 0x28820002u}, // slti  v0, a0, 2
            {0x3EB190u, 0x0C0FAB9Eu}, // jal   0x3EAE78 (in the INTC stub)
        };
        for (const Word &word : kExpected)
        {
            uint32_t value = 0;
            std::memcpy(&value, rdram + word.address, sizeof(value));
            if (value != word.value)
            {
                std::cerr << "[60fps] PS2X_60FPS ignored: VBlank handler code not recognised at 0x"
                          << std::hex << word.address << std::dec << std::endl;
                return;
            }
        }
        s_sims2VblankOriginal = runtime.lookupFunction(0x3EAE78u);
        if (!s_sims2VblankOriginal || !runtime.replaceFunction(0x3EAE78u, &sims2Vblank60))
        {
            std::cerr << "[60fps] PS2X_60FPS ignored: cannot hook 0x3EAE78" << std::endl;
            return;
        }
        s_sims2FpsGate.adaptive = std::strcmp(env, "always") != 0;
        if (const char *hold = std::getenv("PS2X_60FPS_HOLD"); hold && *hold)
            s_sims2FpsGate.holdMs = std::atof(hold);
        if (const char *wait = std::getenv("PS2X_60FPS_HOLD_WAIT"); wait && *wait)
            s_sims2FpsGate.holdWaitMs = std::atof(wait);
        const char *logEnv = std::getenv("PS2X_60FPS_LOG");
        s_sims2FpsGate.log = logEnv && *logEnv && *logEnv != '0';
        if (s_sims2FpsGate.adaptive)
            std::cerr << "[60fps] adaptive: flip on every VBlank while frames keep up, else every 2nd (PS2X_60FPS); "
                         "not bit-exact with 30 FPS runs" << std::endl;
        else
            std::cerr << "[60fps] flip on every VBlank (PS2X_60FPS=always); not bit-exact with 30 FPS runs" << std::endl;
    }

    // Widescreen: the game asks its display object for the aspect (two vtable getters that
    // return 4/3) and passes it to the perspective builder at 0x401198 (cot(fov/2)/aspect in
    // m[0]). Returning 16/9 keeps the vertical FOV and widens the horizontal one; the host
    // presenter then shows the 640x448 frame at 16:9, so the picture is anamorphic, not stretched.
    constexpr uint32_t kFloat4x3 = 0x3FAAAAABu;
    constexpr float kWideAspect = 16.0f / 9.0f;
    PS2Runtime::RecompiledFunction s_sims2PerspectiveOriginal = nullptr;

    void sims2WideAspectGetter(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        std::memcpy(&ctx->f[0], &kWideAspect, sizeof(kWideAspect));
        ctx->pc = GPR_U32(ctx, 31);
    }

    void sims2WidePerspective(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == 0x401198u) // fresh entry: f13 = aspect; some callers pass a literal 4/3
        {
            uint32_t bits = 0;
            std::memcpy(&bits, &ctx->f[13], sizeof(bits));
            if (bits == kFloat4x3)
                std::memcpy(&ctx->f[13], &kWideAspect, sizeof(kWideAspect));
        }
        s_sims2PerspectiveOriginal(rdram, ctx, runtime);
    }

    bool sims2WidescreenRequested()
    {
        if (const char *env = std::getenv("PS2X_WIDESCREEN"); env && *env)
            return *env != '0';
        const char *aspect = std::getenv("PS2X_ASPECT");
        return aspect && std::strcmp(aspect, "16:9") == 0;
    }

    void applySims2WidescreenOverride(PS2Runtime &runtime, uint8_t *rdram)
    {
        if (!rdram || !sims2WidescreenRequested())
            return;
        struct Word { uint32_t address, value; };
        static constexpr Word kExpected[] = {
            {0x3DB168u, 0x3C013FAAu}, // lui at, 0x3FAA (aspect getter, display class)
            {0x3DB16Cu, 0x3421AAABu},
            {0x40A110u, 0x3C013FAAu}, // lui at, 0x3FAA (aspect getter, base display class)
            {0x40A114u, 0x3421AAABu},
            {0x4011C8u, 0x0C0EEFF0u}, // jal 0x3BBFC0 (perspective matrix)
        };
        for (const Word &word : kExpected)
        {
            uint32_t value = 0;
            std::memcpy(&value, rdram + word.address, sizeof(value));
            if (value != word.value)
            {
                std::cerr << "[widescreen] ignored: projection code not recognised at 0x"
                          << std::hex << word.address << std::dec << std::endl;
                return;
            }
        }
        s_sims2PerspectiveOriginal = runtime.lookupFunction(0x401198u);
        if (!s_sims2PerspectiveOriginal ||
            !runtime.replaceFunction(0x3DB168u, &sims2WideAspectGetter) ||
            !runtime.replaceFunction(0x40A110u, &sims2WideAspectGetter) ||
            !runtime.replaceFunction(0x401198u, &sims2WidePerspective))
        {
            std::cerr << "[widescreen] ignored: cannot hook the projection code" << std::endl;
            return;
        }
        std::cerr << "[widescreen] 16:9 field of view enabled" << std::endl;
    }
}

namespace
{
    // Online co-op: the game draws two side-by-side views (func 0x18ABC8, one call per player:
    // aspect halved, viewport rect x0 y0 x1 y1 = left or right half). Both machines run the same
    // patch so the EE stays in sync: each view is drawn over the whole screen at the full aspect,
    // and its height is cut by one or two rows so its GS scissor tells the views apart. The GS on
    // each machine then drops the other player's view (GS::setCoopView).
    PS2Runtime::RecompiledFunction s_coopPerspectiveOriginal = nullptr;
    PS2Runtime::RecompiledFunction s_coopViewRectOriginal = nullptr;
    PS2Runtime::RecompiledFunction s_coopViewportOriginal = nullptr;
    bool s_coopFullscreen = false;

    void coopPerspective(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == 0x401198u && GPR_U32(ctx, 31) == 0x18AD64u)
            ctx->f[13] *= 2.0f;
        s_coopPerspectiveOriginal(rdram, ctx, runtime);
    }

    void coopFullscreenRect(uint8_t *rdram, R5900Context *ctx, int player)
    {
        const float rect[4] = {0.0f, 0.0f, 1.0f, 1.0f - static_cast<float>(player + 1) / 448.0f};
        std::memcpy(rdram + (GPR_U32(ctx, 5) & PS2_RAM_MASK), rect, sizeof(rect));
    }

    void coopViewRect(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // 0x4012F0(cam, rect)
    {
        if (ctx->pc == 0x4012F0u)
        {
            const uint32_t ra = GPR_U32(ctx, 31);
            if (ra == 0x18AD98u || ra == 0x18ADDCu)
                coopFullscreenRect(rdram, ctx, ra == 0x18AD98u ? 0 : 1);
        }
        s_coopViewRectOriginal(rdram, ctx, runtime);
    }

    void coopViewport(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) // 0x4160F0(cam, rect)
    {
        if (ctx->pc == 0x4160F0u)
        {
            const uint32_t ra = GPR_U32(ctx, 31);
            if (ra == 0x18ADB4u || ra == 0x18ADF8u)
                coopFullscreenRect(rdram, ctx, ra == 0x18ADB4u ? 0 : 1);
        }
        s_coopViewportOriginal(rdram, ctx, runtime);
    }

    // The view this machine shows: PS2X_COOP_VIEW=0/1 forces it (local split-screen testing).
    int coopForcedView()
    {
        const char *e = std::getenv("PS2X_COOP_VIEW");
        return (e && (*e == '0' || *e == '1')) ? *e - '0' : -1;
    }

    void applySims2CoopFullscreen(PS2Runtime &runtime, uint8_t *rdram)
    {
        const char *netplay = std::getenv("PS2X_NETPLAY");
        const bool split = std::getenv("PS2X_NETPLAY_SPLIT") && *std::getenv("PS2X_NETPLAY_SPLIT") != '0';
        if (!rdram || split || (coopForcedView() < 0 && !(netplay && *netplay && *netplay != '0')))
            return;
        struct Word { uint32_t address, value; };
        static constexpr Word kExpected[] = {
            {0x18AD5Cu, 0x0C100466u}, // jal 0x401198 (split view projection)
            {0x18AD90u, 0x0C1004BCu}, // jal 0x4012F0 (player 0 view rect)
            {0x18ADACu, 0x0C10583Cu}, // jal 0x4160F0
            {0x18ADD4u, 0x0C1004BCu}, // jal 0x4012F0 (player 1 view rect)
            {0x18ADF0u, 0x0C10583Cu}, // jal 0x4160F0
        };
        for (const Word &word : kExpected)
        {
            uint32_t value = 0;
            std::memcpy(&value, rdram + word.address, sizeof(value));
            if (value != word.value)
            {
                std::cerr << "[coop] full-screen views ignored: split-screen code not recognised at 0x"
                          << std::hex << word.address << std::dec << std::endl;
                return;
            }
        }
        s_coopPerspectiveOriginal = runtime.lookupFunction(0x401198u);
        s_coopViewRectOriginal = runtime.lookupFunction(0x4012F0u);
        s_coopViewportOriginal = runtime.lookupFunction(0x4160F0u);
        if (!s_coopPerspectiveOriginal || !s_coopViewRectOriginal || !s_coopViewportOriginal ||
            !runtime.replaceFunction(0x401198u, &coopPerspective) ||
            !runtime.replaceFunction(0x4012F0u, &coopViewRect) ||
            !runtime.replaceFunction(0x4160F0u, &coopViewport))
        {
            std::cerr << "[coop] full-screen views ignored: cannot hook the split-screen code" << std::endl;
            return;
        }
        s_coopFullscreen = true;
        std::cerr << "[coop] split-screen views drawn full screen" << std::endl;
    }
}

bool PS2Runtime::loadELF(const std::string &elfPath)
{
    configureIoPathsFromElf(elfPath);

    std::ifstream file(elfPath, std::ios::binary);
    if (!file)
    {
        std::cerr << "Failed to open ELF file: " << elfPath << std::endl;
        return false;
    }

    file.seekg(0, std::ios::end);
    const std::streamoff fileSize = file.tellg();
    if (fileSize < static_cast<std::streamoff>(sizeof(ElfHeader)))
    {
        std::cerr << "ELF file is too small: " << elfPath << std::endl;
        return false;
    }
    file.seekg(0, std::ios::beg);

    ElfHeader header{};
    if (!file.read(reinterpret_cast<char *>(&header), sizeof(header)))
    {
        std::cerr << "Failed to read ELF header from: " << elfPath << std::endl;
        return false;
    }

    if (header.magic != ELF_MAGIC)
    {
        std::cerr << "Invalid ELF magic number" << std::endl;
        return false;
    }

    if (header.elf_class != 1u || header.endianness != 1u)
    {
        std::cerr << "Unsupported ELF format (expected 32-bit little-endian)." << std::endl;
        return false;
    }

    if (header.machine != EM_MIPS || header.type != ET_EXEC)
    {
        std::cerr << "Not a MIPS executable ELF file" << std::endl;
        return false;
    }

    if (header.phnum != 0u && header.phentsize < sizeof(ProgramHeader))
    {
        std::cerr << "Unsupported ELF program-header entry size: " << header.phentsize << std::endl;
        return false;
    }

    const uint64_t programHeaderTableEnd =
        static_cast<uint64_t>(header.phoff) +
        static_cast<uint64_t>(header.phnum) * static_cast<uint64_t>(header.phentsize);
    if (programHeaderTableEnd > static_cast<uint64_t>(fileSize))
    {
        std::cerr << "ELF program-header table is out of range." << std::endl;
        return false;
    }

    m_cpuContext.pc = header.entry;
    m_debugPc.store(m_cpuContext.pc, std::memory_order_relaxed);

    uint32_t maxLoadedRdramEnd = kGuestHeapDefaultBase;
    uint32_t moduleBase = std::numeric_limits<uint32_t>::max();
    uint32_t moduleEnd = 0u;
    bool loadedAnySegment = false;

    for (uint16_t i = 0; i < header.phnum; i++)
    {
        const uint64_t phOffset =
            static_cast<uint64_t>(header.phoff) +
            static_cast<uint64_t>(i) * static_cast<uint64_t>(header.phentsize);
        if (phOffset + sizeof(ProgramHeader) > static_cast<uint64_t>(fileSize))
        {
            std::cerr << "ELF program header " << i << " is out of range." << std::endl;
            return false;
        }

        ProgramHeader ph{};
        file.seekg(static_cast<std::streamoff>(phOffset), std::ios::beg);
        if (!file.read(reinterpret_cast<char *>(&ph), sizeof(ph)))
        {
            std::cerr << "Failed to read ELF program header " << i << std::endl;
            return false;
        }

        if (ph.type != PT_LOAD || ph.memsz == 0u)
        {
            continue;
        }

        if (ph.filesz > ph.memsz)
        {
            std::cerr << "ELF segment " << i << " has filesz > memsz." << std::endl;
            return false;
        }

        const uint64_t segmentFileEnd = static_cast<uint64_t>(ph.offset) + static_cast<uint64_t>(ph.filesz);
        if (segmentFileEnd > static_cast<uint64_t>(fileSize))
        {
            std::cerr << "ELF segment " << i << " exceeds file bounds." << std::endl;
            return false;
        }

        const bool scratch =
            ph.vaddr >= PS2_SCRATCHPAD_BASE &&
            ph.vaddr < (PS2_SCRATCHPAD_BASE + PS2_SCRATCHPAD_SIZE);

        uint32_t physAddr = 0u;
        try
        {
            physAddr = m_memory.translateAddress(ph.vaddr);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Failed to translate ELF segment " << i
                      << " virtual address 0x" << std::hex << ph.vaddr
                      << std::dec << ": " << e.what() << std::endl;
            return false;
        }
        const uint64_t regionSize = scratch ? static_cast<uint64_t>(PS2_SCRATCHPAD_SIZE)
                                            : static_cast<uint64_t>(PS2_RAM_SIZE);
        const uint64_t segmentMemEnd = static_cast<uint64_t>(physAddr) + static_cast<uint64_t>(ph.memsz);
        if (segmentMemEnd > regionSize)
        {
            std::cerr << "ELF segment " << i << " exceeds "
                      << (scratch ? "scratchpad" : "RDRAM")
                      << " bounds (vaddr=0x" << std::hex << ph.vaddr
                      << " memsz=0x" << ph.memsz << std::dec << ")." << std::endl;
            return false;
        }

        uint8_t *destBase = scratch ? m_memory.getScratchpad() : m_memory.getRDRAM();
        if (!destBase)
        {
            std::cerr << "ELF segment " << i << " has no destination memory backing." << std::endl;
            return false;
        }

        uint8_t *dest = destBase + physAddr;
        if (ph.filesz > 0u)
        {
            file.seekg(static_cast<std::streamoff>(ph.offset), std::ios::beg);
            if (!file.read(reinterpret_cast<char *>(dest), ph.filesz))
            {
                std::cerr << "Failed to read ELF segment " << i << " payload." << std::endl;
                return false;
            }
        }

        if (ph.memsz > ph.filesz)
        {
            std::memset(dest + ph.filesz, 0, ph.memsz - ph.filesz);
        }

        RUNTIME_LOG("Loading segment: 0x" << std::hex << ph.vaddr
                                          << " - 0x" << (static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.memsz))
                                          << " (filesz: 0x" << ph.filesz
                                          << ", memsz: 0x" << ph.memsz << ")"
                                          << std::dec << std::endl);

        if (!scratch)
        {
            maxLoadedRdramEnd = std::max(maxLoadedRdramEnd, static_cast<uint32_t>(segmentMemEnd));
        }

        if (ph.flags & 0x1u) // PF_X
        {
            const uint64_t execEnd = static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.filesz);
            if (execEnd <= std::numeric_limits<uint32_t>::max())
            {
                m_memory.registerCodeRegion(ph.vaddr, static_cast<uint32_t>(execEnd));
            }
        }

        loadedAnySegment = true;
        moduleBase = std::min(moduleBase, ph.vaddr);
        const uint64_t segmentVirtualEnd = static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.memsz);
        const uint32_t clampedVirtualEnd =
            (segmentVirtualEnd > std::numeric_limits<uint32_t>::max())
                ? std::numeric_limits<uint32_t>::max()
                : static_cast<uint32_t>(segmentVirtualEnd);
        moduleEnd = std::max(moduleEnd, clampedVirtualEnd);
    }

    if (!loadedAnySegment)
    {
        std::cerr << "ELF contains no loadable PT_LOAD segments." << std::endl;
        return false;
    }

    if (maxLoadedRdramEnd > PS2_RAM_SIZE)
    {
        maxLoadedRdramEnd = PS2_RAM_SIZE;
    }

    const uint32_t paddedEnd = (maxLoadedRdramEnd > (PS2_RAM_SIZE - kGuestHeapSafetyPad))
                                   ? PS2_RAM_SIZE
                                   : (maxLoadedRdramEnd + kGuestHeapSafetyPad);
    const uint32_t suggestedHeapBase = alignGuestHeapValue(paddedEnd, kGuestHeapDefaultAlignment);
    {
        std::lock_guard<std::mutex> lock(m_guestHeapMutex);
        if (!m_guestHeapConfigured)
        {
            const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
            m_guestHeapSuggestedBase = std::min(suggestedHeapBase, hardLimit);
            m_guestHeapBase = m_guestHeapSuggestedBase;
            m_guestHeapEnd = m_guestHeapSuggestedBase;
            m_guestHeapLimit = hardLimit;
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_asyncCallbackStackMutex);
        const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
        m_asyncCallbackStackFloor = std::min(std::max(hardLimit, suggestedHeapBase), PS2_RAM_SIZE);
        m_asyncCallbackStackTop = PS2_RAM_SIZE;
    }

    {
        // The private runtime region (0x80000-0x100000) is usable when nothing was loaded below 1MB.
        const uint32_t lowestLoad = (moduleBase == std::numeric_limits<uint32_t>::max()) ? 0x00100000u : moduleBase;
        PrivateArena &arena = privateArena();
        std::lock_guard<std::mutex> arenaLock(arena.mutex);
        arena.regionUsable = lowestLoad >= kPrivateStackTop;
    }

    LoadedModule module;
    module.name = elfPath.substr(elfPath.find_last_of("/\\") + 1);
    module.baseAddress = (moduleBase == std::numeric_limits<uint32_t>::max()) ? 0x00100000u : moduleBase;
    module.size = (moduleEnd > module.baseAddress) ? static_cast<size_t>(moduleEnd - module.baseAddress) : 0u;
    module.active = true;

    m_loadedModules.push_back(module);

    uint32_t elfCrc32 = 0u;
    const bool elfCrc32Valid = computeFileCrc32(elfPath, elfCrc32);
    if (!elfCrc32Valid)
    {
        std::cerr << "[ps2xIOP] failed to compute ELF CRC32 for '" << elfPath << "'" << std::endl;
    }
    ps2x::iop::GameIdentity identity;
    identity.elfName = module.name;
    identity.entryPoint = m_cpuContext.pc;
    identity.crc32 = elfCrc32;
    std::string romError;
    if (!m_romDevice.configure(identity, &romError))
    {
        std::cerr << "[ROM0] failed to configure profile: " << romError << std::endl;
        return false;
    }

    m_iopSubsystem->reset();

    ps2_game_overrides::applyMatching(*this,
                                      elfPath,
                                      m_cpuContext.pc,
                                      elfCrc32,
                                      elfCrc32Valid);
    applySims2FrameRateOverride(*this, m_memory.getRDRAM());
    applySims2WidescreenOverride(*this, m_memory.getRDRAM());
    applySims2CoopFullscreen(*this, m_memory.getRDRAM());

    RUNTIME_LOG("ELF file loaded successfully. Entry point: 0x" << std::hex << m_cpuContext.pc << std::dec);
    return true;
}

const PS2Runtime::IoPaths &PS2Runtime::getIoPaths()
{
    return runtimeIoPaths();
}

void PS2Runtime::setIoPaths(const IoPaths &paths)
{
    IoPaths normalized = paths;
    normalized.elfPath = normalizeAbsolutePath(normalized.elfPath);
    normalized.elfDirectory = normalizeAbsolutePath(normalized.elfDirectory);
    normalized.hostRoot = normalizeAbsolutePath(normalized.hostRoot);
    normalized.cdRoot = normalizeAbsolutePath(normalized.cdRoot);
    normalized.mcRoot = normalizeAbsolutePath(normalized.mcRoot);
    normalized.cdImage = normalizeAbsolutePath(normalized.cdImage);

    if (normalized.elfDirectory.empty() && !normalized.elfPath.empty())
    {
        normalized.elfDirectory = normalized.elfPath.parent_path();
    }

    if (normalized.hostRoot.empty())
    {
        normalized.hostRoot = normalized.elfDirectory;
    }
    if (normalized.cdRoot.empty())
    {
        normalized.cdRoot = normalized.elfDirectory;
    }
    if (normalized.mcRoot.empty())
    {
        normalized.mcRoot = normalized.elfDirectory / "mc0";
    }

    runtimeIoPaths() = normalized;
}

void PS2Runtime::configureIoPathsFromElf(const std::string &elfPath)
{
    IoPaths paths = runtimeIoPaths();
    paths.elfPath = normalizeAbsolutePath(std::filesystem::path(elfPath));
    if (!paths.elfPath.empty())
    {
        paths.elfDirectory = paths.elfPath.parent_path();
    }

    if (!paths.elfDirectory.empty())
    {
        paths.hostRoot = paths.elfDirectory;
        paths.cdRoot = paths.elfDirectory;
        paths.mcRoot = paths.elfDirectory / "mc0";
    }
    // PS2X_MC_ROOT=<dir>: use another memory card 0 folder (e.g. a copy, for test runs that must
    // not touch the real saves).
    if (const char *mcRoot = std::getenv("PS2X_MC_ROOT"); mcRoot && *mcRoot)
        paths.mcRoot = std::filesystem::path(mcRoot);

    setIoPaths(paths);
}

namespace
{
    bool generatedFunctionTableSlot(uint32_t address, uint32_t &slot)
    {
        if ((address & 3u) != 0u || g_ps2RecompiledFunctionTableSlotCount == 0u)
        {
            return false;
        }

        if (address < g_ps2RecompiledFunctionTableBase || address >= g_ps2RecompiledFunctionTableEnd)
        {
            return false;
        }

        const uint32_t offset = address - g_ps2RecompiledFunctionTableBase;
        slot = offset >> 2;
        return slot < g_ps2RecompiledFunctionTableSlotCount;
    }
}

bool PS2Runtime::replaceFunction(uint32_t address, RecompiledFunction func)
{
    uint32_t slot = 0u;
    if (!generatedFunctionTableSlot(address, slot))
    {
        std::cerr << "[function-table] cannot replace guest PC 0x" << std::hex << address
                  << ": outside generated dense table [0x" << g_ps2RecompiledFunctionTableBase
                  << ", 0x" << g_ps2RecompiledFunctionTableEnd << ")"
                  << std::dec << std::endl;
        return false;
    }

    g_ps2RecompiledFunctionTable[slot] = func;
    return true;
}

bool PS2Runtime::registerFunction(uint32_t address, RecompiledFunction func)
{
    return replaceFunction(address, func);
}

bool PS2Runtime::hasFunction(uint32_t address) const
{
    uint32_t slot = 0u;
    return generatedFunctionTableSlot(address, slot) && g_ps2RecompiledFunctionTable[slot] != nullptr;
}

const char *describeGuestBranchKind(PS2Runtime::GuestBranchKind kind)
{
    switch (kind)
    {
    case PS2Runtime::GuestBranchKind::DirectJump:
        return "DirectJump";
    case PS2Runtime::GuestBranchKind::DirectCall:
        return "DirectCall";
    case PS2Runtime::GuestBranchKind::IndirectJump:
        return "IndirectJump";
    case PS2Runtime::GuestBranchKind::IndirectCall:
        return "IndirectCall";
    case PS2Runtime::GuestBranchKind::Return:
        return "Return";
    default:
        return "Unknown";
    }
}

PS2Runtime::RecompiledFunction PS2Runtime::lookupFunction(uint32_t address)
{
    pushDispatchPc(address);

    uint32_t slot = 0u;
    if (generatedFunctionTableSlot(address, slot))
    {
        RecompiledFunction fn = g_ps2RecompiledFunctionTable[slot];
        if (fn != nullptr)
        {
            return fn;
        }
    }

    std::cerr << "Error: No exact recompiled function for guest PC 0x" << std::hex << address
              << " tableBase=0x" << g_ps2RecompiledFunctionTableBase
              << " tableEnd=0x" << g_ps2RecompiledFunctionTableEnd
              << " codeRegion=" << (m_memory.isCodeAddress(address) ? "yes" : "no")
              << " trace=" << formatDispatchHistory()
              << std::dec << std::endl;

    static RecompiledFunction missingFunction = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t badPc = ctx->pc;
        runtime->reportMissingFunction(rdram,
                                       ctx,
                                       badPc,
                                       0u,
                                       PS2Runtime::GuestBranchKind::IndirectJump,
                                       "dispatch");
    };

    return missingFunction;
}

void PS2Runtime::setMissingFunctionPolicy(MissingFunctionPolicy policy)
{
    m_missingFunctionPolicy.store(static_cast<uint32_t>(policy), std::memory_order_release);
}

PS2Runtime::MissingFunctionPolicy PS2Runtime::missingFunctionPolicy() const
{
    return static_cast<MissingFunctionPolicy>(m_missingFunctionPolicy.load(std::memory_order_acquire));
}

void PS2Runtime::resetMissingFunctionReportOnce()
{
    m_missingFunctionReported.store(false, std::memory_order_release);
}

void PS2Runtime::reportMissingFunction(uint8_t *rdram,
                                       R5900Context *ctx,
                                       uint32_t targetPc,
                                       uint32_t sourcePc,
                                       GuestBranchKind kind,
                                       const char *debugName)
{
    const MissingFunctionPolicy policy = missingFunctionPolicy();
    const bool firstReport = !m_missingFunctionReported.exchange(true, std::memory_order_acq_rel);

    const uint32_t pc = ctx->pc;
    const uint32_t ra = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0));
    const uint32_t sp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0));
    const uint32_t gp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[28], 0));
    const uint32_t a0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0));
    const uint32_t a1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[5], 0));
    const uint32_t a2 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[6], 0));
    const uint32_t a3 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[7], 0));
    const uint32_t s0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[16], 0));
    const uint32_t s1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[17], 0));
    const uint32_t v0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[2], 0));
    const uint32_t v1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[3], 0));

    auto readGuestU32At = [rdram](uint32_t addr, uint32_t &out) -> bool
    {
        // TODO this !rdram exist only because of test fix those test later
        if (!rdram || addr > PS2_RAM_SIZE - sizeof(uint32_t))
        {
            out = 0u;
            return false;
        }

        std::memcpy(&out, rdram + addr, sizeof(uint32_t));
        return true;
    };

    auto readGuestU32Offset = [&readGuestU32At](uint32_t base, uint32_t offset, uint32_t &out) -> bool
    {
        if (base > PS2_RAM_SIZE - sizeof(uint32_t) || offset > PS2_RAM_SIZE - sizeof(uint32_t) - base)
        {
            out = 0u;
            return false;
        }

        return readGuestU32At(base + offset, out);
    };

    uint32_t a0Word0 = 0u;
    uint32_t a0Word4 = 0u;
    uint32_t a0Word8 = 0u;
    uint32_t a0WordC = 0u;
    const bool a0Readable =
        readGuestU32Offset(a0, 0x00u, a0Word0) &&
        readGuestU32Offset(a0, 0x04u, a0Word4) &&
        readGuestU32Offset(a0, 0x08u, a0Word8) &&
        readGuestU32Offset(a0, 0x0cu, a0WordC);

    uint32_t s0Word0 = 0u;
    uint32_t s0Word4 = 0u;
    uint32_t s0Word8 = 0u;
    uint32_t s0WordC = 0u;
    const bool s0Readable =
        readGuestU32Offset(s0, 0x00u, s0Word0) &&
        readGuestU32Offset(s0, 0x04u, s0Word4) &&
        readGuestU32Offset(s0, 0x08u, s0Word8) &&
        readGuestU32Offset(s0, 0x0cu, s0WordC);

    uint32_t recordWord0 = 0u;
    uint32_t recordWord4 = 0u;
    uint32_t recordWord8 = 0u;
    uint32_t recordWordC = 0u;
    const bool recordReadable =
        s0Readable && s0Word4 != 0u &&
        readGuestU32Offset(s0Word4, 0x00u, recordWord0) &&
        readGuestU32Offset(s0Word4, 0x04u, recordWord4) &&
        readGuestU32Offset(s0Word4, 0x08u, recordWord8) &&
        readGuestU32Offset(s0Word4, 0x0cu, recordWordC);

    uint32_t vtableSlot0 = 0u;
    uint32_t vtableSlot4 = 0u;
    uint32_t vtableSlot8 = 0u;
    uint32_t vtableSlotC = 0u;
    const bool vtableReadable =
        a0Readable && a0Word0 != 0u &&
        readGuestU32Offset(a0Word0, 0x00u, vtableSlot0) &&
        readGuestU32Offset(a0Word0, 0x04u, vtableSlot4) &&
        readGuestU32Offset(a0Word0, 0x08u, vtableSlot8) &&
        readGuestU32Offset(a0Word0, 0x0cu, vtableSlotC);

    if (firstReport)
    {
        std::ostringstream oss;
        oss << "[guest-branch:missing-target] kind=" << describeGuestBranchKind(kind)
            << " op=" << (debugName ? debugName : "<unknown>")
            << " source=0x" << std::hex << sourcePc
            << " target=0x" << targetPc
            << " pc=0x" << pc
            << " ra=0x" << ra
            << " sp=0x" << sp
            << " gp=0x" << gp
            << " a0=0x" << a0
            << " a1=0x" << a1
            << " a2=0x" << a2
            << " a3=0x" << a3
            << " s0=0x" << s0
            << " s1=0x" << s1
            << " v0=0x" << v0
            << " v1=0x" << v1
            << " a0Readable=" << (a0Readable ? "yes" : "no")
            << " a0[0]=0x" << a0Word0
            << " a0[4]=0x" << a0Word4
            << " a0[8]=0x" << a0Word8
            << " a0[c]=0x" << a0WordC
            << " s0Readable=" << (s0Readable ? "yes" : "no")
            << " s0[0]=0x" << s0Word0
            << " s0[4]=0x" << s0Word4
            << " s0[8]=0x" << s0Word8
            << " s0[c]=0x" << s0WordC
            << " recordReadable=" << (recordReadable ? "yes" : "no")
            << " record[0]=0x" << recordWord0
            << " record[4]=0x" << recordWord4
            << " record[8]=0x" << recordWord8
            << " record[c]=0x" << recordWordC
            << " vtableReadable=" << (vtableReadable ? "yes" : "no")
            << " vtbl[0]=0x" << vtableSlot0
            << " vtbl[4]=0x" << vtableSlot4
            << " vtbl[8]=0x" << vtableSlot8
            << " vtbl[c]=0x" << vtableSlotC
            << " codeRegion=" << (m_memory.isCodeAddress(targetPc) ? "yes" : "no")
            << " policy=" << static_cast<uint32_t>(policy)
            << " trace=" << formatDispatchHistory()
            << std::dec;

        static std::mutex s_missingFunctionLogMutex;
        {
            std::lock_guard<std::mutex> lock(s_missingFunctionLogMutex);
            std::cerr << oss.str() << std::endl;
            dumpDispatchRing(600u);
        }
    }

    if (firstReport && policy == MissingFunctionPolicy::BreakOnce)
    {
#if defined(_MSC_VER)
        __debugbreak();
#endif // TODO others breakpoints
    }

    if (ctx)
    {
        ctx->pc = targetPc;
    }

    if (policy == MissingFunctionPolicy::Stop)
    {
        requestStop();
    }
}

static inline bool isCallKindForPrint(PS2Runtime::GuestBranchKind kind)
{
    return kind == PS2Runtime::GuestBranchKind::DirectCall || kind == PS2Runtime::GuestBranchKind::IndirectCall;
}


// COP0 Count: the EE increments it at the CPU clock (294.912 MHz). Generated code reads and writes
// it through these helpers so it advances with host time (games use it as a high-resolution clock;
// a frozen Count stops their UI timers, e.g. The Sims 2's Apt menus never advance a frame).
// ctx->cop0_count holds an offset so that MTC0 Count still sets the visible value.
uint32_t ps2ReadCop0Count(R5900Context *ctx)
{
    // Co-op: host time differs per machine, so Count follows emulated EE cycles instead (what the
    // hardware does), the same clock the SPU2 audio runs on. It used to step once per input exchange
    // plus 4096 per read: frequent readers pushed it frames ahead, then it stalled, and cutscene timing
    // built on it drifted against the audio (stutter). Nudged by 64 per read so a spin loop sees it move.
    int64_t frame = 0, epoch = 0;
    if (ps2NetplayClock(frame, epoch))
    {
        extern std::atomic<uint64_t> g_ps2xEeCycleNow; // EeScheduler.cpp
        static std::atomic<uint64_t> s_last{0};
        const uint64_t base = g_ps2xEeCycleNow.load(std::memory_order_relaxed);
        uint64_t prev = s_last.load(std::memory_order_relaxed), next;
        do
            next = std::max<uint64_t>(base, prev + 64u);
        while (!s_last.compare_exchange_weak(prev, next, std::memory_order_relaxed));
        return ctx->cop0_count + static_cast<uint32_t>(next);
    }
    static const auto t0 = std::chrono::steady_clock::now();
    const uint64_t us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
    const uint64_t cycles = us * 294912ull / 1000ull;
    return ctx->cop0_count + static_cast<uint32_t>(cycles);
}

void ps2WriteCop0Count(R5900Context *ctx, uint32_t value)
{
    const uint32_t visible = ps2ReadCop0Count(ctx);
    ctx->cop0_count += value - visible;
}

// Debug hooks of dispatchGuestBranch (PS2X_CALL_TRACE, PS2X_OBJDUMP, PS2X_GAME_PRINT[_AUTO],
// PS2X_FUNC_PROF): kept out of the hot path; dispatchGuestBranch calls this only if one is set.
void PS2Runtime::dispatchDebugHooks(uint8_t *rdram, R5900Context *ctx, uint32_t targetPc, uint32_t sourcePc, GuestBranchKind kind)
{
    {
        // PS2X_CALL_TRACE=0xADDR: log targets (and v0 on return) of calls made from guest address ADDR.
        static const uint32_t traceFrom = [] { const char *e = std::getenv("PS2X_CALL_TRACE"); return e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 0)) : 0u; }();
        static uint32_t traced = 0;
        static uint32_t seenCalls = 0;
        if (traceFrom != 0u && sourcePc == traceFrom && (++seenCalls <= 64u || (seenCalls % 50u) == 0u))
        {
            ++traced;
            std::fprintf(stderr, "[call] from=0x%x target=0x%x a0=0x%x a1=0x%x f12=%g f0=%g\n", sourcePc, targetPc, getRegU32(ctx, 4), getRegU32(ctx, 5), ctx->f[12], ctx->f[0]);
        }
    }

    {
        // PS2X_OBJDUMP=0xFUNC[:len[:secs]]: on calls to FUNC print <len> bytes at a0 (default 0x200),
        // at most once every <secs> seconds (default 10), to watch an object's fields over time.
        static uint32_t odFunc = 0, odLen = 0x200; static double odSecs = 10.0; static bool odInit = false;
        if (!odInit)
        {
            odInit = true;
            if (const char *e = std::getenv("PS2X_OBJDUMP"))
            {
                char *end = nullptr; odFunc = static_cast<uint32_t>(std::strtoul(e, &end, 0));
                if (end && *end == ':') { odLen = static_cast<uint32_t>(std::strtoul(end + 1, &end, 0)); if (end && *end == ':') odSecs = std::atof(end + 1); }
            }
        }
        if (odFunc != 0u && targetPc == odFunc)
        {
            static auto last = std::chrono::steady_clock::now() - std::chrono::hours(1);
            const auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration<double>(now - last).count() >= odSecs)
            {
                last = now;
                const uint32_t a0 = getRegU32(ctx, 4) & 0x01FFFFFCu;
                std::fprintf(stderr, "[objdump %x] a0=%08x a1=%08x ra=%08x\n", odFunc, getRegU32(ctx, 4), getRegU32(ctx, 5), getRegU32(ctx, 31));
                for (uint32_t off = 0; off < odLen && a0 + off + 16 <= 0x02000000u; off += 16)
                {
                    uint32_t w[4]; std::memcpy(w, rdram + a0 + off, 16);
                    std::fprintf(stderr, "  +%03x: %08x %08x %08x %08x\n", off, w[0], w[1], w[2], w[3]);
                }
            }
        }
    }
    {
        // PS2X_GAME_PRINT=0xADDR[,0xADDR...]: treat calls to these guest functions as printf(fmt=a0, ...)
        // (or with fmt in a1 when the address is prefixed by '1:') and print the formatted text.
        // Retail games often keep their debug-log calls with an empty logger; this shows them.
        static const std::vector<std::pair<uint32_t, int>> printHooks = [] {
            std::vector<std::pair<uint32_t, int>> v;
            if (const char *e = std::getenv("PS2X_GAME_PRINT"))
                for (const char *p = e; *p;)
                {
                    int fmtReg = 4;
                    if (p[0] == '1' && p[1] == ':') { fmtReg = 5; p += 2; }
                    char *end = nullptr;
                    const uint32_t a = static_cast<uint32_t>(std::strtoul(p, &end, 0));
                    if (end == p) break;
                    v.push_back({a, fmtReg});
                    p = end;
                    while (*p == ',') ++p;
                }
            return v;
        }();
        {
            // PS2X_GAME_PRINT_AUTO=1: for every call whose a0 or a1 points at a read-only string
            // (0x49DC80-0x518500 in Sims 2) print "<target>: <string>" once per (target, string).
            // PS2X_GAME_PRINT_AUTO=2: also strings anywhere in RAM (heap), e.g. Apt variable names.
            static const int autoMode = [] { const char *e = std::getenv("PS2X_GAME_PRINT_AUTO"); return e ? std::atoi(e) : 0; }();
            static const bool autoPrint = autoMode != 0;
            if (autoPrint && isCallKindForPrint(kind))
            {
                static std::unordered_set<uint64_t> seen;
                const uint8_t *ram = m_memory.getRDRAM();
                for (int reg : {4, 5})
                {
                    const uint32_t a = getRegU32(ctx, reg);
                    if (autoMode >= 2 && targetPc >= 0x460000u && targetPc < 0x480000u ? (a < 0x100000u || a >= 0x2000000u) : (a < 0x49DC80u || a >= 0x518500u))
                        continue;
                    std::string str;
                    bool ok = true;
                    for (uint32_t i = 0; i < 160; ++i)
                    {
                        const char c = static_cast<char>(ram[(a + i) & 0x1FFFFFFu]);
                        if (!c) break;
                        if ((c < 32 || c > 126) && c != '\n' && c != '\t') { ok = false; break; }
                        str.push_back(c);
                    }
                    if (!ok || str.size() < 4)
                        continue;
                    const uint64_t key = (static_cast<uint64_t>(targetPc) << 32) ^ (autoMode >= 2 ? std::hash<std::string>{}(str) : a);
                    {
                        // Every 20 s: the strings passed most often in that window (per-frame state machines).
                        static std::unordered_map<std::string, uint64_t> window;
                        static auto windowStart = std::chrono::steady_clock::now();
                        char k[220];
                        std::snprintf(k, sizeof(k), "%x th%d %s", targetPc, m_eeScheduler ? m_eeScheduler->currentThreadId() : 0, str.substr(0, 120).c_str());
                        ++window[k];
                        if (std::chrono::steady_clock::now() - windowStart >= std::chrono::seconds(20))
                        {
                            std::vector<std::pair<uint64_t, std::string>> v;
                            for (auto &kv : window) v.push_back({kv.second, kv.first});
                            std::sort(v.rbegin(), v.rend());
                            std::fprintf(stderr, "[autoprint-top] ---- %zu distinct\n", v.size());
                            for (size_t i = 0; i < v.size() && i < (autoMode >= 2 ? 60u : 25u); ++i)
                                std::fprintf(stderr, "[autoprint-top] %6llu %s\n", (unsigned long long)v[i].first, v[i].second.c_str());
                            window.clear();
                            windowStart = std::chrono::steady_clock::now();
                        }
                    }
                    if (seen.insert(key).second && seen.size() < 50000)
                    {
                        while (!str.empty() && str.back() == '\n') str.pop_back();
                        std::fprintf(stderr, "[autoprint %x a%d th%d] %s\n", targetPc, reg - 4,
                                     m_eeScheduler ? m_eeScheduler->currentThreadId() : 0, str.c_str());
                    }
                }
            }
        }
        for (const auto &hook : printHooks)
        {
            if (hook.first != targetPc || !isCallKindForPrint(kind))
                continue;
            static uint32_t printed = 0;
            if (++printed > 20000u)
                break;
            const uint8_t *ram = m_memory.getRDRAM();
            auto readStr = [&](uint32_t addr, std::string &out) {
                out.clear();
                for (uint32_t i = 0; i < 256; ++i)
                {
                    const uint32_t a = (addr + i) & 0x1FFFFFFu;
                    const char c = static_cast<char>(ram[a]);
                    if (!c) break;
                    out.push_back(c);
                }
            };
            std::string fmt, out, tmp;
            readStr(getRegU32(ctx, hook.second), fmt);
            int intArg = hook.second + 1; // next GPR after the format
            int fltArg = 12;
            auto nextInt = [&]() -> uint32_t {
                static const int order[] = {4, 5, 6, 7, 8, 9, 10, 11};
                for (int r : order) if (r == intArg) { ++intArg; return getRegU32(ctx, r); }
                ++intArg;
                return 0u;
            };
            for (size_t i = 0; i < fmt.size(); ++i)
            {
                if (fmt[i] != '%') { out.push_back(fmt[i]); continue; }
                size_t j = i + 1;
                while (j < fmt.size() && std::strchr("-+ #0123456789.l", fmt[j])) ++j;
                if (j >= fmt.size()) break;
                const char conv = fmt[j];
                const std::string spec = fmt.substr(i, j - i + 1);
                char buf[300];
                if (conv == 's') { readStr(nextInt(), tmp); std::snprintf(buf, sizeof(buf), "%s", tmp.c_str()); }
                else if (conv == 'f' || conv == 'g' || conv == 'e') { const double d = (fltArg < 20) ? ctx->f[fltArg] : 0.0; fltArg += 2; std::snprintf(buf, sizeof(buf), spec.c_str(), d); }
                else if (conv == '%') { buf[0] = '%'; buf[1] = 0; }
                else if (conv == 'c') { buf[0] = static_cast<char>(nextInt()); buf[1] = 0; }
                else { std::string s2 = spec; s2.erase(std::remove(s2.begin(), s2.end(), 'l'), s2.end()); std::snprintf(buf, sizeof(buf), s2.c_str(), nextInt()); }
                out += buf;
                i = j;
            }
            while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
            std::fprintf(stderr, "[gameprint %x] %s\n", hook.first, out.c_str());
        }
    }
    {
        // PS2X_FUNC_PROF=<thread id>: count guest call/jump targets made by that EE thread and print
        // the top 20 every 5 s (a cheap "what is this thread doing" profile).
        static const int profThread = [] { const char *e = std::getenv("PS2X_FUNC_PROF"); return e ? std::atoi(e) : 0; }();
        if (profThread != 0 && m_eeScheduler && m_eeScheduler->currentThreadId() == profThread)
        {
            static std::unordered_map<uint32_t, uint64_t> hits;
            static auto last = std::chrono::steady_clock::now();
            ++hits[targetPc];
            {
                // PS2X_FUNC_EDGES=<file>: also write (return address -> target) call counts of this
                // thread once, 30 s after profiling started, for offline call-graph analysis.
                static const char *edgeFile = std::getenv("PS2X_FUNC_EDGES");
                static std::unordered_map<uint64_t, uint64_t> edges;
                static auto edgeStart = std::chrono::steady_clock::now();
                static bool edgeDone = false;
                if (edgeFile && !edgeDone && isCallKindForPrint(kind))
                {
                    const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - edgeStart).count();
                    const double from = std::getenv("PS2X_FUNC_EDGES_FROM") ? std::atof(std::getenv("PS2X_FUNC_EDGES_FROM")) : 0.0;
                    if (el >= from)
                        ++edges[(static_cast<uint64_t>(getRegU32(ctx, 31)) << 32) | targetPc];
                    if (el >= from + 20.0)
                    {
                        edgeDone = true;
                        if (std::FILE *f = std::fopen(edgeFile, "w"))
                        {
                            for (auto &kv : edges)
                                std::fprintf(f, "%x %x %llu\n", (uint32_t)(kv.first >> 32), (uint32_t)kv.first, (unsigned long long)kv.second);
                            std::fclose(f);
                        }
                    }
                }
            }
            const auto now = std::chrono::steady_clock::now();
            if (now - last >= std::chrono::seconds(5))
            {
                std::vector<std::pair<uint64_t, uint32_t>> v;
                for (auto &kv : hits) v.push_back({kv.second, kv.first});
                std::sort(v.rbegin(), v.rend());
                std::fprintf(stderr, "[funcprof] thread %d, %zu targets:", profThread, v.size());
                for (size_t i = 0; i < v.size() && i < 20; ++i)
                    std::fprintf(stderr, " %x=%llu", v[i].second, (unsigned long long)v[i].first);
                std::fprintf(stderr, "\n");
                if (const char *w = std::getenv("PS2X_FUNC_WATCH"))
                {
                    std::fprintf(stderr, "[funcwatch]");
                    for (const char *p = w; *p;)
                    {
                        char *end = nullptr;
                        const uint32_t a = static_cast<uint32_t>(std::strtoul(p, &end, 0));
                        if (end == p) break;
                        std::fprintf(stderr, " %x=%llu", a, (unsigned long long)hits[a]);
                        p = end; while (*p == ',') ++p;
                    }
                    std::fprintf(stderr, "\n");
                }
                hits.clear();
                last = now;
            }
        }
    }

}

static int g_dispatchHooksOn = -1; // -1 = not read yet (first dispatch, as the hooks' own statics did)

static int readDispatchHooksOn()
{
    for (const char *name : {"PS2X_CALL_TRACE", "PS2X_OBJDUMP", "PS2X_GAME_PRINT", "PS2X_GAME_PRINT_AUTO", "PS2X_FUNC_PROF"})
        if (std::getenv(name))
            return 1;
    return 0;
}

bool PS2Runtime::dispatchGuestBranch(uint8_t *rdram,
                                     R5900Context *ctx,
                                     uint32_t targetPc,
                                     uint32_t sourcePc,
                                     uint32_t fallthroughPc,
                                     GuestBranchKind kind,
                                     const char *debugName)
{
    if (g_dispatchHooksOn != 0) [[unlikely]]
    {
        if (g_dispatchHooksOn < 0)
            g_dispatchHooksOn = readDispatchHooksOn();
        if (g_dispatchHooksOn > 0)
            dispatchDebugHooks(rdram, ctx, targetPc, sourcePc, kind);
    }

    ctx->pc = targetPc;
    const bool isCall = (kind == GuestBranchKind::DirectCall || kind == GuestBranchKind::IndirectCall);

    // Every inter-function transfer is also a deterministic EE safe point.
    // Backward edges inside generated functions use eeCheckpointDue(), while
    // this charge bounds straight-line call chains that have no local loop.
    if (m_eeScheduler && m_eeScheduler->checkpointDue(EeScheduler::kGuestDispatchCycles))
    {
        ++t_dispatchContinuations;
        return false;
    }

    if (!isCall)
    {
        if (!hasFunction(targetPc))
        {
            reportMissingFunction(rdram, ctx, targetPc, sourcePc, kind, debugName);
        }

        ctx->pc = targetPc;
        ++t_dispatchContinuations;
        return false;
    }

    // Sims 2: New Game warns "Story mode and Freeplay mode share a single save file!". With save
    // slots that no longer holds, so instead of opening that Yes/No dialog (func_2A4498 called from
    // sub_002B3288) run its Yes handler, sub_002B3E58(this = $s1, button = 1).
    if (targetPc == 0x2A4498u && sourcePc == 0x2B35ACu) [[unlikely]]
    {
        SET_GPR_U32(ctx, 4, GPR_U32(ctx, 17));
        SET_GPR_U32(ctx, 5, 1u);
        targetPc = 0x2B3E58u;
        ctx->pc = targetPc;
    }

    uint32_t targetSlot = 0u;
    RecompiledFunction targetFn = generatedFunctionTableSlot(targetPc, targetSlot) ? g_ps2RecompiledFunctionTable[targetSlot] : nullptr;
    if (targetFn == nullptr)
    {
        reportMissingFunction(rdram, ctx, targetPc, sourcePc, kind, debugName);

        const MissingFunctionPolicy policy = missingFunctionPolicy();

        if (policy == MissingFunctionPolicy::SkipCallDebug && isCall)
        {
            ctx->pc = fallthroughPc;
            return true;
        }

        if (policy == MissingFunctionPolicy::ContinueToTarget)
        {
            ctx->pc = targetPc;
            // if you need the app to keep open to open debug pannel change this to false
            ++t_dispatchContinuations;
            return false;
        }

        ++t_dispatchContinuations;
        return false;
    }

    pushDispatchPc(targetPc); // as lookupFunction does
    const uint32_t entryPc = ctx->pc;
    const uint64_t continuationsBefore = t_dispatchContinuations;
    const bool ring = g_dispatchRingOn;
    if (ring)
        ps2xDispatchEvent('C', targetPc, sourcePc, ctx);
    targetFn(rdram, ctx, this);

    if (isStopRequested() || ctx->pc == 0u)
    {
        if (ring)
            ps2xDispatchEvent('R', targetPc, 0u, ctx);
        return false;
    }

    // A callee that never touched ctx->pc (HLE stubs in the function table) "returns" with
    // pc == entry. But pc == entry is also what a continuation request looks like when the
    // guest is about to (re)enter this same function: a recursive call or tail jump that the
    // dispatcher deferred for a scheduler checkpoint, or a loop at the entry that hit
    // eeCheckpointDue. Treating those as "returned" skipped the rest of the call and resumed
    // the caller on the callee's stack frame (Sims 2: intermittent crash to pc == ra == 0x10
    // in the scene-graph walk sub_00391160 <-> sub_00389348 entering Create-A-Sim).
    if (ctx->pc == entryPc && t_dispatchContinuations == continuationsBefore)
    {
        ctx->pc = fallthroughPc;
    }

    if (ring)
        ps2xDispatchEvent('R', targetPc, ctx->pc == fallthroughPc ? 1u : 0u, ctx);
    return ctx->pc == fallthroughPc;
}

void PS2Runtime::SignalException(R5900Context *ctx, PS2Exception exception)
{
    if (exception == EXCEPTION_INTEGER_OVERFLOW)
    {
        HandleIntegerOverflow(ctx);
        return;
    }

    raiseCop0Exception(ctx, static_cast<uint32_t>(exception),
                       exception == EXCEPTION_TLB_REFILL);
}

void PS2Runtime::executeVU0Microprogram(uint8_t *rdram, R5900Context *ctx, uint32_t address)
{
    (void)rdram;

    uint8_t *const vu0Code = m_memory.getVU0Code();
    uint8_t *const vu0Data = m_memory.getVU0Data();
    const uint32_t startPC = address & ~0x7u;

    if (!vu0Code || !vu0Data || startPC + 8u > PS2_VU0_CODE_SIZE)
    {
        seedVu0IdleSuccess(ctx);
        return;
    }

    m_vu0.reset();
    copyVu0ContextToState(ctx, m_vu0.state());
    m_vu0.execute(vu0Code, PS2_VU0_CODE_SIZE,
                  vu0Data, PS2_VU0_DATA_SIZE,
                  m_gs, &m_memory,
                  startPC, 0u, ctx->vu0_itop, 4096);
    copyVu0StateToContext(m_vu0.state(), ctx);
}

void PS2Runtime::vu0StartMicroProgram(uint8_t *rdram, R5900Context *ctx, uint32_t address)
{
    // VCALLMS and VCALLMSR both route here.
    executeVU0Microprogram(rdram, ctx, address);
}

void PS2Runtime::handleSyscall(uint8_t *rdram, R5900Context *ctx)
{
    handleSyscall(rdram, ctx, 0);
}

void PS2Runtime::handleSyscall(uint8_t *rdram, R5900Context *ctx, uint32_t encodedSyscallId)
{
    if (ctx->in_delay_slot)
    {
        throw std::runtime_error("Attempted to execute a syscall inside a branch delay slot! "
                                 "This breaks the atomic basic block model and is structurally unsupported by the emulator.");
    }

    const uint32_t syscallId = (encodedSyscallId != 0u)
                                   ? encodedSyscallId
                                   : getRegU32(ctx, 3); // $v1 / $3 is the EE kernel syscall number

    if (ps2_syscalls::dispatchNumericSyscall(syscallId, rdram, ctx, this))
    {
        return;
    }

    // God help you
    ps2_syscalls::TODO(rdram, ctx, this, encodedSyscallId);
}

void PS2Runtime::handleBreak(uint8_t *rdram, R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_BREAKPOINT);
}

void PS2Runtime::drainCompletedDmacHandlers(uint8_t *rdram)
{
    for (uint32_t cause : m_memory.consumeCompletedDmacCauses())
    {
        ps2_syscalls::dispatchDmacHandlersForCause(rdram, this, cause);
    }
}

void PS2Runtime::handleTrap(uint8_t *rdram, R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_TRAP);
}

void PS2Runtime::handleTLBR(uint8_t *rdram, R5900Context *ctx)
{
    uint32_t vpn = 0;
    uint32_t pfn = 0;
    uint32_t mask = 0;
    bool valid = false;

    const uint32_t index = ctx->cop0_index & 0x3Fu;
    if (!m_memory.tlbRead(index, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    // Preserve low ASID bits in EntryHi.
    ctx->cop0_entryhi = (ctx->cop0_entryhi & 0x00000FFFu) | (vpn & 0xFFFFF000u);
    ctx->cop0_entrylo0 = (ctx->cop0_entrylo0 & ~0x03FFFFC2u) |
                         ((pfn & 0x000FFFFFu) << 6) |
                         (valid ? 0x2u : 0u);
    ctx->cop0_pagemask = mask & 0x01FFE000u;
}

void PS2Runtime::handleTLBWI(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t index = ctx->cop0_index & 0x3Fu;
    const uint32_t vpn = ctx->cop0_entryhi & 0xFFFFF000u;
    const uint32_t pfn = (ctx->cop0_entrylo0 >> 6) & 0x000FFFFFu;
    const uint32_t mask = ctx->cop0_pagemask & 0x01FFE000u;
    const bool valid = (ctx->cop0_entrylo0 & 0x2u) != 0u;

    if (!m_memory.tlbWrite(index, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
    }
}

void PS2Runtime::handleTLBWR(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t entryCount = static_cast<uint32_t>(m_memory.tlbEntryCount());
    if (entryCount == 0)
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    const uint32_t wired = std::min(ctx->cop0_wired, entryCount - 1);
    uint32_t random = ctx->cop0_random % entryCount;
    if (random < wired)
    {
        random = wired;
    }

    const uint32_t vpn = ctx->cop0_entryhi & 0xFFFFF000u;
    const uint32_t pfn = (ctx->cop0_entrylo0 >> 6) & 0x000FFFFFu;
    const uint32_t mask = ctx->cop0_pagemask & 0x01FFE000u;
    const bool valid = (ctx->cop0_entrylo0 & 0x2u) != 0u;

    if (!m_memory.tlbWrite(random, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    // Keep COP0 bookkeeping in sync with the selected slot.
    ctx->cop0_index = (ctx->cop0_index & ~0x3Fu) | (random & 0x3Fu);
    ctx->cop0_random = (random <= wired) ? (entryCount - 1) : (random - 1);
}

void PS2Runtime::handleTLBP(uint8_t *rdram, R5900Context *ctx)
{
    const int32_t index = m_memory.tlbProbe(ctx->cop0_entryhi & 0xFFFFF000u);
    if (index >= 0)
    {
        ctx->cop0_index = (ctx->cop0_index & ~0x8000003Fu) |
                          (static_cast<uint32_t>(index) & 0x3Fu);
    }
    else
    {
        // MIPS sets probe failure bit (P) in Index[31].
        ctx->cop0_index |= 0x80000000u;
    }
}

void PS2Runtime::clearLLBit(R5900Context *ctx)
{
    // LL/SC reservation is tracked separately from COP0 Status.
    ctx->llbit = 0;
    ctx->lladdr = 0;
}

uint32_t PS2Runtime::alignGuestHeapValue(uint32_t value, uint32_t alignment)
{
    if (alignment == 0)
    {
        return value;
    }

    const uint32_t mask = alignment - 1u;
    if (value > (std::numeric_limits<uint32_t>::max() - mask))
    {
        return std::numeric_limits<uint32_t>::max();
    }
    return (value + mask) & ~mask;
}

bool PS2Runtime::isGuestHeapAlignmentValid(uint32_t alignment)
{
    return alignment != 0u && (alignment & (alignment - 1u)) == 0u;
}

uint32_t PS2Runtime::normalizeGuestHeapAlignment(uint32_t alignment)
{
    if (!isGuestHeapAlignmentValid(alignment))
    {
        return kGuestHeapDefaultAlignment;
    }
    return std::max(alignment, kGuestHeapDefaultAlignment);
}

uint32_t PS2Runtime::clampGuestHeapBase(uint32_t guestBase) const
{
    uint32_t normalized = guestBase;
    if (normalized >= PS2_RAM_SIZE)
    {
        normalized &= PS2_RAM_MASK;
    }
    const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    return std::min(normalized, hardLimit);
}

uint32_t PS2Runtime::clampGuestHeapLimit(uint32_t guestLimit) const
{
    const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    if (guestLimit == 0u || guestLimit > hardLimit)
    {
        return hardLimit;
    }
    return guestLimit;
}

void PS2Runtime::resetGuestHeapLocked(uint32_t guestBase, uint32_t guestLimit)
{
    uint32_t base = alignGuestHeapValue(clampGuestHeapBase(guestBase), kGuestHeapDefaultAlignment);
    uint32_t limit = clampGuestHeapLimit(guestLimit);
    if (base == 0u)
    {
        const uint32_t fallbackBase = (m_guestHeapSuggestedBase != 0u) ? m_guestHeapSuggestedBase : kGuestHeapDefaultBase;
        base = alignGuestHeapValue(clampGuestHeapBase(fallbackBase), kGuestHeapDefaultAlignment);
    }

    if (limit <= base)
    {
        base = alignGuestHeapValue(clampGuestHeapBase(m_guestHeapSuggestedBase), kGuestHeapDefaultAlignment);
        limit = clampGuestHeapLimit(0u);
    }

    if (limit <= base)
    {
        base = 0u;
        limit = 0u;
    }

    m_guestHeapBlocks.clear();
    if (limit > base)
    {
        m_guestHeapBlocks.push_back({base, limit - base, true});
    }

    m_guestHeapBase = base;
    m_guestHeapEnd = base;
    m_guestHeapLimit = limit;
    m_guestHeapConfigured = true;
}

void PS2Runtime::ensureGuestHeapInitializedLocked()
{
    if (m_guestHeapConfigured)
    {
        return;
    }

    const uint32_t suggested = (m_guestHeapSuggestedBase == 0u) ? kGuestHeapDefaultBase : m_guestHeapSuggestedBase;
    resetGuestHeapLocked(suggested, clampGuestHeapLimit(0u));
}

int32_t PS2Runtime::findGuestHeapBlockIndexLocked(uint32_t guestAddr) const
{
    const uint32_t normalizedAddr = guestAddr & PS2_RAM_MASK;
    for (size_t i = 0; i < m_guestHeapBlocks.size(); ++i)
    {
        const GuestHeapBlock &block = m_guestHeapBlocks[i];
        if (!block.free && block.addr == normalizedAddr)
        {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

uint32_t PS2Runtime::allocateGuestBlockLocked(uint32_t size, uint32_t alignment)
{
    if (size == 0u)
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    if (size > (std::numeric_limits<uint32_t>::max() - (kGuestHeapDefaultAlignment - 1u)))
    {
        return 0u;
    }

    const uint32_t allocSize = alignGuestHeapValue(size, kGuestHeapDefaultAlignment);
    if (allocSize == 0u)
    {
        return 0u;
    }

    for (size_t i = 0; i < m_guestHeapBlocks.size(); ++i)
    {
        const GuestHeapBlock block = m_guestHeapBlocks[i];
        if (!block.free)
        {
            continue;
        }

        const uint64_t blockStart = block.addr;
        const uint64_t blockEnd = blockStart + static_cast<uint64_t>(block.size);
        const uint32_t alignedAddr = alignGuestHeapValue(block.addr, normalizedAlignment);
        if (alignedAddr < block.addr)
        {
            continue;
        }

        const uint64_t alignedStart = alignedAddr;
        if (alignedStart > blockEnd)
        {
            continue;
        }

        const uint64_t allocEnd = alignedStart + static_cast<uint64_t>(allocSize);
        if (allocEnd > blockEnd)
        {
            continue;
        }

        const uint32_t prefixSize = static_cast<uint32_t>(alignedStart - blockStart);
        const uint32_t suffixSize = static_cast<uint32_t>(blockEnd - allocEnd);

        std::vector<GuestHeapBlock> replacement;
        replacement.reserve(3);
        if (prefixSize > 0u)
        {
            replacement.push_back({block.addr, prefixSize, true});
        }
        replacement.push_back({alignedAddr, allocSize, false});
        if (suffixSize > 0u)
        {
            replacement.push_back({static_cast<uint32_t>(allocEnd), suffixSize, true});
        }

        m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i));
        m_guestHeapBlocks.insert(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i),
                                 replacement.begin(),
                                 replacement.end());

        m_guestHeapEnd = std::max(m_guestHeapEnd, static_cast<uint32_t>(allocEnd));
        return alignedAddr;
    }

    return 0u;
}

void PS2Runtime::coalesceGuestHeapLocked()
{
    if (m_guestHeapBlocks.empty())
    {
        return;
    }

    size_t i = 1;
    while (i < m_guestHeapBlocks.size())
    {
        GuestHeapBlock &prev = m_guestHeapBlocks[i - 1];
        GuestHeapBlock &curr = m_guestHeapBlocks[i];
        const uint64_t prevEnd = static_cast<uint64_t>(prev.addr) + static_cast<uint64_t>(prev.size);
        if (prev.free && curr.free && prevEnd == curr.addr)
        {
            prev.size += curr.size;
            m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
    }
}

void PS2Runtime::freeGuestBlockLocked(uint32_t guestAddr)
{
    const int32_t index = findGuestHeapBlockIndexLocked(guestAddr);
    if (index < 0)
    {
        return;
    }

    m_guestHeapBlocks[static_cast<size_t>(index)].free = true;
    coalesceGuestHeapLocked();
}

void PS2Runtime::configureGuestHeap(uint32_t guestBase, uint32_t guestLimit)
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    uint32_t normalizedBase = alignGuestHeapValue(clampGuestHeapBase(guestBase), kGuestHeapDefaultAlignment);
    if (normalizedBase == 0u)
    {
        normalizedBase = (m_guestHeapSuggestedBase != 0u) ? m_guestHeapSuggestedBase : kGuestHeapDefaultBase;
    }
    m_guestHeapSuggestedBase = normalizedBase;
    resetGuestHeapLocked(normalizedBase, guestLimit);
}

uint32_t PS2Runtime::guestMalloc(uint32_t size, uint32_t alignment)
{
    {
        PrivateArena &arena = privateArena();
        std::lock_guard<std::mutex> arenaLock(arena.mutex);
        if (arena.heapActive)
        {
            if (const uint32_t address = arena.allocate(size, alignment))
                return address;
        }
    }
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();
    return allocateGuestBlockLocked(size, alignment);
}

uint32_t PS2Runtime::guestCalloc(uint32_t count, uint32_t size, uint32_t alignment)
{
    if (count == 0u || size == 0u)
    {
        return 0u;
    }
    if (count > (std::numeric_limits<uint32_t>::max() / size))
    {
        return 0u;
    }

    const uint32_t totalSize = count * size;
    const uint32_t guestAddr = guestMalloc(totalSize, alignment);
    if (guestAddr != 0u)
    {
        uint8_t *rdram = m_memory.getRDRAM();
        if (rdram)
        {
            uint32_t physAddr = guestAddr & PS2_RAM_MASK;
            if (physAddr + totalSize <= PS2_RAM_SIZE)
                std::memset(rdram + physAddr, 0, totalSize);
        }
    }

    return guestAddr;
}

uint32_t PS2Runtime::guestRealloc(uint32_t guestAddr, uint32_t newSize, uint32_t alignment)
{
    if (guestAddr == 0u)
    {
        return guestMalloc(newSize, alignment);
    }
    if (newSize == 0u)
    {
        guestFree(guestAddr);
        return 0u;
    }

    if (newSize > (std::numeric_limits<uint32_t>::max() - (kGuestHeapDefaultAlignment - 1u)))
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    const uint32_t requestedSize = alignGuestHeapValue(newSize, kGuestHeapDefaultAlignment);

    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();

    const int32_t index = findGuestHeapBlockIndexLocked(guestAddr);
    if (index < 0)
    {
        return 0u;
    }

    const size_t blockIndex = static_cast<size_t>(index);
    const uint32_t oldAddr = m_guestHeapBlocks[blockIndex].addr;
    const uint32_t oldSize = m_guestHeapBlocks[blockIndex].size;

    if (requestedSize <= oldSize)
    {
        if (requestedSize < oldSize)
        {
            const uint32_t tailAddr = oldAddr + requestedSize;
            const uint32_t tailSize = oldSize - requestedSize;
            m_guestHeapBlocks[blockIndex].size = requestedSize;
            m_guestHeapBlocks.insert(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(blockIndex + 1u),
                                     GuestHeapBlock{tailAddr, tailSize, true});
            coalesceGuestHeapLocked();
        }
        return oldAddr;
    }

    if (blockIndex + 1u < m_guestHeapBlocks.size())
    {
        GuestHeapBlock &next = m_guestHeapBlocks[blockIndex + 1u];
        const uint64_t blockEnd = static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].addr) +
                                  static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].size);
        if (next.free && blockEnd == next.addr)
        {
            const uint64_t combined = static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].size) +
                                      static_cast<uint64_t>(next.size);
            if (combined >= requestedSize)
            {
                const uint32_t extraNeeded = requestedSize - m_guestHeapBlocks[blockIndex].size;
                m_guestHeapBlocks[blockIndex].size = requestedSize;
                if (next.size == extraNeeded)
                {
                    m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(blockIndex + 1u));
                }
                else
                {
                    next.addr += extraNeeded;
                    next.size -= extraNeeded;
                }
                m_guestHeapEnd = std::max(m_guestHeapEnd, oldAddr + requestedSize);
                return oldAddr;
            }
        }
    }

    const uint32_t newAddr = allocateGuestBlockLocked(newSize, normalizedAlignment);
    if (newAddr == 0u)
    {
        return 0u;
    }

    uint8_t *rdram = m_memory.getRDRAM();
    if (rdram)
    {
        const uint32_t copyBytes = std::min(oldSize, newSize);
        uint32_t dstPhys = newAddr & PS2_RAM_MASK;
        uint32_t srcPhys = oldAddr & PS2_RAM_MASK;
        if (dstPhys + copyBytes <= PS2_RAM_SIZE && srcPhys + copyBytes <= PS2_RAM_SIZE)
            std::memmove(rdram + dstPhys, rdram + srcPhys, copyBytes);
    }

    freeGuestBlockLocked(oldAddr);
    return newAddr;
}

void PS2Runtime::guestFree(uint32_t guestAddr)
{
    if (guestAddr == 0u)
    {
        return;
    }
    {
        PrivateArena &arena = privateArena();
        std::lock_guard<std::mutex> arenaLock(arena.mutex);
        if (arena.owns(guestAddr))
        {
            arena.release(guestAddr);
            return;
        }
    }

    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();
    freeGuestBlockLocked(guestAddr);
}

uint32_t PS2Runtime::guestHeapBase() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapBase : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::guestHeapEnd() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapEnd : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::guestHeapLimit() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapLimit : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::reserveAsyncCallbackStack(uint32_t size, uint32_t alignment)
{
    if (size == 0u)
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    const uint32_t allocSize = alignGuestHeapValue(size, kGuestHeapDefaultAlignment);
    if (allocSize == 0u)
    {
        return 0u;
    }

    {
        // Prefer the private region so invocation stacks never overlap the game's
        // heap or a main-thread stack placed at the end of RAM.
        PrivateArena &arena = privateArena();
        std::lock_guard<std::mutex> arenaLock(arena.mutex);
        if (arena.regionUsable && arena.stackTop >= kPrivateStackFloor + allocSize)
        {
            const uint32_t base = (arena.stackTop - allocSize) & ~(normalizedAlignment - 1u);
            if (base >= kPrivateStackFloor)
            {
                const uint32_t top = arena.stackTop;
                arena.stackTop = base;
                return top - 0x10u;
            }
        }
    }

    std::lock_guard<std::mutex> lock(m_asyncCallbackStackMutex);
    uint32_t top = m_asyncCallbackStackTop;
    if (top > PS2_RAM_SIZE)
    {
        top = PS2_RAM_SIZE;
    }
    top &= ~(kGuestHeapDefaultAlignment - 1u);

    if (top <= allocSize)
    {
        return 0u;
    }

    uint32_t base = top - allocSize;
    base &= ~(normalizedAlignment - 1u);
    if (base < m_asyncCallbackStackFloor || base >= top)
    {
        return 0u;
    }

    m_asyncCallbackStackTop = base;
    return top - 0x10u;
}

uint8_t PS2Runtime::Load8(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read8(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint16_t PS2Runtime::Load16(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read16(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint32_t PS2Runtime::Load32(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read32(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint64_t PS2Runtime::Load64(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read64(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

__m128i PS2Runtime::Load128(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read128(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return _mm_setzero_si128();
    }
}

void PS2Runtime::Store8(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint8_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 1u, value, 0u, "WRITE8", ctx);
    try
    {
        m_memory.write8(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store16(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint16_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 2u, value, 0u, "WRITE16", ctx);
    try
    {
        m_memory.write16(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store32(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint32_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 4u, value, 0u, "WRITE32", ctx);
    try
    {
        m_memory.write32(vaddr, value);
        drainCompletedDmacHandlers(rdram);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store64(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint64_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 8u, value, 0u, "WRITE64", ctx);
    try
    {
        m_memory.write64(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store128(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, __m128i value)
{
    alignas(16) uint64_t _parts[2];
    _mm_storeu_si128(reinterpret_cast<__m128i *>(_parts), value);
    ps2TraceGuestWrite(rdram, vaddr, 16u, _parts[0], _parts[1], "WRITE128", ctx);
    try
    {
        m_memory.write128(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::kickGifDmaChainFromMMIO(uint8_t *rdram,
                                         R5900Context *ctx,
                                         uint32_t dPcrValue,
                                         uint32_t dStatValue,
                                         uint32_t tadr,
                                         uint32_t chcr)
{
    constexpr uint32_t D_PCR = 0x1000E020u;
    constexpr uint32_t D_STAT = 0x1000E010u;
    constexpr uint32_t GIF_TADR = 0x1000A030u;
    constexpr uint32_t GIF_CHCR = 0x1000A000u;

    ps2TraceGuestWrite(rdram, D_PCR, 4u, dPcrValue, 0u, "WRITE32", ctx);
    m_memory.writeIORegister(D_PCR, dPcrValue);
    ps2TraceGuestWrite(rdram, D_STAT, 4u, dStatValue, 0u, "WRITE32", ctx);
    m_memory.writeIORegister(D_STAT, dStatValue);
    ps2TraceGuestWrite(rdram, GIF_TADR, 4u, tadr, 0u, "WRITE32", ctx);
    m_memory.writeIORegister(GIF_TADR, tadr);
    ps2TraceGuestWrite(rdram, GIF_CHCR, 4u, chcr, 0u, "WRITE32", ctx);
    if (m_memory.tryProcessNativeGifImageUploadChain(m_gs, tadr, chcr))
    {
        drainCompletedDmacHandlers(rdram);
        return;
    }
    if (m_memory.tryProcessNativeGifPackedChain(m_gs, tadr, chcr))
    {
        drainCompletedDmacHandlers(rdram);
        return;
    }
    m_memory.writeIORegister(GIF_CHCR, chcr);
    m_memory.processPendingTransfers();
    drainCompletedDmacHandlers(rdram);
}

void PS2Runtime::requestStop()
{
    m_stopRequested.store(true, std::memory_order_relaxed);
    if (m_eeScheduler)
    {
        m_eeScheduler->requestStop();
    }
}

bool PS2Runtime::isStopRequested() const
{
    return m_stopRequested.load(std::memory_order_relaxed);
}

EeScheduler &PS2Runtime::eeScheduler()
{
    return *m_eeScheduler;
}

const EeScheduler &PS2Runtime::eeScheduler() const
{
    return *m_eeScheduler;
}

void PS2Runtime::postEeEvent(EeEvent event)
{
    m_eeScheduler->postEvent(event);
}

bool PS2Runtime::eeCheckpointDue(uint32_t cycles) noexcept
{
    // Generated code returns to the dispatcher with ctx->pc = the loop head when this is true:
    // a continuation request (see dispatchGuestBranch).
    const bool due = m_eeScheduler->checkpointDue(cycles);
    if (due)
        ++t_dispatchContinuations;
    return due;
}

[[noreturn]] void PS2Runtime::eeWaitVSyncTicks(uint32_t ticks, uint32_t resumePc)
{
    const uint64_t currentTick = m_eeScheduler->currentVSyncTick();
    const uint64_t waitTicks = std::max<uint64_t>(1u, ticks);
    m_eeScheduler->waitVSync(currentTick + waitTicks - 1u,
                             0,
                             [resumePc](R5900Context &context)
                             {
                                 context.pc = resumePc;
                             });
}

void PS2Runtime::addEeExitHandler(int threadId, uint32_t function, uint32_t argument)
{
    std::lock_guard lock(m_eeKernelStateMutex);
    m_eeExitHandlers[threadId].push_back({function, argument});
}

std::vector<PS2Runtime::EeExitHandlerRegistration> PS2Runtime::takeEeExitHandlers(int threadId)
{
    std::lock_guard lock(m_eeKernelStateMutex);
    auto it = m_eeExitHandlers.find(threadId);
    if (it == m_eeExitHandlers.end())
    {
        return {};
    }
    auto handlers = std::move(it->second);
    m_eeExitHandlers.erase(it);
    return handlers;
}

void PS2Runtime::removeEeExitHandlers(int threadId)
{
    std::lock_guard lock(m_eeKernelStateMutex);
    m_eeExitHandlers.erase(threadId);
}

bool PS2Runtime::findEeSyscallOverride(uint32_t syscallNumber, uint32_t &handler) const
{
    std::lock_guard lock(m_eeKernelStateMutex);
    const auto it = m_eeSyscallOverrides.find(syscallNumber);
    if (it == m_eeSyscallOverrides.end())
    {
        return false;
    }
    handler = it->second;
    return true;
}

void PS2Runtime::setEeSyscallOverride(uint8_t *rdram, uint32_t syscallNumber, uint32_t handler)
{
    constexpr uint32_t kTableBase = 0x80011F80u & 0x1FFFFFFFu;
    constexpr uint32_t kMirrorLimit = 0x00080000u;
    const int64_t offset = static_cast<int64_t>(static_cast<int32_t>(syscallNumber)) * 4;
    const int64_t address = static_cast<int64_t>(kTableBase) + offset;

    std::lock_guard lock(m_eeKernelStateMutex);
    if (handler == 0u)
    {
        m_eeSyscallOverrides.erase(syscallNumber);
    }
    else
    {
        m_eeSyscallOverrides[syscallNumber] = handler;
    }
    if (!rdram || address < 0 || address + 4 > kMirrorLimit)
    {
        return;
    }
    const uint32_t guestAddress = static_cast<uint32_t>(address);
    std::memcpy(rdram + guestAddress, &handler, sizeof(handler));
    if (handler == 0u)
    {
        m_eeSyscallMirrorAddresses.erase(guestAddress);
    }
    else
    {
        m_eeSyscallMirrorAddresses.insert(guestAddress);
    }
}

void PS2Runtime::initializeEeKernelState(uint8_t *rdram)
{
    if (!rdram)
    {
        return;
    }
    constexpr uint32_t kTableGuestBase = 0x80011F80u;
    constexpr uint32_t kTableBase = kTableGuestBase & 0x1FFFFFFFu;
    constexpr uint32_t kMirrorLimit = 0x00080000u;
    constexpr uint32_t kProbeBase = 0x000002F0u;

    std::lock_guard lock(m_eeKernelStateMutex);
    for (const uint32_t address : m_eeSyscallMirrorAddresses)
    {
        const uint32_t zero = 0u;
        std::memcpy(rdram + address, &zero, sizeof(zero));
    }
    m_eeSyscallMirrorAddresses.clear();
    const uint32_t high = kTableGuestBase >> 16;
    const uint32_t low = kTableGuestBase & 0xFFFFu;
    std::memcpy(rdram + kProbeBase, &high, sizeof(high));
    std::memcpy(rdram + kProbeBase + 8u, &low, sizeof(low));
    m_eeSyscallMirrorAddresses.insert(kProbeBase);
    m_eeSyscallMirrorAddresses.insert(kProbeBase + 8u);

    for (const auto &[syscallNumber, handler] : m_eeSyscallOverrides)
    {
        const int64_t offset = static_cast<int64_t>(static_cast<int32_t>(syscallNumber)) * 4;
        const int64_t address = static_cast<int64_t>(kTableBase) + offset;
        if (address < 0 || address + 4 > kMirrorLimit)
        {
            continue;
        }
        const uint32_t guestAddress = static_cast<uint32_t>(address);
        std::memcpy(rdram + guestAddress, &handler, sizeof(handler));
        m_eeSyscallMirrorAddresses.insert(guestAddress);
    }
}

void PS2Runtime::HandleIntegerOverflow(R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_INTEGER_OVERFLOW);
}

// ---------------------------------------------------------------------------
// Host presentation: how the 640x448 guest picture is scaled into the window.
// PS2X_ASPECT=4:3 (default, what a TV showed) | 16:9 (games with a widescreen option) |
//             stretch (fill the window) | native (square guest pixels, the old behaviour)
// PS2X_FILTER=sharp (default: integer nearest prescale + bilinear for the fraction, crisp without
//             uneven pixel sizes) | linear | nearest
// F11 toggles borderless fullscreen.
// ---------------------------------------------------------------------------
namespace
{
    enum class HostAspect { Ratio4x3, Ratio16x9, Stretch, Native };
    enum class HostFilter { Sharp, Linear, Nearest };

    struct HostPresenter
    {
        HostAspect aspect = HostAspect::Ratio4x3;
        HostFilter filter = HostFilter::Sharp;
        Shader sharpShader{};
        bool hasSharpShader = false;
        int texSizeLoc = -1;
        int prescaleLoc = -1;

        void init(Texture2D &tex)
        {
            if (const char *env = std::getenv("PS2X_ASPECT"))
            {
                const std::string v(env);
                if (v == "16:9") aspect = HostAspect::Ratio16x9;
                else if (v == "stretch") aspect = HostAspect::Stretch;
                else if (v == "native") aspect = HostAspect::Native;
            }
            if (const char *env = std::getenv("PS2X_FILTER"))
            {
                const std::string v(env);
                if (v == "linear") filter = HostFilter::Linear;
                else if (v == "nearest") filter = HostFilter::Nearest;
            }
#if !defined(PLATFORM_VITA) && !defined(PLATFORM_ANDROID)
            if (filter == HostFilter::Sharp)
            {
                static const char *kSharpBilinearFs = R"(#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform vec2 texSize;
uniform vec2 prescale;
out vec4 finalColor;
void main()
{
    vec2 texel = fragTexCoord * texSize;
    vec2 texelFloor = floor(texel);
    vec2 centerDist = texel - texelFloor - 0.5;
    vec2 regionRange = 0.5 - 0.5 / prescale;
    vec2 f = (centerDist - clamp(centerDist, -regionRange, regionRange)) * prescale + 0.5;
    finalColor = texture(texture0, (texelFloor + f) / texSize) * colDiffuse * fragColor;
}
)";
                sharpShader = LoadShaderFromMemory(nullptr, kSharpBilinearFs);
                hasSharpShader = IsShaderValid(sharpShader);
                if (hasSharpShader)
                {
                    texSizeLoc = GetShaderLocation(sharpShader, "texSize");
                    prescaleLoc = GetShaderLocation(sharpShader, "prescale");
                }
                else
                {
                    filter = HostFilter::Linear;
                }
            }
#else
            if (filter == HostFilter::Sharp)
                filter = HostFilter::Linear;
#endif
            SetTextureFilter(tex, filter == HostFilter::Nearest ? TEXTURE_FILTER_POINT : TEXTURE_FILTER_BILINEAR);
        }

        void shutdown()
        {
            if (hasSharpShader)
                UnloadShader(sharpShader);
            hasSharpShader = false;
        }

        // crop (optional): the part of the picture to show, as fractions x0 y0 x1 y1.
        void draw(const Texture2D &tex, float srcWidth, float srcHeight, const float *crop = nullptr)
        {
            float srcX = 0.0f, srcY = 0.0f;
            if (crop)
            {
                srcX = crop[0] * srcWidth;
                srcY = crop[1] * srcHeight;
                srcWidth *= crop[2] - crop[0];
                srcHeight *= crop[3] - crop[1];
            }
#if !defined(PLATFORM_VITA) && !defined(PLATFORM_ANDROID)
            if (IsKeyPressed(KEY_F11))
                toggleFullscreen();
#endif
            const float screenWidth = static_cast<float>(GetScreenWidth());
            const float screenHeight = static_cast<float>(GetScreenHeight());
            float dstWidth = screenWidth;
            float dstHeight = screenHeight;
            if (aspect != HostAspect::Stretch)
            {
                float ratio = aspect == HostAspect::Ratio4x3    ? 4.0f / 3.0f
                              : aspect == HostAspect::Ratio16x9 ? 16.0f / 9.0f
                                                                : srcWidth / srcHeight;
                // A cropped half keeps the shape it has inside the full picture.
                if (crop && aspect != HostAspect::Native)
                    ratio *= (crop[2] - crop[0]) / (crop[3] - crop[1]);
                dstHeight = std::min(screenHeight, screenWidth / ratio);
                dstWidth = dstHeight * ratio;
            }
            const Rectangle srcRect{srcX, srcY, srcWidth, srcHeight};
            const Rectangle dstRect{std::floor((screenWidth - dstWidth) * 0.5f),
                                    std::floor((screenHeight - dstHeight) * 0.5f),
                                    std::floor(dstWidth),
                                    std::floor(dstHeight)};
            const bool useShader = filter == HostFilter::Sharp && hasSharpShader;
            if (useShader)
            {
                const float texSize[2] = {static_cast<float>(tex.width), static_cast<float>(tex.height)};
                const float prescale[2] = {std::max(1.0f, std::floor(dstRect.width / srcWidth)),
                                           std::max(1.0f, std::floor(dstRect.height / srcHeight))};
                SetShaderValue(sharpShader, texSizeLoc, texSize, SHADER_UNIFORM_VEC2);
                SetShaderValue(sharpShader, prescaleLoc, prescale, SHADER_UNIFORM_VEC2);
                BeginShaderMode(sharpShader);
            }
            DrawTexturePro(tex, srcRect, dstRect, Vector2{0.0f, 0.0f}, 0.0f, WHITE);
            if (useShader)
                EndShaderMode();
        }
    };
}

void PS2Runtime::run()
{
    m_stopRequested.store(false, std::memory_order_relaxed);
    ps2_stubs::resetSifState();
    resetIop();
    ps2_stubs::resetAudioStubState();
    ps2_stubs::resetMpegStubState();
    initializeEeKernelState(m_memory.getRDRAM());
    m_cpuContext.r[4] = _mm_setzero_si128();
    m_cpuContext.r[5] = _mm_setzero_si128();
    m_cpuContext.r[29] = _mm_set_epi64x(0, static_cast<int64_t>(PS2_RAM_SIZE - 0x10u));
    m_debugPc.store(m_cpuContext.pc, std::memory_order_relaxed);
    m_debugRa.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[31], 0)), std::memory_order_relaxed);
    m_debugSp.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[29], 0)), std::memory_order_relaxed);
    m_debugGp.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[28], 0)), std::memory_order_relaxed);

    RUNTIME_LOG("Starting execution at address 0x" << std::hex << m_cpuContext.pc << std::dec);

    // A blank image to use as a framebuffer
    Image blank = GenImageColor(FB_WIDTH, FB_HEIGHT, BLANK);
    Texture2D frameTex = LoadTextureFromImage(blank);
    UnloadImage(blank);
    HostPresenter presenter;
    presenter.init(frameTex);
    if (ps2x_framegen::enabled())
        ps2x_framegen::init(FB_WIDTH, FB_HEIGHT,
                            presenter.filter == HostFilter::Nearest ? TEXTURE_FILTER_POINT : TEXTURE_FILTER_BILINEAR);

    // Code mods start before the game does (a save-slot mod picks the memory card folders first).
    ps2x::mods::startCodeMods(m_memory.getRDRAM());

    std::atomic<bool> gameThreadFinished{false};

    std::thread gameThread([&]()
                           {
        ThreadNaming::SetCurrentThreadName("GameThread");
        try
        {
            ps2NetplayShareSave(); // co-op: both games boot from the host's save (the window keeps drawing meanwhile)
            m_eeScheduler->reset(m_memory.getRDRAM(), m_cpuContext);
            m_eeScheduler->run();
            uint32_t pc = m_debugPc.load(std::memory_order_relaxed);
            RUNTIME_LOG("Game thread returned. PC=0x" << std::hex << pc
                      << " RA=0x" << static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[31], 0)) << std::dec << std::endl);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Error during program execution: " << e.what() << std::endl;
        }
        catch (...)
        {
            std::cerr << "Error during program execution: unknown exception" << std::endl;
        }
        gameThreadFinished.store(true, std::memory_order_release); });

    uint64_t tick = 0;
    while (!isStopRequested() && !gameThreadFinished.load(std::memory_order_acquire))
    {
        PS2_IF_AGRESSIVE_LOGS({
            tick++;
            if ((tick % 120) == 0)
            {
                uint64_t curDma = m_memory.dmaStartCount();
                uint64_t curGif = m_memory.gifCopyCount();
                uint64_t curGs = m_memory.gsWriteCount();
                uint64_t curVif = m_memory.vifWriteCount();
                const GSRegisters &gs = m_memory.gs();
                const uint32_t dbgPc = m_debugPc.load(std::memory_order_relaxed);
                const uint32_t dbgRa = m_debugRa.load(std::memory_order_relaxed);
                const uint32_t dbgSp = m_debugSp.load(std::memory_order_relaxed);
                const uint32_t dbgGp = m_debugGp.load(std::memory_order_relaxed);
                const auto eeSnapshot = m_eeScheduler->snapshot();

                RUNTIME_LOG("[run:tick] tick=" << tick
                                               << " pc=0x" << std::hex << dbgPc
                                               << " ra=0x" << dbgRa
                                               << " sp=0x" << dbgSp
                                               << " gp=0x" << dbgGp
                                               << " dispfb1=0x" << gs.dispfb1
                                               << " display1=0x" << gs.display1
                                               << std::dec
                                               << " activeThreads=" << eeSnapshot.threads.size()
                                               << " dma=" << curDma
                                               << " gif=" << curGif
                                               << " gsw=" << curGs
                                               << " vif=" << curVif
                                               << std::endl);

            }
        });
        uint32_t presentWidth = FB_WIDTH;
        uint32_t presentHeight = DEFAULT_DISPLAY_HEIGHT;
        UploadFrame(frameTex, this, presentWidth, presentHeight);
        const Texture2D *drawTex = &frameTex;
        Texture2D hiresTex{};
        bool hires = false;
        if (ps2x_framegen::enabled())
        {
            // The internal-resolution picture goes with the frame that arrived this present (PS2X_GS_SCALE).
            if (unsigned id = 0; ps2xGsGpuAcquireHiresFrame(id, hiresTex.width, hiresTex.height))
            {
                ps2x_framegen::submitHires(id, hiresTex.width, hiresTex.height);
                hires = true; // released after EndDrawing, like a picture drawn directly
            }
            // Display only: may show a generated in-between picture (renders to its own target before drawing).
            if (const Texture2D *generated = ps2x_framegen::present(presentWidth, presentHeight))
                drawTex = generated;
        }
        else if (unsigned id = 0; ps2xGsGpuAcquireHiresFrame(id, hiresTex.width, hiresTex.height))
        {
            // The GPU renderer's internal-resolution picture of the same display (PS2X_GS_SCALE).
            hiresTex.id = id;
            hiresTex.mipmaps = 1;
            hiresTex.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
            drawTex = &hiresTex;
            presentWidth = static_cast<uint32_t>(hiresTex.width);
            presentHeight = static_cast<uint32_t>(hiresTex.height);
            hires = true;
            static int s_loggedHeight = 0;
            if (hiresTex.height != s_loggedHeight)
            {
                s_loggedHeight = hiresTex.height;
                std::fprintf(stderr, "[present] internal-resolution picture %dx%d\n", hiresTex.width, hiresTex.height);
            }
        }

        BeginDrawing();
        ClearBackground(BLACK);
        if (s_coopFullscreen)
        {
            static const int s_forcedView = coopForcedView();
            m_gs.setCoopView(s_forcedView >= 0 ? s_forcedView : ps2NetplayLocalPlayer());
        }
        presenter.draw(*drawTex,
                       static_cast<float>(std::max<uint32_t>(1u, presentWidth)),
                       static_cast<float>(std::max<uint32_t>(1u, presentHeight)),
                       nullptr);
        if (m_debugUiInitialized && m_debugUiDrawCallback)
        {
            m_debugUiDrawCallback(*this, m_debugUiUserData);
        }
        if (const std::string msg = ps2NetplayOverlayText(); !msg.empty())
        {
            // Co-op: the game is held until the other player's input arrives; say so instead of a still/black picture.
            std::vector<std::string> lines;
            for (size_t a = 0, b = 0; a <= msg.size(); a = b + 1)
            {
                b = msg.find('\n', a);
                if (b == std::string::npos)
                    b = msg.size();
                lines.push_back(msg.substr(a, b - a));
            }
            const int size = std::max(20, GetScreenHeight() / 24);
            const int lineH = size + size / 3;
            const int count = static_cast<int>(lines.size());
            const int top = (GetScreenHeight() - lineH * count) / 2;
            DrawRectangle(0, top - size, GetScreenWidth(), lineH * count + size * 2, Color{0, 0, 0, 200});
            for (int i = 0; i < count; ++i)
            {
                const int lineSize = i == 0 ? size : size * 3 / 4;
                DrawText(lines[i].c_str(), (GetScreenWidth() - MeasureText(lines[i].c_str(), lineSize)) / 2,
                         top + lineH * i, lineSize, i == 0 ? RAYWHITE : LIGHTGRAY);
            }
        }
        EndDrawing();
        if (hires)
            ps2xGsGpuReleaseHiresFrame();
        if (s_presentVsync)
        {
            // Measure the monitor refresh from the VSync-blocked presents and publish it for the
            // VBlank phase lock (EeScheduler.cpp). Hitches (outside 0.6-1.6x) are left out; nothing
            // is published until 60 clean intervals, or when presents don't block (VSync forced off).
            extern std::atomic<int64_t> g_ps2xHostVsyncNs;
            extern std::atomic<int64_t> g_ps2xHostRefreshNs;
            static int64_t lastPresentNs = 0;
            static double refreshNs = 0.0;
            static int cleanIntervals = 0;
            const int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::steady_clock::now().time_since_epoch())
                                      .count();
            const double interval = static_cast<double>(nowNs - lastPresentNs);
            if (lastPresentNs != 0)
            {
                if (refreshNs == 0.0)
                {
                    const int hz = GetMonitorRefreshRate(GetCurrentMonitor());
                    refreshNs = hz > 0 ? 1.0e9 / hz : interval;
                }
                else if (interval > refreshNs * 0.6 && interval < refreshNs * 1.6)
                {
                    refreshNs += (interval - refreshNs) / 32.0;
                    ++cleanIntervals;
                }
            }
            lastPresentNs = nowNs;
            if (cleanIntervals >= 60 && refreshNs > 3.0e6)
            {
                g_ps2xHostRefreshNs.store(static_cast<int64_t>(refreshNs), std::memory_order_relaxed);
                g_ps2xHostVsyncNs.store(nowNs, std::memory_order_relaxed);
            }
            else if (cleanIntervals >= 60)
            {
                s_presentVsync = false; // presents don't wait for VSync: back to the 60 FPS timer
                g_ps2xHostRefreshNs.store(0, std::memory_order_relaxed);
                SetTargetFPS(60);
            }
        }
        {
            // PS2X_FRAME_LOG=1: host present times and the game frame shown (pairs with [flip] lines).
            static const bool logFrames = [] { const char *e = std::getenv("PS2X_FRAME_LOG"); return e && *e && *e != '0'; }();
            if (logFrames)
            {
                extern std::atomic<uint64_t> g_ps2xGameFrameCount;
                static const auto start = std::chrono::steady_clock::now();
                std::fprintf(stderr, "[present] %.3f %llu\n",
                             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(),
                             static_cast<unsigned long long>(g_ps2xGameFrameCount.load(std::memory_order_relaxed)));
            }
        }
        // Latch buttons pressed this host frame so the game sees taps shorter than a guest frame.
        ps2PadPollHost();
        ps2x::mods::frameCodeMods();

        if (WindowShouldClose())
        {
            RUNTIME_LOG("[run] window close requested, breaking out of loop");
            requestStop();
            break;
        }
    }

    requestStop();
    ps2NetplayShutdown(); // wakes a game thread waiting on the other player
    if (gameThread.joinable())
    {
        gameThread.join();
    }
    ps2x::mods::shutdownCodeMods();

    if (m_debugUiInitialized && m_debugUiShutdownCallback)
    {
        m_debugUiShutdownCallback(*this, m_debugUiUserData);
        m_debugUiInitialized = false;
    }
    ps2x_framegen::shutdown();
    presenter.shutdown();
    UnloadTexture(frameTex);
    ps2xGsGpuHostShutdown();
    CloseWindow();

    RUNTIME_LOG("[run] exiting loop");
}
