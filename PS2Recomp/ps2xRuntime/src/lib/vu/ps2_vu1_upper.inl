// Upper (FMAC) instruction semantics of the VU core, force-inlined: the interpreter calls it through
// VU1Core::execUpper (ps2_vu1_upper.cpp); generated VU1 programs (vu1gen/) call it with constant
// instruction words, so each pair keeps only its own case.
#pragma once
#include "ps2_vu1_core.h"

#include <bit>
#include <cmath>
#include <cstring>
#include <emmintrin.h>
#include <smmintrin.h>
#include <limits>

namespace vu1upper
{
    // Same as VU1Core::normalizeOperand (denormals -> signed zero, Inf/NaN -> +/-max), but
    // inlinable here: the member lives in ps2_vu1_core.cpp, so every call from this file was an
    // out-of-line call (14 per upper instruction, ~15% of VU1 time).
    VU1_FORCEINLINE float normalizeLane(float value)
    {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        const uint32_t exponent = bits & 0x7F800000u;
        if (exponent == 0u)
            bits &= 0x80000000u;
        else if (exponent == 0x7F800000u)
            bits = (bits & 0x80000000u) | 0x7F7FFFFFu;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    // normalizeLane on four lanes at once (same bits): the magnitude clamped to 0x7F7FFFFF (exponent
    // 255 -> max), zeroed below 0x00800000 (exponent 0 -> zero), then the sign put back. 6 ops instead
    // of the compare/blend form's 9; this runs for up to three operands of every FMAC instruction.
    inline void normalizeLanes4(const float *in, float *out)
    {
        const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i *>(in));
        const __m128i sign = _mm_and_si128(v, _mm_set1_epi32(static_cast<int>(0x80000000u)));
        const __m128i clamped = _mm_min_epu32(_mm_and_si128(v, _mm_set1_epi32(0x7FFFFFFF)), _mm_set1_epi32(0x7F7FFFFF));
        const __m128i normal = _mm_cmpgt_epi32(clamped, _mm_set1_epi32(0x007FFFFF)); // clamped <= 0x7F7FFFFF: signed compare is fine
        _mm_storeu_si128(reinterpret_cast<__m128i *>(out), _mm_or_si128(sign, _mm_and_si128(clamped, normal)));
    }

    // An FMAC operand: normalizeLanes4, or (generated blocks) the register itself when the lanes the
    // instruction depends on are known normalized (kKnown bit 0; normalizeLanes4 would keep them),
    // with the other lanes zeroed unless all four are (bit 1). Those lanes reach no written lane and
    // no flag; zeroing them keeps denormal or NaN garbage out of the arithmetic.
    template <uint32_t kKnown>
    VU1_FORCEINLINE void operandLanes4(const float *in, float *out, uint8_t lanes)
    {
        if constexpr ((kKnown & 2u) != 0u)
            std::memcpy(out, in, 4u * sizeof(float));
        else if constexpr ((kKnown & 1u) != 0u)
            _mm_storeu_ps(out, _mm_and_ps(_mm_loadu_ps(in), vu1detail::destLaneMask(lanes)));
        else
        {
            (void)lanes;
            normalizeLanes4(in, out);
        }
    }

    // Component bits (bit c = component c) <-> VU lane bits (x = 8 ... w = 1): 4-bit reversal.
    inline constexpr uint8_t kReverse4[16] = {0x0, 0x8, 0x4, 0xC, 0x2, 0xA, 0x6, 0xE, 0x1, 0x9, 0x5, 0xD, 0x3, 0xB, 0x7, 0xF};

    inline int32_t vuFloatToInt(float value, float scale)
    {
        const double scaled = static_cast<double>(value) * static_cast<double>(scale);
        if (scaled >= static_cast<double>(std::numeric_limits<int32_t>::max()))
            return std::numeric_limits<int32_t>::max();
        if (scaled <= static_cast<double>(std::numeric_limits<int32_t>::min()))
            return std::numeric_limits<int32_t>::min();
        return static_cast<int32_t>(scaled);
    }
}

using namespace vu1upper;

