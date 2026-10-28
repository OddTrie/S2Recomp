// One VU instruction pair: the body of the interpreter loop, force-inlined so generated VU1 programs
// (vu1gen/) run exactly the interpreter's steps with the pair decoded at compile time. Included by
// ps2_vu1_core.cpp and ps2_vu1_compiled.h.
#pragma once
#include "ps2_vu1_core.h"

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <tuple>
#include <utility>

extern uint64_t g_vuPairs;      // pairs executed (PS2X_VU_STATS, the replay's ns/pair)
extern uint64_t g_vuSteps;      // VU cycles stepped one by one (PS2X_VU_STATS)
extern int g_vuTraceRunGate;    // PS2X_VU_TRACE run number, 0 = off
extern bool g_vuQueuedWrites;   // PS2X_VU_QUEUED_WRITES=1: delayed register writes through the queues
extern uint64_t g_vuCompiledPairs; // of g_vuPairs, run by generated VU1 programs

namespace vu1detail
{
    // A generated pair's decoded pair (Src of stepPair): the interpreter's own constexpr decode of its
    // two words, minus the interlock reads the generator proved ready (filterReady; all kept by
    // default). Every value stepPair reads from it is a compile-time constant (VU1_PAIR).
    template <uint32_t kLower, uint32_t kUpper, uint32_t kVfKeep = 0xFFFFu, uint32_t kViKeep = 0xFFu, uint32_t kAccKeep = 0xFu>
    struct StaticPair
    {
        static constexpr bool kStatic = true;
        static constexpr VU1Core::DecodedInstructionPair d =
            VU1Core::filterReady(VU1Core::decodePairWords(kLower, kUpper, VU1Core::Unit::VU1), kVfKeep, kViKeep, kAccKeep);
    };

    // f(decoded): folded to a constant for a StaticPair (MSVC does not fold loads from the
    // constexpr pair through a reference by itself, and leaves the loops over its lists in place),
    // read at run time for the interpreter.
    template <class Src, class F>
    VU1_FORCEINLINE auto pairValue(const VU1Core::DecodedInstructionPair &decoded, F f)
    {
        if constexpr (Src::kStatic)
        {
            constexpr auto value = f(Src::d);
            return value;
        }
        else
            return f(decoded);
    }

    template <auto kValue>
    inline constexpr auto kConst = kValue;

    // One pair of a generated block (VU1Core::runBlock): its pair index, decoded pair, upper handler
    // and the elisions of its VU1_COMPILED_PAIR(_S) (plain pairs: none, every read kept).
    template <uint32_t kIndex_, uint32_t kLower, uint32_t kUpper, int kUpperHandler_, uint32_t kElide_,
              uint32_t kVfKeep = 0xFFFFu, uint32_t kViKeep = 0xFFu, uint32_t kAccKeep = 0xFu>
    struct PairInfo
    {
        static constexpr uint32_t kIndex = kIndex_;
        static constexpr int kUpperHandler = kUpperHandler_;
        static constexpr uint32_t kElide = kElide_;
        using Src = StaticPair<kLower, kUpper, kVfKeep, kViKeep, kAccKeep>;
    };

    // Lanes known to hold normalized values (normalizeLanes4 keeps them) inside a generated block,
    // tracked at compile time by runBlockFrom: bit reg * 4 + component (x = 0 ... w = 3) for VF1..VF31,
    // reg 0 stands for ACC. VF0 is the constant (0, 0, 0, 1): run() makes sure of it before it uses
    // generated code. Nothing is known at block entry; upper writes of normalized results set lanes
    // (upperNormInfo), every other VF / ACC write clears them.
    struct NormLanes
    {
        uint64_t lo = 0u, hi = 0u;
    };
    constexpr bool normLaneKnown(NormLanes k, uint32_t bit)
    {
        return ((bit < 64u ? k.lo >> bit : k.hi >> (bit - 64u)) & 1u) != 0u;
    }
    constexpr NormLanes normLanesSet(NormLanes k, uint32_t reg, uint8_t lanes, bool known)
    {
        for (uint32_t c = 0; c < 4u; ++c)
        {
            if ((lanes & laneForComponent(c)) == 0u)
                continue;
            const uint32_t bit = reg * 4u + c;
            uint64_t &word = bit < 64u ? k.lo : k.hi;
            const uint64_t mask = 1ull << (bit & 63u);
            word = known ? (word | mask) : (word & ~mask);
        }
        return k;
    }
    // Two bits (kNormShift*): the lanes the instruction depends on are known, all four are.
    constexpr uint32_t normOperandBits(NormLanes k, uint32_t slot, uint8_t lanes)
    {
        bool needed = true, all = true;
        for (uint32_t c = 0; c < 4u; ++c)
        {
            const bool known = normLaneKnown(k, slot * 4u + c);
            all = all && known;
            if ((lanes & laneForComponent(c)) != 0u)
                needed = needed && known;
        }
        return (needed ? 1u : 0u) | (all ? 2u : 0u);
    }
    // VU1_UPPER_OPERANDS' kNormBits for a pair run with lanes k known.
    constexpr uint32_t normPairBits(NormLanes k, const VU1Core::DecodedInstructionPair &d)
    {
        const UpperNormInfo u = upperNormInfo(d.upper);
        const uint32_t fs = FS(d.upper), ft = FT(d.upper);
        const uint32_t fsBits = fs == 0u ? 3u : normOperandBits(k, fs, u.fsLanes);
        const uint32_t ftBits = ft == 0u ? 3u : normOperandBits(k, ft, u.ftLanes);
        return (fsBits << kNormShiftFs) | (ftBits << kNormShiftFt) | (normOperandBits(k, 0u, u.accLanes) << kNormShiftAcc);
    }
    // The lanes known after a pair (its lower write lands after the upper one, so it is cleared last).
    constexpr NormLanes normAfterPair(NormLanes k, const VU1Core::DecodedInstructionPair &d)
    {
        const UpperNormInfo u = upperNormInfo(d.upper);
        if (d.upperUsage.vfWrite.reg != 0u)
            k = normLanesSet(k, d.upperUsage.vfWrite.reg, d.upperUsage.vfWrite.lanes, u.normalizedOutput);
        if (d.upperUsage.accWrite != 0u)
            k = normLanesSet(k, 0u, d.upperUsage.accWrite, u.normalizedOutput);
        if (d.lowerUsage.vfWrite.reg != 0u)
            k = normLanesSet(k, d.lowerUsage.vfWrite.reg, d.lowerUsage.vfWrite.lanes, false);
        return k;
    }

