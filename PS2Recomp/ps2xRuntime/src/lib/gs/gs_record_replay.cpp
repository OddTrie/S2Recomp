// GS command recording and offline replay, for checking rasterizer changes (e.g. banded multi-core
// rendering) for pixel-identical output and for benchmarking.
//
// PS2X_GS_RECORD=<file>,<startSec>,<commands>: from <startSec>, write a VRAM snapshot and then every
//   backend command (draws, CLUT loads, transfers, image uploads, texture flushes) to <file>.
//   <startSec> = key: start when F10 is pressed instead (Windows), to record a scene that has to be played to.
// ps2EntryRunner --gs-replay <file> <bands> [passes]: replay the commands through the CPU
//   rasterizer (bands 1 = single instance) and print a hash of all of VRAM every 2000 commands.
//   Replays with different band counts must print identical hash lists.
// ps2EntryRunner --gs-replay-check <file> <bands> [rounds]: that comparison as one command (see below).
#include "runtime/gs/gs_backend.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/gs_threaded_backend.h"
#include "runtime/gs/gs_gpu_backend.h"
#include "runtime/gs/ps2_gs_memory.h"
#include "raylib.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
extern "C" __declspec(dllimport) short __stdcall GetAsyncKeyState(int vKey);
#endif

namespace
{
    // F10 pressed since the last call (low bit of GetAsyncKeyState), whichever window has focus.
    bool recordKeyPressed()
    {
#if defined(_WIN32)
        return (GetAsyncKeyState(0x79 /* VK_F10 */) & 1) != 0;
#else
        return false;
#endif
    }

    enum class RecOp : uint8_t
    {
        Submit = 1,
        LoadClut = 2,
        BeginTransfer = 3,
        UploadImage = 4,
        TextureFlush = 5,
    };

    uint64_t hashBytes(const uint8_t *p, size_t n)
    {
        uint64_t h = 1469598103934665603ull;
        for (size_t i = 0; i < n; ++i)
        {
            h ^= p[i];
            h *= 1099511628211ull;
        }
        return h;
    }

    class GSRecordingBackend final : public GSRasterBackend
    {
    public:
        GSRecordingBackend(std::unique_ptr<GSRasterBackend> inner, std::string path, double startSec, uint32_t commands, bool onKey)
            : m_inner(std::move(inner)), m_path(std::move(path)), m_startSec(startSec), m_remaining(commands),
              m_t0(std::chrono::steady_clock::now()), m_onKey(onKey)
        {
            if (m_onKey)
            {
                recordKeyPressed(); // clear a stale press
                std::fprintf(stderr, "[gsrecord] press F10 to record %u GS commands to %s\n", commands, m_path.c_str());
            }
        }
        ~GSRecordingBackend() override
        {
            if (m_file)
                std::fclose(m_file);
        }

        void Initialize(uint8_t *vram, uint32_t vramSize) override { m_inner->Initialize(vram, vramSize); }
        void Reset() override { m_inner->Reset(); }
        void Submit(const GSPrimitiveBatch &batch) override
        {
            if (begin())
            {
                write(RecOp::Submit, &batch, sizeof(batch));
            }
            m_inner->Submit(batch);
        }
        void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) override
        {
            if (begin())
            {
                uint8_t buf[sizeof(tex0) + sizeof(texclut)];
                std::memcpy(buf, &tex0, sizeof(tex0));
                std::memcpy(buf + sizeof(tex0), &texclut, sizeof(texclut));
                write(RecOp::LoadClut, buf, sizeof(buf));
            }
            m_inner->LoadClut(tex0, texclut);
        }
        void BeginTransfer(const GSTransferCommand &command) override
        {
            if (begin())
                write(RecOp::BeginTransfer, &command, sizeof(command));
            m_inner->BeginTransfer(command);
        }
        void UploadImage(const uint8_t *data, uint32_t sizeBytes) override
        {
            if (begin())
                write(RecOp::UploadImage, data, sizeBytes);
            m_inner->UploadImage(data, sizeBytes);
        }
        void Flush() override { m_inner->Flush(); }
        void TextureFlush() override
        {
            if (begin())
                write(RecOp::TextureFlush, nullptr, 0);
            m_inner->TextureFlush();
        }
        void Sync(GSSyncReason reason) override { m_inner->Sync(reason); }
        PresentationFrame Present(const GSPresentationRequest &request) override { return m_inner->Present(request); }
        bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override { return m_inner->ClearFramebuffer(context, rgba); }
        uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override { return m_inner->ConsumeLocalToHostBytes(dst, maxBytes); }
        uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override { return m_inner->ReadVram(psm, base, bw, x, y); }
        void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override { m_inner->WriteVram(psm, base, bw, x, y, value); }
        void SnapshotVram(std::vector<uint8_t> &out) const override { m_inner->SnapshotVram(out); }
        GSTransferSnapshot GetTransferSnapshot() const override { return m_inner->GetTransferSnapshot(); }

