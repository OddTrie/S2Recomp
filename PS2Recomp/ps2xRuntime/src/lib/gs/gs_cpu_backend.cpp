#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/ps2_host_cpu.h"
#include "runtime/gs/ps2_gs_common.h"
#include "runtime/gs/ps2_gs_psmct16.h"
#include "runtime/gs/ps2_gs_psmct32.h"
#include "runtime/gs/ps2_gs_psmt4.h"
#include "runtime/gs/ps2_gs_psmt8.h"
#include "runtime/gs/ps2_gs_memory.h"
#include "ps2_log.h"
#include <atomic>
#include <climits>
#include <limits>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>
#if defined(__AVX2__)
#include <immintrin.h>
#endif
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>

#if defined(_MSC_VER)
#define GS_NOINLINE __declspec(noinline)
#else
#define GS_NOINLINE __attribute__((noinline))
#endif

using namespace GSInternal;

namespace
{
    float fabsQ(float q)
    {
        return (std::fabs(q) > 1.0e-8f) ? q : 1.0f;
    }

    u16 Rgba8888ToRgba5551(u32 c)
    {
        uint32_t r = ((c >> 0) & 0xFF) >> 3;
        uint32_t g = ((c >> 8) & 0xFF) >> 3;
        uint32_t b = ((c >> 16) & 0xFF) >> 3;
        uint32_t a = ((c >> 24) & 0xFF) >> 7;

        return (r | (g << 5) | (b << 10) | (a << 15));
    }

    u32 Rgba5551ToRgba8888(u16 c)
    {
        u32 r = ((c >> 0) & 0x1F) << 3;
        u32 g = ((c >> 5) & 0x1F) << 3;
        u32 b = ((c >> 10) & 0x1F) << 3;
        u32 a = ((c >> 15) & 0x01) << 7;

        return (r | (g << 8) | (b << 16) | (a << 24));
    }

    u32 pack32(u8 r, u8 g, u8 b, u8 a)
    {
        return static_cast<u32>(r) | (g << 8) | (b << 16) | (a << 24);
    }

    uint32_t applyTexa(const GSTexaReg &texa, uint8_t psm, uint32_t texel)
    {
        if (psm == GS_PSM_CT32)
            return texel;

        const uint8_t r = static_cast<uint8_t>(texel & 0xFFu);
        const uint8_t g = static_cast<uint8_t>((texel >> 8) & 0xFFu);
        const uint8_t b = static_cast<uint8_t>((texel >> 16) & 0xFFu);
        const bool rgbZero = r == 0u && g == 0u && b == 0u;
        uint8_t a = static_cast<uint8_t>((texel >> 24) & 0xFFu);

        switch (psm)
        {
        case GS_PSM_CT24:
            a = (texa.aem && rgbZero) ? 0u : texa.ta0;
            break;
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
            if ((a & 0x80u) != 0u)
                a = texa.ta1;
            else
                a = (texa.aem && rgbZero) ? 0u : texa.ta0;
            break;
        default:
            break;
        }

        return (texel & 0x00FFFFFFu) | (static_cast<uint32_t>(a) << 24);
    }

    uint32_t addrPSMCT16Family(uint32_t basePtr, uint32_t width, uint8_t psm, uint32_t x, uint32_t y)
    {
        switch (psm)
        {
        case GS_PSM_CT16:
            return GSPSMCT16::addrPSMCT16(basePtr, width, x, y);
        case GS_PSM_CT16S:
            return GSPSMCT16::addrPSMCT16S(basePtr, width, x, y);
        case GS_PSM_Z16:
            return GSPSMCT16::addrPSMZ16(basePtr, width, x, y);
        case GS_PSM_Z16S:
            return GSPSMCT16::addrPSMZ16S(basePtr, width, x, y);
        default:
            return 0u;
        }
    }

    std::atomic<uint32_t> s_debugPrimitiveCount{0};
    std::atomic<uint32_t> s_debugPixelCount{0};
    std::atomic<uint32_t> s_debugContext1PrimitiveCount{0};
    std::atomic<uint32_t> s_debugFbp150PixelCount{0};

    int wrapTextureCoordinate(int coordinate,
                              int textureSize,
                              uint8_t mode,
                              uint16_t regionMin,
                              uint16_t regionMax)
    {
        switch (mode & 0x3u)
        {
        case 0: // REPEAT
            return static_cast<int>(static_cast<uint32_t>(coordinate) & static_cast<uint32_t>(textureSize - 1));
        case 1: // CLAMP
            return clampInt(coordinate, 0, textureSize - 1);
        case 2: // REGION_CLAMP
            return std::min(std::max(coordinate, static_cast<int>(regionMin)), static_cast<int>(regionMax));
        case 3: // REGION_REPEAT
            return static_cast<int>((static_cast<uint32_t>(coordinate) & static_cast<uint32_t>(regionMin)) | static_cast<uint32_t>(regionMax));
        default:
            return coordinate;
        }
    }

    bool passesAlphaTest(uint64_t testReg, uint8_t alpha)
    {
        if ((testReg & 0x1u) == 0u)
            return true;

        const uint8_t atst = static_cast<uint8_t>((testReg >> 1) & 0x7u);
        const uint8_t aref = static_cast<uint8_t>((testReg >> 4) & 0xFFu);

        switch (atst)
        {
        case 0:
            return false;
        case 1:
            return true;
        case 2:
            return alpha < aref;
        case 3:
            return alpha <= aref;
        case 4:
            return alpha == aref;
        case 5:
            return alpha >= aref;
        case 6:
            return alpha > aref;
        case 7:
            return alpha != aref;
        default:
            return true;
        }
    }

    struct PixelWriteMask
    {
        bool writeRgb = true;
        bool writeAlpha = true;
        bool writeDepth = true;

        bool writesFramebuffer() const
        {
            return writeRgb || writeAlpha;
        }

        bool writesAnything() const
        {
            return writesFramebuffer() || writeDepth;
        }
    };

    PixelWriteMask classifyAlphaTest(uint64_t testReg, uint8_t alpha, uint8_t framePsm)
    {
        const bool pass = passesAlphaTest(testReg, alpha);
        if (pass)
            return {};

        // TEST.AFAIL controls what happens when the alpha comparison fails.
        switch (static_cast<uint8_t>((testReg >> 12) & 0x3u))
        {
        case 1: // FB_ONLY
            return {true, true, false};
        case 2: // ZB_ONLY
            return {false, false, true};
        case 3: // RGB_ONLY
            // RGB_ONLY is only distinct for RGBA32. The GS treats it as
            // FB_ONLY for RGB24 and RGBA16 framebuffers.
            if (framePsm == GS_PSM_CT32)
                return {true, false, false};
            return {true, true, false};
        case 0: // KEEP
        default:
            return {false, false, false};
        }
    }

    bool passesDestinationAlphaTest(uint64_t testReg, uint8_t framePsm, uint32_t rawFramebufferPixel)
    {
        const bool date = ((testReg >> 14) & 0x1u) != 0u;
        if (!date)
            return true;

        const bool datm = ((testReg >> 15) & 0x1u) != 0u;
        switch (framePsm)
        {
        case GS_PSM_CT32:
            return (((rawFramebufferPixel >> 31) & 0x1u) != 0u) == datm;
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
            return (((rawFramebufferPixel >> 15) & 0x1u) != 0u) == datm;
        case GS_PSM_CT24:
            // RGB24 has no destination alpha, so DATE always passes.
            return true;
        default:
            return true;
        }
    }

    struct TextureCombineResult
    {
        uint8_t r;
        uint8_t g;
        uint8_t b;
        uint8_t a;
    };

#if defined(_MSC_VER)
    __forceinline
#else
    inline __attribute__((always_inline))
#endif
    TextureCombineResult combineTexture(const GSTex0Reg &tex,
                                        uint8_t vr,
                                        uint8_t vg,
                                        uint8_t vb,
                                        uint8_t va,
                                        uint8_t tr,
                                        uint8_t tg,
                                        uint8_t tb,
                                        uint8_t ta)
    {
        const bool textureHasAlpha = tex.tcc != 0u;
        TextureCombineResult out{tr, tg, tb, textureHasAlpha ? ta : va};

        switch (tex.tfx)
        {
        case 0: // MODULATE
            out.r = clampU8((tr * vr) >> 7);
            out.g = clampU8((tg * vg) >> 7);
            out.b = clampU8((tb * vb) >> 7);
            out.a = textureHasAlpha ? clampU8((ta * va) >> 7) : va;
            break;
        case 1: // DECAL
            out.r = tr;
            out.g = tg;
            out.b = tb;
            out.a = textureHasAlpha ? ta : va;
            break;
        case 2: // HIGHLIGHT
            out.r = clampU8(((tr * vr) >> 7) + va);
            out.g = clampU8(((tg * vg) >> 7) + va);
            out.b = clampU8(((tb * vb) >> 7) + va);
            out.a = textureHasAlpha ? clampU8(ta + va) : va;
            break;
        case 3: // HIGHLIGHT2
            out.r = clampU8(((tr * vr) >> 7) + va);
            out.g = clampU8(((tg * vg) >> 7) + va);
            out.b = clampU8(((tb * vb) >> 7) + va);
            out.a = textureHasAlpha ? ta : va;
            break;
        default:
            out.r = tr;
            out.g = tg;
            out.b = tb;
            out.a = textureHasAlpha ? ta : va;
            break;
        }

        return out;
    }

    // combineTexture with the result packed as r | g << 8 | b << 16 | a << 24 in one register.
    // DrawTriangle used the struct form, which MSVC stored byte by byte and reloaded as a dword
    // (a store-forwarding stall on every textured pixel). Same arithmetic.
#if defined(_MSC_VER)
    __forceinline
#else
    inline __attribute__((always_inline))
#endif
    uint32_t combineTexturePacked(const GSTex0Reg &tex, uint32_t vr, uint32_t vg, uint32_t vb, uint32_t va,
                                  uint32_t tr, uint32_t tg, uint32_t tb, uint32_t ta)
    {
        const bool textureHasAlpha = tex.tcc != 0u;
        auto sat = [](uint32_t x) { return x > 255u ? 255u : x; };
        uint32_t r, g, b, a;
        switch (tex.tfx)
        {
        case 0: // MODULATE
            r = sat((tr * vr) >> 7);
            g = sat((tg * vg) >> 7);
            b = sat((tb * vb) >> 7);
            a = textureHasAlpha ? sat((ta * va) >> 7) : va;
            break;
        case 2: // HIGHLIGHT
            r = sat(((tr * vr) >> 7) + va);
            g = sat(((tg * vg) >> 7) + va);
            b = sat(((tb * vb) >> 7) + va);
            a = textureHasAlpha ? sat(ta + va) : va;
            break;
        case 3: // HIGHLIGHT2
            r = sat(((tr * vr) >> 7) + va);
            g = sat(((tg * vg) >> 7) + va);
            b = sat(((tb * vb) >> 7) + va);
            a = textureHasAlpha ? ta : va;
            break;
        default: // DECAL
            r = tr;
            g = tg;
            b = tb;
            a = textureHasAlpha ? ta : va;
            break;
        }
        return r | (g << 8) | (b << 16) | (a << 24);
    }

    uint32_t swizzleClutIndexCSM1(uint32_t index)
    {
        // CSM1 swaps address bits 3 and 4. Preserve the remaining bits:
        // 16-bit CLUTs expose a ninth address bit through CSA[4].
        return (index & ~0x18u) | ((index & 0x08u) << 1u) | ((index & 0x10u) >> 1u);
    }

    bool isFourBitIndexedPsm(uint8_t psm)
    {
        return psm == GS_PSM_T4 || psm == GS_PSM_T4HL || psm == GS_PSM_T4HH;
    }

    bool isEightBitIndexedPsm(uint8_t psm)
    {
        return psm == GS_PSM_T8 || psm == GS_PSM_T8H;
    }

    uint8_t lerpChannel(uint8_t c00, uint8_t c10, uint8_t c01, uint8_t c11, float fx, float fy)
    {
        const float top = static_cast<float>(c00) + (static_cast<float>(c10) - static_cast<float>(c00)) * fx;
        const float bottom = static_cast<float>(c01) + (static_cast<float>(c11) - static_cast<float>(c01)) * fx;
        // Interpolated value is within [0, 255]; +0.5 truncation == lround there, without the libm call.
        const float value = top + (bottom - top) * fy;
        return clampU8(value <= 0.0f ? 0 : static_cast<int>(value + 0.5f));
    }

    // lerpChannel for the four bytes of RGBA texels at once. The Release build (/arch:AVX2 /fp:fast)
    // compiles lerpChannel to top = fma(c10 - c00, fx, c00), bottom = fma(c11 - c01, fx, c01),
    // value = fma(bottom - top, fy, top), then +0.5, truncate, clamp: the same operations here, so the
    // result is identical (checked with --gs-replay hashes). Other builds use lerpChannel itself.
    inline uint32_t lerpTexel(uint32_t c00, uint32_t c10, uint32_t c01, uint32_t c11, float fx, float fy)
    {
#if defined(__AVX2__)
        const auto widen = [](uint32_t c) { return _mm_cvtepi32_ps(_mm_cvtepu8_epi32(_mm_cvtsi32_si128(static_cast<int>(c)))); };
        const __m128 a00 = widen(c00), a10 = widen(c10), a01 = widen(c01), a11 = widen(c11);
        const __m128 vfx = _mm_set1_ps(fx);
        const __m128 top = _mm_fmadd_ps(_mm_sub_ps(a10, a00), vfx, a00);
        const __m128 bottom = _mm_fmadd_ps(_mm_sub_ps(a11, a01), vfx, a01);
        const __m128 value = _mm_fmadd_ps(_mm_sub_ps(bottom, top), _mm_set1_ps(fy), top);
        __m128i out = _mm_cvttps_epi32(_mm_add_ps(value, _mm_set1_ps(0.5f)));
        out = _mm_and_si128(out, _mm_castps_si128(_mm_cmpgt_ps(value, _mm_setzero_ps()))); // value <= 0 -> 0
        out = _mm_min_epi32(out, _mm_set1_epi32(255));
        out = _mm_packus_epi32(out, out);
        out = _mm_packus_epi16(out, out);
        return static_cast<uint32_t>(_mm_cvtsi128_si32(out));
#else
        return static_cast<uint32_t>(lerpChannel(c00 & 0xFFu, c10 & 0xFFu, c01 & 0xFFu, c11 & 0xFFu, fx, fy)) |
               (static_cast<uint32_t>(lerpChannel((c00 >> 8) & 0xFFu, (c10 >> 8) & 0xFFu, (c01 >> 8) & 0xFFu, (c11 >> 8) & 0xFFu, fx, fy)) << 8) |
               (static_cast<uint32_t>(lerpChannel((c00 >> 16) & 0xFFu, (c10 >> 16) & 0xFFu, (c01 >> 16) & 0xFFu, (c11 >> 16) & 0xFFu, fx, fy)) << 16) |
               (static_cast<uint32_t>(lerpChannel((c00 >> 24) & 0xFFu, (c10 >> 24) & 0xFFu, (c01 >> 24) & 0xFFu, (c11 >> 24) & 0xFFu, fx, fy)) << 24);
#endif
    }
}

namespace
{
    static constexpr uint32_t kDefaultDisplayWidth = 640u;
    static constexpr uint32_t kDefaultDisplayHeight = 448u;
    static constexpr uint32_t kHostFrameWidth = 640u;
    static constexpr uint32_t kHostFrameHeight = 512u;

    uint16_t encodeFramePixelPSMCT16(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
    {
        return static_cast<uint16_t>(((r >> 3) & 0x1Fu) |
                                     (((g >> 3) & 0x1Fu) << 5) |
                                     (((b >> 3) & 0x1Fu) << 10) |
                                     ((a >= 0x40u) ? 0x8000u : 0u));
    }

    void decodeDisplaySize(uint64_t display64, uint32_t &outWidth, uint32_t &outHeight)
    {
        const uint32_t dw = static_cast<uint32_t>((display64 >> 32) & 0x0FFFu);
        const uint32_t dh = static_cast<uint32_t>((display64 >> 44) & 0x07FFu);
        const uint32_t magh = static_cast<uint32_t>((display64 >> 23) & 0x0Fu);

        outWidth = (dw + 1u) / (magh + 1u);
        outHeight = dh + 1u;
        if (outWidth < 64u || outHeight < 64u)
        {
            outWidth = kDefaultDisplayWidth;
            outHeight = kDefaultDisplayHeight;
        }
        outWidth = std::min<uint32_t>(outWidth, kHostFrameWidth);
        outHeight = std::min<uint32_t>(outHeight, kHostFrameHeight);
    }

    GSFrameReg decodeDisplayFrame(uint64_t dispfb64)
    {
        GSFrameReg frame{};
        frame.fbp = static_cast<uint32_t>(dispfb64 & 0x1FFu);
        frame.fbw = static_cast<uint32_t>((dispfb64 >> 9) & 0x3Fu);
        frame.psm = static_cast<uint8_t>((dispfb64 >> 15) & 0x1Fu);
        return frame;
    }

    struct GSDisplayReadOrigin
    {
        uint32_t x = 0u;
        uint32_t y = 0u;
    };

    GSDisplayReadOrigin decodeDisplayReadOrigin(uint64_t dispfb64)
    {
        return {
            static_cast<uint32_t>((dispfb64 >> 32) & 0x7FFu),
            static_cast<uint32_t>((dispfb64 >> 43) & 0x7FFu)};
    }

    bool hasDisplaySetup(uint64_t display64, const GSFrameReg &frame)
    {
        const uint32_t dw = static_cast<uint32_t>((display64 >> 32) & 0x0FFFu);
        const uint32_t dh = static_cast<uint32_t>((display64 >> 44) & 0x07FFu);
        const uint32_t magh = static_cast<uint32_t>((display64 >> 23) & 0x0Fu);
        return frame.fbw != 0u || dw != 0u || dh != 0u || magh != 0u;
    }

    struct GSPmodeState
    {
        bool enableCrt1 = false;
        bool enableCrt2 = false;
        bool mmod = false;
        bool amod = false;
        bool slbg = false;
        uint8_t alp = 0u;
    };

    GSPmodeState decodePmode(uint64_t pmode64)
    {
        return {
            (pmode64 & 0x1ull) != 0ull,
            (pmode64 & 0x2ull) != 0ull,
            ((pmode64 >> 5) & 0x1ull) != 0ull,
            ((pmode64 >> 6) & 0x1ull) != 0ull,
            ((pmode64 >> 7) & 0x1ull) != 0ull,
            static_cast<uint8_t>((pmode64 >> 8) & 0xFFu)};
    }

    struct GSSmode2State
    {
        bool interlaced = false;
        bool frameMode = true;
    };

    GSSmode2State decodeSMode2(uint64_t smode2)
    {
        return {(smode2 & 0x1ull) != 0ull, ((smode2 >> 1) & 0x1ull) != 0ull};
    }

    void applyFieldPresentation(std::vector<uint8_t> &pixels, uint32_t width, uint32_t height, bool oddField)
    {
        if (pixels.empty() || width == 0u || height < 2u)
            return;
        const std::vector<uint8_t> source = pixels;
        for (uint32_t y = 0; y < height; ++y)
        {
            uint32_t sourceY = ((y >> 1u) << 1u) + (oddField ? 1u : 0u);
            if (sourceY >= height)
                sourceY = height - 1u;
            std::memcpy(pixels.data() + y * kHostFrameWidth * 4u,
                        source.data() + sourceY * kHostFrameWidth * 4u,
                        width * 4u);
        }
    }

    void normalizePresentationAlpha(std::vector<uint8_t> &pixels, uint32_t width, uint32_t height)
    {
        for (uint32_t y = 0; y < height; ++y)
        {
            uint8_t *row = pixels.data() + y * kHostFrameWidth * 4u;
            for (uint32_t x = 0; x < width; ++x)
                row[x * 4u + 3u] = 255u;
        }
    }

    uint8_t blendPresentationChannel(uint8_t src, uint8_t dst, uint32_t factor)
    {
        const int delta = static_cast<int>(src) - static_cast<int>(dst);
        return GSInternal::clampU8(static_cast<int>(dst) + ((delta * static_cast<int>(factor)) / 255));
    }

    uint32_t countNonBlackPixels(const std::vector<uint8_t> &pixels, uint32_t width, uint32_t height)
    {
        uint32_t count = 0u;
        for (uint32_t y = 0; y < height; ++y)
        {
            const uint8_t *row = pixels.data() + y * kHostFrameWidth * 4u;
            for (uint32_t x = 0; x < width; ++x)
            {
                if (row[x * 4u] != 0u || row[x * 4u + 1u] != 0u || row[x * 4u + 2u] != 0u)
                    ++count;
            }
        }
        return count;
    }
}

GSCpuBackend::GSCpuBackend()
{
    using namespace GSMem;
    static std::once_flag lookupTablesOnce;
    std::call_once(lookupTablesOnce, []()
                   { InitLookupTables(); });
    for (size_t i = 0; i < kPsmHandlerCount; ++i)
    {
        switch (i)
        {
        case GS_PSM_CT32:
            m_readVramFuncs[i] = ReadCT32;
            m_writeVramFuncs[i] = WriteCT32;
            break;
        case GS_PSM_CT24:
            m_readVramFuncs[i] = ReadCT24;
            m_writeVramFuncs[i] = WriteCT24;
            break;
        case GS_PSM_CT16:
            m_readVramFuncs[i] = ReadCT16;
            m_writeVramFuncs[i] = WriteCT16;
            break;
        case GS_PSM_CT16S:
            m_readVramFuncs[i] = ReadCT16S;
            m_writeVramFuncs[i] = WriteCT16S;
            break;
        case GS_PSM_T8:
            m_readVramFuncs[i] = ReadP8;
            m_writeVramFuncs[i] = WriteP8;
            break;
        case GS_PSM_T8H:
            m_readVramFuncs[i] = ReadP8H;
            m_writeVramFuncs[i] = WriteP8H;
            break;
        case GS_PSM_T4:
            m_readVramFuncs[i] = ReadP4;
            m_writeVramFuncs[i] = WriteP4;
            break;
        case GS_PSM_T4HH:
            m_readVramFuncs[i] = ReadP4HH;
            m_writeVramFuncs[i] = WriteP4HH;
            break;
        case GS_PSM_T4HL:
            m_readVramFuncs[i] = ReadP4HL;
            m_writeVramFuncs[i] = WriteP4HL;
            break;
        case GS_PSM_Z32:
            m_readVramFuncs[i] = ReadZ32;
            m_writeVramFuncs[i] = WriteZ32;
            break;
        case GS_PSM_Z24:
            m_readVramFuncs[i] = ReadZ24;
            m_writeVramFuncs[i] = WriteZ24;
            break;
        case GS_PSM_Z16:
            m_readVramFuncs[i] = ReadZ16;
            m_writeVramFuncs[i] = WriteZ16;
            break;
        case GS_PSM_Z16S:
            m_readVramFuncs[i] = ReadZ16S;
            m_writeVramFuncs[i] = WriteZ16S;
            break;
        default:
            m_readVramFuncs[i] = ReadNull;
            m_writeVramFuncs[i] = WriteNull;
            break;
        }
    }
    Reset();
}

void GSCpuBackend::Initialize(uint8_t *vram, uint32_t vramSize)
{
    if (vram && vramSize < GSMem::MEMORY_SIZE)
        throw std::invalid_argument("GS CPU backend requires at least 4 MiB of VRAM");

    std::lock_guard<std::mutex> lock(m_mutex);
    m_vram = vram;
    m_vramSize = vramSize;
    ResetUnlocked();
}

void GSCpuBackend::SetBand(int index, int count)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_bandCount = count > 1 ? count : 1;
    m_bandIndex = m_bandCount > 1 ? index % m_bandCount : 0;
}

