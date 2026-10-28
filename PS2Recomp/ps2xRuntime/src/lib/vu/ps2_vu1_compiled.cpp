// Statically recompiled VU1 microprograms: the run-time lookup and ps2EntryRunner --vu-gen (code
// images and entry points from a recording; ps2_vu1_gen.cpp writes the programs). See ps2_vu1_compiled.h.
#include "ps2_vu1_compiled.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>

extern void (*g_vu1EntryHook)(uint32_t pc); // ps2_vu1_core.cpp

namespace
{
    constexpr uint32_t kMaxPairs = VU1Core::kMaxDecodedPairs;
}

const std::vector<const VU1CompiledProgram *> &vu1CompiledProgramsAt(uint32_t pairIndex)
{
    static std::once_flag once;
    static std::vector<std::vector<const VU1CompiledProgram *>> byPair;
    std::call_once(once, []
                   {
        byPair.resize(kMaxPairs);
        for (size_t i = 0; i < g_vu1CompiledProgramCount; ++i)
        {
            const VU1CompiledProgram *program = g_vu1CompiledPrograms[i];
            for (uint32_t p = 0; p < program->pairCount; ++p)
                if (program->pairs[p].pairIndex < kMaxPairs)
                    byPair[program->pairs[p].pairIndex].push_back(program);
        } });
    static const std::vector<const VU1CompiledProgram *> none;
    return pairIndex < byPair.size() ? byPair[pairIndex] : none;
}

void vu1MergeCompiledProgram(const VU1CompiledProgram &program, VU1CompiledPairFn *pairs)
{
    for (uint32_t i = 0; i < program.pairCount; ++i)
        if (program.pairs[i].pairIndex < kMaxPairs && !pairs[program.pairs[i].pairIndex])
            pairs[program.pairs[i].pairIndex] = program.pairs[i].fn;
    // A block replaces its first pair only where the table holds this program's own functions for
    // all of its pairs (an earlier program may own some of them): the block then runs exactly the
    // pairs run() would take from the table. Later merges never replace an entry.
    for (uint32_t b = 0; b < program.blockCount; ++b)
    {
        const VU1CompiledBlock &block = program.blocks[b];
        bool own = block.firstPos + block.count <= program.pairCount;
        for (uint32_t j = 0; own && j < block.count; ++j)
        {
            const VU1CompiledPair &pair = program.pairs[block.firstPos + j];
            own = pair.pairIndex < kMaxPairs && pairs[pair.pairIndex] == pair.fn;
        }
        if (own)
            pairs[program.pairs[block.firstPos].pairIndex] = block.fn;
    }
}

void vu1CompiledProgramReach(const VU1CompiledProgram &program, uint64_t *bits)
{
    std::fill(bits, bits + kMaxPairs / 64u, 0ull);
    for (uint32_t i = 0; i < program.pairCount; ++i)
        if (program.pairs[i].pairIndex < kMaxPairs)
            bits[program.pairs[i].pairIndex / 64u] |= 1ull << (program.pairs[i].pairIndex % 64u);
}

bool vu1CompiledProgramMatches(const VU1CompiledProgram &program, const uint8_t *code, uint32_t codeSize)
{
    return vu1ProgramSignature(code, codeSize, program.startPc, nullptr) == program.signature;
}

// ---------------------------------------------------------------------------------------------
// Generator: ps2EntryRunner --vu-gen <recording.vur> <outDir>
// ---------------------------------------------------------------------------------------------
namespace
{
    struct RecordedRun
    {
        uint32_t codeIndex = 0;
        uint32_t header[6]{}; // startPC, top, itop, maxCycles, data size, 0
        VU1State state{};
        std::vector<uint8_t> data;
    };

    bool loadRecording(const char *path, std::vector<std::vector<uint8_t>> &codes, std::vector<RecordedRun> &runs)
    {
        std::FILE *f = std::fopen(path, "rb");
        if (!f)
            return false;
        char magic[4]{};
        if (std::fread(magic, 1, 4, f) != 4 || std::memcmp(magic, "VUR3", 4) != 0)
        {
            std::fclose(f);
            return false;
        }
        for (;;)
        {
            const int tag = std::fgetc(f);
            if (tag == EOF)
                break;
            if (tag == 'C')
            {
                uint32_t size = 0;
                if (std::fread(&size, sizeof(size), 1, f) != 1)
                    break;
                codes.emplace_back(size);
                if (std::fread(codes.back().data(), 1, size, f) != size)
                    break;
            }
            else if (tag == 'R')
            {
                RecordedRun run;
                uint64_t parts[5]{};
                run.codeIndex = codes.empty() ? 0u : static_cast<uint32_t>(codes.size() - 1u);
                if (std::fread(run.header, sizeof(run.header), 1, f) != 1 || std::fread(&run.state, sizeof(run.state), 1, f) != 1)
                    break;
                run.data.resize(run.header[4]);
                if (std::fread(run.data.data(), 1, run.data.size(), f) != run.data.size() || std::fread(parts, sizeof(parts), 1, f) != 1)
                    break;
                if (!codes.empty())
                    runs.push_back(std::move(run));
            }
            else
                break;
        }
        std::fclose(f);
        return !codes.empty();
    }