    private:
        // True while recording; opens the file and writes the VRAM snapshot on first use.
        bool begin()
        {
            if (m_done)
                return false;
            if (!m_file)
            {
                if (m_onKey)
                {
                    if ((++m_keyPolls & 1023u) != 0u || !recordKeyPressed())
                        return false;
                }
                else if (std::chrono::duration<double>(std::chrono::steady_clock::now() - m_t0).count() < m_startSec)
                    return false;
                m_file = std::fopen(m_path.c_str(), "wb");
                if (!m_file)
                {
                    m_done = true;
                    return false;
                }
                std::vector<uint8_t> vram;
                m_inner->SnapshotVram(vram);
                const uint32_t size = static_cast<uint32_t>(vram.size());
                std::fwrite("GSR1", 1, 4, m_file);
                std::fwrite(&size, sizeof(size), 1, m_file);
                std::fwrite(vram.data(), 1, vram.size(), m_file);
                std::fprintf(stderr, "[gsrecord] started (%u bytes of VRAM)\n", size);
            }
            return true;
        }
        void write(RecOp op, const void *data, uint32_t size)
        {
            const uint8_t tag = static_cast<uint8_t>(op);
            std::fwrite(&tag, 1, 1, m_file);
            std::fwrite(&size, sizeof(size), 1, m_file);
            if (size)
                std::fwrite(data, 1, size, m_file);
            if (--m_remaining == 0u)
            {
                std::fclose(m_file);
                m_file = nullptr;
                m_done = true;
                std::fprintf(stderr, "[gsrecord] done\n");
            }
        }

        std::unique_ptr<GSRasterBackend> m_inner;
        std::string m_path;
        double m_startSec;
        uint32_t m_remaining;
        std::chrono::steady_clock::time_point m_t0;
        std::FILE *m_file = nullptr;
        bool m_done = false;
        bool m_onKey = false;
        uint32_t m_keyPolls = 0;
    };
}

// Wraps the default backend in a recorder when PS2X_GS_RECORD is set.
std::unique_ptr<GSRasterBackend> ps2xMaybeRecordGsBackend(std::unique_ptr<GSRasterBackend> backend)
{
    const char *env = std::getenv("PS2X_GS_RECORD");
    if (!env || !*env)
        return backend;
    std::string spec(env);
    const size_t c1 = spec.find(',');
    const size_t c2 = c1 == std::string::npos ? std::string::npos : spec.find(',', c1 + 1);
    const double start = c1 == std::string::npos ? 0.0 : std::atof(spec.c_str() + c1 + 1);
    const bool onKey = c1 != std::string::npos && spec.compare(c1 + 1, 3, "key") == 0;
    const uint32_t count = c2 == std::string::npos ? 200000u : static_cast<uint32_t>(std::strtoul(spec.c_str() + c2 + 1, nullptr, 0));
    return std::make_unique<GSRecordingBackend>(std::move(backend), spec.substr(0, c1), start, count, onKey);
}

namespace
{
    struct ReplayCmd
    {
        uint8_t op;
        std::vector<uint8_t> data;
    };

    bool loadRecording(const char *path, std::vector<uint8_t> &initialVram, std::vector<ReplayCmd> &cmds)
    {
        std::FILE *f = std::fopen(path, "rb");
        if (!f)
        {
            std::fprintf(stderr, "[gsreplay] cannot open %s\n", path);
            return false;
        }
        char magic[4]{};
        uint32_t vramSize = 0;
        if (std::fread(magic, 1, 4, f) != 4 || std::memcmp(magic, "GSR1", 4) != 0 || std::fread(&vramSize, 4, 1, f) != 1)
        {
            std::fprintf(stderr, "[gsreplay] not a GS recording\n");
            std::fclose(f);
            return false;
        }
        initialVram.resize(vramSize);
        if (std::fread(initialVram.data(), 1, vramSize, f) != vramSize)
        {
            std::fclose(f);
            return false;
        }
        for (;;)
        {
            uint8_t tag = 0;
            uint32_t size = 0;
            if (std::fread(&tag, 1, 1, f) != 1 || std::fread(&size, 4, 1, f) != 1)
                break;
            ReplayCmd c{tag, std::vector<uint8_t>(size)};
            if (size && std::fread(c.data.data(), 1, size, f) != size)
                break;
            cmds.push_back(std::move(c));
        }
        std::fclose(f);
        return true;
    }