void GSCpuBackend::Reset()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    ResetUnlocked();
}

void GSCpuBackend::ResetUnlocked()
{
    m_clut.fill(0u);
    m_clutCbp.fill(0u);
    m_texturePageCache.Invalidate();
    InvalidateDecodedTexturesUnlocked();
    m_transfer = {};
    m_transfer.direction = 3u;
    m_transferState = {};
    m_transferState.direction = 3u;
    m_localToHostBuffer.clear();
    m_localToHostReadPos = 0u;
}

std::atomic<uint64_t> g_gsDrawNanos{0}; // debug: host time spent rasterizing (read by PS2X_VU_STATS)

void GSCpuBackend::Submit(const GSPrimitiveBatch &batch)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_vram || batch.vertexCount == 0u)
        return;
    // The legacy page-copy texture cache (PS2X_GS_TEXCACHE=1) lives for one command only: between
    // commands its page may be rewritten by this instance, another band or a transfer.
    m_texturePageCache.Invalidate();
    if (m_hasDecodedTextures)
        InvalidateDecodedTexturesWrittenBy(batch);
    static const bool timed = [] { const char *e = std::getenv("PS2X_VU_STATS"); return e && *e && *e != '0'; }();
    if (!timed)
    {
        DrawPrimitive(batch);
        return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    DrawPrimitive(batch);
    g_gsDrawNanos.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()), std::memory_order_relaxed);
}

void GSCpuBackend::LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_vram || (!isFourBitIndexedPsm(tex0.psm) && !isEightBitIndexedPsm(tex0.psm)))
        return;

    switch (tex0.cld)
    {
    case 0u:
    case 6u:
    case 7u:
        return;
    case 1u:
        break;
    case 2u:
        m_clutCbp[0] = tex0.cbp;
        break;
    case 3u:
        m_clutCbp[1] = tex0.cbp;
        break;
    case 4u:
        if (m_clutCbp[0] == tex0.cbp)
            return;
        m_clutCbp[0] = tex0.cbp;
        break;
    case 5u:
        if (m_clutCbp[1] == tex0.cbp)
            return;
        m_clutCbp[1] = tex0.cbp;
        break;
    default:
        return;
    }

    m_texturePageCache.Invalidate(); // see Submit
    LoadClutUnlocked(tex0, texclut);
}

void GSCpuBackend::LoadClutUnlocked(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
{
    const bool fourBit = isFourBitIndexedPsm(tex0.psm);
    const bool sixteenBit = tex0.cpsm == GS_PSM_CT16 || tex0.cpsm == GS_PSM_CT16S;
    const bool thirtyTwoBit = tex0.cpsm == GS_PSM_CT32 || tex0.cpsm == GS_PSM_CT24;
    if (!sixteenBit && !thirtyTwoBit)
        return;

    const uint32_t entryCount = fourBit ? 16u : 256u;
    const uint32_t csaMask = sixteenBit ? 0x1Fu : 0x0Fu;
    const uint32_t destinationBase = (static_cast<uint32_t>(tex0.csa) & csaMask) << 4u;

    const bool loadCsm1Suffix = tex0.csm == 0u && thirtyTwoBit && !fourBit;
    const uint32_t firstEntry = loadCsm1Suffix ? destinationBase : 0u;
    const std::array<uint16_t, 512> previousClut = m_clut;

    for (uint32_t entry = firstEntry; entry < entryCount; ++entry)
    {
        uint32_t sourceX = 0u;
        uint32_t sourceY = 0u;
        uint32_t sourceWidth = 1u;

        if (tex0.csm == 0u)
        {
            const uint32_t sourceIndex = swizzleClutIndexCSM1(entry);
            sourceX = sourceIndex & 0x0Fu;
            sourceY = sourceIndex >> 4u;
        }
        else
        {
            sourceWidth = texclut.cbw != 0u ? static_cast<uint32_t>(texclut.cbw) : 1u;
            sourceX = (static_cast<uint32_t>(texclut.cou) << 4u) + entry;
            sourceY = static_cast<uint32_t>(texclut.cov);
        }

        const uint32_t raw = ReadTextureVramUnlocked(tex0.cpsm, tex0.cbp, sourceWidth, sourceX, sourceY); 
        const uint32_t destination = (loadCsm1Suffix ? entry : destinationBase + entry) & (sixteenBit ? 0x1FFu : 0x0FFu);
        if (sixteenBit)
        {
            m_clut[destination] = static_cast<uint16_t>(raw);
        }
        else
        {
            m_clut[destination] = static_cast<uint16_t>(raw & 0xFFFFu);
            m_clut[destination + 256u] = static_cast<uint16_t>(raw >> 16u);
        }
    }
    if (m_hasDecodedTextures && m_clut != previousClut)
        InvalidateDecodedTexturesUnlocked();
}

void GSCpuBackend::Flush()
{
    // CPU backend is immediate. GPU backends may submit command buffers here.
}

void GSCpuBackend::TextureFlush()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_texturePageCache.Invalidate();
}

void GSCpuBackend::Sync(GSSyncReason)
{
    // CPU backend is immediate. GPU backends may wait on fences/readbacks here.
}

uint32_t GSCpuBackend::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return ReadVramUnlocked(psm, base, bw, x, y);
}

uint32_t GSCpuBackend::ReadVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    if (!m_vram)
        return 0u;
    return m_readVramFuncs[psm & 0x3Fu](m_vram, base, bw, x, y);
}

uint32_t GSCpuBackend::ReadTextureVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y)
{
    if (!m_vram)
        return 0u;

    // The single-page "texture cache" copied a whole 8 KB page on every page change. Bilinear
    // sampling and wide textures alternate between pages on almost every texel, so most of the
    // GS time went into memcpy. Read VRAM directly (always current data, like a software GS).
    // PS2X_GS_TEXCACHE=1 restores the old page-copy behaviour for comparison.
    static const bool usePageCache = [] { const char *e = std::getenv("PS2X_GS_TEXCACHE"); return e && *e && *e != '0'; }();
    if (usePageCache)
        return GSMem::ReadTexture(m_texturePageCache, m_vram, psm, base, bw, x, y);
    return m_readVramFuncs[psm & 0x3Fu](m_vram, base, bw, x, y);
}

void GSCpuBackend::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    WriteVramUnlocked(psm, base, bw, x, y, value);
}

void GSCpuBackend::WriteVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    InvalidateDecodedTexturesUnlocked();
    if (!m_vram)
        return;
    m_writeVramFuncs[psm & 0x3Fu](m_vram, base, bw, x, y, value);
}

void GSCpuBackend::SnapshotVram(std::vector<uint8_t> &out) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_vram || m_vramSize == 0u)
    {
        out.clear();
        return;
    }
    out.resize(m_vramSize);
    std::memcpy(out.data(), m_vram, m_vramSize);
}

GSTransferSnapshot GSCpuBackend::GetTransferSnapshot() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    GSTransferSnapshot result = m_transferState;
    result.localToHostPendingBytes = m_localToHostReadPos < m_localToHostBuffer.size()
                                         ? m_localToHostBuffer.size() - m_localToHostReadPos
                                         : 0u;
    return result;
}

void GSCpuBackend::DrawPrimitive(const GSPrimitiveBatch &batchIn)
{
    // PS2X_HACK_ALPHA=1 (experiment): primitives whose vertices all have alpha 0 get alpha 0x80.
    static const bool hackAlpha = [] { const char *e = std::getenv("PS2X_HACK_ALPHA"); return e && *e && *e != '0'; }();
    GSPrimitiveBatch hacked;
    const GSPrimitiveBatch *bp = &batchIn;
    if (hackAlpha && batchIn.vertexCount > 0)
    {
        bool allZero = true;
        for (uint32_t i = 0; i < batchIn.vertexCount && i < 3; ++i)
            if (batchIn.vertices[i].a != 0) allZero = false;
        if (allZero)
        {
            hacked = batchIn;
            for (uint32_t i = 0; i < hacked.vertexCount && i < 3; ++i)
                hacked.vertices[i].a = 0x80;
            bp = &hacked;
        }
    }
    const GSPrimitiveBatch &batch = *bp;
    const GSDrawState &state = batch.state;
    const auto &ctx = state.context;
    PS2_IF_AGRESSIVE_LOGS({
        const uint32_t primitiveIndex = s_debugPrimitiveCount.fetch_add(1u, std::memory_order_relaxed);
        if (primitiveIndex < 64u)
        {
            std::cout << "[gs:prim] idx=" << primitiveIndex
                      << " type=" << static_cast<uint32_t>(state.prim.type)
                      << " tme=" << static_cast<uint32_t>(state.prim.tme)
                      << " abe=" << static_cast<uint32_t>(state.prim.abe)
                      << " fst=" << static_cast<uint32_t>(state.prim.fst)
                      << " ctxt=" << static_cast<uint32_t>(state.prim.ctxt)
                      << " fbp=" << ctx.frame.fbp
                      << " fbw=" << ctx.frame.fbw
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.frame.psm) << std::dec
                      << " tex0=("
                      << "tbp0=" << ctx.tex0.tbp0
                      << " tbw=" << static_cast<uint32_t>(ctx.tex0.tbw)
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.psm) << std::dec
                      << " tw=" << static_cast<uint32_t>(ctx.tex0.tw)
                      << " th=" << static_cast<uint32_t>(ctx.tex0.th)
                      << " tcc=" << static_cast<uint32_t>(ctx.tex0.tcc)
                      << " tfx=" << static_cast<uint32_t>(ctx.tex0.tfx)
                      << " cbp=" << ctx.tex0.cbp
                      << " cpsm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.cpsm) << std::dec
                      << " csm=" << static_cast<uint32_t>(ctx.tex0.csm)
                      << " csa=" << static_cast<uint32_t>(ctx.tex0.csa)
                      << ")"
                      << " texclut=("
                      << "cbw=" << static_cast<uint32_t>(state.texclut.cbw)
                      << " cou=" << static_cast<uint32_t>(state.texclut.cou)
                      << " cov=" << state.texclut.cov
                      << ")"
                      << " ofx=" << (ctx.xyoffset.ofx >> 4)
                      << " ofy=" << (ctx.xyoffset.ofy >> 4)
                      << " scissor=(" << ctx.scissor.x0
                      << "," << ctx.scissor.y0
                      << ")-(" << ctx.scissor.x1
                      << "," << ctx.scissor.y1 << ")"
                      << " test=0x" << std::hex << ctx.test
                      << " alpha=0x" << ctx.alpha
                      << std::dec
                      << " v0=(" << batch.vertices[0].x << "," << batch.vertices[0].y << ")"
                      << " uv0=(" << (batch.vertices[0].u >> 4) << "," << (batch.vertices[0].v >> 4) << ")"
                      << " stq0=(" << batch.vertices[0].s << "," << batch.vertices[0].t << "," << batch.vertices[0].q << ")"
                      << " v1=(" << batch.vertices[1].x << "," << batch.vertices[1].y << ")"
                      << " uv1=(" << (batch.vertices[1].u >> 4) << "," << (batch.vertices[1].v >> 4) << ")"
                      << " stq1=(" << batch.vertices[1].s << "," << batch.vertices[1].t << "," << batch.vertices[1].q << ")"
                      << " v2=(" << batch.vertices[2].x << "," << batch.vertices[2].y << ")"
                      << " uv2=(" << (batch.vertices[2].u >> 4) << "," << (batch.vertices[2].v >> 4) << ")"
                      << " stq2=(" << batch.vertices[2].s << "," << batch.vertices[2].t << "," << batch.vertices[2].q << ")"
                      << " rgba0=(" << static_cast<uint32_t>(batch.vertices[0].r) << ","
                      << static_cast<uint32_t>(batch.vertices[0].g) << ","
                      << static_cast<uint32_t>(batch.vertices[0].b) << ","
                      << static_cast<uint32_t>(batch.vertices[0].a) << ")"
                      << " rgba1=(" << static_cast<uint32_t>(batch.vertices[1].r) << ","
                      << static_cast<uint32_t>(batch.vertices[1].g) << ","
                      << static_cast<uint32_t>(batch.vertices[1].b) << ","
                      << static_cast<uint32_t>(batch.vertices[1].a) << ")"
                      << " rgba2=(" << static_cast<uint32_t>(batch.vertices[2].r) << ","
                      << static_cast<uint32_t>(batch.vertices[2].g) << ","
                      << static_cast<uint32_t>(batch.vertices[2].b) << ","
                      << static_cast<uint32_t>(batch.vertices[2].a) << ")"
                      << std::endl;
        }
    });

    PS2_IF_AGRESSIVE_LOGS({
        if ((state.prim.ctxt != 0u || ctx.frame.fbp == 150u) &&
            s_debugContext1PrimitiveCount.fetch_add(1u, std::memory_order_relaxed) < 32u)
        {
            std::cout << "[gs:copy-prim]"
                      << " type=" << static_cast<uint32_t>(state.prim.type)
                      << " tme=" << static_cast<uint32_t>(state.prim.tme)
                      << " abe=" << static_cast<uint32_t>(state.prim.abe)
                      << " fst=" << static_cast<uint32_t>(state.prim.fst)
                      << " ctxt=" << static_cast<uint32_t>(state.prim.ctxt)
                      << " fbp=" << ctx.frame.fbp
                      << " fbw=" << ctx.frame.fbw
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.frame.psm) << std::dec
                      << " tex0=("
                      << "tbp0=" << ctx.tex0.tbp0
                      << " tbw=" << static_cast<uint32_t>(ctx.tex0.tbw)
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.psm) << std::dec
                      << " tcc=" << static_cast<uint32_t>(ctx.tex0.tcc)
                      << " tfx=" << static_cast<uint32_t>(ctx.tex0.tfx)
                      << " cbp=" << ctx.tex0.cbp
                      << " cpsm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.cpsm) << std::dec
                      << " csm=" << static_cast<uint32_t>(ctx.tex0.csm)
                      << " csa=" << static_cast<uint32_t>(ctx.tex0.csa)
                      << ")"
                      << " texclut=("
                      << "cbw=" << static_cast<uint32_t>(state.texclut.cbw)
                      << " cou=" << static_cast<uint32_t>(state.texclut.cou)
                      << " cov=" << state.texclut.cov
                      << ")"
                      << " ofx=" << (ctx.xyoffset.ofx >> 4)
                      << " ofy=" << (ctx.xyoffset.ofy >> 4)
                      << " scissor=(" << ctx.scissor.x0
                      << "," << ctx.scissor.y0
                      << ")-(" << ctx.scissor.x1
                      << "," << ctx.scissor.y1 << ")"
                      << " test=0x" << std::hex << ctx.test
                      << " alpha=0x" << ctx.alpha
                      << std::dec << std::endl;
        }
    });

    {
        // PS2X_GS_PRIMLOG=<sec>: log every primitive (setup + first vertices) for 150 ms starting
        // <sec> seconds after the first primitive, to see one frame's draw order.
        static const double logAt = [] { const char *e = std::getenv("PS2X_GS_PRIMLOG"); return e ? std::atof(e) : -1.0; }();
        if (logAt >= 0.0)
        {
            static const auto t0 = std::chrono::steady_clock::now();
            const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (t >= logAt && t < logAt + 0.6)
            {
                std::fprintf(stderr, "[prim] type=%u tme=%u fst=%u abe=%u alpha=%llx test=%llx fbp=%u fbw=%u fpsm=%x fbmsk=%x zbp=%u zmsk=%u tbp=%u tbw=%u tpsm=%x tw=%u th=%u tcc=%u tfx=%u fba=%llu "
                                     "v0=(%.1f,%.1f z=%u uv=%u,%u st=%.4f,%.4f rgba=%u,%u,%u,%u) v1=(%.1f,%.1f uv=%u,%u st=%.4f,%.4f rgba=%u,%u,%u,%u)\n",
                             (unsigned)state.prim.type, (unsigned)state.prim.tme, (unsigned)state.prim.fst, (unsigned)state.prim.abe,
                             (unsigned long long)ctx.alpha, (unsigned long long)ctx.test, (unsigned)ctx.frame.fbp, (unsigned)ctx.frame.fbw,
                             (unsigned)ctx.frame.psm, (unsigned)ctx.frame.fbmsk, (unsigned)ctx.zbuf.zbp, (unsigned)ctx.zbuf.zmask,
                             (unsigned)ctx.tex0.tbp0, (unsigned)ctx.tex0.tbw, (unsigned)ctx.tex0.psm, (unsigned)state.textureWidth,
                             (unsigned)state.textureHeight, (unsigned)ctx.tex0.tcc, (unsigned)ctx.tex0.tfx, (unsigned long long)ctx.fba,
                             batch.vertices[0].x - (ctx.xyoffset.ofx >> 4), batch.vertices[0].y - (ctx.xyoffset.ofy >> 4), (unsigned)batch.vertices[0].z,
                             (unsigned)(batch.vertices[0].u >> 4), (unsigned)(batch.vertices[0].v >> 4), batch.vertices[0].s, batch.vertices[0].t,
                             batch.vertices[0].r, batch.vertices[0].g, batch.vertices[0].b, batch.vertices[0].a,
                             batch.vertices[1].x - (ctx.xyoffset.ofx >> 4), batch.vertices[1].y - (ctx.xyoffset.ofy >> 4),
                             (unsigned)(batch.vertices[1].u >> 4), (unsigned)(batch.vertices[1].v >> 4), batch.vertices[1].s, batch.vertices[1].t,
                             batch.vertices[1].r, batch.vertices[1].g, batch.vertices[1].b, batch.vertices[1].a);
            }
        }
    }

    switch (state.prim.type)
    {
    case GS_PRIM_SPRITE:
        DrawSprite(batch);
        break;
    case GS_PRIM_TRIANGLE:
    case GS_PRIM_TRISTRIP:
    case GS_PRIM_TRIFAN:
        DrawTriangle(batch);
        break;
    case GS_PRIM_LINE:
    case GS_PRIM_LINESTRIP:
        DrawLine(batch);
        break;
    case GS_PRIM_POINT:
    {
        const GSVertex &v = batch.vertices[0];
        const auto &ctx = state.context;
        int px = static_cast<int>(v.x) - (ctx.xyoffset.ofx >> 4);
        int py = static_cast<int>(v.y) - (ctx.xyoffset.ofy >> 4);
        WritePixel(state, px, py, static_cast<u32>(v.z), v.r, v.g, v.b, v.a, v.fog);
        break;
    }
    default:
        break;
    }
}

