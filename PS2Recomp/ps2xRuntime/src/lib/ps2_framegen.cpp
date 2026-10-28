// Display-only frame generation, see ps2_framegen.h.
//
// PS2X_FRAMEGEN=1           on (off by default; stays off with only 1 logical processor, =2 forces it on)
// PS2X_FRAMEGEN_LAG=<n>     how far the playhead stays behind the newest picture, in 1/100 source frames (default
//                           100 = one frame, ~33 ms at 30 FPS; 50..300). 50 halves the latency but leaves no room
//                           for uneven arrivals and has to wait for the motion search on the present a frame arrives.
// PS2X_FRAMEGEN_CUT=<n>     mean luma SAD per pixel above which a frame pair counts as a scene cut (default 24)
// PS2X_FRAMEGEN_DEBUG=1     tint pixels where the motion-compensated blend gave up (fallback) magenta
// PS2X_FRAMEGEN_LOG=1       per-second stats on stderr (also with PS2X_FPS_LOG=1); =2 also one line per arrival
//                           and per present
//
// Pipeline: the window thread latches a picture per emulated VSync tick (UploadFrame); a changed picture is a
// new source frame. Its pixels go to a GPU texture ring, and a worker thread runs hierarchical block motion
// estimation (16x16 blocks, quarter-res full search +-32 px, half/full-res refinement, SSE2/AVX2 SAD) in both
// directions against the previous frame. A jitter buffer plays the frames back: a playhead advances by the
// measured source rate per present, about one frame behind the newest arrival, so uneven arrivals (the latched
// picture trails the guest flip by 0-1 presents) come out evenly paced. A present whose playhead falls between
// two frames draws a GLSL pass that picks, per pixel, the best of ~10
// candidate vectors (zero, the four nearest backward and forward block vectors, their smooth blend) by local
// match error, then blends the two motion-compensated samples. Where no candidate matches (occlusion, UI over
// motion), it falls back to the nearer real frame; a pair whose mean SAD is high is a scene cut and is not
// interpolated. Nothing here is visible to the emulator.
//
// Internal resolution (PS2X_GS_SCALE > 1): the motion search stays on the GS-resolution pictures, and each source
// frame also keeps a copy of the GPU renderer's internal-resolution picture (submitHires). Real frames are shown
// from those copies and the in-between pass runs at that resolution with the vectors scaled up, so frame
// generation doesn't drop the picture back to 640 wide.

#include "runtime/ps2_framegen.h"
#include "runtime/ps2_host_cpu.h"
#include "ThreadNaming.h"
#include "rlgl.h"

