// GPU renderer for the emulated GS (the game's default; PS2X_GS_GPU=0 = CPU). See gs_gpu_backend.h for the overview.
//
// Memory model. GS local memory (4 MB) exists in three places, tracked per 8 KB page (512 pages):
//   * the CPU copy (the runtime's VRAM buffer): transfers, CLUT loads, readbacks, presentation and
//     the CPU fallback rasterizer work on it (through an embedded GSCpuBackend);
//   * "vtex", a 2048x512 R32UI texture: page p is row p. Textures and CLUT-free reads of any
//     format are decoded from it in shaders (swizzle tables generated from GSMem's own readers);
//   * render targets: one GL texture per (base page, width, layout) the game draws into, RGBA8 for
//     colour (PSMCT32/24/16/16S) and DEPTH32F for Z. A target holds a page set it is valid for and
//     a page set it is the only up-to-date copy of (dirty).
// Pages move between them lazily: a texture read writes the dirty pages of a target back into vtex
// (GPU pass), a CPU read downloads vtex rows, a CPU write marks the page CPU-only (uploaded and
// reloaded into targets when the GPU needs it again).
//
// Threading. The GS frontend thread only queues commands. A worker thread owns a GL 3.3 context
// (a hidden GLFW window sharing objects with the presenter's window, created on the main thread by
// ps2xGsGpuHostInit) and executes them in order. Until that context exists (and if GL setup fails)
// everything runs on the CPU rasterizer, so nothing is ever lost.
#include "runtime/gs/gs_gpu_backend.h"
#include "runtime/gs/gs_texture_hd.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/ps2_gs_common.h"
#include "runtime/gs/ps2_gs_memory.h"

#include <cstdlib>
#include <cstring>

#if !defined(PS2X_GS_GPU_AVAILABLE)

bool ps2xGsGpuRequested() { return false; }
bool ps2xGsGpuAvailable() { return false; }
std::unique_ptr<GSRasterBackend> ps2xMakeGsGpuBackend() { return std::make_unique<GSCpuBackend>(); }
void ps2xGsGpuHostInit(bool) {}
void ps2xGsGpuHostShutdown() {}
bool ps2xGsGpuAcquireHiresFrame(unsigned &, int &, int &) { return false; }
void ps2xGsGpuReleaseHiresFrame() {}
bool ps2xGsGpuWaitReady(int, std::string *reason)
{
    if (reason)
        *reason = "built without GL support";
    return false;
}

#else

#define GLFW_INCLUDE_NONE
#include "external/glad.h"
#include <GLFW/glfw3.h>

#include "ThreadNaming.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <string>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

class GSGpuBackend;

extern std::atomic<uint64_t> g_ps2xGameFrameCount; // EeScheduler.cpp: frames the game has shown

bool ps2xGsGpuRequested()
{
    static const bool on = [] { const char *e = std::getenv("PS2X_GS_GPU"); return e && *e && *e != '0'; }();
    return on;
}

bool ps2xGsGpuAvailable() { return true; }

namespace
{
    // ---------------------------------------------------------------- GL entry points
#define PS2X_GL_FUNCS(X)                                                        \
    X(PFNGLGETSTRINGPROC, GetString, "glGetString")                             \
    X(PFNGLGETINTEGERVPROC, GetIntegerv, "glGetIntegerv")                       \
    X(PFNGLGETERRORPROC, GetError, "glGetError")                                \
    X(PFNGLENABLEPROC, Enable, "glEnable")                                      \
    X(PFNGLDISABLEPROC, Disable, "glDisable")                                   \
    X(PFNGLVIEWPORTPROC, Viewport, "glViewport")                                \
    X(PFNGLSCISSORPROC, Scissor, "glScissor")                                   \
    X(PFNGLCLEARPROC, Clear, "glClear")                                         \
    X(PFNGLCLEARCOLORPROC, ClearColor, "glClearColor")                          \
    X(PFNGLCLEARDEPTHPROC, ClearDepth, "glClearDepth")                          \
    X(PFNGLLOGICOPPROC, LogicOp, "glLogicOp")                                   \
    X(PFNGLUNIFORM4FPROC, Uniform4f, "glUniform4f")                             \
    X(PFNGLCOLORMASKPROC, ColorMask, "glColorMask")                             \
    X(PFNGLDEPTHMASKPROC, DepthMask, "glDepthMask")                             \
    X(PFNGLDEPTHFUNCPROC, DepthFunc, "glDepthFunc")                             \
    X(PFNGLBLENDFUNCSEPARATEPROC, BlendFuncSeparate, "glBlendFuncSeparate")     \
    X(PFNGLBLENDEQUATIONSEPARATEPROC, BlendEquationSeparate, "glBlendEquationSeparate") \
    X(PFNGLPIXELSTOREIPROC, PixelStorei, "glPixelStorei")                       \
    X(PFNGLREADPIXELSPROC, ReadPixels, "glReadPixels")                          \
    X(PFNGLFLUSHPROC, Flush, "glFlush")                                         \
    X(PFNGLFINISHPROC, Finish, "glFinish")                                      \
    X(PFNGLDRAWARRAYSPROC, DrawArrays, "glDrawArrays")                          \
    X(PFNGLGENTEXTURESPROC, GenTextures, "glGenTextures")                       \
    X(PFNGLDELETETEXTURESPROC, DeleteTextures, "glDeleteTextures")              \
    X(PFNGLBINDTEXTUREPROC, BindTexture, "glBindTexture")                       \
    X(PFNGLACTIVETEXTUREPROC, ActiveTexture, "glActiveTexture")                 \
    X(PFNGLTEXIMAGE2DPROC, TexImage2D, "glTexImage2D")                          \
    X(PFNGLTEXSUBIMAGE2DPROC, TexSubImage2D, "glTexSubImage2D")                 \
    X(PFNGLTEXPARAMETERIPROC, TexParameteri, "glTexParameteri")                 \
    X(PFNGLGENERATEMIPMAPPROC, GenerateMipmap, "glGenerateMipmap")              \
    X(PFNGLTEXBUFFERPROC, TexBuffer, "glTexBuffer")                             \
    X(PFNGLGENFRAMEBUFFERSPROC, GenFramebuffers, "glGenFramebuffers")           \
    X(PFNGLDELETEFRAMEBUFFERSPROC, DeleteFramebuffers, "glDeleteFramebuffers")  \
    X(PFNGLBINDFRAMEBUFFERPROC, BindFramebuffer, "glBindFramebuffer")           \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, FramebufferTexture2D, "glFramebufferTexture2D") \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, CheckFramebufferStatus, "glCheckFramebufferStatus") \
    X(PFNGLDRAWBUFFERPROC, DrawBuffer, "glDrawBuffer")                          \
    X(PFNGLREADBUFFERPROC, ReadBuffer, "glReadBuffer")                          \
    X(PFNGLBLITFRAMEBUFFERPROC, BlitFramebuffer, "glBlitFramebuffer")           \
    X(PFNGLGENBUFFERSPROC, GenBuffers, "glGenBuffers")                          \
    X(PFNGLDELETEBUFFERSPROC, DeleteBuffers, "glDeleteBuffers")                 \
    X(PFNGLBINDBUFFERPROC, BindBuffer, "glBindBuffer")                          \
    X(PFNGLBUFFERDATAPROC, BufferData, "glBufferData")                          \
    X(PFNGLBUFFERSUBDATAPROC, BufferSubData, "glBufferSubData")                 \
    X(PFNGLGENVERTEXARRAYSPROC, GenVertexArrays, "glGenVertexArrays")           \
    X(PFNGLDELETEVERTEXARRAYSPROC, DeleteVertexArrays, "glDeleteVertexArrays")  \
    X(PFNGLBINDVERTEXARRAYPROC, BindVertexArray, "glBindVertexArray")           \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, EnableVertexAttribArray, "glEnableVertexAttribArray") \
    X(PFNGLVERTEXATTRIBPOINTERPROC, VertexAttribPointer, "glVertexAttribPointer") \
    X(PFNGLVERTEXATTRIBIPOINTERPROC, VertexAttribIPointer, "glVertexAttribIPointer") \
    X(PFNGLCREATESHADERPROC, CreateShader, "glCreateShader")                    \
    X(PFNGLDELETESHADERPROC, DeleteShader, "glDeleteShader")                    \
    X(PFNGLSHADERSOURCEPROC, ShaderSource, "glShaderSource")                    \
    X(PFNGLCOMPILESHADERPROC, CompileShader, "glCompileShader")                 \
    X(PFNGLGETSHADERIVPROC, GetShaderiv, "glGetShaderiv")                       \
    X(PFNGLGETSHADERINFOLOGPROC, GetShaderInfoLog, "glGetShaderInfoLog")        \
    X(PFNGLCREATEPROGRAMPROC, CreateProgram, "glCreateProgram")                 \
    X(PFNGLDELETEPROGRAMPROC, DeleteProgram, "glDeleteProgram")                 \
    X(PFNGLATTACHSHADERPROC, AttachShader, "glAttachShader")                    \
    X(PFNGLLINKPROGRAMPROC, LinkProgram, "glLinkProgram")                       \
    X(PFNGLGETPROGRAMIVPROC, GetProgramiv, "glGetProgramiv")                    \
    X(PFNGLGETPROGRAMINFOLOGPROC, GetProgramInfoLog, "glGetProgramInfoLog")     \
    X(PFNGLUSEPROGRAMPROC, UseProgram, "glUseProgram")                          \
    X(PFNGLGETUNIFORMLOCATIONPROC, GetUniformLocation, "glGetUniformLocation")  \
    X(PFNGLUNIFORM1IPROC, Uniform1i, "glUniform1i")                             \
    X(PFNGLUNIFORM1UIPROC, Uniform1ui, "glUniform1ui")                          \
    X(PFNGLUNIFORM4UIPROC, Uniform4ui, "glUniform4ui")                          \
    X(PFNGLUNIFORM1FPROC, Uniform1f, "glUniform1f")                             \
    X(PFNGLUNIFORM2FPROC, Uniform2f, "glUniform2f")                             \
    X(PFNGLUNIFORM2IPROC, Uniform2i, "glUniform2i")                             \
    X(PFNGLUNIFORM3FPROC, Uniform3f, "glUniform3f")                             \
    X(PFNGLBINDATTRIBLOCATIONPROC, BindAttribLocation, "glBindAttribLocation")  \
    X(PFNGLBINDFRAGDATALOCATIONINDEXEDPROC, BindFragDataLocationIndexed, "glBindFragDataLocationIndexed") \
    X(PFNGLGETBUFFERSUBDATAPROC, GetBufferSubData, "glGetBufferSubData")        \
    X(PFNGLFENCESYNCPROC, FenceSync, "glFenceSync")                             \
    X(PFNGLCLIENTWAITSYNCPROC, ClientWaitSync, "glClientWaitSync")              \
    X(PFNGLDELETESYNCPROC, DeleteSync, "glDeleteSync")                              X(PFNGLWAITSYNCPROC, WaitSync, "glWaitSync") \
    X(PFNGLGENQUERIESPROC, GenQueries, "glGenQueries")                          \
    X(PFNGLBEGINQUERYPROC, BeginQuery, "glBeginQuery")                          \
    X(PFNGLENDQUERYPROC, EndQuery, "glEndQuery")                                \
    X(PFNGLGETQUERYOBJECTIVPROC, GetQueryObjectiv, "glGetQueryObjectiv")        \
    X(PFNGLGETQUERYOBJECTUI64VPROC, GetQueryObjectui64v, "glGetQueryObjectui64v")

    struct GLApi
    {
#define PS2X_GL_DECL(type, name, sym) type name = nullptr;
        PS2X_GL_FUNCS(PS2X_GL_DECL)
#undef PS2X_GL_DECL
        bool load()
        {
            bool ok = true;
#define PS2X_GL_LOAD(type, name, sym)                                     \
    name = reinterpret_cast<type>(glfwGetProcAddress(sym));                \
    if (!name)                                                            \
    {                                                                     \
        std::fprintf(stderr, "[gsgpu] missing GL function %s\n", sym);    \
        ok = false;                                                       \
    }
            PS2X_GL_FUNCS(PS2X_GL_LOAD)
#undef PS2X_GL_LOAD
            return ok;
        }
    };
    GLApi gl; // worker thread only (one GS GPU context per process)
    constexpr GLenum kGlTimeoutExpired = 0x911B; // glClientWaitSync: GL_TIMEOUT_EXPIRED

    // glTextureBarrier (GL 4.5, ARB/NV_texture_barrier): optional, null when the driver has none.
    using TextureBarrierFn = void(GLAD_API_PTR *)(void);
    TextureBarrierFn glTextureBarrierFn = nullptr;

    // PS2X_GS_GPU_BARRIER (default on, =0 copies the frame instead): a draw that reads the frame (blending in the shader, DATE, alpha keep) samples
    // its own target behind a texture barrier instead of a copy, and a primitive over pixels an earlier one
    // in the batch wrote starts a new barrier-separated range of the same draw instead of a flush and copy.
    bool barrierRequested()
    {
        static const bool on = [] { const char *e = std::getenv("PS2X_GS_GPU_BARRIER"); return !e || !*e || *e != '0'; }();
        return on;
    }

    // ---------------------------------------------------------------- host context
    struct HostContext
    {
        std::mutex mutex;
        GLFWwindow *window = nullptr;
        bool ready = false;  // context created (or creation failed: window null)
        bool failed = false;
    };
    HostContext g_host;

    // Outcome of the renderer worker's GL setup (ps2xGsGpuWaitReady): -1 not yet, 0 failed, 1 ready.
    struct GlReady
    {
        std::mutex mutex;
        std::condition_variable cv;
        int state = -1;
        std::string reason;
    };
    GlReady g_glReady;

    void publishGlReady(bool ok, const char *reason)
    {
        std::lock_guard<std::mutex> lock(g_glReady.mutex);
        g_glReady.state = ok ? 1 : 0;
        g_glReady.reason = reason ? reason : "";
        g_glReady.cv.notify_all();
    }

    constexpr int kPages = 512;
    constexpr uint32_t kPageBytes = 8192;
    using PageMask = std::bitset<kPages>;

    // Swizzle tables (R16UI, 128 wide). Forward: offset within a page (pixel units) of (x, y) for
    // block 0, per layout class. Inverse: (x | y << 8) of each offset, for the target classes.
    // Classes: 0 CT32 (also CT24, T8H, T4HL, T4HH) 1 CT16 2 CT16S 3 T8 4 T4 5 Z32 (Z24) 6 Z16 7 Z16S.
    constexpr int kPageW[8] = {64, 64, 64, 128, 128, 64, 64, 64};
    constexpr int kPageH[8] = {32, 64, 64, 64, 128, 32, 64, 64};
    constexpr int kLutRow[8] = {0, 32, 96, 160, 224, 352, 384, 448};
    constexpr int kInvRow[8] = {512, 544, 576, 0, 0, 528, 608, 640};
    constexpr int kLutRows = 672;

    int psmClass(uint32_t psm)
    {
        switch (psm)
        {
        case GS_PSM_CT16: return 1;
        case GS_PSM_CT16S: return 2;
        case GS_PSM_T8: return 3;
        case GS_PSM_T4: return 4;
        case GS_PSM_Z32: case GS_PSM_Z24: return 5;
        case GS_PSM_Z16: return 6;
        case GS_PSM_Z16S: return 7;
        default: return 0;
        }
    }
    bool isDepthClass(int cls) { return cls >= 5; }
    bool isColorTargetPsm(uint32_t psm)
    {
        return psm == GS_PSM_CT32 || psm == GS_PSM_CT24 || psm == GS_PSM_CT16 || psm == GS_PSM_CT16S;
    }
    bool isDepthTargetPsm(uint32_t psm)
    {
        return psm == GS_PSM_Z32 || psm == GS_PSM_Z24 || psm == GS_PSM_Z16 || psm == GS_PSM_Z16S;
    }
    double depthScale(int cls)
    {
        return cls == 5 ? 4294967296.0 : 65536.0;
    }

    std::vector<uint16_t> buildSwizzleTables()
    {
        using namespace GSMem;
        std::vector<uint8_t> mem(MEMORY_SIZE, 0);
        std::vector<uint16_t> lut(static_cast<size_t>(128) * kLutRows, 0);
        auto fwd = [&](int cls, int bw, auto &&read)
        {
            for (int y = 0; y < kPageH[cls]; ++y)
                for (int x = 0; x < kPageW[cls]; ++x)
                    lut[static_cast<size_t>(kLutRow[cls] + y) * 128 + x] = static_cast<uint16_t>(read(bw, x, y));
        };
        uint8_t *m = mem.data();
        // 32-bit layouts: word i holds i.
        for (uint32_t i = 0; i < 2048; ++i)
            std::memcpy(m + i * 4, &i, 4);
        fwd(0, 1, [&](int bw, int x, int y) { return ReadCT32(m, 0, bw, x, y); });
        fwd(5, 1, [&](int bw, int x, int y) { return ReadZ32(m, 0, bw, x, y); });
        // 16-bit layouts: halfword i holds i.
        std::memset(m, 0, kPageBytes);
        for (uint32_t i = 0; i < 4096; ++i)
        {
            const uint16_t v = static_cast<uint16_t>(i);
            std::memcpy(m + i * 2, &v, 2);
        }
        fwd(1, 1, [&](int bw, int x, int y) { return ReadCT16(m, 0, bw, x, y); });
        fwd(2, 1, [&](int bw, int x, int y) { return ReadCT16S(m, 0, bw, x, y); });
        fwd(6, 1, [&](int bw, int x, int y) { return ReadZ16(m, 0, bw, x, y); });
        fwd(7, 1, [&](int bw, int x, int y) { return ReadZ16S(m, 0, bw, x, y); });
        // 8-bit: byte i holds i, low then high byte.
        std::vector<uint16_t> t8(128 * 64);
        for (int pass = 0; pass < 2; ++pass)
        {
            for (uint32_t i = 0; i < 8192; ++i)
                m[i] = static_cast<uint8_t>(pass == 0 ? i : i >> 8);
            for (int y = 0; y < 64; ++y)
                for (int x = 0; x < 128; ++x)
                    t8[y * 128 + x] |= static_cast<uint16_t>(ReadP8(m, 0, 2, x, y) << (pass * 8));
        }
        fwd(3, 2, [&](int, int x, int y) { return t8[y * 128 + x]; });
        // 4-bit: nibble j holds digit d of j, four passes.
        std::vector<uint16_t> t4(128 * 128);
        for (int pass = 0; pass < 4; ++pass)
        {
            for (uint32_t j = 0; j < 16384; j += 2)
                m[j / 2] = static_cast<uint8_t>(((j >> (pass * 4)) & 15u) | ((((j + 1) >> (pass * 4)) & 15u) << 4));
            for (int y = 0; y < 128; ++y)
                for (int x = 0; x < 128; ++x)
                    t4[y * 128 + x] |= static_cast<uint16_t>(ReadP4(m, 0, 2, x, y) << (pass * 4));
        }
        fwd(4, 2, [&](int, int x, int y) { return t4[y * 128 + x]; });
        for (int cls : {0, 1, 2, 5, 6, 7})
            for (int y = 0; y < kPageH[cls]; ++y)
                for (int x = 0; x < kPageW[cls]; ++x)
                {
                    const uint32_t o = lut[static_cast<size_t>(kLutRow[cls] + y) * 128 + x];
                    lut[static_cast<size_t>(kInvRow[cls]) * 128 + o] = static_cast<uint16_t>(x | (y << 8));
                }
        return lut;
    }

    // Pages of rect [x0,x1]x[y0,y1] (inclusive) of a buffer, by the CPU's page arithmetic.
    // Block number within a page of each block cell (row-major, kBlockCols per row), per layout class.
    constexpr int kBlockCols[8] = {8, 4, 4, 8, 4, 8, 4, 4};
    const std::array<std::array<uint8_t, 32>, 8> &blockTables()
    {
        static const auto t = []
        {
            const std::vector<uint16_t> lut = buildSwizzleTables();
            std::array<std::array<uint8_t, 32>, 8> r{};
            for (int cls = 0; cls < 8; ++cls)
            {
                const int cols = kBlockCols[cls], rows = 32 / cols;
                const int cw = kPageW[cls] / cols, ch = kPageH[cls] / rows;
                const int perBlock = kPageW[cls] * kPageH[cls] / 32;
                for (int i = 0; i < 32; ++i)
                    r[cls][i] = static_cast<uint8_t>(lut[static_cast<size_t>(kLutRow[cls] + (i / cols) * ch) * 128 + (i % cols) * cw] / perBlock);
            }
            return r;
        }();
        return t;
    }

    PageMask rectPages(uint32_t psm, uint32_t baseBlock, uint32_t bw, int x0, int y0, int x1, int y1)
    {
        PageMask m;
        if (x1 < x0 || y1 < y0)
            return m;
        const int cls = psmClass(psm);
        const int pw = kPageW[cls], ph = kPageH[cls];
        const uint32_t ppr = (bw * 64u) / static_cast<uint32_t>(pw);
        x0 = std::max(x0, 0);
        y0 = std::max(y0, 0);
        if ((baseBlock & 31u) == 0u)
        {
            for (int py = y0 / ph; py <= y1 / ph; ++py)
                for (int px = x0 / pw; px <= x1 / pw; ++px)
                    m.set((baseBlock / 32u + static_cast<uint32_t>(py) * ppr + static_cast<uint32_t>(px)) & 511u);
            return m;
        }
        // A base inside a page: each block lands in that page or the next, by its number within the page.
        const auto &blocks = blockTables()[cls];
        const int cols = kBlockCols[cls];
        const int cw = pw / cols, ch = ph / (32 / cols);
        for (int by = y0 / ch; by <= y1 / ch; ++by)
            for (int bx = x0 / cw; bx <= x1 / cw; ++bx)
            {
                const uint32_t page = static_cast<uint32_t>(by / (32 / cols)) * ppr + static_cast<uint32_t>(bx / cols);
                const uint32_t block = blocks[(by % (32 / cols)) * cols + bx % cols];
                m.set(((baseBlock + page * 32u + block) / 32u) & 511u);
            }
        return m;
    }

    // ---------------------------------------------------------------- shaders
    const char *kCommonGlsl = R"(#version 330 core
uniform usampler2D uVram;
uniform usampler2D uLut;
const int kPageW[8] = int[8](64, 64, 64, 128, 128, 64, 64, 64);
const int kPageH[8] = int[8](32, 64, 64, 64, 128, 32, 64, 64);
const int kLutRow[8] = int[8](0, 32, 96, 160, 224, 352, 384, 448);
const int kInvRow[8] = int[8](512, 544, 576, 0, 0, 528, 608, 640);
int psmClass(uint psm)
{
    if (psm == 2u) return 1;
    if (psm == 10u) return 2;
    if (psm == 19u) return 3;
    if (psm == 20u) return 4;
    if (psm == 48u || psm == 49u) return 5;
    if (psm == 50u) return 6;
    if (psm == 58u) return 7;
    return 0;
}
uint vramWord(uint w)
{
    w &= 0xFFFFFu;
    return texelFetch(uVram, ivec2(int(w & 2047u), int(w >> 11u)), 0).r;
}
// Page sizes are powers of two: shifts and masks instead of integer divides (x, y are never negative).
const int kPageWLog[8] = int[8](6, 6, 6, 7, 7, 6, 6, 6);
const int kPageHLog[8] = int[8](5, 6, 6, 6, 7, 5, 6, 6);
uint readRaw(uint psm, uint base, uint bw, int x, int y)
{
    int cls = psmClass(psm);
    int wl = kPageWLog[cls], hl = kPageHLog[cls];
    uint pppLog = uint(wl + hl);
    uint page = (base >> 5u) + uint(y >> hl) * ((bw << 6u) >> uint(wl)) + uint(x >> wl);
    uint a = (page << pppLog) + texelFetch(uLut, ivec2(x & ((1 << wl) - 1), kLutRow[cls] + (y & ((1 << hl) - 1))), 0).r + ((base & 31u) << (pppLog - 5u));
    if (cls == 0 || cls == 5)
    {
        uint v = vramWord(a);
        if (psm == 1u || psm == 49u) return v & 0xFFFFFFu;
        if (psm == 27u) return v >> 24u;
        if (psm == 36u) return (v >> 24u) & 15u;
        if (psm == 44u) return v >> 28u;
        return v;
    }
    if (cls == 3) return (vramWord(a >> 2u) >> ((a & 3u) * 8u)) & 255u;
    if (cls == 4) return (vramWord(a >> 3u) >> ((a & 7u) * 4u)) & 15u;
    return (vramWord(a >> 1u) >> ((a & 1u) * 16u)) & 0xFFFFu;
}
uint c5551to8888(uint c)
{
    return ((c & 31u) << 3u) | (((c >> 5u) & 31u) << 11u) | (((c >> 10u) & 31u) << 19u) | (((c >> 15u) & 1u) << 31u);
}
)";

    const char *kQuadVs = R"(#version 330 core
layout(location = 0) in vec2 aXY;
uniform vec2 uSize;
void main() { gl_Position = vec4(aXY / uSize * 2.0 - 1.0, 0.0, 1.0); }
)";

    // One flat colour (the alpha-bit fast path draws it under a logic op).
    const char *kFillFs = R"(#version 330 core
uniform vec4 uColor;
out vec4 oColor;
void main() { oColor = uColor; }
)";

    // vtex -> render target (colour or depth), one quad per page; every internal-resolution pixel gets its GS pixel.
    const char *kLoadFs = R"(
uniform uint uPsm;
uniform uint uBase;
uniform uint uBw;
uniform float uDepthScale;
uniform int uScale;
out vec4 oColor;
void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy) / uScale;
    uint raw = readRaw(uPsm, uBase, uBw, p.x, p.y);
#ifdef DEPTH
    gl_FragDepth = float(raw) / uDepthScale;
    oColor = vec4(0.0);
#else
    int cls = psmClass(uPsm);
    if (cls == 1 || cls == 2) raw = c5551to8888(raw);
    oColor = vec4(float(raw & 255u), float((raw >> 8u) & 255u), float((raw >> 16u) & 255u), float(raw >> 24u)) / 255.0;
#endif
}
)";

    // vtex rows -> RGBA8 rows with the same bytes: reading back RGBA8 is the fast path for drivers, R32UI is not.
    const char *kPackFs = R"(#version 330 core
uniform usampler2D uWords;
out vec4 oColor;
void main()
{
    uint w = texelFetch(uWords, ivec2(gl_FragCoord.xy), 0).r;
    oColor = vec4(float(w & 255u), float((w >> 8u) & 255u), float((w >> 16u) & 255u), float(w >> 24u)) / 255.0;
}
)";

    // Colour targets -> presentation texture at the internal resolution: the read circuits merged as in
    // GSCpuBackend::PresentFromLocalMemory (circuit 2 or the background, circuit 1 blended over it by ALP
    // or its alpha), alpha 1; in field mode the rows of one field, doubled.
    const char *kPresentFs = R"(#version 330 core
uniform sampler2D uSrc1;
uniform sampler2D uSrc2;
uniform ivec2 uOrigin1;
uniform ivec2 uOrigin2;
uniform ivec2 uSize1;   // internal-resolution rect of each circuit, (0,0) = circuit off
uniform ivec2 uSize2;
uniform bool uSixteen1; // 16-bit frame: one alpha bit
uniform bool uBg2;      // circuit 2 replaced by the background (SLBG)
uniform vec3 uBg;
uniform float uAlp;     // ALP / 255, or < 0: circuit 1's own alpha (x2, clamped)
uniform int uScale;
uniform int uField;     // -1 every row, else rows 2n + uField
out vec4 oColor;
void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    if (uField >= 0)
    {
        int y = p.y / uScale;
        p.y = (((y >> 1) << 1) + uField) * uScale + p.y % uScale;
    }
    vec3 c = uBg;
    if (!uBg2 && p.x < uSize2.x && p.y < uSize2.y)
        c = texelFetch(uSrc2, uOrigin2 + p, 0).rgb;
    if (p.x < uSize1.x && p.y < uSize1.y)
    {
        vec4 s = texelFetch(uSrc1, uOrigin1 + p, 0);
        float a = uSixteen1 ? (s.a >= 0.5 ? 128.0 / 255.0 : 0.0) : s.a;
        float f = uAlp >= 0.0 ? uAlp : min(1.0, a * 2.0);
        c = uSize2.x > 0 || uBg2 ? c + (s.rgb - c) * f : s.rgb;
    }
    oColor = vec4(c, 1.0);
}
)";

    // render target -> vtex rows (one row = one page). At an internal resolution above 1 each GS pixel
    // takes one sample of its block (the one at or just past its centre): exact values, no averaging, so
    // alpha bits, 16-bit packing, Z and colour buffers read back as palette indices stay what was drawn.
    const char *kWritebackFs = R"(
uniform sampler2D uSrc;
uniform int uCls;
uniform uint uBasePage;
uniform uint uPpr;
uniform float uDepthScale;
uniform float uMaxZ;
uniform bool uDepth;
uniform int uScale;
out uint oWord;
ivec2 invXY(int o)
{
    uint v = texelFetch(uLut, ivec2(o & 127, kInvRow[uCls] + (o >> 7)), 0).r;
    return ivec2(int(v & 255u), int(v >> 8u));
}
uint encodePixel(ivec2 xy, bool sixteen)
{
    xy = xy * uScale + uScale / 2;
    if (uDepth)
    {
        float d = texelFetch(uSrc, xy, 0).r;
        return uint(clamp(floor(d * uDepthScale + 0.5), 0.0, uMaxZ));
    }
    uvec4 u = uvec4(texelFetch(uSrc, xy, 0) * 255.0 + 0.5);
    if (sixteen)
        return (u.r >> 3u) | ((u.g >> 3u) << 5u) | ((u.b >> 3u) << 10u) | ((u.a >> 7u) << 15u);
    return u.r | (u.g << 8u) | (u.b << 16u) | (u.a << 24u);
}
void main()
{
    int col = int(gl_FragCoord.x);
    uint rel = (uint(gl_FragCoord.y) - uBasePage) & 511u;
    ivec2 org = ivec2(int(rel % uPpr) * kPageW[uCls], int(rel / uPpr) * kPageH[uCls]);
    if (uCls == 0 || uCls == 5)
        oWord = encodePixel(org + invXY(col), false);
    else
        oWord = encodePixel(org + invXY(col * 2), true) | (encodePixel(org + invXY(col * 2 + 1), true) << 16u);
}
)";

    const char *kDrawVs = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aColor;