void GSCpuBackend::WritePixel(const GSDrawState &state, int x, int y, int z, uint8_t r, uint8_t g, uint8_t b, uint8_t a, uint8_t fog)
{
    if (!ownsRow(y))
        return;
    const auto &ctx = state.context;
    if (x < ctx.scissor.x0 || x > ctx.scissor.x1 || y < ctx.scissor.y0 || y > ctx.scissor.y1)
        return;

    if (state.prim.fge)
    {
        const uint32_t inverseFog = 255u - fog;
        auto applyFog = [&](uint8_t input, uint8_t fogColor) -> uint8_t
        {
            return static_cast<uint8_t>(((static_cast<uint32_t>(fog) * input) >> 8) + ((inverseFog * fogColor) >> 8));
        };

        r = applyFog(r, state.fogR);
        g = applyFog(g, state.fogG);
        b = applyFog(b, state.fogB);
    }

    const u32 fbp = GSInternal::framePageBaseToBlock(ctx.frame.fbp);
    const u32 fbw = std::max<u32>(ctx.frame.fbw, 1u);
    const u32 fpsm = ctx.frame.psm;
    const u32 zbp = GSInternal::framePageBaseToBlock(ctx.zbuf.zbp);
    const u32 zpsm = ctx.zbuf.psm;

    const PixelWriteMask writeMask = classifyAlphaTest(ctx.test, a, static_cast<uint8_t>(fpsm));
    if (!writeMask.writesAnything())
    {
        return;
    }

    const uint32_t ztestMethod = static_cast<uint32_t>((ctx.test >> 17) & 3u);
    const bool alphaBlendEnabled = state.prim.abe;
    const bool preserveDestinationAlpha = writeMask.writeRgb && !writeMask.writeAlpha && fpsm == GS_PSM_CT32;
    const bool destinationAlphaTestNeedsRead = ((ctx.test >> 14) & 0x1u) != 0u && (fpsm == GS_PSM_CT32 || fpsm == GS_PSM_CT16 || fpsm == GS_PSM_CT16S);

    // small optimization, avoid reading the framebuffer for simple draws
    // TODO: only one address lookup for rmw
    const bool frmw = destinationAlphaTestNeedsRead || (writeMask.writesFramebuffer() && ((ctx.frame.fbmsk != 0) || alphaBlendEnabled || preserveDestinationAlpha));

    u32 rawFramebufferPixel = 0;
    u32 fbrgba = 0;
    if (frmw)
    {
        rawFramebufferPixel = ReadVramUnlocked(fpsm, fbp, fbw, x, y);
        fbrgba = rawFramebufferPixel;

        if (bitsPerPixel(fpsm) == 16)
        {
            fbrgba = Rgba5551ToRgba8888(fbrgba);
        }
        else if (fpsm == GS_PSM_CT24)
        {
            // The GS supplies 0x80 as destination alpha for RGB24 blending.
            fbrgba |= 0x80000000u;
        }
    }

    if (!passesDestinationAlphaTest(ctx.test, static_cast<uint8_t>(fpsm), rawFramebufferPixel))
    {
        return;
    }

    bool zpass = false;
    uint32_t storedZ = 0u;
    switch (ztestMethod)
    {
    case 0:
        zpass = false;
        break;
    case 1:
        zpass = true;
        break;
    case 2:
        storedZ = ReadVramUnlocked(zpsm, zbp, fbw, x, y);
        zpass = static_cast<uint32_t>(z) >= storedZ;
        break;
    case 3:
        storedZ = ReadVramUnlocked(zpsm, zbp, fbw, x, y);
        zpass = static_cast<uint32_t>(z) > storedZ;
        break;
    }

    if (!zpass)
    {
        return;
    }

    if (writeMask.writesFramebuffer())
    {
        const u8 srcR = r;
        const u8 srcG = g;
        const u8 srcB = b;

        if (state.prim.abe)
        {
            uint8_t dr = fbrgba & 0xFF;
            uint8_t dg = (fbrgba >> 8) & 0xFF;
            uint8_t db = (fbrgba >> 16) & 0xFF;
            uint8_t da = (fbrgba >> 24) & 0xFF;

            // PABE disables alpha blending when the source alpha MSB is clear.
            if (!(state.pabe && (a & 0x80u) == 0u))
            {
                uint64_t alphaReg = ctx.alpha;
                uint8_t asel = alphaReg & 3;
                uint8_t bsel = (alphaReg >> 2) & 3;
                uint8_t csel = (alphaReg >> 4) & 3;
                uint8_t dsel = (alphaReg >> 6) & 3;
                uint8_t fix = static_cast<uint8_t>((alphaReg >> 32) & 0xFF);

                auto pickRGB = [&](uint8_t sel, int cs, int cd) -> int
                {
                    if (sel == 0)
                        return cs;
                    if (sel == 1)
                        return cd;
                    return 0;
                };
                int cAlpha = (csel == 0) ? a : (csel == 1) ? da
                                                           : fix;

                r = clampU8(((pickRGB(asel, r, dr) - pickRGB(bsel, r, dr)) * cAlpha >> 7) + pickRGB(dsel, r, dr));
                g = clampU8(((pickRGB(asel, g, dg) - pickRGB(bsel, g, dg)) * cAlpha >> 7) + pickRGB(dsel, g, dg));
                b = clampU8(((pickRGB(asel, b, db) - pickRGB(bsel, b, db)) * cAlpha >> 7) + pickRGB(dsel, b, db));
            }
            else
            {
                r = srcR;
                g = srcG;
                b = srcB;
            }
        }

        if (writeMask.writeAlpha && (ctx.fba & 0x1ull) != 0ull && ctx.frame.psm != GS_PSM_CT24)
        {
            a = static_cast<uint8_t>(a | 0x80u);
        }

        u32 pixel = pack32(r, g, b, a);

        if (ctx.frame.fbmsk != 0)
        {
            pixel = (pixel & ~ctx.frame.fbmsk) | (fbrgba & ctx.frame.fbmsk);
        }

        if (preserveDestinationAlpha)
        {
            pixel = (pixel & 0x00FFFFFFu) | (fbrgba & 0xFF000000u);
        }

        // format conversion
        if (bitsPerPixel(fpsm) == 16)
        {
            pixel = Rgba8888ToRgba5551(pixel);
        }

        WriteVramUnlocked(fpsm, fbp, fbw, x, y, pixel);
    }

    if (writeMask.writeDepth && !ctx.zbuf.zmask)
    {
        WriteVramUnlocked(zpsm, zbp, fbw, x, y, z);
    }
}

uint32_t GSCpuBackend::LookupCLUT(const GSDrawState &state,
                                  uint8_t index,
                                  uint8_t cpsm,
                                  uint8_t csm,
                                  uint8_t csa,
                                  uint8_t sourcePsm)
{
    const bool sixteenBit = cpsm == GS_PSM_CT16 || cpsm == GS_PSM_CT16S;
    const uint32_t csaMask = sixteenBit ? 0x1Fu : 0x0Fu;
    const uint32_t clutBase = (static_cast<uint32_t>(csa) & csaMask) << 4u;
    const uint32_t sourceIndex = isFourBitIndexedPsm(sourcePsm)
                                     ? (static_cast<uint32_t>(index) & 0x0Fu)
                                     : static_cast<uint32_t>(index);

    uint32_t clutIndex = (clutBase + sourceIndex) & (sixteenBit ? 0x1FFu : 0x0FFu);
    if (!sixteenBit && csm == 0u && isEightBitIndexedPsm(sourcePsm))
    {
        const uint32_t block = std::min((sourceIndex & 0xF0u) + clutBase, 240u);
        clutIndex = block + (sourceIndex & 0x0Fu);
    }

    switch (cpsm)
    {
    case GS_PSM_CT32:
    {
        const uint32_t raw = static_cast<uint32_t>(m_clut[clutIndex]) | (static_cast<uint32_t>(m_clut[clutIndex + 256u]) << 16u);
        return applyTexa(state.texa, cpsm, raw);
    }
    case GS_PSM_CT24:
    {
        const uint32_t raw = static_cast<uint32_t>(m_clut[clutIndex]) | (static_cast<uint32_t>(m_clut[clutIndex + 256u]) << 16u);
        return applyTexa(state.texa, cpsm, raw & 0x00FFFFFFu);
    }
    case GS_PSM_CT16:
    case GS_PSM_CT16S:
        return applyTexa(state.texa, cpsm, Rgba5551ToRgba8888(m_clut[clutIndex]));
    default:
        break;
    }

    return 0xFFFF00FFu;
}

namespace
{
    // Texture coordinate -> wrap -> point or bilinear sample. fetch(u, v) returns the decoded
    // RGBA8888 texel at wrapped coordinates. Shared by SampleTexture (reads VRAM) and the
    // decoded-texture path of DrawTriangle so both compute exactly the same result.
    // One wrapped texel fetch. Force-inlined: as a lambda MSVC called it out of line four times
    // per bilinear sample (integer work only, no float math moves).
    template <typename Fetch>
#if defined(_MSC_VER)
    __forceinline
#else
    inline __attribute__((always_inline))
#endif
    uint32_t samplePointWith(Fetch &fetch, int sampleU, int sampleV, int texW, int texH, uint8_t wrapU, uint8_t wrapV,
                             uint16_t minU, uint16_t maxU, uint16_t minV, uint16_t maxV)
    {
        return fetch(wrapTextureCoordinate(sampleU, texW, wrapU, minU, maxU),
                     wrapTextureCoordinate(sampleV, texH, wrapV, minV, maxV));
    }

    template <typename Fetch>
    inline uint32_t sampleTextureWith(const GSDrawState &state, float s, float t, float q, uint16_t u, uint16_t v, Fetch &&fetch)
    {
        const auto &ctx = state.context;

        const int texW = state.textureWidth;
        const int texH = state.textureHeight;
        const uint64_t clamp = ctx.clamp;
        const uint8_t wrapU = static_cast<uint8_t>(clamp & 0x3u);
        const uint8_t wrapV = static_cast<uint8_t>((clamp >> 2) & 0x3u);
        const uint16_t minU = static_cast<uint16_t>((clamp >> 4) & 0x3FFu);
        const uint16_t maxU = static_cast<uint16_t>((clamp >> 14) & 0x3FFu);
        const uint16_t minV = static_cast<uint16_t>((clamp >> 24) & 0x3FFu);
        const uint16_t maxV = static_cast<uint16_t>((clamp >> 34) & 0x3FFu);

        float texUf, texVf;
        if (state.prim.fst)
        {
            texUf = static_cast<float>(u) / 16.0f;
            texVf = static_cast<float>(v) / 16.0f;
        }
        else
        {
            const float invQ = 1.0f / fabsQ(q);
            texUf = s * invQ * static_cast<float>(texW);
            texVf = t * invQ * static_cast<float>(texH);
        }

#define samplePoint(su, sv) samplePointWith(fetch, (su), (sv), texW, texH, wrapU, wrapV, minU, maxU, minV, maxV)

        if (!state.linearFilter)
        {
            return samplePoint(static_cast<int>(texUf), static_cast<int>(texVf));
        }

        const float sampleU = texUf - 0.5f;
        const float sampleV = texVf - 0.5f;
        const int u0 = static_cast<int>(std::floor(sampleU));
        const int v0 = static_cast<int>(std::floor(sampleV));
        const int u1 = u0 + 1;
        const int v1 = v0 + 1;
        const float fx = sampleU - static_cast<float>(u0);
        const float fy = sampleV - static_cast<float>(v0);

        const uint32_t c00 = samplePoint(u0, v0);
        const uint32_t c10 = samplePoint(u1, v0);
        const uint32_t c01 = samplePoint(u0, v1);
        const uint32_t c11 = samplePoint(u1, v1);

        return lerpTexel(c00, c10, c01, c11, fx, fy);
#undef samplePoint
    }
}

uint32_t GSCpuBackend::DecodeTexel(const GSDrawState &state, int u, int v)
{
    const auto &tex = state.context.tex0;
    const u32 out = ReadTextureVramUnlocked(tex.psm, tex.tbp0, tex.tbw, u, v);

    switch (tex.psm)
    {
    case GS_PSM_CT32:
    case GS_PSM_Z32:
    case GS_PSM_CT24:
    case GS_PSM_Z24:
        return applyTexa(state.texa, tex.psm, out);
    case GS_PSM_CT16:
    case GS_PSM_CT16S:
    case GS_PSM_Z16:
    case GS_PSM_Z16S:
        return applyTexa(state.texa, tex.psm, Rgba5551ToRgba8888(out));
    case GS_PSM_T8:
    case GS_PSM_T8H:
    case GS_PSM_T4:
    case GS_PSM_T4HL:
    case GS_PSM_T4HH:
        return LookupCLUT(state, static_cast<u8>(out), tex.cpsm, tex.csm, tex.csa, tex.psm);
    }

    return 0xFFFF00FFu;
}

uint32_t GSCpuBackend::SampleTexture(const GSDrawState &state, float s, float t, float q, uint16_t u, uint16_t v)
{
    return sampleTextureWith(state, s, t, q, u, v, [&](int su, int sv) { return DecodeTexel(state, su, sv); });
}

namespace
{
    // WritePixel() with everything that is constant for a primitive decoded once. Must stay
    // behaviourally identical to GSCpuBackend::WritePixel (PS2X_GS_VERIFY checks it).
    // PS2X_SPRITE_STATS also counts what happened to triangle pixels (why they were not written).
    std::atomic<uint64_t> g_triPix[6]; // 0 scissor, 1 alpha test, 2 dest alpha, 3 z test, 4 written, 5 covered
    bool gsStatsOn()
    {
        static const bool on = [] { const char *e = std::getenv("PS2X_SPRITE_STATS"); return e && *e && *e != '0'; }();
        return on;
    }

    struct PixelPipe
    {
        bool countStats = false;
        using ReadFn = uint32_t (*)(uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t);
        using WriteFn = void (*)(uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);

        uint8_t *vram = nullptr;
        ReadFn readFb = nullptr, readZ = nullptr;
        WriteFn writeFb = nullptr, writeZ = nullptr;
        int sx0 = 0, sx1 = 0, sy0 = 0, sy1 = 0;
        bool fge = false;
        uint8_t fogR = 0, fogG = 0, fogB = 0;
        u32 fbp = 0, fbw = 1, fpsm = 0, zbp = 0, zpsm = 0;
        uint64_t test = 0;
        uint32_t ztestMethod = 0;
        bool abe = false, pabe = false;
        uint8_t asel = 0, bsel = 0, csel = 0, dsel = 0, fix = 0;
        bool dateNeedsRead = false;
        bool fbaForce = false;
        u32 fbmsk = 0;
        bool zmask = false;
        bool fb16 = false, fb24 = false;

        void init(const GSDrawState &state, uint8_t *vramPtr, const ReadFn *reads, const WriteFn *writes)
        {
            const auto &ctx = state.context;
            vram = vramPtr;
            sx0 = ctx.scissor.x0; sx1 = ctx.scissor.x1; sy0 = ctx.scissor.y0; sy1 = ctx.scissor.y1;
            fge = state.prim.fge != 0;
            fogR = state.fogR; fogG = state.fogG; fogB = state.fogB;
            fbp = GSInternal::framePageBaseToBlock(ctx.frame.fbp);
            fbw = std::max<u32>(ctx.frame.fbw, 1u);
            fpsm = ctx.frame.psm;
            zbp = GSInternal::framePageBaseToBlock(ctx.zbuf.zbp);
            zpsm = ctx.zbuf.psm;
            readFb = reads[fpsm & 0x3Fu]; writeFb = writes[fpsm & 0x3Fu];
            readZ = reads[zpsm & 0x3Fu]; writeZ = writes[zpsm & 0x3Fu];
            test = ctx.test;
            ztestMethod = static_cast<uint32_t>((test >> 17) & 3u);
            abe = state.prim.abe != 0;
            pabe = state.pabe;
            const uint64_t alphaReg = ctx.alpha;
            asel = alphaReg & 3; bsel = (alphaReg >> 2) & 3; csel = (alphaReg >> 4) & 3; dsel = (alphaReg >> 6) & 3;
            fix = static_cast<uint8_t>((alphaReg >> 32) & 0xFF);
            dateNeedsRead = ((test >> 14) & 0x1u) != 0u && (fpsm == GS_PSM_CT32 || fpsm == GS_PSM_CT16 || fpsm == GS_PSM_CT16S);
            fbaForce = (ctx.fba & 0x1ull) != 0ull && ctx.frame.psm != GS_PSM_CT24;
            fbmsk = ctx.frame.fbmsk;
            zmask = ctx.zbuf.zmask != 0;
            fb16 = bitsPerPixel(static_cast<uint8_t>(fpsm)) == 16;
            fb24 = fpsm == GS_PSM_CT24;
        }

        // True when write() would drop the pixel at its Z test whatever its colour: every test
        // before it only returns, so a caller may skip texturing such pixels. Off with stats on
        // (keeps the per-test counters as they were).
        bool failsDepth(int x, int y, int z) const
        {
            if (countStats)
                return false;
            switch (ztestMethod)
            {
            case 0: return true;
            case 2: return static_cast<uint32_t>(z) < readZ(vram, zbp, fbw, x, y);
            case 3: return static_cast<uint32_t>(z) <= readZ(vram, zbp, fbw, x, y);
            default: return false;
            }
        }

        void write(int x, int y, int z, uint8_t r, uint8_t g, uint8_t b, uint8_t a, uint8_t fog) const
        {
            if (x < sx0 || x > sx1 || y < sy0 || y > sy1)
            {
                if (countStats) g_triPix[0].fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (fge)
            {
                const uint32_t inverseFog = 255u - fog;
                r = static_cast<uint8_t>(((static_cast<uint32_t>(fog) * r) >> 8) + ((inverseFog * fogR) >> 8));
                g = static_cast<uint8_t>(((static_cast<uint32_t>(fog) * g) >> 8) + ((inverseFog * fogG) >> 8));
                b = static_cast<uint8_t>(((static_cast<uint32_t>(fog) * b) >> 8) + ((inverseFog * fogB) >> 8));
            }
            const PixelWriteMask writeMask = classifyAlphaTest(test, a, static_cast<uint8_t>(fpsm));
            if (!writeMask.writesAnything())
            {
                if (countStats) g_triPix[1].fetch_add(1, std::memory_order_relaxed);
                return;
            }
            const bool preserveDestinationAlpha = writeMask.writeRgb && !writeMask.writeAlpha && fpsm == GS_PSM_CT32;
            const bool frmw = dateNeedsRead || (writeMask.writesFramebuffer() && (fbmsk != 0 || abe || preserveDestinationAlpha));
            u32 rawFramebufferPixel = 0;
            u32 fbrgba = 0;
            if (frmw)
            {
                rawFramebufferPixel = readFb(vram, fbp, fbw, x, y);
                fbrgba = rawFramebufferPixel;
                if (fb16)
                    fbrgba = Rgba5551ToRgba8888(fbrgba);
                else if (fb24)
                    fbrgba |= 0x80000000u;
            }
            if (!passesDestinationAlphaTest(test, static_cast<uint8_t>(fpsm), rawFramebufferPixel))
            {
                if (countStats) g_triPix[2].fetch_add(1, std::memory_order_relaxed);
                return;
            }
            bool zpass = false;
            switch (ztestMethod)
            {
            case 0: zpass = false; break;
            case 1: zpass = true; break;
            case 2: zpass = static_cast<uint32_t>(z) >= readZ(vram, zbp, fbw, x, y); break;
            case 3: zpass = static_cast<uint32_t>(z) > readZ(vram, zbp, fbw, x, y); break;
            }
            if (!zpass)
            {
                if (countStats) g_triPix[3].fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (countStats) g_triPix[4].fetch_add(1, std::memory_order_relaxed);
            if (writeMask.writesFramebuffer())
            {
                if (abe)
                {
                    const uint8_t dr = fbrgba & 0xFF, dg = (fbrgba >> 8) & 0xFF, db = (fbrgba >> 16) & 0xFF, da = (fbrgba >> 24) & 0xFF;
                    if (!(pabe && (a & 0x80u) == 0u))
                    {
                        auto pick = [](uint8_t sel, int cs, int cd) -> int { return sel == 0 ? cs : (sel == 1 ? cd : 0); };
                        const int cAlpha = (csel == 0) ? a : (csel == 1) ? da : fix;
                        r = clampU8(((pick(asel, r, dr) - pick(bsel, r, dr)) * cAlpha >> 7) + pick(dsel, r, dr));
                        g = clampU8(((pick(asel, g, dg) - pick(bsel, g, dg)) * cAlpha >> 7) + pick(dsel, g, dg));
                        b = clampU8(((pick(asel, b, db) - pick(bsel, b, db)) * cAlpha >> 7) + pick(dsel, b, db));
                    }
                }
                if (writeMask.writeAlpha && fbaForce)
                    a = static_cast<uint8_t>(a | 0x80u);
                u32 pixel = pack32(r, g, b, a);
                if (fbmsk != 0)
                    pixel = (pixel & ~fbmsk) | (fbrgba & fbmsk);
                if (preserveDestinationAlpha)
                    pixel = (pixel & 0x00FFFFFFu) | (fbrgba & 0xFF000000u);
                if (fb16)
                    pixel = Rgba8888ToRgba5551(pixel);
                writeFb(vram, fbp, fbw, x, y, pixel);
            }
            if (writeMask.writeDepth && !zmask)
                writeZ(vram, zbp, fbw, x, y, static_cast<uint32_t>(z));
        }

        // write() can be replaced by writeTested() for pixels inside the scissor that passed the Z
        // test (failsDepth false; ZTST ALWAYS needs none) when stats are off.
        bool canWriteTested() const
        {
            return !countStats && ztestMethod != 0u;
        }

        // write() for such a pixel, with the fog already applied to `rgba` (r | g << 8 | b << 16 | a << 24).
        void writeTested(int x, int y, int z, uint32_t rgba) const
        {
            const PixelWriteMask writeMask = (test & 0x1u) != 0u ? classifyAlphaTest(test, static_cast<uint8_t>(rgba >> 24), static_cast<uint8_t>(fpsm))
                                                                 : PixelWriteMask{};
            if (!writeMask.writesAnything())
                return;
            const bool preserveDestinationAlpha = writeMask.writeRgb && !writeMask.writeAlpha && fpsm == GS_PSM_CT32;
            u32 rawFramebufferPixel = 0;
            u32 fbrgba = 0;
            if (dateNeedsRead || (writeMask.writesFramebuffer() && (fbmsk != 0 || abe || preserveDestinationAlpha)))
            {
                rawFramebufferPixel = readFb(vram, fbp, fbw, x, y);
                fbrgba = rawFramebufferPixel;
                if (fb16)
                    fbrgba = Rgba5551ToRgba8888(fbrgba);
                else if (fb24)
                    fbrgba |= 0x80000000u;
            }
            if (!passesDestinationAlphaTest(test, static_cast<uint8_t>(fpsm), rawFramebufferPixel))
                return;
            uint8_t r = static_cast<uint8_t>(rgba), g = static_cast<uint8_t>(rgba >> 8), b = static_cast<uint8_t>(rgba >> 16), a = static_cast<uint8_t>(rgba >> 24);
            if (writeMask.writesFramebuffer() && abe)
            {
                const uint8_t dr = fbrgba & 0xFF, dg = (fbrgba >> 8) & 0xFF, db = (fbrgba >> 16) & 0xFF, da = (fbrgba >> 24) & 0xFF;
                if (!(pabe && (a & 0x80u) == 0u))
                {
                    auto pick = [](uint8_t sel, int cs, int cd) -> int { return sel == 0 ? cs : (sel == 1 ? cd : 0); };
                    const int cAlpha = (csel == 0) ? a : (csel == 1) ? da : fix;
                    r = clampU8(((pick(asel, r, dr) - pick(bsel, r, dr)) * cAlpha >> 7) + pick(dsel, r, dr));
                    g = clampU8(((pick(asel, g, dg) - pick(bsel, g, dg)) * cAlpha >> 7) + pick(dsel, g, dg));
                    b = clampU8(((pick(asel, b, db) - pick(bsel, b, db)) * cAlpha >> 7) + pick(dsel, b, db));
                }
            }
            if (writeMask.writesFramebuffer())
            {
                if (writeMask.writeAlpha && fbaForce)
                    a = static_cast<uint8_t>(a | 0x80u);
                u32 pixel = pack32(r, g, b, a);
                if (fbmsk != 0)
                    pixel = (pixel & ~fbmsk) | (fbrgba & fbmsk);
                if (preserveDestinationAlpha)
                    pixel = (pixel & 0x00FFFFFFu) | (fbrgba & 0xFF000000u);
                if (fb16)
                    pixel = Rgba8888ToRgba5551(pixel);
                writeFb(vram, fbp, fbw, x, y, pixel);
            }
            if (writeMask.writeDepth && !zmask)
                writeZ(vram, zbp, fbw, x, y, static_cast<uint32_t>(z));
        }
    };
}

namespace
{
    // Row-parallel rasterization for large primitives. Each pixel's result depends only on its
    // own framebuffer/depth location and on texture memory, so rows can be drawn in parallel as
    // long as the texture does not overlap the memory this primitive writes (checked by the
    // caller). PS2X_GS_THREADS=<n> sets the worker count (0 or 1 = off).
    class GsRowPool
    {
    public:
        static GsRowPool &instance()
        {
            // Never destroyed: the detached workers wait on its members until process exit.
            static GsRowPool *pool = new GsRowPool();
            return *pool;
        }