// FMAC result write-back, the common cases inline; anything else goes to applyFmacDest (same results):
// * Flags unobservable, or dead at this pair (flagsLive) for a single-rounded add/sub/mul: only the
//   value matters, and for these ops the exact path's value equals normalizeResult's (the float result
//   is the exact result rounded toward zero, so zero/underflow/overflow agree in kind and sign).
// * Every written lane passes normalizeFmacResult's fast check (single-rounded: exponent 2..253;
//   product-sum: also within 2^20 of the rounding bound, MADD/MSUB families only): the value stays and
//   each lane's flags are just its sign. Product sticky bits only matter when status is observable,
//   so product-sum ops take this path only when it is not.
// * A lane may instead be an exact zero (float result +/-0 and the exact result zero too): add/sub,
//   mul with a zero operand, or product-sum with a zero ACC lane and a zero product operand (matrix
//   rows with w = 0). Its flags are Z (+S) and the value stays. These were ~13% of all VU1 upper ops
//   on the menu/neighbourhood and went through the out-of-line exact path.
template <int kFm>
VU1_FORCEINLINE void VU1Core::applyFmacDestFast(float *dst, float *result, uint8_t dest, uint32_t instr,
                                                 const float *vs, const float *vt, const float *acc, float q, float i)
{
    // kFm >= 0: a generated VU1 pair's fmacNorm | fmacZero << 8 (compile-time constants); -1: the
    // interpreter's, from the decoded pair through m_currentFmacNorm / m_currentFmacZero.
    const uint8_t norm = kFm >= 0 ? static_cast<uint8_t>(kFm & 0xFF) : m_currentFmacNorm; // computeFmacNorm(instr)
    const uint8_t zeroClass = kFm >= 0 ? static_cast<uint8_t>(kFm >> 8) : m_currentFmacZero;
    const bool isVu1 = kFm >= 0 || m_unit == Unit::VU1; // generated pairs are VU1 only
    const bool productSum = (norm & vu1detail::kFmacNormProductSum) != 0u;
    vu1detail::NextReadySlot &slot = *m_slot;
    const bool flagsLive = m_currentFlagsLive || !isVu1;
    if (!slot.flagsObservableRun || (!flagsLive && !productSum))
    {
        normalizeLanes4(result, result); // normalizeResult's values: exponent 0 -> signed zero, 255 -> +/-max
        applyDest(dst, result, dest);
        return;
    }
    const bool fastProductSum = (norm & vu1detail::kFmacNormProductSumFast) != 0u && isVu1 && slot.statusNeeded == 0;
    if ((!productSum || fastProductSum) && !g_vuForceExactFlags && dest != 0u)
    {
        // Component c is VU lane bit (8 >> c).
        const uint32_t destComponents = ((dest >> 3) & 1u) | ((dest >> 1) & 2u) | ((dest << 1) & 4u) | ((dest << 3) & 8u);
        const __m128i bits = _mm_loadu_si128(reinterpret_cast<const __m128i *>(result));
        const __m128i exponent = _mm_and_si128(_mm_srli_epi32(bits, 23), _mm_set1_epi32(0xFF));
        __m128i ok = _mm_and_si128(_mm_cmpgt_epi32(exponent, _mm_set1_epi32(1)),
                                   _mm_cmplt_epi32(exponent, _mm_set1_epi32(254)));
        // Lanes whose float result is exactly +/-0 (bit c = component c).
        const __m128i magnitude = _mm_and_si128(bits, _mm_set1_epi32(0x7FFFFFFF));
        const uint32_t zeroResult = static_cast<uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(magnitude, _mm_setzero_si128()))));
        // Lanes with a zero product operand (vs lane or the right operand), for the zero cases below.
        uint32_t zeroOperand = 0u;
        if (productSum)
        {
            // normalizeFmacResult's product-sum bound. vs/vt/acc/q/i are the normalized source registers
            // (still unwritten), as normalizeFmacResult reads them; every MADD/MSUB-family op reads ACC.
            __m128 right;
            switch ((norm >> 2) & 3u)
            {
            case 1u:
                right = _mm_set1_ps(vt[(norm >> 4) & 3u]);
                break;
            case 2u:
                right = _mm_set1_ps(q);
                break;
            case 3u:
                right = _mm_set1_ps(i);
                break;
            default:
                right = _mm_loadu_ps(vt);
                break;
            }
            const __m128i product = _mm_castps_si128(_mm_mul_ps(_mm_loadu_ps(vs), right));
            const __m128i productExp = _mm_and_si128(_mm_srli_epi32(product, 23), _mm_set1_epi32(0xFF));
            const __m128i accExp = _mm_and_si128(_mm_srli_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i *>(acc)), 23), _mm_set1_epi32(0xFF));
            const __m128i bound = _mm_max_epi16(productExp, accExp);
            ok = _mm_and_si128(ok, _mm_cmpgt_epi32(_mm_add_epi32(exponent, _mm_set1_epi32(21)), bound));
            if (zeroResult != 0u)
            {
                // acc +/- 0 with acc == +/-0: the exact sum is a signed zero, and IEEE gives the float
                // the same sign (the product of a zero operand is an exact signed zero, fused or not).
                const __m128 zero = _mm_setzero_ps();
                const __m128 opZero = _mm_or_ps(_mm_cmpeq_ps(_mm_loadu_ps(vs), zero), _mm_cmpeq_ps(right, zero));
                zeroOperand = static_cast<uint32_t>(_mm_movemask_ps(_mm_and_ps(opZero, _mm_cmpeq_ps(_mm_loadu_ps(acc), zero))));
            }
        }
        uint32_t settled = static_cast<uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(ok)));
        // Exactly-zero lanes whose exact result is zero too: flags Z (+S), value unchanged. Add/sub:
        // always (normalized operands are never denormal); mul: an operand is zero; product-sum: see above.
        uint32_t zeroLanes = 0u;
        if (const uint32_t candidates = zeroResult & ~settled & destComponents)
        {
            const uint8_t zeroKind = zeroClass & 3u;
            if (productSum)
                zeroLanes = candidates & zeroOperand;
            else if (zeroKind == vu1detail::kFmacZeroAddSub)
                zeroLanes = candidates;
            else if (zeroKind == vu1detail::kFmacZeroMul)
            {
                const __m128 zero = _mm_setzero_ps();
                __m128 right;
                switch ((zeroClass >> 2) & 3u)
                {
                case 1u:
                    right = _mm_set1_ps(vt[(zeroClass >> 4) & 3u]);
                    break;
                case 2u:
                    right = _mm_set1_ps(q);
                    break;
                case 3u:
                    right = _mm_set1_ps(i);
                    break;
                default:
                    right = _mm_loadu_ps(vt);
                    break;
                }
                const __m128 opZero = _mm_or_ps(_mm_cmpeq_ps(_mm_loadu_ps(vs), zero), _mm_cmpeq_ps(right, zero));
                zeroLanes = candidates & static_cast<uint32_t>(_mm_movemask_ps(opZero));
            }
            settled |= zeroLanes;
        }
        const uint32_t freeEntries = ~slot.flagMask & ((1u << kMaxFlagEntries) - 1u);
        if ((settled & destComponents) == destComponents)
        {
            if (!flagsLive)
            {
                applyDest(dst, result, dest); // settled lanes keep their value; dead flags
                return;
            }
            if (slot.masksValid && freeEntries != 0u)
            {
                const uint32_t signs = static_cast<uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(bits))) & destComponents;
                const uint32_t signLanes = kReverse4[signs & 0xFu];
                // updateFmacFlags' entry, extraSticky 0 (status unobservable or not a product-sum op).
                const uint32_t index = static_cast<uint32_t>(std::countr_zero(freeEntries));
                FlagPipelineEntry &entry = m_flagPipeline[index];
                // Every field set one by one (not `entry = {}` first: MSVC built that on the stack and
                // copied it with a 32-byte load, a store-forwarding stall per flag write).
                entry.extraSticky = 0u;
                entry.clip = 0u;
                entry.writesSticky = false;
                entry.writesClip = false;
                entry.valid = true;
                entry.issueCycle = m_cycle;
                entry.readyCycle = m_cycle + kFmacLatency;
                if (slot.next != 0 && entry.readyCycle < slot.next)
                    slot.next = entry.readyCycle;
                slot.flagMask |= 1u << index;
                const uint32_t zeroMacLanes = kReverse4[zeroLanes & 0xFu];
                entry.mac = (signLanes << 4) | zeroMacLanes;
                entry.status = (signs != 0u ? 0x2u : 0u) | (zeroLanes != 0u ? 0x1u : 0u);
                entry.writesMac = true;
                entry.writesStatus = true;
                applyDest(dst, result, dest);
                return;
            }
        }
    }
    if constexpr (kFm >= 0)
    {
        // The exact path reads the executing pair's context from the members (the interpreter sets
        // them for every pair, generated pairs only here).
        m_currentUpperInstruction = instr;
        m_currentFmacNorm = norm;
        m_currentFmacZero = zeroClass;
    }
    applyFmacDest(dst, result, dest);
}