layout(location = 2) in vec3 aSTQ;
layout(location = 3) in uint aState;
layout(location = 4) in vec4 aClamp;
uniform usamplerBuffer uStates;
uniform int uStateBase;
uniform vec2 uRtSize;
uniform float uDepthScale;
flat out uvec4 vS0;
flat out uvec4 vS1;
flat out uint vSnap;
flat out uint vClampOn;
flat out vec4 vClamp;
noperspective out vec4 vColor;
noperspective out vec3 vSTQ;
out float gl_ClipDistance[4];
void main()
{
    int i = (uStateBase + int(aState & 0x3FFFFFFFu)) * 2;
    vSnap = aState >> 31u;
    vClampOn = (aState >> 30u) & 1u;
    vClamp = aClamp;
    vS0 = texelFetch(uStates, i);
    vS1 = texelFetch(uStates, i + 1);
    gl_Position = vec4(aPos.x / uRtSize.x * 2.0 - 1.0, aPos.y / uRtSize.y * 2.0 - 1.0, aPos.z / uDepthScale * 2.0 - 1.0, 1.0);
    // Scissor as clip planes in guest pixels (positions are linear): a fragment centre at hp + 0.5 lands in
    // guest pixel hp / scale, so x0 <= that <= x1 is exactly x0 <= pos.x <= x1 + 1. Clipping instead of a
    // fragment-shader discard keeps early depth testing on.
    gl_ClipDistance[0] = aPos.x - float(vS1.x & 0xFFFFu);
    gl_ClipDistance[1] = float(vS1.x >> 16u) + 1.0 - aPos.x;
    gl_ClipDistance[2] = aPos.y - float(vS1.y & 0xFFFFu);
    gl_ClipDistance[3] = float(vS1.y >> 16u) + 1.0 - aPos.y;
    vColor = aColor * 255.0;
    vSTQ = aSTQ;
}
)";

    // State record (two uvec4 per draw state):
    //  s0.x tbp0 | tbw<<14 | psm<<20 | tme<<26 | linear<<27 | tcc<<28 | tfx<<29
    //  s0.y tw | th<<4 | wms<<8 | wmt<<10 | clutRow<<12 | cpsm<<20 | csa<<26 | csm<<31
    //  s0.z ate | atst<<1 | aref<<4 | fba<<12 | fix<<13 | csel<<21
    //  s0.w ta0 | ta1<<8 | aem<<16
    //  s1.x scissor x0 | x1<<16, s1.y y0 | y1<<16, s1.z minU | maxU<<16, s1.w minV | maxV<<16
    const char *kDrawFs = R"(
uniform usampler2D uClut;
uniform sampler2D uFbCopy;
uniform int uOutMode;   // 0 plain, 1 dual-source factor, 2 blend in shader, 3 (Cs*C)>>7 for an additive ONE/ONE blend
uniform int uAlphaMode; // 0 no alpha test, 1 keep passing, 2 keep failing
uniform int uBurn;      // PS2X_GS_GPU_BURN: dummy ALU work per pixel to act like a slower GPU (0 off)
uniform int uDate;      // 0 off, 1 needs dest alpha bit set, 2 needs it clear
uniform int uSelA;
uniform int uSelB;
uniform int uSelD;
uniform uint uAlphaKeep;
uniform bool uUseCopy;
uniform bool uFrameCt24;
uniform int uScale;        // internal resolution: GS pixel = gl_FragCoord / uScale
uniform bool uHiSrc;       // the texture is a colour target (same base, width, layout) read at the internal resolution
uniform sampler2D uHiTex;
uniform bool uRepl;        // the texture is a high-resolution pack picture (gs_texture_hd.h)
uniform vec2 uReplSize;    // the texels it covers
uniform sampler2D uReplTex;
flat in uvec4 vS0;
flat in uvec4 vS1;
flat in uint vSnap;        // 1:1 texture mapping: sample at the GS pixel centre (no half-texel bleed)
flat in uint vClampOn;     // sprite: keep s/t within what its 1x pixel centres sample (q is 1)
flat in vec4 vClamp;
noperspective in vec4 vColor;
noperspective in vec3 vSTQ;
layout(location = 0, index = 0) out vec4 oColor;
layout(location = 0, index = 1) out vec4 oFactor;
uniform usampler2D uDecTex; // decoded-texture cache: s0.w bit 31 set = texels at (s0.y bits 12-20, 21-29) * 8
)";
    // Texel decoding from the vtex mirror, shared by the draw shader and the decode-cache program (which
    // declares vS0 as a uniform). Reads s0.x/y/w only.
    const char *kTexelGlsl = R"(
uint applyTexa(uint psm, uint t)
{
    if (psm != 1u && psm != 2u && psm != 10u) return t;
    uint ta0 = vS0.w & 255u, ta1 = (vS0.w >> 8u) & 255u;
    bool aem = (vS0.w & 0x10000u) != 0u;
    bool rgbZero = (t & 0xFFFFFFu) == 0u;
    uint a = t >> 24u;
    if (psm == 1u) a = (aem && rgbZero) ? 0u : ta0;
    else a = ((a & 0x80u) != 0u) ? ta1 : ((aem && rgbZero) ? 0u : ta0);
    return (t & 0xFFFFFFu) | (a << 24u);
}
uint clutEntry(uint i) { return texelFetch(uClut, ivec2(int(i), int((vS0.y >> 12u) & 255u)), 0).r; }
uint lookupClut(uint index, uint psm)
{
    uint cpsm = (vS0.y >> 20u) & 63u;
    uint csa = (vS0.y >> 26u) & 31u;
    uint csm = vS0.y >> 31u;
    bool sixteen = cpsm == 2u || cpsm == 10u;
    uint clutBase = (csa & (sixteen ? 31u : 15u)) << 4u;
    bool four = psm == 20u || psm == 36u || psm == 44u;
    uint src = four ? (index & 15u) : index;
    uint ci = (clutBase + src) & (sixteen ? 511u : 255u);
    if (!sixteen && csm == 0u && (psm == 19u || psm == 27u))
        ci = min((src & 0xF0u) + clutBase, 240u) + (src & 15u);
    if (cpsm == 0u) return clutEntry(ci) | (clutEntry(ci + 256u) << 16u);
    if (cpsm == 1u) return applyTexa(1u, (clutEntry(ci) | (clutEntry(ci + 256u) << 16u)) & 0xFFFFFFu);
    if (sixteen) return applyTexa(cpsm, c5551to8888(clutEntry(ci)));
    return 0xFFFF00FFu;
}
uint decodeTexel(int u, int v)
{
    uint psm = (vS0.x >> 20u) & 63u;
    uint raw = readRaw(psm, vS0.x & 0x3FFFu, (vS0.x >> 14u) & 63u, u, v);
    if (psm == 0u || psm == 1u || psm == 48u || psm == 49u) return applyTexa(psm, raw);
    if (psm == 2u || psm == 10u || psm == 50u || psm == 58u) return applyTexa(psm, c5551to8888(raw));
    if (psm == 19u || psm == 27u || psm == 20u || psm == 36u || psm == 44u) return lookupClut(raw & 255u, psm);
    return 0xFFFF00FFu;
}
)";
    // Everything from here on: the draw shader body (after kTexelGlsl).
    const char *kDrawFsMain = R"(
int wrapCoord(int c, int size, uint mode, uint rmin, uint rmax)
{
    if (mode == 0u) return int(uint(c) & uint(size - 1));
    if (mode == 1u) return clamp(c, 0, size - 1);
    if (mode == 2u) return min(max(c, int(rmin)), int(rmax));
    return int((uint(c) & rmin) | rmax);
}
vec4 fetchWrapped(int u, int v, int tw, int th)
{
    uint wms = (vS0.y >> 8u) & 3u, wmt = (vS0.y >> 10u) & 3u;
    int wu = wrapCoord(u, tw, wms, vS1.z & 0xFFFFu, vS1.z >> 16u), wv = wrapCoord(v, th, wmt, vS1.w & 0xFFFFu, vS1.w >> 16u);
    uint c = (vS0.w & 0x80000000u) != 0u
        ? texelFetch(uDecTex, ivec2(int((vS0.y >> 12u) & 511u) * 8 + wu, int((vS0.y >> 21u) & 511u) * 8 + wv), 0).r
        : decodeTexel(wu, wv);
    return vec4(float(c & 255u), float((c >> 8u) & 255u), float((c >> 16u) & 255u), float(c >> 24u));
}
// Wrap at the internal resolution (modes 0-2; region repeat never reads uHiTex).
int hiWrap(int c, int size, uint mode, uint range)
{
    int n = size * uScale;
    if (mode == 0u) return ((c % n) + n) % n;
    if (mode == 1u) return clamp(c, 0, n - 1);
    return clamp(c, int(range & 0xFFFFu) * uScale, int(range >> 16u) * uScale + uScale - 1);
}
vec4 fetchHi(int u, int v, int tw, int th)
{
    uint wms = (vS0.y >> 8u) & 3u, wmt = (vS0.y >> 10u) & 3u;
    ivec2 at = clamp(ivec2(hiWrap(u, tw, wms, vS1.z), hiWrap(v, th, wmt, vS1.w)), ivec2(0), textureSize(uHiTex, 0) - 1);
    uvec4 t = uvec4(texelFetch(uHiTex, at, 0) * 255.0 + 0.5);
    uint psm = (vS0.x >> 20u) & 63u;
    if (psm == 2u || psm == 10u) // what a 16-bit writeback keeps
    {
        t.rgb &= uvec3(0xF8u);
        t.a = t.a >= 128u ? 128u : 0u;
    }
    uint c = applyTexa(psm, t.r | (t.g << 8u) | (t.b << 16u) | (t.a << 24u));
    return vec4(float(c & 255u), float((c >> 8u) & 255u), float((c >> 16u) & 255u), float(c >> 24u));
}
vec4 fetchAny(bool hi, int u, int v, int tw, int th)
{
    return hi ? fetchHi(u, v, tw, th) : fetchWrapped(u, v, tw, th);
}
uvec4 sampleTexture(vec3 stq, vec2 dx, vec2 dy)
{
    int tw = 1 << min(int(vS0.y & 15u), 10);
    int th = 1 << min(int((vS0.y >> 4u) & 15u), 10);
    float q = abs(stq.z) > 1e-8 ? stq.z : 1.0;
    float invQ = 1.0 / q;
    if (uRepl)
    {
        // Filtered (mipmapped) at the picture's own resolution, whatever the primitive asked for.
        vec2 k = vec2(float(tw), float(th)) / uReplSize * invQ;
        vec4 t = textureGrad(uReplTex, stq.xy * k, dx * k, dy * k);
        return uvec4(clamp(floor(t * 255.0 + 0.5), 0.0, 255.0));
    }
    float tu = stq.x * invQ * float(tw);
    float tv = stq.y * invQ * float(th);
    bool hi = uHiSrc;
    if (hi)
    {
        tu *= float(uScale);
        tv *= float(uScale);
    }
    if ((vS0.x & (1u << 27u)) == 0u)
        return hi ? uvec4(fetchHi(int(floor(tu)), int(floor(tv)), tw, th)) : uvec4(fetchWrapped(int(tu), int(tv), tw, th));
    float su = tu - 0.5, sv = tv - 0.5;
    int u0 = int(floor(su)), v0 = int(floor(sv));
    float fx = su - float(u0), fy = sv - float(v0);
    vec4 c00 = fetchAny(hi, u0, v0, tw, th);
    vec4 c10 = fetchAny(hi, u0 + 1, v0, tw, th);
    vec4 c01 = fetchAny(hi, u0, v0 + 1, tw, th);
    vec4 c11 = fetchAny(hi, u0 + 1, v0 + 1, tw, th);
    vec4 top = c00 + (c10 - c00) * fx;
    vec4 bottom = c01 + (c11 - c01) * fx;
    vec4 val = top + (bottom - top) * fy;
    return uvec4(clamp(floor(val + 0.5), 0.0, 255.0));
}
uvec4 combine(uvec4 v, uvec4 t)
{
    uint tfx = (vS0.x >> 29u) & 3u;
    bool tcc = (vS0.x & (1u << 28u)) != 0u;
    uvec4 o;
    if (tfx == 0u)
    {
        o.rgb = min((t.rgb * v.rgb) >> 7u, uvec3(255u));
        o.a = tcc ? min((t.a * v.a) >> 7u, 255u) : v.a;
    }
    else if (tfx == 1u)
    {
        o.rgb = t.rgb;
        o.a = tcc ? t.a : v.a;
    }
    else if (tfx == 2u)
    {
        o.rgb = min(((t.rgb * v.rgb) >> 7u) + uvec3(v.a), uvec3(255u));
        o.a = tcc ? min(t.a + v.a, 255u) : v.a;
    }
    else
    {
        o.rgb = min(((t.rgb * v.rgb) >> 7u) + uvec3(v.a), uvec3(255u));
        o.a = tcc ? t.a : v.a;
    }
    return o;
}
ivec3 pick(int sel, ivec3 cs, ivec3 cd)
{
    if (sel == 0) return cs;
    if (sel == 1) return cd;
    return ivec3(0);
}
void main()
{
    ivec2 hp = ivec2(gl_FragCoord.xy);
    ivec2 p = hp / uScale;
    vec2 dx = dFdx(vSTQ.xy), dy = dFdy(vSTQ.xy); // before any discard (uniform control flow)
    vec3 stq = vSTQ;
    if (vSnap != 0u && !uHiSrc && !uRepl)
    {
        vec2 off = (vec2(p) + 0.5) * float(uScale) - gl_FragCoord.xy;
        stq.xy += dx * off.x + dy * off.y;
    }
    if (vClampOn != 0u && !uHiSrc && !uRepl)
        stq.xy = clamp(stq.xy, vClamp.xy, vClamp.zw);
    uvec4 c = uvec4(clamp(floor(vColor + 0.001), 0.0, 255.0));
    if ((vS0.x & (1u << 26u)) != 0u)
        c = combine(c, sampleTexture(stq, dx, dy));
#ifndef NO_DISCARD
    if (uAlphaMode != 0)
    {
        uint atst = (vS0.z >> 1u) & 7u, aref = (vS0.z >> 4u) & 255u;
        bool pass = true;
        if (atst == 0u) pass = false;
        else if (atst == 2u) pass = c.a < aref;
        else if (atst == 3u) pass = c.a <= aref;
        else if (atst == 4u) pass = c.a == aref;
        else if (atst == 5u) pass = c.a >= aref;
        else if (atst == 6u) pass = c.a > aref;
        else if (atst == 7u) pass = c.a != aref;
        if ((uAlphaMode == 1) != pass) discard;
    }
#endif
    uint oldA = 0u;
    vec3 oldC = vec3(0.0);
    if (uUseCopy)
    {
        vec4 o = texelFetch(uFbCopy, hp, 0);
        oldA = uint(o.a * 255.0 + 0.5);
        oldC = floor(o.rgb * 255.0 + 0.5);
    }
#ifndef NO_DISCARD
    if (uDate == 1 && (oldA & 0x80u) == 0u) discard;
    if (uDate == 2 && (oldA & 0x80u) != 0u) discard;
#endif
    uint csel = (vS0.z >> 21u) & 3u;
    uint ca = csel == 0u ? c.a : (csel == 1u ? (uFrameCt24 ? 128u : oldA) : ((vS0.z >> 13u) & 255u));
    vec3 rgb = vec3(c.rgb);
    oFactor = vec4(min(float(ca) / 128.0, 1.0));
    if (uOutMode == 2)
    {
        ivec3 cs = ivec3(c.rgb), cd = ivec3(oldC);
        ivec3 r = (((pick(uSelA, cs, cd) - pick(uSelB, cs, cd)) * int(ca)) >> 7) + pick(uSelD, cs, cd);
        rgb = vec3(clamp(r, 0, 255));
    }
    else if (uOutMode == 3)
        rgb = vec3((ivec3(c.rgb) * int(ca)) >> 7); // above 255 the output clamps to 1, and so does the clamped sum
    uint a = c.a;
    if ((vS0.z & (1u << 12u)) != 0u) a |= 0x80u;
    a = (a & ~uAlphaKeep) | (oldA & uAlphaKeep);
    if (uBurn > 0)
    {
        float k = gl_FragCoord.x * 0.013 + gl_FragCoord.y * 0.007;
        for (int i = 0; i < uBurn; ++i)
            k = sin(k * 1.37 + 0.11) * 0.93 + cos(k * 0.71);
        rgb += vec3(k * 1e-30); // too small to change the output, but keeps the loop
    }
    oColor = vec4(rgb / 255.0, float(a) / 255.0);
}
)";

    struct GpuVertex
    {
        float x, y, z;
        uint8_t r, g, b, a;
        float s, t, q;
        uint32_t state;
        float cs0, ct0, cs1, ct1; // sprite texture clamp (state bit 30)
    };
    static_assert(sizeof(GpuVertex) == 48, "vertex layout");

    struct StateRecord
    {
        uint32_t w[8];
    };

    // GL-level state of a draw call; consecutive primitives with an equal key share one draw.
    struct DrawKey
    {
        uint32_t fbp, fbw, zbp;
        uint8_t fcls, zcls;
        uint8_t colorOn, depthOn;
        uint8_t colorMask; // bit0 r .. bit3 a
        uint8_t depthWrite, depthFunc; // 1 always, 2 gequal, 3 greater
        uint8_t outMode;   // see kDrawFs
        uint16_t blendSrc, blendDst, blendEq;
        uint8_t selA, selB, selD;
        uint8_t copy, date, alphaKeep;
        uint8_t ate, afail;
        uint8_t ct24;
        uint8_t singlePrim;
        uint16_t needH;
        // Texture read straight from a colour target at the internal resolution (see hiSourceUsable).
        uint8_t srcOn, srcCls;
        uint16_t srcBase, srcBw;
        // High-resolution pack picture (1-based index into ps2xTexHdPictures) and its wrap (bit0 s, bit1 t repeat).
        uint16_t hdId;
        uint8_t hdWrap, pad0;
    };

    struct RenderTarget
    {
        uint32_t basePage = 0, bw = 1;
        int cls = 0;
        bool depth = false;
        int pageW = 64, pageH = 32;
        uint32_t ppr = 1;
        int width = 0, height = 0;
        GLuint tex = 0, fbo = 0;
        PageMask valid, dirty;
        uint64_t lastUse = 0;
    };

    std::atomic<GSGpuBackend *> g_instance{nullptr};

    // PS2X_GS_SCALE=1..4: internal resolution of the GPU renderer (render targets are scale x the GS
    // size; what reads GS memory still sees GS pixels). 1 = the GS's own resolution. Unset = kDefaultScale.
    // PS2X_GS_SCALE=auto (or 0), opt-in: starts at kDefaultScale, steps down while the GPU cannot keep up
    // and back up once it has room (autoScaleStep).
    constexpr int kDefaultScale = 3;
    struct ScaleSetting
    {
        int scale;
        bool autoMode;
    };
    const ScaleSetting &scaleSetting()
    {
        static const ScaleSetting s = []
        {
            const char *e = std::getenv("PS2X_GS_SCALE");
            if (!e || !*e)
                return ScaleSetting{kDefaultScale, false};
            if (std::atoi(e) <= 0)
                return ScaleSetting{kDefaultScale, true};
            return ScaleSetting{std::clamp(std::atoi(e), 1, 4), false};
        }();
        return s;
    }
    int requestedScale() { return scaleSetting().scale; }
    bool autoScale() { return scaleSetting().autoMode; }

    // High-resolution pictures handed from the renderer's worker to the presenter (main thread): a ring
    // of textures in the shared context; fences order the two contexts both ways.
    struct HiresSlot
    {
        GLuint tex = 0, fbo = 0;
        int w = 0, h = 0;
        GLsync ready = nullptr; // worker: picture complete
        GLsync done = nullptr;  // presenter: finished drawing it
    };
    struct HiresShared
    {
        std::mutex mutex;
        std::array<HiresSlot, 3> slots;
        int latest = -1, inUse = -1;
        uint64_t served = 0;            // presents served by the worker
        uint64_t publishedFor = ~0ull;  // present the latest picture belongs to
    };
    HiresShared g_hires;

    double nowSeconds()
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
}

class GSGpuBackend final : public GSRasterBackend
{
public:
    GSGpuBackend()
    {
        g_instance.store(this);
        std::lock_guard<std::mutex> lock(g_host.mutex);
        if (g_host.ready)
            startWorkerLocked();
    }
    ~GSGpuBackend() override
    {
        stopWorker();
        GSGpuBackend *self = this;
        g_instance.compare_exchange_strong(self, nullptr);
    }

    // Main thread (ps2xGsGpuHostInit / Shutdown); g_host.mutex held.
    void startWorkerLocked()
    {
        std::lock_guard<std::mutex> process(m_processMutex);
        if (m_threaded.load())
            return;
        m_stop = false;
        m_threaded.store(true);
        m_thread = std::thread([this] { workerMain(); });
    }
    void stopWorker()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_threaded.load())
                return;
            m_stop = true;
        }
        m_wake.notify_all();
        if (m_thread.joinable())
            m_thread.join();
        std::lock_guard<std::mutex> process(m_processMutex);
        m_threaded.store(false);
    }

    void Initialize(uint8_t *vram, uint32_t vramSize) override
    {
        call([this, vram, vramSize] { initShadow(vram, vramSize); });
    }
    void Reset() override
    {
        call([this] {
            flushDraw();
            m_cpu.Reset();
            m_clut.fill(0);
            m_clutCbp = {};
            m_stateValid = false;
        });
    }
    void Submit(const GSPrimitiveBatch &batch) override
    {
        if (batch.vertexCount == 0u)
            return;
        if (!m_threaded.load())
        {
            std::lock_guard<std::mutex> process(m_processMutex);
            if (!m_threaded.load())
            {
                setState(batch.state);
                Prim p;
                p.count = batch.vertexCount;
                p.v = batch.vertices;
                addPrim(p);
                return;
            }
        }
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_hasLastState || std::memcmp(&m_lastState, &batch.state, sizeof(GSDrawState)) != 0)
        {
            m_lastState = batch.state;
            m_hasLastState = true;
            m_pending.cmds.push_back({Op::State, static_cast<uint32_t>(m_pending.states.size()), 0});
            m_pending.states.push_back(batch.state);
            ++m_enqueued; // one ticket per queued command (m_completed counts commands)
        }
        m_pending.cmds.push_back({Op::Prim, static_cast<uint32_t>(m_pending.prims.size()), 0});
        Prim &p = m_pending.prims.emplace_back();
        p.count = batch.vertexCount;
        p.v = batch.vertices;
        afterPushLocked(lock);
    }
    void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) override
    {
        if (directCall([&] { loadClut(tex0, texclut); }))
            return;
        std::unique_lock<std::mutex> lock(m_mutex);
        m_pending.cmds.push_back({Op::LoadClut, static_cast<uint32_t>(m_pending.cluts.size()), 0});
        m_pending.cluts.push_back({tex0, texclut});
        afterPushLocked(lock);
    }
    void BeginTransfer(const GSTransferCommand &command) override
    {
        if (directCall([&] { beginTransfer(command); }))
            return;
        std::unique_lock<std::mutex> lock(m_mutex);
        m_pending.cmds.push_back({Op::Transfer, static_cast<uint32_t>(m_pending.transfers.size()), 0});
        m_pending.transfers.push_back(command);
        afterPushLocked(lock);
    }
    void UploadImage(const uint8_t *data, uint32_t sizeBytes) override
    {
        if (!data || sizeBytes == 0u)
            return;
        if (directCall([&] { uploadImage(data, sizeBytes); }))
            return;
        std::unique_lock<std::mutex> lock(m_mutex);
        m_pending.cmds.push_back({Op::Upload, static_cast<uint32_t>(m_pending.data.size()), sizeBytes});
        m_pending.data.insert(m_pending.data.end(), data, data + sizeBytes);
        afterPushLocked(lock);
    }
    void Flush() override {}
    void TextureFlush() override {}
    void Sync(GSSyncReason reason) override
    {
        if (reason == GSSyncReason::Presentation || reason == GSSyncReason::Finish)
            return; // every reader below synchronises itself
        waitIdle();
    }
    PresentationFrame Present(const GSPresentationRequest &request) override;
    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override
    {
        bool result = false;
        call([&] {
            flushDraw();
            const uint32_t fbp = context.frame.fbp * 32u;
            const PageMask m = rectPages(context.frame.psm, fbp, std::max<uint32_t>(context.frame.fbw, 1u),
                                         context.scissor.x0, context.scissor.y0, context.scissor.x1, context.scissor.y1);
            ensureCpu(m);
            result = m_cpu.ClearFramebuffer(context, rgba);
            cpuWritten(m);
        });
        return result;
    }
    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override
    {
        waitIdle();
        return m_cpu.ConsumeLocalToHostBytes(dst, maxBytes);
    }
    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override
    {
        uint32_t value = 0;
        auto *self = const_cast<GSGpuBackend *>(this);
        self->call([&] {
            self->flushDraw();
            self->ensureCpu(rectPages(psm, base, std::max<uint32_t>(bw, 1u), static_cast<int>(x), static_cast<int>(y),
                                      static_cast<int>(x), static_cast<int>(y)));
            value = self->m_cpu.ReadVram(psm, base, bw, x, y);
        });
        return value;
    }
    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override
    {
        call([=] {
            flushDraw();
            const PageMask m = rectPages(psm, base, std::max<uint32_t>(bw, 1u), static_cast<int>(x), static_cast<int>(y),
                                         static_cast<int>(x), static_cast<int>(y));
            ensureCpu(m);
            m_cpu.WriteVram(psm, base, bw, x, y, value);
            cpuWritten(m);
        });
    }
    void SnapshotVram(std::vector<uint8_t> &out) const override
    {
        auto *self = const_cast<GSGpuBackend *>(this);
        self->call([&] {
            self->flushDraw();
            self->ensureCpu(PageMask().set());
            self->m_cpu.SnapshotVram(out);
        });
    }
    GSTransferSnapshot GetTransferSnapshot() const override { return m_cpu.GetTransferSnapshot(); }