        int workers() const { return static_cast<int>(m_threads.size()); }

        // Runs fn(y) for y in [y0, y1], split into contiguous bands across the workers plus the
        // calling thread. Returns when all rows are done.
        void run(int y0, int y1, const std::function<void(int)> &fn)
        {
            const int rows = y1 - y0 + 1;
            const int parts = std::min(rows, workers() + 1);
            if (parts <= 1)
            {
                for (int y = y0; y <= y1; ++y)
                    fn(y);
                return;
            }
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_fn = &fn;
                m_y0 = y0;
                m_rows = rows;
                m_parts = parts;
                m_nextPart = 1; // part 0 runs on the caller
                m_pending = parts - 1;
                ++m_generation;
            }
            m_cv.notify_all();
            runPart(0);
            std::unique_lock<std::mutex> lock(m_mutex);
            // Help with parts no worker has picked up yet, then wait for the rest.
            while (m_nextPart < m_parts)
            {
                const int part = m_nextPart++;
                lock.unlock();
                runPart(part);
                lock.lock();
                --m_pending;
            }
            m_doneCv.wait(lock, [this] { return m_pending == 0; });
            m_fn = nullptr;
        }

    private:
        GsRowPool()
        {
            int count = ps2xHostCpu().logical;
            count = std::max(0, std::min(count - 2, 7)); // leave room for the EE and IOP threads
            if (const char *e = std::getenv("PS2X_GS_THREADS"))
                count = std::max(0, std::min(std::atoi(e) - 1, 15));
            for (int i = 0; i < count; ++i)
                m_threads.emplace_back([this] { workerLoop(); });
            for (std::thread &t : m_threads)
                t.detach();
        }

        void runPart(int part)
        {
            const int begin = m_y0 + static_cast<int>((static_cast<int64_t>(m_rows) * part) / m_parts);
            const int end = m_y0 + static_cast<int>((static_cast<int64_t>(m_rows) * (part + 1)) / m_parts);
            for (int y = begin; y < end; ++y)
                (*m_fn)(y);
        }

        void workerLoop()
        {
            uint64_t seen = 0;
            std::unique_lock<std::mutex> lock(m_mutex);
            for (;;)
            {
                m_cv.wait(lock, [&] { return m_generation != seen; });
                seen = m_generation;
                while (m_fn != nullptr && m_nextPart < m_parts)
                {
                    const int part = m_nextPart++;
                    lock.unlock();
                    runPart(part);
                    lock.lock();
                    if (--m_pending == 0)
                        m_doneCv.notify_one();
                }
            }
        }

        std::vector<std::thread> m_threads;
        std::mutex m_mutex;
        std::condition_variable m_cv;
        std::condition_variable m_doneCv;
        const std::function<void(int)> *m_fn = nullptr;
        int m_y0 = 0, m_rows = 0, m_parts = 0, m_nextPart = 0, m_pending = 0;
        uint64_t m_generation = 0;
    };

    // GS memory is addressed in 256-byte blocks, 32 blocks per 8 KB page. Returns the block range
    // [first, last) that rows [y0, y1] of a buffer at `baseBlock` with width `bw` (64-pixel units)
    // can touch. Conservative: whole page rows.
    void gsBlockRange(uint32_t psm, uint32_t baseBlock, uint32_t bw, int y0, int y1, uint64_t &first, uint64_t &last)
    {
        uint32_t pageW = 64u, pageH = 32u;
        switch (psm)
        {
        case GS_PSM_CT16: case GS_PSM_CT16S: case GS_PSM_Z16: case GS_PSM_Z16S: pageW = 64u; pageH = 64u; break;
        case GS_PSM_T8: pageW = 128u; pageH = 64u; break;
        case GS_PSM_T4: pageW = 128u; pageH = 128u; break;
        default: break;
        }
        const uint32_t widthPixels = std::max<uint32_t>(bw, 1u) * 64u;
        const uint64_t pagesPerRow = (widthPixels + pageW - 1u) / pageW;
        const uint64_t firstRow = static_cast<uint64_t>(std::max(y0, 0)) / pageH;
        const uint64_t lastRow = static_cast<uint64_t>(std::max(y1, 0)) / pageH;
        first = static_cast<uint64_t>(baseBlock) + firstRow * pagesPerRow * 32u;
        last = static_cast<uint64_t>(baseBlock) + (lastRow + 1u) * pagesPerRow * 32u;
    }

    bool rangesOverlap(uint64_t a0, uint64_t a1, uint64_t b0, uint64_t b1)
    {
        // GS memory is 4 MB = 16384 blocks and addresses wrap; treat anything that wraps as overlapping.
        if (a1 > 16384u || b1 > 16384u)
            return true;
        return a0 < b1 && b0 < a1;
    }

    // Blocks a transfer can write: rows [DSAY, DSAY + RRH) of the destination buffer.
    bool transferDestinationBlocks(const GSTransferCommand &command, uint64_t &first, uint64_t &last)
    {
        if (command.direction == 1u) // local -> host only reads
            return false;
        const int y0 = static_cast<int>(command.trxpos.dsay);
        const int y1 = y0 + std::max<int>(static_cast<int>(command.trxreg.rrh), 1) - 1;
        gsBlockRange(command.bitbltbuf.dpsm, command.bitbltbuf.dbp, command.bitbltbuf.dbw, y0, y1, first, last);
        return true;
    }

    // Per-row x-span of a triangle for DrawTriangle. Its pixel test computes, in float,
    //   w0 = ((A0 * (px - fx2) + B0 * (py - fy2)) * winding) * invAbsDenom   (w1 alike, w2 = 1 - w0 - w1)
    // and draws when every w >= -eps. Here the same planes are evaluated in double with a
    // margin far above any float rounding of that expression (a few ulps of the largest term,
    // whichever way the compiler associates or contracts it under /fp:fast: relative 1e-5 vs
    // ~1e-7), plus one extra pixel on each side. Pixels outside the span therefore fail the
    // float test too; inside it the float test still decides alone, so the output is unchanged.
    struct TriangleSpanSetup
    {
        double a[3] = {}, b[3] = {}, k[3] = {}, bound[3] = {};
        int minX = 0, maxX = 0;
    };

    GS_NOINLINE TriangleSpanSetup triangleSpanSetup(float fx0, float fy0, float fx1, float fy1, float fx2, float fy2,
                                                             float winding, float invAbsDenom, float eps,
                                                             int minX, int maxX, int minY, int maxY)
    {
        TriangleSpanSetup s;
        s.minX = minX;
        s.maxX = maxX;
        const double scale = static_cast<double>(winding) * static_cast<double>(invAbsDenom);
        const double A[2] = {static_cast<double>(fy1) - fy2, static_cast<double>(fy2) - fy0};
        const double B[2] = {static_cast<double>(fx2) - fx1, static_cast<double>(fx0) - fx2};
        // Largest |px|, |py| (plus the vertex offset) any term of the float expression can see.
        const double mx = std::max(std::fabs(static_cast<double>(minX)), std::fabs(static_cast<double>(maxX))) + 1.0 + std::fabs(static_cast<double>(fx2));
        const double my = std::max(std::fabs(static_cast<double>(minY)), std::fabs(static_cast<double>(maxY))) + 1.0 + std::fabs(static_cast<double>(fy2));
        double mag[2];
        for (int i = 0; i < 2; ++i)
        {
            s.a[i] = A[i] * scale;
            s.b[i] = B[i] * scale;
            s.k[i] = -(A[i] * fx2 + B[i] * fy2) * scale;
            mag[i] = (std::fabs(A[i]) * mx + std::fabs(B[i]) * my) * std::fabs(scale);
            s.bound[i] = -static_cast<double>(eps) - (1.0e-5 * mag[i] + 1.0e-6);
        }
        // w2 = 1 - w0 - w1
        s.a[2] = -(s.a[0] + s.a[1]);
        s.b[2] = -(s.b[0] + s.b[1]);
        s.k[2] = 1.0 - s.k[0] - s.k[1];
        s.bound[2] = -static_cast<double>(eps) - (1.0e-5 * (1.0 + 2.0 * (mag[0] + mag[1])) + 3.0e-6);
        return s;
    }

    // Columns [x0, x1] of row y that can pass the pixel test; false = none.
    GS_NOINLINE bool triangleRowSpan(const TriangleSpanSetup &s, int y, int &x0, int &x1)
    {
        const double py = static_cast<double>(y) + 0.5;
        double lo = static_cast<double>(s.minX) + 0.5, hi = static_cast<double>(s.maxX) + 0.5; // px range
        for (int i = 0; i < 3; ++i)
        {
            // a * px + c >= bound
            const double t = s.bound[i] - (s.b[i] * py + s.k[i]);
            const double a = s.a[i];
            if (a > 0.0)
                lo = std::max(lo, t / a);
            else if (a < 0.0)
                hi = std::min(hi, t / a);
            else if (t > 0.0)
                return false;
            if (!(lo <= hi + 2.0)) // also catches NaN
                return false;
        }
        const double xl = std::floor(lo - 0.5) - 1.0;
        const double xh = std::ceil(hi - 0.5) + 1.0;
        x0 = xl > static_cast<double>(s.minX) ? static_cast<int>(xl) : s.minX;
        x1 = xh < static_cast<double>(s.maxX) ? static_cast<int>(xh) : s.maxX;
        return x0 <= x1;
    }

    // Wrapped coordinates of this axis stay inside [0, size)?
    bool wrapStaysInTexture(uint8_t mode, uint16_t regionMin, uint16_t regionMax, int size)
    {
        switch (mode & 0x3u)
        {
        case 2: // REGION_CLAMP: min(max(c, MIN), MAX) <= MAX
            return regionMax < size;
        case 3: // REGION_REPEAT: (c & MIN) | MAX <= MIN | MAX
            return (regionMin | regionMax) < size;
        default: // REPEAT masks with size - 1, CLAMP clamps to [0, size - 1]
            return true;
        }
    }

#if defined(__AVX2__)
    // DrawTriangle's pixel loop for 8 pixels of a row at a time. Every lane does exactly the IEEE
    // operations the Release build (/arch:AVX2 /fp:fast) compiles the scalar loop to, in the same
    // order: w = ((fma(A, px - fx2, B * dy)) * winding) * invAbsDenom, w2 = (1 - w0) - w1, attributes
    // a0 * w0 then fma(w1, a1, .) then fma(w2, a2, .) (z the same in double), 1 / q, texel coords
    // (s * invQ) * texW, floor, truncations, and lerpTexel's fma chain per channel. So the output is
    // bit-identical (checked with --gs-replay hashes); if the scalar code changes, re-check its
    // disassembly. The depth pre-test runs for 8 pixels before their writes, so it is only used when
    // the Z buffer cannot share memory with the frame. Texels come from the decoded texture only.
    // For a CT32 frame with Z32/Z16/Z16S, no DATE and ATE off or AFAIL KEEP, the pixel writes are
    // done here too ("direct": inline swizzle addresses, alpha test, blend, FBMSK on 8 lanes, the
    // integer math of PixelPipe::write); other states call PixelPipe::writeTested per lane.
    // PS2X_GS_SCALAR=1 = scalar loop everywhere, PS2X_GS_NODIRECT=1 = no direct writes.

    // GSMem::PixelStorageTraits<psm>::Address/Read/Write of a 32- or 16-bit buffer, inline.
    struct SwizzledBuffer
    {
        const uint16_t *table = nullptr;
        uint32_t pwShift = 0, phShift = 0, basePage = 0, blockOffset = 0, pagesPerRow = 0, pixelsPerPage = 0, bits = 0, mask = 0;

        bool init(uint32_t psm, uint32_t bp, uint32_t bw)
        {
            table = GSMem::PageLookupData(psm);
            const uint32_t bpp = bitsPerPixel(static_cast<uint8_t>(psm));
            if (!table || (bpp != 32u && bpp != 16u))
                return false;
            pwShift = 6u;                   // 64 pixels wide
            phShift = bpp == 32u ? 5u : 6u; // 32 or 64 rows
            basePage = bp / 32u;
            blockOffset = (bp % 32u) << (pwShift + phShift);
            pagesPerRow = (bw * 64u) >> pwShift;
            pixelsPerPage = 1u << (pwShift + phShift);
            bits = bpp;
            mask = static_cast<uint32_t>(GSMem::MEMORY_SIZE) - bpp / 8u;
            return true;
        }

        uint32_t byteAddr(uint32_t x, uint32_t y) const
        {
            const uint32_t page = basePage + (y >> phShift) * pagesPerRow + (x >> pwShift);
            const uint32_t pixel = page * pixelsPerPage + table[blockOffset + ((y & ((1u << phShift) - 1u)) << pwShift) + (x & ((1u << pwShift) - 1u))];
            return ((pixel * bits) / 8u) & mask;
        }
    };

    struct TriangleRowsAvx2
    {
        const GSVertex *v[3] = {};
        const uint32_t *texels = nullptr;
        bool tme = false, fst = false, iip = false, linear = false, fge = false, ztest = false;
        uint32_t tfx = 0;
        bool tcc = false;
        __m256i texWm1, texHm1, stride, minU, maxU, minV, maxV;
        uint8_t wrapU = 0, wrapV = 0;
        __m256 texWf, texHf;
        __m256i flatColor;
        __m256i fogC[3];
        bool fastWrite = false;
        // Direct pixel path: CT32 frame, Z32/Z16/Z16S, no DATE, ATE off or AFAIL KEEP.
        bool direct = false, z16 = false, zmask = false, ate = false, abe = false, pabe = false, fbaForce = false;
        uint32_t ztestMethod = 0, atst = 0, aref = 0, fbmsk = 0;
        uint8_t asel = 0, bsel = 0, csel = 0, dsel = 0;
        __m256i fix;
        uint8_t *vram = nullptr;
        SwizzledBuffer fb, zb;

        bool init(const GSDrawState &state, const GSVertex *verts, const PixelPipe &pipe, const uint32_t *decoded, int decodedStride,
                  bool verifyDecoded, int minY, int maxY)
        {
            static const bool forceScalar = [] { const char *e = std::getenv("PS2X_GS_SCALAR"); return e && *e && *e != '0'; }();
            if (forceScalar || pipe.countStats)
                return false;
            tme = state.prim.tme != 0;
            if (tme && (!decoded || verifyDecoded))
                return false;
            ztest = pipe.ztestMethod >= 2u;
            // Frame and Z in disjoint memory (the block ranges only hold while x stays inside the
            // buffer width): needed by the batched depth test and by the direct path.
            bool separate = pipe.sx1 < static_cast<int>(pipe.fbw * 64u);
            if (separate)
            {
                uint64_t f0, f1, z0, z1;
                gsBlockRange(pipe.fpsm, pipe.fbp, pipe.fbw, minY, maxY, f0, f1);
                gsBlockRange(pipe.zpsm, pipe.zbp, pipe.fbw, minY, maxY, z0, z1);
                separate = !rangesOverlap(f0, f1, z0, z1);
            }
            if (ztest && !separate)
                return false;
            v[0] = &verts[0]; v[1] = &verts[1]; v[2] = &verts[2];
            texels = decoded;
            fst = state.prim.fst != 0;
            iip = state.prim.iip != 0;
            linear = state.linearFilter;
            fge = pipe.fge;
            fastWrite = pipe.canWriteTested();
            fogC[0] = _mm256_set1_epi32(pipe.fogR);
            fogC[1] = _mm256_set1_epi32(pipe.fogG);
            fogC[2] = _mm256_set1_epi32(pipe.fogB);
            static const bool noDirect = [] { const char *e = std::getenv("PS2X_GS_NODIRECT"); return e && *e && *e != '0'; }();
            ate = (pipe.test & 0x1u) != 0u;
            direct = !noDirect && separate && fastWrite && pipe.fpsm == GS_PSM_CT32 &&
                     (pipe.zpsm == GS_PSM_Z32 || pipe.zpsm == GS_PSM_Z16 || pipe.zpsm == GS_PSM_Z16S) &&
                     ((pipe.test >> 14) & 0x1u) == 0u && (!ate || ((pipe.test >> 12) & 0x3u) == 0u) &&
                     fb.init(pipe.fpsm, pipe.fbp, pipe.fbw) && zb.init(pipe.zpsm, pipe.zbp, pipe.fbw);
            if (direct)
            {
                vram = pipe.vram;
                z16 = pipe.zpsm != GS_PSM_Z32;
                zmask = pipe.zmask;
                ztestMethod = pipe.ztestMethod;
                atst = static_cast<uint32_t>((pipe.test >> 1) & 0x7u);
                aref = static_cast<uint32_t>((pipe.test >> 4) & 0xFFu);
                abe = pipe.abe;
                pabe = pipe.pabe;
                fbaForce = pipe.fbaForce;
                fbmsk = pipe.fbmsk;
                asel = pipe.asel;
                bsel = pipe.bsel;
                csel = pipe.csel;
                dsel = pipe.dsel;
                fix = _mm256_set1_epi32(pipe.fix);
            }
            tfx = state.context.tex0.tfx;
            tcc = state.context.tex0.tcc != 0u;
            const uint64_t clamp = state.context.clamp;
            wrapU = static_cast<uint8_t>(clamp & 0x3u);
            wrapV = static_cast<uint8_t>((clamp >> 2) & 0x3u);
            minU = _mm256_set1_epi32(static_cast<int>((clamp >> 4) & 0x3FFu));
            maxU = _mm256_set1_epi32(static_cast<int>((clamp >> 14) & 0x3FFu));
            minV = _mm256_set1_epi32(static_cast<int>((clamp >> 24) & 0x3FFu));
            maxV = _mm256_set1_epi32(static_cast<int>((clamp >> 34) & 0x3FFu));
            texWm1 = _mm256_set1_epi32(state.textureWidth - 1);
            texHm1 = _mm256_set1_epi32(state.textureHeight - 1);
            texWf = _mm256_set1_ps(static_cast<float>(state.textureWidth));
            texHf = _mm256_set1_ps(static_cast<float>(state.textureHeight));
            stride = _mm256_set1_epi32(decodedStride);
            flatColor = _mm256_set1_epi32(static_cast<int>(pack32(verts[2].r, verts[2].g, verts[2].b, verts[2].a)));
            return true;
        }

        // a0 * w0 + a1 * w1 + a2 * w2 as compiled: mul, fma, fma.
        static __m256 interp(float a0, float a1, float a2, __m256 w0, __m256 w1, __m256 w2)
        {
            __m256 r = _mm256_mul_ps(_mm256_set1_ps(a0), w0);
            r = _mm256_fmadd_ps(w1, _mm256_set1_ps(a1), r);
            return _mm256_fmadd_ps(w2, _mm256_set1_ps(a2), r);
        }

        // clampU8(static_cast<int>(f)): truncate, < 0 (also the 0x80000000 overflow value) -> 0, > 255 -> 255.
        static __m256i clampU8v(__m256 f)
        {
            return _mm256_max_epi32(_mm256_min_epi32(_mm256_cvttps_epi32(f), _mm256_set1_epi32(255)), _mm256_setzero_si256());
        }

        static __m256i wrap(__m256i c, uint8_t mode, __m256i sizeM1, __m256i rmin, __m256i rmax)
        {
            switch (mode)
            {
            case 0: return _mm256_and_si256(c, sizeM1);
            case 1: return _mm256_andnot_si256(_mm256_cmpgt_epi32(_mm256_setzero_si256(), c), _mm256_min_epi32(c, sizeM1));
            case 2: return _mm256_min_epi32(_mm256_max_epi32(c, rmin), rmax);
            default: return _mm256_or_si256(_mm256_and_si256(c, rmin), rmax);
            }
        }

        __m256i fetch(__m256i u, __m256i vv, __m256i mask) const
        {
            const __m256i idx = _mm256_add_epi32(_mm256_mullo_epi32(wrap(vv, wrapV, texHm1, minV, maxV), stride), wrap(u, wrapU, texWm1, minU, maxU));
            return _mm256_mask_i32gather_epi32(_mm256_setzero_si256(), reinterpret_cast<const int *>(texels), idx, mask, 4);
        }

        // lerpTexel for one byte channel of 8 texel quads.
        static __m256i lerpChannel8(__m256i c00, __m256i c10, __m256i c01, __m256i c11, int shift, __m256 fx, __m256 fy)
        {
            const __m256i m = _mm256_set1_epi32(0xFF);
            const auto ch = [&](__m256i c) { return _mm256_cvtepi32_ps(_mm256_and_si256(_mm256_srli_epi32(c, shift), m)); };
            const __m256 a00 = ch(c00), a10 = ch(c10), a01 = ch(c01), a11 = ch(c11);
            const __m256 top = _mm256_fmadd_ps(_mm256_sub_ps(a10, a00), fx, a00);
            const __m256 bottom = _mm256_fmadd_ps(_mm256_sub_ps(a11, a01), fx, a01);
            const __m256 value = _mm256_fmadd_ps(_mm256_sub_ps(bottom, top), fy, top);
            __m256i out = _mm256_cvttps_epi32(_mm256_add_ps(value, _mm256_set1_ps(0.5f)));
            out = _mm256_and_si256(out, _mm256_castps_si256(_mm256_cmp_ps(value, _mm256_setzero_ps(), _CMP_GT_OQ)));
            out = _mm256_min_epi32(out, m);
            return _mm256_max_epi32(out, _mm256_setzero_si256()); // packus: negative -> 0
        }