// ============================================================================
// Upper instructions (FMAC pipeline)
// ============================================================================
// Each opcode group of the upper (FMAC) instructions is its own handler (upperOpN); vu1UpperHandler()
// maps an instruction word to its handler. The interpreter dispatches through it at run time
// (execUpperInline), a generated VU1 pair calls its handler directly (the word is a compile-time
// constant), so it compiles only its own operation. Split out of one switch by a script; the
// operation code is unchanged.
#define VU1_UPPER_OPERANDS(instr)                                                                    \
    if constexpr (kFm < 0) /* generated pairs: in applyFmacDestFast, before the exact path */            \
        m_currentUpperInstruction = instr;                                                               \
    uint8_t dest = DEST(instr);                                                                          \
    uint8_t ft = FT(instr);                                                                              \
    uint8_t fs = FS(instr);                                                                              \
    uint8_t fd = FD(instr);                                                                              \
    uint8_t op = instr & 0x3F;                                                                           \
    if (op >= 0x3Cu)                                                                                     \
    {                                                                                                    \
        const uint8_t special = static_cast<uint8_t>((instr & 0x3u) | ((instr >> 4) & 0x7Cu));           \
        if (special == 0x2Fu || special == 0x30u)                                                        \
            return;                                                                                      \
    }                                                                                                    \
    float *vd = m_state.vf[fd];                                                                          \
    float normalizedVs[4];                                                                               \
    float normalizedVt[4];                                                                               \
    float normalizedAcc[4];                                                                              \
    {                                                                                                    \
        const uint8_t s = op >= 0x3Cu ? static_cast<uint8_t>((instr & 0x3u) | ((instr >> 4) & 0x7Cu)) : 0xFFu; \
        const bool readsAcc = op < 0x3Cu                                                                 \
                                  ? ((op >= 0x08u && op <= 0x0Fu) || op == 0x21u || op == 0x23u || op == 0x25u || \
                                     op == 0x27u || op == 0x29u || op == 0x2Du || op == 0x2Eu)           \
                                  : ((s >= 0x08u && s <= 0x0Fu) || s == 0x21u || s == 0x23u || s == 0x25u || \
                                     s == 0x27u || s == 0x29u || s == 0x2Du);                            \
        /* kFm bits 16+ (generated blocks): operand lanes known to be normalized (kNormShift*) */    \
        constexpr uint32_t kNormBits = kFm >= 0 ? static_cast<uint32_t>(kFm) >> 16 : 0u;             \
        const vu1detail::UpperNormInfo normInfo = vu1detail::upperNormInfo(instr);                   \
        operandLanes4<(kNormBits >> vu1detail::kNormShiftFs) & 3u>(m_state.vf[fs], normalizedVs, normInfo.fsLanes);\
        operandLanes4<(kNormBits >> vu1detail::kNormShiftFt) & 3u>(m_state.vf[ft], normalizedVt, normInfo.ftLanes);\
        if (readsAcc)                                                                                    \
            operandLanes4<(kNormBits >> vu1detail::kNormShiftAcc) & 3u>(m_state.acc, normalizedAcc, normInfo.accLanes);\
        else                                                                                             \
            std::memset(normalizedAcc, 0, sizeof(normalizedAcc));                                        \
    }                                                                                                    \
    const float *vs = normalizedVs;                                                                      \
    const float *vt = normalizedVt;                                                                      \
    const float *acc = normalizedAcc;                                                                    \
    const float q = normalizeLane(m_state.q);                                                            \
    const float i = normalizeLane(m_state.i);                                                            \
    float result[4];                                                                                     \
    (void)0

// upper 0x00,0x01,0x02,0x03
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp0(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    // ADDbc
    {
        float bc = vt[op & 3];
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] + bc;
        applyFmacDestFast<kFm>(vd, result, dest, instr, vs, vt, acc, q, i);
        return;
    }
}

// upper 0x04,0x05,0x06,0x07
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp1(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    // SUBbc
    {
        float bc = vt[op & 3];
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] - bc;
        applyFmacDestFast<kFm>(vd, result, dest, instr, vs, vt, acc, q, i);
        return;
    }
}

// upper 0x08,0x09,0x0A,0x0B
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp2(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    // MADDbc
    {
        float bc = vt[op & 3];
        for (int c = 0; c < 4; c++)
            result[c] = acc[c] + vs[c] * bc;
        applyFmacDestFast<kFm>(vd, result, dest, instr, vs, vt, acc, q, i);
        return;
    }
}

// upper 0x0C,0x0D,0x0E,0x0F
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp3(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    // MSUBbc
    {
        float bc = vt[op & 3];
        for (int c = 0; c < 4; c++)
            result[c] = acc[c] - vs[c] * bc;
        applyFmacDestFast<kFm>(vd, result, dest, instr, vs, vt, acc, q, i);
        return;
    }
}

// upper 0x10,0x11,0x12,0x13
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp4(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    // MAXbc
    {
        float bc = vt[op & 3];
        for (int c = 0; c < 4; c++)
            result[c] = (vs[c] > bc) ? vs[c] : bc;
        applyDest(vd, result, dest);
        return;
    }
}

// upper 0x14,0x15,0x16,0x17
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp5(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    // MINIbc
    {
        float bc = vt[op & 3];
        for (int c = 0; c < 4; c++)
            result[c] = (vs[c] < bc) ? vs[c] : bc;
        applyDest(vd, result, dest);
        return;
    }
}

// upper 0x18,0x19,0x1A,0x1B
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp6(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    // MULbc
    {
        float bc = vt[op & 3];
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] * bc;
        applyFmacDestFast<kFm>(vd, result, dest, instr, vs, vt, acc, q, i);
        return;
    }
}

// upper 0x1C
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp7(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    // MULq
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] * q;
        applyFmacDestFast<kFm>(vd, result, dest, instr, vs, vt, acc, q, i);
        return;
}

// upper 0x1D
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp8(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    // MAXi
        for (int c = 0; c < 4; c++)
            result[c] = (vs[c] > i) ? vs[c] : i;
        applyDest(vd, result, dest);
        return;
}

// upper 0x1E
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp9(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    // MULi
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] * i;
        applyFmacDestFast<kFm>(vd, result, dest, instr, vs, vt, acc, q, i);
        return;
}