    // Whether the first of Infos (the next pair of a block) has a lower op that reads m_state.pc:
    // branches (target, BAL/JALR link) and XGKICK (its PS2X_VIF_TRACE line).
    template <class... Infos>
    constexpr bool nextPairReadsPc()
    {
        if constexpr (sizeof...(Infos) == 0u)
            return false;
        else
        {
            using Next = std::tuple_element_t<0, std::tuple<Infos...>>;
            constexpr auto &d = Next::Src::d;
            return !d.iBit && (d.lowerUsage.pipeline == VU1Core::PipelineBranch || d.lowerUsage.pipeline == VU1Core::PipelineXgkick);
        }
    }

    // VU1Core::normalizeOperand on raw float bits (denormal -> signed zero, Inf/NaN -> +/-max).
    constexpr uint32_t normalizedBits(uint32_t bits)
    {
        const uint32_t exponent = (bits >> 23) & 0xFFu;
        if (exponent == 0u)
            return bits & 0x80000000u;
        if (exponent == 0xFFu)
            return (bits & 0x80000000u) | 0x7F7FFFFFu;
        return bits;
    }

    // applyFmacDestFast's kFm: fmacNorm | fmacZero << 8 of a StaticPair, -1 (from the members) otherwise.
    // kNormBits (generated blocks): normPairBits, in bits 16+.
    template <class Src, uint32_t kNormBits = 0u>
    constexpr int fmacContext()
    {
        if constexpr (Src::kStatic)
            return static_cast<int>(Src::d.fmacNorm) | (static_cast<int>(Src::d.fmacZero) << 8) | static_cast<int>(kNormBits << 16);
        else
            return -1;
    }

    // row[c] = value for every component c whose VU lane bit (8 >> c) is in lanes.
    VU1_FORCEINLINE void setLanes(uint64_t *row, uint8_t lanes, uint64_t value)
    {
        if ((lanes & 8u) != 0u)
            row[0] = value;
        if ((lanes & 4u) != 0u)
            row[1] = value;
        if ((lanes & 2u) != 0u)
            row[2] = value;
        if ((lanes & 1u) != 0u)
            row[3] = value;
    }

    // dst = written lanes (VU lane bits: x = 8 ... w = 1) from dst, the rest from old: the same as
    // restoring old and copying the written lanes back. One 16-byte store, like applyDest (every VF /
    // ACC write is a whole 16-byte store, so the 16-byte loads of registers always forward).
    VU1_FORCEINLINE void keepWrittenLanes(float *dst, const float *old, uint8_t lanes)
    {
        if (lanes == 0xFu)
            return;
        _mm_storeu_ps(dst, _mm_blendv_ps(_mm_loadu_ps(old), _mm_loadu_ps(dst), destLaneMask(lanes)));
    }
}

// A value of the decoded pair inside stepPair and its helpers (`d` = the pair): a compile-time
// constant in generated pairs (Src = StaticPair), a load in the interpreter.
#define VU1_PAIR(expr) vu1detail::pairValue<Src>(decoded, [](const DecodedInstructionPair &d) constexpr { return (expr); })

template <int kUpperHandler, int kFm>
VU1_FORCEINLINE void VU1Core::execUpperStep(uint32_t instr)
{
    if constexpr (kUpperHandler >= 0)
        callUpperHandler<kUpperHandler, kFm>(instr);
    else
        execUpper(instr);
}

template <bool kInlineExec>
VU1_FORCEINLINE void VU1Core::execLowerStep(uint32_t instr, StepArgs &args, uint32_t upperInstr)
{
    if constexpr (kInlineExec)
        execLowerInline(instr, args.vuData, args.dataSize, *args.gs, args.memory, upperInstr);
    else
        execLower(instr, args.vuData, args.dataSize, *args.gs, args.memory, upperInstr);
}