    void replayCommand(GSRasterBackend &backend, const ReplayCmd &c)
    {
        switch (static_cast<RecOp>(c.op))
        {
        case RecOp::Submit:
        {
            GSPrimitiveBatch batch;
            std::memcpy(&batch, c.data.data(), sizeof(batch));
            backend.Submit(batch);
            break;
        }
        case RecOp::LoadClut:
        {
            GSTex0Reg tex0;
            GSTexClutReg texclut;
            std::memcpy(&tex0, c.data.data(), sizeof(tex0));
            std::memcpy(&texclut, c.data.data() + sizeof(tex0), sizeof(texclut));
            backend.LoadClut(tex0, texclut);
            break;
        }
        case RecOp::BeginTransfer:
        {
            GSTransferCommand t;
            std::memcpy(&t, c.data.data(), sizeof(t));
            backend.BeginTransfer(t);
            break;
        }
        case RecOp::UploadImage:
            backend.UploadImage(c.data.data(), static_cast<uint32_t>(c.data.size()));
            break;
        case RecOp::TextureFlush:
            backend.TextureFlush();
            break;
        }
    }

    std::unique_ptr<GSRasterBackend> makeReplayBackend(int bands, bool skew)
    {
        if (ps2xGsGpuRequested()) // PS2X_GS_GPU=1: time / dump the GPU renderer instead
        {
            ps2xGsGpuHostInit(true);
            return ps2xMakeGsGpuBackend();
        }
        if (bands <= 1)
            return std::make_unique<GSCpuBackend>();
        // PS2X_GS_BANDS_BASE=<n>: only n bands draw unless the queue is full (as in game);
        // with PS2X_GS_BANDS_FLIP=1 the drawing count alternates every batch (race check).
        int baseBands = 0;
        if (const char *e = std::getenv("PS2X_GS_BANDS_BASE"))
            baseBands = std::atoi(e);
        auto banded = std::make_unique<GSBandedBackend>(bands, baseBands);
        banded->SetBandSkewForTesting(skew);
        return banded;
    }

    constexpr size_t kCheckpointEvery = 2000;

    void dumpFrame(GSRasterBackend &backend, uint32_t fbp, size_t index)
    {
        backend.Sync(GSSyncReason::Finish);
        char name[64];
        std::snprintf(name, sizeof(name), "gsdump_%07zu.ppm", index);
        std::FILE *f = std::fopen(name, "wb");
        if (!f)
            return;
        std::fputs("P6 640 448 255\n", f);
        for (uint32_t y = 0; y < 448; ++y)
            for (uint32_t x = 0; x < 640; ++x)
            {
                const uint32_t c = backend.ReadVram(0, fbp << 5u, 10, x, y);
                const uint8_t rgb[3] = {static_cast<uint8_t>(c), static_cast<uint8_t>(c >> 8), static_cast<uint8_t>(c >> 16)};
                std::fwrite(rgb, 1, 3, f);
            }
        std::fclose(f);
    }