private:
    // ------------------------------------------------------------ command queue
    enum class Op : uint8_t
    {
        State,
        Prim,
        LoadClut,
        Transfer,
        Upload,
        Call,
    };
    struct Cmd
    {
        Op op;
        uint32_t a;
        uint32_t b;
    };
    struct Prim
    {
        uint8_t count = 0;
        std::array<GSVertex, 3> v{};
    };
    struct ClutCmd
    {
        GSTex0Reg tex0;
        GSTexClutReg texclut;
    };
    struct Queue
    {
        std::vector<Cmd> cmds;
        std::vector<GSDrawState> states;
        std::vector<Prim> prims;
        std::vector<ClutCmd> cluts;
        std::vector<GSTransferCommand> transfers;
        std::vector<uint8_t> data;
        std::vector<std::function<void()>> calls;
        void clear()
        {
            cmds.clear();
            states.clear();
            prims.clear();
            cluts.clear();
            transfers.clear();
            data.clear();
            calls.clear();
        }
    };
    static constexpr size_t kMaxQueuedPrims = 65536;

    // Runs fn now when there is no worker (before the GL context exists); false = enqueue it.
    template <typename Fn>
    bool directCall(Fn &&fn)
    {
        if (m_threaded.load())
            return false;
        std::lock_guard<std::mutex> process(m_processMutex);
        if (m_threaded.load())
            return false;
        fn();
        return true;
    }
    // Runs fn in command order and waits for it.
    void call(std::function<void()> fn)
    {
        if (directCall([&] { fn(); }))
            return;
        uint64_t ticket;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_pending.cmds.push_back({Op::Call, static_cast<uint32_t>(m_pending.calls.size()), 0});
            m_pending.calls.push_back(std::move(fn));
            ticket = ++m_enqueued;
            if (m_workerWaiting)
                m_wake.notify_one();
            m_progress.wait(lock, [&] { return m_completed >= ticket || !m_threaded.load(); });
        }
    }
    void waitIdle()
    {
        if (!m_threaded.load())
            return;
        std::unique_lock<std::mutex> lock(m_mutex);
        const uint64_t ticket = m_enqueued;
        if (m_workerWaiting)
            m_wake.notify_one();
        m_progress.wait(lock, [&] { return m_completed >= ticket || !m_threaded.load(); });
    }
    void afterPushLocked(std::unique_lock<std::mutex> &lock)
    {
        ++m_enqueued;
        if (m_workerWaiting)
            m_wake.notify_one();
        if (m_pending.prims.size() >= kMaxQueuedPrims)
            m_space.wait(lock, [&] { return m_pending.prims.size() < kMaxQueuedPrims || m_stop; });
    }

    void workerMain()
    {
        ThreadNaming::SetCurrentThreadName("GsGpu");
        {
            std::lock_guard<std::mutex> process(m_processMutex);
            initGl();
        }
        publishGlReady(m_glOk, m_glFailReason);
        Queue work;
        for (;;)
        {
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                for (;;)
                {
                    if (!m_pending.cmds.empty() || m_stop)
                        break;
                    if (m_presentRequested.load())
                    {
                        lock.unlock();
                        {
                            std::lock_guard<std::mutex> process(m_processMutex);
                            servePresent();
                        }
                        lock.lock();
                        continue;
                    }
                    m_workerWaiting = true;
                    m_wake.wait_for(lock, std::chrono::milliseconds(2));
                    m_workerWaiting = false;
                }
                if (m_pending.cmds.empty() && m_stop)
                    break;
                std::swap(work, m_pending);
            }
            m_space.notify_all();
            {
                std::lock_guard<std::mutex> process(m_processMutex);
                execute(work);
                flushDraw();
                if (m_glOk)
                    gl.Flush();
            }
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_completed += work.cmds.size();
            }
            m_progress.notify_all();
            work.clear();
        }
        {
            std::lock_guard<std::mutex> process(m_processMutex);
            shutdownGl();
        }
        m_progress.notify_all();
    }

    void execute(Queue &q)
    {
        size_t n = 0;
        for (const Cmd &c : q.cmds)
        {
            switch (c.op)
            {
            case Op::State: setState(q.states[c.a]); break;
            case Op::Prim: addPrim(q.prims[c.a]); break;
            case Op::LoadClut: loadClut(q.cluts[c.a].tex0, q.cluts[c.a].texclut); break;
            case Op::Transfer: beginTransfer(q.transfers[c.a]); break;
            case Op::Upload: uploadImage(q.data.data() + c.a, c.b); break;
            case Op::Call: q.calls[c.a](); break;
            }
            if ((++n & 63u) == 0u && m_presentRequested.load(std::memory_order_relaxed))
                servePresent();
        }
    }

    // ------------------------------------------------------------ worker state
    std::mutex m_mutex;           // queue
    std::mutex m_processMutex;    // everything below (worker or direct caller)
    std::condition_variable m_wake, m_progress, m_space;
    Queue m_pending;
    uint64_t m_enqueued = 0, m_completed = 0;
    bool m_stop = false;
    bool m_workerWaiting = false;
    std::atomic<bool> m_threaded{false};
    std::thread m_thread;
    GSDrawState m_lastState{};
    bool m_hasLastState = false;

    uint8_t *m_shadow = nullptr;
    uint32_t m_shadowSize = 0;
    mutable GSCpuBackend m_cpu;
    PageMask m_cpuValid = PageMask().set();
    PageMask m_vtexValid;
    std::vector<std::unique_ptr<RenderTarget>> m_rts;
    uint64_t m_useClock = 0;

    std::array<uint16_t, 512> m_clut{};
    std::array<uint32_t, 2> m_clutCbp{};
    static constexpr int kClutRows = 256;
    static constexpr int kDecSize = 4096;
    std::vector<uint16_t> m_clutRing = std::vector<uint16_t>(kClutRows * 512, 0);
    std::bitset<kClutRows> m_clutRowDirty;
    std::bitset<kClutRows> m_clutRowInBatch;
    int m_clutRow = 0;
    bool m_clutRowCurrent = false; // m_clutRow holds m_clut

    // Current draw state, derived once per state change.
    GSDrawState m_state{};
    bool m_stateValid = false;
    bool m_cpuOnly = false;
    const char *m_cpuReason = nullptr;
    bool m_skip = false;
    DrawKey m_key{};
    StateRecord m_rec{};
    PageMask m_texMask;
    bool m_tme = false;
    int m_ofx = 0, m_ofy = 0;
    bool m_srcCand = false; // the texture could be read from a colour target (DrawKey src* when one is usable)
    uint16_t m_srcBase = 0, m_srcBw = 0, m_srcW = 0, m_srcH = 0;
    uint8_t m_srcCls = 0;
    PageMask m_primSrcMask, m_batchSrc; // pages a primitive / the open batch reads from that target

    // High-resolution pack pictures (gs_texture_hd.h). A region is an upload a picture names, while its
    // texels are still in GS memory; a primitive whose TEX0 (and CLUT, if the picture names one) reads
    // such regions samples the picture. Small textures and CLUTs share pages with other uploads, so a
    // transfer into a region's pages only makes it suspect: the next lookup compares its texels in GS
    // memory with the uploaded bytes. A draw into its pages ends it.
    struct HdRegion
    {
        PageMask pages;
        uint32_t dbp, dbw, psm;
        uint64_t key;
        uint32_t w, h;
        std::vector<uint8_t> bytes; // as uploaded (row-major, packed at the format's bits per pixel)
        bool suspect;
    };
    static bool hdDebug()
    {
        static const bool on = [] { const char *e = std::getenv("PS2X_TEXPACK_HD_DEBUG"); return e && *e && *e != '0'; }();
        return on;
    }
    bool hdIntact(const HdRegion &r)
    {
        if ((r.pages & ~m_cpuValid).any())
            return false;
        const uint32_t bits = (r.psm == GS_PSM_CT24 || r.psm == GS_PSM_Z24) ? 24u : GSInternal::bitsPerPixel(static_cast<uint8_t>(r.psm));
        GSMem::TexturePageCache cache;
        const uint8_t *b = r.bytes.data();
        size_t i = 0; // pixel index
        for (uint32_t y = 0; y < r.h; ++y)
            for (uint32_t x = 0; x < r.w; ++x, ++i)
            {
                uint32_t want;
                if (bits == 4u)
                    want = (b[i >> 1] >> ((i & 1u) * 4u)) & 15u;
                else if (bits == 8u)
                    want = b[i];
                else if (bits == 16u)
                    want = b[i * 2] | (b[i * 2 + 1] << 8);
                else if (bits == 24u)
                    want = b[i * 3] | (b[i * 3 + 1] << 8) | (b[i * 3 + 2] << 16);
                else
                    want = b[i * 4] | (b[i * 4 + 1] << 8) | (b[i * 4 + 2] << 16) | (static_cast<uint32_t>(b[i * 4 + 3]) << 24);
                uint32_t got = GSMem::ReadTexture(cache, m_shadow, r.psm, r.dbp, r.dbw, x, y);
                if (bits == 24u)
                    got &= 0xFFFFFFu;
                if (got != want)
                    return false;
            }
        return true;
    }
    // Suspect regions the current TEX0 or CLUT would use: still intact, or gone.
    void hdVerify(uint32_t tbp0, uint32_t cbp)
    {
        for (size_t i = 0; i < m_hdRegions.size();)
        {
            HdRegion &r = m_hdRegions[i];
            if (r.suspect && (r.dbp == tbp0 || r.dbp == cbp))
            {
                const double t0 = nowSeconds();
                const bool intact = hdIntact(r);
                m_stats.hdVerifyMs += (nowSeconds() - t0) * 1000.0;
                ++m_stats.hdVerifies;
                if (hdDebug())
                    std::fprintf(stderr, "[texpack-hd] region %016llx at %u: %s\n", (unsigned long long)r.key, r.dbp, intact ? "intact" : "overwritten");
                if (!intact)
                {
                    m_hdRegions.erase(m_hdRegions.begin() + static_cast<std::ptrdiff_t>(i));
                    continue;
                }
                r.suspect = false;
            }
            ++i;
        }
    }
    std::vector<HdRegion> m_hdRegions;
    uint32_t m_hdGen = 1;          // bumped when m_hdRegions changes
    uint32_t m_hdLookGen = 0;      // m_hdLook is for this generation (0: state changed)
    uint16_t m_hdLook = 0;
    uint8_t m_hdLookWrap = 0;
    bool m_hdCapture = false;      // the open transfer is hashed when complete
    uint32_t m_hdExpected = 0;
    std::vector<uint8_t> m_hdBytes;
    std::vector<GLuint> m_hdTex;   // per picture, made on first use
    uint32_t m_recIndex = UINT32_MAX; // record index of the current state in the open batch

    // Open batch.
    std::vector<GpuVertex> m_verts;
    std::vector<StateRecord> m_records;
    DrawKey m_batchKey{};
    PageMask m_batchColor, m_batchDepth, m_batchTex;
    int m_bx0 = 0, m_by0 = 0, m_bx1 = -1, m_by1 = -1;
    // 16px tiles a copy-mode batch (DATE, alpha keep, blending) has drawn: the frame copy is taken once per
    // batch, so a primitive over pixels an earlier one in the batch wrote would read stale alpha/colour.
    static constexpr int kCopyTile = 16, kCopyGrid = 128;
    bool m_copyTilesUsed = false;
    // The primitives behind those tiles (convex outlines, listed per tile), tested exactly: strip and fan
    // triangles share edges, which the tiles alone see as overlap, one barrier or flush per triangle.
    struct CopyPoly
    {
        double x[4], y[4];
        int n;
        uint32_t seen;
    };
    std::vector<CopyPoly> m_copyPolys;
    std::vector<std::vector<uint32_t>> m_copyTileList = std::vector<std::vector<uint32_t>>(kCopyGrid * kCopyGrid);
    std::vector<uint32_t> m_copyTouched;
    uint32_t m_copySeen = 0;
    // The copy only feeds bits this draw never writes (FBMSK-kept alpha, DATE on a kept bit 7):
    // what the batch drew under a primitive cannot change what it reads.
    bool m_copyStable = false;
    // Barrier mode: first vertex of each range after the first (a texture barrier goes before every range).
    bool m_barrier = false;
    std::vector<uint32_t> m_barrierSplits;

    // GL objects.
    bool m_glOk = false;
    const char *m_glFailReason = nullptr; // why initGl left m_glOk false
    GLuint m_decTex = 0, m_decFbo = 0; // decoded-texture cache atlas (kDecSize^2 R32UI)
    // Decoded-texture cache: a texture (base, width, format, TEXA, CLUT words, extent) decoded once into the
    // atlas, read by the draw shader with texelFetch instead of the per-texel swizzle/CLUT decode. An entry
    // is dropped as soon as anything writes its VRAM pages (writeback / upload), so it always equals a decode.
    struct DecEntry
    {
        uint32_t w0, w1, w3, w, h;
        std::vector<uint16_t> clut; // CLUT formats: the 512 words of the row it was decoded with
        PageMask pages;
        uint16_t x, y;              // atlas origin / 8
    };
    std::unordered_map<uint64_t, std::vector<DecEntry>> m_dec;
    PageMask m_decPages;
    int m_decX = 0, m_decY = 0, m_decShelf = 0;
    struct DecMemo
    {
        uint32_t w0, w1, w3, w6, w7, out;
    };
    std::vector<DecMemo> m_decMemo; // this flush: record words -> rewritten s0.y (0 = not cached)
    GLuint m_vtex = 0, m_vtexFbo = 0, m_lutTex = 0, m_clutTex = 0, m_stateBuf = 0, m_stateTex = 0;
    GLuint m_vbo = 0, m_vao = 0, m_quadVbo = 0, m_quadVao = 0;
    GLuint m_copyTex = 0, m_copyFbo = 0;
    int m_copyW = 0, m_copyH = 0;
    size_t m_vboOffset = 0, m_stateOffset = 0;
    static constexpr size_t kVboBytes = 8u << 20;
    static constexpr size_t kStateBytes = 1u << 20;
    std::map<std::pair<GLuint, GLuint>, GLuint> m_fbos;
    struct Program
    {
        GLuint id = 0;
        std::map<std::string, GLint> loc;
        GLint u(const char *name)
        {
            auto it = loc.find(name);
            if (it != loc.end())
                return it->second;
            const GLint l = gl.GetUniformLocation(id, name);
            loc[name] = l;
            return l;
        }
    };
    Program m_drawProg, m_drawProgNd, m_loadColorProg, m_loadDepthProg, m_wbProg, m_packProg, m_presentProg, m_fillProg, m_decProg;
    int m_scale = 1; // internal resolution (PS2X_GS_SCALE, limited by GL_MAX_TEXTURE_SIZE)
    GLuint m_srcCopyTex = 0, m_srcCopyFbo = 0; // a target's texture rect, when a draw textures from its own target
    int m_srcCopyW = 0, m_srcCopyH = 0;
    GLuint m_packTex = 0, m_packFbo = 0; // RGBA8 staging rows for display readback
    std::vector<float> m_quads;

    // Presentation handshake.
    std::mutex m_presentMutex;
    std::condition_variable m_presentCv;
    std::atomic<bool> m_presentRequested{false};
    std::atomic<bool> m_zeroFallback{false}; // the last decode showed a context frame for a black display at page 0
    bool m_presentDone = false;
    PageMask m_presentMask;
    GSPresentationRequest m_presentRequest{}; // request posted with m_presentMask
    std::vector<uint8_t> m_presentBuf;
    GSPresentationRequest m_presentBufRequest{}; // request m_presentBuf was read for
    PresentationFrame m_lastFrame;
    // m_presentBuf's content version (++ at every copy / collected read), and the version and request
    // m_lastFrame was decoded from: the window presents ~60 times a second, the game draws 15-30 new
    // pictures, and decoding the same buffer again gives the same frame.
    uint64_t m_presentBufGen = 0;
    uint64_t m_lastFrameGen = ~0ull;
    GSPresentationRequest m_lastFrameRequest{};
    // Asynchronous display readback: a present reads the display pages into a PBO and hands over the
    // previous read (one VBlank later), so the worker never waits for the GPU to finish the frame.
    struct ReadSlot
    {
        GLuint pbo = 0;
        GLsync fence = nullptr;
        PageMask gpuPages;            // pages read from vtex into the PBO
        std::vector<uint8_t> data;    // CPU-valid pages copied when the read was issued
        GSPresentationRequest request{};
        bool pending = false;
    };
    std::array<ReadSlot, 2> m_readSlots;
    int m_readSlot = 0;
    PageMask m_presentChanged = PageMask().set(); // pages drawn or written since the last issued read
    PageMask m_presentIssued;                     // pages covered by the last issued read

    // Statistics (PS2X_GS_GPU_STATS=1).
    struct Stats
    {
        uint64_t prims = 0, draws = 0, cpuPrims = 0, wbPages = 0, loadPages = 0, upPages = 0, downPages = 0, presentPages = 0, copies = 0, rts = 0;
        uint64_t flushKey = 0, flushFeedback = 0, flushOverlap = 0, flushSingle = 0, singleTex = 0, copyPx = 0, barriers = 0;
        uint64_t presents = 0, reads = 0;
        uint64_t hiSrcDraws = 0, hiSrcCopies = 0, hiSrcMiss = 0, snapPrims = 0, hiresFrames = 0, readsDeferred = 0, vtexSkipped = 0, decHits = 0, decMisses = 0, decTexels = 0, decInval = 0, decResets = 0, decChecked = 0, decCheckBad = 0;
        uint64_t hdPrims = 0, hdRegions = 0, hdVerifies = 0, fastClears = 0, fastAlpha = 0, fastZeroAlpha = 0;
        double hdVerifyMs = 0.0;
        double presentMs = 0.0, waitMs = 0.0, flushMs = 0.0, issueMs = 0.0, copyMs = 0.0, throttleMs = 0.0;
        double wbMs = 0.0, upMs = 0.0, downMs = 0.0, drawMs = 0.0;
        double gpuMs = 0.0; // GPU time of this context's work (timer queries, PS2X_GS_GPU_STATS only)
        uint64_t gpuFrames = 0;
        uint64_t fsInvocations = 0, samplesPassed = 0; // pipeline statistics of the timed flushes (PS2X_GS_GPU_STATS only)
    } m_stats;
    double m_statsT0 = 0.0;
    std::map<std::string, uint64_t> m_cpuReasons;
    std::map<const char *, uint64_t> m_hiresMiss;
    bool m_hiresShown = false; // the last present went out at internal resolution
    std::array<GLsync, 2> m_frameFences{}; // one per present still on the GPU; bounds how far the worker runs ahead
    size_t m_frameFenceIdx = 0;
    std::vector<GLuint> m_gpuQueries; // GL_TIME_ELAPSED ring, one per flushDraw (PS2X_GS_GPU_STATS only)
    std::vector<GLuint> m_fsQueries, m_spQueries; // fragment shader invocations / samples passed, same slots (stats only)
    std::vector<uint64_t> m_spanTag;               // what each slot drew (see flushDraw), same slots (stats only)
    std::vector<GLuint> m_drawQueries;             // GL_TIME_ELAPSED of the draw part of each slot (stats only; m_gpuQueries then times the setup)
    std::vector<uint8_t> m_spanSplit;              // the slot's span was split (it reached the draws)
    struct FragAgg
    {
        uint64_t frags = 0, prims = 0, flushes = 0;
        double drawMs = 0.0, setupMs = 0.0;
    };
    std::map<uint64_t, FragAgg> m_fragByTag;
    size_t m_gpuQueryHead = 0, m_gpuQueryTail = 0;
    bool m_gpuQueryOpen = false;
    // PS2X_GS_SCALE=auto (autoScaleStep): the current one-second window and what was learnt so far.
    struct AutoScale
    {
        double t0 = 0.0, gpuMs = 0.0, throttleMs = 0.0;
        uint64_t frames0 = 0;
        int over = 0, under = 0;   // windows in a row too slow / with room for the next scale up
        bool settle = false;       // the window after a change rebuilds every target: not judged
        int fromScale = 0;         // the last change, and the GPU ms per frame before it
        double fromMs = 0.0;
        double changedT = 0.0, raisedT = -1.0e9;
        double holdS = 5.0;        // seconds at a scale before trying the next one up
        std::array<double, 5> ratio{}; // GPU cost of scale s+1 over scale s (learnt from changes)
    } m_auto;
    int m_autoMax = 1;
    double m_shotT0 = 0.0;

    // ------------------------------------------------------------ setup
    void initShadow(uint8_t *vram, uint32_t size)
    {
        flushDraw();
        m_shadow = vram;
        m_shadowSize = size;
        m_cpu.Initialize(vram, size);
        m_cpuValid.set();
        m_vtexValid.reset();
        m_presentChanged.set();
        for (auto &rt : m_rts)
        {
            rt->valid.reset();
            rt->dirty.reset();
        }
    }

    static GLuint compile(GLenum type, const std::string &src)
    {
        const GLuint s = gl.CreateShader(type);
        const char *p = src.c_str();
        gl.ShaderSource(s, 1, &p, nullptr);
        gl.CompileShader(s);
        GLint ok = 0;
        gl.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok)
        {
            char log[4096];
            gl.GetShaderInfoLog(s, sizeof(log), nullptr, log);
            std::fprintf(stderr, "[gsgpu] shader compile failed:\n%s\n", log);
            gl.DeleteShader(s);
            return 0;
        }
        return s;
    }
    static bool link(Program &prog, const std::string &vs, const std::string &fs, bool dualSource)
    {
        const GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
        if (!v || !f)
            return false;
        prog.id = gl.CreateProgram();
        gl.AttachShader(prog.id, v);
        gl.AttachShader(prog.id, f);
        (void)dualSource;
        gl.LinkProgram(prog.id);
        gl.DeleteShader(v);
        gl.DeleteShader(f);
        GLint ok = 0;
        gl.GetProgramiv(prog.id, GL_LINK_STATUS, &ok);
        if (!ok)
        {
            char log[4096];
            gl.GetProgramInfoLog(prog.id, sizeof(log), nullptr, log);
            std::fprintf(stderr, "[gsgpu] program link failed:\n%s\n", log);
            return false;
        }
        return true;
    }

    void initGl()
    {
        m_glOk = false;
        m_glFailReason = "GL setup did not finish";
        if (const char *e = std::getenv("PS2X_GS_GPU_FAIL_TEST"); e && *e == '1')
        {
            m_glFailReason = "PS2X_GS_GPU_FAIL_TEST=1";
            return;
        }
        GLFWwindow *window = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_host.mutex);
            window = g_host.window;
        }
        if (!window)
        {
            std::fprintf(stderr, "[gsgpu] no GL context: drawing on the CPU\n");
            m_glFailReason = "no GL 3.3 context";
            return;
        }
        glfwMakeContextCurrent(window);
        if (!gl.load())
        {
            std::fprintf(stderr, "[gsgpu] GL entry points missing: drawing on the CPU\n");
            m_glFailReason = "GL entry points missing";
            return;
        }
        std::fprintf(stderr, "[gsgpu] GL %s | %s | %s\n", reinterpret_cast<const char *>(gl.GetString(GL_VERSION)),
                     reinterpret_cast<const char *>(gl.GetString(GL_RENDERER)), reinterpret_cast<const char *>(gl.GetString(GL_VENDOR)));
        const std::string common = kCommonGlsl;
        // m_drawProgNd: no alpha test or DATE, so no discard and the driver can test depth before shading.
        if (!link(m_drawProg, kDrawVs, common + kDrawFs + kTexelGlsl + kDrawFsMain, true) ||
            !link(m_drawProgNd, kDrawVs, std::string(common).replace(common.find('\n') + 1, 0, "#define NO_DISCARD 1\n") + kDrawFs + kTexelGlsl + kDrawFsMain, true) ||
            !link(m_decProg, kQuadVs, common + "uniform usampler2D uClut;\nuniform uvec4 vS0;\nuniform ivec2 uOrg;\nout uint oWord;\n" + kTexelGlsl +
                  "void main() { oWord = decodeTexel(int(gl_FragCoord.x) - uOrg.x, int(gl_FragCoord.y) - uOrg.y); }\n", false) ||
            !link(m_loadColorProg, kQuadVs, common + kLoadFs, false) ||
            !link(m_loadDepthProg, kQuadVs, std::string(kCommonGlsl).replace(std::string(kCommonGlsl).find('\n') + 1, 0, "#define DEPTH 1\n") + kLoadFs, false) ||
            !link(m_wbProg, kQuadVs, common + kWritebackFs, false) ||
            !link(m_packProg, kQuadVs, kPackFs, false) ||
            !link(m_presentProg, kQuadVs, kPresentFs, false) ||
            !link(m_fillProg, kQuadVs, kFillFs, false))
        {
            std::fprintf(stderr, "[gsgpu] shader setup failed: drawing on the CPU\n");
            m_glFailReason = "shaders did not compile";
            return;
        }
        auto makeTex = [](GLuint &tex, GLenum internal, int w, int h, GLenum format, GLenum type, const void *data)
        {
            gl.GenTextures(1, &tex);
            gl.BindTexture(GL_TEXTURE_2D, tex);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            gl.TexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(internal), w, h, 0, format, type, data);
        };
        gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
        gl.PixelStorei(GL_PACK_ALIGNMENT, 4);
        GLint maxTex = 0;
        gl.GetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTex);
        m_scale = std::max(1, std::min(requestedScale(), static_cast<int>(maxTex) / 2048));
        m_autoMax = m_scale;
        // Steam Deck (Van Gogh APU, ~1/12 of an RTX 4070's fill rate): 3x is ~100 ms/frame in the neighbourhood
        // pan. Auto starts at 1x there and goes no higher than 2x (1280x896, already past the 800-line screen).
        const char *renderer = reinterpret_cast<const char *>(gl.GetString(GL_RENDERER));
        const bool deckGpu = renderer && (std::strstr(renderer, "vangogh") || std::strstr(renderer, "VanGogh") ||
                                          std::strstr(renderer, "Custom GPU 0405") || std::strstr(renderer, "Custom GPU 0932"));
        if (deckGpu && autoScale())
        {
            m_autoMax = std::min(m_autoMax, 2);
            m_scale = 1;
        }
        {
            const char *e = std::getenv("PS2X_GS_SCALE");
            std::fprintf(stderr, "[gs] GPU internal resolution %dx: %dx%d for a 640x448 picture (PS2X_GS_SCALE=%s)%s%s\n", m_scale,
                         640 * m_scale, 448 * m_scale, e && *e ? e : "unset", autoScale() ? ", auto: lowered while the GPU cannot keep up" : "",
                         deckGpu ? (autoScale() ? ", Steam Deck: up to 2x" : ", Steam Deck: 1x or auto recommended") : "");
        }
        makeTex(m_vtex, GL_R32UI, 2048, kPages, GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr);
        gl.GenFramebuffers(1, &m_vtexFbo);
        gl.BindFramebuffer(GL_FRAMEBUFFER, m_vtexFbo);
        gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_vtex, 0);
        bool fboOk = gl.CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        makeTex(m_decTex, GL_R32UI, kDecSize, kDecSize, GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr);
        gl.GenFramebuffers(1, &m_decFbo);
        gl.BindFramebuffer(GL_FRAMEBUFFER, m_decFbo);
        gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_decTex, 0);
        fboOk = fboOk && gl.CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        if (!fboOk)
        {
            std::fprintf(stderr, "[gsgpu] R32UI framebuffer unsupported: drawing on the CPU\n");
            m_glFailReason = "integer (R32UI) render targets unsupported";
            return;
        }
        const std::vector<uint16_t> lut = buildSwizzleTables();
        gl.PixelStorei(GL_UNPACK_ALIGNMENT, 2);
        makeTex(m_lutTex, GL_R16UI, 128, kLutRows, GL_RED_INTEGER, GL_UNSIGNED_SHORT, lut.data());
        makeTex(m_clutTex, GL_R16UI, 512, kClutRows, GL_RED_INTEGER, GL_UNSIGNED_SHORT, m_clutRing.data());
        gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);

        gl.GenBuffers(1, &m_stateBuf);
        gl.BindBuffer(GL_TEXTURE_BUFFER, m_stateBuf);
        gl.BufferData(GL_TEXTURE_BUFFER, kStateBytes, nullptr, GL_STREAM_DRAW);
        gl.GenTextures(1, &m_stateTex);
        gl.BindTexture(GL_TEXTURE_BUFFER, m_stateTex);
        gl.TexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32UI, m_stateBuf);

        gl.GenVertexArrays(1, &m_vao);
        gl.BindVertexArray(m_vao);
        gl.GenBuffers(1, &m_vbo);
        gl.BindBuffer(GL_ARRAY_BUFFER, m_vbo);
        gl.BufferData(GL_ARRAY_BUFFER, kVboBytes, nullptr, GL_STREAM_DRAW);
        gl.EnableVertexAttribArray(0);
        gl.VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(GpuVertex), reinterpret_cast<void *>(offsetof(GpuVertex, x)));
        gl.EnableVertexAttribArray(1);
        gl.VertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GpuVertex), reinterpret_cast<void *>(offsetof(GpuVertex, r)));
        gl.EnableVertexAttribArray(2);
        gl.VertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(GpuVertex), reinterpret_cast<void *>(offsetof(GpuVertex, s)));
        gl.EnableVertexAttribArray(3);
        gl.VertexAttribIPointer(3, 1, GL_UNSIGNED_INT, sizeof(GpuVertex), reinterpret_cast<void *>(offsetof(GpuVertex, state)));
        gl.EnableVertexAttribArray(4);
        gl.VertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, sizeof(GpuVertex), reinterpret_cast<void *>(offsetof(GpuVertex, cs0)));

        gl.GenVertexArrays(1, &m_quadVao);
        gl.BindVertexArray(m_quadVao);
        gl.GenBuffers(1, &m_quadVbo);
        gl.BindBuffer(GL_ARRAY_BUFFER, m_quadVbo);
        gl.BufferData(GL_ARRAY_BUFFER, 1 << 20, nullptr, GL_STREAM_DRAW);
        gl.EnableVertexAttribArray(0);
        gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, nullptr);
        gl.BindVertexArray(0);

        for (Program *p : {&m_drawProg, &m_drawProgNd, &m_loadColorProg, &m_loadDepthProg, &m_wbProg, &m_decProg})
        {
            gl.UseProgram(p->id);
            gl.Uniform1i(p->u("uVram"), 0);
            gl.Uniform1i(p->u("uLut"), 1);
        }
        {
            const char *e = std::getenv("PS2X_GS_GPU_BURN");
            const int burn = e && *e ? std::max(0, std::atoi(e)) : 0;
            for (Program *dp : {&m_drawProg, &m_drawProgNd})
            {
                gl.UseProgram(dp->id);
                gl.Uniform1i(dp->u("uClut"), 2);
                gl.Uniform1i(dp->u("uStates"), 3);
                gl.Uniform1i(dp->u("uFbCopy"), 4);
                gl.Uniform1i(dp->u("uHiTex"), 6);
                gl.Uniform1i(dp->u("uReplTex"), 7);
                gl.Uniform1i(dp->u("uDecTex"), 8);
                gl.Uniform1i(dp->u("uBurn"), burn);
            }
            if (burn > 0)
                std::fprintf(stderr, "[gsgpu] PS2X_GS_GPU_BURN=%d: extra shader work per pixel to act like a slower GPU\n", burn);
        }
        gl.UseProgram(m_decProg.id);
        gl.Uniform1i(m_decProg.u("uClut"), 2);
        gl.Uniform2f(m_decProg.u("uSize"), static_cast<float>(kDecSize), static_cast<float>(kDecSize));
        gl.UseProgram(m_presentProg.id);
        gl.Uniform1i(m_presentProg.u("uSrc1"), 0);
        gl.Uniform1i(m_presentProg.u("uSrc2"), 1);
        gl.UseProgram(m_wbProg.id);
        gl.Uniform1i(m_wbProg.u("uSrc"), 5);
        setScaleUniforms();
        gl.UseProgram(m_packProg.id);
        gl.Uniform1i(m_packProg.u("uWords"), 0);
        gl.UseProgram(0);
        gl.Enable(GL_DEPTH_CLAMP);
        const GLenum err = gl.GetError();
        if (err != GL_NO_ERROR)
            std::fprintf(stderr, "[gsgpu] GL error 0x%x during setup\n", err);
        m_vtexValid.reset();
        m_clutRowDirty.set();
        if (barrierRequested())
        {
            GLint major = 0, minor = 0;
            gl.GetIntegerv(GL_MAJOR_VERSION, &major);
            gl.GetIntegerv(GL_MINOR_VERSION, &minor);
            if (major * 10 + minor >= 45 || glfwExtensionSupported("GL_ARB_texture_barrier"))
                glTextureBarrierFn = reinterpret_cast<TextureBarrierFn>(glfwGetProcAddress("glTextureBarrier"));
            if (!glTextureBarrierFn && glfwExtensionSupported("GL_NV_texture_barrier"))
                glTextureBarrierFn = reinterpret_cast<TextureBarrierFn>(glfwGetProcAddress("glTextureBarrierNV"));
            m_barrier = glTextureBarrierFn != nullptr;
            std::fprintf(stderr, "[gsgpu] texture barrier %s\n", m_barrier ? "on" : "unavailable: copying the frame");
        }
        m_glOk = true;
        m_statsT0 = nowSeconds();
    }

    void shutdownGl()
    {
        if (m_glOk)
        {
            // Keep the CPU copy complete: later commands (if any) run on the CPU rasterizer.
            flushDraw();
            ensureCpu(PageMask().set());
            for (auto &rt : m_rts)
                deleteRt(*rt);
            m_rts.clear();
            for (auto &kv : m_fbos)
                gl.DeleteFramebuffers(1, &kv.second);
            m_fbos.clear();
            GLuint texs[] = {m_vtex, m_lutTex, m_clutTex, m_stateTex, m_copyTex, m_decTex};
            gl.DeleteTextures(6, texs);
            for (GLuint &t : m_hdTex)
                if (t)
                    gl.DeleteTextures(1, &t);
            m_hdTex.clear();
            GLuint fbos[] = {m_vtexFbo, m_copyFbo, m_decFbo};
            gl.DeleteFramebuffers(3, fbos);
            GLuint bufs[] = {m_vbo, m_quadVbo, m_stateBuf};
            gl.DeleteBuffers(3, bufs);
            if (m_packTex)
                gl.DeleteTextures(1, &m_packTex);
            if (m_packFbo)
                gl.DeleteFramebuffers(1, &m_packFbo);
            m_packTex = m_packFbo = 0;
            for (ReadSlot &s : m_readSlots)
            {
                if (s.fence)
                    gl.DeleteSync(s.fence);
                if (s.pbo)
                    gl.DeleteBuffers(1, &s.pbo);
                s = ReadSlot{};
            }
            GLuint vaos[] = {m_vao, m_quadVao};
            gl.DeleteVertexArrays(2, vaos);
            for (Program *p : {&m_drawProg, &m_drawProgNd, &m_loadColorProg, &m_loadDepthProg, &m_wbProg, &m_packProg, &m_presentProg, &m_fillProg, &m_decProg})
                gl.DeleteProgram(p->id);
            if (m_srcCopyTex)
                gl.DeleteTextures(1, &m_srcCopyTex);
            if (m_srcCopyFbo)
                gl.DeleteFramebuffers(1, &m_srcCopyFbo);
            m_srcCopyTex = m_srcCopyFbo = 0;
            m_srcCopyW = m_srcCopyH = 0;
            {
                std::lock_guard<std::mutex> lock(g_hires.mutex);
                for (HiresSlot &h : g_hires.slots)
                {
                    if (h.ready)
                        gl.DeleteSync(h.ready);
                    if (h.done)
                        gl.DeleteSync(h.done);
                    if (h.fbo)
                        gl.DeleteFramebuffers(1, &h.fbo);
                    if (h.tex)
                        gl.DeleteTextures(1, &h.tex);
                    h = HiresSlot{};
                }
                g_hires.latest = g_hires.inUse = -1;
                g_hires.publishedFor = ~0ull;
            }
            gl.Finish();
            m_copyTex = m_copyFbo = 0;
            m_copyW = m_copyH = 0;
            m_glOk = false;
        }
        glfwMakeContextCurrent(nullptr);
        m_stateValid = false;
    }

    // ------------------------------------------------------------ page coherence
    void decInvalidate(const PageMask &mask)
    {
        if ((mask & m_decPages).none())
            return;
        m_decPages.reset();
        for (auto it = m_dec.begin(); it != m_dec.end();)
        {
            auto &v = it->second;
            for (size_t i = 0; i < v.size();)
                if ((v[i].pages & mask).any())
                {
                    v[i] = std::move(v.back());
                    v.pop_back();
                    ++m_stats.decInval;
                }
                else
                    m_decPages |= v[i++].pages;
            it = v.empty() ? m_dec.erase(it) : std::next(it);
        }
    }
    void writeback(RenderTarget &rt, const PageMask &mask)
    {
        decInvalidate(mask);
        gl.BindFramebuffer(GL_FRAMEBUFFER, m_vtexFbo);
        gl.Viewport(0, 0, 2048, kPages);
        gl.Disable(GL_BLEND);
        gl.Disable(GL_DEPTH_TEST);
        gl.Disable(GL_SCISSOR_TEST);
        gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        gl.UseProgram(m_wbProg.id);
        gl.Uniform2f(m_wbProg.u("uSize"), 2048.0f, static_cast<float>(kPages));
        gl.Uniform1i(m_wbProg.u("uCls"), rt.cls);
        gl.Uniform1ui(m_wbProg.u("uBasePage"), rt.basePage);
        gl.Uniform1ui(m_wbProg.u("uPpr"), rt.ppr);
        gl.Uniform1i(m_wbProg.u("uDepth"), rt.depth ? 1 : 0);
        gl.Uniform1f(m_wbProg.u("uDepthScale"), static_cast<float>(depthScale(rt.cls)));
        gl.Uniform1f(m_wbProg.u("uMaxZ"), rt.cls == 5 ? 4294967040.0f : 65535.0f);
        bindCommonTextures();
        gl.ActiveTexture(GL_TEXTURE5);
        gl.BindTexture(GL_TEXTURE_2D, rt.tex);
        m_quads.clear();
        for (int p = 0; p < kPages;)
        {
            if (!mask.test(p))
            {
                ++p;
                continue;
            }
            int e = p;
            while (e < kPages && mask.test(e))
                ++e;
            pushQuad(0.0f, static_cast<float>(p), 2048.0f, static_cast<float>(e));
            m_stats.wbPages += static_cast<uint64_t>(e - p);
            p = e;
        }
        drawQuads();
        gl.ActiveTexture(GL_TEXTURE5);
        gl.BindTexture(GL_TEXTURE_2D, 0);
    }

    void loadPages(RenderTarget &rt, const PageMask &mask)
    {
        gl.BindFramebuffer(GL_FRAMEBUFFER, rt.fbo);
        gl.Viewport(0, 0, rt.width * m_scale, rt.height * m_scale);
        gl.Disable(GL_BLEND);
        gl.Disable(GL_SCISSOR_TEST);
        Program &prog = rt.depth ? m_loadDepthProg : m_loadColorProg;
        if (rt.depth)
        {
            gl.Enable(GL_DEPTH_TEST);
            gl.DepthFunc(GL_ALWAYS);
            gl.DepthMask(GL_TRUE);
        }
        else
        {
            gl.Disable(GL_DEPTH_TEST);
            gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        }
        gl.UseProgram(prog.id);
        gl.Uniform2f(prog.u("uSize"), static_cast<float>(rt.width), static_cast<float>(rt.height));
        static const uint32_t kReadPsm[8] = {GS_PSM_CT32, GS_PSM_CT16, GS_PSM_CT16S, GS_PSM_T8, GS_PSM_T4, GS_PSM_Z32, GS_PSM_Z16, GS_PSM_Z16S};
        gl.Uniform1ui(prog.u("uPsm"), kReadPsm[rt.cls]);
        gl.Uniform1ui(prog.u("uBase"), rt.basePage * 32u);
        gl.Uniform1ui(prog.u("uBw"), rt.bw);
        gl.Uniform1f(prog.u("uDepthScale"), static_cast<float>(depthScale(rt.cls)));
        bindCommonTextures();
        m_quads.clear();
        for (int p = 0; p < kPages; ++p)
        {
            if (!mask.test(p))
                continue;
            const uint32_t rel = (static_cast<uint32_t>(p) - rt.basePage) & 511u;
            const int px = static_cast<int>(rel % rt.ppr), py = static_cast<int>(rel / rt.ppr);
            const int x0 = px * rt.pageW, y0 = py * rt.pageH;
            if (y0 >= rt.height)
                continue;
            pushQuad(static_cast<float>(x0), static_cast<float>(y0), static_cast<float>(x0 + rt.pageW), static_cast<float>(y0 + rt.pageH));
            ++m_stats.loadPages;
        }
        drawQuads();
    }

    void pushQuad(float x0, float y0, float x1, float y1)
    {
        const float v[12] = {x0, y0, x1, y0, x0, y1, x1, y0, x1, y1, x0, y1};
        m_quads.insert(m_quads.end(), v, v + 12);
    }
    void drawQuads()
    {
        if (m_quads.empty())
            return;
        gl.BindVertexArray(m_quadVao);
        gl.BindBuffer(GL_ARRAY_BUFFER, m_quadVbo);
        const size_t bytes = m_quads.size() * sizeof(float);
        gl.BufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(std::max<size_t>(bytes, 1u << 16)), nullptr, GL_STREAM_DRAW);
        gl.BufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(bytes), m_quads.data());
        gl.DrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(m_quads.size() / 2));
        gl.BindVertexArray(0);
        m_quads.clear();
    }
    void bindCommonTextures()
    {
        gl.ActiveTexture(GL_TEXTURE0);
        gl.BindTexture(GL_TEXTURE_2D, m_vtex);
        gl.ActiveTexture(GL_TEXTURE1);
        gl.BindTexture(GL_TEXTURE_2D, m_lutTex);
    }

    void upload(const PageMask &mask)
    {
        decInvalidate(mask);
        gl.BindTexture(GL_TEXTURE_2D, m_vtex);
        for (int p = 0; p < kPages;)
        {
            if (!mask.test(p))
            {
                ++p;
                continue;
            }
            int e = p;
            while (e < kPages && mask.test(e))
                ++e;
            gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, p, 2048, e - p, GL_RED_INTEGER, GL_UNSIGNED_INT, m_shadow + static_cast<size_t>(p) * kPageBytes);
            m_stats.upPages += static_cast<uint64_t>(e - p);
            p = e;
        }
    }
    void download(const PageMask &mask)
    {
        gl.BindFramebuffer(GL_READ_FRAMEBUFFER, m_vtexFbo);
        gl.ReadBuffer(GL_COLOR_ATTACHMENT0);
        for (int p = 0; p < kPages;)
        {
            if (!mask.test(p))
            {
                ++p;
                continue;
            }
            int e = p;
            while (e < kPages && mask.test(e))
                ++e;
            gl.ReadPixels(0, p, 2048, e - p, GL_RED_INTEGER, GL_UNSIGNED_INT, m_shadow + static_cast<size_t>(p) * kPageBytes);
            m_stats.downPages += static_cast<uint64_t>(e - p);
            p = e;
        }
        gl.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    }

    // vtex holds the newest data of the pages afterwards.
    void ensureVtex(const PageMask &mask)
    {
        if (!m_glOk || !m_shadow)
            return;
        for (auto &rt : m_rts)
        {
            const PageMask d = rt->dirty & mask;
            if (d.none())
                continue;
            const double t0 = nowSeconds();
            writeback(*rt, d);
            m_stats.wbMs += (nowSeconds() - t0) * 1000.0;
            rt->dirty &= ~d;
            m_vtexValid |= d;
        }
        const PageMask need = mask & ~m_vtexValid;
        if (need.any())
        {
            const double t0 = nowSeconds();
            upload(need & m_cpuValid);
            m_stats.upMs += (nowSeconds() - t0) * 1000.0;
            m_vtexValid |= need;
        }
    }
    // The CPU copy holds the newest data of the pages afterwards.
    void ensureCpu(const PageMask &mask)
    {
        if (!m_glOk || !m_shadow)
            return;
        if ((mask & ~m_cpuValid).none())
            return;
        ensureVtex(mask);
        const PageMask need = mask & ~m_cpuValid;
        if (need.any())
        {
            const double t0 = nowSeconds();
            download(need);
            m_stats.downMs += (nowSeconds() - t0) * 1000.0;
            m_cpuValid |= need;
        }
    }
    // The CPU copy of the pages was just written.
    void hdOverwritten(const PageMask &mask, bool drawn)
    {
        const size_t n = m_hdRegions.size();
        bool changed = false;
        if (drawn)
            m_hdRegions.erase(std::remove_if(m_hdRegions.begin(), m_hdRegions.end(), [&](const HdRegion &r) { return (r.pages & mask).any(); }),
                              m_hdRegions.end());
        else
            for (HdRegion &r : m_hdRegions)
                if ((r.pages & mask).any() && !r.suspect)
                {
                    r.suspect = true;
                    changed = true;
                }
        if (changed || m_hdRegions.size() != n)
            ++m_hdGen;
    }
    void cpuWritten(const PageMask &mask)
    {
        if (!m_hdRegions.empty())
            hdOverwritten(mask, false);
        m_presentChanged |= mask;
        m_cpuValid |= mask;
        m_vtexValid &= ~mask;
        for (auto &rt : m_rts)
            rt->valid &= ~mask;
    }
    void ensureRtValid(RenderTarget &rt, const PageMask &mask)
    {
        const PageMask need = mask & ~rt.valid;
        if (need.none())
            return;
        ensureVtex(need);
        loadPages(rt, need);
        rt.valid |= need;
    }
    void markDrawn(RenderTarget &rt, const PageMask &mask)
    {
        if (!m_hdRegions.empty())
            hdOverwritten(mask, true);
        m_presentChanged |= mask;
        const PageMask fresh = mask & ~rt.dirty;
        if (fresh.none())
            return;
        rt.dirty |= fresh;
        rt.valid |= fresh;
        m_vtexValid &= ~fresh;
        m_cpuValid &= ~fresh;
        for (auto &other : m_rts)
            if (other.get() != &rt)
                other->valid &= ~fresh;
    }

    void setScaleUniforms()
    {
        for (Program *p : {&m_drawProg, &m_drawProgNd, &m_presentProg, &m_loadColorProg, &m_loadDepthProg, &m_wbProg})
        {
            gl.UseProgram(p->id);
            gl.Uniform1i(p->u("uScale"), m_scale);
        }
    }

    // PS2X_GS_SCALE=auto, once a second at a present: GPU time per game frame (the flushDraw queries) and
    // the time the worker waited on frames still on the GPU say whether the GPU keeps up with the frame
    // rate. Two slow windows in a row step the scale down; three with room for the next scale up (by the
    // cost ratio learnt from earlier changes) step it back up, held off longer each time a raise had to
    // be undone so a scene on the edge does not flip back and forth.
    void autoScaleStep()
    {
        AutoScale &a = m_auto;
        const double t = nowSeconds();
        const uint64_t frames = g_ps2xGameFrameCount.load(std::memory_order_relaxed);
        if (a.t0 == 0.0)
        {
            for (int s = 1; s < 4; ++s)
                a.ratio[s] = ((s + 1.0) * (s + 1.0) + 1.0) / (s * s + 1.0);
            a.t0 = t;
            a.frames0 = frames;
            a.gpuMs = a.throttleMs = 0.0;
            return;
        }
        const double secs = t - a.t0;
        if (secs < 1.0)
            return;
        const double n = static_cast<double>(frames - a.frames0);
        const double perFrame = n > 0 ? a.gpuMs / n : 0.0, throttle = a.throttleMs / secs;
        a.t0 = t;
        a.frames0 = frames;
        a.gpuMs = a.throttleMs = 0.0;
        if (a.settle)
        {
            a.settle = false;
            return;
        }
        static const double budget = []
        {
            const char *e = std::getenv("PS2X_60FPS");
            return e && *e && std::strcmp(e, "0") != 0 ? 1000.0 / 60.0 : 1000.0 / 30.0;
        }();
        const bool gpuBound = m_scale > 1 && (perFrame > 0.9 * budget || throttle > 300.0);
        // Loading, paused or a menu: nothing to judge. Under 10 frames because the GPU is that slow (a weak
        // iGPU at 3x) still steps down, or auto would never leave a scale too heavy to reach 10 FPS.
        if (n < 10.0 * secs && !(gpuBound && n > 0))
        {
            a.over = a.under = 0;
            return;
        }
        if (a.fromScale && perFrame > 0.0) // learn what the last change cost
        {
            const int lo = std::min(a.fromScale, m_scale);
            const double r = a.fromScale < m_scale ? perFrame / a.fromMs : a.fromMs / perFrame;
            if (std::abs(a.fromScale - m_scale) == 1 && std::isfinite(r))
                a.ratio[lo] = std::clamp(r, 1.05, 5.0);
            a.fromScale = 0;
        }
        if (gpuBound)
        {
            a.under = 0;
            if (++a.over >= 2)
            {
                if (t - a.raisedT < 10.0)
                    a.holdS = std::min(a.holdS * 2.0, 60.0);
                rescale(m_scale - 1, perFrame);
            }
        }
        else if (m_scale < m_autoMax && perFrame * a.ratio[m_scale] < 0.7 * budget && throttle < 100.0)
        {
            a.over = 0;
            if (++a.under >= 3 && t - a.changedT >= a.holdS)
            {
                a.raisedT = t;
                rescale(m_scale + 1, perFrame);
            }
        }
        else
            a.over = a.under = 0;
    }

    // Worker: switch the internal resolution. Every render target goes (its pages written back to GS
    // memory first) and is rebuilt at the new size as the game draws; one frame of extra work.
    void rescale(int scale, double perFrame)
    {
        flushDraw();
        ensureVtex(PageMask().set());
        for (auto &rt : m_rts)
            deleteRt(*rt);
        m_rts.clear();
        m_stateValid = false;
        std::fprintf(stderr, "[gs] internal resolution %dx -> %dx (GPU %.1f ms/frame)\n", m_scale, scale, perFrame);
        m_auto.fromScale = m_scale;
        m_auto.fromMs = perFrame;
        m_scale = scale;
        setScaleUniforms();
        gl.UseProgram(0);
        m_auto.changedT = nowSeconds();
        m_auto.over = m_auto.under = 0;
        m_auto.settle = true;
    }

    // ------------------------------------------------------------ render targets
    void deleteRt(RenderTarget &rt)
    {
        for (auto it = m_fbos.begin(); it != m_fbos.end();)
        {
            if (it->first.first == rt.tex || it->first.second == rt.tex)
            {
                gl.DeleteFramebuffers(1, &it->second);
                it = m_fbos.erase(it);
            }
            else
                ++it;
        }
        if (rt.fbo)
            gl.DeleteFramebuffers(1, &rt.fbo);
        if (rt.tex)
            gl.DeleteTextures(1, &rt.tex);
        rt.fbo = rt.tex = 0;
    }
    void retireRt(size_t index)
    {
        RenderTarget &rt = *m_rts[index];
        ensureVtex(rt.dirty);
        deleteRt(rt);
        m_rts.erase(m_rts.begin() + static_cast<std::ptrdiff_t>(index));
    }
    RenderTarget *findRt(uint32_t basePage, uint32_t bw, int cls, bool depth, int needH)
    {
        bw = std::max<uint32_t>(bw, 1u);
        for (size_t i = 0; i < m_rts.size(); ++i)
        {
            RenderTarget &rt = *m_rts[i];
            if (rt.basePage != basePage || rt.bw != bw || rt.cls != cls || rt.depth != depth)
                continue;
            if (rt.height >= needH)
            {
                rt.lastUse = m_useClock;
                return &rt;
            }
            retireRt(i); // too small: rebuilt below from vtex
            break;
        }
        if (m_rts.size() >= 48)
        {
            size_t lru = SIZE_MAX;
            for (size_t i = 0; i < m_rts.size(); ++i)
                if (m_rts[i]->lastUse != m_useClock && (lru == SIZE_MAX || m_rts[i]->lastUse < m_rts[lru]->lastUse))
                    lru = i;
            if (lru != SIZE_MAX)
                retireRt(lru);
        }
        auto rt = std::make_unique<RenderTarget>();
        rt->basePage = basePage & 511u;
        rt->bw = bw;
        rt->cls = cls;
        rt->depth = depth;
        rt->pageW = kPageW[cls];
        rt->pageH = kPageH[cls];
        rt->ppr = std::max<uint32_t>(1u, (bw * 64u) / static_cast<uint32_t>(rt->pageW));
        rt->width = static_cast<int>(rt->ppr) * rt->pageW;
        rt->height = (needH + rt->pageH - 1) / rt->pageH * rt->pageH;
        rt->lastUse = m_useClock;
        gl.GenTextures(1, &rt->tex);
        gl.BindTexture(GL_TEXTURE_2D, rt->tex);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        const int tw = rt->width * m_scale, th = rt->height * m_scale; // the GS size times the internal resolution
        if (depth)
            gl.TexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, tw, th, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        else
            gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        gl.GenFramebuffers(1, &rt->fbo);
        gl.BindFramebuffer(GL_FRAMEBUFFER, rt->fbo);
        if (depth)
        {
            gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, rt->tex, 0);
            gl.DrawBuffer(GL_NONE);
            gl.ReadBuffer(GL_NONE);
        }
        else
            gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt->tex, 0);
        ++m_stats.rts;
        m_rts.push_back(std::move(rt));
        return m_rts.back().get();
    }
    GLuint drawFbo(RenderTarget *color, RenderTarget *depth)
    {
        if (!depth)
            return color->fbo;
        if (!color)
            return depth->fbo;
        const auto key = std::make_pair(color->tex, depth->tex);
        auto it = m_fbos.find(key);
        if (it != m_fbos.end())
            return it->second;
        GLuint fbo = 0;
        gl.GenFramebuffers(1, &fbo);
        gl.BindFramebuffer(GL_FRAMEBUFFER, fbo);
        gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color->tex, 0);
        gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth->tex, 0);
        m_fbos[key] = fbo;
        return fbo;
    }

    // ------------------------------------------------------------ draw state
    void unsupported(const char *reason)
    {
        m_cpuOnly = true;
        m_cpuReason = reason;
    }

    void setState(const GSDrawState &s)
    {
        if (m_stateValid && std::memcmp(&s, &m_state, sizeof(GSDrawState)) == 0)
            return;
        m_state = s;
        m_stateValid = true;
        m_recIndex = UINT32_MAX;
        m_cpuOnly = false;
        m_cpuReason = nullptr;
        m_skip = false;
        m_srcCand = false;
        m_hdLookGen = 0;
        m_copyStable = false;
        const GSContext &ctx = s.context;
        const GSPrimReg &prim = s.prim;
        DrawKey k;
        std::memset(&k, 0, sizeof(k));
        m_ofx = ctx.xyoffset.ofx >> 4;
        m_ofy = ctx.xyoffset.ofy >> 4;
        m_tme = prim.tme;

        if (prim.type != GS_PRIM_TRIANGLE && prim.type != GS_PRIM_TRISTRIP && prim.type != GS_PRIM_TRIFAN && prim.type != GS_PRIM_SPRITE)
            unsupported("points/lines");
        if (prim.fge)
            unsupported("fog");
        if (prim.abe && (s.pabe || (s.colclamp & 1u) == 0u))
            unsupported("pabe/colclamp");
        if ((s.scanmsk & 3u) >= 2u)
            unsupported("scanmsk");
        const uint32_t fpsm = ctx.frame.psm;
        if (!isColorTargetPsm(fpsm))
            unsupported("frame psm");

        // Frame writes.
        const uint32_t fbmsk = ctx.frame.fbmsk;
        const bool ct24 = fpsm == GS_PSM_CT24;
        uint8_t colorMask = 0;
        for (int ch = 0; ch < 3; ++ch)
        {
            const uint32_t m = (fbmsk >> (ch * 8)) & 0xFFu;
            if (m == 0u)
                colorMask |= static_cast<uint8_t>(1u << ch);
            else if (m != 0xFFu)
                unsupported("partial rgb fbmsk");
        }
        uint32_t alphaKeep = (fbmsk >> 24) & 0xFFu;
        if (ct24)
            alphaKeep = 0xFFu;
        if (alphaKeep != 0xFFu)
            colorMask |= 8u;
        k.alphaKeep = static_cast<uint8_t>(alphaKeep == 0xFFu ? 0u : alphaKeep);
        k.ct24 = ct24;

        // Tests.
        const uint64_t test = ctx.test;
        const uint32_t ztst = static_cast<uint32_t>((test >> 17) & 3u);
        if (ztst == 0u)
            m_skip = true;
        const bool zmsk = ctx.zbuf.zmask;
        k.depthOn = (ztst >= 2u || (ztst == 1u && !zmsk)) ? 1 : 0;
        k.depthWrite = !zmsk ? 1 : 0;
        k.depthFunc = static_cast<uint8_t>(ztst);
        if (k.depthOn)
        {
            if (!isDepthTargetPsm(ctx.zbuf.psm))
                unsupported("zbuf psm");
            else if (ctx.zbuf.psm == GS_PSM_Z24 && k.depthWrite)
                unsupported("Z24 writes");
            k.zbp = ctx.zbuf.zbp & 511u;
            k.zcls = static_cast<uint8_t>(psmClass(ctx.zbuf.psm));
        }
        bool ate = (test & 1u) != 0u;
        const uint32_t atst = static_cast<uint32_t>((test >> 1) & 7u);
        if (ate && atst == 1u)
            ate = false;
        k.ate = ate;
        k.afail = ate ? static_cast<uint8_t>((test >> 12) & 3u) : 0;
        if (k.afail == 3u && fpsm != GS_PSM_CT32)
            k.afail = 1; // RGB_ONLY is FB_ONLY for 24/16-bit frames
        // Without a depth write, FB_ONLY (and RGB_ONLY with alpha not written) writes the same channels
        // whether the test passes or fails, so the test changes nothing: one pass, no discard.
        if (ate && !(k.depthOn && k.depthWrite) && (k.afail == 1u || (k.afail == 3u && !(colorMask & 8u))))
        {
            ate = false;
            k.ate = 0;
            k.afail = 0;
        }
        const bool date = ((test >> 14) & 1u) != 0u && !ct24;
        k.date = date ? (((test >> 15) & 1u) ? 1 : 2) : 0;

        // Blending: Cv = (A - B) * C / 128 + D, A/B/D in {Cs, Cd, 0}, C in {As, Ad, FIX}.
        uint32_t csel = 0;
        bool readsDst = false; // RGB or Ad from the frame
        uint32_t fix = static_cast<uint32_t>((ctx.alpha >> 32) & 0xFFu);
        k.outMode = 0;
        if (prim.abe)
        {
            const int A = static_cast<int>(ctx.alpha & 3u), B = static_cast<int>((ctx.alpha >> 2) & 3u);
            const int C = static_cast<int>((ctx.alpha >> 4) & 3u), D = static_cast<int>((ctx.alpha >> 6) & 3u);
            csel = static_cast<uint32_t>(std::min(C, 2));
            k.selA = static_cast<uint8_t>(std::min(A, 2));
            k.selB = static_cast<uint8_t>(std::min(B, 2));
            k.selD = static_cast<uint8_t>(std::min(D, 2));
            // Coefficients of Cs and Cd: (factor-of-f, constant).
            auto coef = [&](int target, int &fPart, int &cPart)
            {
                fPart = (k.selA == target ? 1 : 0) - (k.selB == target ? 1 : 0);
                cPart = k.selD == target ? 1 : 0;
            };
            int sf, sc, df, dc;
            coef(0, sf, sc);
            coef(1, df, dc);
            const bool usesCd = df != 0 || dc != 0;
            const bool bigFactor = csel == 1u || (csel == 2u && fix > 128u);
            auto factor = [](int f, int c, GLenum &out) -> bool
            {
                if (f == 0 && c == 0) out = GL_ZERO;
                else if (f == 0 && c == 1) out = GL_ONE;
                else if (f == 1 && c == 0) out = GL_SRC1_ALPHA;
                else if (f == -1 && c == 1) out = GL_ONE_MINUS_SRC1_ALPHA;
                else return false;
                return true;
            };
            GLenum fs = GL_ZERO, fd = GL_ZERO;
            if (!usesCd)
                k.outMode = 2; // exact, in the shader
            else if (sf == 1 && sc == 0 && df == 0 && dc == 1 && bigFactor)
                k.outMode = 3; // Cs * C + Cd with C up to 2: exact source term, ONE/ONE blend
            else if (bigFactor)
                k.outMode = 2;
            else if (factor(sf, sc, fs) && factor(df, dc, fd))
            {
                k.outMode = 1;
                k.blendEq = GL_FUNC_ADD;
            }
            else if (sf == -1 && sc == 0 && factor(df, dc, fd))
            {
                k.outMode = 1;
                k.blendEq = GL_FUNC_REVERSE_SUBTRACT;
                fs = GL_SRC1_ALPHA;
            }
            else if (df == -1 && dc == 0 && factor(sf, sc, fs))
            {
                k.outMode = 1;
                k.blendEq = GL_FUNC_SUBTRACT;
                fd = GL_SRC1_ALPHA;
            }
            else
                k.outMode = 2;
            k.blendSrc = static_cast<uint16_t>(fs);
            k.blendDst = static_cast<uint16_t>(fd);
            if (k.outMode == 2 && usesCd)
                k.copy = 1, readsDst = true;
            if (csel == 1u && !ct24)
                k.copy = 1, readsDst = true;
        }
        if (k.date || k.alphaKeep)
            k.copy = 1;
        m_copyStable = k.copy && !readsDst && (!k.date || (k.alphaKeep & 0x80u));
        k.colorMask = colorMask;
        k.colorOn = (colorMask != 0 || k.copy) ? 1 : 0;
        if (!k.colorOn && !k.depthOn)
            m_skip = true;
        k.fbp = ctx.frame.fbp & 511u;
        k.fbw = std::max<uint32_t>(ctx.frame.fbw, 1u);
        k.fcls = static_cast<uint8_t>(psmClass(fpsm));
        const int sy1 = std::min<int>(ctx.scissor.y1, 2047);
        k.needH = static_cast<uint16_t>(std::clamp((sy1 + 64) / 64 * 64, 512, 2048));
        if (!k.colorOn && k.depthOn)
            k.fcls = 0;
        m_key = k;

        // State record.
        StateRecord r{};
        if (prim.tme)
        {
            const GSTex0Reg &t = ctx.tex0;
            r.w[0] = (t.tbp0 & 0x3FFFu) | ((t.tbw & 63u) << 14) | ((t.psm & 63u) << 20) | (1u << 26) |
                     ((s.linearFilter ? 1u : 0u) << 27) | ((t.tcc & 1u) << 28) | ((t.tfx & 3u) << 29);
            r.w[1] = (t.tw & 15u) | ((t.th & 15u) << 4) | (static_cast<uint32_t>(ctx.clamp & 3u) << 8) |
                     (static_cast<uint32_t>((ctx.clamp >> 2) & 3u) << 10) | ((t.cpsm & 63u) << 20) | ((t.csa & 31u) << 26) |
                     ((t.csm & 1u) << 31);
            const uint32_t minU = static_cast<uint32_t>((ctx.clamp >> 4) & 0x3FFu), maxU = static_cast<uint32_t>((ctx.clamp >> 14) & 0x3FFu);
            const uint32_t minV = static_cast<uint32_t>((ctx.clamp >> 24) & 0x3FFu), maxV = static_cast<uint32_t>((ctx.clamp >> 34) & 0x3FFu);
            r.w[6] = minU | (maxU << 16);
            r.w[7] = minV | (maxV << 16);
            const uint32_t psm = t.psm;
            const bool known = psm == GS_PSM_CT32 || psm == GS_PSM_CT24 || psm == GS_PSM_CT16 || psm == GS_PSM_CT16S ||
                               psm == GS_PSM_T8 || psm == GS_PSM_T8H || psm == GS_PSM_T4 || psm == GS_PSM_T4HL ||
                               psm == GS_PSM_T4HH || isDepthTargetPsm(psm);
            if (!known)
                unsupported("texture psm");
            int w = s.textureWidth, h = s.textureHeight;
            if ((ctx.clamp & 3u) >= 2u)
                w = std::max<int>(w, static_cast<int>(std::max(minU | maxU, maxU)) + 1);
            if (((ctx.clamp >> 2) & 3u) >= 2u)
                h = std::max<int>(h, static_cast<int>(std::max(minV | maxV, maxV)) + 1);
            m_texMask = rectPages(psm, t.tbp0, std::max<uint32_t>(t.tbw, 1u), 0, 0, w - 1, h - 1);
            // A colour target with this base page, width and layout can be read at the internal resolution
            // (texel (u, v) is its pixel (u, v)); region repeat keeps the GS-resolution path.
            // PS2X_GS_HISRC=0 (diagnostic): always read targets through the GS-resolution copy.
            static const bool hiSrcOff = [] { const char *e = std::getenv("PS2X_GS_HISRC"); return e && *e == '0'; }();
            m_srcCand = m_scale > 1 && !hiSrcOff && (t.tbp0 & 31u) == 0u && isColorTargetPsm(psm) && (ctx.clamp & 3u) != 3u &&
                        ((ctx.clamp >> 2) & 3u) != 3u;
            if (m_srcCand)
            {
                m_srcBase = static_cast<uint16_t>((t.tbp0 / 32u) & 511u);
                m_srcBw = static_cast<uint16_t>(std::max<uint32_t>(t.tbw, 1u));
                m_srcCls = static_cast<uint8_t>(psmClass(psm));
                m_srcW = static_cast<uint16_t>(w);
                m_srcH = static_cast<uint16_t>(h);
            }
        }
        else
            m_texMask.reset();
        r.w[2] = (k.ate ? 1u : 0u) | (atst << 1) | (static_cast<uint32_t>((test >> 4) & 0xFFu) << 4) |
                 ((((ctx.fba & 1u) != 0u && !ct24) ? 1u : 0u) << 12) | (fix << 13) | (csel << 21);
        r.w[3] = s.texa.ta0 | (static_cast<uint32_t>(s.texa.ta1) << 8) | ((s.texa.aem ? 1u : 0u) << 16);
        r.w[4] = ctx.scissor.x0 | (static_cast<uint32_t>(ctx.scissor.x1) << 16);
        r.w[5] = ctx.scissor.y0 | (static_cast<uint32_t>(ctx.scissor.y1) << 16);
        m_rec = r;
    }

    // ------------------------------------------------------------ primitives
    void cpuDraw(const Prim &p)
    {
        flushDraw();
        GSPrimitiveBatch b;
        b.state = m_state;
        b.vertexCount = p.count;
        b.vertices = p.v;
        const GSContext &ctx = m_state.context;
        PageMask m;
        if (m_glOk)
        {
            const uint32_t fbw = std::max<uint32_t>(ctx.frame.fbw, 1u);
            m |= rectPages(ctx.frame.psm, ctx.frame.fbp * 32u, fbw, ctx.scissor.x0, ctx.scissor.y0, ctx.scissor.x1, ctx.scissor.y1);
            m |= rectPages(ctx.zbuf.psm, ctx.zbuf.zbp * 32u, fbw, ctx.scissor.x0, ctx.scissor.y0, ctx.scissor.x1, ctx.scissor.y1);
            ensureCpu(m | m_texMask);
        }
        m_cpu.Submit(b);
        if (m_glOk)
            cpuWritten(m);
        ++m_stats.cpuPrims;
        if (m_cpuReason)
            ++m_cpuReasons[m_cpuReason];
    }

    void addPrim(const Prim &p)
    {
        ++m_stats.prims;
        if (!m_stateValid)
            return;
        if (!m_glOk || m_cpuOnly || !m_shadow)
        {
            if (m_shadow)
                cpuDraw(p);
            return;
        }
        if (m_skip)
            return;
        const GSPrimReg &prim = m_state.prim;
        const GSContext &ctx = m_state.context;
        GpuVertex v[6] = {};
        int nv = 0;
        bool snap = false; // 1:1 texture mapping (see vSnap)
        bool clampTex = false; // keep s/t inside the range the 1x pixel centres sample (see vClampOn)
        int bx0, by0, bx1, by1;
        const float texW = static_cast<float>(m_state.textureWidth), texH = static_cast<float>(m_state.textureHeight);
        auto stq = [&](const GSVertex &s, GpuVertex &o)
        {
            if (!prim.tme)
            {
                o.s = o.t = 0.0f;
                o.q = 1.0f;
            }
            else if (prim.fst)
            {
                o.s = static_cast<float>(s.u) / 16.0f / texW;
                o.t = static_cast<float>(s.v) / 16.0f / texH;
                o.q = 1.0f;
            }
            else
            {
                o.s = s.s;
                o.t = s.t;
                o.q = s.q;
            }
        };
        if (prim.type == GS_PRIM_SPRITE)
        {
            if (p.count < 2)
                return;
            const GSVertex &v0 = p.v[0], &v1 = p.v[1];
            int x0 = static_cast<int>(v0.x) - m_ofx, y0 = static_cast<int>(v0.y) - m_ofy;
            int x1 = static_cast<int>(v1.x) - m_ofx, y1 = static_cast<int>(v1.y) - m_ofy;
            if (x0 > x1)
                std::swap(x0, x1);
            if (y0 > y1)
                std::swap(y0, y1);
            const int xe = x0 + std::max(1, x1 - x0), ye = y0 + std::max(1, y1 - y0);
            bx0 = x0;
            by0 = y0;
            bx1 = xe - 1;
            by1 = ye - 1;
            float u0 = 0, t0 = 0, u1 = 0, t1 = 0;
            if (prim.tme)
            {
                if (prim.fst)
                {
                    u0 = static_cast<float>(v0.u >> 4) / texW;
                    t0 = static_cast<float>(v0.v >> 4) / texH;
                    u1 = static_cast<float>(v1.u >> 4) / texW;
                    t1 = static_cast<float>(v1.v >> 4) / texH;
                }
                else
                {
                    const float q0 = std::fabs(v0.q) > 1.0e-8f ? v0.q : 1.0f, q1 = std::fabs(v1.q) > 1.0e-8f ? v1.q : 1.0f;
                    u0 = v0.s / q0;
                    t0 = v0.t / q0;
                    u1 = v1.s / q1;
                    t1 = v1.t / q1;
                }
            }
            const float z = static_cast<float>(v1.z);
            auto corner = [&](int x, int y, float s, float t, GpuVertex &o)
            {
                o.x = static_cast<float>(x);
                o.y = static_cast<float>(y);
                o.z = z;
                o.r = v1.r;
                o.g = v1.g;
                o.b = v1.b;
                o.a = v1.a;
                o.s = s;
                o.t = t;
                o.q = 1.0f;
            };
            if (m_scale > 1 && prim.tme)
            {
                const float du = (u1 - u0) * texW / static_cast<float>(xe - x0), dv = (t1 - t0) * texH / static_cast<float>(ye - y0);
                snap = std::fabs(std::fabs(du) - 1.0f) < 0.01f && std::fabs(std::fabs(dv) - 1.0f) < 0.01f;
            }
            float cs0 = 0, ct0 = 0, cs1 = 0, ct1 = 0;
            if (m_scale > 1 && prim.tme && !snap && m_state.linearFilter)
            {
                // At 1x the outermost pixel centres sample half a pixel's step inside the sprite's texel rect;
                // the extra sub-pixels at the internal resolution would otherwise filter in texels past it
                // (the neighbouring glyph of a font atlas: thin dark lines between letters).
                const float hs = 0.5f * (u1 - u0) / static_cast<float>(xe - x0), ht = 0.5f * (t1 - t0) / static_cast<float>(ye - y0);
                cs0 = std::min(u0 + hs, u1 - hs);
                cs1 = std::max(u0 + hs, u1 - hs);
                ct0 = std::min(t0 + ht, t1 - ht);
                ct1 = std::max(t0 + ht, t1 - ht);
                clampTex = true;
            }
            corner(x0, y0, u0, t0, v[0]);
            corner(xe, y0, u1, t0, v[1]);
            corner(x0, ye, u0, t1, v[2]);
            v[3] = v[1];
            corner(xe, ye, u1, t1, v[4]);
            v[5] = v[2];
            nv = 6;
            for (int i = 0; i < nv; ++i)
            {
                v[i].cs0 = cs0;
                v[i].ct0 = ct0;
                v[i].cs1 = cs1;
                v[i].ct1 = ct1;
            }
        }
        else
        {
            if (p.count < 3)
                return;
            float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
            const GSVertex &flat = p.v[2];
            for (int i = 0; i < 3; ++i)
            {
                const GSVertex &s = p.v[i];
                GpuVertex &o = v[i];
                o.x = s.x - static_cast<float>(m_ofx);
                o.y = s.y - static_cast<float>(m_ofy);
                o.z = static_cast<float>(s.z);
                const GSVertex &c = prim.iip ? s : flat;
                o.r = c.r;
                o.g = c.g;
                o.b = c.b;
                o.a = c.a;
                stq(s, o);
                minX = std::min(minX, o.x);
                maxX = std::max(maxX, o.x);
                minY = std::min(minY, o.y);
                maxY = std::max(maxY, o.y);
            }
            bx0 = static_cast<int>(std::floor(minX));
            by0 = static_cast<int>(std::floor(minY));
            bx1 = static_cast<int>(std::ceil(maxX));
            by1 = static_cast<int>(std::ceil(maxY));
            nv = 3;
            if (m_scale > 1 && prim.tme && v[0].q == v[1].q && v[1].q == v[2].q && v[0].q != 0.0f)
            {
                // Affine texture mapping of one texel per pixel (screen-aligned UI quads drawn as triangles).
                const float ex1 = v[1].x - v[0].x, ey1 = v[1].y - v[0].y, ex2 = v[2].x - v[0].x, ey2 = v[2].y - v[0].y;
                const float det = ex1 * ey2 - ex2 * ey1;
                if (std::fabs(det) > 1.0f)
                {
                    const float k = texW / v[0].q, l = texH / v[0].q;
                    const float du1 = (v[1].s - v[0].s) * k, du2 = (v[2].s - v[0].s) * k;
                    const float dv1 = (v[1].t - v[0].t) * l, dv2 = (v[2].t - v[0].t) * l;
                    const float dudx = (du1 * ey2 - du2 * ey1) / det, dudy = (du2 * ex1 - du1 * ex2) / det;
                    const float dvdx = (dv1 * ey2 - dv2 * ey1) / det, dvdy = (dv2 * ex1 - dv1 * ex2) / det;
                    snap = std::fabs(std::fabs(dudx) - 1.0f) < 0.01f && std::fabs(std::fabs(dvdy) - 1.0f) < 0.01f &&
                           std::fabs(dudy) < 0.01f && std::fabs(dvdx) < 0.01f;
                    // Axis-aligned mapping (font glyph quads, magnified): clamp s/t to what the 1x pixel centres
                    // inside the bounds sample, like the sprite path, so the extra sub-pixels don't filter in the
                    // atlas column past the glyph's cell.
                    if (!snap && m_state.linearFilter && std::fabs(dudy) * (maxY - minY) < 0.05f && std::fabs(dvdx) * (maxX - minX) < 0.05f)
                    {
                        const float xa = std::ceil(minX - 0.5f) + 0.5f, xb = std::ceil(maxX - 0.5f) - 0.5f;
                        const float ya = std::ceil(minY - 0.5f) + 0.5f, yb = std::ceil(maxY - 0.5f) - 0.5f;
                        if (xa <= xb && ya <= yb)
                        {
                            const float sa = v[0].s + (xa - v[0].x) * dudx / k, sb = v[0].s + (xb - v[0].x) * dudx / k;
                            const float ta = v[0].t + (ya - v[0].y) * dvdy / l, tb = v[0].t + (yb - v[0].y) * dvdy / l;
                            for (int i = 0; i < 3; ++i)
                            {
                                v[i].cs0 = std::min(sa, sb);
                                v[i].cs1 = std::max(sa, sb);
                                v[i].ct0 = std::min(ta, tb);
                                v[i].ct1 = std::max(ta, tb);
                            }
                            clampTex = true;
                        }
                    }
                }
            }
        }
        const RenderTargetLimits lim = targetLimits();
        bx0 = std::max<int>(bx0, ctx.scissor.x0);
        by0 = std::max<int>(by0, ctx.scissor.y0);
        bx1 = std::min<int>({bx1, ctx.scissor.x1, lim.w - 1});
        by1 = std::min<int>({by1, ctx.scissor.y1, lim.h - 1});
        if (bx1 < bx0 || by1 < by0 || bx1 < 0 || by1 < 0)
            return;
        bx0 = std::max(bx0, 0);
        by0 = std::max(by0, 0);
        PageMask colorPages, depthPages;
        if (m_key.colorOn)
            colorPages = rectPages(m_state.context.frame.psm, m_key.fbp * 32u, m_key.fbw, bx0, by0, bx1, by1);
        if (m_key.depthOn)
            depthPages = rectPages(m_state.context.zbuf.psm, m_key.zbp * 32u, m_key.fbw, bx0, by0, bx1, by1);

        if (m_srcCand)
        {
            const bool on = hiSourceUsable(v, nv);
            m_key.srcOn = on ? 1 : 0;
            m_key.srcCls = on ? m_srcCls : 0;
            m_key.srcBase = on ? m_srcBase : 0;
            m_key.srcBw = on ? m_srcBw : 0;
        }
        m_key.hdId = 0;
        m_key.hdWrap = 0;
        if (m_tme && !m_hdRegions.empty())
        {
            if (m_hdLookGen != m_hdGen)
                hdLookup();
            m_key.hdId = m_hdLook;
            m_key.hdWrap = m_hdLookWrap;
            if (m_hdLook)
            {
                m_key.srcOn = 0;
                m_key.srcCls = 0;
                m_key.srcBase = 0;
                m_key.srcBw = 0;
                snap = false;
                ++m_stats.hdPrims;
            }
        }
        bool single = m_key.singlePrim != 0;
        if (m_tme && (m_texMask & (colorPages | depthPages | m_batchColor | m_batchDepth)).any())
        {
            if (!m_verts.empty())
                ++m_stats.flushFeedback;
            flushDraw(); // the texture reads what this batch draws: one primitive at a time
            single = true;
            ++m_stats.singleTex;
            static const bool dbg = std::getenv("PS2X_GS_GPU_DEBUG_FEEDBACK") != nullptr;
            if (dbg && m_stats.singleTex <= 4)
                std::fprintf(stderr, "[gsgpu] feedback prim type %u fb %u/%u/%u z %u tex %u/%u/%u %ux%u rect %d,%d-%d,%d out %u copy %u single %u\n",
                             prim.type, ctx.frame.fbp, ctx.frame.fbw, ctx.frame.psm, ctx.zbuf.zbp, ctx.tex0.tbp0, ctx.tex0.tbw, ctx.tex0.psm,
                             m_state.textureWidth, m_state.textureHeight, bx0, by0, bx1, by1, m_key.outMode, m_key.copy, m_key.singlePrim);
        }
        if (!m_verts.empty() && (std::memcmp(&m_key, &m_batchKey, sizeof(DrawKey)) != 0 ||
                                 (m_verts.size() + 6) * sizeof(GpuVertex) > kVboBytes / 2 ||
                                 (m_records.size() + 1) * sizeof(StateRecord) > kStateBytes / 2))
        {
            ++m_stats.flushKey;
            flushDraw();
        }
        const int tx0 = std::min(bx0 / kCopyTile, kCopyGrid - 1), ty0 = std::min(by0 / kCopyTile, kCopyGrid - 1);
        const int tx1 = std::min(bx1 / kCopyTile, kCopyGrid - 1), ty1 = std::min(by1 / kCopyTile, kCopyGrid - 1);
        CopyPoly poly{};
        const bool polyArea = m_key.copy && copyPoly(v, nv, poly);
        if (m_key.copy && !m_copyStable && m_copyTilesUsed && !m_verts.empty() && polyArea)
        {
            bool hit = false;
            ++m_copySeen;
            for (int ty = ty0; ty <= ty1 && !hit; ++ty)
                for (int tx = tx0; tx <= tx1 && !hit; ++tx)
                    for (uint32_t i : m_copyTileList[static_cast<size_t>(ty * kCopyGrid + tx)])
                    {
                        CopyPoly &o = m_copyPolys[i];
                        if (o.seen == m_copySeen)
                            continue;
                        o.seen = m_copySeen;
                        if (copyPolysOverlap(poly, o))
                        {
                            hit = true;
                            break;
                        }
                    }
            if (hit && m_barrier && m_key.colorOn)
            {
                // New range behind a barrier: this primitive sees what the batch drew under it.
                m_barrierSplits.push_back(static_cast<uint32_t>(m_verts.size()));
                clearCopyPolys();
            }
            else if (hit)
            {
                ++m_stats.flushOverlap;
                flushDraw(); // re-copy so this primitive sees what the batch drew under it
            }
        }
        if (m_verts.empty())
        {
            m_batchKey = m_key;
            m_recIndex = UINT32_MAX;
        }
        if (m_recIndex == UINT32_MAX)
        {
            if (m_tme && !m_clutRowCurrent)
                useClutRow();
            StateRecord r = m_rec;
            r.w[1] |= static_cast<uint32_t>(m_clutRow) << 12;
            m_recIndex = static_cast<uint32_t>(m_records.size());
            m_records.push_back(r);
            m_clutRowInBatch.set(static_cast<size_t>(m_clutRow));
        }
        if (snap || clampTex)
        {
            // Only for textures that are art (fonts, UI), not render targets: effects that re-read the frame
            // with a 1:1 mapping (the shower steam's feedback blur) depend on its sub-pixel offsets.
            for (const auto &rt : m_rts)
                if (!rt->depth && (rt->valid & m_texMask).any())
                {
                    snap = false;
                    clampTex = false;
                    break;
                }
        }
        if (snap)
            ++m_stats.snapPrims;
        for (int i = 0; i < nv; ++i)
        {
            v[i].state = m_recIndex | (snap ? 0x80000000u : 0u) | (clampTex ? 0x40000000u : 0u);
            m_verts.push_back(v[i]);
        }
        m_batchColor |= colorPages;
        m_batchDepth |= depthPages;
        if (polyArea)
        {
            const uint32_t idx = static_cast<uint32_t>(m_copyPolys.size());
            m_copyPolys.push_back(poly);
            for (int ty = ty0; ty <= ty1; ++ty)
                for (int tx = tx0; tx <= tx1; ++tx)
                {
                    const uint32_t t = static_cast<uint32_t>(ty * kCopyGrid + tx);
                    if (m_copyTileList[t].empty())
                        m_copyTouched.push_back(t);
                    m_copyTileList[t].push_back(idx);
                }
            m_copyTilesUsed = true;
        }
        if (m_tme)
            m_batchTex |= m_texMask;
        if (m_key.srcOn)
            m_batchSrc |= m_primSrcMask;
        m_bx0 = m_bx1 < m_bx0 ? bx0 : std::min(m_bx0, bx0);
        m_by0 = m_by1 < m_by0 ? by0 : std::min(m_by0, by0);
        m_bx1 = std::max(m_bx1, bx1);
        m_by1 = std::max(m_by1, by1);
        if (single)
        {
            ++m_stats.flushSingle;
            flushDraw();
        }
    }

    // The pack picture for the current texture: its upload (at TBP0, same width and format) is still in
    // GS memory, and so is the CLUT upload the picture names, if any. A picture for any CLUT is the fallback.
    void hdLookup()
    {
        m_hdLookGen = m_hdGen;
        m_hdLook = 0;
        m_hdLookWrap = 0;
        const GSTex0Reg &t = m_state.context.tex0;
        hdVerify(t.tbp0, t.cbp);
        uint64_t tex = 0;
        bool found = false;
        for (const HdRegion &r : m_hdRegions)
            if (r.dbp == t.tbp0 && r.psm == t.psm && r.dbw == std::max<uint32_t>(t.tbw, 1u))
            {
                tex = r.key;
                found = true;
            }
        if (!found)
        {
            if (hdDebug())
                for (const HdRegion &r : m_hdRegions)
                    if (r.dbp == t.tbp0)
                        std::fprintf(stderr, "[texpack-hd] tex0 %u psm %u tbw %u does not match region psm %u dbw %u\n", t.tbp0, t.psm, t.tbw, r.psm, r.dbw);
            return;
        }
        const std::vector<PS2XTexHd> &pics = ps2xTexHdPictures();
        for (size_t i = 0; i < pics.size(); ++i)
        {
            if (pics[i].tex != tex)
                continue;
            if (pics[i].clut == 0u)
            {
                if (!m_hdLook)
                    m_hdLook = static_cast<uint16_t>(i + 1);
                continue;
            }
            for (const HdRegion &r : m_hdRegions)
                if (r.dbp == t.cbp && r.psm == t.cpsm && r.key == pics[i].clut)
                {
                    m_hdLook = static_cast<uint16_t>(i + 1);
                    break;
                }
            if (m_hdLook == i + 1)
                break;
        }
        if (hdDebug())
            std::fprintf(stderr, "[texpack-hd] tex0 %u cbp %u key %016llx -> picture %u\n", t.tbp0, t.cbp, (unsigned long long)tex, m_hdLook);
        if (m_hdLook)
        {
            const PS2XTexHd &pic = pics[m_hdLook - 1u];
            const uint64_t clamp = m_state.context.clamp;
            m_hdLookWrap = static_cast<uint8_t>(((clamp & 3u) == 0u && static_cast<uint32_t>(m_state.textureWidth) == pic.w ? 1u : 0u) |
                                                (((clamp >> 2) & 3u) == 0u && static_cast<uint32_t>(m_state.textureHeight) == pic.h ? 2u : 0u));
        }
    }
    GLuint hdTexture(uint16_t id)
    {
        const std::vector<PS2XTexHd> &pics = ps2xTexHdPictures();
        if (m_hdTex.size() < pics.size())
            m_hdTex.resize(pics.size(), 0);
        GLuint &tex = m_hdTex[id - 1u];
        if (!tex)
        {
            const PS2XTexHd &pic = pics[id - 1u];
            gl.GenTextures(1, &tex);
            gl.BindTexture(GL_TEXTURE_2D, tex);
            gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
            gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, pic.pw, pic.ph, 0, GL_RGBA, GL_UNSIGNED_BYTE, pic.rgba.data());
            gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
            gl.GenerateMipmap(GL_TEXTURE_2D);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        }
        return tex;
    }

    RenderTarget *peekRt(uint32_t basePage, uint32_t bw, int cls, bool depth)
    {
        for (auto &rt : m_rts)
            if (rt->basePage == basePage && rt->bw == bw && rt->cls == cls && rt->depth == depth)
                return rt.get();
        return nullptr;
    }
    // Outline of a primitive as drawn (positions are exact in 1/16 pixels, so doubles hold them exactly).
    // False for one of no area, which covers no pixels.
    static bool copyPoly(const GpuVertex *v, int nv, CopyPoly &p)
    {
        static constexpr int kSprite[4] = {0, 1, 4, 2};
        p.n = nv == 6 ? 4 : 3;
        for (int i = 0; i < p.n; ++i)
        {
            const GpuVertex &s = v[nv == 6 ? kSprite[i] : i];
            p.x[i] = s.x;
            p.y[i] = s.y;
        }
        double area = 0.0;
        for (int i = 0; i < p.n; ++i)
        {
            const int j = (i + 1) % p.n;
            area += p.x[i] * p.y[j] - p.x[j] * p.y[i];
        }
        p.seen = 0;
        return area != 0.0;
    }
    // Two primitives share a pixel. Separating-axis test; outlines that only touch along a line don't, by
    // the fill rule, provided both rasterise that line the same way: an axis-aligned one, or an edge both
    // have with the same end points (strip and fan neighbours). Any other touch counts as overlap.
    static bool copyPolysOverlap(const CopyPoly &a, const CopyPoly &b)
    {
        const CopyPoly *ps[2] = {&a, &b};
        for (int k = 0; k < 2; ++k)
        {
            const CopyPoly &p = *ps[k], &o = *ps[1 - k];
            for (int i = 0; i < p.n; ++i)
            {
                const int j = (i + 1) % p.n;
                const double nx = p.y[i] - p.y[j], ny = p.x[j] - p.x[i];
                double amin = 1e300, amax = -1e300, bmin = 1e300, bmax = -1e300;
                for (int m = 0; m < p.n; ++m)
                {
                    const double d = p.x[m] * nx + p.y[m] * ny;
                    amin = std::min(amin, d);
                    amax = std::max(amax, d);
                }
                for (int m = 0; m < o.n; ++m)
                {
                    const double d = o.x[m] * nx + o.y[m] * ny;
                    bmin = std::min(bmin, d);
                    bmax = std::max(bmax, d);
                }
                const double gap = std::max(amin, bmin) - std::min(amax, bmax);
                if (gap > 0.0)
                    return false;
                if (gap < 0.0)
                    continue;
                if (nx == 0.0 || ny == 0.0)
                    return false;
                for (int m = 0; m < o.n; ++m)
                {
                    const int q = (m + 1) % o.n;
                    if ((o.x[m] == p.x[i] && o.y[m] == p.y[i] && o.x[q] == p.x[j] && o.y[q] == p.y[j]) ||
                        (o.x[m] == p.x[j] && o.y[m] == p.y[j] && o.x[q] == p.x[i] && o.y[q] == p.y[i]))
                        return false;
                }
            }
        }
        return true;
    }
    void clearCopyPolys()
    {
        for (uint32_t t : m_copyTouched)
            m_copyTileList[t].clear();
        m_copyTouched.clear();
        m_copyPolys.clear();
    }
    // The colour target the current texture could be read from holds the newest data of every page the
    // texture covers.
    // Judged on the texels this primitive can read (its texture coordinates, one texel of filter margin,
    // the wrap mode), not the declared size: a 640x512 target is typically read as a 1024x1024 texture.
    // m_primSrcMask = their pages.
    bool hiSourceUsable(const GpuVertex *v, int nv)
    {
        const RenderTarget *rt = peekRt(m_srcBase, m_srcBw, m_srcCls, false);
        if (!rt)
            return false;
        const GSContext &ctx = m_state.context;
        int lo[2], hi[2];
        const float size[2] = {static_cast<float>(m_state.textureWidth), static_cast<float>(m_state.textureHeight)};
        for (int a = 0; a < 2; ++a)
        {
            float mn = 1e30f, mx = -1e30f;
            for (int i = 0; i < nv; ++i)
            {
                if (!(v[i].q > 0.0f))
                {
                    mn = -1e30f;
                    mx = 1e30f;
                    break;
                }
                const float c = (a == 0 ? v[i].s : v[i].t) / v[i].q * size[a];
                mn = std::min(mn, c);
                mx = std::max(mx, c);
            }
            if (nv == 6 && mx - mn < 8192.0f)
            {
                // Sprite: the extreme pixel centres are half a pixel inside its edges.
                const float px = a == 0 ? v[1].x - v[0].x : v[2].y - v[0].y;
                const float half = px > 0.0f ? (mx - mn) * 0.5f / px : 0.0f;
                mn += half;
                mx -= half;
            }
            const int n = a == 0 ? m_srcW : m_srcH;
            const uint32_t mode = static_cast<uint32_t>((ctx.clamp >> (a * 2)) & 3u);
            // Nearest reads floor(c), bilinear floor(c - 0.5) and the texel after it. That last one only counts
            // up to floor(c): past it, at the far edge, it has a weight of at most one half (half-texel offset
            // copies blend their last row with the row below, which here is often another buffer's).
            lo[a] = mn < -4096.0f ? -4096 : static_cast<int>(std::floor(mn - 0.5f));
            hi[a] = mx > 4096.0f ? 4096 : static_cast<int>(std::floor(mx));
            if (mode == 0u && (lo[a] < 0 || hi[a] >= n))
            {
                lo[a] = 0;
                hi[a] = n - 1;
            }
            else if (mode == 1u)
            {
                lo[a] = std::clamp(lo[a], 0, n - 1);
                hi[a] = std::clamp(hi[a], 0, n - 1);
            }
            else if (mode == 2u)
            {
                const int rmin = static_cast<int>((ctx.clamp >> (a == 0 ? 4 : 24)) & 0x3FFu), rmax = static_cast<int>((ctx.clamp >> (a == 0 ? 14 : 34)) & 0x3FFu);
                lo[a] = std::clamp(lo[a], rmin, rmax);
                hi[a] = std::clamp(hi[a], rmin, rmax);
            }
        }
        // One texel past the edge is allowed: coordinates that end exactly on it give it a filter weight below
        // one half, and the internal-resolution read clamps to the target there.
        if (lo[0] < 0 || lo[1] < 0 || hi[0] > rt->width || hi[1] > rt->height)
            return false;
        m_primSrcMask = rectPages(ctx.tex0.psm, ctx.tex0.tbp0, m_srcBw, lo[0], lo[1], std::min(hi[0], rt->width - 1), std::min(hi[1], rt->height - 1));
        return (m_primSrcMask & ~rt->valid).none();
    }

    struct RenderTargetLimits
    {
        int w, h;
    };
    RenderTargetLimits targetLimits() const
    {
        return {static_cast<int>(std::min<uint32_t>(m_key.fbw * 64u, 2048u)), m_key.needH};
    }

    // ------------------------------------------------------------ CLUT
    void useClutRow()
    {
        // Rows are immutable once a batch references them; a full ring flushes first.
        int next = (m_clutRow + 1) % kClutRows;
        if (m_clutRowInBatch.test(static_cast<size_t>(next)))
            flushDraw();
        m_clutRow = next;
        std::memcpy(&m_clutRing[static_cast<size_t>(m_clutRow) * 512], m_clut.data(), 512 * sizeof(uint16_t));
        m_clutRowDirty.set(static_cast<size_t>(m_clutRow));
        m_clutRowCurrent = true;
        m_recIndex = UINT32_MAX;
    }

    void loadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
    {
        const bool fourBit = tex0.psm == GS_PSM_T4 || tex0.psm == GS_PSM_T4HL || tex0.psm == GS_PSM_T4HH;
        const bool eightBit = tex0.psm == GS_PSM_T8 || tex0.psm == GS_PSM_T8H;
        if (!m_shadow || (!fourBit && !eightBit))
            return;
        switch (tex0.cld)
        {
        case 1u: break;
        case 2u: m_clutCbp[0] = tex0.cbp; break;
        case 3u: m_clutCbp[1] = tex0.cbp; break;
        case 4u:
            if (m_clutCbp[0] == tex0.cbp) return;
            m_clutCbp[0] = tex0.cbp;
            break;
        case 5u:
            if (m_clutCbp[1] == tex0.cbp) return;
            m_clutCbp[1] = tex0.cbp;
            break;
        default: return;
        }
        const bool sixteenBit = tex0.cpsm == GS_PSM_CT16 || tex0.cpsm == GS_PSM_CT16S;
        const bool thirtyTwoBit = tex0.cpsm == GS_PSM_CT32 || tex0.cpsm == GS_PSM_CT24;
        if (!sixteenBit && !thirtyTwoBit)
            return;
        // Source pages must be current in the CPU copy (flush only when the open batch draws there).
        PageMask src;
        if (tex0.csm == 0u)
            src = rectPages(tex0.cpsm, tex0.cbp, 1, 0, 0, 15, 15);
        else
        {
            const uint32_t cbw = texclut.cbw ? texclut.cbw : 1u;
            src = rectPages(tex0.cpsm, tex0.cbp, cbw, texclut.cou * 16, texclut.cov, texclut.cou * 16 + 255, texclut.cov);
        }
        if (m_glOk)
        {
            if ((src & (m_batchColor | m_batchDepth)).any())
                flushDraw();
            ensureCpu(src);
        }
        m_cpu.LoadClut(tex0, texclut); // keeps the CPU fallback rasterizer's CLUT in step

        const uint32_t entryCount = fourBit ? 16u : 256u;
        const uint32_t csaMask = sixteenBit ? 0x1Fu : 0x0Fu;
        const uint32_t destinationBase = (static_cast<uint32_t>(tex0.csa) & csaMask) << 4u;
        const bool loadCsm1Suffix = tex0.csm == 0u && thirtyTwoBit && !fourBit;
        const uint32_t firstEntry = loadCsm1Suffix ? destinationBase : 0u;
        const std::array<uint16_t, 512> previous = m_clut;
        for (uint32_t entry = firstEntry; entry < entryCount; ++entry)
        {
            uint32_t sx, sy, sw = 1u;
            if (tex0.csm == 0u)
            {
                const uint32_t si = (entry & ~0x18u) | ((entry & 0x08u) << 1u) | ((entry & 0x10u) >> 1u);
                sx = si & 0x0Fu;
                sy = si >> 4u;
            }
            else
            {
                sw = texclut.cbw != 0u ? texclut.cbw : 1u;
                sx = (static_cast<uint32_t>(texclut.cou) << 4u) + entry;
                sy = texclut.cov;
            }
            const uint32_t raw = m_cpu.ReadVram(tex0.cpsm, tex0.cbp, sw, sx, sy);
            const uint32_t dst = (loadCsm1Suffix ? entry : destinationBase + entry) & (sixteenBit ? 0x1FFu : 0x0FFu);
            if (sixteenBit)
                m_clut[dst] = static_cast<uint16_t>(raw);
            else
            {
                m_clut[dst] = static_cast<uint16_t>(raw & 0xFFFFu);
                m_clut[dst + 256u] = static_cast<uint16_t>(raw >> 16u);
            }
        }
        if (m_clut != previous)
        {
            m_clutRowCurrent = false;
            m_recIndex = UINT32_MAX;
        }
    }

    // ------------------------------------------------------------ transfers
    PageMask transferDest(const GSTransferCommand &t) const
    {
        const GSBitBltBuf &b = t.bitbltbuf;
        return rectPages(b.dpsm, b.dbp, std::max<uint32_t>(b.dbw, 1u), t.trxpos.dsax, t.trxpos.dsay,
                         t.trxpos.dsax + t.trxreg.rrw - 1, t.trxpos.dsay + t.trxreg.rrh - 1);
    }
    PageMask transferSrc(const GSTransferCommand &t) const
    {
        const GSBitBltBuf &b = t.bitbltbuf;
        return rectPages(b.spsm, b.sbp, std::max<uint32_t>(b.sbw, 1u), t.trxpos.ssax, t.trxpos.ssay,
                         t.trxpos.ssax + t.trxreg.rrw - 1, t.trxpos.ssay + t.trxreg.rrh - 1);
    }
    GSTransferCommand m_transfer{};
    PageMask m_transferDest;
    void beginTransfer(const GSTransferCommand &t)
    {
        if (!m_shadow)
            return;
        flushDraw();
        m_transfer = t;
        m_transferDest.reset();
        m_hdCapture = false;
        if (t.trxreg.rrw == 0u || t.trxreg.rrh == 0u)
        {
            m_cpu.BeginTransfer(t);
            return;
        }
        if (t.direction == 0u || t.direction == 2u)
            m_transferDest = transferDest(t);
        if (t.direction == 0u && t.trxpos.dsax == 0u && t.trxpos.dsay == 0u)
        {
            const uint32_t bits = (t.bitbltbuf.dpsm == GS_PSM_CT24 || t.bitbltbuf.dpsm == GS_PSM_Z24) ? 24u : GSInternal::bitsPerPixel(t.bitbltbuf.dpsm);
            m_hdExpected = static_cast<uint32_t>((static_cast<uint64_t>(t.trxreg.rrw) * t.trxreg.rrh * bits + 7u) / 8u);
            m_hdCapture = m_hdExpected != 0u && ps2xTexHdTracks(t.trxreg.rrw, t.trxreg.rrh, t.bitbltbuf.dpsm, m_hdExpected);
            m_hdBytes.clear();
        }
        if (t.direction == 1u || t.direction == 2u)
            ensureCpu(transferSrc(t));
        ensureCpu(m_transferDest);
        m_cpu.BeginTransfer(t);
        if (t.direction == 2u)
            cpuWritten(m_transferDest);
    }
    void uploadImage(const uint8_t *data, uint32_t size)
    {
        if (!m_shadow)
            return;
        flushDraw();
        if (m_transferDest.any())
            ensureCpu(m_transferDest);
        m_cpu.UploadImage(data, size);
        cpuWritten(m_transferDest);
        if (m_hdCapture)
        {
            const uint32_t take = std::min<uint32_t>(size, m_hdExpected - static_cast<uint32_t>(m_hdBytes.size()));
            m_hdBytes.insert(m_hdBytes.end(), data, data + take);
            uint64_t key = 0;
            if (m_hdBytes.size() >= m_hdExpected)
            {
                m_hdCapture = false;
                if (ps2xTexHdKey(m_hdBytes.data(), m_hdBytes.size(), key))
                {
                    const GSBitBltBuf &b = m_transfer.bitbltbuf;
                    const uint32_t bw = std::max<uint32_t>(b.dbw, 1u);
                    const uint32_t w = m_transfer.trxreg.rrw, h = m_transfer.trxreg.rrh;
                    // A new upload at the same place replaces an older region.
                    m_hdRegions.erase(std::remove_if(m_hdRegions.begin(), m_hdRegions.end(), [&](const HdRegion &r) { return r.dbp == b.dbp; }),
                                      m_hdRegions.end());
                    m_hdRegions.push_back({m_transferDest, b.dbp, bw, b.dpsm, key, w, h, m_hdBytes, false});
                    if (hdDebug())
                        std::fprintf(stderr, "[texpack-hd] upload %016llx at %u bw %u psm %u %ux%u\n", (unsigned long long)key, b.dbp, bw, b.dpsm, w, h);
                    ++m_hdGen;
                    ++m_stats.hdRegions;
                }
            }
        }
    }

    // ------------------------------------------------------------ drawing
    void ensureCopy(RenderTarget &rt)
    {
        if (m_copyW < rt.width * m_scale || m_copyH < rt.height * m_scale)
        {
            if (m_copyTex)
                gl.DeleteTextures(1, &m_copyTex);
            if (!m_copyFbo)
                gl.GenFramebuffers(1, &m_copyFbo);
            m_copyW = std::max(m_copyW, rt.width * m_scale);
            m_copyH = std::max(m_copyH, rt.height * m_scale);
            gl.GenTextures(1, &m_copyTex);
            gl.BindTexture(GL_TEXTURE_2D, m_copyTex);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, m_copyW, m_copyH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            gl.BindFramebuffer(GL_FRAMEBUFFER, m_copyFbo);
            gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_copyTex, 0);
        }
        gl.Disable(GL_SCISSOR_TEST);
        gl.BindFramebuffer(GL_READ_FRAMEBUFFER, rt.fbo);
        gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, m_copyFbo);
        const int x0 = std::max(m_bx0, 0) * m_scale, y0 = std::max(m_by0, 0) * m_scale;
        const int x1 = std::min(m_bx1 + 1, rt.width) * m_scale, y1 = std::min(m_by1 + 1, rt.height) * m_scale;
        if (x1 > x0 && y1 > y0)
        {
            gl.BlitFramebuffer(x0, y0, x1, y1, x0, y0, x1, y1, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            m_stats.copyPx += static_cast<uint64_t>(x1 - x0) * static_cast<uint64_t>(y1 - y0);
        }
        gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
        ++m_stats.copies;
    }

    // Copy of rect (0,0)-(w,h) of a target at the internal resolution, for a draw into that same target.
    GLuint copySource(RenderTarget &rt, int w, int h)
    {
        const int cw = rt.width * m_scale, ch = rt.height * m_scale;
        if (m_srcCopyW < cw || m_srcCopyH < ch)
        {
            if (m_srcCopyTex)
                gl.DeleteTextures(1, &m_srcCopyTex);
            if (!m_srcCopyFbo)
                gl.GenFramebuffers(1, &m_srcCopyFbo);
            m_srcCopyW = std::max(m_srcCopyW, cw);
            m_srcCopyH = std::max(m_srcCopyH, ch);
            gl.GenTextures(1, &m_srcCopyTex);
            gl.BindTexture(GL_TEXTURE_2D, m_srcCopyTex);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, m_srcCopyW, m_srcCopyH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            gl.BindFramebuffer(GL_FRAMEBUFFER, m_srcCopyFbo);
            gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_srcCopyTex, 0);
        }
        gl.Disable(GL_SCISSOR_TEST);
        gl.BindFramebuffer(GL_READ_FRAMEBUFFER, rt.fbo);
        gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, m_srcCopyFbo);
        const int x1 = std::min(w * m_scale, cw), y1 = std::min(h * m_scale, ch);
        gl.BlitFramebuffer(0, 0, x1, y1, 0, 0, x1, y1, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
        m_stats.copyPx += static_cast<uint64_t>(x1) * static_cast<uint64_t>(y1);
        ++m_stats.hiSrcCopies;
        return m_srcCopyTex;
    }

    void uploadClutRows()
    {
        if (m_clutRowDirty.none())
            return;
        gl.BindTexture(GL_TEXTURE_2D, m_clutTex);
        gl.PixelStorei(GL_UNPACK_ALIGNMENT, 2);
        for (int r = 0; r < kClutRows; ++r)
            if (m_clutRowDirty.test(static_cast<size_t>(r)))
                gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, r, 512, 1, GL_RED_INTEGER, GL_UNSIGNED_SHORT, &m_clutRing[static_cast<size_t>(r) * 512]);
        gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
        m_clutRowDirty.reset();
    }

    // A batch that is one flat rectangle, done without the draw shader: a clear (colour and depth written as
    // given) becomes a scissored glClear, and the alpha-bit pass (only the frame alpha's top bit changes) a
    // logic-op fill, which needs no copy of the target. Same pixels as the draw: the pixel centres inside the
    // rect at the internal resolution, within the GS scissor.
    // Returns the vertices it drew: the whole batch, or a leading zero-As sprite (the UI's reset comes first in
    // a batch of its icons, which the normal draw then takes from vertex 6), else 0.
    size_t tryFastFill(const DrawKey &k, RenderTarget *crt, RenderTarget *zrt, GLuint hiTex)
    {
        if (!crt || m_verts.size() < 6 || k.hdId || k.ate || k.date)
            return 0;
        const bool flat = !hiTex && !m_batchTex.any() && !k.srcOn;
        const bool clear = flat && k.outMode == 0 && k.alphaKeep == 0 && (!k.depthOn || (k.depthFunc == 1 && zrt));
        const bool alphaBit = flat && k.colorMask == 8 && k.alphaKeep == 0x7F && !k.depthOn;
        // (Cs - Cd) * As + Cd with As = 0 leaves RGB as it is: only the frame alpha and depth are written
        // (the UI's full-screen alpha/depth reset, otherwise a target copy and a full-screen textured pass).
        const bool zeroAs = !clear && !alphaBit && k.outMode == 1 && k.selA == 0 && k.selB == 1 && k.selD == 1 && !k.srcOn &&
                            (!k.depthOn || (k.depthFunc == 1 && zrt));
        if ((!clear && !alphaBit && !zeroAs) || (m_verts.size() != 6 && !zeroAs))
            return 0;
        const GpuVertex &v0 = m_verts[0];
        const StateRecord &r0 = m_records[v0.state & 0x3FFFFFFFu];
        float xs[2] = {v0.x, v0.x}, ys[2] = {v0.y, v0.y};
        for (size_t i = 0; i < 6; ++i)
        {
            const GpuVertex &v = m_verts[i];
            if (v.z != v0.z || v.a != v0.a)
                return 0;
            if (v.state != v0.state)
            {
                // The sprite's two halves may each get a record (they differ only in texture words, which a
                // zero As never reads); the words read below must match.
                const StateRecord &r = m_records[v.state & 0x3FFFFFFFu];
                if (!zeroAs || ((v.state ^ v0.state) & 0xC0000000u) || r.w[0] != r0.w[0] || r.w[2] != r0.w[2] ||
                    r.w[4] != r0.w[4] || r.w[5] != r0.w[5])
                    return 0;
            }
            if (!zeroAs && (v.r != v0.r || v.g != v0.g || v.b != v0.b || v.q != v0.q))
                return 0;
            xs[0] = std::min(xs[0], v.x);
            xs[1] = std::max(xs[1], v.x);
            ys[0] = std::min(ys[0], v.y);
            ys[1] = std::max(ys[1], v.y);
        }
        // Two triangles on the rect's corners, each missing the corner opposite the other's: the whole rect.
        int missing[2];
        for (int t = 0; t < 2; ++t)
        {
            unsigned seen = 0;
            for (int i = 0; i < 3; ++i)
            {
                const GpuVertex &v = m_verts[t * 3 + i];
                if ((v.x != xs[0] && v.x != xs[1]) || (v.y != ys[0] && v.y != ys[1]))
                    return 0;
                seen |= 1u << ((v.x == xs[1] ? 1 : 0) | (v.y == ys[1] ? 2 : 0));
            }
            if (seen != 7u && seen != 11u && seen != 13u && seen != 14u)
                return 0; // three distinct corners
            missing[t] = seen == 14u ? 0 : seen == 13u ? 1 : seen == 11u ? 2 : 3;
        }
        if ((missing[0] ^ missing[1]) != 3)
            return 0;
        const StateRecord &rec = m_records[v0.state & 0x3FFFFFFFu];
        const int s = m_scale;
        auto px = [s](float c) { return static_cast<int>(std::ceil(c * s - 0.5f)); };
        const int x0 = std::max({px(xs[0]), static_cast<int>(rec.w[4] & 0xFFFFu) * s, 0});
        const int x1 = std::min({px(xs[1]), static_cast<int>((rec.w[4] >> 16) + 1) * s, crt->width * s});
        const int y0 = std::max({px(ys[0]), static_cast<int>(rec.w[5] & 0xFFFFu) * s, 0});
        const int y1 = std::min({px(ys[1]), static_cast<int>((rec.w[5] >> 16) + 1) * s, crt->height * s});
        const uint32_t a = v0.a | (((rec.w[2] >> 12) & 1u) ? 0x80u : 0u);
        const bool textured = (rec.w[0] & (1u << 26)) != 0;
        if (zeroAs)
        {
            // As = Av, or At * Av for MODULATE with TCC: 0 for any texel when Av is 0. C must be As.
            const uint32_t tfx = (rec.w[0] >> 29) & 3u, tcc = (rec.w[0] >> 28) & 1u;
            if (v0.a != 0 || ((rec.w[2] >> 21) & 3u) != 0 || (textured && tcc && tfx != 0))
                return 0;
        }
        else if (textured)
            return 0;
        if (x1 > x0 && y1 > y0)
        {
            gl.BindFramebuffer(GL_FRAMEBUFFER, drawFbo(crt, zrt));
            gl.Viewport(0, 0, crt->width * s, crt->height * s);
            gl.Enable(GL_SCISSOR_TEST);
            gl.Scissor(x0, y0, x1 - x0, y1 - y0);
            if (clear)
            {
                const uint8_t cm = k.colorMask;
                GLbitfield bits = 0;
                if (cm)
                {
                    gl.ColorMask(cm & 1u ? GL_TRUE : GL_FALSE, cm & 2u ? GL_TRUE : GL_FALSE, cm & 4u ? GL_TRUE : GL_FALSE, cm & 8u ? GL_TRUE : GL_FALSE);
                    gl.ClearColor(v0.r / 255.0f, v0.g / 255.0f, v0.b / 255.0f, a / 255.0f);
                    bits |= GL_COLOR_BUFFER_BIT;
                }
                if (k.depthOn && k.depthWrite)
                {
                    gl.DepthMask(GL_TRUE);
                    gl.ClearDepth((v0.z / static_cast<float>(depthScale(zrt->cls)) * 2.0f - 1.0f) * 0.5f + 0.5f); // as the vertex shader maps it
                    bits |= GL_DEPTH_BUFFER_BIT;
                }
                if (bits)
                    gl.Clear(bits);
                ++m_stats.fastClears;
            }
            else if (zeroAs)
            {
                if (k.depthOn && k.depthWrite)
                {
                    gl.ColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                    gl.DepthMask(GL_TRUE);
                    gl.ClearDepth((v0.z / static_cast<float>(depthScale(zrt->cls)) * 2.0f - 1.0f) * 0.5f + 0.5f);
                    gl.Clear(GL_DEPTH_BUFFER_BIT);
                }
                if (k.colorMask & 8u)
                {
                    // Frame alpha = (Ad & keep) | (a & ~keep): an AND with keep, then an OR with the new bits.
                    const uint32_t keep = k.alphaKeep, set = a & ~keep & 0xFFu;
                    gl.Disable(GL_DEPTH_TEST);
                    gl.Disable(GL_BLEND);
                    gl.UseProgram(m_fillProg.id);
                    gl.Uniform2f(m_fillProg.u("uSize"), static_cast<float>(crt->width), static_cast<float>(crt->height));
                    gl.ColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
                    gl.Enable(GL_COLOR_LOGIC_OP);
                    for (int pass = 0; pass < 2; ++pass)
                    {
                        if (pass == 1 && !set)
                            break;
                        gl.Uniform4f(m_fillProg.u("uColor"), 0.0f, 0.0f, 0.0f, (pass ? set : keep) / 255.0f);
                        gl.LogicOp(pass ? GL_OR : GL_AND);
                        pushQuad(0.0f, 0.0f, static_cast<float>(crt->width), static_cast<float>(crt->height));
                        drawQuads();
                    }
                    gl.Disable(GL_COLOR_LOGIC_OP);
                }
                ++m_stats.fastZeroAlpha;
            }
            else
            {
                gl.Disable(GL_DEPTH_TEST);
                gl.Disable(GL_BLEND);
                gl.UseProgram(m_fillProg.id);
                gl.Uniform2f(m_fillProg.u("uSize"), static_cast<float>(crt->width), static_cast<float>(crt->height));
                const bool set = (a & 0x80u) != 0;
                gl.Uniform4f(m_fillProg.u("uColor"), 0.0f, 0.0f, 0.0f, set ? 128.0f / 255.0f : 127.0f / 255.0f);
                gl.Enable(GL_COLOR_LOGIC_OP);
                gl.LogicOp(set ? GL_OR : GL_AND);
                gl.ColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
                pushQuad(0.0f, 0.0f, static_cast<float>(crt->width), static_cast<float>(crt->height)); // the scissor is the rect
                drawQuads();
                gl.Disable(GL_COLOR_LOGIC_OP);
                ++m_stats.fastAlpha;
            }
            gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            gl.DepthMask(GL_TRUE);
            gl.Disable(GL_SCISSOR_TEST);
        }
        return 6;
    }

    // PS2X_GS_GPU_FASTFILL: unset/1 = on, 0 = off, check = run both the fast fill and the normal draw on every
    // batch it takes and log any pixel where they differ (debug: full render-target readbacks, slow).
    static int fastFillMode()
    {
        static const int mode = []
        {
            const char *e = std::getenv("PS2X_GS_GPU_FASTFILL");
            if (!e || !*e)
                return 1;
            return std::strcmp(e, "check") == 0 ? 2 : (std::atoi(e) ? 1 : 0);
        }();
        return mode;
    }
    struct FastFillCheck
    {
        bool on = false;
        std::vector<uint8_t> color;
        std::vector<float> depth;
        uint64_t batches = 0, bad = 0;
    } m_ffCheck;
    void readTarget(RenderTarget *rt, GLuint fbo, std::vector<uint8_t> *color, std::vector<float> *depth)
    {
        const int w = rt->width * m_scale, h = rt->height * m_scale;
        gl.BindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
        if (color)
        {
            color->resize(static_cast<size_t>(w) * h * 4u);
            gl.ReadBuffer(GL_COLOR_ATTACHMENT0);
            gl.ReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, color->data());
        }
        if (depth)
        {
            depth->resize(static_cast<size_t>(w) * h);
            gl.ReadPixels(0, 0, w, h, GL_DEPTH_COMPONENT, GL_FLOAT, depth->data());
        }
    }
    // Saves the targets, does the fast fill and keeps its result, then puts the targets back for the normal draw;
    // fastFillCheckEnd compares the two.
    size_t fastFillCheckBegin(const DrawKey &k, RenderTarget *crt, RenderTarget *zrt, GLuint hiTex)
    {
        if (!crt)
            return 0;
        const GLuint fbo = drawFbo(crt, zrt);
        std::vector<uint8_t> c0;
        std::vector<float> d0;
        readTarget(crt, fbo, &c0, zrt ? &d0 : nullptr);
        const size_t done = tryFastFill(k, crt, zrt, hiTex);
        if (!done)
            return 0;
        readTarget(crt, fbo, &m_ffCheck.color, zrt ? &m_ffCheck.depth : nullptr);
        const int w = crt->width * m_scale, h = crt->height * m_scale;
        gl.BindTexture(GL_TEXTURE_2D, crt->tex);
        gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, c0.data());
        if (zrt)
        {
            gl.BindTexture(GL_TEXTURE_2D, zrt->tex);
            gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, zrt->width * m_scale, zrt->height * m_scale, GL_DEPTH_COMPONENT, GL_FLOAT, d0.data());
        }
        m_ffCheck.on = true;
        return done;
    }
    void fastFillCheckEnd(RenderTarget *crt, RenderTarget *zrt)
    {
        m_ffCheck.on = false;
        std::vector<uint8_t> c;
        std::vector<float> d;
        readTarget(crt, drawFbo(crt, zrt), &c, zrt ? &d : nullptr);
        const int w = crt->width * m_scale;
        size_t badC = 0, badD = 0, first = SIZE_MAX;
        for (size_t i = 0; i < c.size(); ++i)
            if (c[i] != m_ffCheck.color[i])
            {
                ++badC;
                if (first == SIZE_MAX)
                    first = i / 4;
            }
        for (size_t i = 0; i < d.size() && i < m_ffCheck.depth.size(); ++i)
            if (d[i] != m_ffCheck.depth[i])
                ++badD;
        ++m_ffCheck.batches;
        if (badC || badD)
        {
            ++m_ffCheck.bad;
            if (m_ffCheck.bad <= 20)
            {
                const size_t f = first == SIZE_MAX ? 0 : first;
                std::fprintf(stderr, "[gsgpu] fastfill check: batch %llu differs: %zu colour bytes, %zu depth | first px (%zu,%zu) fast %02x%02x%02x%02x draw %02x%02x%02x%02x\n",
                             static_cast<unsigned long long>(m_ffCheck.batches), badC, badD, f % w, f / w,
                             m_ffCheck.color[f * 4], m_ffCheck.color[f * 4 + 1], m_ffCheck.color[f * 4 + 2], m_ffCheck.color[f * 4 + 3],
                             c[f * 4], c[f * 4 + 1], c[f * 4 + 2], c[f * 4 + 3]);
            }
        }
        if (m_ffCheck.batches % 500 == 0)
            std::fprintf(stderr, "[gsgpu] fastfill check: %llu batches, %llu differ\n",
                         static_cast<unsigned long long>(m_ffCheck.batches), static_cast<unsigned long long>(m_ffCheck.bad));
    }

    // PS2X_GS_DECCACHE=0 (diagnostic): decode every texel in the draw shader, as before the cache.
    static bool decCacheOn()
    {
        static const bool on = [] { const char *e = std::getenv("PS2X_GS_DECCACHE"); return !(e && *e == '0'); }();
        return on;
    }
    // Points each textured record of the batch at a decoded copy in the atlas (decoding the ones not there yet).
    // The vtex pages are current here (ensureVtex) and the CLUT rows uploaded.
    void decodeCached()
    {
        m_decMemo.clear();
        bool bound = false;
        for (StateRecord &r : m_records)
        {
            if (!(r.w[0] & (1u << 26)))
                continue;
            const DecMemo *memo = nullptr;
            for (const DecMemo &m : m_decMemo)
                if (m.w0 == r.w[0] && m.w1 == r.w[1] && m.w3 == r.w[3] && m.w6 == r.w[6] && m.w7 == r.w[7])
                {
                    memo = &m;
                    break;
                }
            uint32_t out = 0;
            if (memo)
                out = memo->out;
            else
            {
                out = decLookup(r, bound);
                m_decMemo.push_back({r.w[0], r.w[1], r.w[3], r.w[6], r.w[7], out});
            }
            if (out)
            {
                r.w[1] = out;
                r.w[3] |= 0x80000000u;
            }
        }
    }
    void decBind(bool &bound)
    {
        if (bound)
            return;
        gl.BindFramebuffer(GL_FRAMEBUFFER, m_decFbo);
        gl.Viewport(0, 0, kDecSize, kDecSize);
        gl.Disable(GL_BLEND);
        gl.Disable(GL_DEPTH_TEST);
        gl.Disable(GL_SCISSOR_TEST);
        gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        gl.UseProgram(m_decProg.id);
        bindCommonTextures();
        gl.ActiveTexture(GL_TEXTURE2);
        gl.BindTexture(GL_TEXTURE_2D, m_clutTex);
        bound = true;
    }
    void decDraw(const StateRecord &r, int x, int y, int w, int h)
    {
        gl.Uniform2i(m_decProg.u("uOrg"), x, y);
        gl.Uniform4ui(m_decProg.u("vS0"), r.w[0], r.w[1], r.w[2], r.w[3]);
        pushQuad(static_cast<float>(x), static_cast<float>(y), static_cast<float>(x + w), static_cast<float>(y + h));
        drawQuads();
    }
    // PS2X_GS_DECCHECK=1 (diagnostic, slow): on every hit, decode the texture again from the current vtex/CLUT
    // and compare it word for word with the cached copy; mismatches are counted and the first few printed.
    static bool decCheckOn()
    {
        static const bool on = [] { const char *e = std::getenv("PS2X_GS_DECCHECK"); return e && *e == '1'; }();
        return on;
    }
    GLuint m_decChkTex = 0, m_decChkFbo = 0;
    void decVerify(const DecEntry &e, const StateRecord &r, bool &bound)
    {
        const int w = static_cast<int>(e.w), h = static_cast<int>(e.h);
        if (w > 2048 || h > 2048)
            return;
        if (!m_decChkTex)
        {
            gl.GenTextures(1, &m_decChkTex);
            gl.BindTexture(GL_TEXTURE_2D, m_decChkTex);
            gl.TexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, 2048, 2048, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            gl.GenFramebuffers(1, &m_decChkFbo);
            gl.BindFramebuffer(GL_FRAMEBUFFER, m_decChkFbo);
            gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_decChkTex, 0);
            bound = false;
        }
        decBind(bound);
        gl.BindFramebuffer(GL_FRAMEBUFFER, m_decChkFbo);
        decDraw(r, 0, 0, w, h);
        std::vector<uint32_t> fresh(static_cast<size_t>(w) * h), cached(fresh.size());
        gl.ReadBuffer(GL_COLOR_ATTACHMENT0);
        gl.ReadPixels(0, 0, w, h, GL_RED_INTEGER, GL_UNSIGNED_INT, fresh.data());
        gl.BindFramebuffer(GL_FRAMEBUFFER, m_decFbo);
        gl.ReadBuffer(GL_COLOR_ATTACHMENT0);
        gl.ReadPixels(e.x * 8, e.y * 8, w, h, GL_RED_INTEGER, GL_UNSIGNED_INT, cached.data());
        ++m_stats.decChecked;
        size_t bad = 0, first = 0;
        for (size_t i = 0; i < fresh.size(); ++i)
            if (fresh[i] != cached[i] && bad++ == 0)
                first = i;
        if (bad && ++m_stats.decCheckBad <= 20)
            std::fprintf(stderr, "[gsgpu] DECCHECK mismatch: psm %u tbp %u tbw %u %dx%d: %zu texels differ, first (%zu,%zu) cached %08x fresh %08x\n",
                         (r.w[0] >> 20) & 63u, r.w[0] & 0x3FFFu, (r.w[0] >> 14) & 63u, w, h, bad, first % w, first / w, cached[first], fresh[first]);
    }
    uint32_t decLookup(const StateRecord &r, bool &bound)
    {
        const uint32_t w0 = r.w[0], w1 = r.w[1], w3 = r.w[3];
        const uint32_t psm = (w0 >> 20) & 63u;
        const bool clut = psm == GS_PSM_T8 || psm == GS_PSM_T8H || psm == GS_PSM_T4 || psm == GS_PSM_T4HL || psm == GS_PSM_T4HH;
        // Every texel wrapCoord can return (as the texture page mask is built).
        int w = 1 << std::min<uint32_t>(w1 & 15u, 10u), h = 1 << std::min<uint32_t>((w1 >> 4) & 15u, 10u);
        const uint32_t minU = r.w[6] & 0xFFFFu, maxU = r.w[6] >> 16, minV = r.w[7] & 0xFFFFu, maxV = r.w[7] >> 16;
        if (((w1 >> 8) & 3u) >= 2u)
            w = std::max<int>(w, static_cast<int>(std::max(minU | maxU, maxU)) + 1);
        if (((w1 >> 10) & 3u) >= 2u)
            h = std::max<int>(h, static_cast<int>(std::max(minV | maxV, maxV)) + 1);
        const uint32_t kw0 = w0 & 0x3FFFFFFu, kw1 = clut ? (w1 & 0xFFF00000u) : 0u, kw3 = w3 & 0x1FFFFu;
        const uint16_t *row = &m_clutRing[static_cast<size_t>((w1 >> 12) & 255u) * 512];
        uint64_t key = (uint64_t(kw0) << 32) ^ (uint64_t(kw1) * 0x9E3779B97F4A7C15ull) ^ (uint64_t(kw3) << 7) ^ (uint64_t(w) << 40) ^ uint64_t(h);
        if (clut)
            for (int i = 0; i < 512; ++i)
                key = (key ^ row[i]) * 0x100000001B3ull;
        auto &bucket = m_dec[key];
        for (const DecEntry &e : bucket)
            if (e.w0 == kw0 && e.w1 == kw1 && e.w3 == kw3 && e.w == uint32_t(w) && e.h == uint32_t(h) &&
                (!clut || std::memcmp(e.clut.data(), row, 512 * sizeof(uint16_t)) == 0))
            {
                ++m_stats.decHits;
                if (decCheckOn())
                    decVerify(e, r, bound);
                return (w1 & 0xFFFu) | (uint32_t(e.x) << 12) | (uint32_t(e.y) << 21);
            }
        // Shelf allocation, 8-aligned; when the atlas is full it starts over, unless this flush already
        // points records into it (then this one keeps the shader decode).
        const int aw = (w + 7) & ~7, ah = (h + 7) & ~7;
        if (m_decX + aw > kDecSize)
        {
            m_decX = 0;
            m_decY += m_decShelf;
            m_decShelf = 0;
        }
        if (m_decY + ah > kDecSize)
        {
            for (const DecMemo &m : m_decMemo)
                if (m.out)
                {
                    if (bucket.empty())
                        m_dec.erase(key);
                    return 0;
                }
            m_dec.clear();
            m_decPages.reset();
            m_decX = m_decY = m_decShelf = 0;
            ++m_stats.decResets;
        }
        DecEntry e{kw0, kw1, kw3, uint32_t(w), uint32_t(h), {}, rectPages(psm, w0 & 0x3FFFu, std::max<uint32_t>((w0 >> 14) & 63u, 1u), 0, 0, w - 1, h - 1),
                   static_cast<uint16_t>(m_decX / 8), static_cast<uint16_t>(m_decY / 8)};
        if (clut)
            e.clut.assign(row, row + 512);
        const int x = m_decX, y = m_decY;
        m_decX += aw;
        m_decShelf = std::max(m_decShelf, ah);
        decBind(bound);
        decDraw(r, x, y, w, h);
        m_decPages |= e.pages;
        m_dec[key].push_back(std::move(e));
        ++m_stats.decMisses;
        m_stats.decTexels += uint64_t(w) * uint64_t(h);
        return (w1 & 0xFFFu) | (uint32_t(x / 8) << 12) | (uint32_t(y / 8) << 21);
    }

    void flushDraw()
    {
        if (m_verts.empty())
            return;
        beginGpuSpan();
        const DrawKey k = m_batchKey;
        ++m_useClock;
        if (k.srcOn)
            if (RenderTarget *s = peekRt(k.srcBase, k.srcBw, k.srcCls, false))
                s->lastUse = m_useClock; // not retired for the targets below
        RenderTarget *crt = k.colorOn ? findRt(k.fbp, k.fbw, k.fcls, false, k.needH) : nullptr;
        RenderTarget *zrt = k.depthOn ? findRt(k.zbp, k.fbw, k.zcls, true, k.needH) : nullptr;
        if (crt)
            crt->lastUse = m_useClock;
        // Texture straight from a colour target at the internal resolution, if it still holds every page
        // (else the vtex path, made current below; skipped when the target serves it, as its pages are
        // only written back when something else reads them). Its own target: from a copy of the texture rect.
        GLuint hiTex = 0;
        if (k.srcOn)
        {
            RenderTarget *srt = peekRt(k.srcBase, k.srcBw, k.srcCls, false);
            if (srt && (m_batchSrc & ~srt->valid).none())
            {
                hiTex = srt == crt ? copySource(*srt, srt->width, srt->height) : srt->tex;
                ++m_stats.hiSrcDraws;
            }
            else
                ++m_stats.hiSrcMiss;
        }
        if (m_batchTex.any())
        {
            if (!hiTex)
                ensureVtex(m_batchTex);
            else
                ++m_stats.vtexSkipped;
        }
        if (m_gpuQueryOpen && !m_spanTag.empty())
        {
            // Stats only: fbp 14 | target w 12 | h 12 | tex 1 | hi 1 | outMode 2 | ate 1 | date 2 | depth 1 | copy 1 | prims in the top 16.
            const RenderTarget *rt = crt ? crt : zrt;
            m_spanTag[m_gpuQueryHead] = (k.fbp & 0x3FFFu) | (uint64_t(rt ? rt->width & 0xFFF : 0) << 14) | (uint64_t(rt ? rt->height & 0xFFF : 0) << 26) |
                                        (uint64_t(m_batchTex.any() || hiTex || k.hdId) << 38) | (uint64_t(hiTex != 0) << 39) | (uint64_t(k.outMode & 3u) << 40) |
                                        (uint64_t(k.ate & 1u) << 42) | (uint64_t(k.date & 3u) << 43) | (uint64_t(k.depthOn & 1u) << 45) | (uint64_t(k.copy & 1u) << 46) |
                                        (uint64_t(std::min<size_t>(m_verts.size() / 3, 0xFFFF)) << 48);
        }
        uploadClutRows();
        if (crt)
            ensureRtValid(*crt, m_batchColor);
        if (zrt)
            ensureRtValid(*zrt, m_batchDepth);
        // Segments of the batch to draw: from the first vertex a fast fill left, or in check mode the fast-filled
        // vertices alone (compared before the rest is drawn).
        size_t seg[3] = {0, m_verts.size(), m_verts.size()};
        if (fastFillMode() == 1)
        {
            seg[0] = tryFastFill(k, crt, zrt, hiTex);
            if (seg[0] == m_verts.size())
            {
                finishFlush(k, crt, zrt);
                return;
            }
        }
        else if (fastFillMode() == 2)
            if (const size_t done = fastFillCheckBegin(k, crt, zrt, hiTex))
                seg[1] = done;
        // Barrier mode reads the bound target itself (a feedback loop made safe by the barrier before
        // each range, whose primitives never cover a pixel twice).
        const bool barrier = k.copy && crt && m_barrier;
        if (k.copy && crt && !barrier)
            ensureCopy(*crt);
        if (!hiTex && !k.hdId && m_batchTex.any() && decCacheOn())
            decodeCached();
        // Stream vertices and state records.
        const size_t vbytes = m_verts.size() * sizeof(GpuVertex);
        if (m_vboOffset + vbytes > kVboBytes)
        {
            gl.BindBuffer(GL_ARRAY_BUFFER, m_vbo);
            gl.BufferData(GL_ARRAY_BUFFER, kVboBytes, nullptr, GL_STREAM_DRAW);
            m_vboOffset = 0;
        }
        gl.BindBuffer(GL_ARRAY_BUFFER, m_vbo);
        gl.BufferSubData(GL_ARRAY_BUFFER, static_cast<GLintptr>(m_vboOffset), static_cast<GLsizeiptr>(vbytes), m_verts.data());
        const GLint firstVertex = static_cast<GLint>(m_vboOffset / sizeof(GpuVertex));
        m_vboOffset += vbytes;
        const size_t sbytes = m_records.size() * sizeof(StateRecord);
        if (m_stateOffset + sbytes > kStateBytes)
        {
            gl.BindBuffer(GL_TEXTURE_BUFFER, m_stateBuf);
            gl.BufferData(GL_TEXTURE_BUFFER, kStateBytes, nullptr, GL_STREAM_DRAW);
            m_stateOffset = 0;
        }
        gl.BindBuffer(GL_TEXTURE_BUFFER, m_stateBuf);
        gl.BufferSubData(GL_TEXTURE_BUFFER, static_cast<GLintptr>(m_stateOffset), static_cast<GLsizeiptr>(sbytes), m_records.data());
        const GLint stateBase = static_cast<GLint>(m_stateOffset / sizeof(StateRecord));
        m_stateOffset += sbytes;

        splitGpuSpan();
        RenderTarget *sizeRt = crt ? crt : zrt;
        gl.BindFramebuffer(GL_FRAMEBUFFER, drawFbo(crt, zrt));
        if (!crt)
            gl.DrawBuffer(GL_NONE);
        gl.Viewport(0, 0, sizeRt->width * m_scale, sizeRt->height * m_scale);
        gl.Disable(GL_SCISSOR_TEST);
        Program &dp = k.ate || k.date ? m_drawProg : m_drawProgNd;
        gl.UseProgram(dp.id);
        gl.Uniform2f(dp.u("uRtSize"), static_cast<float>(sizeRt->width), static_cast<float>(sizeRt->height));
        gl.Uniform1f(dp.u("uDepthScale"), static_cast<float>(zrt ? depthScale(zrt->cls) : 65536.0));
        gl.Uniform1i(dp.u("uStateBase"), stateBase);
        gl.Uniform1i(dp.u("uOutMode"), k.outMode);
        gl.Uniform1i(dp.u("uDate"), k.date);
        gl.Uniform1i(dp.u("uSelA"), k.selA);
        gl.Uniform1i(dp.u("uSelB"), k.selB);
        gl.Uniform1i(dp.u("uSelD"), k.selD);
        gl.Uniform1ui(dp.u("uAlphaKeep"), k.alphaKeep);
        gl.Uniform1i(dp.u("uUseCopy"), k.copy ? 1 : 0);
        gl.Uniform1i(dp.u("uFrameCt24"), k.ct24 ? 1 : 0);
        bindCommonTextures();
        gl.ActiveTexture(GL_TEXTURE2);
        gl.BindTexture(GL_TEXTURE_2D, m_clutTex);
        gl.ActiveTexture(GL_TEXTURE3);
        gl.BindTexture(GL_TEXTURE_BUFFER, m_stateTex);
        gl.ActiveTexture(GL_TEXTURE4);
        gl.BindTexture(GL_TEXTURE_2D, barrier ? crt->tex : k.copy ? m_copyTex : 0);
        gl.Uniform1i(dp.u("uHiSrc"), hiTex ? 1 : 0);
        gl.ActiveTexture(GL_TEXTURE6);
        gl.BindTexture(GL_TEXTURE_2D, hiTex);
        gl.ActiveTexture(GL_TEXTURE8);
        gl.BindTexture(GL_TEXTURE_2D, m_decTex);
        gl.Uniform1i(dp.u("uRepl"), k.hdId ? 1 : 0);
        if (k.hdId)
        {
            const PS2XTexHd &pic = ps2xTexHdPictures()[k.hdId - 1u];
            gl.ActiveTexture(GL_TEXTURE7);
            gl.BindTexture(GL_TEXTURE_2D, hdTexture(k.hdId));
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (k.hdWrap & 1u) ? GL_REPEAT : GL_CLAMP_TO_EDGE);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (k.hdWrap & 2u) ? GL_REPEAT : GL_CLAMP_TO_EDGE);
            gl.Uniform2f(dp.u("uReplSize"), static_cast<float>(pic.w), static_cast<float>(pic.h));
        }

        if (k.outMode == 1)
        {
            gl.Enable(GL_BLEND);
            gl.BlendEquationSeparate(k.blendEq, GL_FUNC_ADD);
            gl.BlendFuncSeparate(k.blendSrc, k.blendDst, GL_ONE, GL_ZERO);
        }
        else if (k.outMode == 3)
        {
            gl.Enable(GL_BLEND);
            gl.BlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
            gl.BlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ZERO);
        }
        else
            gl.Disable(GL_BLEND);
        if (k.depthOn)
        {
            gl.Enable(GL_DEPTH_TEST);
            gl.DepthFunc(k.depthFunc == 1 ? GL_ALWAYS : k.depthFunc == 2 ? GL_GEQUAL : GL_GREATER);
        }
        else
            gl.Disable(GL_DEPTH_TEST);
        gl.BindVertexArray(m_vao);
        for (GLenum c = GL_CLIP_DISTANCE0; c < GL_CLIP_DISTANCE0 + 4; ++c)
            gl.Enable(c); // the scissor (kDrawVs)

        // Passes: alpha test pass/fail split (AFAIL).
        struct Pass
        {
            int alphaMode;
            uint8_t colorMask;
            bool depthWrite;
        };
        Pass passes[2];
        int passCount = 0;
        const uint8_t cm = k.colorMask;
        const bool zw = k.depthWrite && k.depthOn;
        if (!k.ate)
            passes[passCount++] = {0, cm, zw};
        else
        {
            passes[passCount++] = {1, cm, zw};
            switch (k.afail)
            {
            case 1: passes[passCount++] = {2, cm, false}; break;                       // FB_ONLY
            case 2: // ZB_ONLY: nothing to write without a depth write
                if (zw)
                    passes[passCount++] = {2, 0, zw};
                break;
            case 3: passes[passCount++] = {2, static_cast<uint8_t>(cm & 7u), false}; break; // RGB_ONLY
            default: break;
            }
        }
        // Ranges: the whole batch, or in barrier mode the runs between overlaps. A range's primitives
        // cover each pixel at most once, so its passes (disjoint pixels per primitive) need no barrier.
        const size_t rangeCount = barrier ? m_barrierSplits.size() + 1 : 1;
        for (int sg = 0; sg < 2; ++sg)
        {
            if (sg == 1 && m_ffCheck.on)
                fastFillCheckEnd(crt, zrt);
            for (size_t r = 0; r < rangeCount; ++r)
            {
                const uint32_t v0 = std::max<uint32_t>(r == 0 ? 0u : m_barrierSplits[r - 1], static_cast<uint32_t>(seg[sg]));
                const uint32_t v1 = std::min<uint32_t>(r + 1 < rangeCount ? m_barrierSplits[r] : static_cast<uint32_t>(m_verts.size()), static_cast<uint32_t>(seg[sg + 1]));
                if (v1 <= v0)
                    continue;
                if (barrier)
                {
                    glTextureBarrierFn();
                    ++m_stats.barriers;
                }
                for (int i = 0; i < passCount; ++i)
                {
                    const Pass &ps = passes[i];
                    gl.Uniform1i(dp.u("uAlphaMode"), ps.alphaMode);
                    const uint8_t mask = ps.colorMask;
                    gl.ColorMask(mask & 1u ? GL_TRUE : GL_FALSE, mask & 2u ? GL_TRUE : GL_FALSE, mask & 4u ? GL_TRUE : GL_FALSE,
                                 mask & 8u ? GL_TRUE : GL_FALSE);
                    gl.DepthMask(ps.depthWrite ? GL_TRUE : GL_FALSE);
                    gl.DrawArrays(GL_TRIANGLES, firstVertex + static_cast<GLint>(v0), static_cast<GLsizei>(v1 - v0));
                    ++m_stats.draws;
                }
            }
        }
        if (barrier)
        {
            gl.ActiveTexture(GL_TEXTURE4);
            gl.BindTexture(GL_TEXTURE_2D, 0); // no feedback loop for the next draws into this target
        }
        gl.BindVertexArray(0);
        for (GLenum c = GL_CLIP_DISTANCE0; c < GL_CLIP_DISTANCE0 + 4; ++c)
            gl.Disable(c);
        gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        gl.DepthMask(GL_TRUE);
        gl.Disable(GL_BLEND);
        if (hiTex)
        {
            gl.ActiveTexture(GL_TEXTURE6);
            gl.BindTexture(GL_TEXTURE_2D, 0);
        }
        if (k.hdId)
        {
            gl.ActiveTexture(GL_TEXTURE7);
            gl.BindTexture(GL_TEXTURE_2D, 0);
        }
        if (!crt)
            gl.DrawBuffer(GL_NONE);
        finishFlush(k, crt, zrt);
    }

    void finishFlush(const DrawKey &k, RenderTarget *crt, RenderTarget *zrt)
    {
        if (m_ffCheck.on)
            fastFillCheckEnd(crt, zrt);
        if (crt && (k.colorMask != 0))
            markDrawn(*crt, m_batchColor);
        if (zrt && k.depthWrite)
            markDrawn(*zrt, m_batchDepth);

        m_verts.clear();
        m_records.clear();
        m_recIndex = UINT32_MAX;
        m_clutRowInBatch.reset();
        m_batchColor.reset();
        m_batchDepth.reset();
        m_batchTex.reset();
        m_batchSrc.reset();
        m_bx0 = m_by0 = 0;
        m_bx1 = m_by1 = -1;
        if (m_copyTilesUsed)
        {
            clearCopyPolys();
            m_copyTilesUsed = false;
        }
        m_barrierSplits.clear();
        endGpuSpan();
        printStats();
    }

    static constexpr GLenum kGlFragmentShaderInvocations = 0x82F4; // ARB_pipeline_statistics_query
    static constexpr GLenum kGlSamplesPassed = 0x8914;

    static bool statsOn()
    {
        static const bool on = [] { const char *e = std::getenv("PS2X_GS_GPU_STATS"); return e && *e && *e != '0'; }();
        return on;
    }

    // Stats only: GPU time of each flushDraw (its uploads, copies and draws), so idle time between frames
    // is not counted and a GPU-bound run shows as gpu busy near 1000 ms/s. A ring of queries, read at
    // presents once available (never blocking); a flush finding the ring full goes untimed.
    void beginGpuSpan()
    {
        if (!(statsOn() || autoScale()) || m_gpuQueryOpen)
            return;
        if (m_gpuQueries.empty())
        {
            m_gpuQueries.resize(4096);
            gl.GenQueries(static_cast<GLsizei>(m_gpuQueries.size()), m_gpuQueries.data());
            if (statsOn())
            {
                m_fsQueries.resize(m_gpuQueries.size());
                m_spQueries.resize(m_gpuQueries.size());
                gl.GenQueries(static_cast<GLsizei>(m_fsQueries.size()), m_fsQueries.data());
                gl.GenQueries(static_cast<GLsizei>(m_spQueries.size()), m_spQueries.data());
                m_spanTag.assign(m_gpuQueries.size(), 0);
                m_drawQueries.resize(m_gpuQueries.size());
                gl.GenQueries(static_cast<GLsizei>(m_drawQueries.size()), m_drawQueries.data());
                m_spanSplit.assign(m_gpuQueries.size(), 0);
            }
        }
        if ((m_gpuQueryHead + 1) % m_gpuQueries.size() == m_gpuQueryTail)
            return;
        if (!m_spanSplit.empty())
            m_spanSplit[m_gpuQueryHead] = 0;
        gl.BeginQuery(GL_TIME_ELAPSED, m_gpuQueries[m_gpuQueryHead]);
        if (!m_fsQueries.empty())
        {
            gl.BeginQuery(kGlFragmentShaderInvocations, m_fsQueries[m_gpuQueryHead]);
            gl.BeginQuery(kGlSamplesPassed, m_spQueries[m_gpuQueryHead]);
        }
        m_gpuQueryOpen = true;
    }

    // Stats only: the span's time so far was its setup (uploads, target loads, copies); time the draws apart.
    // TIME_ELAPSED queries cannot nest, so the setup query ends and the draw query takes over.
    void splitGpuSpan()
    {
        if (!m_gpuQueryOpen || m_drawQueries.empty())
            return;
        gl.EndQuery(GL_TIME_ELAPSED);
        gl.BeginQuery(GL_TIME_ELAPSED, m_drawQueries[m_gpuQueryHead]);
        m_spanSplit[m_gpuQueryHead] = 1;
    }

    void endGpuSpan()
    {
        if (!m_gpuQueryOpen)
            return;
        gl.EndQuery(GL_TIME_ELAPSED);
        if (!m_fsQueries.empty())
        {
            gl.EndQuery(kGlFragmentShaderInvocations);
            gl.EndQuery(kGlSamplesPassed);
        }
        m_gpuQueryHead = (m_gpuQueryHead + 1) % m_gpuQueries.size();
        m_gpuQueryOpen = false;
    }

    void timeGpuFrame()
    {
        if (!(statsOn() || autoScale()))
            return;
        ++m_stats.gpuFrames;
        while (m_gpuQueryTail != m_gpuQueryHead)
        {
            GLint ready = 0;
            gl.GetQueryObjectiv(m_gpuQueries[m_gpuQueryTail], GL_QUERY_RESULT_AVAILABLE, &ready);
            if (!ready)
                break;
            const bool split = !m_spanSplit.empty() && m_spanSplit[m_gpuQueryTail];
            if (split)
            {
                gl.GetQueryObjectiv(m_drawQueries[m_gpuQueryTail], GL_QUERY_RESULT_AVAILABLE, &ready);
                if (!ready)
                    break;
            }
            GLuint64 ns = 0, drawNs = 0;
            gl.GetQueryObjectui64v(m_gpuQueries[m_gpuQueryTail], GL_QUERY_RESULT, &ns);
            if (split)
                gl.GetQueryObjectui64v(m_drawQueries[m_gpuQueryTail], GL_QUERY_RESULT, &drawNs);
            m_stats.gpuMs += (ns + drawNs) / 1.0e6;
            m_auto.gpuMs += (ns + drawNs) / 1.0e6;
            if (!m_fsQueries.empty())
            {
                GLuint64 n = 0;
                gl.GetQueryObjectui64v(m_fsQueries[m_gpuQueryTail], GL_QUERY_RESULT, &n);
                m_stats.fsInvocations += n;
                const uint64_t tag = m_spanTag[m_gpuQueryTail];
                FragAgg &f = m_fragByTag[tag & 0xFFFFFFFFFFFFull];
                f.frags += n;
                f.prims += tag >> 48;
                ++f.flushes;
                f.drawMs += drawNs / 1.0e6;
                f.setupMs += ns / 1.0e6;
                n = 0;
                gl.GetQueryObjectui64v(m_spQueries[m_gpuQueryTail], GL_QUERY_RESULT, &n);
                m_stats.samplesPassed += n;
            }
            m_gpuQueryTail = (m_gpuQueryTail + 1) % m_gpuQueries.size();
        }
    }

    void printStats()
    {
        if (!statsOn())
            return;
        const double t = nowSeconds();
        if (t - m_statsT0 < 1.0)
            return;
        std::fprintf(stderr, "[gsgpu] %.1fs: prims %llu draws %llu cpuPrims %llu | pages wb %llu load %llu up %llu down %llu present %llu | copies %llu rts %zu (+%llu) | fast clears %llu alpha %llu zeroAs %llu\n",
                     t - m_statsT0, (unsigned long long)m_stats.prims, (unsigned long long)m_stats.draws, (unsigned long long)m_stats.cpuPrims,
                     (unsigned long long)m_stats.wbPages, (unsigned long long)m_stats.loadPages, (unsigned long long)m_stats.upPages,
                     (unsigned long long)m_stats.downPages, (unsigned long long)m_stats.presentPages, (unsigned long long)m_stats.copies, m_rts.size(), (unsigned long long)m_stats.rts,
                     (unsigned long long)m_stats.fastClears, (unsigned long long)m_stats.fastAlpha, (unsigned long long)m_stats.fastZeroAlpha);
        std::fprintf(stderr, "[gsgpu]   flushes key %llu feedback %llu overlap %llu single %llu (tex %llu) | copy %.1f Mpx | barriers %llu\n", (unsigned long long)m_stats.flushKey,
                     (unsigned long long)m_stats.flushFeedback, (unsigned long long)m_stats.flushOverlap, (unsigned long long)m_stats.flushSingle, (unsigned long long)m_stats.singleTex, m_stats.copyPx / 1.0e6, (unsigned long long)m_stats.barriers);
        std::fprintf(stderr, "[gsgpu]   gl ms: draw %.1f wb %.1f up %.1f down %.1f\n", m_stats.drawMs, m_stats.wbMs, m_stats.upMs, m_stats.downMs);
        std::fprintf(stderr, "[gsgpu]   gpu busy %.1f ms over %llu frames (%.2f ms/frame)\n", m_stats.gpuMs, (unsigned long long)m_stats.gpuFrames,
                     m_stats.gpuFrames ? m_stats.gpuMs / m_stats.gpuFrames : 0.0);
        std::fprintf(stderr, "[gsgpu]   fragments %.1f M shaded %.1f M passed\n", m_stats.fsInvocations / 1.0e6, m_stats.samplesPassed / 1.0e6);
        {
            std::vector<std::pair<uint64_t, FragAgg>> v(m_fragByTag.begin(), m_fragByTag.end());
            std::sort(v.begin(), v.end(), [](const auto &a, const auto &b) { return a.second.frags > b.second.frags; });
            for (size_t i = 0; i < v.size() && i < 14; ++i)
            {
                const uint64_t g = v[i].first;
                std::fprintf(stderr, "[gsgpu]   frag %6.1f M fbp %llx %llux%llu tex %d hi %d out %d ate %d date %d z %d copy %d | flushes %llu prims %llu | ms draw %.2f setup %.2f\n",
                             v[i].second.frags / 1.0e6, (unsigned long long)(g & 0x3FFF), (unsigned long long)((g >> 14) & 0xFFF), (unsigned long long)((g >> 26) & 0xFFF),
                             int((g >> 38) & 1), int((g >> 39) & 1), int((g >> 40) & 3), int((g >> 42) & 1), int((g >> 43) & 3), int((g >> 45) & 1), int((g >> 46) & 1),
                             (unsigned long long)v[i].second.flushes, (unsigned long long)v[i].second.prims, v[i].second.drawMs, v[i].second.setupMs);
            }
            double restDraw = 0.0, restSetup = 0.0;
            for (size_t i = 14; i < v.size(); ++i)
            {
                restDraw += v[i].second.drawMs;
                restSetup += v[i].second.setupMs;
            }
            if (v.size() > 14)
                std::fprintf(stderr, "[gsgpu]   frag rest (%zu passes) | ms draw %.2f setup %.2f\n", v.size() - 14, restDraw, restSetup);
            m_fragByTag.clear();
        }
        std::fprintf(stderr, "[gsgpu]   presents %llu reads %llu (deferred %llu): %.1f ms (flush %.1f issue %.1f wait %.1f copy %.1f) throttle %.1f\n", (unsigned long long)m_stats.presents,
                     (unsigned long long)m_stats.reads, (unsigned long long)m_stats.readsDeferred, m_stats.presentMs, m_stats.flushMs, m_stats.issueMs, m_stats.waitMs, m_stats.copyMs, m_stats.throttleMs);
        for (const auto &kv : m_cpuReasons)
            std::fprintf(stderr, "[gsgpu]   cpu: %s x%llu\n", kv.first.c_str(), (unsigned long long)kv.second);
        if (!ps2xTexHdPictures().empty())
            std::fprintf(stderr, "[gsgpu]   hd pictures: prims %llu | uploads matched %llu (%zu live) | checks %llu %.2f ms\n", (unsigned long long)m_stats.hdPrims,
                         (unsigned long long)m_stats.hdRegions, m_hdRegions.size(), (unsigned long long)m_stats.hdVerifies, m_stats.hdVerifyMs);
        m_cpuReasons.clear();
        if (m_scale > 1)
        {
            std::fprintf(stderr, "[gsgpu]   x%d: target textures %llu (copies %llu, gone %llu, writebacks skipped %llu) | 1:1 prims %llu | presents at x%d %llu\n", m_scale,
                         (unsigned long long)m_stats.hiSrcDraws, (unsigned long long)m_stats.hiSrcCopies, (unsigned long long)m_stats.hiSrcMiss,
                         (unsigned long long)m_stats.vtexSkipped,
                         (unsigned long long)m_stats.snapPrims, m_scale, (unsigned long long)m_stats.hiresFrames);
            const GSPresentationRequest &q = m_presentRequest;
            std::fprintf(stderr, "[gsgpu]   display: pmode %llx smode2 %llx dispfb1 %llx display1 %llx dispfb2 %llx display2 %llx preferred %d %x/%u/%u -> %u\n",
                         (unsigned long long)q.pmode, (unsigned long long)q.smode2, (unsigned long long)q.dispfb1, (unsigned long long)q.display1,
                         (unsigned long long)q.dispfb2, (unsigned long long)q.display2, q.hasPreferredSource ? 1 : 0, q.preferredSource.fbp,
                         q.preferredSource.fbw, q.preferredSource.psm, q.preferredDestFbp);
            for (const auto &kv : m_hiresMiss)
                std::fprintf(stderr, "[gsgpu]   present at GS resolution: %s x%llu\n", kv.first, (unsigned long long)kv.second);
            m_hiresMiss.clear();
        }
        if (decCacheOn())
            std::fprintf(stderr, "[gsgpu]   decoded textures: hits %llu misses %llu (%.2f M texels) dropped %llu atlas resets %llu | %zu live\n",
                         (unsigned long long)m_stats.decHits, (unsigned long long)m_stats.decMisses, m_stats.decTexels / 1e6,
                         (unsigned long long)m_stats.decInval, (unsigned long long)m_stats.decResets, m_dec.size());
        if (decCheckOn())
            std::fprintf(stderr, "[gsgpu]   DECCHECK: %llu hits compared, %llu differed\n", (unsigned long long)m_stats.decChecked,
                         (unsigned long long)m_stats.decCheckBad);
        m_stats = {};
        m_statsT0 = t;
    }

    // ------------------------------------------------------------ presentation
    // Worker, m_processMutex held: make the requested pages current in the CPU copy and hand them over.
    void servePresent()
    {
        std::lock_guard<std::mutex> lock(m_presentMutex);
        if (!m_presentRequested.load())
            return;
        if (m_glOk && m_shadow && !s_syncPresent)
            presentAsync();
        else
        {
            copyPresentPages(m_presentMask);
            m_presentBufRequest = m_presentRequest;
        }
        if (m_glOk && m_shadow && (m_scale > 1 || autoScale()))
            presentHires(m_presentRequest);
        if (m_glOk && m_shadow && autoScale() && !s_syncPresent)
            autoScaleStep();
        m_presentRequested.store(false);
        m_presentDone = true;
        m_presentCv.notify_all();
    }
    void copyPresentPages(const PageMask &mask)
    {
        if (!m_shadow)
            return;
        flushDraw();
        ensureCpu(mask);
        m_presentBuf.resize(m_shadowSize);
        for (int p = 0; p < kPages; ++p)
            if (mask.test(p) && static_cast<size_t>(p + 1) * kPageBytes <= m_shadowSize)
                std::memcpy(m_presentBuf.data() + static_cast<size_t>(p) * kPageBytes, m_shadow + static_cast<size_t>(p) * kPageBytes, kPageBytes);
        ++m_presentBufGen;
    }

    // ------------------------------------------------------------ internal-resolution presentation
    // What the presentation reads, mirroring GSCpuBackend::PresentFromLocalMemory: the circuits' colour
    // targets, when they hold the displayed rects (preferred copy source and DISPFB origins as there).
    // Anything else (a black display at page 0, which the CPU decode swapped for a context frame, pages
    // last written by the CPU, no target) is presented from the GS-resolution decode.
    struct HiresCircuit
    {
        RenderTarget *rt = nullptr;
        int ox = 0, oy = 0, w = 0, h = 0;
        bool sixteen = false;
    };
    bool hiresCircuit(const GSPresentationRequest &r, uint64_t dispfb, uint64_t display, bool allowPreferred, HiresCircuit &c, const char *&miss)
    {
        const uint32_t magh = static_cast<uint32_t>((display >> 23) & 0xFu);
        c.w = static_cast<int>((((display >> 32) & 0xFFFu) + 1u) / (magh + 1u));
        c.h = static_cast<int>(((display >> 44) & 0x7FFu) + 1u);
        if (c.w < 64 || c.h < 64)
        {
            c.w = 640;
            c.h = 448;
        }
        c.w = std::min(c.w, 640);
        c.h = std::min(c.h, 512);
        const uint32_t fbp = static_cast<uint32_t>(dispfb & 0x1FFu);
        uint32_t base = fbp * 32u, fbw = static_cast<uint32_t>((dispfb >> 9) & 0x3Fu), psm = static_cast<uint32_t>((dispfb >> 15) & 0x1Fu);
        c.ox = static_cast<int>((dispfb >> 32) & 0x7FFu);
        c.oy = static_cast<int>((dispfb >> 43) & 0x7FFu);
        if (allowPreferred && r.hasPreferredSource && r.preferredDestFbp == fbp && (r.preferredSource.fbw != 0u || r.preferredSource.fbp != fbp))
        {
            base = r.preferredSource.fbp;
            fbw = r.preferredSource.fbw;
            psm = r.preferredSource.psm;
            c.ox = c.oy = 0;
        }
        else if (fbp == 0u && m_zeroFallback.load(std::memory_order_relaxed))
            return (miss = "display at page 0 replaced by a context frame"), false;
        fbw = fbw ? fbw : 10u;
        if ((base & 31u) != 0u || !isColorTargetPsm(psm))
            return (miss = "display format"), false;
        c.sixteen = psm == GS_PSM_CT16 || psm == GS_PSM_CT16S;
        c.rt = peekRt((base / 32u) & 511u, fbw, psmClass(psm), false);
        if (!c.rt)
            return (miss = "no GPU target"), false;
        if (c.rt->width < c.ox + c.w || c.rt->height < c.oy + c.h)
            return (miss = "target smaller than the display"), false;
        if ((rectPages(psm, base, fbw, c.ox, c.oy, c.ox + c.w - 1, c.oy + c.h - 1) & ~c.rt->valid).any())
            return (miss = "display pages not on the GPU"), false;
        return true;
    }

    // Worker: render the displayed picture at the internal resolution into a free slot and publish it.
    void presentHires(const GSPresentationRequest &r)
    {
        uint64_t served;
        {
            std::lock_guard<std::mutex> lock(g_hires.mutex);
            served = ++g_hires.served;
        }
        if (m_shotT0 == 0.0)
            m_shotT0 = nowSeconds(); // the first present, like the GS-resolution shots' clock
        auto circuitOn = [](uint64_t display, uint64_t dispfb)
        {
            return ((dispfb >> 9) & 0x3Fu) != 0u || ((display >> 32) & 0xFFFu) != 0u || ((display >> 44) & 0x7FFu) != 0u ||
                   ((display >> 23) & 0xFu) != 0u;
        };
        const bool valid1 = (r.pmode & 1u) && circuitOn(r.display1, r.dispfb1);
        const bool valid2 = (r.pmode & 2u) && circuitOn(r.display2, r.dispfb2);
        const char *miss = nullptr;
        HiresCircuit c1, c2; // c1 blends over c2 (both circuits), else c1 alone
        bool ok = valid1 || valid2;
        if (!ok)
            miss = "no display";
        else if (valid1 && valid2)
            ok = hiresCircuit(r, r.dispfb1, r.display1, false, c1, miss) && hiresCircuit(r, r.dispfb2, r.display2, false, c2, miss);
        else
            ok = hiresCircuit(r, valid1 ? r.dispfb1 : r.dispfb2, valid1 ? r.display1 : r.display2, true, c1, miss);
        if (!ok)
        {
            ++m_hiresMiss[miss];
            m_hiresShown = false;
            return;
        }
        const bool merge = valid1 && valid2;
        const bool slbg = merge && ((r.pmode >> 7) & 1u) != 0u;
        const bool mmod = ((r.pmode >> 5) & 1u) != 0u;
        const float alp = static_cast<float>((r.pmode >> 8) & 0xFFu) / 255.0f;
        const int w = merge ? std::max(c1.w, c2.w) : c1.w, h = merge ? std::max(c1.h, c2.h) : c1.h;
        const bool fieldMode = (r.smode2 & 1u) != 0u && (r.smode2 & 2u) == 0u;

        int s = 0;
        GLsync done = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_hires.mutex);
            while (s == g_hires.latest || s == g_hires.inUse)
                ++s;
            std::swap(done, g_hires.slots[s].done);
        }
        if (done)
        {
            gl.WaitSync(done, 0, GL_TIMEOUT_IGNORED);
            gl.DeleteSync(done);
        }
        HiresSlot &slot = g_hires.slots[s]; // neither latest nor in use: the presenter does not read it
        const int W = w * m_scale, H = h * m_scale;
        if (slot.w != W || slot.h != H)
        {
            if (!slot.tex)
            {
                gl.GenTextures(1, &slot.tex);
                gl.GenFramebuffers(1, &slot.fbo);
            }
            gl.BindTexture(GL_TEXTURE_2D, slot.tex);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            gl.BindFramebuffer(GL_FRAMEBUFFER, slot.fbo);
            gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, slot.tex, 0);
            slot.w = W;
            slot.h = H;
        }
        if (slot.ready)
        {
            gl.DeleteSync(slot.ready);
            slot.ready = nullptr;
        }
        gl.BindFramebuffer(GL_FRAMEBUFFER, slot.fbo);
        gl.Viewport(0, 0, W, H);
        gl.Disable(GL_BLEND);
        gl.Disable(GL_DEPTH_TEST);
        gl.Disable(GL_SCISSOR_TEST);
        gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        Program &pr = m_presentProg;
        gl.UseProgram(pr.id);
        gl.Uniform2f(pr.u("uSize"), static_cast<float>(W), static_cast<float>(H));
        gl.Uniform2i(pr.u("uOrigin1"), c1.ox * m_scale, c1.oy * m_scale);
        gl.Uniform2i(pr.u("uSize1"), c1.w * m_scale, c1.h * m_scale);
        // The circuits' offset (Sims 2: circuit 2 one line lower, blended 50%: the flicker filter) is one internal
        // line, which keeps the picture sharp; PS2X_GS_FLICKER=tv scales it to a whole GS line (blurred like the TV).
        static const bool sharpMerge = [] { const char *e = std::getenv("PS2X_GS_FLICKER"); return !(e && std::strcmp(e, "tv") == 0); }();
        const int k = sharpMerge ? 1 : m_scale;
        gl.Uniform2i(pr.u("uOrigin2"), c1.ox * m_scale + (c2.ox - c1.ox) * k, c1.oy * m_scale + (c2.oy - c1.oy) * k);
        gl.Uniform2i(pr.u("uSize2"), merge && !slbg ? c2.w * m_scale : 0, merge && !slbg ? c2.h * m_scale : 0);
        gl.Uniform1i(pr.u("uSixteen1"), c1.sixteen ? 1 : 0);
        gl.Uniform1i(pr.u("uBg2"), slbg ? 1 : 0);
        gl.Uniform3f(pr.u("uBg"), static_cast<float>(r.bgcolor & 0xFFu) / 255.0f, static_cast<float>((r.bgcolor >> 8) & 0xFFu) / 255.0f,
                     static_cast<float>((r.bgcolor >> 16) & 0xFFu) / 255.0f);
        gl.Uniform1f(pr.u("uAlp"), mmod ? alp : -1.0f);
        gl.Uniform1i(pr.u("uField"), fieldMode ? static_cast<int>(r.vsyncTick & 1u) : -1);
        gl.ActiveTexture(GL_TEXTURE0);
        gl.BindTexture(GL_TEXTURE_2D, c1.rt->tex);
        gl.ActiveTexture(GL_TEXTURE1);
        gl.BindTexture(GL_TEXTURE_2D, c2.rt ? c2.rt->tex : c1.rt->tex);
        m_quads.clear();
        pushQuad(0.0f, 0.0f, static_cast<float>(W), static_cast<float>(H));
        drawQuads();
        gl.BindTexture(GL_TEXTURE_2D, 0);
        gl.ActiveTexture(GL_TEXTURE0);
        gl.BindTexture(GL_TEXTURE_2D, 0);
        writeHiresShot(slot);
        gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
        GLsync ready = gl.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        gl.Flush();
        {
            std::lock_guard<std::mutex> lock(g_hires.mutex);
            slot.ready = ready;
            g_hires.latest = s;
            g_hires.publishedFor = served;
        }
        ++m_stats.hiresFrames;
        m_hiresShown = true;
    }

    // PS2X_GS_SHOT=<s>[,<s>...] also writes gsshot_<s>_hr.ppm: the internal-resolution picture (slot fbo bound).
    void writeHiresShot(const HiresSlot &slot)
    {
        static std::vector<double> times = []
        {
            std::vector<double> t;
            if (const char *e = std::getenv("PS2X_GS_SHOT"))
                for (const char *p = e; *p;)
                {
                    t.push_back(std::atof(p));
                    while (*p && *p != ',')
                        ++p;
                    if (*p == ',')
                        ++p;
                }
            return t;
        }();
        if (times.empty() || nowSeconds() - m_shotT0 < times.front())
            return;
        char name[64];
        std::snprintf(name, sizeof(name), "gsshot_%03d_hr.ppm", static_cast<int>(times.front()));
        times.erase(times.begin());
        std::vector<uint8_t> px(static_cast<size_t>(slot.w) * slot.h * 4u);
        gl.BindFramebuffer(GL_READ_FRAMEBUFFER, slot.fbo);
        gl.ReadBuffer(GL_COLOR_ATTACHMENT0);
        gl.ReadPixels(0, 0, slot.w, slot.h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        gl.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        if (std::FILE *f = std::fopen(name, "wb"))
        {
            std::fprintf(f, "P6 %d %d 255\n", slot.w, slot.h);
            for (size_t i = 0; i < px.size(); i += 4)
                std::fwrite(px.data() + i, 1, 3, f);
            std::fclose(f);
        }
    }

    // PS2X_GS_GPU_SYNC_PRESENT=1: read the display pages synchronously at every present (no VBlank of latency).
    static inline const bool s_syncPresent = []
    {
        const char *e = std::getenv("PS2X_GS_GPU_SYNC_PRESENT");
        return e && e[0] == '1';
    }();

    // Hand over the read issued by the previous present, then start reading the current display pages
    // into a PBO unless nothing on them changed since that read. CPU-valid pages are copied right away.
    void presentAsync()
    {
        const double t0 = nowSeconds();
        ++m_stats.presents;
        flushDraw();
        m_stats.flushMs += (nowSeconds() - t0) * 1000.0;
        timeGpuFrame();
        // With no readback left to wait on, nothing else keeps the worker from queueing frames faster than
        // the GPU draws them (seconds of lag in the 3x steam): wait for the present two frames back.
        if (GLsync &fence = m_frameFences[m_frameFenceIdx]; fence)
        {
            const double w0 = nowSeconds();
            gl.ClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
            m_stats.throttleMs += (nowSeconds() - w0) * 1000.0;
            m_auto.throttleMs += (nowSeconds() - w0) * 1000.0;
            gl.DeleteSync(fence);
        }
        m_frameFences[m_frameFenceIdx] = gl.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        m_frameFenceIdx = (m_frameFenceIdx + 1) % m_frameFences.size();
        ReadSlot &slot = m_readSlots[0];
        bool collected = false;
        // While the internal-resolution picture is what is shown, the GS-resolution read is only the
        // fallback: if the GPU has not finished the last one, keep it in flight rather than wait for the
        // GPU to drain (heavy frames at 3x stalled the worker ~750 ms a second here).
        if (slot.pending && slot.fence && m_hiresShown && (m_scale > 1 || autoScale()) &&
            gl.ClientWaitSync(slot.fence, 0, 0) == kGlTimeoutExpired)
        {
            ++m_stats.readsDeferred;
            m_stats.presentMs += (nowSeconds() - t0) * 1000.0;
            return;
        }
        if (slot.pending)
        {
            collectRead(slot);
            m_presentBufRequest = slot.request;
            collected = true;
        }
        const PageMask mask = m_presentMask;
        const bool changed = (mask & m_presentChanged).any() || (mask & ~m_presentIssued).any();
        if (changed)
        {
            const double i0 = nowSeconds();
            issueRead(slot, mask);
            m_stats.issueMs += (nowSeconds() - i0) * 1000.0;
        }
        if (!collected && !changed)
            m_presentBufRequest = m_presentRequest; // m_presentBuf already holds these pages as they are now
        m_stats.presentMs += (nowSeconds() - t0) * 1000.0;
    }
    void issueRead(ReadSlot &slot, const PageMask &mask)
    {
        const size_t pages = std::min<size_t>(kPages, m_shadowSize / kPageBytes);
        const PageMask gpu = mask & ~m_cpuValid;
        ensureVtex(gpu);
        if (!slot.pbo)
        {
            gl.GenBuffers(1, &slot.pbo);
            gl.BindBuffer(GL_PIXEL_PACK_BUFFER, slot.pbo);
            gl.BufferData(GL_PIXEL_PACK_BUFFER, static_cast<GLsizeiptr>(pages * kPageBytes), nullptr, GL_STREAM_READ);
        }
        slot.data.resize(m_shadowSize);
        for (size_t p = 0; p < pages; ++p)
            if (mask.test(p) && !gpu.test(p))
                std::memcpy(slot.data.data() + p * kPageBytes, m_shadow + p * kPageBytes, kPageBytes);
        if (!m_packTex)
        {
            gl.GenTextures(1, &m_packTex);
            gl.BindTexture(GL_TEXTURE_2D, m_packTex);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2048, kPages, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            gl.GenFramebuffers(1, &m_packFbo);
            gl.BindFramebuffer(GL_FRAMEBUFFER, m_packFbo);
            gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_packTex, 0);
        }
        gl.BindFramebuffer(GL_FRAMEBUFFER, m_packFbo);
        gl.Viewport(0, 0, 2048, kPages);
        gl.Disable(GL_BLEND);
        gl.Disable(GL_DEPTH_TEST);
        gl.Disable(GL_SCISSOR_TEST);
        gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        gl.UseProgram(m_packProg.id);
        gl.Uniform2f(m_packProg.u("uSize"), 2048.0f, static_cast<float>(kPages));
        gl.ActiveTexture(GL_TEXTURE0);
        gl.BindTexture(GL_TEXTURE_2D, m_vtex);
        m_quads.clear();
        for (size_t p = 0; p < pages;)
        {
            if (!gpu.test(p))
            {
                ++p;
                continue;
            }
            size_t e = p;
            while (e < pages && gpu.test(e))
                ++e;
            pushQuad(0.0f, static_cast<float>(p), 2048.0f, static_cast<float>(e));
            p = e;
        }
        drawQuads();
        gl.BindFramebuffer(GL_READ_FRAMEBUFFER, m_packFbo);
        gl.ReadBuffer(GL_COLOR_ATTACHMENT0);
        gl.BindBuffer(GL_PIXEL_PACK_BUFFER, slot.pbo);
        for (size_t p = 0; p < pages;)
        {
            if (!gpu.test(p))
            {
                ++p;
                continue;
            }
            size_t e = p;
            while (e < pages && gpu.test(e))
                ++e;
            gl.ReadPixels(0, static_cast<GLint>(p), 2048, static_cast<GLsizei>(e - p), GL_RGBA, GL_UNSIGNED_BYTE,
                          reinterpret_cast<void *>(p * kPageBytes));
            m_stats.presentPages += static_cast<uint64_t>(e - p);
            p = e;
        }
        gl.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        gl.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
        slot.fence = gl.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        gl.Flush();
        slot.gpuPages = gpu;
        slot.request = m_presentRequest;
        slot.pending = true;
        ++m_stats.reads;
        m_presentChanged &= ~mask;
        m_presentIssued = mask;
    }
    void collectRead(ReadSlot &slot)
    {
        if (slot.fence)
        {
            const double w0 = nowSeconds();
            gl.ClientWaitSync(slot.fence, GL_SYNC_FLUSH_COMMANDS_BIT, 100000000ull);
            m_stats.waitMs += (nowSeconds() - w0) * 1000.0;
            gl.DeleteSync(slot.fence);
            slot.fence = nullptr;
        }
        if (slot.gpuPages.any())
        {
            // Copy only the runs that were read (mapping the whole buffer costs more than the copy).
            const size_t pages = std::min<size_t>(kPages, m_shadowSize / kPageBytes);
            const double c0 = nowSeconds();
            gl.BindBuffer(GL_PIXEL_PACK_BUFFER, slot.pbo);
            for (size_t p = 0; p < pages;)
            {
                if (!slot.gpuPages.test(p))
                {
                    ++p;
                    continue;
                }
                size_t e = p;
                while (e < pages && slot.gpuPages.test(e))
                    ++e;
                gl.GetBufferSubData(GL_PIXEL_PACK_BUFFER, static_cast<GLintptr>(p * kPageBytes), static_cast<GLsizeiptr>((e - p) * kPageBytes),
                                    slot.data.data() + p * kPageBytes);
                p = e;
            }
            gl.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            m_stats.copyMs += (nowSeconds() - c0) * 1000.0;
        }
        m_presentBuf.swap(slot.data);
        ++m_presentBufGen;
        slot.pending = false;
    }