// commitReadyPipelines' flag-only case: commit the due flag entries in ascending index order and
// set next to the earliest one still pending. Caller checked masksValid, !m_fdiv.valid and that
// no other pipeline has entries.
VU1_FORCEINLINE void VU1Core::commitFlagsOnly()
{
    vu1detail::NextReadySlot &slot = *m_slot;
    uint64_t earliest = std::numeric_limits<uint64_t>::max();
    const uint64_t now = m_cycle;
    for (uint32_t bits = slot.flagMask; bits != 0u; bits &= bits - 1u)
    {
        const uint32_t i = static_cast<uint32_t>(std::countr_zero(bits));
        FlagPipelineEntry &entry = m_flagPipeline[i];
        if (entry.readyCycle > now)
        {
            earliest = std::min(earliest, entry.readyCycle);
            continue;
        }
        if (entry.writesMac)
            m_state.mac = entry.mac;
        if (entry.writesStatus)
        {
            const uint32_t current = entry.status & 0xFu;
            m_state.status = (m_state.status & 0xFF0u) | current | ((current | entry.extraSticky) << 6);
        }
        if (entry.writesSticky)
            m_state.status = (m_state.status & 0x03Fu) | (entry.status & 0xFC0u);
        if (entry.writesClip)
            m_state.clip = entry.clip;
        entry.valid = false;
        slot.flagMask &= ~(1u << i);
    }
    slot.next = earliest;
}

VU1_FORCEINLINE void VU1Core::commitDue()
{
    const vu1detail::NextReadySlot &slot = *m_slot;
    if (slot.masksValid && !m_fdiv.valid &&
        (slot.efuMask | slot.storeMask | slot.vfMask | slot.viMask | slot.accMask) == 0u)
    {
#if VU1_COMMIT_LEAF
        commitFlagPipeline();
#else
        commitFlagsOnly();
#endif
        return;
    }
    commitReadyPipelines();
}

template <class Src>
VU1_FORCEINLINE uint64_t VU1Core::calculatePairReadyCycle(const DecodedInstructionPair &decoded) const
{
    uint64_t ready = m_cycle;
    // Register interlocks, from the flattened lists built at decode. Once the cycle has passed every
    // ready-table entry (m_readyHorizon) none of them can stall.
    if constexpr (Src::kStatic)
    {
        // Unrolled, with the table offsets as constants (same entries; max is order-independent).
        if constexpr (Src::d.readyVfCount != 0u || Src::d.readyViCount != 0u || Src::d.readyAccComponents != 0u)
        {
            if (m_cycle < m_readyHorizon)
            {
                const uint64_t *vfReady = m_vfReady[0].data();
                [&]<size_t... I>(std::index_sequence<I...>) {
                    ((ready = std::max(ready, vfReady[vu1detail::kConst<Src::d.readyVf[I]>])), ...);
                }(std::make_index_sequence<Src::d.readyVfCount>{});
                [&]<size_t... I>(std::index_sequence<I...>) {
                    ((ready = std::max(ready, m_viReady[vu1detail::kConst<Src::d.readyVi[I]>])), ...);
                }(std::make_index_sequence<Src::d.readyViCount>{});
                [&]<size_t... C>(std::index_sequence<C...>) {
                    ((((Src::d.readyAccComponents >> C) & 1u) != 0u ? (ready = std::max(ready, m_accReady[C])) : ready), ...);
                }(std::make_index_sequence<4>{});
            }
        }
    }
    else if (m_cycle < m_readyHorizon)
    {
        const uint64_t *vfReady = m_vfReady[0].data();
        for (uint32_t index = 0; index < decoded.readyVfCount; ++index)
            ready = std::max(ready, vfReady[decoded.readyVf[index]]);
        for (uint32_t index = 0; index < decoded.readyViCount; ++index)
            ready = std::max(ready, m_viReady[decoded.readyVi[index]]);
        for (uint32_t bits = decoded.readyAccComponents; bits != 0u; bits &= bits - 1u)
            ready = std::max(ready, m_accReady[static_cast<uint32_t>(std::countr_zero(bits))]);
    }

    if (const uint8_t misc = VU1_PAIR(d.readyMisc))
    {
        if ((misc & (kReadyFdiv | kReadyWaitQ)) != 0u && m_fdiv.valid)
            ready = std::max(ready, m_fdiv.readyCycle);
        if ((misc & kReadyEfu) != 0u)
            ready = std::max(ready, m_efuResourceReady);
        if ((misc & kReadyWaitP) != 0u)
        {
            for (const ScalarPipelineEntry &entry : m_efu)
                if (entry.valid)
                    ready = std::max(ready, entry.readyCycle);
        }
        if ((misc & kReadyXgkick) != 0u && m_xgkick.active)
            ready = std::max(ready, m_cycle + 1u);
    }
    return ready;
}