// upper 0x1F
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp10(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    // MINIi
        for (int c = 0; c < 4; c++)
            result[c] = (vs[c] < i) ? vs[c] : i;
        applyDest(vd, result, dest);
        return;
}

// upper 0x20
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp11(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    // ADDq
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] + q;
        applyFmacDestFast<kFm>(vd, result, dest, instr, vs, vt, acc, q, i);
        return;
}

// upper 0x21
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp12(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    // MADDq
        for (int c = 0; c < 4; c++)
            result[c] = acc[c] + vs[c] * q;
        applyFmacDestFast<kFm>(vd, result, dest, instr, vs, vt, acc, q, i);
        return;
}

// upper 0x22
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp13(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    // ADDi
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] + i;
        applyFmacDestFast<kFm>(vd, result, dest, instr, vs, vt, acc, q, i);
        return;
}

// upper 0x23
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp14(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    // MADDi
        for (int c = 0; c < 4; c++)
            result[c] = acc[c] + vs[c] * i;
        applyFmacDestFast<kFm>(vd, result, dest, instr, vs, vt, acc, q, i);
        return;
}

// upper 0x24
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp15(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    // SUBq
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] - q;
        applyFmacDestFast<kFm>(vd, result, dest, instr, vs, vt, acc, q, i);
        return;
}

// upper 0x25
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp16(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    // MSUBq
        for (int c = 0; c < 4; c++)
            result[c] = acc[c] - vs[c] * q;
        applyFmacDestFast<kFm>(vd, result, dest, instr, vs, vt, acc, q, i);
        return;
}

// upper 0x26
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp17(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    // SUBi
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] - i;
        applyFmacDestFast<kFm>(vd, result, dest, instr, vs, vt, acc, q, i);
        return;
}

// upper 0x27
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp18(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    // MSUBi
        for (int c = 0; c < 4; c++)
            result[c] = acc[c] - vs[c] * i;
        applyFmacDestFast<kFm>(vd, result, dest, instr, vs, vt, acc, q, i);
        return;
}

// upper 0x28
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp19(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    // ADD
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] + vt[c];
        applyFmacDestFast<kFm>(vd, result, dest, instr, vs, vt, acc, q, i);
        return;
}

// upper 0x29
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp20(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    // MADD
        for (int c = 0; c < 4; c++)
            result[c] = acc[c] + vs[c] * vt[c];
        applyFmacDestFast<kFm>(vd, result, dest, instr, vs, vt, acc, q, i);
        return;
}

// upper 0x2A
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp21(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    // MUL
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] * vt[c];
        applyFmacDestFast<kFm>(vd, result, dest, instr, vs, vt, acc, q, i);
        return;
}

// upper 0x2B
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp22(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    // MAX
        for (int c = 0; c < 4; c++)
            result[c] = (vs[c] > vt[c]) ? vs[c] : vt[c];
        applyDest(vd, result, dest);
        return;
}

// upper 0x2C
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp23(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    // SUB
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] - vt[c];
        applyFmacDestFast<kFm>(vd, result, dest, instr, vs, vt, acc, q, i);
        return;
}

// upper 0x2D
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp24(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    // MSUB
        for (int c = 0; c < 4; c++)
            result[c] = acc[c] - vs[c] * vt[c];
        applyFmacDestFast<kFm>(vd, result, dest, instr, vs, vt, acc, q, i);
        return;
}

// upper 0x2E
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp25(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    // OPMSUB
        result[0] = acc[0] - vs[1] * vt[2];
        result[1] = acc[1] - vs[2] * vt[0];
        result[2] = acc[2] - vs[0] * vt[1];
        result[3] = 0.0f;
        applyFmacDestFast<kFm>(vd, result, dest, instr, vs, vt, acc, q, i);
        return;
}

// upper 0x2F
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp26(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    // MINI
        for (int c = 0; c < 4; c++)
            result[c] = (vs[c] < vt[c]) ? vs[c] : vt[c];
        applyDest(vd, result, dest);
        return;

    // Upper special group (low op 0x3C..0x3F).
    // Like lower1 special, the real selector is not just bits 5:0.  Dobie decodes:
    //   op = (instr & 0x3) | ((instr >> 4) & 0x7C)
    // Several instructions in this group also use FT as the destination, not FD.
}

// upper special 0x00,0x01,0x02,0x03
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp27(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    const uint8_t specialOp = static_cast<uint8_t>((instr & 0x3u) | ((instr >> 4) & 0x7Cu));
    float *vtDest = m_state.vf[ft];
    // ADDAbc
    {
        float bc = vt[specialOp & 3];
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] + bc;
        applyFmacDestFast<kFm>(m_state.acc, result, dest, instr, vs, vt, acc, q, i);
        return;
    }
}

// upper special 0x04,0x05,0x06,0x07
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp28(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    const uint8_t specialOp = static_cast<uint8_t>((instr & 0x3u) | ((instr >> 4) & 0x7Cu));
    float *vtDest = m_state.vf[ft];
    // SUBAbc
    {
        float bc = vt[specialOp & 3];
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] - bc;
        applyFmacDestFast<kFm>(m_state.acc, result, dest, instr, vs, vt, acc, q, i);
        return;
    }
}

// upper special 0x08,0x09,0x0A,0x0B
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp29(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    const uint8_t specialOp = static_cast<uint8_t>((instr & 0x3u) | ((instr >> 4) & 0x7Cu));
    float *vtDest = m_state.vf[ft];
    // MADDAbc
    {
        float bc = vt[specialOp & 3];
        for (int c = 0; c < 4; c++)
            result[c] = acc[c] + vs[c] * bc;
        applyFmacDestFast<kFm>(m_state.acc, result, dest, instr, vs, vt, acc, q, i);
        return;
    }
}

// upper special 0x0C,0x0D,0x0E,0x0F
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp30(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    const uint8_t specialOp = static_cast<uint8_t>((instr & 0x3u) | ((instr >> 4) & 0x7Cu));
    float *vtDest = m_state.vf[ft];
    // MSUBAbc
    {
        float bc = vt[specialOp & 3];
        for (int c = 0; c < 4; c++)
            result[c] = acc[c] - vs[c] * bc;
        applyFmacDestFast<kFm>(m_state.acc, result, dest, instr, vs, vt, acc, q, i);
        return;
    }
}