public:
    // Pages the presentation of this request can read (display buffers, the preferred copy source).
    static PageMask presentPages(const GSPresentationRequest &r)
    {
        PageMask m;
        auto add = [&](uint32_t baseBlock, uint32_t fbw, uint32_t psm, uint32_t originY)
        {
            fbw = fbw ? fbw : 10u;
            m |= rectPages(psm, baseBlock, fbw, 0, 0, static_cast<int>(fbw * 64u) - 1, static_cast<int>(std::min(originY + 512u, 2048u)) - 1);
        };
        bool zeroBase = false;
        for (uint64_t dispfb : {r.dispfb1, r.dispfb2})
        {
            const uint32_t fbp = static_cast<uint32_t>(dispfb & 0x1FFu);
            add(fbp * 32u, static_cast<uint32_t>((dispfb >> 9) & 0x3Fu), static_cast<uint32_t>((dispfb >> 15) & 0x1Fu),
                static_cast<uint32_t>((dispfb >> 43) & 0x7FFu));
            zeroBase |= fbp == 0u;
        }
        if (r.hasPreferredSource)
            add(r.preferredSource.fbp, r.preferredSource.fbw, r.preferredSource.psm, 0u);
        if (zeroBase)
            for (const GSFrameReg &f : r.contextFrames)
                add(f.fbp * 32u, f.fbw, f.psm, 0u);
        return m;
    }
};