template <uint32_t kElide, class Src>
VU1_FORCEINLINE void VU1Core::markPairWrites(const DecodedInstructionPair &decoded)
{
    using vu1detail::setLanes;
    constexpr bool kLowerMark = (kElide & kElideLowerMark) == 0u;
    if (kLowerMark && VU1_PAIR(d.lowerUsage.vfWrite.reg != 0u && d.suppressedLowerVf != d.lowerUsage.vfWrite.reg))
    {
        const uint64_t until = m_cycle + VU1_PAIR(d.lowerUsage.vfLatency != 0u ? d.lowerUsage.vfLatency : d.lowerUsage.latency);
        setLanes(m_vfReady[VU1_PAIR(d.lowerUsage.vfWrite.reg)].data(), VU1_PAIR(d.lowerUsage.vfWrite.lanes), until);
        m_readyHorizon = std::max<uint64_t>(m_readyHorizon, until);
    }

    constexpr bool kUpperMark = (kElide & kElideUpperMark) == 0u;
    if (kUpperMark && VU1_PAIR(d.upperUsage.vfWrite.reg != 0u))
    {
        const uint64_t until = m_cycle + VU1_PAIR(d.upperUsage.vfLatency != 0u ? d.upperUsage.vfLatency : d.upperUsage.latency);
        setLanes(m_vfReady[VU1_PAIR(d.upperUsage.vfWrite.reg)].data(), VU1_PAIR(d.upperUsage.vfWrite.lanes), until);
        m_readyHorizon = std::max<uint64_t>(m_readyHorizon, until);
    }

    if constexpr ((kElide & kElideViMark) == 0u)
    {
        if constexpr (Src::kStatic)
        {
            constexpr uint32_t kViBits = static_cast<uint32_t>(Src::d.lowerUsage.viWrite) & 0xFFFEu;
            if constexpr (kViBits != 0u)
            {
                const uint64_t viUntil = m_cycle + VU1_PAIR(d.lowerUsage.viLatency != 0u ? d.lowerUsage.viLatency : d.lowerUsage.latency);
                [&]<size_t... R>(std::index_sequence<R...>) {
                    ((((kViBits >> R) & 1u) != 0u ? (m_viReady[R] = viUntil, m_readyHorizon = std::max<uint64_t>(m_readyHorizon, viUntil))
                                                  : m_readyHorizon),
                     ...);
                }(std::make_index_sequence<16>{}); // every VI register (m_viReady.size())
            }
        }
        else
        {
            for (uint32_t bits = static_cast<uint32_t>(decoded.lowerUsage.viWrite) & 0xFFFEu; bits != 0u; bits &= bits - 1u)
            {
                const uint32_t reg = static_cast<uint32_t>(std::countr_zero(bits));
                if (reg < m_viReady.size())
                {
                    m_viReady[reg] = m_cycle + (decoded.lowerUsage.viLatency != 0u ? decoded.lowerUsage.viLatency : decoded.lowerUsage.latency);
                    m_readyHorizon = std::max<uint64_t>(m_readyHorizon, m_viReady[reg]);
                }
            }
        }
    }
    if constexpr ((kElide & kElideAccMark) == 0u)
    {
        if (const uint8_t accLanes = VU1_PAIR(d.upperUsage.accWrite))
        {
            setLanes(m_accReady.data(), accLanes, m_cycle + kAccForwardLatency);
            m_readyHorizon = std::max<uint64_t>(m_readyHorizon, m_cycle + kAccForwardLatency);
        }
    }
}