// upper special 0x10
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp31(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    const uint8_t specialOp = static_cast<uint8_t>((instr & 0x3u) | ((instr >> 4) & 0x7Cu));
    float *vtDest = m_state.vf[ft];
    // ITOF0
        for (int c = 0; c < 4; c++)
        {
            int32_t iv;
            std::memcpy(&iv, &m_state.vf[fs][c], 4);
            result[c] = static_cast<float>(iv);
        }
        applyDest(vtDest, result, dest);
        return;
}

// upper special 0x11
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp32(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    const uint8_t specialOp = static_cast<uint8_t>((instr & 0x3u) | ((instr >> 4) & 0x7Cu));
    float *vtDest = m_state.vf[ft];
    // ITOF4
        for (int c = 0; c < 4; c++)
        {
            int32_t iv;
            std::memcpy(&iv, &m_state.vf[fs][c], 4);
            result[c] = static_cast<float>(iv) / 16.0f;
        }
        applyDest(vtDest, result, dest);
        return;
}

// upper special 0x12
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp33(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    const uint8_t specialOp = static_cast<uint8_t>((instr & 0x3u) | ((instr >> 4) & 0x7Cu));
    float *vtDest = m_state.vf[ft];
    // ITOF12
        for (int c = 0; c < 4; c++)
        {
            int32_t iv;
            std::memcpy(&iv, &m_state.vf[fs][c], 4);
            result[c] = static_cast<float>(iv) / 4096.0f;
        }
        applyDest(vtDest, result, dest);
        return;
}

// upper special 0x13
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp34(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    const uint8_t specialOp = static_cast<uint8_t>((instr & 0x3u) | ((instr >> 4) & 0x7Cu));
    float *vtDest = m_state.vf[ft];
    // ITOF15
        for (int c = 0; c < 4; c++)
        {
            int32_t iv;
            std::memcpy(&iv, &m_state.vf[fs][c], 4);
            result[c] = static_cast<float>(iv) / 32768.0f;
        }
        applyDest(vtDest, result, dest);
        return;
}

// upper special 0x14
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp35(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    const uint8_t specialOp = static_cast<uint8_t>((instr & 0x3u) | ((instr >> 4) & 0x7Cu));
    float *vtDest = m_state.vf[ft];
    // FTOI0
        for (int c = 0; c < 4; c++)
        {
            int32_t iv = vuFloatToInt(vs[c], 1.0f);
            std::memcpy(&result[c], &iv, 4);
        }
        applyDest(vtDest, result, dest);
        return;
}

// upper special 0x15
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp36(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    const uint8_t specialOp = static_cast<uint8_t>((instr & 0x3u) | ((instr >> 4) & 0x7Cu));
    float *vtDest = m_state.vf[ft];
    // FTOI4
        for (int c = 0; c < 4; c++)
        {
            int32_t iv = vuFloatToInt(vs[c], 16.0f);
            std::memcpy(&result[c], &iv, 4);
        }
        applyDest(vtDest, result, dest);
        return;
}

// upper special 0x16
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp37(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    const uint8_t specialOp = static_cast<uint8_t>((instr & 0x3u) | ((instr >> 4) & 0x7Cu));
    float *vtDest = m_state.vf[ft];
    // FTOI12
        for (int c = 0; c < 4; c++)
        {
            int32_t iv = vuFloatToInt(vs[c], 4096.0f);
            std::memcpy(&result[c], &iv, 4);
        }
        applyDest(vtDest, result, dest);
        return;
}

// upper special 0x17
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp38(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    const uint8_t specialOp = static_cast<uint8_t>((instr & 0x3u) | ((instr >> 4) & 0x7Cu));
    float *vtDest = m_state.vf[ft];
    // FTOI15
        for (int c = 0; c < 4; c++)
        {
            int32_t iv = vuFloatToInt(vs[c], 32768.0f);
            std::memcpy(&result[c], &iv, 4);
        }
        applyDest(vtDest, result, dest);
        return;
}

// upper special 0x18,0x19,0x1A,0x1B
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp39(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    const uint8_t specialOp = static_cast<uint8_t>((instr & 0x3u) | ((instr >> 4) & 0x7Cu));
    float *vtDest = m_state.vf[ft];
    // MULAbc
    {
        float bc = vt[specialOp & 3];
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] * bc;
        applyFmacDestFast<kFm>(m_state.acc, result, dest, instr, vs, vt, acc, q, i);
        return;
    }
}

// upper special 0x1C
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp40(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    const uint8_t specialOp = static_cast<uint8_t>((instr & 0x3u) | ((instr >> 4) & 0x7Cu));
    float *vtDest = m_state.vf[ft];
    // MULAq
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] * q;
        applyFmacDestFast<kFm>(m_state.acc, result, dest, instr, vs, vt, acc, q, i);
        return;
}

// upper special 0x1D
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp41(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    const uint8_t specialOp = static_cast<uint8_t>((instr & 0x3u) | ((instr >> 4) & 0x7Cu));
    float *vtDest = m_state.vf[ft];
    // ABS
        for (int c = 0; c < 4; c++)
            result[c] = std::fabs(vs[c]);
        applyDest(vtDest, result, dest);
        return;
}

// upper special 0x1E
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp42(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    const uint8_t specialOp = static_cast<uint8_t>((instr & 0x3u) | ((instr >> 4) & 0x7Cu));
    float *vtDest = m_state.vf[ft];
    // MULAi
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] * i;
        applyFmacDestFast<kFm>(m_state.acc, result, dest, instr, vs, vt, acc, q, i);
        return;
}

// upper special 0x1F
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp43(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    const uint8_t specialOp = static_cast<uint8_t>((instr & 0x3u) | ((instr >> 4) & 0x7Cu));
    float *vtDest = m_state.vf[ft];
    // CLIP
    {
        uint32_t wBits = 0u;
        std::memcpy(&wBits, &m_state.vf[ft][3], sizeof(wBits));
        const int32_t limit = (wBits & 0x7F800000u) != 0u ? static_cast<int32_t>(wBits & 0x7FFFFFFFu) : 0x007FFFFF;

        const auto exceedsClipPlane = [limit](float value, uint32_t signMask)
        {
            uint32_t bits = 0u;
            std::memcpy(&bits, &value, sizeof(bits));
            bits ^= signMask;
            int32_t orderedBits = 0;
            std::memcpy(&orderedBits, &bits, sizeof(orderedBits));
            return orderedBits > limit;
        };

        uint32_t flags = 0u;
        if (exceedsClipPlane(m_state.vf[fs][0], 0x00000000u))
            flags |= 0x01u;
        if (exceedsClipPlane(m_state.vf[fs][0], 0x80000000u))
            flags |= 0x02u;
        if (exceedsClipPlane(m_state.vf[fs][1], 0x00000000u))
            flags |= 0x04u;
        if (exceedsClipPlane(m_state.vf[fs][1], 0x80000000u))
            flags |= 0x08u;
        if (exceedsClipPlane(m_state.vf[fs][2], 0x00000000u))
            flags |= 0x10u;
        if (exceedsClipPlane(m_state.vf[fs][2], 0x80000000u))
            flags |= 0x20u;
        queueClip(flags);
        return;
    }
}

