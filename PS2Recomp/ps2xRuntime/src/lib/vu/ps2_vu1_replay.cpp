// Offline VU1 replay: ps2EntryRunner --vu-replay <file> [repeat]
// Re-runs every VU1 program run recorded with PS2X_VU_RECORD through the interpreter, checks that
// each produces the recorded result hash (registers, data memory, XGKICK packets, cycle count) and
// reports the time per run and per instruction pair. Used to check that interpreter optimisations
// keep results bit-identical on real game workloads.
#include "runtime/ps2_vu1.h"
#include "runtime/ps2_memory.h"
#include "runtime/gs/gs_frontend.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

uint64_t ps2xVuHashBytes(uint64_t h, const void *data, size_t size);
void ps2xVuHashRunParts(const VU1State &s, const uint8_t *vuData, uint32_t dataSize, uint64_t kickHash, uint64_t cycles, uint64_t out[5]);
uint64_t ps2xVuPairsExecuted();
uint64_t ps2xVuCompiledPairsExecuted();

namespace
{
    struct RecordedRun
    {
        uint32_t codeIndex = 0;
        uint32_t header[6]{};
        VU1State state{};
        std::vector<uint8_t> data;
        uint64_t parts[5]{}; // registers (except status), data memory, XGKICK packets, cycles, status
    };

    template <class T>
    bool readValue(std::FILE *f, T &out)
    {
        return std::fread(&out, sizeof(T), 1, f) == 1;
    }
}