template <bool kInlineExec, int kUpperHandler, uint32_t kElide, class Src, uint32_t kBlockNextPc, uint32_t kNormBits, bool kStorePc>
VU1_FORCEINLINE bool VU1Core::stepPair(const DecodedInstructionPair &decoded, bool flagsLive, StepArgs &args)
{
    using vu1detail::keepWrittenLanes;
    using vu1detail::NextReadySlot;
    constexpr int kFm = vu1detail::fmacContext<Src, kNormBits>();
    constexpr bool kBlock = kBlockNextPc != 0u; // runBlock counts pairs/steps and stores m_state.cycles itself
    if constexpr (!kBlock)
        ++g_vuPairs;
    if (VU1_PAIR(d.upperUsage.reserved || d.lowerUsage.reserved))
    {
        reportReservedInstruction(decoded.upperUsage.reserved, decoded.upperUsage.reserved ? decoded.upper : decoded.lower);
        return false;
    }

    if constexpr (!kBlock)
    {
        if (g_vuTraceRunGate > 0)
            tracePair(decoded);
        if (calculatePairReadyCycle<Src>(decoded) > m_cycle)
            waitUntilReady(decoded, args.budgetEnd); // stall (out of line: most pairs do not)
        if (m_cycle >= args.budgetEnd)
            return false;
    }
    else if (calculatePairReadyCycle<Src>(decoded) > m_cycle)
    {
        // In a block the budget can only run out after a stall (runBlock's invariant).
        waitUntilReady(decoded, args.budgetEnd);
        if (m_cycle >= args.budgetEnd)
            return false;
    }
    if constexpr ((kElide & kGateDecrement) != 0u)
        --m_dynamicPairs; // counts executed pairs only (a budget stop above executes none)

    const uint32_t lowerWord = VU1_PAIR(d.lower);
    const uint32_t upperWord = VU1_PAIR(d.upper);
    uint8_t writtenVi = 0u;
    int32_t oldVi = 0;
    if (const uint32_t viBits = VU1_PAIR(static_cast<uint32_t>(d.lowerUsage.viWrite) & 0xFFFEu))
    {
        const uint32_t reg = VU1_PAIR(static_cast<uint32_t>(std::countr_zero(static_cast<uint32_t>(d.lowerUsage.viWrite) & 0xFFFEu)));
        (void)viBits;
        writtenVi = static_cast<uint8_t>(reg);
        oldVi = m_state.vi[reg];
    }

    const uint8_t upperReg = VU1_PAIR(d.upperUsage.vfWrite.reg);
    const uint8_t upperLanes = VU1_PAIR(d.upperUsage.vfWrite.lanes);
    const uint8_t lowerReg = VU1_PAIR(d.lowerUsage.vfWrite.reg);
    const uint8_t lowerLanes = VU1_PAIR(d.lowerUsage.vfWrite.lanes);
    const uint8_t accWrite = VU1_PAIR(d.upperUsage.accWrite);
    const bool hasUpperWrite = VU1_PAIR(d.upperUsage.vfWrite.reg != 0u);
    const bool hasLowerWrite = VU1_PAIR(d.lowerUsage.vfWrite.reg != 0u && d.suppressedLowerVf != d.lowerUsage.vfWrite.reg);
    const bool hasDistinctLowerWrite = VU1_PAIR(d.lowerUsage.vfWrite.reg != 0u && d.suppressedLowerVf != d.lowerUsage.vfWrite.reg &&
                                                (d.upperUsage.vfWrite.reg == 0u || d.lowerUsage.vfWrite.reg != d.upperUsage.vfWrite.reg));
    (void)hasLowerWrite;
    // Only read when the matching write exists (here and in retireQueuedWrites).
    float oldUpperVf[4];
    float oldLowerVf[4];
    float oldAcc[4];
    if (hasUpperWrite)
        std::memcpy(oldUpperVf, m_state.vf[upperReg], sizeof(oldUpperVf));
    if (hasDistinctLowerWrite)
        std::memcpy(oldLowerVf, m_state.vf[lowerReg], sizeof(oldLowerVf));
    if (accWrite != 0u)
        std::memcpy(oldAcc, m_state.acc, sizeof(oldAcc));

    // Read only by the FMAC result paths: a generated pair whose upper op has none leaves the previous
    // pair's value (the next FMAC pair sets its own), and its cache load goes away with the store.
    if constexpr (Src::kStatic)
    {
        if constexpr (vu1detail::upperReadsFlagsLive(Src::d.upper))
            m_currentFlagsLive = flagsLive;
    }
    else
        m_currentFlagsLive = flagsLive;
    // A generated pair with its upper handler (kFm >= 0) stores these and m_currentUpperInstruction
    // only before it takes the out-of-line exact path, their only reader (applyFmacDestFast).
    if constexpr (!(Src::kStatic && kUpperHandler >= 0))
    {
        m_currentFmacNorm = VU1_PAIR(d.fmacNorm);
        m_currentFmacZero = VU1_PAIR(d.fmacZero);
    }
    if (VU1_PAIR(d.iBit))
    {
        execUpperStep<kUpperHandler, kFm>(upperWord);
        if constexpr (Src::kStatic)
            m_state.i = std::bit_cast<float>(vu1detail::kConst<vu1detail::normalizedBits(Src::d.lower)>);
        else
        {
            float immediate = 0.0f;
            std::memcpy(&immediate, &decoded.lower, sizeof(immediate));
            m_state.i = normalizeOperand(immediate);
        }
    }
    else if (const uint8_t shadowReg = VU1_PAIR(d.upperVfShadowReg); shadowReg != 0u)
    {
        float oldVf[4]{};
        float upperVf[4]{};
        std::memcpy(oldVf,
                    m_state.vf[shadowReg],
                    sizeof(oldVf));
        execUpperStep<kUpperHandler, kFm>(upperWord);
        std::memcpy(upperVf,
                    m_state.vf[shadowReg],
                    sizeof(upperVf));
        std::memcpy(m_state.vf[shadowReg],
                    oldVf,
                    sizeof(oldVf));
        execLowerStep<kInlineExec>(lowerWord, args, upperWord);
        std::memcpy(m_state.vf[shadowReg],
                    upperVf,
                    sizeof(upperVf));
    }
    else
    {
        execUpperStep<kUpperHandler, kFm>(upperWord);
        execLowerStep<kInlineExec>(lowerWord, args, upperWord);
    }

    if constexpr ((kElide & kElideViBackupClear) == 0u)
        m_viBranchBackupValid = false;

    // VF/VI/ACC reads are interlocked (calculatePairReadyCycle stalls a pair until every
    // register it reads is ready), so committing a write later changes no value anyone reads:
    // apply it now and keep only its ready cycle (timing) plus the end-of-program flush. Same
    // restore-then-apply-lanes steps as the queued model, so results are bit-identical.
    // PS2X_VU_QUEUED_WRITES=1 goes back to the write queues.
    if (kBlock || !g_vuQueuedWrites) // runBlock's guard: never queued in a block
    {
        NextReadySlot &slot = *m_slot;
        if (hasUpperWrite)
        {
            const uint32_t latency = VU1_PAIR(d.upperUsage.vfLatency != 0u ? d.upperUsage.vfLatency : d.upperUsage.latency);
            keepWrittenLanes(m_state.vf[upperReg], oldUpperVf, upperLanes);
            if constexpr ((kElide & kElideRegUntil) == 0u)
                slot.regWritesUntil = std::max<uint64_t>(slot.regWritesUntil, m_cycle + latency);
        }
        if (hasDistinctLowerWrite)
        {
            const uint32_t latency = VU1_PAIR(d.lowerUsage.vfLatency != 0u ? d.lowerUsage.vfLatency : d.lowerUsage.latency);
            keepWrittenLanes(m_state.vf[lowerReg], oldLowerVf, lowerLanes);
            if constexpr ((kElide & kElideRegUntil) == 0u)
                slot.regWritesUntil = std::max<uint64_t>(slot.regWritesUntil, m_cycle + latency);
        }
        if (accWrite != 0u)
        {
            keepWrittenLanes(m_state.acc, oldAcc, accWrite);
            if constexpr ((kElide & kElideRegUntil) == 0u)
                slot.regWritesUntil = std::max<uint64_t>(slot.regWritesUntil, m_cycle + kAccForwardLatency);
        }
        if (writtenVi != 0u)
        {
            const int32_t newVi = m_state.vi[writtenVi];
            const uint32_t latency = VU1_PAIR(d.lowerUsage.viLatency != 0u ? d.lowerUsage.viLatency : d.lowerUsage.latency);
            m_state.vi[writtenVi] = static_cast<int16_t>(newVi); // as the VI commit stores it
            if constexpr ((kElide & kElideRegUntil) == 0u)
                slot.regWritesUntil = std::max<uint64_t>(slot.regWritesUntil, m_cycle + latency);
        }
    }
    else
        retireQueuedWrites(decoded, oldUpperVf, oldLowerVf, oldAcc, writtenVi, oldVi);

    markPairWrites<kElide, Src>(decoded);
    // JR/JALR: the target is not a static edge of any program's analysis.
    if (VU1_PAIR(d.lowerUsage.pipeline == PipelineBranch && (d.lower >> 26) == 0x24u >> 1))
    {
        m_dynamicPairs = kGatePairs;
        m_normTrust = 0u;
    }
    if (writtenVi != 0u && VU1_PAIR(d.lowerUsage.delaysNextBranchRead))
        recordViWriteForBranch(writtenVi, oldVi);

    // vf0 / vi0 are constant: undo a write to them. A generated pair that writes neither register
    // (decode: writesZeroReg) leaves them as the previous pair did.
    bool restoreZeroRegs = true;
    if constexpr (Src::kStatic)
        restoreZeroRegs = Src::d.upperUsage.writesZeroReg || Src::d.lowerUsage.writesZeroReg;
    if (restoreZeroRegs)
    {
        _mm_storeu_ps(m_state.vf[0], _mm_setr_ps(0.0f, 0.0f, 0.0f, 1.0f)); // one store, see applyDest
        m_state.vi[0] = 0;
    }

    if constexpr (kBlock)
    {
        if constexpr (kStorePc)
            m_state.pc = kBlockNextPc; // runBlock's guard: inside the code, no wrap
    }
    else
    {
        uint32_t nextPc = m_state.pc + 8u;
        if (nextPc >= args.codeSize)
            nextPc = 0u;
        m_state.pc = nextPc;
    }

    if ((kElide & kElideBranch) == 0u && m_state.branchPending)
    {
        if (m_state.branchDelay == 0u)
        {
            m_state.pc = m_state.branchTarget & microAddressMask();
            m_state.branchPending = false;
        }
        else
        {
            --m_state.branchDelay;
        }
    }

    const bool dHalt = VU1_PAIR(d.dBit) && m_state.dBitEnabled;
    const bool tHalt = VU1_PAIR(d.tBit) && m_state.tBitEnabled;
    const bool haltBit = dHalt || tHalt;
    const bool haltBranch = haltBit && VU1_PAIR(d.lowerUsage.pipeline == PipelineBranch);

    constexpr bool kPrevEnd = (kElide & kElidePrevEnd) == 0u;
    if (kPrevEnd && m_state.haltAfterDelaySlot)
    {
        m_state.stoppedByD = m_pendingHaltD;
        m_state.stoppedByT = m_pendingHaltT;
        args.programEnded = true;
    }
    else if (kPrevEnd && m_state.ebit)
        args.programEnded = true;
    else if (haltBit && !haltBranch)
    {
        m_state.stoppedByD = dHalt;
        m_state.stoppedByT = tHalt;
        args.programEnded = true;
    }
    else if (VU1_PAIR(d.eBit))
        m_state.ebit = true;
    else if (haltBranch)
    {
        m_state.haltAfterDelaySlot = true;
        m_pendingHaltD = dHalt;
        m_pendingHaltT = tHalt;
    }

    // advanceOneCycle(), with its two early-outs inline: the pipeline commit only when an entry
    // can be due (the same test commitReadyPipelines starts with), PATH1 only while a kick runs.
    if constexpr (!kBlock)
        ++g_vuSteps;
    ++m_cycle;
    if constexpr (!kBlock)
        m_state.cycles = m_cycle;
    // next == 0 (unknown) also commits: m_cycle >= 0.
    if (m_cycle >= m_slot->next)
        commitDue();
    if (m_xgkick.active)
    {
        // PATH1 moves a qword every second cycle: the cycle in between only takes the credit
        // (progressXgkick's own first step when cycleCredit is 0), so it stays inline.
        if (m_xgkick.cycleCredit == 0u && m_activeVuData && m_activeVuDataSize != 0u)
            m_xgkick.cycleCredit = 1u;
        else
            progressXgkick();
    }
    // A pair runs only while programEnded is false; one without an end check above (no previous E bit /
    // halting branch to finish, no D/T bit of its own) cannot have set it.
    if constexpr (Src::kStatic)
        if constexpr (!kPrevEnd && !Src::d.dBit && !Src::d.tBit)
            return true;
    return !args.programEnded;
}