        // Texels (RGBA8888) of the lanes in `mask` for interpolated S/T/Q (or U/V with FST).
        __m256i sample(__m256 w0, __m256 w1, __m256 w2, __m256i mask) const
        {
            const GSVertex &a = *v[0], &b = *v[1], &c = *v[2];
            __m256 texU, texV;
            if (fst)
            {
                const __m256i lo16 = _mm256_set1_epi32(0xFFFF);
                const __m256i iu = _mm256_and_si256(_mm256_cvttps_epi32(interp(a.u, b.u, c.u, w0, w1, w2)), lo16);
                const __m256i iv = _mm256_and_si256(_mm256_cvttps_epi32(interp(a.v, b.v, c.v, w0, w1, w2)), lo16);
                texU = _mm256_mul_ps(_mm256_cvtepi32_ps(iu), _mm256_set1_ps(1.0f / 16.0f));
                texV = _mm256_mul_ps(_mm256_cvtepi32_ps(iv), _mm256_set1_ps(1.0f / 16.0f));
            }
            else
            {
                const __m256 s = interp(a.s, b.s, c.s, w0, w1, w2);
                const __m256 t = interp(a.t, b.t, c.t, w0, w1, w2);
                const __m256 q = interp(a.q, b.q, c.q, w0, w1, w2);
                const __m256 one = _mm256_set1_ps(1.0f);
                const __m256 absQ = _mm256_and_ps(q, _mm256_castsi256_ps(_mm256_set1_epi32(0x7FFFFFFF)));
                const __m256 qq = _mm256_blendv_ps(one, q, _mm256_cmp_ps(absQ, _mm256_set1_ps(1.0e-8f), _CMP_GT_OQ)); // fabsQ
                const __m256 invQ = _mm256_div_ps(one, qq);
                texU = _mm256_mul_ps(_mm256_mul_ps(invQ, s), texWf);
                texV = _mm256_mul_ps(_mm256_mul_ps(invQ, t), texHf);
            }
            if (!linear)
                return fetch(_mm256_cvttps_epi32(texU), _mm256_cvttps_epi32(texV), mask);

            const __m256 half = _mm256_set1_ps(0.5f);
            const __m256 sU = _mm256_sub_ps(texU, half);
            const __m256 sV = _mm256_sub_ps(texV, half);
            const __m256i u0 = _mm256_cvttps_epi32(_mm256_floor_ps(sU));
            const __m256i v0 = _mm256_cvttps_epi32(_mm256_floor_ps(sV));
            const __m256 fx = _mm256_sub_ps(sU, _mm256_cvtepi32_ps(u0));
            const __m256 fy = _mm256_sub_ps(sV, _mm256_cvtepi32_ps(v0));
            const __m256i one = _mm256_set1_epi32(1);
            const __m256i u1 = _mm256_add_epi32(u0, one), v1 = _mm256_add_epi32(v0, one);
            const __m256i c00 = fetch(u0, v0, mask), c10 = fetch(u1, v0, mask);
            const __m256i c01 = fetch(u0, v1, mask), c11 = fetch(u1, v1, mask);
            __m256i out = lerpChannel8(c00, c10, c01, c11, 0, fx, fy);
            out = _mm256_or_si256(out, _mm256_slli_epi32(lerpChannel8(c00, c10, c01, c11, 8, fx, fy), 8));
            out = _mm256_or_si256(out, _mm256_slli_epi32(lerpChannel8(c00, c10, c01, c11, 16, fx, fy), 16));
            return _mm256_or_si256(out, _mm256_slli_epi32(lerpChannel8(c00, c10, c01, c11, 24, fx, fy), 24));
        }

        // combineTexturePacked on 8 lanes (vertex colour and texel as packed RGBA8888).
        __m256i combine(__m256i vc, __m256i tc) const
        {
            const __m256i m = _mm256_set1_epi32(0xFF);
            const auto chan = [&](__m256i c, int i) { return _mm256_and_si256(_mm256_srli_epi32(c, 8 * i), m); };
            const auto sat = [&](__m256i x) { return _mm256_min_epu32(x, m); };
            const __m256i vr = chan(vc, 0), vg = chan(vc, 1), vb = chan(vc, 2), va = chan(vc, 3);
            const __m256i tr = chan(tc, 0), tg = chan(tc, 1), tb = chan(tc, 2), ta = chan(tc, 3);
            const auto mod = [](__m256i t, __m256i x) { return _mm256_srli_epi32(_mm256_mullo_epi16(t, x), 7); };
            __m256i r, g, b, a;
            switch (tfx)
            {
            case 0: // MODULATE
                r = sat(mod(tr, vr)); g = sat(mod(tg, vg)); b = sat(mod(tb, vb));
                a = tcc ? sat(mod(ta, va)) : va;
                break;
            case 2: // HIGHLIGHT
            case 3: // HIGHLIGHT2
                r = sat(_mm256_add_epi32(mod(tr, vr), va)); g = sat(_mm256_add_epi32(mod(tg, vg), va)); b = sat(_mm256_add_epi32(mod(tb, vb), va));
                a = !tcc ? va : (tfx == 2 ? sat(_mm256_add_epi32(ta, va)) : ta);
                break;
            default: // DECAL
                r = tr; g = tg; b = tb;
                a = tcc ? ta : va;
                break;
            }
            return _mm256_or_si256(_mm256_or_si256(r, _mm256_slli_epi32(g, 8)), _mm256_or_si256(_mm256_slli_epi32(b, 16), _mm256_slli_epi32(a, 24)));
        }

        // PixelPipe::writeTested for the lanes in `bits` (Z test passed, fog applied), direct state only.
        void writeDirect(int x, int y, unsigned bits, __m256i color, const int *zs, const uint32_t *zAddr) const
        {
            const __m256i m = _mm256_set1_epi32(0xFF);
            const __m256i a = _mm256_srli_epi32(color, 24);
            if (ate) // AFAIL KEEP: a failing pixel writes nothing
            {
                const __m256i ref = _mm256_set1_epi32(static_cast<int>(aref));
                const __m256i ones = _mm256_set1_epi32(-1);
                __m256i pass;
                switch (atst)
                {
                case 0: pass = _mm256_setzero_si256(); break;
                case 1: pass = ones; break;
                case 2: pass = _mm256_cmpgt_epi32(ref, a); break;
                case 3: pass = _mm256_xor_si256(_mm256_cmpgt_epi32(a, ref), ones); break;
                case 4: pass = _mm256_cmpeq_epi32(a, ref); break;
                case 5: pass = _mm256_xor_si256(_mm256_cmpgt_epi32(ref, a), ones); break;
                case 6: pass = _mm256_cmpgt_epi32(a, ref); break;
                default: pass = _mm256_xor_si256(_mm256_cmpeq_epi32(a, ref), ones); break;
                }
                bits &= static_cast<unsigned>(_mm256_movemask_ps(_mm256_castsi256_ps(pass)));
                if (bits == 0u)
                    return;
            }
            alignas(32) uint32_t addr[8];
            alignas(32) uint32_t dst[8] = {};
            for (unsigned rest = bits; rest != 0u; rest &= rest - 1u)
            {
                const int i = static_cast<int>(_tzcnt_u32(rest));
                addr[i] = fb.byteAddr(static_cast<uint32_t>(x + i), static_cast<uint32_t>(y));
            }
            __m256i pixel = color;
            if (abe || fbmsk != 0u)
            {
                for (unsigned rest = bits; rest != 0u; rest &= rest - 1u)
                {
                    const int i = static_cast<int>(_tzcnt_u32(rest));
                    std::memcpy(&dst[i], vram + addr[i], 4);
                }
                const __m256i d = _mm256_load_si256(reinterpret_cast<const __m256i *>(dst));
                if (abe)
                {
                    // clampU8(((A - B) * C >> 7) + D) per channel, A/B/D in {source, dest, 0}, C in {As, Ad, FIX}.
                    const __m256i zero = _mm256_setzero_si256();
                    const __m256i c = csel == 0 ? a : (csel == 1 ? _mm256_srli_epi32(d, 24) : fix);
                    __m256i blended = _mm256_and_si256(color, _mm256_set1_epi32(static_cast<int>(0xFF000000u)));
                    for (int ch = 0; ch < 3; ++ch)
                    {
                        const __m256i cs = _mm256_and_si256(_mm256_srli_epi32(color, 8 * ch), m);
                        const __m256i cd = _mm256_and_si256(_mm256_srli_epi32(d, 8 * ch), m);
                        const auto pick = [&](uint8_t sel) { return sel == 0 ? cs : (sel == 1 ? cd : zero); };
                        __m256i v = _mm256_srai_epi32(_mm256_mullo_epi32(_mm256_sub_epi32(pick(asel), pick(bsel)), c), 7);
                        v = _mm256_max_epi32(_mm256_min_epi32(_mm256_add_epi32(v, pick(dsel)), m), zero);
                        blended = _mm256_or_si256(blended, _mm256_slli_epi32(v, 8 * ch));
                    }
                    if (pabe) // A < 0x80: no blending
                    {
                        const __m256i low = _mm256_cmpeq_epi32(_mm256_and_si256(a, _mm256_set1_epi32(0x80)), zero);
                        pixel = _mm256_blendv_epi8(blended, color, low);
                    }
                    else
                        pixel = blended;
                }
                if (fbaForce)
                    pixel = _mm256_or_si256(pixel, _mm256_set1_epi32(static_cast<int>(0x80000000u)));
                if (fbmsk != 0u)
                {
                    const __m256i msk = _mm256_set1_epi32(static_cast<int>(fbmsk));
                    pixel = _mm256_or_si256(_mm256_andnot_si256(msk, pixel), _mm256_and_si256(d, msk));
                }
            }
            else if (fbaForce)
                pixel = _mm256_or_si256(pixel, _mm256_set1_epi32(static_cast<int>(0x80000000u)));
            alignas(32) uint32_t out[8];
            _mm256_store_si256(reinterpret_cast<__m256i *>(out), pixel);
            for (; bits != 0u; bits &= bits - 1u)
            {
                const int i = static_cast<int>(_tzcnt_u32(bits));
                std::memcpy(vram + addr[i], &out[i], 4);
                if (!zmask)
                {
                    if (z16)
                    {
                        const uint16_t v16 = static_cast<uint16_t>(zs[i]);
                        std::memcpy(vram + zAddr[i], &v16, 2);
                    }
                    else
                        std::memcpy(vram + zAddr[i], &zs[i], 4);
                }
            }
        }

        void drawRow(const PixelPipe &pipe, int y, float py, int x0, int x1, float fx0, float fy0, float fx1, float fy1, float fx2, float fy2,
                     float winding, float invAbsDenom, float eps) const
        {
            const float dy = py - fy2;
            const __m256 A0 = _mm256_set1_ps(fy1 - fy2), A1 = _mm256_set1_ps(fy2 - fy0);
            const __m256 B0dy = _mm256_set1_ps((fx2 - fx1) * dy), B1dy = _mm256_set1_ps((fx0 - fx2) * dy);
            const __m256 vfx2 = _mm256_set1_ps(fx2), vWind = _mm256_set1_ps(winding), vInv = _mm256_set1_ps(invAbsDenom);
            const __m256 one = _mm256_set1_ps(1.0f), negEps = _mm256_set1_ps(-eps), half = _mm256_set1_ps(0.5f);
            const __m256d z0 = _mm256_set1_pd(v[0]->z), z1 = _mm256_set1_pd(v[1]->z), z2 = _mm256_set1_pd(v[2]->z);
            const __m256d halfD = _mm256_set1_pd(0.5), lim = _mm256_set1_pd(2147483648.0), negLim = _mm256_set1_pd(-2147483648.0);
            const __m256i lane = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
            alignas(32) int zs[8];
            alignas(32) uint32_t colors[8];
            alignas(32) int fogs[8] = {};
            alignas(32) double zd[8];
            uint32_t zAddr[8];
            for (int x = x0; x <= x1; x += 8)
            {
                const __m256 px = _mm256_add_ps(_mm256_cvtepi32_ps(_mm256_add_epi32(_mm256_set1_epi32(x), lane)), half);
                const __m256 dx = _mm256_sub_ps(px, vfx2);
                const __m256 w0 = _mm256_mul_ps(_mm256_mul_ps(_mm256_fmadd_ps(dx, A0, B0dy), vWind), vInv);
                const __m256 w1 = _mm256_mul_ps(_mm256_mul_ps(_mm256_fmadd_ps(A1, dx, B1dy), vWind), vInv);
                const __m256 w2 = _mm256_sub_ps(_mm256_sub_ps(one, w0), w1);
                const __m256 inside = _mm256_and_ps(_mm256_and_ps(_mm256_cmp_ps(w0, negEps, _CMP_GE_OQ), _mm256_cmp_ps(w1, negEps, _CMP_GE_OQ)),
                                                    _mm256_cmp_ps(w2, negEps, _CMP_GE_OQ));
                const int n = x1 - x + 1;
                unsigned bits = static_cast<unsigned>(_mm256_movemask_ps(inside)) & (n >= 8 ? 0xFFu : ((1u << n) - 1u));
                if (bits == 0u)
                    continue;

                // z = w0 * z0, fma(w1, z1), fma(w2, z2) in double, + 0.5, truncated through int64.
                const auto zHalf = [&](__m128 a, __m128 b, __m128 c) {
                    __m256d z = _mm256_mul_pd(_mm256_cvtps_pd(a), z0);
                    z = _mm256_fmadd_pd(_mm256_cvtps_pd(b), z1, z);
                    z = _mm256_fmadd_pd(_mm256_cvtps_pd(c), z2, z);
                    return _mm256_add_pd(z, halfD);
                };
                const __m256d zLo = zHalf(_mm256_castps256_ps128(w0), _mm256_castps256_ps128(w1), _mm256_castps256_ps128(w2));
                const __m256d zHi = zHalf(_mm256_extractf128_ps(w0, 1), _mm256_extractf128_ps(w1, 1), _mm256_extractf128_ps(w2, 1));
                _mm256_store_si256(reinterpret_cast<__m256i *>(zs), _mm256_set_m128i(_mm256_cvttpd_epi32(zHi), _mm256_cvttpd_epi32(zLo)));
                const unsigned inRange = static_cast<unsigned>(
                    _mm256_movemask_pd(_mm256_and_pd(_mm256_cmp_pd(zLo, lim, _CMP_LT_OQ), _mm256_cmp_pd(zLo, negLim, _CMP_GE_OQ))) |
                    (_mm256_movemask_pd(_mm256_and_pd(_mm256_cmp_pd(zHi, lim, _CMP_LT_OQ), _mm256_cmp_pd(zHi, negLim, _CMP_GE_OQ))) << 4));
                if ((bits & ~inRange) != 0u)
                {
                    _mm256_store_pd(zd, zLo);
                    _mm256_store_pd(zd + 4, zHi);
                    for (unsigned rest = bits & ~inRange; rest != 0u; rest &= rest - 1u)
                    {
                        const int i = static_cast<int>(_tzcnt_u32(rest));
                        zs[i] = static_cast<int>(static_cast<uint32_t>(_mm_cvttsd_si64(_mm_set_sd(zd[i]))));
                    }
                }
                if (direct)
                {
                    for (unsigned rest = bits; rest != 0u; rest &= rest - 1u)
                    {
                        const int i = static_cast<int>(_tzcnt_u32(rest));
                        zAddr[i] = zb.byteAddr(static_cast<uint32_t>(x + i), static_cast<uint32_t>(y));
                        if (ztestMethod >= 2u)
                        {
                            uint32_t old;
                            if (z16)
                            {
                                uint16_t v16;
                                std::memcpy(&v16, vram + zAddr[i], 2);
                                old = v16;
                            }
                            else
                                std::memcpy(&old, vram + zAddr[i], 4);
                            if (ztestMethod == 2u ? static_cast<uint32_t>(zs[i]) < old : static_cast<uint32_t>(zs[i]) <= old)
                                bits &= ~(1u << i);
                        }
                    }
                    if (bits == 0u)
                        continue;
                }
                else if (ztest)
                {
                    for (unsigned rest = bits; rest != 0u; rest &= rest - 1u)
                    {
                        const int i = static_cast<int>(_tzcnt_u32(rest));
                        if (pipe.failsDepth(x + i, y, zs[i]))
                            bits &= ~(1u << i);
                    }
                    if (bits == 0u)
                        continue;
                }

                __m256i color = flatColor;
                if (iip)
                {
                    const GSVertex &a = *v[0], &b = *v[1], &c = *v[2];
                    color = _mm256_or_si256(
                        _mm256_or_si256(clampU8v(interp(a.r, b.r, c.r, w0, w1, w2)), _mm256_slli_epi32(clampU8v(interp(a.g, b.g, c.g, w0, w1, w2)), 8)),
                        _mm256_or_si256(_mm256_slli_epi32(clampU8v(interp(a.b, b.b, c.b, w0, w1, w2)), 16), _mm256_slli_epi32(clampU8v(interp(a.a, b.a, c.a, w0, w1, w2)), 24)));
                }
                if (tme)
                {
                    const __m256i laneBits = _mm256_setr_epi32(1, 2, 4, 8, 16, 32, 64, 128);
                    const __m256i mask = _mm256_cmpeq_epi32(_mm256_and_si256(_mm256_set1_epi32(static_cast<int>(bits)), laneBits), laneBits);
                    color = combine(color, sample(w0, w1, w2, mask));
                }
                if (fastWrite)
                {
                    if (fge) // write()'s fog: c = ((fog * c) >> 8) + (((255 - fog) * fogC) >> 8) for R, G, B
                    {
                        const __m256i fog = clampU8v(interp(v[0]->fog, v[1]->fog, v[2]->fog, w0, w1, w2));
                        const __m256i inv = _mm256_sub_epi32(_mm256_set1_epi32(255), fog);
                        const __m256i m = _mm256_set1_epi32(0xFF);
                        __m256i out = _mm256_andnot_si256(_mm256_set1_epi32(0x00FFFFFF), color);
                        for (int i = 0; i < 3; ++i)
                        {
                            const __m256i c = _mm256_and_si256(_mm256_srli_epi32(color, 8 * i), m);
                            const __m256i f = _mm256_add_epi32(_mm256_srli_epi32(_mm256_mullo_epi16(fog, c), 8),
                                                               _mm256_srli_epi32(_mm256_mullo_epi16(inv, fogC[i]), 8));
                            out = _mm256_or_si256(out, _mm256_slli_epi32(_mm256_and_si256(f, m), 8 * i));
                        }
                        color = out;
                    }
                    if (direct)
                    {
                        writeDirect(x, y, bits, color, zs, zAddr);
                        continue;
                    }
                    _mm256_store_si256(reinterpret_cast<__m256i *>(colors), color);
                    for (; bits != 0u; bits &= bits - 1u)
                    {
                        const int i = static_cast<int>(_tzcnt_u32(bits));
                        pipe.writeTested(x + i, y, zs[i], colors[i]);
                    }
                    continue;
                }
                _mm256_store_si256(reinterpret_cast<__m256i *>(colors), color);
                if (fge)
                    _mm256_store_si256(reinterpret_cast<__m256i *>(fogs), clampU8v(interp(v[0]->fog, v[1]->fog, v[2]->fog, w0, w1, w2)));
                for (; bits != 0u; bits &= bits - 1u)
                {
                    const int i = static_cast<int>(_tzcnt_u32(bits));
                    const uint32_t c = colors[i];
                    pipe.write(x + i, y, zs[i], static_cast<uint8_t>(c), static_cast<uint8_t>(c >> 8), static_cast<uint8_t>(c >> 16),
                               static_cast<uint8_t>(c >> 24), static_cast<uint8_t>(fogs[i]));
                }
            }
        }
    };
#endif
}

void GSCpuBackend::InvalidateDecodedTextures()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    InvalidateDecodedTexturesUnlocked();
}

void GSCpuBackend::InvalidateDecodedTexturesUnlocked()
{
    if (!m_hasDecodedTextures)
        return;
    for (DecodedTexture &entry : m_decodedTextures)
        entry.decoded = false;
    m_hasDecodedTextures = false;
}

void GSCpuBackend::InvalidateDecodedTexturesInBlocks(uint64_t first, uint64_t last)
{
    if (!m_hasDecodedTextures)
        return;
    bool any = false;
    for (DecodedTexture &entry : m_decodedTextures)
    {
        if (entry.decoded && rangesOverlap(entry.blockFirst, entry.blockLast, first, last))
            entry.decoded = false;
        any = any || entry.decoded;
    }
    m_hasDecodedTextures = any;
}

void GSCpuBackend::NoteTransferStart(const GSTransferCommand &command)
{
    m_transferWrites = transferDestinationBlocks(command, m_transferWriteFirst, m_transferWriteLast);
    if (m_transferWrites)
        InvalidateDecodedTexturesInBlocks(m_transferWriteFirst, m_transferWriteLast);
}

void GSCpuBackend::NoteTransferData()
{
    if (m_transferWrites)
        InvalidateDecodedTexturesInBlocks(m_transferWriteFirst, m_transferWriteLast);
}

void GSCpuBackend::NoteSkippedTransfer(const GSTransferCommand &command)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    NoteTransferStart(command);
}

void GSCpuBackend::CopyClutStateFrom(const GSCpuBackend &other)
{
    std::scoped_lock lock(m_mutex, other.m_mutex);
    m_clut = other.m_clut;
    m_clutCbp = other.m_clutCbp;
}

void GSCpuBackend::NoteSkippedUpload()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    NoteTransferData();
}

