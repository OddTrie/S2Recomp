// Statically recompiled VU1 microprograms.
//
// `ps2EntryRunner --vu-gen <recording.vur> <dir>` turns every VU1 program seen in a PS2X_VU_RECORD
// recording into C++ (vu1gen/vu1p_<signature>.cpp): one function per instruction pair, decoded at
// compile time by the interpreter's own constexpr decode and run by the interpreter's own pair step
// (VU1Core::stepPair, force-inlined with the constant instruction words, so each pair keeps only
// its own opcode cases and operand fields). Results, flags and cycle timing are therefore the
// interpreter's, by construction; `--vu-replay` checks it. Players' builds write the same files with
// ps2xVuGen from vu1_locations.toml (next to sims2_config.toml), which says where the code of each
// program lies in the game's own files (ps2xRuntime/tools/vu1_locations.py writes it from vu1gen/).
//
// A program is identified by its start pc and a signature over every pair reachable from it
// (vu1ProgramSignature). At run time a program start looks up the signature of the code currently
// in VU1 micro memory; a match runs the compiled pairs, anything else (unknown program, a pair
// that was not compiled, a jump outside the program) continues in the interpreter on the same state.
#pragma once

#include "ps2_vu1_core.h"
#include "ps2_vu1_upper.inl"
#include "ps2_vu1_lower.inl"
#include "ps2_vu1_step.inl"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using VU1CompiledPairFn = VU1Core::CompiledPairFn;

struct VU1CompiledPair
{
    uint32_t pairIndex;
    VU1CompiledPairFn fn;
};

// A straight run of a program's pairs (pairs[firstPos .. firstPos + count - 1], consecutive pair
// indices) fused into one function: no return to run() between them (VU1_COMPILED_BLOCK_*).
struct VU1CompiledBlock
{
    uint32_t firstPos;
    uint32_t count;
    VU1CompiledPairFn fn;
};

struct VU1CompiledProgram
{
    uint64_t signature;
    uint32_t startPc;
    uint32_t pairCount;
    const VU1CompiledPair *pairs;
    uint32_t blockCount = 0;
    const VU1CompiledBlock *blocks = nullptr;
};

// vu1gen/vu1_registry.cpp (generated).
extern const VU1CompiledProgram *const g_vu1CompiledPrograms[];
extern const size_t g_vu1CompiledProgramCount;

// Signature of the VU1 program starting at startPc: FNV-1a over the start pc and the index and
// both instruction words of every pair reachable from it (branch targets, fall-through, delay
// slots, E-bit end; JR/JALR continue at the return address of every BAL/JALR). reachable (optional)
// receives the pair indices in ascending order.
uint64_t vu1ProgramSignature(const uint8_t *code, uint32_t codeSize, uint32_t startPc, std::vector<uint32_t> *reachable);

// Every registered program that compiles the pair at pairIndex.
const std::vector<const VU1CompiledProgram *> &vu1CompiledProgramsAt(uint32_t pairIndex);
// Adds the program's pair functions to a dense table (kMaxDecodedPairs entries).
void vu1MergeCompiledProgram(const VU1CompiledProgram &program, VU1CompiledPairFn *pairs);
// The program's compiled pairs as a bitset (kMaxDecodedPairs / 64 words, bit i = pair i).
void vu1CompiledProgramReach(const VU1CompiledProgram &program, uint64_t *bits);
// Whether the code holds exactly that program (its signature from its start pc matches).
bool vu1CompiledProgramMatches(const VU1CompiledProgram &program, const uint8_t *code, uint32_t codeSize);

// Generator (ps2_vu1_gen.cpp): writes vu1p_<signature>.cpp for the program at every (code image,
// start pc) of starts not generated yet in outDir, then vu1_registry.cpp. sources[i] names where code
// image i came from (in the files' first line).
int vu1GeneratePrograms(const std::vector<std::vector<uint8_t>> &codes, std::vector<std::pair<uint32_t, uint32_t>> starts,
                        const std::vector<std::string> &sources, const char *outDir);
// ps2xVuGen <locations.toml> <extracted dir> <outDir>: the programs a locations file names (file
// offsets of their code in the game's files, no instruction words), read from the player's own files.
int ps2xVuGenerateDisc(const char *locationsPath, const char *extractedDir, const char *outDir);