PresentationFrame GSGpuBackend::Present(const GSPresentationRequest &request)
{
    const PageMask mask = presentPages(request);
    std::unique_lock<std::mutex> lock(m_presentMutex);
    bool ready = false;
    if (!m_threaded.load())
    {
        std::lock_guard<std::mutex> process(m_processMutex);
        if (!m_threaded.load())
        {
            copyPresentPages(mask);
            m_presentBufRequest = request;
            ready = true;
        }
    }
    if (!ready)
    {
        m_presentMask = mask;
        m_presentRequest = request;
        m_presentDone = false;
        m_presentRequested.store(true);
        m_wake.notify_one();
        ready = m_presentCv.wait_for(lock, std::chrono::milliseconds(50), [&] { return m_presentDone; });
    }
    if (!ready || m_presentBuf.empty())
        return m_lastFrame;
    // Same buffer content and same display setup as the last decode: the same frame (the decode only
    // reads the request's display registers, and the vsync tick only for the field parity).
    auto sameFrameReg = [](const GSFrameReg &a, const GSFrameReg &b)
    { return a.fbp == b.fbp && a.fbw == b.fbw && a.psm == b.psm && a.fbmsk == b.fbmsk; };
    const GSPresentationRequest &r = m_presentBufRequest, &l = m_lastFrameRequest;
    if (m_lastFrame && m_lastFrameGen == m_presentBufGen && r.pmode == l.pmode && r.smode2 == l.smode2 &&
        r.dispfb1 == l.dispfb1 && r.display1 == l.display1 && r.dispfb2 == l.dispfb2 && r.display2 == l.display2 &&
        r.bgcolor == l.bgcolor && ((r.vsyncTick ^ l.vsyncTick) & 1ull) == 0ull && sameFrameReg(r.contextFrames[0], l.contextFrames[0]) &&
        sameFrameReg(r.contextFrames[1], l.contextFrames[1]) && sameFrameReg(r.preferredSource, l.preferredSource) &&
        r.preferredDestFbp == l.preferredDestFbp && r.hasPreferredSource == l.hasPreferredSource)
        return m_lastFrame;
    thread_local GSCpuBackend snapshotBackend;
    snapshotBackend.Initialize(m_presentBuf.data(), static_cast<uint32_t>(m_presentBuf.size()));
    PresentationFrame frame = snapshotBackend.Present(m_presentBufRequest);
    m_zeroFallback.store(frame.displayFbp == 0u && frame.sourceFbp != 0u && !frame.usedPreferred, std::memory_order_relaxed);
    m_lastFrame = frame;
    m_lastFrameGen = m_presentBufGen;
    m_lastFrameRequest = m_presentBufRequest;
    return frame;
}