void GSCpuBackend::InvalidateDecodedTexturesWrittenBy(const GSPrimitiveBatch &batch)
{
    // Every band instance runs every draw (for its own rows), so each one sees the writes that
    // can change a texture it decoded, including those of other bands.
    const GSContext &ctx = batch.state.context;
    const int y0 = ctx.scissor.y0;
    const int y1 = std::max<int>(ctx.scissor.y0, ctx.scissor.y1);
    uint64_t first = 0, last = 0;
    if (ctx.frame.fbmsk != 0xFFFFFFFFu)
    {
        gsBlockRange(ctx.frame.psm, GSInternal::framePageBaseToBlock(ctx.frame.fbp), ctx.frame.fbw, y0, y1, first, last);
        InvalidateDecodedTexturesInBlocks(first, last);
    }
    if (!ctx.zbuf.zmask)
    {
        gsBlockRange(ctx.zbuf.psm, GSInternal::framePageBaseToBlock(ctx.zbuf.zbp), ctx.frame.fbw, y0, y1, first, last);
        InvalidateDecodedTexturesInBlocks(first, last);
    }
}

const uint32_t *GSCpuBackend::DecodedTriangleTexture(const GSDrawState &state, int minY, int maxY, uint64_t samples)
{
    static const bool off = []
    {
        const char *noDecode = std::getenv("PS2X_GS_NODECODE");
        const char *pageCache = std::getenv("PS2X_GS_TEXCACHE");
        return (noDecode && *noDecode && *noDecode != '0') || (pageCache && *pageCache && *pageCache != '0');
    }();
    if (off)
        return nullptr;

    const GSContext &ctx = state.context;
    const GSTex0Reg &tex = ctx.tex0;
    const int width = state.textureWidth;
    const int height = state.textureHeight;
    const uint64_t area = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
    if (width <= 0 || height <= 0 || area > 512u * 512u)
        return nullptr;
    const uint64_t clamp = ctx.clamp;
    if (!wrapStaysInTexture(static_cast<uint8_t>(clamp & 0x3u), static_cast<uint16_t>((clamp >> 4) & 0x3FFu),
                            static_cast<uint16_t>((clamp >> 14) & 0x3FFu), width) ||
        !wrapStaysInTexture(static_cast<uint8_t>((clamp >> 2) & 0x3u), static_cast<uint16_t>((clamp >> 24) & 0x3FFu),
                            static_cast<uint16_t>((clamp >> 34) & 0x3FFu), height))
        return nullptr;

    // Conservative block range: a texture wider than TBW still addresses whole page rows.
    const uint32_t bw = std::max<uint32_t>(tex.tbw, static_cast<uint32_t>((width + 63) / 64));
    uint64_t t0 = 0, t1 = 0;
    gsBlockRange(tex.psm, tex.tbp0, bw, 0, height - 1, t0, t1);
    // A triangle that samples what it draws into must see its own writes: read VRAM directly.
    uint64_t f0 = 0, f1 = 0;
    if (ctx.frame.fbmsk != 0xFFFFFFFFu)
    {
        gsBlockRange(ctx.frame.psm, GSInternal::framePageBaseToBlock(ctx.frame.fbp), ctx.frame.fbw, minY, maxY, f0, f1);
        if (rangesOverlap(t0, t1, f0, f1))
            return nullptr;
    }
    if (!ctx.zbuf.zmask)
    {
        gsBlockRange(ctx.zbuf.psm, GSInternal::framePageBaseToBlock(ctx.zbuf.zbp), ctx.frame.fbw, minY, maxY, f0, f1);
        if (rangesOverlap(t0, t1, f0, f1))
            return nullptr;
    }

    DecodedTextureKey key;
    key.tbp0 = tex.tbp0;
    key.width = static_cast<uint16_t>(width);
    key.height = static_cast<uint16_t>(height);
    key.tbw = tex.tbw;
    key.psm = tex.psm;
    if (isFourBitIndexedPsm(tex.psm) || isEightBitIndexedPsm(tex.psm))
    {
        key.cpsm = tex.cpsm;
        key.csm = tex.csm;
        key.csa = tex.csa;
    }
    key.ta0 = state.texa.ta0;
    key.ta1 = state.texa.ta1;
    key.aem = state.texa.aem;

    ++m_decodedUseClock;
    DecodedTexture *entry = nullptr;
    for (DecodedTexture &candidate : m_decodedTextures)
        if (candidate.used && candidate.key == key)
        {
            entry = &candidate;
            break;
        }
    if (!entry)
    {
        entry = &m_decodedTextures[0];
        for (DecodedTexture &candidate : m_decodedTextures)
        {
            if (!candidate.used)
            {
                entry = &candidate;
                break;
            }
            if (candidate.lastUse < entry->lastUse)
                entry = &candidate;
        }
        entry->key = key;
        entry->used = true;
        entry->decoded = false;
        entry->samples = 0;
    }
    entry->lastUse = m_decodedUseClock;
    if (entry->decoded)
        return entry->texels.data();

    // Decode once the triangles using this texture have taken about half its texels in samples
    // (counted since it was last decoded), so one-off draws keep the direct path.
    entry->samples += samples;
    if (entry->samples * 2u < area)
        return nullptr;
    entry->texels.resize(static_cast<size_t>(area));
    uint32_t *out = entry->texels.data();
    for (int v = 0; v < height; ++v)
        for (int u = 0; u < width; ++u)
            *out++ = DecodeTexel(state, u, v);
    entry->blockFirst = t0;
    entry->blockLast = t1;
    entry->decoded = true;
    entry->samples = 0;
    m_hasDecodedTextures = true;
    return entry->texels.data();
}

void GSCpuBackend::DrawSprite(const GSPrimitiveBatch &batch)
{
    const GSDrawState &state = batch.state;
    const GSVertex &v0 = batch.vertices[0];
    const GSVertex &v1 = batch.vertices[1];
    const auto &ctx = state.context;

    int ofx = ctx.xyoffset.ofx >> 4;
    int ofy = ctx.xyoffset.ofy >> 4;

    int x0 = static_cast<int>(v0.x) - ofx;
    int y0 = static_cast<int>(v0.y) - ofy;
    int x1 = static_cast<int>(v1.x) - ofx;
    int y1 = static_cast<int>(v1.y) - ofy;
    u32 z1 = static_cast<u32>(v1.z);

    if (x0 > x1)
        std::swap(x0, x1);
    if (y0 > y1)
        std::swap(y0, y1);

    const int unclippedX0 = x0;
    const int unclippedY0 = y0;
    const int spanX = std::max(1, x1 - x0);
    const int spanY = std::max(1, y1 - y0);
    const int unclippedX1 = unclippedX0 + spanX - 1;
    const int unclippedY1 = unclippedY0 + spanY - 1;

    // If the sprite rectangle is fully outside scissor, nothing should render.
    if (unclippedX1 < ctx.scissor.x0 || unclippedX0 > ctx.scissor.x1 ||
        unclippedY1 < ctx.scissor.y0 || unclippedY0 > ctx.scissor.y1)
        return;

    const int drawX0 = clampInt(unclippedX0, ctx.scissor.x0, ctx.scissor.x1);
    const int drawY0 = clampInt(unclippedY0, ctx.scissor.y0, ctx.scissor.y1);
    const int drawX1 = clampInt(unclippedX1, ctx.scissor.x0, ctx.scissor.x1);
    const int drawY1 = clampInt(unclippedY1, ctx.scissor.y0, ctx.scissor.y1);

    // Large sprites are drawn row-parallel unless the texture may overlap what this sprite
    // writes (then a row could read texels another row already changed), or the legacy
    // page-copy texture cache is enabled (it is not thread-safe).
    auto spriteTextureIsSeparate = [&]() -> bool
    {
        static const bool pageCache = [] { const char *e = std::getenv("PS2X_GS_TEXCACHE"); return e && *e && *e != '0'; }();
        if (pageCache)
            return false;
        const auto &tex0 = ctx.tex0;
        uint64_t t0, t1, f0, f1, z0, zEnd;
        gsBlockRange(tex0.psm, tex0.tbp0, tex0.tbw, 0, std::max<int>(state.textureHeight, 1) - 1, t0, t1);
        gsBlockRange(ctx.frame.psm, GSInternal::framePageBaseToBlock(ctx.frame.fbp), ctx.frame.fbw, drawY0, drawY1, f0, f1);
        if (rangesOverlap(t0, t1, f0, f1))
            return false;
        const bool zWrites = !ctx.zbuf.zmask;
        if (zWrites)
        {
            gsBlockRange(ctx.zbuf.psm, GSInternal::framePageBaseToBlock(ctx.zbuf.zbp), ctx.frame.fbw, drawY0, drawY1, z0, zEnd);
            if (rangesOverlap(t0, t1, z0, zEnd))
                return false;
        }
        return true;
    };
    auto spriteCanRunParallel = [&](bool textured) -> bool
    {
        if (m_bandCount > 1 || GsRowPool::instance().workers() == 0)
            return false; // banded instances already run in parallel; the row pool is shared
        const int64_t area = static_cast<int64_t>(drawX1 - drawX0 + 1) * static_cast<int64_t>(drawY1 - drawY0 + 1);
        if (area < 4096 || drawY1 - drawY0 < 8)
            return false;
        return !textured || spriteTextureIsSeparate();
    };

    {
        // PS2X_SPRITE_STATS=1: once per second, the sprite setups that drew the most pixels.
        static const bool spriteStats = [] { const char *e = std::getenv("PS2X_SPRITE_STATS"); return e && *e && *e != '0'; }();
        if (spriteStats)
        {
            static std::map<std::string, uint64_t> pixels;
            static auto last = std::chrono::steady_clock::now();
            char key[200];
            std::snprintf(key, sizeof(key), "tme=%u fst=%u psm=%x tfx=%u tcc=%u lin=%d abe=%u alpha=%llx fge=%u test=%llx fbp=%u fpsm=%x fbmsk=%x tw=%d th=%d",
                          (unsigned)state.prim.tme, (unsigned)state.prim.fst, (unsigned)ctx.tex0.psm, (unsigned)ctx.tex0.tfx, (unsigned)ctx.tex0.tcc,
                          state.linearFilter ? 1 : 0, (unsigned)state.prim.abe, (unsigned long long)ctx.alpha, (unsigned)state.prim.fge,
                          (unsigned long long)ctx.test, (unsigned)ctx.frame.fbp, (unsigned)ctx.frame.psm, (unsigned)ctx.frame.fbmsk,
                          (int)state.textureWidth, (int)state.textureHeight);
            pixels[key] += static_cast<uint64_t>(drawX1 - drawX0 + 1) * static_cast<uint64_t>(drawY1 - drawY0 + 1);
            const auto now = std::chrono::steady_clock::now();
            if (now - last >= std::chrono::seconds(1))
            {
                std::vector<std::pair<uint64_t, std::string>> sorted;
                for (auto &kv : pixels) sorted.push_back({kv.second, kv.first});
                std::sort(sorted.rbegin(), sorted.rend());
                for (size_t i = 0; i < sorted.size() && i < 4; ++i)
                    std::fprintf(stderr, "[sprites] %llu px  %s\n", (unsigned long long)sorted[i].first, sorted[i].second.c_str());
                pixels.clear();
                last = now;
            }
        }
    }

    const uint64_t alphaReg = ctx.alpha;
    const uint8_t alphaMode = static_cast<uint8_t>(alphaReg & 0xFFu);
    const uint8_t alphaFix = static_cast<uint8_t>((alphaReg >> 32) & 0xFFu);

    uint8_t r = v1.r, g = v1.g, b = v1.b, a = v1.a;

    if (state.prim.tme)
    {
        const auto &tex = ctx.tex0;
        const int texW = state.textureWidth;
        const int texH = state.textureHeight;

        float u0f, v0f, u1f, v1f;
        if (state.prim.fst)
        {
            u0f = static_cast<float>(v0.u >> 4);
            v0f = static_cast<float>(v0.v >> 4);
            u1f = static_cast<float>(v1.u >> 4);
            v1f = static_cast<float>(v1.v >> 4);
        }
        else
        {
            const float q0 = fabsQ(v0.q);
            const float q1 = fabsQ(v1.q);
            u0f = (v0.s / q0) * static_cast<float>(texW);
            v0f = (v0.t / q0) * static_cast<float>(texH);
            u1f = (v1.s / q1) * static_cast<float>(texW);
            v1f = (v1.t / q1) * static_cast<float>(texH);
        }

        float spriteW = static_cast<float>(spanX);
        float spriteH = static_cast<float>(spanY);
        if (spriteW < 1.0f)
            spriteW = 1.0f;
        if (spriteH < 1.0f)
            spriteH = 1.0f;

        // Fast path for the common "copy a texture to the screen" sprite (movie frames, UI,
        // full-screen passes): point sampling, 32-bit framebuffer, no blending, fog, masks or
        // depth/alpha tests. Produces exactly what the generic loop below produces, but the
        // texture coordinates depend only on x (U) or y (V), so they are computed once per
        // column/row and the per-pixel WritePixel/SampleTexture state decoding is skipped.
        const uint32_t fpsm = ctx.frame.psm;
        const bool fastSprite =
            !state.linearFilter && !state.prim.fge && !state.prim.abe &&
            ctx.frame.fbmsk == 0u &&
            (ctx.test & 0x1u) == 0u &&                // alpha test off
            ((ctx.test >> 14) & 0x1u) == 0u &&        // destination alpha test off
            ((ctx.test >> 17) & 0x3u) == 1u &&        // depth test ALWAYS
            fpsm == GS_PSM_CT32 &&
            (tex.psm == GS_PSM_CT32 || tex.psm == GS_PSM_CT24) &&
            m_vram != nullptr && drawX1 >= drawX0;
        if (fastSprite)
        {
            const uint64_t clamp = ctx.clamp;
            const uint8_t wrapU = static_cast<uint8_t>(clamp & 0x3u);
            const uint8_t wrapV = static_cast<uint8_t>((clamp >> 2) & 0x3u);
            const uint16_t minU = static_cast<uint16_t>((clamp >> 4) & 0x3FFu);
            const uint16_t maxU = static_cast<uint16_t>((clamp >> 14) & 0x3FFu);
            const uint16_t minV = static_cast<uint16_t>((clamp >> 24) & 0x3FFu);
            const uint16_t maxV = static_cast<uint16_t>((clamp >> 34) & 0x3FFu);

            auto texCoord = [&](float texF, int texSize) -> int
            {
                if (state.prim.fst)
                {
                    const int fixed = static_cast<int>((texF * 16.0f) + 0.5f);
                    const uint16_t sample = static_cast<uint16_t>(clampInt(fixed, 0, 0xFFFF));
                    return static_cast<int>(static_cast<float>(sample) / 16.0f);
                }
                // Same arithmetic as SampleTexture(s = texF / size, q = 1).
                const float st = texF / static_cast<float>(texSize);
                const float invQ = 1.0f / fabsQ(1.0f);
                return static_cast<int>(st * invQ * static_cast<float>(texSize));
            };

            thread_local std::vector<int> columnU;
            columnU.resize(static_cast<size_t>(drawX1 - drawX0 + 1));
            for (int x = drawX0; x <= drawX1; ++x)
            {
                const float tx = (static_cast<float>(x - unclippedX0) + 0.5f) / spriteW;
                const float texUf = u0f + (u1f - u0f) * tx;
                columnU[static_cast<size_t>(x - drawX0)] = wrapTextureCoordinate(texCoord(texUf, texW), texW, wrapU, minU, maxU);
            }

            const u32 fbp = GSInternal::framePageBaseToBlock(ctx.frame.fbp);
            const u32 fbw = std::max<u32>(ctx.frame.fbw, 1u);
            const u32 zbp = GSInternal::framePageBaseToBlock(ctx.zbuf.zbp);
            const u32 zpsm = ctx.zbuf.psm;
            const bool writeZ = !ctx.zbuf.zmask;
            const bool forceAlphaMsb = (ctx.fba & 0x1ull) != 0ull;

            for (int y = drawY0; y <= drawY1; ++y)
            {
                const float ty = (static_cast<float>(y - unclippedY0) + 0.5f) / spriteH;
                const float texVf = v0f + (v1f - v0f) * ty;
                const int sampleV = wrapTextureCoordinate(texCoord(texVf, texH), texH, wrapV, minV, maxV);
                for (int x = drawX0; x <= drawX1; ++x)
                {
                    const u32 raw = ReadTextureVramUnlocked(tex.psm, tex.tbp0, tex.tbw,
                                                            columnU[static_cast<size_t>(x - drawX0)], sampleV);
                    const uint32_t texel = applyTexa(state.texa, tex.psm, raw);
                    const TextureCombineResult color = combineTexture(tex, r, g, b, a,
                                                                      static_cast<uint8_t>(texel & 0xFF),
                                                                      static_cast<uint8_t>((texel >> 8) & 0xFF),
                                                                      static_cast<uint8_t>((texel >> 16) & 0xFF),
                                                                      static_cast<uint8_t>((texel >> 24) & 0xFF));
                    const uint8_t outA = forceAlphaMsb ? static_cast<uint8_t>(color.a | 0x80u) : color.a;
                    WriteVramUnlocked(fpsm, fbp, fbw, x, y, pack32(color.r, color.g, color.b, outA));
                    if (writeZ)
                        WriteVramUnlocked(zpsm, zbp, fbw, x, y, z1);
                }
            }
            return;
        }

        // Texture coordinates of a sprite depend only on x (U) or only on y (V). Precompute the
        // wrapped texel columns/rows and filter weights once, with exactly the arithmetic
        // SampleTexture() uses, instead of redoing it for every pixel.
        struct TexAxis
        {
            int c0 = 0, c1 = 0; // wrapped texel coordinates (c1 only used when filtering)
            float frac = 0.0f;  // bilinear weight
        };
        const uint64_t texClamp = ctx.clamp;
        const uint8_t texWrapU = static_cast<uint8_t>(texClamp & 0x3u);
        const uint8_t texWrapV = static_cast<uint8_t>((texClamp >> 2) & 0x3u);
        const uint16_t texMinU = static_cast<uint16_t>((texClamp >> 4) & 0x3FFu);
        const uint16_t texMaxU = static_cast<uint16_t>((texClamp >> 14) & 0x3FFu);
        const uint16_t texMinV = static_cast<uint16_t>((texClamp >> 24) & 0x3FFu);
        const uint16_t texMaxV = static_cast<uint16_t>((texClamp >> 34) & 0x3FFu);
        const bool texLinear = state.linearFilter;
        auto makeAxis = [&](float texF, int texSize, uint8_t wrap, uint16_t minC, uint16_t maxC) -> TexAxis
        {
            float coord;
            if (state.prim.fst)
            {
                const int fixed = static_cast<int>((texF * 16.0f) + 0.5f);
                const uint16_t sample = static_cast<uint16_t>(clampInt(fixed, 0, 0xFFFF));
                coord = static_cast<float>(sample) / 16.0f;
            }
            else
            {
                const float st = texF / static_cast<float>(texSize);
                const float invQ = 1.0f / fabsQ(1.0f);
                coord = st * invQ * static_cast<float>(texSize);
            }
            TexAxis axis;
            if (!texLinear)
            {
                axis.c0 = wrapTextureCoordinate(static_cast<int>(coord), texSize, wrap, minC, maxC);
                return axis;
            }
            const float sample = coord - 0.5f;
            const int c0 = static_cast<int>(std::floor(sample));
            axis.frac = sample - static_cast<float>(c0);
            axis.c0 = wrapTextureCoordinate(c0, texSize, wrap, minC, maxC);
            axis.c1 = wrapTextureCoordinate(c0 + 1, texSize, wrap, minC, maxC);
            return axis;
        };
        auto texelAt = [&](int u, int v) -> uint32_t
        {
            const u32 out = ReadTextureVramUnlocked(tex.psm, tex.tbp0, tex.tbw, u, v);
            switch (tex.psm)
            {
            case GS_PSM_CT32:
            case GS_PSM_Z32:
            case GS_PSM_CT24:
            case GS_PSM_Z24:
                return applyTexa(state.texa, tex.psm, out);
            case GS_PSM_CT16:
            case GS_PSM_CT16S:
            case GS_PSM_Z16:
            case GS_PSM_Z16S:
                return applyTexa(state.texa, tex.psm, Rgba5551ToRgba8888(out));
            case GS_PSM_T8:
            case GS_PSM_T8H:
            case GS_PSM_T4:
            case GS_PSM_T4HL:
            case GS_PSM_T4HH:
                return LookupCLUT(state, static_cast<u8>(out), tex.cpsm, tex.csm, tex.csa, tex.psm);
            }
            return 0xFFFF00FFu;
        };
        PixelPipe pipe;
        pipe.init(state, m_vram, m_readVramFuncs.data(), m_writeVramFuncs.data());
        std::vector<TexAxis> columns(static_cast<size_t>(std::max(0, drawX1 - drawX0 + 1)));
        for (int x = drawX0; x <= drawX1; ++x)
        {
            const float tx = (static_cast<float>(x - unclippedX0) + 0.5f) / spriteW;
            columns[static_cast<size_t>(x - drawX0)] = makeAxis(u0f + (u1f - u0f) * tx, texW, texWrapU, texMinU, texMaxU);
        }

        std::vector<TexAxis> rowAxes(static_cast<size_t>(std::max(0, drawY1 - drawY0 + 1)));
        for (int y = drawY0; y <= drawY1; ++y)
        {
            const float rowTy = (static_cast<float>(y - unclippedY0) + 0.5f) / spriteH;
            rowAxes[static_cast<size_t>(y - drawY0)] = makeAxis(v0f + (v1f - v0f) * rowTy, texH, texWrapV, texMinV, texMaxV);
        }

        // Decode the texels this sprite samples once (unswizzle + CLUT + TEXA) into a linear
        // buffer, when that region is not much bigger than the sprite. Bilinear sprites read
        // each texel ~4 times, so this removes most of the address/CLUT work. Only when the
        // texture does not overlap what the sprite writes (otherwise rows must see each
        // other's writes, as in serial direct reads).
        int decU0 = 0, decV0 = 0, decW = 0, decH = 0;
        std::vector<uint32_t> decoded;
        {
            int uMin = (std::numeric_limits<int>::max)(), uMax = (std::numeric_limits<int>::min)(), vMin = (std::numeric_limits<int>::max)(), vMax = (std::numeric_limits<int>::min)();
            for (const TexAxis &c : columns)
            {
                uMin = std::min(uMin, c.c0); uMax = std::max(uMax, c.c0);
                if (texLinear) { uMin = std::min(uMin, c.c1); uMax = std::max(uMax, c.c1); }
            }
            for (const TexAxis &rw : rowAxes)
            {
                vMin = std::min(vMin, rw.c0); vMax = std::max(vMax, rw.c0);
                if (texLinear) { vMin = std::min(vMin, rw.c1); vMax = std::max(vMax, rw.c1); }
            }
            const int64_t pixelCount = static_cast<int64_t>(columns.size()) * static_cast<int64_t>(rowAxes.size());
            const int64_t region = (uMax >= uMin && vMax >= vMin)
                                       ? static_cast<int64_t>(uMax - uMin + 1) * static_cast<int64_t>(vMax - vMin + 1)
                                       : -1;
            static const bool decodeOff = [] { const char *e = std::getenv("PS2X_GS_NODECODE"); return e && *e && *e != '0'; }();
            if (!decodeOff && region > 0 && pixelCount >= 1024 && region <= pixelCount * (texLinear ? 3 : 1) + 1024 &&
                region <= 2048 * 2048 && spriteTextureIsSeparate())
            {
                decU0 = uMin; decV0 = vMin; decW = uMax - uMin + 1; decH = vMax - vMin + 1;
                decoded.resize(static_cast<size_t>(region));
                auto decodeRow = [&](int v)
                {
                    uint32_t *out = decoded.data() + static_cast<size_t>(v - decV0) * static_cast<size_t>(decW);
                    for (int u = decU0; u <= uMax; ++u)
                        out[u - decU0] = texelAt(u, v);
                };
                if (decH >= 16 && m_bandCount <= 1 && GsRowPool::instance().workers() > 0)
                    GsRowPool::instance().run(decV0, vMax, decodeRow);
                else
                    for (int v = decV0; v <= vMax; ++v)
                        decodeRow(v);
            }
        }
        const bool useDecoded = !decoded.empty();
        auto sampleAt = [&](int u, int v) -> uint32_t
        {
            if (useDecoded)
                return decoded[static_cast<size_t>(v - decV0) * static_cast<size_t>(decW) + static_cast<size_t>(u - decU0)];
            return texelAt(u, v);
        };

        auto drawRow = [&](int y)
        {
            if (!ownsRow(y))
                return;
            const float rowTy = (static_cast<float>(y - unclippedY0) + 0.5f) / spriteH;
            const TexAxis &row = rowAxes[static_cast<size_t>(y - drawY0)];
            for (int x = drawX0; x <= drawX1; ++x)
            {
                const TexAxis &col = columns[static_cast<size_t>(x - drawX0)];
                uint32_t texel;
                if (!texLinear)
                {
                    texel = sampleAt(col.c0, row.c0);
                }
                else
                {
                    const uint32_t c00 = sampleAt(col.c0, row.c0);
                    const uint32_t c10 = sampleAt(col.c1, row.c0);
                    const uint32_t c01 = sampleAt(col.c0, row.c1);
                    const uint32_t c11 = sampleAt(col.c1, row.c1);
                    const float fx = col.frac, fy = row.frac;
                    texel = static_cast<uint32_t>(lerpChannel(c00 & 0xFFu, c10 & 0xFFu, c01 & 0xFFu, c11 & 0xFFu, fx, fy)) |
                            (static_cast<uint32_t>(lerpChannel((c00 >> 8) & 0xFFu, (c10 >> 8) & 0xFFu, (c01 >> 8) & 0xFFu, (c11 >> 8) & 0xFFu, fx, fy)) << 8) |
                            (static_cast<uint32_t>(lerpChannel((c00 >> 16) & 0xFFu, (c10 >> 16) & 0xFFu, (c01 >> 16) & 0xFFu, (c11 >> 16) & 0xFFu, fx, fy)) << 16) |
                            (static_cast<uint32_t>(lerpChannel((c00 >> 24) & 0xFFu, (c10 >> 24) & 0xFFu, (c01 >> 24) & 0xFFu, (c11 >> 24) & 0xFFu, fx, fy)) << 24);
                }
                {
                    static const bool verify = [] { const char *e = std::getenv("PS2X_GS_VERIFY"); return e && *e && *e != '0'; }();
                    if (verify)
                    {
                        const float tx = (static_cast<float>(x - unclippedX0) + 0.5f) / spriteW;
                        const float texUf = u0f + (u1f - u0f) * tx;
                        const float texVf = v0f + (v1f - v0f) * rowTy;
                        uint32_t ref;
                        if (state.prim.fst)
                        {
                            const uint16_t su = static_cast<uint16_t>(clampInt(static_cast<int>((texUf * 16.0f) + 0.5f), 0, 0xFFFF));
                            const uint16_t sv = static_cast<uint16_t>(clampInt(static_cast<int>((texVf * 16.0f) + 0.5f), 0, 0xFFFF));
                            ref = SampleTexture(state, 0.0f, 0.0f, 1.0f, su, sv);
                        }
                        else
                            ref = SampleTexture(state, texUf / static_cast<float>(texW), texVf / static_cast<float>(texH), 1.0f, 0u, 0u);
                        static std::atomic<uint64_t> checked{0}, bad{0};
                        const uint64_t c = ++checked;
                        if (ref != texel && ++bad <= 20u)
                            std::fprintf(stderr, "[gsverify] mismatch x=%d y=%d ref=%08x new=%08x\n", x, y, ref, texel);
                        if ((c & ((1u << 24) - 1u)) == 0u)
                            std::fprintf(stderr, "[gsverify] checked=%llu bad=%llu\n", (unsigned long long)c, (unsigned long long)bad.load());
                    }
                }
                const TextureCombineResult color = combineTexture(tex, r, g, b, a,
                                                                  static_cast<uint8_t>(texel & 0xFF),
                                                                  static_cast<uint8_t>((texel >> 8) & 0xFF),
                                                                  static_cast<uint8_t>((texel >> 16) & 0xFF),
                                                                  static_cast<uint8_t>((texel >> 24) & 0xFF));
                pipe.write(x, y, static_cast<int>(z1), color.r, color.g, color.b, color.a, v1.fog);
            }
        };
        auto drawRowReference = [&](int y)
        {
            float ty = (static_cast<float>(y - unclippedY0) + 0.5f) / spriteH;
            float texVf = v0f + (v1f - v0f) * ty;

            for (int x = drawX0; x <= drawX1; ++x)
            {
                float tx = (static_cast<float>(x - unclippedX0) + 0.5f) / spriteW;
                float texUf = u0f + (u1f - u0f) * tx;
                uint32_t texel = 0xFFFF00FFu;
                if (state.prim.fst)
                {
                    const int fixedU = static_cast<int>((texUf * 16.0f) + 0.5f);
                    const int fixedV = static_cast<int>((texVf * 16.0f) + 0.5f);
                    const uint16_t sampleU = static_cast<uint16_t>(clampInt(fixedU, 0, 0xFFFF));
                    const uint16_t sampleV = static_cast<uint16_t>(clampInt(fixedV, 0, 0xFFFF));
                    texel = SampleTexture(state, 0.0f, 0.0f, 1.0f, sampleU, sampleV);
                }
                else
                {
                    texel = SampleTexture(state, texUf / static_cast<float>(texW), texVf / static_cast<float>(texH), 1.0f, 0u, 0u);
                }

                uint8_t tr = static_cast<uint8_t>(texel & 0xFF);
                uint8_t tg = static_cast<uint8_t>((texel >> 8) & 0xFF);
                uint8_t tb = static_cast<uint8_t>((texel >> 16) & 0xFF);
                uint8_t ta = static_cast<uint8_t>((texel >> 24) & 0xFF);

                const TextureCombineResult color = combineTexture(tex, r, g, b, a, tr, tg, tb, ta);
                WritePixel(state, x, y, z1, color.r, color.g, color.b, color.a, v1.fog);
            }
        };
        (void)drawRowReference; // the original per-pixel version, kept for comparison
        if (spriteCanRunParallel(true))
            GsRowPool::instance().run(drawY0, drawY1, drawRow);
        else
            for (int y = drawY0; y <= drawY1; ++y)
                drawRow(y);
    }
    else
    {
        PixelPipe pipe;
        pipe.init(state, m_vram, m_readVramFuncs.data(), m_writeVramFuncs.data());
        auto fillRow = [&](int y)
        {
            if (!ownsRow(y))
                return;
            for (int x = drawX0; x <= drawX1; ++x)
                pipe.write(x, y, static_cast<int>(z1), r, g, b, a, v1.fog);
        };
        if (spriteCanRunParallel(false))
            GsRowPool::instance().run(drawY0, drawY1, fillRow);
        else
            for (int y = drawY0; y <= drawY1; ++y)
                fillRow(y);
    }
}