// run()'s loop between two compiled pairs, for the pair at pairIndex = the previous one + 1 of the same
// program: it runs next exactly when the previous pair fell through to it, the budget is not used up,
// no stop was requested and it lies inside the code. Its table entry is the block's own pair
// function (vu1MergeCompiledProgram installs a block only then), and a fall-through entry needs no
// checkCompiledEntry, so the block runs the very pairs run() would.
VU1_FORCEINLINE bool VU1Core::continueBlock(uint32_t pairIndex, StepArgs &args)
{
    const uint32_t pc = pairIndex * 8u;
    if (m_state.pc != pc || m_cycle >= args.budgetEnd || m_stopRequested || pc + 8u > args.codeSize)
        return false;
    ++g_vuCompiledPairs;
    args.sequentialPc = pc + 8u >= args.codeSize ? 0u : pc + 8u;
    return true;
}

template <int kUpperHandler, uint32_t kElide, class Src>
VU1_FORCEINLINE bool VU1Core::stepCompiledPair(const DecodedInstructionPair &decoded, uint32_t pairIndex, StepArgs &args)
{
    if constexpr ((kElide & kElideBackward) != 0u)
        if (m_dynamicPairs != 0u)
            return stepGated(pairIndex, args);
    return stepPair<true, kUpperHandler, kElide, Src>(decoded, m_decodedCodeCache[pairIndex].flagsLive, args);
}