#include <immintrin.h>
#include <algorithm>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ps2x_framegen
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        constexpr int kBlock = 16;
        constexpr int kRing = 4;
        constexpr int kMaxGap = 4;       // presents between two source frames beyond which we don't interpolate
        constexpr int kFieldMaxW = 64;   // field texture size (blocks)
        constexpr int kFieldMaxH = 64;

        int envInt(const char *name, int def)
        {
            const char *e = std::getenv(name);
            return (e && *e) ? std::atoi(e) : def;
        }

        // ------------------------------------------------------------------ motion estimation (worker thread)

        struct Plane
        {
            int w = 0, h = 0;
            std::vector<uint8_t> px;
            const uint8_t *at(int x, int y) const { return px.data() + static_cast<size_t>(y) * w + x; }
        };

        struct Pyramid
        {
            Plane l0, l1, l2; // full, half, quarter resolution luma
        };

        struct Field
        {
            uint64_t serial = 0;          // newer frame of the pair
            int gw = 0, gh = 0;
            std::vector<uint8_t> bwd, fwd; // RGBA8 per block: r, g = content motion older->newer + 128
            float meanSad = 0.0f;
            float badFrac = 0.0f;
            bool cut = false;
            double ms = 0.0;
        };

        struct Job
        {
            uint64_t prevSerial = 0;
            uint64_t serial = 0;
            int w = 0, h = 0, stride = 0;
            std::shared_ptr<std::vector<uint8_t>> rgba;
        };

        void halve(const Plane &src, Plane &dst)
        {
            dst.w = src.w / 2;
            dst.h = src.h / 2;
            dst.px.resize(static_cast<size_t>(dst.w) * dst.h);
            for (int y = 0; y < dst.h; ++y)
            {
                const uint8_t *a = src.at(0, 2 * y);
                const uint8_t *b = src.at(0, 2 * y + 1);
                uint8_t *d = dst.px.data() + static_cast<size_t>(y) * dst.w;
                for (int x = 0; x < dst.w; ++x)
                    d[x] = static_cast<uint8_t>((a[2 * x] + a[2 * x + 1] + b[2 * x] + b[2 * x + 1] + 2) >> 2);
            }
        }

        void buildPyramid(const uint8_t *rgba, int stride, int w, int h, Pyramid &p)
        {
            p.l0.w = w;
            p.l0.h = h;
            p.l0.px.resize(static_cast<size_t>(w) * h);
            for (int y = 0; y < h; ++y)
            {
                const uint8_t *s = rgba + static_cast<size_t>(y) * stride * 4;
                uint8_t *d = p.l0.px.data() + static_cast<size_t>(y) * w;
                for (int x = 0; x < w; ++x)
                    d[x] = static_cast<uint8_t>((s[4 * x] * 54 + s[4 * x + 1] * 183 + s[4 * x + 2] * 19 + 128) >> 8);
            }
            halve(p.l0, p.l1);
            halve(p.l1, p.l2);
        }

        inline uint32_t sad8x8(const uint8_t *a, int sa, const uint8_t *b, int sb)
        {
            __m128i acc = _mm_setzero_si128();
            for (int y = 0; y < 8; y += 2)
            {
                const __m128i x = _mm_unpacklo_epi64(_mm_loadl_epi64(reinterpret_cast<const __m128i *>(a + y * sa)),
                                                     _mm_loadl_epi64(reinterpret_cast<const __m128i *>(a + (y + 1) * sa)));
                const __m128i z = _mm_unpacklo_epi64(_mm_loadl_epi64(reinterpret_cast<const __m128i *>(b + y * sb)),
                                                     _mm_loadl_epi64(reinterpret_cast<const __m128i *>(b + (y + 1) * sb)));
                acc = _mm_add_epi64(acc, _mm_sad_epu8(x, z));
            }
            return static_cast<uint32_t>(_mm_cvtsi128_si32(acc) + _mm_cvtsi128_si32(_mm_srli_si128(acc, 8)));
        }

        inline uint32_t sad16x16(const uint8_t *a, int sa, const uint8_t *b, int sb)
        {
#if defined(__AVX2__)
            __m256i acc = _mm256_setzero_si256();
            for (int y = 0; y < 16; y += 2)
            {
                const __m256i x = _mm256_inserti128_si256(
                    _mm256_castsi128_si256(_mm_loadu_si128(reinterpret_cast<const __m128i *>(a + y * sa))),
                    _mm_loadu_si128(reinterpret_cast<const __m128i *>(a + (y + 1) * sa)), 1);
                const __m256i z = _mm256_inserti128_si256(
                    _mm256_castsi128_si256(_mm_loadu_si128(reinterpret_cast<const __m128i *>(b + y * sb))),
                    _mm_loadu_si128(reinterpret_cast<const __m128i *>(b + (y + 1) * sb)), 1);
                acc = _mm256_add_epi64(acc, _mm256_sad_epu8(x, z));
            }
            const __m128i s = _mm_add_epi64(_mm256_castsi256_si128(acc), _mm256_extracti128_si256(acc, 1));
#else
            __m128i s = _mm_setzero_si128();
            for (int y = 0; y < 16; ++y)
                s = _mm_add_epi64(s, _mm_sad_epu8(_mm_loadu_si128(reinterpret_cast<const __m128i *>(a + y * sa)),
                                                  _mm_loadu_si128(reinterpret_cast<const __m128i *>(b + y * sb))));
#endif
            return static_cast<uint32_t>(_mm_cvtsi128_si32(s) + _mm_cvtsi128_si32(_mm_srli_si128(s, 8)));
        }

        struct Vec
        {
            int x = 0, y = 0;
        };

        // For every 16x16 block of T: d such that T(block) ~ R(block + d). Returns the full-res SAD per block.
        void searchField(const Pyramid &T, const Pyramid &R, int gw, int gh, std::vector<Vec> &out,
                         std::vector<uint32_t> &outSad)
        {
            constexpr int kRange2 = 8;   // quarter res: +-32 px
            constexpr uint32_t kLambda2 = 16, kLambda1 = 8, kLambda0 = 16;
            const int w = T.l0.w, h = T.l0.h;
            const int n = gw * gh;
            std::vector<Vec> v2(n), v1(n);
            out.assign(n, Vec{});
            outSad.assign(n, 0u);
            auto originX = [&](int bx) { return std::min(bx * kBlock, w - kBlock); };
            auto originY = [&](int by) { return std::min(by * kBlock, h - kBlock); };

            // Quarter resolution: 8x8 window (32x32 px) centred on the block, full search.
            const Plane &t2 = T.l2, &r2 = R.l2;
            for (int by = 0; by < gh; ++by)
                for (int bx = 0; bx < gw; ++bx)
                {
                    const int qx = std::clamp((originX(bx) + 8) / 4 - 4, 0, t2.w - 8);
                    const int qy = std::clamp((originY(by) + 8) / 4 - 4, 0, t2.h - 8);
                    const uint8_t *tb = t2.at(qx, qy);
                    uint32_t best = UINT_MAX;
                    Vec bv;
                    for (int dy = -kRange2; dy <= kRange2; ++dy)
                    {
                        const int ry = qy + dy;
                        if (ry < 0 || ry > r2.h - 8)
                            continue;
                        for (int dx = -kRange2; dx <= kRange2; ++dx)
                        {
                            const int rx = qx + dx;
                            if (rx < 0 || rx > r2.w - 8)
                                continue;
                            const uint32_t c = sad8x8(tb, t2.w, r2.at(rx, ry), r2.w) +
                                               kLambda2 * static_cast<uint32_t>(std::abs(dx) + std::abs(dy));
                            if (c < best)
                            {
                                best = c;
                                bv = {dx, dy};
                            }
                        }
                    }
                    v2[by * gw + bx] = bv;
                }

            // Half resolution: the block itself (8x8); seeds = 2x the quarter vectors of it and its 4 neighbours,
            // then +-1 steps around the best (twice).
            const Plane &t1 = T.l1, &r1 = R.l1;
            for (int by = 0; by < gh; ++by)
                for (int bx = 0; bx < gw; ++bx)
                {
                    const int hx = std::min(originX(bx) / 2, t1.w - 8);
                    const int hy = std::min(originY(by) / 2, t1.h - 8);
                    const uint8_t *tb = t1.at(hx, hy);
                    const Vec pred{2 * v2[by * gw + bx].x, 2 * v2[by * gw + bx].y};
                    uint32_t best = UINT_MAX;
                    Vec bv;
                    auto eval = [&](Vec d) {
                        const int rx = hx + d.x, ry = hy + d.y;
                        if (rx < 0 || ry < 0 || rx > r1.w - 8 || ry > r1.h - 8)
                            return;
                        const uint32_t c = sad8x8(tb, t1.w, r1.at(rx, ry), r1.w) +
                                           kLambda1 * static_cast<uint32_t>(std::abs(d.x - pred.x) + std::abs(d.y - pred.y));
                        if (c < best)
                        {
                            best = c;
                            bv = d;
                        }
                    };
                    eval(pred);
                    eval(Vec{0, 0});
                    const int nb[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
                    for (const auto &o : nb)
                    {
                        const int nx = bx + o[0], ny = by + o[1];
                        if (nx >= 0 && ny >= 0 && nx < gw && ny < gh)
                            eval(Vec{2 * v2[ny * gw + nx].x, 2 * v2[ny * gw + nx].y});
                    }
                    for (int it = 0; it < 2; ++it)
                    {
                        const Vec c = bv;
                        for (int dy = -1; dy <= 1; ++dy)
                            for (int dx = -1; dx <= 1; ++dx)
                                if (dx || dy)
                                    eval(Vec{c.x + dx, c.y + dy});
                        if (bv.x == c.x && bv.y == c.y)
                            break;
                    }
                    v1[by * gw + bx] = bv;
                }

            // Full resolution: 16x16 block; 2x the half vector +-1, zero, and the left/top results.
            const Plane &t0 = T.l0, &r0 = R.l0;
            for (int by = 0; by < gh; ++by)
                for (int bx = 0; bx < gw; ++bx)
                {
                    const int ox = originX(bx), oy = originY(by);
                    const uint8_t *tb = t0.at(ox, oy);
                    const Vec pred{2 * v1[by * gw + bx].x, 2 * v1[by * gw + bx].y};
                    uint32_t best = UINT_MAX, bestSad = 0;
                    Vec bv;
                    auto eval = [&](Vec d) {
                        const int rx = ox + d.x, ry = oy + d.y;
                        if (rx < 0 || ry < 0 || rx > r0.w - kBlock || ry > r0.h - kBlock)
                            return;
                        const uint32_t s = sad16x16(tb, t0.w, r0.at(rx, ry), r0.w);
                        const uint32_t c = s + kLambda0 * static_cast<uint32_t>(std::abs(d.x - pred.x) + std::abs(d.y - pred.y));
                        if (c < best)
                        {
                            best = c;
                            bestSad = s;
                            bv = d;
                        }
                    };
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx)
                            eval(Vec{pred.x + dx, pred.y + dy});
                    eval(Vec{0, 0});
                    if (bx > 0)
                        eval(out[by * gw + bx - 1]);
                    if (by > 0)
                        eval(out[(by - 1) * gw + bx]);
                    if (best == UINT_MAX) // every candidate out of range (tiny picture)
                        bestSad = sad16x16(tb, t0.w, r0.at(ox, oy), r0.w), bv = {};
                    out[by * gw + bx] = bv;
                    outSad[by * gw + bx] = bestSad;
                }
        }

        void estimate(const Pyramid &A, const Pyramid &B, Field &f, int cutSad)
        {
            const int w = A.l0.w, h = A.l0.h;
            f.gw = std::min((w + kBlock - 1) / kBlock, kFieldMaxW);
            f.gh = std::min((h + kBlock - 1) / kBlock, kFieldMaxH);
            const int n = f.gw * f.gh;
            std::vector<Vec> dB, dF;
            std::vector<uint32_t> sB, sF;
            searchField(B, A, f.gw, f.gh, dB, sB); // B(x) ~ A(x + d): content moved by -d
            searchField(A, B, f.gw, f.gh, dF, sF); // A(x) ~ B(x + d): content moved by +d
            f.bwd.resize(static_cast<size_t>(n) * 4);
            f.fwd.resize(static_cast<size_t>(n) * 4);
            auto pack = [](uint8_t *p, int mx, int my, uint32_t sad) {
                p[0] = static_cast<uint8_t>(std::clamp(mx + 128, 0, 255));
                p[1] = static_cast<uint8_t>(std::clamp(my + 128, 0, 255));
                p[2] = static_cast<uint8_t>(std::min<uint32_t>(sad / (kBlock * kBlock), 255u));
                p[3] = 255;
            };
            double sum = 0.0;
            int bad = 0;
            for (int i = 0; i < n; ++i)
            {
                pack(&f.bwd[i * 4], -dB[i].x, -dB[i].y, sB[i]);
                pack(&f.fwd[i * 4], dF[i].x, dF[i].y, sF[i]);
                const double perPixel = static_cast<double>(sB[i]) / (kBlock * kBlock);
                sum += perPixel;
                bad += perPixel > 32.0 ? 1 : 0;
            }
            f.meanSad = static_cast<float>(sum / std::max(1, n));
            f.badFrac = static_cast<float>(bad) / std::max(1, n);
            f.cut = f.meanSad > static_cast<float>(cutSad) || f.badFrac > 0.5f;
        }

        // ------------------------------------------------------------------ GPU pass

        const char *kInterpFs = R"(#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0; // older frame