// One generated pair. The decoded pair is a compile-time constant; flag liveness depends on the
// whole code image, so it comes from the interpreter's decode cache (valid: compiled programs only
// run on tracked VU1 code, whose cache run() has brought up to date).
// body_N is the pair's step, pair_N the function in the program's pair table, info_N its description
// for blocks (VU1Core::runBlock).
#define VU1_COMPILED_PAIR(index, lowerWord, upperWord)                                                  \
    using info_##index = vu1detail::PairInfo<index, lowerWord, upperWord, vu1UpperHandler(upperWord), 0u>; \
    VU1_FORCEINLINE bool body_##index(VU1Core &core, VU1Core::StepArgs &args)                          \
    {                                                                                                  \
        using Src = vu1detail::StaticPair<lowerWord, upperWord>;                                       \
        return core.stepPair<true, vu1UpperHandler(upperWord), 0u, Src>(Src::d, core.m_decodedCodeCache[index].flagsLive, args); \
    }                                                                                                  \
    bool pair_##index(VU1Core &core, VU1Core::StepArgs &args) { return body_##index(core, args); }

// The same with the bookkeeping the generator proved unnecessary left out (VU1Core::kElide*):
// elide = kElide bits, vfKeep/viKeep/accKeep = the interlock reads still checked (bit i = readyVf[i]
// / readyVi[i], ACC components).
#define VU1_COMPILED_PAIR_S(index, lowerWord, upperWord, elide, vfKeep, viKeep, accKeep)               \
    using info_##index = vu1detail::PairInfo<index, lowerWord, upperWord, vu1UpperHandler(upperWord), elide, vfKeep, viKeep, accKeep>; \
    VU1_FORCEINLINE bool body_##index(VU1Core &core, VU1Core::StepArgs &args)                          \
    {                                                                                                  \
        using Src = vu1detail::StaticPair<lowerWord, upperWord, vfKeep, viKeep, accKeep>;              \
        return core.stepCompiledPair<vu1UpperHandler(upperWord), elide, Src>(Src::d, index, args);    \
    }                                                                                                  \
    bool pair_##index(VU1Core &core, VU1Core::StepArgs &args) { return body_##index(core, args); }

// A block of consecutive pairs (VU1CompiledBlock): BEGIN(first) NEXT(first + 1) ... END, run by
// VU1Core::runBlock: the very pairs run() would take, returning to run() when the next pair would not
// run as the straight continuation. With clang the block then makes run()'s next loop iteration
// itself (VU1Core::chainNext) and tail-calls the next compiled function: one indirect jump per block
// exit instead of a return to run()'s single call site, and no stack growth (musttail).
#if defined(__clang__)
#define VU1_COMPILED_CHAIN(core, args)                                                                 \
    if (VU1CompiledPairFn next = core.chainNext(args))                                                 \
        [[clang::musttail]] return next(core, args);
#else
#define VU1_COMPILED_CHAIN(core, args)
#endif
#define VU1_COMPILED_BLOCK_BEGIN(first) VU1_COMPILED_BLOCK_BEGIN_N(first, 0u, 0u, 0u)
// BEGIN_N: a block entered with lanes lo/hi known normalized (vu1detail::NormLanes) on every path of
// the program `trust` (its signature) into it; BEGIN_Z: one entered with nothing known (see runBlock).
#define VU1_COMPILED_BLOCK_BEGIN_Z(first, trust) VU1_COMPILED_BLOCK_BEGIN_N(first, trust, 0u, 0u)
#define VU1_COMPILED_BLOCK_BEGIN_N(first, trust, lo, hi)                                               \
    bool block_##first(VU1Core &core, VU1Core::StepArgs &args)                                         \
    {                                                                                                  \
        if (!core.runBlock<trust, lo, hi, info_##first
#define VU1_COMPILED_BLOCK_NEXT(index) , info_##index
#define VU1_COMPILED_BLOCK_END()                                                                       \
        >(args))                                                                                       \
            return false;                                                                              \
        VU1_COMPILED_CHAIN(core, args)                                                                 \
        return true;                                                                                   \
    }
#define VU1_COMPILED_BLOCK_DECL(first) bool block_##first(VU1Core &core, VU1Core::StepArgs &args);

// Declaration of a generated pair (the program file's pair table; the pairs live in chunk files).
#define VU1_COMPILED_PAIR_DECL(index) bool pair_##index(VU1Core &core, VU1Core::StepArgs &args);