// upper special 0x20
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp44(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    const uint8_t specialOp = static_cast<uint8_t>((instr & 0x3u) | ((instr >> 4) & 0x7Cu));
    float *vtDest = m_state.vf[ft];
    // ADDAq
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] + q;
        applyFmacDestFast<kFm>(m_state.acc, result, dest, instr, vs, vt, acc, q, i);
        return;
}

// upper special 0x21
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp45(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    const uint8_t specialOp = static_cast<uint8_t>((instr & 0x3u) | ((instr >> 4) & 0x7Cu));
    float *vtDest = m_state.vf[ft];
    // MADDAq
        for (int c = 0; c < 4; c++)
            result[c] = acc[c] + vs[c] * q;
        applyFmacDestFast<kFm>(m_state.acc, result, dest, instr, vs, vt, acc, q, i);
        return;
}

// upper special 0x22
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp46(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    const uint8_t specialOp = static_cast<uint8_t>((instr & 0x3u) | ((instr >> 4) & 0x7Cu));
    float *vtDest = m_state.vf[ft];
    // ADDAi
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] + i;
        applyFmacDestFast<kFm>(m_state.acc, result, dest, instr, vs, vt, acc, q, i);
        return;
}

// upper special 0x23
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp47(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    const uint8_t specialOp = static_cast<uint8_t>((instr & 0x3u) | ((instr >> 4) & 0x7Cu));
    float *vtDest = m_state.vf[ft];
    // MADDAi
        for (int c = 0; c < 4; c++)
            result[c] = acc[c] + vs[c] * i;
        applyFmacDestFast<kFm>(m_state.acc, result, dest, instr, vs, vt, acc, q, i);
        return;
}

// upper special 0x24
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp48(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    const uint8_t specialOp = static_cast<uint8_t>((instr & 0x3u) | ((instr >> 4) & 0x7Cu));
    float *vtDest = m_state.vf[ft];
    // SUBAq
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] - q;
        applyFmacDestFast<kFm>(m_state.acc, result, dest, instr, vs, vt, acc, q, i);
        return;
}

// upper special 0x25
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp49(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    const uint8_t specialOp = static_cast<uint8_t>((instr & 0x3u) | ((instr >> 4) & 0x7Cu));
    float *vtDest = m_state.vf[ft];
    // MSUBAq
        for (int c = 0; c < 4; c++)
            result[c] = acc[c] - vs[c] * q;
        applyFmacDestFast<kFm>(m_state.acc, result, dest, instr, vs, vt, acc, q, i);
        return;
}

// upper special 0x26
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp50(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    const uint8_t specialOp = static_cast<uint8_t>((instr & 0x3u) | ((instr >> 4) & 0x7Cu));
    float *vtDest = m_state.vf[ft];
    // SUBAi
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] - i;
        applyFmacDestFast<kFm>(m_state.acc, result, dest, instr, vs, vt, acc, q, i);
        return;
}

// upper special 0x27
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp51(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    const uint8_t specialOp = static_cast<uint8_t>((instr & 0x3u) | ((instr >> 4) & 0x7Cu));
    float *vtDest = m_state.vf[ft];
    // MSUBAi
        for (int c = 0; c < 4; c++)
            result[c] = acc[c] - vs[c] * i;
        applyFmacDestFast<kFm>(m_state.acc, result, dest, instr, vs, vt, acc, q, i);
        return;
}

// upper special 0x28
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp52(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    const uint8_t specialOp = static_cast<uint8_t>((instr & 0x3u) | ((instr >> 4) & 0x7Cu));
    float *vtDest = m_state.vf[ft];
    // ADDA
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] + vt[c];
        applyFmacDestFast<kFm>(m_state.acc, result, dest, instr, vs, vt, acc, q, i);
        return;
}

// upper special 0x29
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp53(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    const uint8_t specialOp = static_cast<uint8_t>((instr & 0x3u) | ((instr >> 4) & 0x7Cu));
    float *vtDest = m_state.vf[ft];
    // MADDA
        for (int c = 0; c < 4; c++)
            result[c] = acc[c] + vs[c] * vt[c];
        applyFmacDestFast<kFm>(m_state.acc, result, dest, instr, vs, vt, acc, q, i);
        return;
}

// upper special 0x2A
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp54(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    const uint8_t specialOp = static_cast<uint8_t>((instr & 0x3u) | ((instr >> 4) & 0x7Cu));
    float *vtDest = m_state.vf[ft];
    // MULA
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] * vt[c];
        applyFmacDestFast<kFm>(m_state.acc, result, dest, instr, vs, vt, acc, q, i);
        return;
}

// upper special 0x2C
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp55(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    const uint8_t specialOp = static_cast<uint8_t>((instr & 0x3u) | ((instr >> 4) & 0x7Cu));
    float *vtDest = m_state.vf[ft];
    // SUBA
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] - vt[c];
        applyFmacDestFast<kFm>(m_state.acc, result, dest, instr, vs, vt, acc, q, i);
        return;
}

// upper special 0x2D
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp56(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    const uint8_t specialOp = static_cast<uint8_t>((instr & 0x3u) | ((instr >> 4) & 0x7Cu));
    float *vtDest = m_state.vf[ft];
    // MSUBA
        for (int c = 0; c < 4; c++)
            result[c] = acc[c] - vs[c] * vt[c];
        applyFmacDestFast<kFm>(m_state.acc, result, dest, instr, vs, vt, acc, q, i);
        return;
}

// upper special 0x2E
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp57(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    const uint8_t specialOp = static_cast<uint8_t>((instr & 0x3u) | ((instr >> 4) & 0x7Cu));
    float *vtDest = m_state.vf[ft];
    // OPMULA
        result[0] = vs[1] * vt[2];
        result[1] = vs[2] * vt[0];
        result[2] = vs[0] * vt[1];
        result[3] = 0.0f;
        applyFmacDestFast<kFm>(m_state.acc, result, dest, instr, vs, vt, acc, q, i);
        return;
}