uniform sampler2D texB;     // newer frame
uniform sampler2D fieldB;   // per block on the newer frame's grid: content motion older->newer (+128)
uniform sampler2D fieldF;   // the same on the older frame's grid
uniform float alpha;        // 0 = older frame, 1 = newer frame
uniform vec2 picSize;
uniform vec2 fieldSize;
uniform vec2 failRange;     // mean abs error per channel: start / end of the fallback
uniform float debugMode;
uniform vec2 vecScale;      // picture pixels per motion-search pixel (internal resolution), 1 at GS resolution
out vec4 finalColor;

ivec2 pix(vec2 p) { return ivec2(clamp(floor(p), vec2(0.0), picSize - 1.0)); }
vec4 bil(sampler2D t, vec2 p)
{
    vec2 q = clamp(p, vec2(0.5), picSize - 0.5) - 0.5;
    vec2 f = fract(q);
    ivec2 i = ivec2(floor(q));
    ivec2 mx = ivec2(picSize) - 1;
    vec4 c00 = texelFetch(t, i, 0);
    vec4 c10 = texelFetch(t, min(i + ivec2(1, 0), mx), 0);
    vec4 c01 = texelFetch(t, min(i + ivec2(0, 1), mx), 0);
    vec4 c11 = texelFetch(t, min(i + ivec2(1, 1), mx), 0);
    return mix(mix(c00, c10, f.x), mix(c01, c11, f.x), f.y);
}
vec2 vecAt(sampler2D f, ivec2 b)
{
    b = clamp(b, ivec2(0), ivec2(fieldSize) - 1);
    return (floor(texelFetch(f, b, 0).rg * 255.0 + 0.5) - 128.0) * vecScale;
}
// Match error of vector m at p: mean abs difference per channel over a 5x5 area (3x3 taps, 2 px apart) with the
// centre counted 4 more times, so thin objects (tree trunks, poles) pick their own vector over the background's.
// `centre` = the centre tap alone.
float cost(vec2 p, vec2 m, out float centre)
{
    vec2 pa = p - alpha * m;
    vec2 pb = p + (1.0 - alpha) * m;
    float e = 0.0;
    centre = 0.0;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx)
        {
            vec2 o = vec2(dx, dy) * 2.0 * vecScale;
            vec3 d = abs(texelFetch(texture0, pix(pa + o), 0).rgb - texelFetch(texB, pix(pb + o), 0).rgb);
            float s = d.r + d.g + d.b;
            if (dx == 0 && dy == 0)
            {
                // Bilinear here: at half-pixel positions a nearest tap misjudges 1-2 px wide objects.
                vec3 dc = abs(bil(texture0, pa).rgb - bil(texB, pb).rgb);
                s = dc.r + dc.g + dc.b;
                centre = s * (1.0 / 3.0);
                s *= 5.0;
            }
            e += s;
        }
    return e * (1.0 / 39.0);
}
void main()
{
    vec2 p = gl_FragCoord.xy; // render target row r = picture row r (drawn like any uploaded frame)
    if (p.x > picSize.x || p.y > picSize.y)
    {
        finalColor = vec4(0.0);
        return;
    }
    vec2 blockPx = 16.0 * vecScale;
    vec2 bp = p / blockPx - 0.5;
    ivec2 b0 = ivec2(floor(bp));
    vec2 bf = fract(bp);
    vec2 m00 = vecAt(fieldB, b0), m10 = vecAt(fieldB, b0 + ivec2(1, 0));
    vec2 m01 = vecAt(fieldB, b0 + ivec2(0, 1)), m11 = vecAt(fieldB, b0 + ivec2(1, 1));
    vec2 sm = mix(mix(m00, m10, bf.x), mix(m01, m11, bf.x), bf.y);
    // Candidates: the smooth vector, the 3x3 backward blocks around the pixel's own block (thin objects borrow
    // the vector of nearby foreground), the 2x2 nearest forward blocks.
    ivec2 bc = ivec2(floor(p / blockPx));
    vec2 cands[14] = vec2[14](sm,
                              vecAt(fieldB, bc + ivec2(-1, -1)), vecAt(fieldB, bc + ivec2(0, -1)), vecAt(fieldB, bc + ivec2(1, -1)),
                              vecAt(fieldB, bc + ivec2(-1, 0)), vecAt(fieldB, bc), vecAt(fieldB, bc + ivec2(1, 0)),
                              vecAt(fieldB, bc + ivec2(-1, 1)), vecAt(fieldB, bc + ivec2(0, 1)), vecAt(fieldB, bc + ivec2(1, 1)),
                              vecAt(fieldF, b0), vecAt(fieldF, b0 + ivec2(1, 0)),
                              vecAt(fieldF, b0 + ivec2(0, 1)), vecAt(fieldF, b0 + ivec2(1, 1)));
    vec2 best = vec2(0.0);
    float bestC;
    float bestE = cost(p, best, bestC) - 0.004; // still pixels (HUD, UI) stay put
    for (int i = 0; i < 14; ++i)
    {
        float c;
        float e = cost(p, cands[i], c) + 0.0015 * length((cands[i] - sm) / vecScale);
        if (e < bestE)
        {
            bestE = e;
            bestC = c;
            best = cands[i];
        }
    }
    vec4 ca = bil(texture0, p - alpha * best);
    vec4 cb = bil(texB, p + (1.0 - alpha) * best);
    vec4 mc = mix(ca, cb, alpha);
    vec4 nearest = alpha < 0.5 ? ca : cb; // one-sided along the same vector: no pieces drawn out of place
    // Give up where the whole area matches badly (occlusion edges, effects): one-sided from the nearer frame.
    // Not per pixel: a thin object moving unlike its background (tree trunk, pole) then vanished from the
    // generated frame; blended it stays visible, a little soft, at about the right place.
    float fail = smoothstep(failRange.x, failRange.y, bestE);
    vec4 c = mix(mc, nearest, fail);
    if (debugMode > 0.5)
        c = mix(c, vec4(1.0, 0.0, 1.0, 1.0), fail * 0.7);
    finalColor = c;
}
)";

        // Copies a texture texel for texel into a render target (same rows as the source).
        const char *kCopyFs = R"(#version 330