void GSCpuBackend::DrawTriangle(const GSPrimitiveBatch &batch)
{
    const GSDrawState &state = batch.state;
    const GSVertex &v0 = batch.vertices[0];
    const GSVertex &v1 = batch.vertices[1];
    const GSVertex &v2 = batch.vertices[2];
    const auto &ctx = state.context;

    int ofx = ctx.xyoffset.ofx >> 4;
    int ofy = ctx.xyoffset.ofy >> 4;

    float fx0 = v0.x - static_cast<float>(ofx);
    float fy0 = v0.y - static_cast<float>(ofy);
    float fx1 = v1.x - static_cast<float>(ofx);
    float fy1 = v1.y - static_cast<float>(ofy);
    float fx2 = v2.x - static_cast<float>(ofx);
    float fy2 = v2.y - static_cast<float>(ofy);

    int minX = static_cast<int>(std::floor(std::min({fx0, fx1, fx2})));
    int maxX = static_cast<int>(std::ceil(std::max({fx0, fx1, fx2})));
    int minY = static_cast<int>(std::floor(std::min({fy0, fy1, fy2})));
    int maxY = static_cast<int>(std::ceil(std::max({fy0, fy1, fy2})));

    minX = clampInt(minX, ctx.scissor.x0, ctx.scissor.x1);
    maxX = clampInt(maxX, ctx.scissor.x0, ctx.scissor.x1);
    minY = clampInt(minY, ctx.scissor.y0, ctx.scissor.y1);
    maxY = clampInt(maxY, ctx.scissor.y0, ctx.scissor.y1);

    // First row of [minY, maxY] this band instance draws; none = nothing to do here (most
    // small triangles touch only one or two of the 4-row bands).
    const int firstRow = firstOwnedRow(minY);
    if (firstRow > maxY)
        return;

    float denom = (fy1 - fy2) * (fx0 - fx2) + (fx2 - fx1) * (fy0 - fy2);
    if (std::fabs(denom) < 0.001f)
        return;

    const float winding = (denom < 0.0f) ? -1.0f : 1.0f;
    const float invAbsDenom = 1.0f / std::fabs(denom);
    constexpr float kEdgeEpsilon = 1.0e-4f;

    // Decoded texture (null = sample VRAM directly). Sample estimate: half the bounding box of
    // the rows this instance draws, 4 texels per pixel when bilinear.
    const uint32_t *decodedTexels = nullptr;
    if (state.prim.tme)
    {
        const uint64_t rows = static_cast<uint64_t>(std::max(0, maxY - minY + 1) + m_bandCount - 1) / static_cast<uint64_t>(m_bandCount);
        const uint64_t samples = rows * static_cast<uint64_t>(std::max(0, maxX - minX + 1)) / 2u * (state.linearFilter ? 4u : 1u);
        decodedTexels = DecodedTriangleTexture(state, minY, maxY, samples);
    }
    const int decodedStride = state.textureWidth;
    static const bool verifyDecoded = [] { const char *e = std::getenv("PS2X_GS_VERIFY"); return e && *e && *e != '0'; }();

    PixelPipe triPipe;
    triPipe.init(state, m_vram, m_readVramFuncs.data(), m_writeVramFuncs.data());
    triPipe.countStats = gsStatsOn();
    if (triPipe.countStats)
    {
        static std::map<std::string, uint64_t> triKeys;
        static auto lastTri = std::chrono::steady_clock::now();
        char key[200];
        std::snprintf(key, sizeof(key), "tme=%u psm=%x abe=%u alpha=%llx test=%llx fbp=%u fpsm=%x zbp=%u zpsm=%x zmsk=%u fbmsk=%x sc=%d,%d-%d,%d",
                      (unsigned)state.prim.tme, (unsigned)ctx.tex0.psm, (unsigned)state.prim.abe, (unsigned long long)ctx.alpha,
                      (unsigned long long)ctx.test, (unsigned)ctx.frame.fbp, (unsigned)ctx.frame.psm, (unsigned)ctx.zbuf.zbp,
                      (unsigned)ctx.zbuf.psm, (unsigned)ctx.zbuf.zmask, (unsigned)ctx.frame.fbmsk, ctx.scissor.x0, ctx.scissor.y0, ctx.scissor.x1, ctx.scissor.y1);
        ++triKeys[key];
        const auto now = std::chrono::steady_clock::now();
        if (now - lastTri >= std::chrono::seconds(1))
        {
            std::fprintf(stderr, "[tris] covered?=%llu scissor=%llu alpha=%llu date=%llu z=%llu written=%llu\n",
                         (unsigned long long)g_triPix[5].exchange(0), (unsigned long long)g_triPix[0].exchange(0), (unsigned long long)g_triPix[1].exchange(0),
                         (unsigned long long)g_triPix[2].exchange(0), (unsigned long long)g_triPix[3].exchange(0), (unsigned long long)g_triPix[4].exchange(0));
            std::vector<std::pair<uint64_t, std::string>> v;
            for (auto &kv : triKeys) v.push_back({kv.second, kv.first});
            std::sort(v.rbegin(), v.rend());
            for (size_t i = 0; i < v.size() && i < 5; ++i)
                std::fprintf(stderr, "[tris]   %llu x %s\n", (unsigned long long)v[i].first, v[i].second.c_str());
            triKeys.clear();
            lastTri = now;
        }
    }
    // Conservative x-span per row: the pixel test below stays the only decision, the span only
    // leaves out pixels that are far enough outside an edge that no float rounding of the test
    // can let them in (see triangleRowSpan). PS2X_GS_NOSPAN=1 = whole bounding box.
    static const bool noSpan = [] { const char *e = std::getenv("PS2X_GS_NOSPAN"); return e && *e && *e != '0'; }();
    const TriangleSpanSetup span = triangleSpanSetup(fx0, fy0, fx1, fy1, fx2, fy2, winding, invAbsDenom, kEdgeEpsilon, minX, maxX, minY, maxY);
#if defined(__AVX2__)
    TriangleRowsAvx2 rows8;
    const bool vectorRows = rows8.init(state, batch.vertices.data(), triPipe, decodedTexels, decodedStride, verifyDecoded, minY, maxY);
#endif
    for (int y = firstRow; y <= maxY; y = nextOwnedRow(y))
    {
        float py = static_cast<float>(y) + 0.5f;
        int rowX0 = minX, rowX1 = maxX;
        if (!noSpan && !triangleRowSpan(span, y, rowX0, rowX1))
            continue;
#if defined(__AVX2__)
        if (vectorRows)
        {
            rows8.drawRow(triPipe, y, py, rowX0, rowX1, fx0, fy0, fx1, fy1, fx2, fy2, winding, invAbsDenom, kEdgeEpsilon);
            continue;
        }
#endif
        for (int x = rowX0; x <= rowX1; ++x)
        {
            float px = static_cast<float>(x) + 0.5f;

            float w0 = (((fy1 - fy2) * (px - fx2) + (fx2 - fx1) * (py - fy2)) * winding) * invAbsDenom;
            float w1 = (((fy2 - fy0) * (px - fx2) + (fx0 - fx2) * (py - fy2)) * winding) * invAbsDenom;
            float w2 = 1.0f - w0 - w1;

            if (w0 < -kEdgeEpsilon || w1 < -kEdgeEpsilon || w2 < -kEdgeEpsilon)
                continue;

            double z = v0.z * w0 + v1.z * w1 + v2.z * w2;
            const int zValue = static_cast<int>(static_cast<u32>(z + 0.5));
            if (triPipe.failsDepth(x, y, zValue))
                continue;

            uint8_t r, g, b, a;
            if (state.prim.iip)
            {
                r = clampU8(static_cast<int>(v0.r * w0 + v1.r * w1 + v2.r * w2));
                g = clampU8(static_cast<int>(v0.g * w0 + v1.g * w1 + v2.g * w2));
                b = clampU8(static_cast<int>(v0.b * w0 + v1.b * w1 + v2.b * w2));
                a = clampU8(static_cast<int>(v0.a * w0 + v1.a * w1 + v2.a * w2));
            }
            else
            {
                r = v2.r;
                g = v2.g;
                b = v2.b;
                a = v2.a;
            }

            if (state.prim.tme)
            {
                float is, it, iq;
                uint16_t iu, iv;
                if (state.prim.fst)
                {
                    iu = static_cast<uint16_t>(v0.u * w0 + v1.u * w1 + v2.u * w2);
                    iv = static_cast<uint16_t>(v0.v * w0 + v1.v * w1 + v2.v * w2);
                    is = 0.0f;
                    it = 0.0f;
                    iq = 1.0f;
                }
                else
                {
                    // The GS DDA interpolates the homogeneous S, T and Q
                    // values. Texel coordinates are calculated from S/Q and
                    // T/Q only after interpolation.
                    is = v0.s * w0 + v1.s * w1 + v2.s * w2;
                    it = v0.t * w0 + v1.t * w1 + v2.t * w2;
                    iq = v0.q * w0 + v1.q * w1 + v2.q * w2;
                    iu = 0;
                    iv = 0;
                }

                uint32_t texel;
                if (decodedTexels)
                {
                    texel = sampleTextureWith(state, is, it, iq, iu, iv, [&](int su, int sv)
                                              { return decodedTexels[static_cast<size_t>(sv) * static_cast<size_t>(decodedStride) + static_cast<size_t>(su)]; });
                    if (verifyDecoded)
                    {
                        const uint32_t ref = SampleTexture(state, is, it, iq, iu, iv);
                        static std::atomic<uint64_t> checked{0}, bad{0};
                        const uint64_t c = ++checked;
                        if (ref != texel && ++bad <= 20u)
                            std::fprintf(stderr, "[gsverify] triangle mismatch x=%d y=%d ref=%08x decoded=%08x\n", x, y, ref, texel);
                        if ((c & ((1u << 20) - 1u)) == 0u)
                            std::fprintf(stderr, "[gsverify] triangles checked=%llu bad=%llu\n", (unsigned long long)c, (unsigned long long)bad.load());
                    }
                }
                else
                    texel = SampleTexture(state, is, it, iq, iu, iv);

                uint8_t tr = static_cast<uint8_t>(texel & 0xFF);
                uint8_t tg = static_cast<uint8_t>((texel >> 8) & 0xFF);
                uint8_t tb = static_cast<uint8_t>((texel >> 16) & 0xFF);
                uint8_t ta = static_cast<uint8_t>((texel >> 24) & 0xFF);

                const uint32_t color = combineTexturePacked(ctx.tex0, r, g, b, a, tr, tg, tb, ta);
                r = static_cast<uint8_t>(color);
                g = static_cast<uint8_t>(color >> 8);
                b = static_cast<uint8_t>(color >> 16);
                a = static_cast<uint8_t>(color >> 24);
            }

            const uint8_t fog = clampU8(static_cast<int>(v0.fog * w0 + v1.fog * w1 + v2.fog * w2));
            triPipe.write(x, y, zValue, r, g, b, a, fog);
        }
    }
}

void GSCpuBackend::DrawLine(const GSPrimitiveBatch &batch)
{
    const GSDrawState &state = batch.state;
    const GSVertex &v0 = batch.vertices[0];
    const GSVertex &v1 = batch.vertices[1];
    const auto &ctx = state.context;

    int ofx = ctx.xyoffset.ofx >> 4;
    int ofy = ctx.xyoffset.ofy >> 4;

    int x0 = static_cast<int>(v0.x) - ofx;
    int y0 = static_cast<int>(v0.y) - ofy;
    int x1 = static_cast<int>(v1.x) - ofx;
    int y1 = static_cast<int>(v1.y) - ofy;

    int dx = std::abs(x1 - x0);
    int dy = -std::abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;

    int totalSteps = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
    if (totalSteps == 0)
        totalSteps = 1;
    int step = 0;

    for (;;)
    {
        float t = static_cast<float>(step) / static_cast<float>(totalSteps);
        uint8_t r, g, b, a;
        if (state.prim.iip)
        {
            r = clampU8(static_cast<int>(v0.r + (v1.r - v0.r) * t));
            g = clampU8(static_cast<int>(v0.g + (v1.g - v0.g) * t));
            b = clampU8(static_cast<int>(v0.b + (v1.b - v0.b) * t));
            a = clampU8(static_cast<int>(v0.a + (v1.a - v0.a) * t));
        }
        else
        {
            r = v1.r;
            g = v1.g;
            b = v1.b;
            a = v1.a;
        }

        double z = (v0.z + (v1.z - v0.z) * t);
        const uint8_t fog = clampU8(static_cast<int>(v0.fog + (v1.fog - v0.fog) * t));
        WritePixel(state, x0, y0, static_cast<u32>(z), r, g, b, a, fog);

        if (x0 == x1 && y0 == y1)
            break;

        int e2 = 2 * err;
        if (e2 >= dy)
        {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx)
        {
            err += dx;
            y0 += sy;
        }
        ++step;
    }
}

void GSCpuBackend::BeginTransfer(const GSTransferCommand &command)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_transfer = command;
    m_transferState.x = command.trxpos.dsax;
    m_transferState.y = command.trxpos.dsay;
    m_transferState.totalPixels = static_cast<uint32_t>(command.trxreg.rrw) * static_cast<uint32_t>(command.trxreg.rrh);
    m_transferState.copiedPixels = 0u;
    m_transferState.direction = command.direction;
    m_transferState.localToHostPendingBytes = 0u;
    NoteTransferStart(command);

    if (command.direction == 2u)
        PerformLocalToLocalTransfer();
    else if (command.direction == 1u)
        PerformLocalToHostTransfer();
}