    // PS2X_GS_PROBE=x,y (with --gs-replay ... 1): print every primitive that changes frame pixel (x,y) of
    // its own FRAME buffer, with its state and vertices. Finds the draw behind a wrong-looking pixel.
    void probePixel(GSRasterBackend &backend, const ReplayCmd &c, size_t index, int px, int py)
    {
        if (static_cast<RecOp>(c.op) != RecOp::Submit)
        {
            replayCommand(backend, c);
            return;
        }
        GSPrimitiveBatch batch;
        std::memcpy(&batch, c.data.data(), sizeof(batch));
        const GSContext &ctx = batch.state.context;
        const uint32_t base = ctx.frame.fbp << 5u; // FBP is in 2048-word pages, ReadVram takes 64-word blocks
        const uint32_t before = backend.ReadVram(ctx.frame.psm, base, ctx.frame.fbw, px, py);
        replayCommand(backend, c);
        const uint32_t after = backend.ReadVram(ctx.frame.psm, base, ctx.frame.fbw, px, py);
        if (before == after)
            return;
        const GSPrimReg &p = batch.state.prim;
        std::printf("[probe] cmd %zu fbp=%u %08x->%08x prim=%d tme=%d abe=%d fst=%d iip=%d fge=%d alpha=%llx test=%llx "
                    "tex0 tbp=%u tbw=%u psm=%u %ux%u tfx=%u tcc=%u cbp=%u clamp=%llx tex1=%llx zbp=%u\n",
                    index, ctx.frame.fbp, before, after, static_cast<int>(p.type), p.tme, p.abe, p.fst, p.iip, p.fge,
                    static_cast<unsigned long long>(ctx.alpha), static_cast<unsigned long long>(ctx.test), ctx.tex0.tbp0,
                    ctx.tex0.tbw, ctx.tex0.psm, 1u << ctx.tex0.tw, 1u << ctx.tex0.th, ctx.tex0.tfx, ctx.tex0.tcc, ctx.tex0.cbp,
                    static_cast<unsigned long long>(ctx.clamp), static_cast<unsigned long long>(ctx.tex1), ctx.zbuf.zbp);
        if (p.tme && ctx.tex0.psm == 19u && ctx.tex0.cpsm == 0u) // PSMT8 + CT32 CLUT: write it as tex_<tbp>.ppm, once
        {
            static std::vector<uint32_t> dumped;
            if (std::find(dumped.begin(), dumped.end(), ctx.tex0.tbp0) == dumped.end())
            {
                dumped.push_back(ctx.tex0.tbp0);
                const uint32_t w = 1u << ctx.tex0.tw, h = 1u << ctx.tex0.th;
                char name[64];
                std::snprintf(name, sizeof(name), "tex_%u.ppm", ctx.tex0.tbp0);
                if (std::FILE *f = std::fopen(name, "wb"))
                {
                    std::fprintf(f, "P6 %u %u 255\n", w, h);
                    for (uint32_t y = 0; y < h; ++y)
                        for (uint32_t x = 0; x < w; ++x)
                        {
                            const uint32_t idx = backend.ReadVram(19, ctx.tex0.tbp0, ctx.tex0.tbw, x, y) & 0xFFu;
                            const uint32_t slot = (idx & 0xE7u) | ((idx & 0x08u) << 1) | ((idx & 0x10u) >> 1); // CSM1 order
                            const uint32_t c = backend.ReadVram(0, ctx.tex0.cbp, 1, slot & 15u, slot >> 4);
                            const uint8_t rgb[3] = {static_cast<uint8_t>(c), static_cast<uint8_t>(c >> 8), static_cast<uint8_t>(c >> 16)};
                            std::fwrite(rgb, 1, 3, f);
                        }
                    std::fclose(f);
                }
            }
        }
        for (int i = 0; i < batch.vertexCount; ++i)
        {
            const GSVertex &v = batch.vertices[i];
            std::printf("[probe]   v%d xy=(%.2f,%.2f) z=%.0f rgba=%u,%u,%u,%u q=%g st=(%g,%g) uv=(%u,%u)\n", i,
                        v.x - (ctx.xyoffset.ofx >> 4), v.y - (ctx.xyoffset.ofy >> 4), v.z, v.r, v.g, v.b, v.a, v.q, v.s, v.t, v.u, v.v);
        }
    }
}

int ps2xGsReplay(const char *path, int bands, int passes)
{
    std::vector<uint8_t> initialVram;
    std::vector<ReplayCmd> cmds;
    if (!loadRecording(path, initialVram, cmds))
        return 1;
    const uint32_t vramSize = static_cast<uint32_t>(initialVram.size());

    std::vector<uint8_t> vram(vramSize);
    double seconds = 0.0;
    int probeX = 0, probeY = 0;
    const char *probeEnv = std::getenv("PS2X_GS_PROBE");
    const bool probe = probeEnv && std::sscanf(probeEnv, "%d,%d", &probeX, &probeY) == 2;
    if (probe)
        bands = 1; // read pixels between draws: no rasterizer threads
    // PS2X_GS_DUMP=<fbp>,<every>: write the 640x448 PSMCT32 buffer at <fbp> to gsdump_<command>.ppm every <every> commands.
    unsigned dumpFbp = 0, dumpEvery = 0;
    if (const char *e = std::getenv("PS2X_GS_DUMP"))
        std::sscanf(e, "%u,%u", &dumpFbp, &dumpEvery);
    for (int pass = 0; pass < passes; ++pass)
    {
        std::memcpy(vram.data(), initialVram.data(), vramSize);
        std::unique_ptr<GSRasterBackend> backend = makeReplayBackend(bands, false);
        backend->Initialize(vram.data(), vramSize);
        std::vector<uint8_t> snap;
        const auto t0 = std::chrono::steady_clock::now();
        for (size_t i = 0; i < cmds.size(); ++i)
        {
            if (probe)
                probePixel(*backend, cmds[i], i, probeX, probeY);
            else
                replayCommand(*backend, cmds[i]);
            if (dumpEvery && pass == 0 && (i + 1) % dumpEvery == 0u)
                dumpFrame(*backend, dumpFbp, i + 1);
            if (pass == 0 && ((i + 1) % kCheckpointEvery == 0u || i + 1 == cmds.size()))
            {
                backend->SnapshotVram(snap); // waits for the rasterizer threads
                std::printf("[gsreplay] after %zu: %016llx\n", i + 1, static_cast<unsigned long long>(hashBytes(snap.data(), snap.size())));
            }
        }
        backend->Sync(GSSyncReason::Finish);
        // Pass 0 checks hashes (each snapshot drains the threads); time only the passes after it.
        if (pass > 0 || passes == 1)
            seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }
    const int timed = passes > 1 ? passes - 1 : 1;
    std::printf("[gsreplay] %zu commands, bands=%d: %.3f s per pass (%d timed)\n", cmds.size(), bands, seconds / timed, timed);
    return 0;
}