std::unique_ptr<GSRasterBackend> ps2xMakeGsGpuBackend()
{
    return std::make_unique<GSGpuBackend>();
}

namespace
{
    // The presenter's own context (main thread): fence entry points.
    struct MainGl
    {
        PFNGLFENCESYNCPROC FenceSync = nullptr;
        PFNGLWAITSYNCPROC WaitSync = nullptr;
        PFNGLDELETESYNCPROC DeleteSync = nullptr;
        PFNGLFLUSHPROC Flush = nullptr;
        bool loaded = false;
        bool ok = false;
    };
    MainGl g_mainGl;
}

bool ps2xGsGpuAcquireHiresFrame(unsigned &tex, int &width, int &height)
{
    if (requestedScale() <= 1 && !autoScale())
        return false;
    if (!g_mainGl.loaded)
    {
        g_mainGl.loaded = true;
        if (glfwGetCurrentContext())
        {
            g_mainGl.FenceSync = reinterpret_cast<PFNGLFENCESYNCPROC>(glfwGetProcAddress("glFenceSync"));
            g_mainGl.WaitSync = reinterpret_cast<PFNGLWAITSYNCPROC>(glfwGetProcAddress("glWaitSync"));
            g_mainGl.DeleteSync = reinterpret_cast<PFNGLDELETESYNCPROC>(glfwGetProcAddress("glDeleteSync"));
            g_mainGl.Flush = reinterpret_cast<PFNGLFLUSHPROC>(glfwGetProcAddress("glFlush"));
            g_mainGl.ok = g_mainGl.FenceSync && g_mainGl.WaitSync && g_mainGl.DeleteSync && g_mainGl.Flush;
        }
    }
    if (!g_mainGl.ok)
        return false;
    std::lock_guard<std::mutex> lock(g_hires.mutex);
    if (g_hires.latest < 0 || g_hires.publishedFor != g_hires.served)
        return false;
    HiresSlot &slot = g_hires.slots[static_cast<size_t>(g_hires.latest)];
    if (slot.ready)
        g_mainGl.WaitSync(slot.ready, 0, GL_TIMEOUT_IGNORED);
    g_hires.inUse = g_hires.latest;
    tex = slot.tex;
    width = slot.w;
    height = slot.h;
    return true;
}