// run()'s loop body for a compiled function at m_state.pc (same checks, same bookkeeping); every case
// in which run() would do anything else (stop, interpreter, program lookup) returns nullptr to it.
VU1_FORCEINLINE VU1Core::CompiledPairFn VU1Core::chainNext(StepArgs &args)
{
    const uint32_t pc = m_state.pc;
    if (m_cycle >= args.budgetEnd || m_stopRequested || pc + 8u > args.codeSize || (pc & 7u) != 0u || pc / 8u >= kMaxDecodedPairs)
        return nullptr;
    const uint32_t pairIndex = pc / 8u;
    CompiledPairFn fn = m_compiledPairs[pairIndex];
    if (!fn)
        return nullptr;
    if (pc != args.sequentialPc)
        checkCompiledEntry(pairIndex, args.sequentialPc, args.codeSize);
    ++g_vuCompiledPairs;
    args.sequentialPc = pc + 8u >= args.codeSize ? 0u : pc + 8u;
    return fn;
}

template <class Info>
VU1_FORCEINLINE bool VU1Core::stepInfo(StepArgs &args)
{
    using Src = typename Info::Src;
    return stepCompiledPair<Info::kUpperHandler, Info::kElide, Src>(Src::d, Info::kIndex, args);
}

// A generated block of consecutive pairs. run() would take them one by one from the pair table, with
// continueBlock's checks in between; the checks that hold for the whole block are made once here:
// - m_dynamicPairs == 0, so every pair takes its elided step (only a JR/JALR sets it inside a block:
//   the block returns to run() after one);
// - m_cycle + count <= budgetEnd: without a stall pair kPos starts below the budget. The invariant
//   m_cycle + (count - kPos) <= budgetEnd is re-checked only after pairs that can stall;
// - the block ends inside the code (no pc wrap), no PS2X_VU_TRACE run, immediate register writes.
// Inside it each pair stores its fall-through pc as a constant, compares the pc only where a pending
// branch can resolve (no kElideBranch), and the stats counters and m_state.cycles are updated once
// when the block returns. Otherwise the block runs its first pair as run() would and returns.
// Entry lanes (the generator's dataflow over the program kTrust): a block with kNormLo/Hi known runs
// with them only while m_normTrust == kTrust; one the generator found nothing known at (kTrust != 0,
// no lanes) sets m_normTrust: from here on every path stays in the static flow its program's entry
// lanes were computed over, or something clears m_normTrust. kTrust == 0: no knowledge, no claim.
template <uint64_t kTrust, uint64_t kNormLo, uint64_t kNormHi, class... Infos>
VU1_FORCEINLINE bool VU1Core::runBlock(StepArgs &args)
{
    using First = std::tuple_element_t<0, std::tuple<Infos...>>;
    constexpr uint32_t kCount = static_cast<uint32_t>(sizeof...(Infos));
    constexpr uint32_t kEndPc = (First::kIndex + kCount) * 8u;
    constexpr bool kKnown = (kNormLo | kNormHi) != 0u;
    if constexpr (kTrust != 0u && !kKnown)
        m_normTrust = kTrust;
    if (m_dynamicPairs != 0u || g_vuTraceRunGate > 0 || g_vuQueuedWrites || m_cycle + kCount > args.budgetEnd || kEndPc >= args.codeSize ||
        (kKnown && m_normTrust != kTrust))
        return stepInfo<First>(args);
    return runBlockFrom<0u, kCount, true, kNormLo, kNormHi, Infos...>(args);
}