// --gs-replay-check <file> <bands> [rounds]: regression check for the banded rasterizer. Replays the
// recording once with one rasterizer, then <rounds> times with <bands>, and compares VRAM at every
// checkpoint. Odd rounds run with band skew (one band per batch made to lag), which turns a missing
// hazard barrier between bands into a mismatch instead of rare timing luck. Exit code 1 on mismatch.
int ps2xGsReplayCheck(const char *path, int bands, int rounds)
{
    std::vector<uint8_t> initialVram;
    std::vector<ReplayCmd> cmds;
    if (!loadRecording(path, initialVram, cmds))
        return 1;
    const uint32_t vramSize = static_cast<uint32_t>(initialVram.size());
    std::vector<uint8_t> vram(vramSize), snap;
    auto run = [&](int runBands, bool skew)
    {
        std::vector<uint64_t> hashes;
        std::memcpy(vram.data(), initialVram.data(), vramSize);
        std::unique_ptr<GSRasterBackend> backend = makeReplayBackend(runBands, skew);
        backend->Initialize(vram.data(), vramSize);
        for (size_t i = 0; i < cmds.size(); ++i)
        {
            replayCommand(*backend, cmds[i]);
            if ((i + 1) % kCheckpointEvery == 0u || i + 1 == cmds.size())
            {
                backend->SnapshotVram(snap);
                hashes.push_back(hashBytes(snap.data(), snap.size()));
            }
        }
        backend->Sync(GSSyncReason::Finish);
        return hashes;
    };
    const std::vector<uint64_t> reference = run(1, false);
    int failures = 0;
    for (int round = 0; round < rounds; ++round)
    {
        const bool skew = (round & 1) != 0;
        const std::vector<uint64_t> hashes = run(bands, skew);
        size_t match = 0;
        while (match < reference.size() && match < hashes.size() && hashes[match] == reference[match])
            ++match;
        if (match == reference.size())
        {
            std::printf("[gscheck] round %d (%d bands%s): all %zu checkpoints match\n", round, bands, skew ? ", skew" : "", reference.size());
            continue;
        }
        ++failures;
        std::printf("[gscheck] round %d (%d bands%s): MISMATCH from command %zu\n", round, bands, skew ? ", skew" : "",
                    std::min((match + 1) * kCheckpointEvery, cmds.size()));
    }
    std::printf("[gscheck] %s: %zu commands, %d of %d rounds mismatched\n", failures ? "FAIL" : "PASS", cmds.size(), failures, rounds);
    return failures ? 1 : 0;
}