int ps2xVuReplay(const char *path, int repeat)
{
    std::FILE *f = std::fopen(path, "rb");
    if (!f)
    {
        std::fprintf(stderr, "[vureplay] cannot open %s\n", path);
        return 1;
    }
    char magic[4]{};
    if (std::fread(magic, 1, 4, f) != 4 || std::memcmp(magic, "VUR3", 4) != 0)
    {
        std::fprintf(stderr, "[vureplay] %s is not a VU1 recording\n", path);
        std::fclose(f);
        return 1;
    }

    std::vector<std::vector<uint8_t>> codes;
    std::vector<RecordedRun> runs;
    for (;;)
    {
        const int tag = std::fgetc(f);
        if (tag == EOF)
            break;
        if (tag == 'C')
        {
            uint32_t size = 0;
            if (!readValue(f, size))
                break;
            codes.emplace_back(size);
            if (std::fread(codes.back().data(), 1, size, f) != size)
                break;
        }
        else if (tag == 'R')
        {
            RecordedRun run;
            run.codeIndex = codes.empty() ? 0u : static_cast<uint32_t>(codes.size() - 1u);
            if (std::fread(run.header, sizeof(run.header), 1, f) != 1 || !readValue(f, run.state))
                break;
            run.data.resize(run.header[4]);
            if (std::fread(run.data.data(), 1, run.data.size(), f) != run.data.size() || std::fread(run.parts, sizeof(run.parts), 1, f) != 1)
                break; // last run of an unfinished recording
            runs.push_back(std::move(run));
        }
        else
        {
            std::fprintf(stderr, "[vureplay] bad record tag %d\n", tag);
            break;
        }
    }
    std::fclose(f);
    if (runs.empty() || codes.empty())
    {
        std::fprintf(stderr, "[vureplay] no runs in %s\n", path);
        return 1;
    }

    auto memory = std::make_unique<PS2Memory>();
    if (!memory->initialize())
    {
        std::fprintf(stderr, "[vureplay] PS2Memory init failed\n");
        return 1;
    }
    uint64_t kickHash = 0;
    memory->setGifPacketCallback([&kickHash](const uint8_t *data, uint32_t size)
                                 {
        kickHash = ps2xVuHashBytes(kickHash, &size, sizeof(size));
        kickHash = ps2xVuHashBytes(kickHash, data, size); });
    GS gs;
    VU1Interpreter vu(VU1Interpreter::Unit::VU1);

    uint8_t *code = memory->getVU1Code();
    uint8_t *data = memory->getVU1Data();
    // Per code image: does it contain FSEQ/FSAND/FSOR (lower opcodes 0x14/0x16/0x17)?
    std::vector<bool> codeReadsStatus;
    for (const std::vector<uint8_t> &c : codes)
    {
        bool reads = false;
        for (size_t at = 0; at + 8 <= c.size() && !reads; at += 8)
        {
            uint32_t lower = 0, upper = 0;
            std::memcpy(&lower, c.data() + at, 4);
            std::memcpy(&upper, c.data() + at + 4, 4);
            const uint32_t op = lower >> 25;
            reads = (upper & 0x80000000u) == 0u && (op == 0x14u || op == 0x16u || op == 0x17u);
        }
        codeReadsStatus.push_back(reads);
    }
    uint32_t unobservableStatusDiffs = 0;
    uint32_t mismatches = 0;
    double seconds = 0.0;
    const uint64_t pairsBefore = ps2xVuPairsExecuted();
    const uint64_t compiledBefore = ps2xVuCompiledPairsExecuted();
    for (int pass = 0; pass < repeat; ++pass)
    {
        int64_t loadedCode = -1;
        for (size_t index = 0; index < runs.size(); ++index)
        {
            const RecordedRun &run = runs[index];
            if (static_cast<int64_t>(run.codeIndex) != loadedCode)
            {
                const std::vector<uint8_t> &c = codes[run.codeIndex];
                std::memcpy(code, c.data(), std::min<size_t>(c.size(), PS2_VU1_CODE_SIZE));
                memory->markVU1CodeModified();
                loadedCode = run.codeIndex;
            }
            std::memcpy(data, run.data.data(), std::min<size_t>(run.data.size(), PS2_VU1_DATA_SIZE));
            // state().cycles mirrors the interpreter's own cycle counter after a run; the recorded
            // value is the game's counter, so measure the run against the interpreter's.
            const uint64_t cyclesBefore = vu.state().cycles;
            vu.state() = run.state;
            vu.state().cycles = cyclesBefore;
            kickHash = 1469598103934665603ull;
            const auto t0 = std::chrono::steady_clock::now();
            vu.execute(code, PS2_VU1_CODE_SIZE, data, PS2_VU1_DATA_SIZE, gs, memory.get(),
                       run.header[0], run.header[1], run.header[2], run.header[3]);
            seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            uint64_t parts[5];
            ps2xVuHashRunParts(vu.state(), data, PS2_VU1_DATA_SIZE, kickHash, vu.state().cycles - cyclesBefore, parts);
            if (pass != 0)
                continue;
            const bool observableMismatch = std::memcmp(parts, run.parts, 4 * sizeof(uint64_t)) != 0;
            const bool statusMismatch = parts[4] != run.parts[4];
            // Status (and its sticky bits) can only be read by FSEQ/FSAND/FSOR; without them in the
            // loaded code a status-only difference is invisible to the game.
            if (statusMismatch && !observableMismatch && !codeReadsStatus[run.codeIndex])
            {
                ++unobservableStatusDiffs;
                continue;
            }
            if (observableMismatch || statusMismatch)
            {
                static const char *names[5] = {"registers", "data", "xgkick", "cycles", "status"};
                std::string what;
                for (int k = 0; k < 5; ++k)
                    if (parts[k] != run.parts[k])
                        what += std::string(what.empty() ? "" : "+") + names[k];
                if (++mismatches <= 10u)
                    std::fprintf(stderr, "[vureplay] MISMATCH run %zu (start pc=0x%x): %s (cycles %llu, recorded %llu)\n", index, run.header[0], what.c_str(),
                                 static_cast<unsigned long long>(parts[3]), static_cast<unsigned long long>(run.parts[3]));
            }
        }
    }
    const uint64_t pairs = ps2xVuPairsExecuted() - pairsBefore;
    const double runCount = static_cast<double>(runs.size()) * repeat;
    std::printf("[vureplay] %zu runs x %d passes, %zu code images: %u mismatches (%u status-only differences in code without FS* reads)\n",
                runs.size(), repeat, codes.size(), mismatches, unobservableStatusDiffs);
    const uint64_t compiledPairs = ps2xVuCompiledPairsExecuted() - compiledBefore;
    std::printf("[vureplay] %.3f s total, %.1f us/run, %.1f ns/pair (%llu pairs, %.1f%% in compiled VU1 programs)\n", seconds,
                seconds * 1e6 / runCount, pairs ? seconds * 1e9 / static_cast<double>(pairs) : 0.0,
                static_cast<unsigned long long>(pairs), pairs ? 100.0 * static_cast<double>(compiledPairs) / static_cast<double>(pairs) : 0.0);

    return mismatches == 0u ? 0 : 2;
}