// kPrevMaySetViBackup: the previous pair of the block may have set m_viBranchBackupValid (always true for
// the first pair, entered from outside). args.sequentialPc is stored only when the block leaves: no pair
// reads it, and run() / chainNext need the fall-through pc of the last pair that ran.
// kNormLo/Hi: the vu1detail::NormLanes known normalized before this pair; its upper op skips normalizing
// operands whose lanes are. A product-sum result left unnormalized by the exact path (two roundings)
// sets kBlockBreakNorm, and the block leaves right after that pair (m_blockBreak is checked anyway).
template <uint32_t kPos, uint32_t kCount, bool kPrevMaySetViBackup, uint64_t kNormLo, uint64_t kNormHi, class Info, class... Rest>
VU1_FORCEINLINE bool VU1Core::runBlockFrom(StepArgs &args)
{
    using Src = typename Info::Src;
    constexpr uint32_t kNextPc = (Info::kIndex + 1u) * 8u;
    constexpr vu1detail::NormLanes kNorm{kNormLo, kNormHi};
    constexpr uint32_t kNormBits = vu1detail::normPairBits(kNorm, Src::d);
    constexpr vu1detail::NormLanes kNormNext = vu1detail::normAfterPair(kNorm, Src::d);
    // m_state.pc is stored only where something reads it: this pair may resolve a branch (its pc
    // test below), the next pair's lower op is a branch (target / link from pc) or XGKICK (trace), or
    // the block leaves after this pair (each exit below stores it). Inside the block, pair kPos + 1
    // always starts at kNextPc.
    constexpr bool kLast = sizeof...(Rest) == 0u;
    constexpr bool kJumpPair = Src::d.lowerUsage.pipeline == PipelineBranch && (Src::d.lower >> 26) == 0x24u >> 1;
    constexpr bool kBranchPair = (Info::kElide & kElideBranch) == 0u;
    constexpr bool kStorePc = kLast || kJumpPair || kBranchPair || vu1detail::nextPairReadsPc<Rest...>();
    // What run() and continueBlock would have counted for the pairs entered so far.
    auto leave = [&](bool more) {
        g_vuPairs += kPos + 1u;
        g_vuSteps += kPos + ((more || args.programEnded) ? 1u : 0u);
        g_vuCompiledPairs += kPos;
        m_state.cycles = m_cycle;
        return more;
    };
    constexpr uint32_t kElide = Info::kElide | (kPrevMaySetViBackup ? 0u : kElideViBackupClear);
    if (!stepPair<true, Info::kUpperHandler, kElide, Src, kNextPc, kNormBits, kStorePc>(Src::d, m_decodedCodeCache[Info::kIndex].flagsLive, args))
    {
        // Out of budget after a stall: the pair did not run, pc is its own (the previous pair may not
        // have stored it). Program end: the pc this pair would have stored.
        if constexpr (kPos != 0u)
            if (!args.programEnded)
                m_state.pc = Info::kIndex * 8u;
        if constexpr (!kStorePc)
            if (args.programEnded)
                m_state.pc = kNextPc;
        if constexpr (kPos != 0u)
            args.sequentialPc = Info::kIndex * 8u; // the previous pair's fall-through
        return leave(false);
    }
    if constexpr (sizeof...(Rest) == 0u)
    {
        args.sequentialPc = kNextPc;
        return leave(true);
    }
    else
    {
        constexpr bool kJump = Src::d.lowerUsage.pipeline == PipelineBranch && (Src::d.lower >> 26) == 0x24u >> 1;
        constexpr bool kMayBranch = (Info::kElide & kElideBranch) == 0u;
        constexpr bool kMayStall = Src::d.readyVfCount != 0u || Src::d.readyViCount != 0u || Src::d.readyAccComponents != 0u ||
                                   Src::d.readyMisc != 0u;
        // stepPair calls recordViWriteForBranch exactly then.
        constexpr bool kMaySetViBackup = (Src::d.lowerUsage.viWrite & 0xFFFEu) != 0u && Src::d.lowerUsage.delaysNextBranchRead;
        if constexpr (kJump)
        {
            args.sequentialPc = kNextPc;
            return leave(true); // m_dynamicPairs is set: run() takes the gated steps
        }
        if ((kMayBranch && m_state.pc != kNextPc) || (kMayStall && m_cycle + (kCount - kPos - 1u) > args.budgetEnd) || m_blockBreak != 0u)
        {
            m_blockBreak &= kBlockBreakStop; // a norm break ends only this block
            if constexpr (!kStorePc)
                m_state.pc = kNextPc;
            args.sequentialPc = kNextPc;
            return leave(true);
        }
        return runBlockFrom<kPos + 1u, kCount, kMaySetViBackup, kNormNext.lo, kNormNext.hi, Rest...>(args);
    }
}