// --gs-stats <file>: what a recording uses (primitive kinds, buffer and texture formats, blend and
// test modes, transfers, CLUT loads, render-to-texture), to scope the GPU renderer.
int ps2xGsStats(const char *path)
{
    std::vector<uint8_t> initialVram;
    std::vector<ReplayCmd> cmds;
    if (!loadRecording(path, initialVram, cmds))
        return 1;
    std::map<std::string, uint64_t> counts;
    auto bump = [&](const char *fmt, auto... args)
    {
        char key[200];
        std::snprintf(key, sizeof(key), fmt, args...);
        ++counts[key];
    };
    // Per 8 KB page: the frame/Z buffer that drew into it last.
    struct PageOwner
    {
        bool drawn = false;
        uint32_t psm = 0, base = 0, bw = 0;
    };
    std::vector<PageOwner> owner(512);
    auto pageRange = [](uint32_t psm, uint32_t baseBlock, uint32_t bw, int y0, int y1, uint32_t &p0, uint32_t &p1)
    {
        uint32_t pageW = 64u, pageH = 32u;
        switch (psm)
        {
        case GS_PSM_CT16: case GS_PSM_CT16S: case GS_PSM_Z16: case GS_PSM_Z16S: pageH = 64u; break;
        case GS_PSM_T8: pageW = 128u; pageH = 64u; break;
        case GS_PSM_T4: pageW = 128u; pageH = 128u; break;
        default: break;
        }
        const uint32_t perRow = (std::max<uint32_t>(bw, 1u) * 64u + pageW - 1u) / pageW;
        p0 = baseBlock / 32u + static_cast<uint32_t>(std::max(y0, 0)) / pageH * perRow;
        p1 = baseBlock / 32u + (static_cast<uint32_t>(std::max(y1, 0)) / pageH + 1u) * perRow + ((baseBlock & 31u) ? 1u : 0u);
    };
    auto readsDrawn = [&](const char *what, uint32_t psm, uint32_t baseBlock, uint32_t bw, int h)
    {
        uint32_t p0, p1;
        pageRange(psm, baseBlock, bw, 0, h - 1, p0, p1);
        for (uint32_t p = p0; p < p1; ++p)
        {
            const PageOwner &o = owner[p & 511u];
            if (!o.drawn)
                continue;
            const bool aligned = o.base * 32u == baseBlock && o.bw == bw;
            bump("rtt %s: drawn psm=%02x read as psm=%02x %s", what, o.psm, psm, aligned ? "same base/width" : "other base/width");
            return;
        }
    };
    uint64_t submits = 0;
    for (const ReplayCmd &c : cmds)
    {
        switch (static_cast<RecOp>(c.op))
        {
        case RecOp::Submit:
        {
            GSPrimitiveBatch b;
            std::memcpy(&b, c.data.data(), sizeof(b));
            ++submits;
            const GSDrawState &s = b.state;
            const GSContext &ctx = s.context;
            const GSPrimReg &p = s.prim;
            bump("prim type=%u tme=%u fst=%u iip=%u fge=%u abe=%u aa1=%u", p.type, p.tme, p.fst, p.iip, p.fge, p.abe, p.aa1);
            bump("frame psm=%02x fbw=%u fbmsk=%08x", ctx.frame.psm, ctx.frame.fbw, ctx.frame.fbmsk);
            const uint32_t ztst = static_cast<uint32_t>((ctx.test >> 17) & 3u);
            bump("zbuf psm=%02x zmsk=%u zte=%u ztst=%u", ctx.zbuf.psm, ctx.zbuf.zmask, static_cast<unsigned>((ctx.test >> 16) & 1u), ztst);
            bump("test ate=%u atst=%u aref=%02x afail=%u date=%u datm=%u", static_cast<unsigned>(ctx.test & 1u),
                 static_cast<unsigned>((ctx.test >> 1) & 7u), static_cast<unsigned>((ctx.test >> 4) & 0xFFu),
                 static_cast<unsigned>((ctx.test >> 12) & 3u), static_cast<unsigned>((ctx.test >> 14) & 1u),
                 static_cast<unsigned>((ctx.test >> 15) & 1u));
            if (p.abe)
                bump("alpha abcd=%02x fix=%02x pabe=%u", static_cast<unsigned>(ctx.alpha & 0xFFu),
                     static_cast<unsigned>((ctx.alpha >> 32) & 0xFFu), s.pabe ? 1u : 0u);
            bump("misc fba=%u colclamp=%u dthe=%u scanmsk=%u", static_cast<unsigned>(ctx.fba & 1u),
                 static_cast<unsigned>(s.colclamp & 1u), static_cast<unsigned>(s.dthe & 1u), static_cast<unsigned>(s.scanmsk & 3u));
            if (p.fge)
                bump("fog col=%02x%02x%02x", s.fogR, s.fogG, s.fogB);
            if (p.tme)
            {
                bump("tex psm=%02x cpsm=%02x csm=%u tfx=%u tcc=%u lin=%u tex1=%llx", ctx.tex0.psm, ctx.tex0.cpsm, ctx.tex0.csm,
                     ctx.tex0.tfx, ctx.tex0.tcc, s.linearFilter ? 1u : 0u, static_cast<unsigned long long>(ctx.tex1 & 0x1FFFFFFFFull));
                bump("clamp wms=%u wmt=%u", static_cast<unsigned>(ctx.clamp & 3u), static_cast<unsigned>((ctx.clamp >> 2) & 3u));
                bump("texsize %ux%u", s.textureWidth, s.textureHeight);
                bump("texa ta0=%02x ta1=%02x aem=%u", s.texa.ta0, s.texa.ta1, s.texa.aem ? 1u : 0u);
                readsDrawn("texture", ctx.tex0.psm, ctx.tex0.tbp0, ctx.tex0.tbw, s.textureHeight);
            }
            float lo = 1e9f, hi = -1e9f;
            for (int i = 0; i < b.vertexCount; ++i)
            {
                lo = std::min(lo, b.vertices[i].y - (ctx.xyoffset.ofy >> 4));
                hi = std::max(hi, b.vertices[i].y - (ctx.xyoffset.ofy >> 4));
            }
            const int y0 = std::max<int>(ctx.scissor.y0, static_cast<int>(lo));
            const int y1 = std::min<int>(ctx.scissor.y1, static_cast<int>(hi) + 1);
            if (y1 < y0)
                break;
            auto markDrawn = [&](uint32_t psm, uint32_t basePage, uint32_t bw)
            {
                uint32_t p0, p1;
                pageRange(psm, basePage * 32u, bw, y0, y1, p0, p1);
                for (uint32_t pg = p0; pg < p1; ++pg)
                {
                    PageOwner &o = owner[pg & 511u];
                    if (o.drawn && (o.psm != psm || o.base != basePage || o.bw != bw))
                        bump("reinterpret draw: psm %02x base %u bw %u -> psm %02x base %u bw %u", o.psm, o.base, o.bw, psm, basePage, bw);
                    o = {true, psm, basePage, bw};
                }
            };
            if (ctx.frame.fbmsk != 0xFFFFFFFFu)
                markDrawn(ctx.frame.psm, ctx.frame.fbp, ctx.frame.fbw);
            if (!ctx.zbuf.zmask && ztst != 0u)
                markDrawn(ctx.zbuf.psm, ctx.zbuf.zbp, ctx.frame.fbw);
            break;
        }
        case RecOp::LoadClut:
        {
            GSTex0Reg t;
            std::memcpy(&t, c.data.data(), sizeof(t));
            bump("clut cld=%u cpsm=%02x csm=%u psm=%02x", t.cld, t.cpsm, t.csm, t.psm);
            if (t.cld != 0u)
                readsDrawn("clut", t.cpsm, t.cbp, 1, 16);
            break;
        }
        case RecOp::BeginTransfer:
        {
            GSTransferCommand t;
            std::memcpy(&t, c.data.data(), sizeof(t));
            const GSBitBltBuf &bb = t.bitbltbuf;
            if (t.direction == 0u)
                bump("transfer host->local dpsm=%02x", bb.dpsm);
            else if (t.direction == 1u)
                bump("transfer local->host spsm=%02x %ux%u", bb.spsm, t.trxreg.rrw, t.trxreg.rrh);
            else if (t.direction == 2u)
                bump("transfer local->local spsm=%02x dpsm=%02x %ux%u", bb.spsm, bb.dpsm, t.trxreg.rrw, t.trxreg.rrh);
            if (t.direction == 1u || t.direction == 2u)
                readsDrawn("transfer src", bb.spsm, bb.sbp, bb.sbw, t.trxpos.ssay + t.trxreg.rrh);
            if (t.direction == 0u || t.direction == 2u)
            {
                uint32_t p0, p1;
                pageRange(bb.dpsm, bb.dbp, bb.dbw, t.trxpos.dsay, t.trxpos.dsay + t.trxreg.rrh - 1, p0, p1);
                for (uint32_t pg = p0; pg < p1; ++pg)
                    if (owner[pg & 511u].drawn)
                    {
                        bump("transfer into drawn page (dir %u dpsm=%02x)", t.direction, bb.dpsm);
                        break;
                    }
            }
            break;
        }
        case RecOp::UploadImage:
            bump("upload chunks");
            break;
        case RecOp::TextureFlush:
            bump("texflush");
            break;
        }
    }
    std::printf("[gsstats] %s: %zu commands, %llu draws\n", path, cmds.size(), static_cast<unsigned long long>(submits));
    for (const auto &kv : counts)
        std::printf("%10llu  %s\n", static_cast<unsigned long long>(kv.second), kv.first.c_str());
    return 0;
}