void GSCpuBackend::UploadImage(const uint8_t *data, uint32_t sizeBytes)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!data || sizeBytes == 0u || !m_vram || m_transferState.direction != 0u)
        return;
    if (m_transfer.trxreg.rrw == 0u || m_transfer.trxreg.rrh == 0u || m_transferState.totalPixels == 0u)
        return;
    NoteTransferData();

    const uint32_t dbp = m_transfer.bitbltbuf.dbp;
    const uint32_t dbw = std::max<uint32_t>(m_transfer.bitbltbuf.dbw, 1u);
    const uint8_t dpsm = m_transfer.bitbltbuf.dpsm;
    const uint32_t rrw = m_transfer.trxreg.rrw;
    const uint32_t dsax = m_transfer.trxpos.dsax;
    uint32_t offset = 0u;

    auto advancePixel = [&](uint32_t count)
    {
        const uint32_t totalPixels = m_transferState.totalPixels;
        m_transferState.copiedPixels =
            std::min<uint32_t>(totalPixels, m_transferState.copiedPixels + count);

        if (m_transferState.copiedPixels >= totalPixels)
        {
            m_transferState.direction = 3u;
            m_transferState.totalPixels = 0u;
            return;
        }

        m_transferState.x = dsax + (m_transferState.copiedPixels % rrw);
        m_transferState.y = m_transfer.trxpos.dsay + (m_transferState.copiedPixels / rrw);
    };

    while (offset < sizeBytes && m_transferState.direction == 0u)
    {
        switch (dpsm)
        {
        case GS_PSM_CT32:
        case GS_PSM_Z32:
        {
            if (sizeBytes - offset < 4u)
                return;
            uint32_t value = 0u;
            std::memcpy(&value, data + offset, sizeof(value));
            WriteVramUnlocked(dpsm, dbp, dbw, m_transferState.x, m_transferState.y, value);
            offset += 4u;
            advancePixel(1u);
            break;
        }
        case GS_PSM_CT24:
        case GS_PSM_Z24:
        {
            if (sizeBytes - offset < 3u)
                return;
            const uint32_t value = static_cast<uint32_t>(data[offset]) |
                                   (static_cast<uint32_t>(data[offset + 1u]) << 8u) |
                                   (static_cast<uint32_t>(data[offset + 2u]) << 16u);
            WriteVramUnlocked(dpsm, dbp, dbw, m_transferState.x, m_transferState.y, value);
            offset += 3u;
            advancePixel(1u);
            break;
        }
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        case GS_PSM_Z16:
        case GS_PSM_Z16S:
        {
            if (sizeBytes - offset < 2u)
                return;
            uint16_t value = 0u;
            std::memcpy(&value, data + offset, sizeof(value));
            WriteVramUnlocked(dpsm, dbp, dbw, m_transferState.x, m_transferState.y, value);
            offset += 2u;
            advancePixel(1u);
            break;
        }
        case GS_PSM_T8:
        case GS_PSM_T8H:
            WriteVramUnlocked(dpsm, dbp, dbw, m_transferState.x, m_transferState.y, data[offset++]);
            advancePixel(1u);
            break;
        case GS_PSM_T4:
        case GS_PSM_T4HL:
        case GS_PSM_T4HH:
        {
            const uint8_t packed = data[offset++];
            const uint32_t firstPixel = m_transferState.copiedPixels;
            WriteVramUnlocked(dpsm, dbp, dbw,
                              dsax + (firstPixel % rrw),
                              m_transfer.trxpos.dsay + (firstPixel / rrw),
                              packed & 0x0Fu);
            if (firstPixel + 1u < m_transferState.totalPixels)
            {
                const uint32_t secondPixel = firstPixel + 1u;
                WriteVramUnlocked(dpsm, dbp, dbw,
                                  dsax + (secondPixel % rrw),
                                  m_transfer.trxpos.dsay + (secondPixel / rrw),
                                  (packed >> 4u) & 0x0Fu);
            }
            advancePixel(std::min<uint32_t>(2u, m_transferState.totalPixels - firstPixel));
            break;
        }
        default:
            return;
        }
    }
}

void GSCpuBackend::PerformLocalToLocalTransfer()
{
    if (!m_vram)
        return;

    const uint32_t rrw = m_transfer.trxreg.rrw;
    const uint32_t rrh = m_transfer.trxreg.rrh;
    const uint32_t total = rrw * rrh;
    if (total == 0u)
    {
        m_transferState.direction = 3u;
        return;
    }

    for (uint32_t pixel = 0; pixel < total; ++pixel)
    {
        uint32_t x = pixel % rrw;
        uint32_t y = pixel / rrw;
        if ((m_transfer.trxpos.dir & 0x2u) != 0u)
            x = rrw - x - 1u;
        if ((m_transfer.trxpos.dir & 0x1u) != 0u)
            y = rrh - y - 1u;

        const uint32_t value = ReadVramUnlocked(m_transfer.bitbltbuf.spsm,
                                                m_transfer.bitbltbuf.sbp,
                                                std::max<uint32_t>(m_transfer.bitbltbuf.sbw, 1u),
                                                x + m_transfer.trxpos.ssax,
                                                y + m_transfer.trxpos.ssay);
        WriteVramUnlocked(m_transfer.bitbltbuf.dpsm,
                          m_transfer.bitbltbuf.dbp,
                          std::max<uint32_t>(m_transfer.bitbltbuf.dbw, 1u),
                          x + m_transfer.trxpos.dsax,
                          y + m_transfer.trxpos.dsay,
                          value);
    }

    m_transferState.copiedPixels = total;
    m_transferState.direction = 3u;
}

void GSCpuBackend::PerformLocalToHostTransfer()
{
    m_localToHostBuffer.clear();
    m_localToHostReadPos = 0u;
    if (!m_vram)
        return;

    const uint32_t rrw = m_transfer.trxreg.rrw;
    const uint32_t rrh = m_transfer.trxreg.rrh;
    const uint32_t sbw = std::max<uint32_t>(m_transfer.bitbltbuf.sbw, 1u);
    const uint8_t spsm = m_transfer.bitbltbuf.spsm;
    const uint32_t bpp = static_cast<uint32_t>(GSMem::BitsPerPixel(static_cast<GSMem::PixelStorageMode>(spsm)));
    const uint32_t total = rrw * rrh;
    m_localToHostBuffer.reserve((static_cast<size_t>(total) * bpp + 7u) / 8u);

    for (uint32_t pixel = 0u; pixel < total; ++pixel)
    {
        const uint32_t x = pixel % rrw;
        const uint32_t y = pixel / rrw;
        const uint32_t value = ReadVramUnlocked(spsm,
                                                m_transfer.bitbltbuf.sbp,
                                                sbw,
                                                x + m_transfer.trxpos.ssax,
                                                y + m_transfer.trxpos.ssay);
        switch (bpp)
        {
        case 32:
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 8u));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 16u));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 24u));
            break;
        case 24:
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 8u));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 16u));
            break;
        case 16:
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 8u));
            break;
        case 8:
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value));
            break;
        case 4:
        {
            if ((pixel & 1u) != 0u)
                break;
            uint32_t next = 0u;
            if (pixel + 1u < total)
            {
                const uint32_t nextPixel = pixel + 1u;
                const uint32_t nextX = nextPixel % rrw;
                const uint32_t nextY = nextPixel / rrw;
                next = ReadVramUnlocked(spsm, m_transfer.bitbltbuf.sbp, sbw,
                                        nextX + m_transfer.trxpos.ssax,
                                        nextY + m_transfer.trxpos.ssay);
            }
            m_localToHostBuffer.push_back(static_cast<uint8_t>((value & 0x0Fu) | ((next & 0x0Fu) << 4u)));
            break;
        }
        default:
            break;
        }
    }

    m_transferState.copiedPixels = total;
    m_transferState.localToHostPendingBytes = m_localToHostBuffer.size();
}

uint32_t GSCpuBackend::ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!dst || maxBytes == 0u || m_localToHostReadPos >= m_localToHostBuffer.size())
        return 0u;
    const size_t count = std::min<size_t>(maxBytes, m_localToHostBuffer.size() - m_localToHostReadPos);
    std::memcpy(dst, m_localToHostBuffer.data() + m_localToHostReadPos, count);
    m_localToHostReadPos += count;
    m_transferState.localToHostPendingBytes = m_localToHostBuffer.size() - m_localToHostReadPos;
    return static_cast<uint32_t>(count);
}

bool GSCpuBackend::ClearFramebuffer(const GSContext &context, uint32_t rgba)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_vram || context.frame.fbw == 0u)
        return false;
    InvalidateDecodedTexturesUnlocked();

    const uint32_t x0 = context.scissor.x0;
    const uint32_t x1 = std::max<uint32_t>(x0, context.scissor.x1);
    const uint32_t y0 = context.scissor.y0;
    const uint32_t y1 = std::max<uint32_t>(y0, context.scissor.y1);
    uint8_t r = static_cast<uint8_t>(rgba);
    uint8_t g = static_cast<uint8_t>(rgba >> 8u);
    uint8_t b = static_cast<uint8_t>(rgba >> 16u);
    uint8_t a = static_cast<uint8_t>(rgba >> 24u);
    if ((context.fba & 1ull) != 0ull && context.frame.psm != GS_PSM_CT24)
        a |= 0x80u;

    const uint32_t fbp = GSInternal::framePageBaseToBlock(context.frame.fbp);
    const uint32_t fbw = std::max<uint32_t>(context.frame.fbw, 1u);
    if (context.frame.psm == GS_PSM_CT32 || context.frame.psm == GS_PSM_CT24)
    {
        const uint32_t source = static_cast<uint32_t>(r) |
                                (static_cast<uint32_t>(g) << 8u) |
                                (static_cast<uint32_t>(b) << 16u) |
                                (static_cast<uint32_t>(a) << 24u);
        for (uint32_t y = y0; y <= y1; ++y)
            for (uint32_t x = x0; x <= x1; ++x)
            {
                uint32_t pixel = source;
                if (context.frame.fbmsk != 0u)
                {
                    const uint32_t old = ReadVramUnlocked(context.frame.psm, fbp, fbw, x, y);
                    pixel = (pixel & ~context.frame.fbmsk) | (old & context.frame.fbmsk);
                }
                WriteVramUnlocked(context.frame.psm, fbp, fbw, x, y, pixel);
            }
        return true;
    }

    if (context.frame.psm == GS_PSM_CT16 || context.frame.psm == GS_PSM_CT16S)
    {
        const uint16_t source = encodeFramePixelPSMCT16(r, g, b, a);
        const uint16_t mask = static_cast<uint16_t>(context.frame.fbmsk);
        for (uint32_t y = y0; y <= y1; ++y)
            for (uint32_t x = x0; x <= x1; ++x)
            {
                uint16_t pixel = source;
                if (mask != 0u)
                {
                    const uint16_t old = static_cast<uint16_t>(ReadVramUnlocked(context.frame.psm, fbp, fbw, x, y));
                    pixel = static_cast<uint16_t>((pixel & ~mask) | (old & mask));
                }
                WriteVramUnlocked(context.frame.psm, fbp, fbw, x, y, pixel);
            }
        return true;
    }
    return false;
}

bool GSCpuBackend::CopyFrameToHostRgba(const GSFrameReg &frame,
                                       uint32_t width,
                                       uint32_t height,
                                       std::vector<uint8_t> &outPixels,
                                       bool preserveAlpha,
                                       bool useLocalMemoryLayout,
                                       bool frameBaseIsPages,
                                       uint32_t sourceOriginX,
                                       uint32_t sourceOriginY) const
{
    if (!m_vram || m_vramSize == 0u)
        return false;

    outPixels.assign(kHostFrameWidth * kHostFrameHeight * 4u, 0u);
    const uint32_t baseBytes = frameBaseIsPages ? frame.fbp * 8192u : frame.fbp * 256u;
    const uint32_t basePtr = frameBaseIsPages ? GSInternal::framePageBaseToBlock(frame.fbp) : frame.fbp;
    const uint32_t fbw = frame.fbw ? frame.fbw : kHostFrameWidth / 64u;
    const uint32_t bytesPerPixel = (frame.psm == GS_PSM_CT16 || frame.psm == GS_PSM_CT16S) ? 2u : 4u;
    const uint32_t stride = fbw * 64u * bytesPerPixel;

    for (uint32_t y = 0; y < height; ++y)
    {
        uint8_t *dst = outPixels.data() + y * kHostFrameWidth * 4u;
        for (uint32_t x = 0; x < width; ++x)
        {
            const uint32_t sx = sourceOriginX + x;
            const uint32_t sy = sourceOriginY + y;
            if (frame.psm == GS_PSM_CT32 || frame.psm == GS_PSM_CT24)
            {
                uint32_t color = 0u;
                if (useLocalMemoryLayout)
                    color = ReadVramUnlocked(frame.psm, basePtr, fbw, sx, sy);
                else
                {
                    const uint32_t pixelBytes = frame.psm == GS_PSM_CT24 ? 3u : 4u;
                    const uint64_t offset = static_cast<uint64_t>(baseBytes) + static_cast<uint64_t>(sy) * stride + static_cast<uint64_t>(sx) * pixelBytes;
                    if (offset + pixelBytes > m_vramSize)
                        return false;
                    color = m_vram[offset] | (static_cast<uint32_t>(m_vram[offset + 1u]) << 8u) |
                            (static_cast<uint32_t>(m_vram[offset + 2u]) << 16u);
                    if (pixelBytes == 4u)
                        color |= static_cast<uint32_t>(m_vram[offset + 3u]) << 24u;
                }
                dst[x * 4u] = static_cast<uint8_t>(color);
                dst[x * 4u + 1u] = static_cast<uint8_t>(color >> 8u);
                dst[x * 4u + 2u] = static_cast<uint8_t>(color >> 16u);
                dst[x * 4u + 3u] = preserveAlpha && frame.psm != GS_PSM_CT24 ? static_cast<uint8_t>(color >> 24u) : 255u;
            }
            else if (frame.psm == GS_PSM_CT16 || frame.psm == GS_PSM_CT16S)
            {
                uint16_t color = 0u;
                if (useLocalMemoryLayout)
                    color = static_cast<uint16_t>(ReadVramUnlocked(frame.psm, basePtr, fbw, sx, sy));
                else
                {
                    const uint64_t offset = static_cast<uint64_t>(baseBytes) + static_cast<uint64_t>(sy) * stride + static_cast<uint64_t>(sx) * 2u;
                    if (offset + 2u > m_vramSize)
                        return false;
                    std::memcpy(&color, m_vram + offset, sizeof(color));
                }
                const uint32_t r = color & 31u;
                const uint32_t g = (color >> 5u) & 31u;
                const uint32_t b = (color >> 10u) & 31u;
                dst[x * 4u] = static_cast<uint8_t>((r << 3u) | (r >> 2u));
                dst[x * 4u + 1u] = static_cast<uint8_t>((g << 3u) | (g >> 2u));
                dst[x * 4u + 2u] = static_cast<uint8_t>((b << 3u) | (b >> 2u));
                dst[x * 4u + 3u] = preserveAlpha ? ((color & 0x8000u) ? 0x80u : 0u) : 255u;
            }
            else
            {
                outPixels.clear();
                return false;
            }
        }
    }
    return true;
}

PresentationFrame GSCpuBackend::Present(const GSPresentationRequest &request)
{
    // Snapshot local memory under the backend lock, then perform the expensive
    // display conversion without holding the producer-side raster lock.
    thread_local std::vector<uint8_t> snapshot;
    SnapshotVram(snapshot);
    if (snapshot.empty())
        return {};

    thread_local GSCpuBackend snapshotBackend;
    snapshotBackend.Initialize(snapshot.data(), static_cast<uint32_t>(snapshot.size()));
    return snapshotBackend.PresentFromLocalMemory(request);
}

PresentationFrame GSCpuBackend::PresentFromLocalMemory(const GSPresentationRequest &request)
{
    PresentationFrame result{};
    const GSPmodeState pmode = decodePmode(request.pmode);
    const GSSmode2State smode2 = decodeSMode2(request.smode2);
    const bool fieldMode = smode2.interlaced && !smode2.frameMode;
    const bool oddField = (request.vsyncTick & 1ull) != 0ull;
    const GSFrameReg displayFrame1 = decodeDisplayFrame(request.dispfb1);
    const GSFrameReg displayFrame2 = decodeDisplayFrame(request.dispfb2);
    const GSDisplayReadOrigin origin1 = decodeDisplayReadOrigin(request.dispfb1);
    const GSDisplayReadOrigin origin2 = decodeDisplayReadOrigin(request.dispfb2);
    uint32_t width1 = 0u, height1 = 0u, width2 = 0u, height2 = 0u;
    decodeDisplaySize(request.display1, width1, height1);
    decodeDisplaySize(request.display2, width2, height2);
    const bool valid1 = pmode.enableCrt1 && hasDisplaySetup(request.display1, displayFrame1);
    const bool valid2 = pmode.enableCrt2 && hasDisplaySetup(request.display2, displayFrame2);
    if (!valid1 && !valid2)
        return result;

    auto copySource = [&](const GSFrameReg &displayFrame,
                          const GSDisplayReadOrigin &origin,
                          uint32_t width,
                          uint32_t height,
                          bool allowPreferred,
                          bool preserveAlpha,
                          GSFrameReg &selected,
                          std::vector<uint8_t> &pixels,
                          bool &usedPreferred) -> bool
    {
        selected = displayFrame;
        pixels.clear();
        usedPreferred = false;
        if (allowPreferred && request.hasPreferredSource && request.preferredDestFbp == displayFrame.fbp &&
            (request.preferredSource.fbw != 0u || request.preferredSource.fbp != displayFrame.fbp) &&
            CopyFrameToHostRgba(request.preferredSource, width, height, pixels, preserveAlpha, true, false, 0u, 0u))
        {
            selected = request.preferredSource;
            usedPreferred = true;
        }
        if (pixels.empty() && !CopyFrameToHostRgba(displayFrame, width, height, pixels, preserveAlpha, true, true, origin.x, origin.y))
            return false;

        if (!usedPreferred && displayFrame.fbp == 0u && countNonBlackPixels(pixels, width, height) == 0u)
        {
            for (const GSFrameReg &candidate : request.contextFrames)
            {
                if (candidate.fbp == selected.fbp && candidate.fbw == selected.fbw && candidate.psm == selected.psm)
                    continue;
                std::vector<uint8_t> candidatePixels;
                if (!CopyFrameToHostRgba(candidate, width, height, candidatePixels, preserveAlpha, true, true, 0u, 0u))
                    continue;
                if (countNonBlackPixels(candidatePixels, width, height) == 0u)
                    continue;
                selected = candidate;
                pixels.swap(candidatePixels);
                break;
            }
        }
        return true;
    };

    if (valid1 && valid2)
    {
        GSFrameReg selected1{}, selected2{};
        std::vector<uint8_t> crt1, crt2;
        bool preferred1 = false, preferred2 = false;
        if (copySource(displayFrame1, origin1, width1, height1, false, true, selected1, crt1, preferred1) &&
            copySource(displayFrame2, origin2, width2, height2, false, true, selected2, crt2, preferred2))
        {
            result.width = std::max(width1, width2);
            result.height = std::max(height1, height2);
            result.pixels.assign(kHostFrameWidth * kHostFrameHeight * 4u, 0u);
            const uint8_t bgR = static_cast<uint8_t>(request.bgcolor);
            const uint8_t bgG = static_cast<uint8_t>(request.bgcolor >> 8u);
            const uint8_t bgB = static_cast<uint8_t>(request.bgcolor >> 16u);
            for (uint32_t y = 0; y < result.height; ++y)
                for (uint32_t x = 0; x < result.width; ++x)
                {
                    uint8_t *dst = result.pixels.data() + (y * kHostFrameWidth + x) * 4u;
                    dst[0] = bgR;
                    dst[1] = bgG;
                    dst[2] = bgB;
                    dst[3] = pmode.alp;
                }
            if (!pmode.slbg)
                for (uint32_t y = 0; y < height2; ++y)
                    std::memcpy(result.pixels.data() + y * kHostFrameWidth * 4u, crt2.data() + y * kHostFrameWidth * 4u, width2 * 4u);
            for (uint32_t y = 0; y < height1; ++y)
                for (uint32_t x = 0; x < width1; ++x)
                {
                    const uint8_t *src = crt1.data() + (y * kHostFrameWidth + x) * 4u;
                    uint8_t *dst = result.pixels.data() + (y * kHostFrameWidth + x) * 4u;
                    const uint32_t factor = pmode.mmod ? pmode.alp : std::min<uint32_t>(255u, static_cast<uint32_t>(src[3]) * 2u);
                    dst[0] = blendPresentationChannel(src[0], dst[0], factor);
                    dst[1] = blendPresentationChannel(src[1], dst[1], factor);
                    dst[2] = blendPresentationChannel(src[2], dst[2], factor);
                    dst[3] = pmode.amod ? dst[3] : src[3];
                }
            normalizePresentationAlpha(result.pixels, result.width, result.height);
            if (fieldMode)
                applyFieldPresentation(result.pixels, result.width, result.height, oddField);
            result.displayFbp = displayFrame1.fbp;
            result.sourceFbp = selected1.fbp;
            return result;
        }
    }

    const GSFrameReg &displayFrame = valid1 ? displayFrame1 : displayFrame2;
    const GSDisplayReadOrigin &origin = valid1 ? origin1 : origin2;
    result.width = valid1 ? width1 : width2;
    result.height = valid1 ? height1 : height2;
    GSFrameReg selected = displayFrame;
    if (!copySource(displayFrame, origin, result.width, result.height, true, false, selected, result.pixels, result.usedPreferred))
        return {};
    if (fieldMode)
        applyFieldPresentation(result.pixels, result.width, result.height, oddField);
    normalizePresentationAlpha(result.pixels, result.width, result.height);
    result.displayFbp = displayFrame.fbp;
    result.sourceFbp = selected.fbp;
    return result;
}