// upper special 0x2F,0x30
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp58(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    const uint8_t specialOp = static_cast<uint8_t>((instr & 0x3u) | ((instr >> 4) & 0x7Cu));
    float *vtDest = m_state.vf[ft];
    // NOP
        return;
}

// upper special default
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp59(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
    const uint8_t specialOp = static_cast<uint8_t>((instr & 0x3u) | ((instr >> 4) & 0x7Cu));
    float *vtDest = m_state.vf[ft];
        reportReservedInstruction(true, instr);
        return;
}

// upper 0x30,0x31,0x32,0x33,default
template <int kFm>
VU1_FORCEINLINE void VU1Core::upperOp60(uint32_t instr)
{
    VU1_UPPER_OPERANDS(instr);
        reportReservedInstruction(true, instr);
        return;
}

constexpr int vu1UpperHandler(uint32_t instr)
{
    const uint8_t op = static_cast<uint8_t>(instr & 0x3Fu);
    if (op >= 0x3Cu)
    {
        switch (static_cast<uint8_t>((instr & 0x3u) | ((instr >> 4) & 0x7Cu)))
        {
        case 0x00:
        case 0x01:
        case 0x02:
        case 0x03:
            return 27;
        case 0x04:
        case 0x05:
        case 0x06:
        case 0x07:
            return 28;
        case 0x08:
        case 0x09:
        case 0x0A:
        case 0x0B:
            return 29;
        case 0x0C:
        case 0x0D:
        case 0x0E:
        case 0x0F:
            return 30;
        case 0x10:
            return 31;
        case 0x11:
            return 32;
        case 0x12:
            return 33;
        case 0x13:
            return 34;
        case 0x14:
            return 35;
        case 0x15:
            return 36;
        case 0x16:
            return 37;
        case 0x17:
            return 38;
        case 0x18:
        case 0x19:
        case 0x1A:
        case 0x1B:
            return 39;
        case 0x1C:
            return 40;
        case 0x1D:
            return 41;
        case 0x1E:
            return 42;
        case 0x1F:
            return 43;
        case 0x20:
            return 44;
        case 0x21:
            return 45;
        case 0x22:
            return 46;
        case 0x23:
            return 47;
        case 0x24:
            return 48;
        case 0x25:
            return 49;
        case 0x26:
            return 50;
        case 0x27:
            return 51;
        case 0x28:
            return 52;
        case 0x29:
            return 53;
        case 0x2A:
            return 54;
        case 0x2C:
            return 55;
        case 0x2D:
            return 56;
        case 0x2E:
            return 57;
        case 0x2F:
        case 0x30:
            return 58;
        default:
            return 59;
        }
    }
    switch (op)
    {
    case 0x00:
    case 0x01:
    case 0x02:
    case 0x03:
        return 0;
    case 0x04:
    case 0x05:
    case 0x06:
    case 0x07:
        return 1;
    case 0x08:
    case 0x09:
    case 0x0A:
    case 0x0B:
        return 2;
    case 0x0C:
    case 0x0D:
    case 0x0E:
    case 0x0F:
        return 3;
    case 0x10:
    case 0x11:
    case 0x12:
    case 0x13:
        return 4;
    case 0x14:
    case 0x15:
    case 0x16:
    case 0x17:
        return 5;
    case 0x18:
    case 0x19:
    case 0x1A:
    case 0x1B:
        return 6;
    case 0x1C:
        return 7;
    case 0x1D:
        return 8;
    case 0x1E:
        return 9;
    case 0x1F:
        return 10;
    case 0x20:
        return 11;
    case 0x21:
        return 12;
    case 0x22:
        return 13;
    case 0x23:
        return 14;
    case 0x24:
        return 15;
    case 0x25:
        return 16;
    case 0x26:
        return 17;
    case 0x27:
        return 18;
    case 0x28:
        return 19;
    case 0x29:
        return 20;
    case 0x2A:
        return 21;
    case 0x2B:
        return 22;
    case 0x2C:
        return 23;
    case 0x2D:
        return 24;
    case 0x2E:
        return 25;
    case 0x2F:
        return 26;
    case 0x30:
    case 0x31:
    case 0x32:
    case 0x33:
    default:
        return 60;
    }
}

using VU1UpperHandlerFn = void (VU1Core::*)(uint32_t);
inline constexpr VU1UpperHandlerFn kVu1UpperHandlers[] = {
    &VU1Core::upperOp0<-1>,
    &VU1Core::upperOp1<-1>,
    &VU1Core::upperOp2<-1>,
    &VU1Core::upperOp3<-1>,
    &VU1Core::upperOp4<-1>,
    &VU1Core::upperOp5<-1>,
    &VU1Core::upperOp6<-1>,
    &VU1Core::upperOp7<-1>,
    &VU1Core::upperOp8<-1>,
    &VU1Core::upperOp9<-1>,
    &VU1Core::upperOp10<-1>,
    &VU1Core::upperOp11<-1>,
    &VU1Core::upperOp12<-1>,
    &VU1Core::upperOp13<-1>,
    &VU1Core::upperOp14<-1>,
    &VU1Core::upperOp15<-1>,
    &VU1Core::upperOp16<-1>,
    &VU1Core::upperOp17<-1>,
    &VU1Core::upperOp18<-1>,
    &VU1Core::upperOp19<-1>,
    &VU1Core::upperOp20<-1>,
    &VU1Core::upperOp21<-1>,
    &VU1Core::upperOp22<-1>,
    &VU1Core::upperOp23<-1>,
    &VU1Core::upperOp24<-1>,
    &VU1Core::upperOp25<-1>,
    &VU1Core::upperOp26<-1>,
    &VU1Core::upperOp27<-1>,
    &VU1Core::upperOp28<-1>,
    &VU1Core::upperOp29<-1>,
    &VU1Core::upperOp30<-1>,
    &VU1Core::upperOp31<-1>,
    &VU1Core::upperOp32<-1>,
    &VU1Core::upperOp33<-1>,
    &VU1Core::upperOp34<-1>,
    &VU1Core::upperOp35<-1>,
    &VU1Core::upperOp36<-1>,
    &VU1Core::upperOp37<-1>,
    &VU1Core::upperOp38<-1>,
    &VU1Core::upperOp39<-1>,
    &VU1Core::upperOp40<-1>,
    &VU1Core::upperOp41<-1>,
    &VU1Core::upperOp42<-1>,
    &VU1Core::upperOp43<-1>,
    &VU1Core::upperOp44<-1>,
    &VU1Core::upperOp45<-1>,
    &VU1Core::upperOp46<-1>,
    &VU1Core::upperOp47<-1>,
    &VU1Core::upperOp48<-1>,
    &VU1Core::upperOp49<-1>,
    &VU1Core::upperOp50<-1>,
    &VU1Core::upperOp51<-1>,
    &VU1Core::upperOp52<-1>,
    &VU1Core::upperOp53<-1>,
    &VU1Core::upperOp54<-1>,
    &VU1Core::upperOp55<-1>,
    &VU1Core::upperOp56<-1>,
    &VU1Core::upperOp57<-1>,
    &VU1Core::upperOp58<-1>,
    &VU1Core::upperOp59<-1>,
    &VU1Core::upperOp60<-1>,
};