void ps2xGsGpuReleaseHiresFrame()
{
    if (!g_mainGl.ok)
        return;
    std::lock_guard<std::mutex> lock(g_hires.mutex);
    if (g_hires.inUse < 0)
        return;
    HiresSlot &slot = g_hires.slots[static_cast<size_t>(g_hires.inUse)];
    if (slot.done)
        g_mainGl.DeleteSync(slot.done);
    slot.done = g_mainGl.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    g_mainGl.Flush();
    g_hires.inUse = -1;
}

bool ps2xGsGpuWaitReady(int timeoutMs, std::string *reason)
{
    std::unique_lock<std::mutex> lock(g_glReady.mutex);
    if (!g_glReady.cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [] { return g_glReady.state >= 0; }))
    {
        if (reason)
            *reason = "GL setup timed out";
        return false;
    }
    if (reason)
        *reason = g_glReady.reason;
    return g_glReady.state == 1;
}

void ps2xGsGpuHostInit(bool standalone)
{
    std::lock_guard<std::mutex> lock(g_host.mutex);
    if (g_host.ready)
        return;
    GLFWwindow *share = nullptr;
    bool ok = true;
    if (standalone)
        ok = glfwInit() == GLFW_TRUE;
    else
        share = glfwGetCurrentContext();
    GLFWwindow *window = nullptr;
    if (ok)
    {
        glfwDefaultWindowHints();
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
        window = glfwCreateWindow(16, 16, "S2Recomp GS", nullptr, share);
        glfwDefaultWindowHints();
    }
    if (!window)
        std::fprintf(stderr, "[gsgpu] could not create a GL 3.3 context: the GS draws on the CPU\n");
    g_host.window = window;
    g_host.ready = true;
    g_host.failed = window == nullptr;
    if (GSGpuBackend *b = g_instance.load())
        b->startWorkerLocked();
}

void ps2xGsGpuHostShutdown()
{
    if (GSGpuBackend *b = g_instance.load())
        b->stopWorker();
    std::lock_guard<std::mutex> lock(g_host.mutex);
    if (g_host.window)
        glfwDestroyWindow(g_host.window);
    g_host.window = nullptr;
}

#endif