    // Runs the recording through the interpreter and collects every (code image, pc) where a run
    // starts or jumps in (JR/JALR targets) without a compiled program in this binary.
    std::vector<std::vector<uint32_t>> *g_entriesOut = nullptr;
    uint32_t g_entriesCode = 0;
    void noteEntry(uint32_t pc)
    {
        if (g_entriesOut)
            (*g_entriesOut)[g_entriesCode].push_back(pc);
    }

    std::vector<std::pair<uint32_t, uint32_t>> collectEntryPoints(const std::vector<std::vector<uint8_t>> &codes, const std::vector<RecordedRun> &runs)
    {
        std::vector<std::pair<uint32_t, uint32_t>> starts;
        for (const RecordedRun &run : runs)
            starts.push_back({run.codeIndex, run.header[0]});
        auto memory = std::make_unique<PS2Memory>();
        if (!memory->initialize())
            return starts;
        memory->setGifPacketCallback([](const uint8_t *, uint32_t) {});
        GS gs;
        VU1Interpreter vu(VU1Interpreter::Unit::VU1);
        uint8_t *code = memory->getVU1Code();
        uint8_t *data = memory->getVU1Data();
        std::vector<std::vector<uint32_t>> entries(codes.size());
        g_entriesOut = &entries;
        g_vu1EntryHook = noteEntry;
        int64_t loadedCode = -1;
        for (const RecordedRun &run : runs)
        {
            if (static_cast<int64_t>(run.codeIndex) != loadedCode)
            {
                const std::vector<uint8_t> &c = codes[run.codeIndex];
                std::memcpy(code, c.data(), std::min<size_t>(c.size(), PS2_VU1_CODE_SIZE));
                memory->markVU1CodeModified();
                loadedCode = run.codeIndex;
            }
            std::memcpy(data, run.data.data(), std::min<size_t>(run.data.size(), PS2_VU1_DATA_SIZE));
            const uint64_t cycles = vu.state().cycles;
            vu.state() = run.state;
            vu.state().cycles = cycles;
            g_entriesCode = run.codeIndex;
            vu.execute(code, PS2_VU1_CODE_SIZE, data, PS2_VU1_DATA_SIZE, gs, memory.get(),
                       run.header[0], run.header[1], run.header[2], run.header[3]);
        }
        g_vu1EntryHook = nullptr;
        g_entriesOut = nullptr;
        size_t jumpIns = 0;
        for (uint32_t c = 0; c < entries.size(); ++c)
            for (uint32_t pc : entries[c])
            {
                starts.push_back({c, pc});
                ++jumpIns;
            }
        std::printf("[vugen] %zu runs replayed, %zu entries without a compiled program\n", runs.size(), jumpIns);
        // Also the start of every compiled program in this binary that matches a recorded code image:
        // its pairs can run here although no run starts at its pc (programs overlap), so deleting an
        // old program's files and regenerating rebuilds it with the current analysis (elisions).
        size_t registered = 0;
        for (uint32_t c = 0; c < codes.size(); ++c)
            for (size_t p = 0; p < g_vu1CompiledProgramCount; ++p)
                if (const VU1CompiledProgram *program = g_vu1CompiledPrograms[p];
                    program && vu1CompiledProgramMatches(*program, codes[c].data(), static_cast<uint32_t>(codes[c].size())))
                {
                    starts.push_back({c, program->startPc});
                    ++registered;
                }
        std::printf("[vugen] %zu matches of compiled programs in this binary added as start points\n", registered);
        return starts;
    }
}

int ps2xVuGenerate(const char *recordingPath, const char *outDir)
{
    std::vector<std::vector<uint8_t>> codes;
    std::vector<RecordedRun> runs;
    if (!loadRecording(recordingPath, codes, runs))
    {
        std::fprintf(stderr, "[vugen] cannot read VU1 recording %s\n", recordingPath);
        return 1;
    }
    const std::vector<std::string> sources(codes.size(), std::filesystem::path(recordingPath).filename().string());
    return vu1GeneratePrograms(codes, collectEntryPoints(codes, runs), sources, outDir);
}