template <int kHandler, int kFm>
VU1_FORCEINLINE void VU1Core::callUpperHandler(uint32_t instr)
{
    if constexpr (kHandler == 0)
        upperOp0<kFm>(instr);
    else if constexpr (kHandler == 1)
        upperOp1<kFm>(instr);
    else if constexpr (kHandler == 2)
        upperOp2<kFm>(instr);
    else if constexpr (kHandler == 3)
        upperOp3<kFm>(instr);
    else if constexpr (kHandler == 4)
        upperOp4<kFm>(instr);
    else if constexpr (kHandler == 5)
        upperOp5<kFm>(instr);
    else if constexpr (kHandler == 6)
        upperOp6<kFm>(instr);
    else if constexpr (kHandler == 7)
        upperOp7<kFm>(instr);
    else if constexpr (kHandler == 8)
        upperOp8<kFm>(instr);
    else if constexpr (kHandler == 9)
        upperOp9<kFm>(instr);
    else if constexpr (kHandler == 10)
        upperOp10<kFm>(instr);
    else if constexpr (kHandler == 11)
        upperOp11<kFm>(instr);
    else if constexpr (kHandler == 12)
        upperOp12<kFm>(instr);
    else if constexpr (kHandler == 13)
        upperOp13<kFm>(instr);
    else if constexpr (kHandler == 14)
        upperOp14<kFm>(instr);
    else if constexpr (kHandler == 15)
        upperOp15<kFm>(instr);
    else if constexpr (kHandler == 16)
        upperOp16<kFm>(instr);
    else if constexpr (kHandler == 17)
        upperOp17<kFm>(instr);
    else if constexpr (kHandler == 18)
        upperOp18<kFm>(instr);
    else if constexpr (kHandler == 19)
        upperOp19<kFm>(instr);
    else if constexpr (kHandler == 20)
        upperOp20<kFm>(instr);
    else if constexpr (kHandler == 21)
        upperOp21<kFm>(instr);
    else if constexpr (kHandler == 22)
        upperOp22<kFm>(instr);
    else if constexpr (kHandler == 23)
        upperOp23<kFm>(instr);
    else if constexpr (kHandler == 24)
        upperOp24<kFm>(instr);
    else if constexpr (kHandler == 25)
        upperOp25<kFm>(instr);
    else if constexpr (kHandler == 26)
        upperOp26<kFm>(instr);
    else if constexpr (kHandler == 27)
        upperOp27<kFm>(instr);
    else if constexpr (kHandler == 28)
        upperOp28<kFm>(instr);
    else if constexpr (kHandler == 29)
        upperOp29<kFm>(instr);
    else if constexpr (kHandler == 30)
        upperOp30<kFm>(instr);
    else if constexpr (kHandler == 31)
        upperOp31<kFm>(instr);
    else if constexpr (kHandler == 32)
        upperOp32<kFm>(instr);
    else if constexpr (kHandler == 33)
        upperOp33<kFm>(instr);
    else if constexpr (kHandler == 34)
        upperOp34<kFm>(instr);
    else if constexpr (kHandler == 35)
        upperOp35<kFm>(instr);
    else if constexpr (kHandler == 36)
        upperOp36<kFm>(instr);
    else if constexpr (kHandler == 37)
        upperOp37<kFm>(instr);
    else if constexpr (kHandler == 38)
        upperOp38<kFm>(instr);
    else if constexpr (kHandler == 39)
        upperOp39<kFm>(instr);
    else if constexpr (kHandler == 40)
        upperOp40<kFm>(instr);
    else if constexpr (kHandler == 41)
        upperOp41<kFm>(instr);
    else if constexpr (kHandler == 42)
        upperOp42<kFm>(instr);
    else if constexpr (kHandler == 43)
        upperOp43<kFm>(instr);
    else if constexpr (kHandler == 44)
        upperOp44<kFm>(instr);
    else if constexpr (kHandler == 45)
        upperOp45<kFm>(instr);
    else if constexpr (kHandler == 46)
        upperOp46<kFm>(instr);
    else if constexpr (kHandler == 47)
        upperOp47<kFm>(instr);
    else if constexpr (kHandler == 48)
        upperOp48<kFm>(instr);
    else if constexpr (kHandler == 49)
        upperOp49<kFm>(instr);
    else if constexpr (kHandler == 50)
        upperOp50<kFm>(instr);
    else if constexpr (kHandler == 51)
        upperOp51<kFm>(instr);
    else if constexpr (kHandler == 52)
        upperOp52<kFm>(instr);
    else if constexpr (kHandler == 53)
        upperOp53<kFm>(instr);
    else if constexpr (kHandler == 54)
        upperOp54<kFm>(instr);
    else if constexpr (kHandler == 55)
        upperOp55<kFm>(instr);
    else if constexpr (kHandler == 56)
        upperOp56<kFm>(instr);
    else if constexpr (kHandler == 57)
        upperOp57<kFm>(instr);
    else if constexpr (kHandler == 58)
        upperOp58<kFm>(instr);
    else if constexpr (kHandler == 59)
        upperOp59<kFm>(instr);
    else if constexpr (kHandler == 60)
        upperOp60<kFm>(instr);
    else
        static_assert(kHandler < 0, "unknown upper handler");
}

VU1_FORCEINLINE void VU1Core::execUpperInline(uint32_t instr)
{
    (this->*kVu1UpperHandlers[vu1UpperHandler(instr)])(instr);
}