// ps2EntryRunner --vu-run <vu1_run_*.bin>: run one VU1 run dumped by PS2X_VU_DUMP / PS2X_VU_FIND_TBP and print
// the XYZ2/XYZF2 and ST values of the GIF packets it kicks (one vertex per line), to compare against its inputs.
int ps2xVuRunDump(const char *path)
{
    std::FILE *f = std::fopen(path, "rb");
    if (!f)
        return 1;
    uint32_t header[6]{};
    std::vector<uint8_t> codeImage, dataImage;
    VU1State state{};
    bool ok = std::fread(header, sizeof(header), 1, f) == 1;
    if (ok)
    {
        codeImage.resize(header[0]);
        dataImage.resize(header[1]);
        ok = std::fread(codeImage.data(), 1, codeImage.size(), f) == codeImage.size() &&
             std::fread(dataImage.data(), 1, dataImage.size(), f) == dataImage.size() && readValue(f, state);
    }
    std::fclose(f);
    if (!ok)
    {
        std::fprintf(stderr, "[vurun] cannot read %s\n", path);
        return 1;
    }
    auto memory = std::make_unique<PS2Memory>();
    if (!memory->initialize())
        return 1;
    memory->setGifPacketCallback([](const uint8_t *p, uint32_t n)
                                 {
        uint32_t off = 0;
        float s = 0.0f, t = 0.0f, q = 1.0f;
        while (off + 16 <= n)
        {
            uint64_t tag, regs;
            std::memcpy(&tag, p + off, 8);
            std::memcpy(&regs, p + off + 8, 8);
            off += 16;
            const uint32_t nloop = tag & 0x7fffu, flg = (tag >> 58) & 3u;
            uint32_t nreg = (tag >> 60) & 15u;
            if (!nreg)
                nreg = 16;
            if (flg != 0u)
            {
                off += flg == 1u ? ((nloop * nreg + 1u) & ~1u) * 8u : nloop * 16u;
            }
            else
            {
                for (uint32_t i = 0; i < nloop; ++i)
                    for (uint32_t r = 0; r < nreg && off + 16 <= n; ++r, off += 16)
                    {
                        const uint32_t reg = (regs >> (4 * r)) & 15u;
                        uint32_t w[4];
                        std::memcpy(w, p + off, 16);
                        if (reg == 2) // ST: s, t, q as floats
                        {
                            std::memcpy(&s, &w[0], 4);
                            std::memcpy(&t, &w[1], 4);
                            std::memcpy(&q, &w[2], 4);
                        }
                        else if (reg == 4 || reg == 5) // XYZF2 / XYZ2, packed: 12.4 fixed point
                            std::printf("[vurun] xy=(%.2f,%.2f) z=%u adc=%u st/q=(%.4f,%.4f)\n", (w[0] & 0xffffu) / 16.0, (w[1] & 0xffffu) / 16.0,
                                        reg == 4 ? (w[2] >> 4) & 0xffffffu : w[2], (w[3] >> 15) & 1u, q != 0.0f ? s / q : 0.0, q != 0.0f ? t / q : 0.0);
                    }
            }
            if ((tag >> 15) & 1u)
                break;
        } });
    GS gs;
    VU1Interpreter vu(VU1Interpreter::Unit::VU1);
    std::memcpy(memory->getVU1Code(), codeImage.data(), std::min<size_t>(codeImage.size(), PS2_VU1_CODE_SIZE));
    memory->markVU1CodeModified();
    std::memcpy(memory->getVU1Data(), dataImage.data(), std::min<size_t>(dataImage.size(), PS2_VU1_DATA_SIZE));
    vu.state() = state;
    std::printf("[vurun] start pc=0x%x top=%u itop=%u\n", header[2], header[3], header[4]);
    vu.execute(memory->getVU1Code(), PS2_VU1_CODE_SIZE, memory->getVU1Data(), PS2_VU1_DATA_SIZE, gs, memory.get(), header[2], header[3], header[4], header[5]);
    // PS2X_VU_RUN_QWORDS=<n>: also print data qwords 0..n-1 that the run changed (as floats).
    if (const char *e = std::getenv("PS2X_VU_RUN_QWORDS"))
        for (uint32_t q = 0, n = static_cast<uint32_t>(std::atoi(e)); q < n && q * 16u + 16u <= dataImage.size(); ++q)
            if (std::memcmp(memory->getVU1Data() + q * 16u, dataImage.data() + q * 16u, 16) != 0)
            {
                float v[4], o[4];
                std::memcpy(v, memory->getVU1Data() + q * 16u, 16);
                std::memcpy(o, dataImage.data() + q * 16u, 16);
                std::printf("[vurun] q%u: %g %g %g %g (was %g %g %g %g)\n", q, v[0], v[1], v[2], v[3], o[0], o[1], o[2], o[3]);
            }
    return 0;
}