uniform sampler2D texture0;
out vec4 finalColor;
void main() { finalColor = texelFetch(texture0, ivec2(gl_FragCoord.xy), 0); }
)";

        // ------------------------------------------------------------------ state (window thread)

        struct Source
        {
            uint64_t serial = 0; // distinct pictures are numbered 1, 2, 3...: the playhead's timeline
            int64_t arrival = 0; // present index
            uint32_t w = 0, h = 0;
            int slot = 0;
            bool hires = false; // hires[slot] holds this frame's internal-resolution picture
            bool shown = false;
            Clock::time_point t;
        };

        struct Stats
        {
            int shown = 0, real = 0, generated = 0, cut = 0, late = 0, gap = 0, repeats = 0;
            double latencySum = 0.0, latencyMax = 0.0, waitMax = 0.0;
            int latencyN = 0;
            double meSum = 0.0, sadSum = 0.0;
            int meN = 0;
        };

        struct State
        {
            bool inited = false;
            double lag = 1.0; // source frames the playhead stays behind the newest picture
            int cutSad = 24;
            float debug = 0.0f;
            bool log = false;
            bool trace = false;
            int waitMs = 8;
            int texW = 0, texH = 0;
            Texture2D tex[kRing]{};
            int nextSlot = 0;
            RenderTexture2D rt{};
            Shader sh{};
            int locTexB = -1, locFieldB = -1, locFieldF = -1, locAlpha = -1, locPic = -1, locField = -1, locFail = -1,
                locDebug = -1, locVecScale = -1;
            int filter = TEXTURE_FILTER_BILINEAR;
            Shader copySh{};
            RenderTexture2D hires[kRing]{}; // internal-resolution copies, by slot (allocated at the first one)
            RenderTexture2D hiresRt{};      // the in-between picture at internal resolution
            int hiresW = 0, hiresH = 0;
            Texture2D fieldB{}, fieldF{};
            uint64_t fieldSerial = 0;

            std::vector<uint8_t> last;
            uint32_t lastW = 0, lastH = 0;
            bool hasLast = false;
            uint64_t serial = 0;
            std::deque<Source> sources;
            std::deque<int64_t> recentArrivals; // for the source rate
            int64_t presentIndex = 0;
            double head = -1.0; // playhead on the source timeline (serial + fraction)
            uint64_t shownKey[3] = {0, 0, 0};

            Stats stats;
            Clock::time_point statsStart;

            // worker
            std::thread worker;
            std::mutex m;
            std::condition_variable cv, doneCv;
            std::deque<Job> jobs;
            std::deque<std::shared_ptr<Field>> results;
            bool stop = false;
        };

        State &st()
        {
            static State s;
            return s;
        }

        void workerLoop()
        {
            ThreadNaming::SetCurrentThreadName("FrameGen");
            State &s = st();
            std::shared_ptr<Pyramid> cached;
            uint64_t cachedSerial = 0;
            for (;;)
            {
                Job j;
                {
                    std::unique_lock<std::mutex> lock(s.m);
                    s.cv.wait(lock, [&] { return s.stop || !s.jobs.empty(); });
                    if (s.stop)
                        return;
                    j = std::move(s.jobs.front());
                    s.jobs.pop_front();
                }
                const auto t0 = Clock::now();
                auto pyr = std::make_shared<Pyramid>();
                buildPyramid(j.rgba->data(), j.stride, j.w, j.h, *pyr);
                auto f = std::make_shared<Field>();
                f->serial = j.serial;
                if (cached && j.prevSerial != 0 && cachedSerial == j.prevSerial && cached->l0.w == j.w &&
                    cached->l0.h == j.h)
                    estimate(*cached, *pyr, *f, s.cutSad);
                else
                    f->cut = true;
                cached = std::move(pyr);
                cachedSerial = j.serial;
                f->ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
                {
                    std::lock_guard<std::mutex> lock(s.m);
                    s.results.push_back(std::move(f));
                    while (s.results.size() > 8)
                        s.results.pop_front();
                }
                s.doneCv.notify_all();
            }
        }

        // The motion field of the pair ending at `serial`, waiting up to waitMs for the worker.
        std::shared_ptr<Field> fieldFor(uint64_t serial, double &waitedMs)
        {
            State &s = st();
            const auto t0 = Clock::now();
            std::unique_lock<std::mutex> lock(s.m);
            std::shared_ptr<Field> found;
            s.doneCv.wait_until(lock, t0 + std::chrono::milliseconds(s.waitMs), [&] {
                for (const auto &r : s.results)
                    if (r->serial == serial)
                    {
                        found = r;
                        return true;
                    }
                return false;
            });
            waitedMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
            return found;
        }

        // The in-between picture of a and b into s.rt, or into s.hiresRt from their internal-resolution copies.
        void renderInterp(const Source &a, const Source &b, const Field &f, float alpha, bool hires)
        {
            State &s = st();
            if (s.fieldSerial != f.serial)
            {
                const Rectangle rect{0.0f, 0.0f, static_cast<float>(f.gw), static_cast<float>(f.gh)};
                UpdateTextureRec(s.fieldB, rect, f.bwd.data());
                UpdateTextureRec(s.fieldF, rect, f.fwd.data());
                s.fieldSerial = f.serial;
            }
            const float pic[2] = {static_cast<float>(hires ? s.hiresW : a.w), static_cast<float>(hires ? s.hiresH : a.h)};
            const float vecScale[2] = {pic[0] / static_cast<float>(a.w), pic[1] / static_cast<float>(a.h)};
            const float field[2] = {static_cast<float>(f.gw), static_cast<float>(f.gh)};
            const float fail[2] = {0.05f, 0.12f};
            const Texture2D &texA = hires ? s.hires[a.slot].texture : s.tex[a.slot];
            const Texture2D &texB = hires ? s.hires[b.slot].texture : s.tex[b.slot];
            BeginTextureMode(hires ? s.hiresRt : s.rt);
            rlDisableColorBlend(); // write the pass's colour and alpha as they are
            BeginShaderMode(s.sh);
            SetShaderValue(s.sh, s.locAlpha, &alpha, SHADER_UNIFORM_FLOAT);
            SetShaderValue(s.sh, s.locPic, pic, SHADER_UNIFORM_VEC2);
            SetShaderValue(s.sh, s.locField, field, SHADER_UNIFORM_VEC2);
            SetShaderValue(s.sh, s.locFail, fail, SHADER_UNIFORM_VEC2);
            SetShaderValue(s.sh, s.locDebug, &s.debug, SHADER_UNIFORM_FLOAT);
            SetShaderValue(s.sh, s.locVecScale, vecScale, SHADER_UNIFORM_VEC2);
            SetShaderValueTexture(s.sh, s.locTexB, texB);
            SetShaderValueTexture(s.sh, s.locFieldB, s.fieldB);
            SetShaderValueTexture(s.sh, s.locFieldF, s.fieldF);
            const Rectangle full{0.0f, 0.0f, static_cast<float>(texA.width), static_cast<float>(texA.height)};
            DrawTexturePro(texA, full, full, Vector2{0.0f, 0.0f}, 0.0f, WHITE);
            EndShaderMode();
            rlEnableColorBlend();
            EndTextureMode();
        }

        // PS2X_FRAMEGEN_DUMP=<dir>,<first present>,<presents>: every arrival and every shown picture in that
        // window as <dir>/p<present>_<what>.rgba ("w h" line + RGBA, tools/rgba2png.py converts).
        struct Dump
        {
            std::string dir;
            int64_t first = -1, count = 0;
            bool active(int64_t present) const { return !dir.empty() && present >= first && present < first + count; }
        };
        Dump &dump()
        {
            static Dump d = [] {
                Dump r;
                if (const char *e = std::getenv("PS2X_FRAMEGEN_DUMP"))
                {
                    std::string v(e);
                    const size_t c1 = v.find(','), c2 = v.find(',', c1 == std::string::npos ? c1 : c1 + 1);
                    if (c1 != std::string::npos && c2 != std::string::npos)
                    {
                        r.dir = v.substr(0, c1);
                        r.first = std::atoll(v.c_str() + c1 + 1);
                        r.count = std::atoll(v.c_str() + c2 + 1);
                    }
                }
                return r;
            }();
            return d;
        }
        void dumpPicture(int64_t present, const char *what, const uint8_t *rgba, int w, int h, int stride)
        {
            char path[512];
            std::snprintf(path, sizeof(path), "%s/p%06lld_%s.rgba", dump().dir.c_str(), static_cast<long long>(present), what);
            if (FILE *f = std::fopen(path, "wb"))
            {
                std::fprintf(f, "%d %d\n", w, h);
                for (int y = 0; y < h; ++y)
                    std::fwrite(rgba + static_cast<size_t>(y) * stride * 4, 1, static_cast<size_t>(w) * 4, f);
                std::fclose(f);
            }
        }

        void logStats()
        {
            State &s = st();
            const auto now = Clock::now();
            const double secs = std::chrono::duration<double>(now - s.statsStart).count();
            if (secs < 1.0)
                return;
            if (s.log)
            {
                Stats &t = s.stats;
                std::fprintf(stderr,
                             "[framegen] shown %.0f/s (real %.0f, generated %.0f, repeats %.0f) held: cut %d late %d gap %d"
                             "  latency %.1f ms (max %.1f)  me %.2f ms  sad %.1f  wait max %.1f ms\n",
                             t.shown / secs, t.real / secs, t.generated / secs, t.repeats / secs, t.cut, t.late, t.gap,
                             t.latencyN ? t.latencySum / t.latencyN : 0.0, t.latencyMax, t.meN ? t.meSum / t.meN : 0.0,
                             t.meN ? t.sadSum / t.meN : 0.0, t.waitMax);
            }
            s.stats = Stats{};
            s.statsStart = now;
        }
    }

    bool enabled()
    {
        static const bool on = [] {
            const char *e = std::getenv("PS2X_FRAMEGEN");
            if (!(e && *e && *e != '0'))
                return false;
            // The motion search needs a processor of its own. Down to 2 logical processors (1 P-core + HT) it
            // cost no measurable game FPS (lot 12.8 -> 12.6, 4 E-cores pan 14.8 -> 15.0); with 1 it would run
            // on the emulator's time. PS2X_FRAMEGEN=2 forces it on.
            if (ps2xHostCpu().logical < 2 && std::strcmp(e, "2") != 0)
            {
                std::fprintf(stderr, "[framegen] off: only 1 logical processor (PS2X_FRAMEGEN=2 forces it on)\n");
                return false;
            }
            return true;
        }();
        return on;
    }

    void init(int texWidth, int texHeight, int textureFilter)
    {
        State &s = st();
        if (s.inited || !enabled())
            return;
        s.lag = std::clamp(envInt("PS2X_FRAMEGEN_LAG", 100), 50, 300) / 100.0;
        s.cutSad = envInt("PS2X_FRAMEGEN_CUT", 24);
        s.debug = envInt("PS2X_FRAMEGEN_DEBUG", 0) ? 1.0f : 0.0f;
        s.log = envInt("PS2X_FRAMEGEN_LOG", 0) || envInt("PS2X_FPS_LOG", 0);
        s.trace = envInt("PS2X_FRAMEGEN_LOG", 0) >= 2;
        s.waitMs = 10;
        s.texW = texWidth;
        s.texH = texHeight;

        s.sh = LoadShaderFromMemory(nullptr, kInterpFs);
        if (!IsShaderValid(s.sh))
        {
            std::fprintf(stderr, "[framegen] shader failed to compile, frame generation off\n");
            return;
        }
        s.locTexB = GetShaderLocation(s.sh, "texB");
        s.locFieldB = GetShaderLocation(s.sh, "fieldB");
        s.locFieldF = GetShaderLocation(s.sh, "fieldF");
        s.locAlpha = GetShaderLocation(s.sh, "alpha");
        s.locPic = GetShaderLocation(s.sh, "picSize");
        s.locField = GetShaderLocation(s.sh, "fieldSize");
        s.locFail = GetShaderLocation(s.sh, "failRange");
        s.locDebug = GetShaderLocation(s.sh, "debugMode");
        s.locVecScale = GetShaderLocation(s.sh, "vecScale");
        s.copySh = LoadShaderFromMemory(nullptr, kCopyFs);
        s.filter = textureFilter;

        Image blank = GenImageColor(texWidth, texHeight, BLANK);
        for (Texture2D &t : s.tex)
        {
            t = LoadTextureFromImage(blank);
            SetTextureFilter(t, textureFilter);
        }
        UnloadImage(blank);
        Image zero = GenImageColor(kFieldMaxW, kFieldMaxH, Color{128, 128, 0, 255});
        s.fieldB = LoadTextureFromImage(zero);
        s.fieldF = LoadTextureFromImage(zero);
        UnloadImage(zero);
        s.rt = LoadRenderTexture(texWidth, texHeight);
        SetTextureFilter(s.rt.texture, textureFilter);
        s.last.assign(static_cast<size_t>(texWidth) * texHeight * 4, 0u);
        s.statsStart = Clock::now();
        s.stop = false;
        s.worker = std::thread(workerLoop);
        s.inited = true;
        std::fprintf(stderr, "[framegen] on: lag %.2f source frames, cut SAD %d\n", s.lag, s.cutSad);
    }

    void shutdown()
    {
        State &s = st();
        if (!s.inited)
            return;
        {
            std::lock_guard<std::mutex> lock(s.m);
            s.stop = true;
        }
        s.cv.notify_all();
        if (s.worker.joinable())
            s.worker.join();
        for (Texture2D &t : s.tex)
            UnloadTexture(t);
        UnloadTexture(s.fieldB);
        UnloadTexture(s.fieldF);
        UnloadRenderTexture(s.rt);
        if (s.hiresW > 0)
        {
            for (RenderTexture2D &t : s.hires)
                UnloadRenderTexture(t);
            UnloadRenderTexture(s.hiresRt);
            s.hiresW = s.hiresH = 0;
        }
        UnloadShader(s.copySh);
        UnloadShader(s.sh);
        s.inited = false;
    }

    void reset()
    {
        State &s = st();
        if (!s.inited)
            return;
        s.sources.clear();
        s.hasLast = false;
        s.head = -1.0;
    }

    void submit(const uint8_t *rgba, uint32_t width, uint32_t height)
    {
        State &s = st();
        if (!s.inited || !rgba || width == 0 || height == 0)
            return;
        width = std::min<uint32_t>(width, static_cast<uint32_t>(s.texW));
        height = std::min<uint32_t>(height, static_cast<uint32_t>(s.texH));
        const size_t bytes = static_cast<size_t>(s.texW) * s.texH * 4;
        // Only a changed picture is a new source frame. Guest page flips are no use as a clock here: the latched
        // picture follows the flip by 0-1 presents (GS feed lag), so they would make the timeline jitter.
        if (s.hasLast && width == s.lastW && height == s.lastH && std::memcmp(s.last.data(), rgba, bytes) == 0)
            return;

        Source src;
        src.serial = ++s.serial;
        src.arrival = s.presentIndex + 1; // present() runs right after this in the same window-loop pass
        src.w = width;
        src.h = height;
        src.t = Clock::now();
        src.slot = s.nextSlot;
        s.nextSlot = (s.nextSlot + 1) % kRing;
        UpdateTexture(s.tex[src.slot], rgba);
        std::memcpy(s.last.data(), rgba, bytes);
        const bool contiguous = s.hasLast && !s.sources.empty() && s.sources.back().serial + 1 == src.serial &&
                                width == s.lastW && height == s.lastH && width >= 64 && height >= 64;
        s.lastW = width;
        s.lastH = height;
        s.hasLast = true;

        Job j;
        j.prevSerial = contiguous ? src.serial - 1 : 0;
        j.serial = src.serial;
        j.w = static_cast<int>(width);
        j.h = static_cast<int>(height);
        j.stride = s.texW;
        j.rgba = std::make_shared<std::vector<uint8_t>>(rgba, rgba + bytes);
        {
            std::lock_guard<std::mutex> lock(s.m);
            s.jobs.push_back(std::move(j));
        }
        s.cv.notify_one();

        if (s.trace)
            std::fprintf(stderr, "[fg] arrive p%lld serial %llu\n", static_cast<long long>(src.arrival),
                         static_cast<unsigned long long>(src.serial));
        if (dump().active(src.arrival))
        {
            char what[64];
            std::snprintf(what, sizeof(what), "arrive%llu", static_cast<unsigned long long>(src.serial));
            dumpPicture(src.arrival, what, rgba, static_cast<int>(width), static_cast<int>(height), s.texW);
        }
        s.sources.push_back(src);
        s.recentArrivals.push_back(src.arrival);
        // The texture ring holds kRing pictures; the playhead never needs more than the newest three.
        while (s.sources.size() > kRing - 1)
            s.sources.pop_front();
    }

    void submitHires(unsigned tex, int width, int height)
    {
        State &s = st();
        if (!s.inited || !IsShaderValid(s.copySh) || width <= 0 || height <= 0 || s.sources.empty())
            return;
        Source &src = s.sources.back();
        if (src.arrival != s.presentIndex + 1 || src.hires)
            return; // only the frame that arrived this present (the picture belongs to it)
        if (width != s.hiresW || height != s.hiresH)
        {
            // First picture, or the internal resolution changed: (re)allocate, and older copies no longer match.
            if (s.hiresW > 0)
            {
                for (RenderTexture2D &t : s.hires)
                    UnloadRenderTexture(t);
                UnloadRenderTexture(s.hiresRt);
            }
            for (RenderTexture2D &t : s.hires)
            {
                t = LoadRenderTexture(width, height);
                SetTextureFilter(t.texture, s.filter);
            }
            s.hiresRt = LoadRenderTexture(width, height);
            SetTextureFilter(s.hiresRt.texture, s.filter);
            s.hiresW = width;
            s.hiresH = height;
            for (Source &o : s.sources)
                o.hires = false;
            std::fprintf(stderr, "[framegen] internal-resolution pictures %dx%d\n", width, height);
        }
        Texture2D in{};
        in.id = tex;
        in.width = width;
        in.height = height;
        in.mipmaps = 1;
        in.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
        BeginTextureMode(s.hires[src.slot]);
        rlDisableColorBlend();
        BeginShaderMode(s.copySh);
        const Rectangle full{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)};
        DrawTexturePro(in, full, full, Vector2{0.0f, 0.0f}, 0.0f, WHITE);
        EndShaderMode();
        rlEnableColorBlend();
        EndTextureMode();
        src.hires = true;
    }

    const Texture2D *present(uint32_t &width, uint32_t &height)
    {
        State &s = st();
        if (!s.inited)
            return nullptr;
        ++s.presentIndex;
        logStats();
        if (s.sources.empty())
            return nullptr;

        // Jitter buffer: the playhead advances by the measured source rate (frames per present) and is pulled
        // gently towards `lag` frames behind where the newest picture would be by now; it never runs backwards,
        // past the newest picture, or far behind it.
        constexpr int64_t kRateWindow = 32;
        while (!s.recentArrivals.empty() && s.recentArrivals.front() <= s.presentIndex - kRateWindow)
            s.recentArrivals.pop_front();
        const double rate =
            std::clamp(static_cast<double>(s.recentArrivals.size()) / kRateWindow, 1.0 / kRateWindow, 1.0);
        const Source &newest = s.sources.back();
        const double newestPos = static_cast<double>(newest.serial);
        const double oldestPos = static_cast<double>(s.sources.front().serial);
        const double target = newestPos + static_cast<double>(s.presentIndex - newest.arrival) * rate - s.lag;
        double head = s.head;
        if (head < 0.0 || head < newestPos - 2.0 - s.lag)
            head = target;
        else
        {
            const double next = head + rate;
            head = std::max(head, next + 0.15 * (target - next));
        }
        head = std::clamp(head, oldestPos, newestPos);
        s.head = head;

        uint64_t k = static_cast<uint64_t>(head);
        float alpha = static_cast<float>(head - static_cast<double>(k));
        if (alpha < 0.08f)
            alpha = 0.0f;
        else if (alpha > 0.92f)
        {
            ++k;
            alpha = 0.0f;
        }
        while (s.sources.size() > 1 && s.sources.front().serial < k)
            s.sources.pop_front();
        Source &a = s.sources.front();
        const Texture2D *out = &s.tex[a.slot];
        width = a.w;
        height = a.h;
        if (a.hires)
        {
            out = &s.hires[a.slot].texture;
            width = static_cast<uint32_t>(s.hiresW);
            height = static_cast<uint32_t>(s.hiresH);
        }
        float shownAlpha = 0.0f;
        if (alpha > 0.0f && s.sources.size() > 1)
        {
            const Source &b = s.sources[1];
            if (b.arrival - a.arrival > kMaxGap || b.w != a.w || b.h != a.h)
                ++s.stats.gap;
            else
            {
                double waited = 0.0;
                const std::shared_ptr<Field> f = fieldFor(b.serial, waited);
                s.stats.waitMax = std::max(s.stats.waitMax, waited);
                if (!f)
                    ++s.stats.late;
                else
                {
                    if (s.fieldSerial != f->serial)
                    {
                        s.stats.meSum += f->ms;
                        s.stats.sadSum += f->meanSad;
                        ++s.stats.meN;
                    }
                    if (f->cut)
                        ++s.stats.cut;
                    else
                    {
                        const bool hires = a.hires && b.hires;
                        renderInterp(a, b, *f, alpha, hires);
                        out = hires ? &s.hiresRt.texture : &s.rt.texture;
                        width = hires ? static_cast<uint32_t>(s.hiresW) : a.w;
                        height = hires ? static_cast<uint32_t>(s.hiresH) : a.h;
                        shownAlpha = alpha;
                        ++s.stats.generated;
                    }
                }
            }
        }
        if (shownAlpha == 0.0f && !a.shown)
        {
            a.shown = true;
            const double ms = std::chrono::duration<double, std::milli>(Clock::now() - a.t).count();
            s.stats.latencySum += ms;
            s.stats.latencyMax = std::max(s.stats.latencyMax, ms);
            ++s.stats.latencyN;
            ++s.stats.real;
        }
        if (s.trace)
            std::fprintf(stderr, "[fg] present p%lld head %.3f rate %.3f shows %llu alpha %.2f\n",
                         static_cast<long long>(s.presentIndex), head, rate, static_cast<unsigned long long>(a.serial),
                         shownAlpha);
        if (dump().active(s.presentIndex) && shownAlpha != 0.0f)
        {
            // Debug only (a GPU read-back): the generated picture as shown.
            Image img = LoadImageFromTexture(s.rt.texture);
            char what[64];
            std::snprintf(what, sizeof(what), "gen%llu_%llu_a%02d", static_cast<unsigned long long>(a.serial),
                          static_cast<unsigned long long>(s.sources[1].serial), static_cast<int>(shownAlpha * 100.0f));
            dumpPicture(s.presentIndex, what, static_cast<const uint8_t *>(img.data), static_cast<int>(a.w),
                        static_cast<int>(a.h), img.width);
            UnloadImage(img);
        }
        // Distinct pictures shown (what the eye sees as frames): a new source picture or a generated one.
        uint64_t key[3] = {a.serial, 0, 0};
        if (shownAlpha != 0.0f)
        {
            uint32_t bits;
            std::memcpy(&bits, &shownAlpha, sizeof(bits));
            key[1] = s.sources[1].serial;
            key[2] = bits;
        }
        if (std::memcmp(key, s.shownKey, sizeof(key)) != 0)
            ++s.stats.shown;
        else
            ++s.stats.repeats;
        std::memcpy(s.shownKey, key, sizeof(key));
        return out;
    }
}