// --gs-gpu-compare <file> [every=2000] [pngEvery=25] [outDir=.]: replays a recording through the CPU
// reference and the GPU renderer side by side. Every <every> commands it compares each 640-wide PSMCT32
// buffer drawn since the last checkpoint (RGB): PSNR, % of pixels differing at all and by more than 8.
// Every <pngEvery>th checkpoint writes gpucmp_<command>.png (CPU | GPU | difference x4). Then times a
// full pass of each.
int ps2xGsGpuCompare(const char *path, int every, int pngEvery, const char *outDir)
{
    std::vector<uint8_t> initialVram;
    std::vector<ReplayCmd> cmds;
    if (!loadRecording(path, initialVram, cmds))
        return 1;
    ps2xGsGpuHostInit(true);
    const uint32_t size = static_cast<uint32_t>(initialVram.size());
    const int bands = std::max(1, std::min(16, static_cast<int>(std::thread::hardware_concurrency()) / 2));
    every = std::max(every, 1);
    std::vector<uint8_t> vc(initialVram), vg(initialVram), sc, sg;
    {
        std::unique_ptr<GSRasterBackend> cpu = std::make_unique<GSBandedBackend>(bands);
        std::unique_ptr<GSRasterBackend> gpu = ps2xMakeGsGpuBackend();
        cpu->Initialize(vc.data(), size);
        gpu->Initialize(vg.data(), size);
        std::map<uint32_t, uint64_t> drawn;
        double psnrSum = 0.0, psnrMin = 1e9, over8Sum = 0.0;
        int samples = 0, checkpoint = 0;
        for (size_t i = 0; i < cmds.size(); ++i)
        {
            replayCommand(*cpu, cmds[i]);
            replayCommand(*gpu, cmds[i]);
            if (static_cast<RecOp>(cmds[i].op) == RecOp::Submit)
            {
                GSPrimitiveBatch b;
                std::memcpy(&b, cmds[i].data.data(), sizeof(b));
                const GSFrameReg &f = b.state.context.frame;
                if (f.psm == GS_PSM_CT32 && f.fbw == 10u)
                    ++drawn[f.fbp];
            }
            if ((i + 1) % static_cast<size_t>(every) != 0u && i + 1 != cmds.size())
                continue;
            ++checkpoint;
            cpu->SnapshotVram(sc);
            gpu->SnapshotVram(sg);
            uint32_t mainFbp = 0;
            uint64_t mainDraws = 0;
            for (const auto &kv : drawn)
            {
                const uint32_t base = kv.first * 32u;
                double se = 0.0;
                uint64_t diff0 = 0, diff8 = 0;
                for (uint32_t y = 0; y < 448; ++y)
                    for (uint32_t x = 0; x < 640; ++x)
                    {
                        const uint32_t a = GSMem::ReadCT32(sc.data(), base, 10, x, y), g = GSMem::ReadCT32(sg.data(), base, 10, x, y);
                        int maxd = 0;
                        for (int ch = 0; ch < 3; ++ch)
                        {
                            const int d = static_cast<int>((a >> (ch * 8)) & 255u) - static_cast<int>((g >> (ch * 8)) & 255u);
                            se += static_cast<double>(d * d);
                            maxd = std::max(maxd, std::abs(d));
                        }
                        diff0 += maxd > 0 ? 1u : 0u;
                        diff8 += maxd > 8 ? 1u : 0u;
                    }
                const double mse = se / (640.0 * 448.0 * 3.0);
                const double psnr = mse <= 0.0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
                const double n = 640.0 * 448.0;
                std::printf("[gpucmp] cmd %zu fbp %u (%llu draws): PSNR %.2f dB, differ %.2f%%, >8 %.2f%%\n", i + 1, kv.first,
                            static_cast<unsigned long long>(kv.second), psnr, 100.0 * diff0 / n, 100.0 * diff8 / n);
                psnrSum += psnr;
                psnrMin = std::min(psnrMin, psnr);
                over8Sum += 100.0 * diff8 / n;
                ++samples;
                if (kv.second > mainDraws)
                {
                    mainDraws = kv.second;
                    mainFbp = kv.first;
                }
            }
            if (pngEvery > 0 && mainDraws && checkpoint % pngEvery == 0)
            {
                std::vector<uint8_t> px(1920u * 448u * 4u);
                for (uint32_t y = 0; y < 448; ++y)
                    for (uint32_t x = 0; x < 640; ++x)
                    {
                        const uint32_t a = GSMem::ReadCT32(sc.data(), mainFbp * 32u, 10, x, y), g = GSMem::ReadCT32(sg.data(), mainFbp * 32u, 10, x, y);
                        for (int ch = 0; ch < 3; ++ch)
                        {
                            const int ca = static_cast<int>((a >> (ch * 8)) & 255u), cg = static_cast<int>((g >> (ch * 8)) & 255u);
                            px[(y * 1920u + x) * 4u + ch] = static_cast<uint8_t>(ca);
                            px[(y * 1920u + 640u + x) * 4u + ch] = static_cast<uint8_t>(cg);
                            px[(y * 1920u + 1280u + x) * 4u + ch] = static_cast<uint8_t>(std::min(255, std::abs(ca - cg) * 4));
                        }
                        px[(y * 1920u + x) * 4u + 3] = 255;
                        px[(y * 1920u + 640u + x) * 4u + 3] = 255;
                        px[(y * 1920u + 1280u + x) * 4u + 3] = 255;
                    }
                char name[512];
                std::snprintf(name, sizeof(name), "%s/gpucmp_%07zu.png", outDir, i + 1);
                Image img{px.data(), 1920, 448, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
                ExportImage(img, name);
            }
            drawn.clear();
        }
        if (samples)
            std::printf("[gpucmp] %d buffer checks: mean PSNR %.2f dB, worst %.2f dB, mean >8 %.2f%%\n", samples, psnrSum / samples,
                        psnrMin, over8Sum / samples);
    }
    auto timePass = [&](std::unique_ptr<GSRasterBackend> backend, const char *what)
    {
        std::vector<uint8_t> v(initialVram), snap;
        backend->Initialize(v.data(), size);
        const auto t0 = std::chrono::steady_clock::now();
        for (const ReplayCmd &c : cmds)
            replayCommand(*backend, c);
        backend->SnapshotVram(snap);
        std::printf("[gpucmp] %s: %.3f s per pass\n", what, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    };
    timePass(std::make_unique<GSBandedBackend>(bands), "cpu (banded)");
    timePass(ps2xMakeGsGpuBackend(), "gpu");
    timePass(ps2xMakeGsGpuBackend(), "gpu (2nd)");
    return 0;
}
