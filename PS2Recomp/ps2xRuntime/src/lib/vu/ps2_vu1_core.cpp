#include <chrono>
#include "ps2_vu1_core.h"
#include "runtime/gs/ps2_gif_arbiter.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"
#include "ps2_vu1_detail.h"
#include "ps2_vu1_step.inl"

#include <algorithm>
#include <cfenv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <emmintrin.h>
#include <filesystem>
#include <bit>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <ps2_log.h>

// VU1 run recorder: hash of the XGKICK packets of the run being recorded (see PS2X_VU_RECORD).
static bool g_vuRecActive = false;
static uint64_t g_vuRecKickHash = 0;
uint64_t ps2xVuHashBytes(uint64_t h, const void *data, size_t size);

using vu1detail::laneForComponent;
using vu1detail::kFmacNormProductSum;
using vu1detail::kFmacNormProductSumFast;
using vu1detail::kFmacZeroAddSub;
using vu1detail::kFmacZeroMul;

namespace
{
    // normalizeOperand on four lanes (SSE2, same bits): denormal -> signed zero, Inf/NaN -> +/-max.
    inline __m128i normalizeOperands4(__m128i v)
    {
        const __m128i expMask = _mm_set1_epi32(0x7F800000);
        const __m128i sign = _mm_and_si128(v, _mm_set1_epi32(static_cast<int>(0x80000000u)));
        const __m128i exponent = _mm_and_si128(v, expMask);
        const __m128i isZeroExp = _mm_cmpeq_epi32(exponent, _mm_setzero_si128());
        const __m128i isMaxExp = _mm_cmpeq_epi32(exponent, expMask);
        const __m128i special = _mm_or_si128(isZeroExp, isMaxExp);
        const __m128i replacement = _mm_or_si128(sign, _mm_and_si128(isMaxExp, _mm_set1_epi32(0x7F7FFFFF)));
        return _mm_or_si128(_mm_andnot_si128(special, v), _mm_and_si128(special, replacement));
    }

    inline __m128i exponents4(__m128i v)
    {
        return _mm_and_si128(_mm_srli_epi32(v, 23), _mm_set1_epi32(0xFF));
    }

}


#include <atomic>
extern std::atomic<uint64_t> g_gsDrawNanos;
static uint64_t g_vuRunNanos = 0;
static uint32_t g_vuRunPc = 0; static uint32_t g_kickHist[2048] = {};

// PS2X_VU1_BADKICK=<dir>: a ring of the last VU1 runs, and at the first bad XGKICK a dump of VU1 code/data
// memory, the VU1 state, this ring and the VIF1 command ring (ps2_vif1_interpreter.cpp) into <dir>.
void ps2xVif1DiagDump(const char *dir);
static uint32_t g_vuTraceRunIndex = 0;
namespace
{
    struct VuDiagRun
    {
        uint32_t index, startPc, endPc, top, itop;
        uint32_t cycles;
        uint8_t ended, budget, stopped;
        int32_t vi[16];
    };
    constexpr uint32_t kVuDiagRing = 1u << 14;
    struct VuDiag
    {
        std::string dir;
        VuDiagRun runs[kVuDiagRing];
        uint64_t count = 0;
        bool dumped = false;
        uint32_t budgetHits = 0;
    };
    VuDiag *vuDiag()
    {
        static VuDiag *d = []() -> VuDiag *
        {
            const char *e = std::getenv("PS2X_VU1_BADKICK");
            if (!e || !*e)
                return nullptr;
            VuDiag *v = new VuDiag();
            v->dir = e;
            std::error_code ec;
            std::filesystem::create_directories(v->dir, ec);
            return v;
        }();
        return d;
    }
}
uint64_t g_vuPairs = 0;
uint64_t g_vuCompiledPairs = 0; // of those, run by statically recompiled programs
uint64_t ps2xVuCompiledPairsExecuted() { return g_vuCompiledPairs; }
struct VU1CompiledProgram;                                                                                   // ps2_vu1_compiled.h
const std::vector<const VU1CompiledProgram *> &vu1CompiledProgramsAt(uint32_t pairIndex);                  // ps2_vu1_compiled.cpp
bool vu1CompiledProgramMatches(const VU1CompiledProgram &program, const uint8_t *code, uint32_t codeSize); // ps2_vu1_compiled.cpp
void vu1MergeCompiledProgram(const VU1CompiledProgram &program, VU1Core::CompiledPairFn *pairs);         // ps2_vu1_compiled.cpp
void vu1CompiledProgramReach(const VU1CompiledProgram &program, uint64_t *bits);                          // ps2_vu1_compiled.cpp
uint64_t vu1ProgramSignature(const uint8_t *code, uint32_t codeSize, uint32_t startPc, std::vector<uint32_t> *reachable);
// --vu-gen: called with every pc where a VU1 run starts or jumps in without a compiled program.
void (*g_vu1EntryHook)(uint32_t pc) = nullptr;
uint64_t g_vuSteps = 0;
static uint64_t g_vuJumps = 0, g_vuKicks = 0, g_vuKickBytes = 0;
static uint64_t g_vuDecodeRebuilds = 0; // debug: decode-cache rebuilds (PS2X_VU_STATS)

using vu1detail::NextReadySlot;

namespace
{
    inline NextReadySlot &slotFor(const void *owner)
    {
        return static_cast<const VU1Core *>(owner)->sched();
    }
    uint64_t &nextReadyFor(const void *owner)
    {
        return slotFor(owner).next;
    }

    // First free entry of a pipeline array, i.e. the same index a linear "first !valid" scan finds.
    // The slot's valid-bitmask is exact once masksValid is set (every queue sets its bit, every
    // commit clears it); before that, scan the array.
    template <class Array>
    int firstFreeEntry(const Array &entries, uint32_t validMask, bool masksValid)
    {
        if (masksValid)
        {
            constexpr uint32_t count = static_cast<uint32_t>(std::tuple_size<Array>::value);
            constexpr uint32_t all = count >= 32u ? 0xFFFFFFFFu : ((1u << count) - 1u);
            const uint32_t freeBits = ~validMask & all;
            return freeBits ? static_cast<int>(std::countr_zero(freeBits)) : -1;
        }
        for (size_t i = 0; i < entries.size(); ++i)
            if (!entries[i].valid)
                return static_cast<int>(i);
        return -1;
    }

    inline void noteReadyCycle(NextReadySlot &slot, uint64_t readyCycle)
    {
        if (slot.next != 0 && readyCycle < slot.next)
            slot.next = readyCycle;
    }
}

VU1Core::VU1Core(Unit unit)
    : m_unit(unit), m_slot(new NextReadySlot())
{
    reset();
}

VU1Core::~VU1Core()
{
    delete m_slot;
}

void VU1Core::resetScheduler()
{
    m_flagPipeline = {};
    m_fdiv = {};
    m_efu = {};
    m_storePipeline = {};
    m_vfWritePipeline = {};
    m_viWritePipeline = {};
    m_accWritePipeline = {};
    // Reset XGKICK state without zeroing its 64 KB packet buffer (only the copied bytes are ever read).
    m_xgkick.sourceAddress = 0; m_xgkick.totalBytes = 0; m_xgkick.copiedBytes = 0; m_xgkick.currentTagEnd = 0;
    m_xgkick.cycleCredit = 0; m_xgkick.issueCycle = 0; m_xgkick.active = false; m_xgkick.currentTagEop = false;
    m_vfReady = {};
    m_viReady = {};
    m_accReady = {};
    m_readyHorizon = 0;
    m_dynamicPairs = 0;
    m_vfLatestWrite = {};
    m_viLatestWrite = {};
    m_accLatestWrite = {};
    m_nextWriteSequence = 0;
    m_efuResourceReady = 0;
    m_workingClip = m_state.clip;
    m_viBranchBackupValue = 0;
    m_viBranchBackupReg = 0;
    m_viBranchBackupValid = false;
    m_stopRequested = false;
    m_blockBreak = 0u;
    m_normTrust = 0u;
    m_pendingHaltD = false;
    m_pendingHaltT = false;
    {
        NextReadySlot &slot = slotFor(this);
        slot.next = 0;
        slot.regWritesUntil = 0;
        slot.flagMask = slot.efuMask = slot.storeMask = slot.vfMask = slot.viMask = slot.accMask = 0;
        slot.masksValid = true;
    }
}

void VU1Core::reset()
{
    std::memset(&m_state, 0, sizeof(m_state));
    m_state.vf[0][3] = 1.0f;
    m_state.q = 1.0f;
    m_state.r = 0x3F800000u;
    m_cycle = 0;
    resetScheduler();
}

float VU1Core::broadcast(const float *vf, uint8_t bc)
{
    return normalizeOperand(vf[bc & 3u]);
}

float VU1Core::normalizeOperand(float value) const
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t exponent = (bits >> 23) & 0xFFu;
    if (exponent == 0u)
    {
        bits &= 0x80000000u;
    }
    else if (exponent == 0xFFu)
    {
        bits = (bits & 0x80000000u) | 0x7F7FFFFFu;
    }
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

float VU1Core::normalizeResult(float value, uint32_t &laneFlags) const
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t sign = bits & 0x80000000u;
    const uint32_t magnitude = bits & 0x7FFFFFFFu;
    const uint32_t exponent = (bits >> 23) & 0xFFu;

    laneFlags = sign != 0u ? 0x2u : 0u;
    if (magnitude == 0u)
    {
        laneFlags |= 0x1u;
    }
    else if (exponent == 0u)
    {
        laneFlags |= 0x5u;
        bits = sign;
    }
    else if (exponent == 0xFFu)
    {
        laneFlags |= 0x8u;
        bits = sign | 0x7F7FFFFFu;
    }

    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

bool g_vuForceExactFlags = [] { const char *e = std::getenv("PS2X_VU_EXACT_FLAGS"); return e && *e && *e != '0'; }();
bool g_vuNoInlineLower = [] { const char *e = std::getenv("PS2X_VU_NO_INLINE_LOWER"); return e && *e && *e != '0'; }();

void VU1Core::normalizeFmacResult(float *result, uint8_t dest,
                                         uint8_t laneFlags[4])
{
    // Fast path: for ops that are a single rounded add/sub/mul (not product-sum), a float result
    // that is a comfortably normal number already tells the exact result's flags: non-zero, no
    // overflow/underflow, same sign (rounding never flips the sign). Only zero, tiny, huge or
    // non-finite results need the exact recomputation below.
    const uint32_t upper = m_currentUpperInstruction;
    const uint8_t norm = m_currentFmacNorm; // computeFmacNorm(upper)
    const bool productSum = (norm & kFmacNormProductSum) != 0u;
    static const bool forceExact = [] { const char *e = std::getenv("PS2X_VU_EXACT_FLAGS"); return e && *e && *e != '0'; }();
    // Product-sum (MADD/MSUB family, not OPMULA/OPMSUB): acc +/- left*right. Rounding the product
    // and the sum moves the value by at most ~2^(max(exp(product), exp(acc)) - 23); a normal
    // result that is still 2^20 above that bound has the exact result's sign and range.
    const bool productSumFast = (norm & kFmacNormProductSumFast) != 0u && !forceExact;
    const uint8_t fsReg = FS(upper);
    const uint8_t ftReg = FT(upper);
    const bool fastOk = !productSum && !forceExact;

    // The fast checks below for all four lanes at once (same arithmetic, SSE2): bit c of `settled`
    // = component c passes them, so its flags are just its sign. Other lanes take the scalar path.
    uint32_t settled = 0u;
    if (fastOk || productSumFast)
    {
        const __m128i bits = _mm_loadu_si128(reinterpret_cast<const __m128i *>(result));
        const __m128i exponent = exponents4(bits);
        __m128i ok = _mm_and_si128(_mm_cmpgt_epi32(exponent, _mm_set1_epi32(1)),
                                   _mm_cmplt_epi32(exponent, _mm_set1_epi32(254)));
        if (!fastOk)
        {
            const __m128i left = normalizeOperands4(_mm_loadu_si128(reinterpret_cast<const __m128i *>(m_state.vf[fsReg])));
            __m128i right;
            switch ((norm >> 2) & 3u)
            {
            case 1u:
                right = _mm_set1_epi32(static_cast<int>(std::bit_cast<uint32_t>(m_state.vf[ftReg][(norm >> 4) & 3u])));
                break;
            case 2u:
                right = _mm_set1_epi32(static_cast<int>(std::bit_cast<uint32_t>(m_state.q)));
                break;
            case 3u:
                right = _mm_set1_epi32(static_cast<int>(std::bit_cast<uint32_t>(m_state.i)));
                break;
            default:
                right = _mm_loadu_si128(reinterpret_cast<const __m128i *>(m_state.vf[ftReg]));
                break;
            }
            right = normalizeOperands4(right);
            const __m128i accValue = normalizeOperands4(_mm_loadu_si128(reinterpret_cast<const __m128i *>(m_state.acc)));
            const __m128i product = _mm_castps_si128(_mm_mul_ps(_mm_castsi128_ps(left), _mm_castsi128_ps(right)));
            // Exponents are 0..255 (high halves zero), so the 16-bit max is the 32-bit max.
            const __m128i bound = _mm_max_epi16(exponents4(product), exponents4(accValue));
            // exponent >= bound - 20  <=>  exponent + 21 > bound
            ok = _mm_and_si128(ok, _mm_cmpgt_epi32(_mm_add_epi32(exponent, _mm_set1_epi32(21)), bound));
        }
        settled = static_cast<uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(ok)));
    }

    for (uint32_t component = 0; component < 4u; ++component)
    {
        laneFlags[component] = 0u;
        if ((dest & laneForComponent(component)) == 0u)
            continue;
        if ((settled >> component) & 1u)
        {
            laneFlags[component] = (std::bit_cast<uint32_t>(result[component]) & 0x80000000u) ? 0x2u : 0u;
            continue;
        }
        if (const uint8_t zeroKind = m_currentFmacZero & 3u; zeroKind != 0u && !forceExact)
        {
            uint32_t bits;
            std::memcpy(&bits, &result[component], sizeof(bits));
            if ((bits & 0x7FFFFFFFu) == 0u)
            {
                bool exactZero = zeroKind == kFmacZeroAddSub;
                if (!exactZero)
                {
                    float right;
                    switch ((m_currentFmacZero >> 2) & 3u)
                    {
                    case 1u:
                        right = m_state.vf[ftReg][(m_currentFmacZero >> 4) & 3u];
                        break;
                    case 2u:
                        right = m_state.q;
                        break;
                    case 3u:
                        right = m_state.i;
                        break;
                    default:
                        right = m_state.vf[ftReg][component];
                        break;
                    }
                    exactZero = normalizeOperand(m_state.vf[fsReg][component]) == 0.0f || normalizeOperand(right) == 0.0f;
                }
                if (exactZero)
                {
                    laneFlags[component] = static_cast<uint8_t>(0x1u | ((bits & 0x80000000u) ? 0x2u : 0u));
                    continue;
                }
            }
        }

        long double exactResult = 0.0L;
        if (calculateFmacExactResult(component, exactResult))
        {
            laneFlags[component] = normalizeFmacExactResult(result[component], exactResult);
            continue;
        }

        uint32_t flags = 0u;
        result[component] = normalizeResult(result[component], flags);
        laneFlags[component] = static_cast<uint8_t>(flags);
    }
}

bool VU1Core::calculateFmacExactResult(uint32_t component,
                                               long double &resultOut) const
{
    // Products of two floats are exact in double (24+24 bits), and a double sum keeps the sign,
    // zero-ness and float overflow/underflow classification the flags need; long double (x87)
    // was several times slower here.
    auto compute = [&](double &result) -> bool
    {
    const uint32_t upper = m_currentUpperInstruction;
    const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
    const uint8_t special = op >= 0x3Cu
                                ? static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu))
                                : 0xFFu;
    const uint8_t fs = FS(upper);
    const uint8_t ft = FT(upper);

    const auto operand = [this](float value)
    {
        return static_cast<double>(normalizeOperand(value));
    };
    const auto vs = [&](uint32_t lane)
    {
        return operand(m_state.vf[fs][lane]);
    };
    const auto vt = [&](uint32_t lane)
    {
        return operand(m_state.vf[ft][lane]);
    };
    const auto acc = [&](uint32_t lane)
    {
        return operand(m_state.acc[lane]);
    };

    const double q = operand(m_state.q);
    const double i = operand(m_state.i);

    if (op < 0x3Cu)
    {
        if (op <= 0x03u)
            result = vs(component) + vt(op & 3u);
        else if (op <= 0x07u)
            result = vs(component) - vt(op & 3u);
        else if (op <= 0x0Bu)
            result = acc(component) + vs(component) * vt(op & 3u);
        else if (op <= 0x0Fu)
            result = acc(component) - vs(component) * vt(op & 3u);
        else if (op >= 0x18u && op <= 0x1Bu)
            result = vs(component) * vt(op & 3u);
        else
        {
            switch (op)
            {
            case 0x1Cu:
                result = vs(component) * q;
                break;
            case 0x1Eu:
                result = vs(component) * i;
                break;
            case 0x20u:
                result = vs(component) + q;
                break;
            case 0x21u:
                result = acc(component) + vs(component) * q;
                break;
            case 0x22u:
                result = vs(component) + i;
                break;
            case 0x23u:
                result = acc(component) + vs(component) * i;
                break;
            case 0x24u:
                result = vs(component) - q;
                break;
            case 0x25u:
                result = acc(component) - vs(component) * q;
                break;
            case 0x26u:
                result = vs(component) - i;
                break;
            case 0x27u:
                result = acc(component) - vs(component) * i;
                break;
            case 0x28u:
                result = vs(component) + vt(component);
                break;
            case 0x29u:
                result = acc(component) + vs(component) * vt(component);
                break;
            case 0x2Au:
                result = vs(component) * vt(component);
                break;
            case 0x2Cu:
                result = vs(component) - vt(component);
                break;
            case 0x2Du:
                result = acc(component) - vs(component) * vt(component);
                break;
            case 0x2Eu:
            {
                static constexpr uint8_t left[4] = {1u, 2u, 0u, 3u};
                static constexpr uint8_t right[4] = {2u, 0u, 1u, 3u};
                result = component == 3u
                             ? 0.0
                             : acc(component) - vs(left[component]) * vt(right[component]);
                break;
            }
            default:
                return false;
            }
        }
        return true;
    }

    if (special <= 0x03u)
        result = vs(component) + vt(special & 3u);
    else if (special <= 0x07u)
        result = vs(component) - vt(special & 3u);
    else if (special <= 0x0Bu)
        result = acc(component) + vs(component) * vt(special & 3u);
    else if (special <= 0x0Fu)
        result = acc(component) - vs(component) * vt(special & 3u);
    else if (special >= 0x18u && special <= 0x1Bu)
        result = vs(component) * vt(special & 3u);
    else
    {
        switch (special)
        {
        case 0x1Cu:
            result = vs(component) * q;
            break;
        case 0x1Eu:
            result = vs(component) * i;
            break;
        case 0x20u:
            result = vs(component) + q;
            break;
        case 0x21u:
            result = acc(component) + vs(component) * q;
            break;
        case 0x22u:
            result = vs(component) + i;
            break;
        case 0x23u:
            result = acc(component) + vs(component) * i;
            break;
        case 0x24u:
            result = vs(component) - q;
            break;
        case 0x25u:
            result = acc(component) - vs(component) * q;
            break;
        case 0x26u:
            result = vs(component) - i;
            break;
        case 0x27u:
            result = acc(component) - vs(component) * i;
            break;
        case 0x28u:
            result = vs(component) + vt(component);
            break;
        case 0x29u:
            result = acc(component) + vs(component) * vt(component);
            break;
        case 0x2Au:
            result = vs(component) * vt(component);
            break;
        case 0x2Cu:
            result = vs(component) - vt(component);
            break;
        case 0x2Du:
            result = acc(component) - vs(component) * vt(component);
            break;
        case 0x2Eu:
        {
            static constexpr uint8_t left[4] = {1u, 2u, 0u, 3u};
            static constexpr uint8_t right[4] = {2u, 0u, 1u, 3u};
            result = component == 3u
                         ? 0.0
                         : vs(left[component]) * vt(right[component]);
            break;
        }
        default:
            return false;
        }
    }
    return true;
    };
    double result = 0.0;
    const bool handled = compute(result);
    resultOut = static_cast<long double>(result);
    return handled;
}

uint8_t VU1Core::normalizeFmacExactResult(float &value,
                                                  long double exactResult) const
{
    const bool negative = std::signbit(exactResult);
    const long double magnitude = std::fabs(exactResult);
    const long double maximum = static_cast<long double>(std::numeric_limits<float>::max());
    const long double minimum = static_cast<long double>(std::numeric_limits<float>::min());
    uint8_t flags = negative ? 0x2u : 0u;

    uint32_t bits = negative ? 0x80000000u : 0u;
    if (magnitude == 0.0L)
    {
        flags |= 0x1u;
        std::memcpy(&value, &bits, sizeof(value));
    }
    else if (magnitude > maximum)
    {
        flags |= 0x8u;
        bits |= 0x7F7FFFFFu;
        std::memcpy(&value, &bits, sizeof(value));
    }
    else if (magnitude < minimum)
    {
        flags |= 0x5u;
        std::memcpy(&value, &bits, sizeof(value));
    }

    return flags;
}

uint32_t VU1Core::calculateFmacProductSticky(uint8_t dest) const
{
    uint32_t extraSticky = 0u;
    const uint32_t upper = m_currentUpperInstruction;
    const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
    const uint8_t special = op >= 0x3Cu ? static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu)) : 0xFFu;
    const bool productSum =
        (op >= 0x08u && op <= 0x0Fu) ||
        op == 0x21u || op == 0x23u || op == 0x25u || op == 0x27u ||
        op == 0x29u || op == 0x2Du || op == 0x2Eu ||
        (special >= 0x08u && special <= 0x0Fu) ||
        special == 0x21u || special == 0x23u || special == 0x25u ||
        special == 0x27u || special == 0x29u || special == 0x2Du;
    if (!productSum)
        return 0u;

    const uint8_t fs = FS(upper);
    const uint8_t ft = FT(upper);
    for (uint32_t component = 0; component < 4u; ++component)
    {
        if ((dest & laneForComponent(component)) == 0u)
            continue;
        static constexpr uint8_t crossLeft[4] = {1u, 2u, 0u, 3u};
        static constexpr uint8_t crossRight[4] = {2u, 0u, 1u, 3u};
        const uint8_t leftComponent = op == 0x2Eu ? crossLeft[component] : static_cast<uint8_t>(component);
        const float left = normalizeOperand(m_state.vf[fs][leftComponent]);
        float right = 0.0f;
        if ((op >= 0x08u && op <= 0x0Fu) || (special >= 0x08u && special <= 0x0Fu))
        {
            right = normalizeOperand(m_state.vf[ft][(op >= 0x08u && op <= 0x0Fu ? op : special) & 3u]);
        }
        else if (op == 0x21u || op == 0x25u || special == 0x21u || special == 0x25u)
        {
            right = normalizeOperand(m_state.q);
        }
        else if (op == 0x23u || op == 0x27u || special == 0x23u || special == 0x27u)
        {
            right = normalizeOperand(m_state.i);
        }
        else if (op == 0x2Eu)
        {
            right = normalizeOperand(m_state.vf[ft][crossRight[component]]);
        }
        else
        {
            right = normalizeOperand(m_state.vf[ft][component]);
        }

        float product = left * right;
        {
            // A comfortably normal float product means the exact product is non-zero, in range and
            // has the same sign, so its flags are just S.
            static const bool forceExact = [] { const char *e = std::getenv("PS2X_VU_EXACT_FLAGS"); return e && *e && *e != '0'; }();
            uint32_t pbits;
            std::memcpy(&pbits, &product, sizeof(pbits));
            const uint32_t pexp = (pbits >> 23) & 0xFFu;
            if (!forceExact && pexp >= 2u && pexp <= 253u)
            {
                extraSticky |= (pbits & 0x80000000u) ? 0x2u : 0u;
                continue;
            }
        }
        const long double exactProduct = static_cast<long double>(static_cast<double>(left) * static_cast<double>(right)); // exact in double
        const uint8_t productFlags = normalizeFmacExactResult(product, exactProduct);
        // Product-sum instructions report Z/S/U/O from the add/subtract result
        // as current flags, while every product condition accumulates into the
        // corresponding sticky flag.
        extraSticky |= productFlags & 0xFu;
    }
    return extraSticky;
}

void VU1Core::updateFmacFlags(const uint8_t laneFlags[4], uint8_t dest,
                                     uint32_t extraSticky)
{
    if (dest == 0u)
        return;

    uint32_t mac = 0u;
    uint32_t status = 0u;
    for (uint32_t component = 0; component < 4u; ++component)
    {
        const uint8_t lane = laneForComponent(component);
        if ((dest & lane) == 0u)
            continue;

        const uint32_t flags = laneFlags[component];
        if ((flags & 0x1u) != 0u)
            mac |= lane;
        if ((flags & 0x2u) != 0u)
            mac |= static_cast<uint32_t>(lane) << 4;
        if ((flags & 0x4u) != 0u)
            mac |= static_cast<uint32_t>(lane) << 8;
        if ((flags & 0x8u) != 0u)
            mac |= static_cast<uint32_t>(lane) << 12;
        status |= flags;
    }

    NextReadySlot &slot = slotFor(this);
    const int index = firstFreeEntry(m_flagPipeline, slot.flagMask, slot.masksValid);
    if (index < 0)
    {
        reportReservedInstruction(true, 0xFFFFFFFFu);
        return;
    }
    FlagPipelineEntry *entry = &m_flagPipeline[static_cast<size_t>(index)];

    *entry = {};
    entry->valid = true;
    entry->issueCycle = m_cycle;
    entry->readyCycle = m_cycle + kFmacLatency;
    noteReadyCycle(slot, entry->readyCycle);
    slot.flagMask |= 1u << static_cast<uint32_t>(index);
    entry->mac = mac;
    entry->status = status;
    entry->extraSticky = extraSticky;
    entry->writesMac = true;
    entry->writesStatus = true;
}

namespace
{
    inline bool lowerReadsFlags(uint32_t lower)
    {
        switch (lower >> 25)
        {
        case 0x14: case 0x16: case 0x17: // FSEQ, FSAND, FSOR
        case 0x18: case 0x1A: case 0x1B: // FMEQ, FMAND, FMOR
            return true;
        default:
            return false;
        }
    }

    // Walks the microcode reachable from startPair (branch targets, fall-through, delay slots,
    // stopping after an E-bit pair's delay slot) and reports whether any of it reads flags.
    // JR/JALR are treated as subroutine returns: they continue at the return address of every
    // BAL/JALR seen. Over-approximating is safe (it only costs speed); a missed path is caught at
    // run time by ps2xVuNoteFlagRead.
    template <class Pair>
    bool programReadsFlags(const Pair *cache, uint32_t pairs, uint32_t startPair)
    {
        std::vector<uint8_t> seen(pairs, 0u);
        std::vector<std::pair<uint32_t, bool>> work; // (pair index, no fall-through after it)
        std::vector<uint32_t> returnPairs;
        bool sawJump = false;
        auto push = [&](uint32_t index, bool noFallthrough) { work.push_back({index % pairs, noFallthrough}); };
        push(startPair, false);
        for (;;)
        {
            while (!work.empty())
            {
                const auto [index, noFallthrough] = work.back();
                work.pop_back();
                // 1 = visited as a delay slot only (no fall-through), 2 = visited with fall-through.
                if (seen[index] == 2u || (seen[index] == 1u && noFallthrough))
                    continue;
                seen[index] = noFallthrough ? 1u : 2u;
                const Pair &pair = cache[index];
                bool fallthrough = !noFallthrough;
                if (!pair.iBit)
                {
                    const uint32_t lower = pair.lower;
                    if (lowerReadsFlags(lower))
                        return true;
                    int32_t imm = static_cast<int32_t>(lower & 0x7FFu);
                    if (imm & 0x400)
                        imm -= 0x800;
                    const uint32_t target = static_cast<uint32_t>(static_cast<int32_t>(index + 1u) + imm + static_cast<int32_t>(pairs)) % pairs;
                    switch (lower >> 25)
                    {
                    case 0x20: // B
                    case 0x21: // BAL
                        if ((lower >> 25) == 0x21u)
                            returnPairs.push_back(index + 2u);
                        push(index + 1u, true); // delay slot, then the target
                        push(target, false);
                        fallthrough = false;
                        break;
                    case 0x28: case 0x29: case 0x2C: case 0x2D: case 0x2E: case 0x2F: // IBxx
                        push(target, false);
                        break;
                    case 0x24: // JR
                    case 0x25: // JALR
                        if ((lower >> 25) == 0x25u)
                            returnPairs.push_back(index + 2u);
                        sawJump = true;
                        push(index + 1u, true);
                        fallthrough = false;
                        break;
                    default:
                        break;
                    }
                }
                if (pair.eBit)
                {
                    push(index + 1u, true); // the delay slot runs, then the program ends
                    fallthrough = false;
                }
                if (fallthrough)
                    push(index + 1u, false);
            }
            if (!sawJump)
                return false;
            bool added = false;
            for (uint32_t r : returnPairs)
                if (seen[r % pairs] != 2u)
                {
                    push(r, false);
                    added = true;
                }
            if (!added)
                return false;
        }
    }

    // Whether the loaded VU1 microcode can observe MAC/status flags (FSEQ/FSAND/FSOR/FMEQ/
    // FMAND/FMOR). If none of those exist, FMAC results only need value normalization and the
    // costly exact-flag computation can be skipped. PS2X_VU_EXACT_FLAGS=1 always computes them.
    template <class Pair>
    bool fmacFlagsObservableImpl(const void *self, bool isVu1, bool cacheValid, uint64_t generation,
                                 const void *code, uint32_t codeSize, const Pair *cache, uint32_t maxPairs)
    {
        static const bool forceExact = [] { const char *e = std::getenv("PS2X_VU_EXACT_FLAGS"); return e && *e && *e != '0'; }();
        if (forceExact || !isVu1 || !cacheValid)
            return true;
        NextReadySlot &slot = slotFor(self);
        if (slot.flagsNeeded < 0 || slot.flagsGeneration != generation || slot.flagsCode != code || slot.flagsCodeSize != codeSize)
        {
            bool needed = false;
            bool statusNeeded = false;
            const uint32_t pairs = std::min<uint32_t>(codeSize / 8u, maxPairs);
            for (uint32_t i = 0; i < pairs && !statusNeeded; ++i)
            {
                const Pair &pair = cache[i];
                if (pair.iBit)
                    continue;
                switch (pair.lower >> 25)
                {
                case 0x14: case 0x16: case 0x17:
                    statusNeeded = true;
                    needed = true;
                    break;
                case 0x18: case 0x1A: case 0x1B:
                    needed = true;
                    break;
                default:
                    break;
                }
            }
            slot.flagsNeeded = needed ? 1 : 0;
            slot.statusNeeded = statusNeeded ? 1 : 0;
            slot.flagsGeneration = generation;
            slot.flagsCode = code;
            slot.flagsCodeSize = codeSize;
        }
        if (slot.flagsNeeded == 0)
            return false;

        // PS2X_VU_FAST_FLAGS=1 (experimental, off by default): some program in code memory reads
        // flags, but skip exact flags for runs whose reachable code reads none. Not safe in
        // general: MAC/status survive between runs, so a later program can read flags a skipped
        // run should have set. In Sims 2 that intermittently corrupted game state (crash to pc 0x10).
        static const bool perRunFlags = [] { const char *e = std::getenv("PS2X_VU_FAST_FLAGS"); return e && *e && *e != '0'; }();
        if (!perRunFlags)
            return true;
        if (slot.runFlagsNeeded < 0)
        {
            if (slot.perStartGeneration != generation)
            {
                slot.perStartNeeded.clear();
                slot.forcedExact.clear();
                slot.perStartGeneration = generation;
            }
            const uint32_t pairs = std::min<uint32_t>(codeSize / 8u, maxPairs);
            const uint32_t startPair = slot.runStartPc / 8u;
            if (slot.forcedExact.count(slot.runStartPc) != 0u || pairs == 0u)
            {
                slot.runFlagsNeeded = 1;
            }
            else if (auto it = slot.perStartNeeded.find(slot.runStartPc); it != slot.perStartNeeded.end())
            {
                slot.runFlagsNeeded = it->second ? 1 : 0;
            }
            else
            {
                const bool needed = programReadsFlags(cache, pairs, startPair);
                slot.perStartNeeded.emplace(slot.runStartPc, needed);
                static const bool trace = [] { const char *e = std::getenv("PS2X_VU_FLAGTRACE"); return e && *e && *e != '0'; }();
                if (trace)
                    std::fprintf(stderr, "[vu1] program at pc=0x%x %s flags\n", slot.runStartPc, needed ? "reads" : "does not read");
                slot.runFlagsNeeded = needed ? 1 : 0;
            }
        }
        return slot.runFlagsNeeded != 0;
    }
}

// A program run begins at the current pc (MSCAL start or MSCNT continuation).
static void beginProgramRun(const void *self, uint32_t startPc)
{
    NextReadySlot &slot = slotFor(self);
    slot.runStartPc = startPc;
    slot.runFlagsNeeded = -1;
}

// Called by the flag-reading lower instructions. If it happens in a run the reachability walk
// called flag-free, the walk missed a path: use exact flags for that start pc from now on.
void ps2xVuNoteFlagRead(const void *self)
{
    NextReadySlot &slot = slotFor(self);
    if (slot.runFlagsNeeded != 0)
        return;
    slot.forcedExact.insert(slot.runStartPc);
    slot.runFlagsNeeded = 1;
    slot.flagsObservableRun = true;
    static uint32_t warnings = 0;
    if (++warnings <= 8u)
        std::fprintf(stderr, "[vu1] flag read in a run marked flag-free (start pc=0x%x); using exact flags for it\n", slot.runStartPc);
}

// Status (with its sticky bits) can only be read by FSEQ/FSAND/FSOR. Valid after a
// VU1_FMAC_FLAGS_OBSERVABLE() check for the same code; unknown (-1) counts as observable.
static bool vuStatusObservable(const void *self, bool isVu1)
{
    static const bool forceExact = [] { const char *e = std::getenv("PS2X_VU_EXACT_FLAGS"); return e && *e && *e != '0'; }();
    return forceExact || !isVu1 || slotFor(self).statusNeeded != 0;
}

#define VU1_FMAC_FLAGS_OBSERVABLE() (m_slot->flagsObservableRun)
#define VU1_FMAC_FLAGS_OBSERVABLE_EVAL() \
    fmacFlagsObservableImpl(this, m_unit == Unit::VU1, m_decodedCodeCacheValid, m_cachedCodeGeneration, \
                            m_cachedVuCode, m_cachedCodeSize, m_decodedCodeCache, kMaxDecodedPairs)

// Whether the dest lanes of v are normalized (normalizeLanes4 keeps them: exponent 1..254, or +/-0).
static bool vuLanesNormalized(const float *v, uint8_t dest)
{
    for (uint32_t component = 0; component < 4u; ++component)
    {
        if ((dest & laneForComponent(component)) == 0u)
            continue;
        const uint32_t bits = std::bit_cast<uint32_t>(v[component]);
        const uint32_t exponent = (bits >> 23) & 0xFFu;
        if (exponent == 0xFFu || (exponent == 0u && (bits & 0x7FFFFFFFu) != 0u))
            return false;
    }
    return true;
}

void VU1Core::applyFmacDest(float *dst, float *result, uint8_t dest)
{
    applyFmacDestExact(dst, result, dest);
    // Generated blocks count FMAC results as normalized (runBlockFrom); the two roundings of a
    // product-sum can leave a denormal here, so that block must stop trusting them.
    if (!vuLanesNormalized(dst, dest))
    {
        m_blockBreak |= kBlockBreakNorm;
        m_normTrust = 0u;
    }
}

void VU1Core::applyFmacDestExact(float *dst, float *result, uint8_t dest)
{
    if (!VU1_FMAC_FLAGS_OBSERVABLE())
    {
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((dest & laneForComponent(component)) == 0u)
                continue;
            uint32_t ignored = 0u;
            result[component] = normalizeResult(result[component], ignored);
        }
        applyDest(dst, result, dest);
        return;
    }
    uint8_t laneFlags[4]{};
    normalizeFmacResult(result, dest, laneFlags); // exact values either way
    if (m_currentFlagsLive || m_unit != Unit::VU1)
    {
        // Product sticky flags only feed status; skip them when nothing can read status.
        updateFmacFlags(laneFlags, dest, vuStatusObservable(this, m_unit == Unit::VU1) ? calculateFmacProductSticky(dest) : 0u);
    }
    applyDest(dst, result, dest);
}

void VU1Core::applyFmacDestAcc(float *result, uint8_t dest)
{
    applyFmacDestAccExact(result, dest);
    if (!vuLanesNormalized(m_state.acc, dest))
    {
        m_blockBreak |= kBlockBreakNorm;
        m_normTrust = 0u;
    }
}

void VU1Core::applyFmacDestAccExact(float *result, uint8_t dest)
{
    if (!VU1_FMAC_FLAGS_OBSERVABLE())
    {
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((dest & laneForComponent(component)) == 0u)
                continue;
            uint32_t ignored = 0u;
            result[component] = normalizeResult(result[component], ignored);
        }
        applyDestAcc(result, dest);
        return;
    }
    uint8_t laneFlags[4]{};
    normalizeFmacResult(result, dest, laneFlags); // exact values either way
    if (m_currentFlagsLive || m_unit != Unit::VU1)
    {
        // Product sticky flags only feed status; skip them when nothing can read status.
        updateFmacFlags(laneFlags, dest, vuStatusObservable(this, m_unit == Unit::VU1) ? calculateFmacProductSticky(dest) : 0u);
    }
    applyDestAcc(result, dest);
}

void VU1Core::queueFsset(uint16_t immediate)
{
    for (FlagPipelineEntry &entry : m_flagPipeline)
    {
        if (entry.valid && entry.issueCycle == m_cycle)
            entry.writesStatus = false;
    }

    for (FlagPipelineEntry &entry : m_flagPipeline)
    {
        if (!entry.valid)
        {
            entry = {};
            entry.valid = true;
            entry.issueCycle = m_cycle;
            entry.readyCycle = m_cycle + kFmacLatency;
            { uint64_t &nr = nextReadyFor(this); const uint64_t rc = entry.readyCycle; if (nr != 0 && rc < nr) nr = rc; }
            slotFor(this).flagMask |= 1u << static_cast<uint32_t>(&entry - m_flagPipeline.data());
            entry.status = static_cast<uint32_t>(immediate) & 0xFC0u;
            entry.writesSticky = true;
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFFEu);
}

void VU1Core::queueClip(uint32_t clip)
{
    m_workingClip = ((m_workingClip << 6) | (clip & 0x3Fu)) & 0xFFFFFFu;
    for (FlagPipelineEntry &entry : m_flagPipeline)
    {
        if (!entry.valid)
        {
            entry = {};
            entry.valid = true;
            entry.issueCycle = m_cycle;
            entry.readyCycle = m_cycle + kFmacLatency;
            { uint64_t &nr = nextReadyFor(this); const uint64_t rc = entry.readyCycle; if (nr != 0 && rc < nr) nr = rc; }
            slotFor(this).flagMask |= 1u << static_cast<uint32_t>(&entry - m_flagPipeline.data());
            entry.clip = m_workingClip;
            entry.writesClip = true;
            return;
        }
    }
    reportReservedInstruction(true, 0xFFFFFFFDu);
}

void VU1Core::queueFcset(uint32_t clip)
{
    m_workingClip = clip & 0xFFFFFFu;
    for (FlagPipelineEntry &entry : m_flagPipeline)
    {
        if (entry.valid && entry.issueCycle == m_cycle)
            entry.writesClip = false;
    }
    for (FlagPipelineEntry &entry : m_flagPipeline)
    {
        if (!entry.valid)
        {
            entry = {};
            entry.valid = true;
            entry.issueCycle = m_cycle;
            entry.readyCycle = m_cycle + kFmacLatency;
            { uint64_t &nr = nextReadyFor(this); const uint64_t rc = entry.readyCycle; if (nr != 0 && rc < nr) nr = rc; }
            slotFor(this).flagMask |= 1u << static_cast<uint32_t>(&entry - m_flagPipeline.data());
            entry.clip = m_workingClip;
            entry.writesClip = true;
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFFAu);
}

void VU1Core::queueQ(float value, uint32_t latency, uint32_t statusDi)
{
    uint32_t ignoredFlags = 0u;
    value = normalizeResult(value, ignoredFlags);
    m_fdiv.valid = true;
    m_fdiv.readyCycle = m_cycle + latency;
    { uint64_t &nr = nextReadyFor(this); const uint64_t rc = m_fdiv.readyCycle; if (nr != 0 && rc < nr) nr = rc; }
    m_fdiv.value = value;
    m_fdiv.statusDi = statusDi & 0x30u;
}

void VU1Core::queueP(float value, uint32_t latency)
{
    uint32_t ignoredFlags = 0u;
    value = normalizeResult(value, ignoredFlags);
    for (ScalarPipelineEntry &entry : m_efu)
    {
        if (!entry.valid)
        {
            entry.valid = true;
            entry.readyCycle = m_cycle + latency;
            { uint64_t &nr = nextReadyFor(this); const uint64_t rc = entry.readyCycle; if (nr != 0 && rc < nr) nr = rc; }
            slotFor(this).efuMask |= 1u << static_cast<uint32_t>(&entry - m_efu.data());
            entry.value = value;
            // EFU throughput is one cycle shorter than result visibility.
            m_efuResourceReady = m_cycle + (latency > 0u ? latency - 1u : 0u);
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFF9u);
}

void VU1Core::queueStore(uint32_t address, const uint32_t words[4], uint8_t laneMask)
{
    for (PendingStore &store : m_storePipeline)
    {
        if (!store.valid)
        {
            store.valid = true;
            store.readyCycle = m_cycle + 1u;
            { uint64_t &nr = nextReadyFor(this); const uint64_t rc = store.readyCycle; if (nr != 0 && rc < nr) nr = rc; }
            slotFor(this).storeMask |= 1u << static_cast<uint32_t>(&store - m_storePipeline.data());
            store.address = address;
            store.laneMask = laneMask;
            std::copy(words, words + 4, store.words.begin());
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFFCu);
}

void VU1Core::queueVfWrite(uint8_t reg, uint8_t laneMask,
                                  const float value[4], uint32_t latency)
{
    if (reg == 0u || laneMask == 0u)
        return;
    NextReadySlot &slot = slotFor(this);
    const int index = firstFreeEntry(m_vfWritePipeline, slot.vfMask, slot.masksValid);
    if (index < 0)
    {
        reportReservedInstruction(false, 0xFFFFFFF7u);
        return;
    }
    PendingVfWrite &write = m_vfWritePipeline[static_cast<size_t>(index)];
    write = {};
    write.valid = true;
    write.readyCycle = m_cycle + latency;
    noteReadyCycle(slot, write.readyCycle);
    slot.vfMask |= 1u << static_cast<uint32_t>(index);
    write.sequence = ++m_nextWriteSequence;
    write.reg = reg;
    write.laneMask = laneMask;
    std::copy(value, value + 4, write.value.begin());
    for (uint32_t component = 0; component < 4u; ++component)
    {
        if ((laneMask & laneForComponent(component)) != 0u)
            m_vfLatestWrite[reg][component] = write.sequence;
    }
}

void VU1Core::queueViWrite(uint8_t reg, int32_t value, uint32_t latency)
{
    if (reg == 0u)
        return;
    NextReadySlot &slot = slotFor(this);
    const int index = firstFreeEntry(m_viWritePipeline, slot.viMask, slot.masksValid);
    if (index < 0)
    {
        reportReservedInstruction(false, 0xFFFFFFF6u);
        return;
    }
    PendingViWrite &write = m_viWritePipeline[static_cast<size_t>(index)];
    write = {};
    write.valid = true;
    write.readyCycle = m_cycle + latency;
    noteReadyCycle(slot, write.readyCycle);
    slot.viMask |= 1u << static_cast<uint32_t>(index);
    write.sequence = ++m_nextWriteSequence;
    write.reg = reg;
    write.value = value;
    m_viLatestWrite[reg] = write.sequence;
}

void VU1Core::queueAccWrite(uint8_t laneMask, const float value[4], uint32_t latency)
{
    if (laneMask == 0u)
        return;
    NextReadySlot &slot = slotFor(this);
    const int index = firstFreeEntry(m_accWritePipeline, slot.accMask, slot.masksValid);
    if (index < 0)
    {
        reportReservedInstruction(true, 0xFFFFFFF5u);
        return;
    }
    PendingAccWrite &write = m_accWritePipeline[static_cast<size_t>(index)];
    write = {};
    write.valid = true;
    write.readyCycle = m_cycle + latency;
    noteReadyCycle(slot, write.readyCycle);
    slot.accMask |= 1u << static_cast<uint32_t>(index);
    write.sequence = ++m_nextWriteSequence;
    write.laneMask = laneMask;
    std::copy(value, value + 4, write.value.begin());
    for (uint32_t component = 0; component < 4u; ++component)
    {
        if ((laneMask & laneForComponent(component)) != 0u)
            m_accLatestWrite[component] = write.sequence;
    }
}

void VU1Core::commitFlagPipeline()
{
    commitFlagsOnly();
}

void VU1Core::commitReadyPipelines()
{
    NextReadySlot &slot = slotFor(this);
    if (slot.next != 0 && m_cycle < slot.next)
        return;
    if (!slot.masksValid)
    {
        auto build = [](const auto &arr) {
            uint32_t m = 0;
            for (size_t i = 0; i < arr.size(); ++i)
                if (arr[i].valid) m |= 1u << i;
            return m;
        };
        slot.flagMask = build(m_flagPipeline);
        slot.efuMask = build(m_efu);
        slot.storeMask = build(m_storePipeline);
        slot.vfMask = build(m_vfWritePipeline);
        slot.viMask = build(m_viWritePipeline);
        slot.accMask = build(m_accWritePipeline);
        slot.masksValid = true;
    }

    // One pass per pipeline over the valid entries only (bitmasks, ascending index = the
    // original array order): commit what is due, remember the earliest still pending.
    uint64_t earliest = std::numeric_limits<uint64_t>::max();
    const uint64_t now = m_cycle;

    auto commitFlags = [&]() {
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
            {
                m_state.status = (m_state.status & 0x03Fu) | (entry.status & 0xFC0u);
            }
            if (entry.writesClip)
                m_state.clip = entry.clip;
            entry.valid = false;
            slot.flagMask &= ~(1u << i);
        }
    };
    // Most commits find only flag entries pending (FMAC flags, latency 4): skip the other pipelines.
    if (!m_fdiv.valid && (slot.efuMask | slot.storeMask | slot.vfMask | slot.viMask | slot.accMask) == 0u)
    {
        commitFlags();
        slot.next = earliest;
        return;
    }
    commitFlags();

    if (m_fdiv.valid)
    {
        if (m_fdiv.readyCycle <= now)
        {
            m_state.q = m_fdiv.value;
            const uint32_t currentDi = m_fdiv.statusDi & 0x30u;
            m_state.status = (m_state.status & 0xFCFu) | currentDi | (currentDi << 6);
            m_fdiv = {};
        }
        else
            earliest = std::min(earliest, m_fdiv.readyCycle);
    }

    for (uint32_t bits = slot.efuMask; bits != 0u; bits &= bits - 1u)
    {
        const uint32_t i = static_cast<uint32_t>(std::countr_zero(bits));
        ScalarPipelineEntry &entry = m_efu[i];
        if (entry.readyCycle <= now)
        {
            m_state.p = entry.value;
            entry.valid = false;
            slot.efuMask &= ~(1u << i);
        }
        else
            earliest = std::min(earliest, entry.readyCycle);
    }

    for (uint32_t bits = slot.storeMask; bits != 0u; bits &= bits - 1u)
    {
        const uint32_t i = static_cast<uint32_t>(std::countr_zero(bits));
        PendingStore &store = m_storePipeline[i];
        if (store.readyCycle > now)
        {
            earliest = std::min(earliest, store.readyCycle);
            continue;
        }
        if (m_activeVuData && store.address + 16u <= m_activeVuDataSize)
        {
            uint32_t oldWords[4]{};
            std::memcpy(oldWords, m_activeVuData + store.address, sizeof(oldWords));
            for (uint32_t component = 0; component < 4u; ++component)
            {
                if ((store.laneMask & laneForComponent(component)) != 0u)
                    oldWords[component] = store.words[component];
            }
            std::memcpy(m_activeVuData + store.address, oldWords, sizeof(oldWords));
        }
        store.valid = false;
        slot.storeMask &= ~(1u << i);
    }

    for (uint32_t bits = slot.vfMask; bits != 0u; bits &= bits - 1u)
    {
        const uint32_t i = static_cast<uint32_t>(std::countr_zero(bits));
        PendingVfWrite &write = m_vfWritePipeline[i];
        if (write.readyCycle > now)
        {
            earliest = std::min(earliest, write.readyCycle);
            continue;
        }
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((write.laneMask & laneForComponent(component)) != 0u &&
                m_vfLatestWrite[write.reg][component] == write.sequence)
            {
                m_state.vf[write.reg][component] = write.value[component];
            }
        }
        write.valid = false;
        slot.vfMask &= ~(1u << i);
    }

    for (uint32_t bits = slot.viMask; bits != 0u; bits &= bits - 1u)
    {
        const uint32_t i = static_cast<uint32_t>(std::countr_zero(bits));
        PendingViWrite &write = m_viWritePipeline[i];
        if (write.readyCycle > now)
        {
            earliest = std::min(earliest, write.readyCycle);
            continue;
        }
        if (m_viLatestWrite[write.reg] == write.sequence)
            m_state.vi[write.reg] = static_cast<int16_t>(write.value);
        write.valid = false;
        slot.viMask &= ~(1u << i);
    }

    for (uint32_t bits = slot.accMask; bits != 0u; bits &= bits - 1u)
    {
        const uint32_t i = static_cast<uint32_t>(std::countr_zero(bits));
        PendingAccWrite &write = m_accWritePipeline[i];
        if (write.readyCycle > now)
        {
            earliest = std::min(earliest, write.readyCycle);
            continue;
        }
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((write.laneMask & laneForComponent(component)) != 0u &&
                m_accLatestWrite[component] == write.sequence)
            {
                m_state.acc[component] = write.value[component];
            }
        }
        write.valid = false;
        slot.accMask &= ~(1u << i);
    }

    slot.next = earliest;
}


// PS2X_VU_FIND_TBP=<tbp> (+PS2X_VU_FIND_AFTER=<sec>): log each kick for 0.5 s after <sec>, and dump the
// input of the first VU1 run whose kicked packet sets TEX0 with that tbp0 to vu1_run_find.bin.
static std::vector<uint8_t> g_findSnap;
static int g_findTbp = -2; static double g_findAfter = 0.0; static bool g_findDone = false;
static int g_findNext = 0; // runs after the hit still to dump (vu1_run_find_<n>.bin), e.g. the geometry runs after a state kick
static std::chrono::steady_clock::time_point g_findT0;
static bool findActive()
{
    if (g_findTbp == -2)
    {
        const char *e = std::getenv("PS2X_VU_FIND_TBP"); g_findTbp = e ? std::atoi(e) : -1;
        const char *a = std::getenv("PS2X_VU_FIND_AFTER"); g_findAfter = a ? std::atof(a) : 0.0;
        g_findT0 = std::chrono::steady_clock::now();
    }
    return g_findTbp >= 0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - g_findT0).count() >= g_findAfter;
}
static void findScanKick(const uint8_t *p, uint32_t n, uint32_t pc)
{
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_findT0).count();
    const bool logIt = t < g_findAfter + 0.5;
    uint32_t off = 0; int tbpHit = -1; char buf[512]; int bl = 0;
    bl += std::snprintf(buf + bl, sizeof(buf) - bl, "[vufind] pc=%04x bytes=%u", pc, n);
    while (off + 16 <= n)
    {
        uint64_t lo, hi; std::memcpy(&lo, p + off, 8); std::memcpy(&hi, p + off + 8, 8); off += 16;
        const uint32_t nloop = lo & 0x7fff, pre = (lo >> 46) & 1, prim = (lo >> 47) & 0x7ff, flg = (lo >> 58) & 3; uint32_t nreg = (lo >> 60) & 15; if (!nreg) nreg = 16;
        if (bl < 400) bl += std::snprintf(buf + bl, sizeof(buf) - bl, " | nl=%u pre=%u prim=%x flg=%u", nloop, pre, prim, flg);
        if (flg == 0)
        {
            bool rgbaShown = false;
            for (uint32_t i = 0; i < nloop; ++i)
                for (uint32_t r = 0; r < nreg; ++r)
                {
                    const uint32_t reg = (hi >> (4 * r)) & 15; const uint8_t *q = p + off + (i * nreg + r) * 16;
                    if (q + 16 > p + n) break;
                    uint64_t d0, d1; std::memcpy(&d0, q, 8); std::memcpy(&d1, q + 8, 8);
                    uint64_t tex = 0; bool isTex = false;
                    if (reg == 6 || reg == 7) { tex = d0; isTex = true; }
                    if (reg == 0xE && ((d1 & 0xff) == 6 || (d1 & 0xff) == 7)) { tex = d0; isTex = true; }
                    if (isTex) { const int tbp = (int)(tex & 0x3fff); if (bl < 440) bl += std::snprintf(buf + bl, sizeof(buf) - bl, " tbp=%d", tbp); if (tbp == g_findTbp) tbpHit = tbp; }
                    if (reg == 1 && !rgbaShown) { rgbaShown = true; const uint32_t *w = (const uint32_t *)q; if (bl < 470) bl += std::snprintf(buf + bl, sizeof(buf) - bl, " rgba=%u,%u,%u,%u", w[0]&255, w[1]&255, w[2]&255, w[3]&255); }
                }
            off += nloop * nreg * 16;
        }
        else if (flg == 1) off += ((nloop * nreg + 1) & ~1u) * 8; else off += nloop * 16;
        if ((lo >> 15) & 1) break;
    }
    if (logIt) std::fprintf(stderr, "%s\n", buf);
    if (tbpHit >= 0 && !g_findDone && !g_findSnap.empty())
    {
        g_findDone = true;
        if (const char *e = std::getenv("PS2X_VU_FIND_NEXT"))
            g_findNext = std::atoi(e);
        if (FILE *f = std::fopen("vu1_run_find.bin", "wb")) { std::fwrite(g_findSnap.data(), 1, g_findSnap.size(), f); std::fclose(f); }
        std::fprintf(stderr, "[vufind] dumped run pc=%04x for tbp %d\n", pc, tbpHit);
    }
}

void VU1Core::progressXgkick()
{
    if (!m_xgkick.active || !m_activeVuData || m_activeVuDataSize == 0u)
        return;

    ++m_xgkick.cycleCredit;
    while (m_xgkick.active && m_xgkick.cycleCredit >= 2u)
    {
        m_xgkick.cycleCredit -= 2u;
        if (m_xgkick.copiedBytes > XgkickPipeline::kBufferSize - 16u)
        {
            badKickDiag("packet over 64 KB", 0);
            reportReservedInstruction(false, 0xFFFFFFFBu);
            m_xgkick.active = false;
            return;
        }

        const uint32_t qwordOffset = m_xgkick.copiedBytes;
        // VU1 data memory is 16 KB: a mask instead of the division (every second cycle of a kick).
        const uint32_t sourceOffset = m_xgkick.sourceAddress + m_xgkick.copiedBytes;
        const uint32_t firstSource = (m_activeVuDataSize & (m_activeVuDataSize - 1u)) == 0u
                                         ? sourceOffset & (m_activeVuDataSize - 1u)
                                         : sourceOffset % m_activeVuDataSize;
        if (firstSource + 16u <= m_activeVuDataSize)
            std::memcpy(m_xgkick.packet.data() + m_xgkick.copiedBytes, m_activeVuData + firstSource, 16u);
        else
            for (uint32_t i = 0; i < 16u; ++i)
            {
                const uint32_t source = (m_xgkick.sourceAddress + m_xgkick.copiedBytes + i) % m_activeVuDataSize;
                m_xgkick.packet[m_xgkick.copiedBytes + i] = m_activeVuData[source];
            }
        m_xgkick.copiedBytes += 16u;

        if (m_xgkick.currentTagEnd == 0u)
        {
            uint64_t tagLo = 0;
            std::memcpy(&tagLo, m_xgkick.packet.data() + qwordOffset, sizeof(tagLo));
            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint32_t format = static_cast<uint32_t>((tagLo >> 58) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;

            uint64_t tagBytes = 16u;
            if (format == 0u)
                tagBytes += static_cast<uint64_t>(nloop) * nreg * 16u;
            else if (format == 1u)
                tagBytes += ((static_cast<uint64_t>(nloop) * nreg + 1u) & ~1ull) * 8u;
            else if (format == 2u)
                tagBytes += static_cast<uint64_t>(nloop) * 16u;
            else
            {
                std::fprintf(stderr, "[xgkick] bad tag at offset %u (src 0x%x): %016llx\n", qwordOffset, m_xgkick.sourceAddress, (unsigned long long)tagLo);
                badKickDiag("FLG=3 tag", tagLo);
                reportReservedInstruction(false, 0xFFFFFFF8u);
                m_xgkick.active = false;
                return;
            }

            if (tagBytes > XgkickPipeline::kBufferSize - qwordOffset)
            {
                badKickDiag("tag past 64 KB", tagLo);
                reportReservedInstruction(false, 0xFFFFFFFBu);
                m_xgkick.active = false;
                return;
            }
            m_xgkick.currentTagEnd = qwordOffset + static_cast<uint32_t>(tagBytes);
            m_xgkick.currentTagEop = ((tagLo >> 15) & 1u) != 0u;
            if (m_xgkick.currentTagEop)
                m_xgkick.totalBytes = m_xgkick.currentTagEnd;
        }

        if (m_xgkick.copiedBytes >= m_xgkick.currentTagEnd)
        {
            if (m_xgkick.currentTagEop)
                finishXgkick();
            else
            {
                // The next transferred qword is another GIFtag.
                m_xgkick.currentTagEnd = 0u;
                m_xgkick.currentTagEop = false;
            }
        }
    }
}

void VU1Core::badKickDiag(const char *why, uint64_t tagLo)
{
    VuDiag *d = vuDiag();
    if (!d || d->dumped || m_unit != Unit::VU1)
        return;
    d->dumped = true;
    const std::string &dir = d->dir;
    std::fprintf(stderr, "[vu1diag] first bad XGKICK (%s) in run %u start pc 0x%x: dump in %s\n", why, g_vuTraceRunIndex, g_vuRunPc, dir.c_str());
    if (m_activeMemory && m_activeMemory->getVU1Code())
        if (std::FILE *f = std::fopen((dir + "/vu1code.bin").c_str(), "wb"))
        {
            std::fwrite(m_activeMemory->getVU1Code(), 1, PS2_VU1_CODE_SIZE, f);
            std::fclose(f);
        }
    if (m_activeVuData)
        if (std::FILE *f = std::fopen((dir + "/vu1data.bin").c_str(), "wb"))
        {
            std::fwrite(m_activeVuData, 1, m_activeVuDataSize, f);
            std::fclose(f);
        }
    if (m_xgkick.copiedBytes)
        if (std::FILE *f = std::fopen((dir + "/kick.bin").c_str(), "wb"))
        {
            std::fwrite(m_xgkick.packet.data(), 1, m_xgkick.copiedBytes, f);
            std::fclose(f);
        }
    if (std::FILE *f = std::fopen((dir + "/state.txt").c_str(), "w"))
    {
        std::fprintf(f, "why %s tag %016llx run %u startPc 0x%x pc 0x%x cycle %llu top 0x%x itop 0x%x kickSrc 0x%x copied %u\n", why,
                     (unsigned long long)tagLo, g_vuTraceRunIndex, g_vuRunPc, m_state.pc, (unsigned long long)m_cycle, m_state.top, m_state.itop,
                     m_xgkick.sourceAddress, m_xgkick.copiedBytes);
        for (int i = 0; i < 16; ++i)
            std::fprintf(f, "vi%d %d (0x%x)\n", i, m_state.vi[i], static_cast<uint32_t>(m_state.vi[i]) & 0xFFFFu);
        for (int i = 0; i < 32; ++i)
        {
            uint32_t b[4];
            std::memcpy(b, m_state.vf[i], 16);
            std::fprintf(f, "vf%d %08x %08x %08x %08x  (%g %g %g %g)\n", i, b[0], b[1], b[2], b[3], m_state.vf[i][0], m_state.vf[i][1], m_state.vf[i][2], m_state.vf[i][3]);
        }
        if (m_activeMemory)
        {
            const VIFRegisters &v = m_activeMemory->vif1_regs;
            std::fprintf(f, "vif1 stat %x base %x ofst %x tops %x top %x itops %x itop %x cycle %x mode %x mask %x\n", v.stat, v.base, v.ofst, v.tops, v.top,
                         v.itops, v.itop, v.cycle, v.mode, v.mask);
        }
        std::fclose(f);
    }
    if (std::FILE *f = std::fopen((dir + "/runs.txt").c_str(), "w"))
    {
        const uint64_t n = std::min<uint64_t>(d->count, kVuDiagRing);
        for (uint64_t i = d->count - n; i < d->count; ++i)
        {
            const VuDiagRun &r = d->runs[i & (kVuDiagRing - 1u)];
            std::fprintf(f, "run %u start 0x%x end 0x%x top 0x%x itop 0x%x cyc %u ended %u budget %u stop %u vi", r.index, r.startPc, r.endPc, r.top, r.itop,
                         r.cycles, r.ended, r.budget, r.stopped);
            for (int k = 1; k < 16; ++k)
                std::fprintf(f, " %x", static_cast<uint32_t>(r.vi[k]) & 0xFFFFu);
            std::fprintf(f, "\n");
        }
        std::fclose(f);
    }
    ps2xVif1DiagDump(dir.c_str());
}

void VU1Core::finishXgkick()
{
    if (!m_xgkick.active)
        return;

    ++g_vuKicks;
    g_vuKickBytes += m_xgkick.totalBytes;
    ++g_kickHist[(g_vuRunPc >> 3) & 2047u];
    if (g_findTbp >= 0 && findActive())
        findScanKick(m_xgkick.packet.data(), m_xgkick.totalBytes, g_vuRunPc);
    if (g_vuRecActive)
    {
        g_vuRecKickHash = ps2xVuHashBytes(g_vuRecKickHash, &m_xgkick.totalBytes, sizeof(m_xgkick.totalBytes));
        g_vuRecKickHash = ps2xVuHashBytes(g_vuRecKickHash, m_xgkick.packet.data(), m_xgkick.totalBytes);
    }
    if (m_activeMemory)
        m_activeMemory->submitGifPacket(GifPathId::Path1, m_xgkick.packet.data(), m_xgkick.totalBytes);
    else if (m_activeGs)
        m_activeGs->processGIFPacket(m_xgkick.packet.data(), m_xgkick.totalBytes);
    m_xgkick.active = false;
}

void VU1Core::startXgkick(uint32_t qwordAddress)
{
    if (m_unit != Unit::VU1 || !m_activeVuData || m_activeVuDataSize < 16u)
        return;

    const uint32_t sourceAddress = (qwordAddress * 16u) % m_activeVuDataSize;
    {
        static const bool trace = [] { const char *e = std::getenv("PS2X_VIF_TRACE"); return e && *e && *e != '0'; }();
        static uint32_t count = 0;
        if (trace && (++count <= 80u || (count & (count - 1u)) == 0u))
        {
            uint64_t t0 = 0, t1 = 0;
            std::memcpy(&t0, m_activeVuData + sourceAddress, 8);
            std::memcpy(&t1, m_activeVuData + sourceAddress + 8, 8);
            std::fprintf(stderr, "[xgkick] #%u pc=0x%x qaddr=0x%x prevActive=%d tag=%016llx %016llx nloop=%u eop=%u flg=%u nreg=%u\n",
                         count, m_state.pc, qwordAddress, m_xgkick.active ? 1 : 0,
                         (unsigned long long)t0, (unsigned long long)t1,
                         (unsigned)(t0 & 0x7FFF), (unsigned)((t0 >> 15) & 1), (unsigned)((t0 >> 58) & 3), (unsigned)((t0 >> 60) & 0xF));
        }
    }
    // Reset XGKICK state without zeroing its 64 KB packet buffer (only the copied bytes are ever read).
    m_xgkick.sourceAddress = 0; m_xgkick.totalBytes = 0; m_xgkick.copiedBytes = 0; m_xgkick.currentTagEnd = 0;
    m_xgkick.cycleCredit = 0; m_xgkick.issueCycle = 0; m_xgkick.active = false; m_xgkick.currentTagEop = false;
    m_xgkick.active = true;
    m_xgkick.sourceAddress = sourceAddress;
    m_xgkick.cycleCredit = 1u; // XGKICK's issue cycle counts toward PATH1.
    m_xgkick.issueCycle = m_cycle;
}

void VU1Core::advanceOneCycle()
{
    ++g_vuSteps;
    ++m_cycle;
    m_state.cycles = m_cycle;
    // LSU commits become visible at the cycle boundary before PATH1 consumes
    // its next qword from VU memory.
    commitReadyPipelines();
    progressXgkick();
}

void VU1Core::advanceTo(uint64_t targetCycle)
{
    // Without an XGKICK in flight nothing happens on a cycle except pipeline commits, so jump
    // from one commit cycle to the next instead of stepping every stall cycle. Same results:
    // commits still happen at exactly the cycles where entries become ready.
    while (m_cycle < targetCycle)
    {
        if (m_xgkick.active)
        {
            advanceOneCycle();
            continue;
        }
        uint64_t &nextReady = nextReadyFor(this);
        if (nextReady == 0)
        {
            advanceOneCycle(); // unknown: step once, which recomputes it
            continue;
        }
        ++g_vuJumps;
        const uint64_t step = std::max<uint64_t>(m_cycle + 1u, std::min(nextReady, targetCycle));
        m_cycle = step;
        m_state.cycles = m_cycle;
        commitReadyPipelines();
    }
}

bool VU1Core::pipelinesPending() const
{
    if (m_fdiv.valid || m_xgkick.active)
        return true;
    if (m_cycle < slotFor(this).regWritesUntil)
        return true; // immediate register writes that the queued model would still be committing
    for (const ScalarPipelineEntry &entry : m_efu)
        if (entry.valid)
            return true;
    for (const FlagPipelineEntry &entry : m_flagPipeline)
        if (entry.valid)
            return true;
    for (const PendingStore &store : m_storePipeline)
        if (store.valid)
            return true;
    for (const PendingVfWrite &write : m_vfWritePipeline)
        if (write.valid)
            return true;
    for (const PendingViWrite &write : m_viWritePipeline)
        if (write.valid)
            return true;
    for (const PendingAccWrite &write : m_accWritePipeline)
        if (write.valid)
            return true;
    return false;
}

void VU1Core::flushPipelines()
{
    // PS2X_VU1_FASTFLUSH=0: step every cycle until nothing is pending (the reference).
    static const bool fast = [] { const char *e = std::getenv("PS2X_VU1_FASTFLUSH"); return !(e && *e == '0'); }();
    if (!fast)
    {
        while (pipelinesPending())
            advanceOneCycle();
        return;
    }
    if (!pipelinesPending())
        return;
    // Same end state and end cycle as stepping, without stepping each cycle:
    // 1. While a store is pending and PATH1 is still reading VU memory, step (the store changes
    //    what the kick reads).
    // 2. Then nothing else touches VU memory: finish the kick on its own (2 cycles per qword),
    //    counting the cycles it takes.
    // 3. Jump from one commit cycle to the next (advanceTo) up to the cycle where the stepped
    //    loop would stop: every entry committed, the kick done, regWritesUntil reached.
    const uint64_t start = m_cycle;
    while (m_xgkick.active && storePending())
        advanceOneCycle();
    uint64_t kickEnd = m_cycle;
    if (m_xgkick.active)
    {
        if (!m_activeVuData || m_activeVuDataSize == 0u)
        {
            while (pipelinesPending())
                advanceOneCycle(); // a kick that never progresses: as before
            return;
        }
        while (m_xgkick.active)
        {
            ++kickEnd;
            progressXgkick();
        }
    }
    uint64_t target = std::max(kickEnd, std::max(m_cycle == start ? start + 1u : m_cycle, slotFor(this).regWritesUntil));
    target = std::max(target, latestPendingReadyCycle());
    advanceTo(target);
    while (pipelinesPending()) // not expected; keeps the reference's stop condition exact
        advanceOneCycle();
}

bool VU1Core::storePending() const
{
    for (const PendingStore &store : m_storePipeline)
        if (store.valid)
            return true;
    return false;
}

uint64_t VU1Core::latestPendingReadyCycle() const
{
    uint64_t latest = 0;
    if (m_fdiv.valid)
        latest = std::max(latest, m_fdiv.readyCycle);
    for (const ScalarPipelineEntry &entry : m_efu)
        if (entry.valid) latest = std::max(latest, entry.readyCycle);
    for (const FlagPipelineEntry &entry : m_flagPipeline)
        if (entry.valid) latest = std::max(latest, entry.readyCycle);
    for (const PendingStore &store : m_storePipeline)
        if (store.valid) latest = std::max(latest, store.readyCycle);
    for (const PendingVfWrite &write : m_vfWritePipeline)
        if (write.valid) latest = std::max(latest, write.readyCycle);
    for (const PendingViWrite &write : m_viWritePipeline)
        if (write.valid) latest = std::max(latest, write.readyCycle);
    for (const PendingAccWrite &write : m_accWritePipeline)
        if (write.valid) latest = std::max(latest, write.readyCycle);
    return latest;
}



VU1Core::DecodedInstructionPair VU1Core::decodeInstructionPair(const uint8_t *vuCode, uint32_t pc) const
{
    uint32_t lower = 0, upper = 0;
    std::memcpy(&lower, vuCode + pc, sizeof(lower));
    std::memcpy(&upper, vuCode + pc + sizeof(lower), sizeof(upper));
    return decodePairWords(lower, upper, m_unit);
}

namespace
{
    // Upper ops whose results go through applyFmacDest/applyFmacDestAcc, i.e. write MAC/status
    // flags (when dest != 0): ADD/SUB/MADD/MSUB/MUL families incl. ACC forms and OPMULA/OPMSUB.
    bool upperWritesFmacFlags(uint32_t upper)
    {
        if (((upper >> 21) & 0xFu) == 0u)
            return false; // dest 0: updateFmacFlags returns without a flag entry
        const uint32_t op = upper & 0x3Fu;
        if (op < 0x3Cu)
            return op <= 0x0Fu || (op >= 0x18u && op <= 0x1Cu) || op == 0x1Eu ||
                   (op >= 0x20u && op <= 0x2Au && op != 0x2Bu) || op == 0x2Cu || op == 0x2Du || op == 0x2Eu;
        const uint32_t s = (upper & 3u) | ((upper >> 4) & 0x7Cu);
        return s <= 0x0Fu || (s >= 0x18u && s <= 0x1Cu) || s == 0x1Eu ||
               (s >= 0x20u && s <= 0x2Au) || s == 0x2Cu || s == 0x2Du || s == 0x2Eu;
    }

    // Static MAC-flag liveness over one code image. A flag read at cycle t sees the newest FMAC
    // op issued at or before t-4; every pair takes >= 1 cycle, so an op followed by >= 4
    // flag-writing ops on every path before any observation point can never be seen.
    // Observation points: FMEQ/FMAND/FMOR (before their own pair's upper), program end (after the
    // E-bit delay slot: the next program may read leftover flags), D/T halts, and JR/JALR targets
    // (unknown). Branch delay slots take the union of fall-through and target successors, which
    // only adds paths (safe). dist[q] = fewest flag-writing ops from q (incl. q's upper) to an
    // observation, capped at 4.
    template <class Pair>
    void computeFlagLiveness(Pair *cache, uint32_t pairs)
    {
        if (pairs == 0u)
            return;
        constexpr uint8_t kCap = 4u;
        std::vector<uint8_t> weight(pairs), readsHere(pairs), obsAfter(pairs);
        std::vector<std::vector<uint32_t>> extra(pairs); // extra successors (branch targets via delay slots)
        bool statusRead = false;
        for (uint32_t i = 0; i < pairs; ++i)
        {
            const Pair &p = cache[i];
            weight[i] = upperWritesFmacFlags(p.upper) ? 1u : 0u;
            if (p.dBit || p.tBit)
            {
                obsAfter[i] = 1u;
                obsAfter[(i + 1u) % pairs] = 1u;
            }
            if (p.eBit)
                obsAfter[(i + 1u) % pairs] = 1u;
            if (p.iBit)
                continue;
            const uint32_t lower = p.lower;
            switch (lower >> 25)
            {
            case 0x18: case 0x1A: case 0x1B: // FMEQ, FMAND, FMOR
                readsHere[i] = 1u;
                break;
            case 0x14: case 0x16: case 0x17: // FSEQ, FSAND, FSOR: sticky status bits accumulate
                statusRead = true;           // from every FMAC op, so nothing may be skipped
                break;
            case 0x20: case 0x21: case 0x28: case 0x29: case 0x2C: case 0x2D: case 0x2E: case 0x2F:
            {
                int32_t imm = static_cast<int32_t>(lower & 0x7FFu);
                if (imm & 0x400)
                    imm -= 0x800;
                const uint32_t target = static_cast<uint32_t>(static_cast<int32_t>(i + 1u) + imm + static_cast<int32_t>(pairs)) % pairs;
                extra[(i + 1u) % pairs].push_back(target);
                break;
            }
            case 0x24: case 0x25: // JR, JALR
                obsAfter[(i + 1u) % pairs] = 1u;
                break;
            default:
                break;
            }
        }
        std::vector<uint8_t> dist(pairs, kCap);
        for (uint32_t i = 0; i < pairs; ++i)
            if (readsHere[i])
                dist[i] = 0u;
        auto succMin = [&](uint32_t q) -> uint8_t
        {
            if (obsAfter[q])
                return 0u;
            uint8_t m = dist[(q + 1u) % pairs];
            for (uint32_t t : extra[q])
                m = std::min(m, dist[t]);
            return m;
        };
        for (bool changed = true; changed;)
        {
            changed = false;
            for (uint32_t k = pairs; k-- > 0u;) // backwards: most edges point forward
            {
                if (readsHere[k])
                    continue;
                const uint8_t d = static_cast<uint8_t>(std::min<uint32_t>(kCap, weight[k] + succMin(k)));
                if (d < dist[k])
                {
                    dist[k] = d;
                    changed = true;
                }
            }
        }
        for (uint32_t i = 0; i < pairs; ++i)
            cache[i].flagsLive = statusRead || !weight[i] || succMin(i) < kCap;
    }
}

namespace
{
    // Hash of a code image for the decoded-image cache (a hit is confirmed with a full compare).
    uint64_t hashCodeImage(const uint8_t *code, uint32_t size)
    {
        uint64_t h[4] = {size, 0x9E3779B97F4A7C15ull, 0xC2B2AE3D27D4EB4Full, 0x165667B19E3779F9ull};
        uint32_t at = 0;
        for (; at + 32u <= size; at += 32u)
            for (uint32_t k = 0; k < 4u; ++k)
            {
                uint64_t word;
                std::memcpy(&word, code + at + 8u * k, sizeof(word));
                h[k] = (h[k] ^ word) * 0x100000001B3ull;
                h[k] ^= h[k] >> 29;
            }
        for (; at < size; ++at)
            h[0] = (h[0] ^ code[at]) * 0x100000001B3ull;
        return (h[0] ^ (h[1] * 3u)) ^ ((h[2] * 5u) ^ (h[3] * 7u));
    }
}

void VU1Core::rebuildDecodedCodeCache(const uint8_t *vuCode, uint32_t codeSize,
                                             const PS2Memory *memory, uint64_t generation)
{
    const uint32_t pairCount = std::min<uint32_t>(codeSize / 8u, kMaxDecodedPairs);
    const uint32_t imageBytes = pairCount * 8u;
    const uint64_t hash = hashCodeImage(vuCode, imageBytes);
    DecodedImage *image = nullptr;
    for (const std::unique_ptr<DecodedImage> &candidate : m_decodedImages)
        if (candidate->hash == hash && candidate->codeSize == codeSize &&
            std::memcmp(candidate->code.data(), vuCode, imageBytes) == 0)
        {
            image = candidate.get();
            break;
        }
    if (!image)
    {
        if (m_decodedImages.size() < kMaxDecodedImages)
            image = m_decodedImages.emplace_back(std::make_unique<DecodedImage>()).get();
        else
            image = std::min_element(m_decodedImages.begin(), m_decodedImages.end(),
                                     [](const auto &a, const auto &b) { return a->lastUse < b->lastUse; })
                        ->get();
        ++g_vuDecodeRebuilds;
        image->code.assign(vuCode, vuCode + imageBytes);
        image->hash = hash;
        image->codeSize = codeSize;
        for (uint32_t i = 0; i < pairCount; ++i)
            image->pairs[i] = decodeInstructionPair(vuCode, i * 8u);
        static const bool noLiveness = [] { const char *a = std::getenv("PS2X_VU_NO_FLAG_LIVENESS"); const char *b = std::getenv("PS2X_VU_EXACT_FLAGS"); return (a && *a && *a != '0') || (b && *b && *b != '0'); }();
        if (m_unit == Unit::VU1 && !noLiveness)
            computeFlagLiveness(image->pairs.data(), pairCount);
        image->compiledPairs.fill(nullptr);
        image->compiledChecked.clear();
        image->compiledMissed.clear();
        image->mergedReach.clear();
        image->knownEntrySource.fill(0xFFFFu);
    }
    image->lastUse = ++m_decodedImageUse;
    m_image = image;
    m_decodedCodeCache = image->pairs.data();
    m_compiledPairs = image->compiledPairs.data();
    m_cachedVuCode = vuCode;
    m_cachedMemory = memory;
    m_cachedCodeSize = codeSize;
    m_cachedCodeGeneration = generation;
    m_decodedCodeCacheValid = true;
}

const VU1Core::DecodedInstructionPair &VU1Core::getDecodedInstructionPairForPc(
    const uint8_t *vuCode, uint32_t codeSize, PS2Memory *memory, uint32_t pc)
{
    if ((pc & 7u) != 0u)
        return m_uncachedPair = decodeInstructionPair(vuCode, pc);

    const bool trackedVu1Code = memory != nullptr &&
                                ((m_unit == Unit::VU1 && vuCode == memory->getVU1Code()) ||
                                 (m_unit == Unit::VU0 && vuCode == memory->getVU0Code()));
    if (!trackedVu1Code)
        return m_uncachedPair = decodeInstructionPair(vuCode, pc);

    const uint64_t generation = m_unit == Unit::VU1 ? memory->getVU1CodeGeneration() : memory->getVU0CodeGeneration();
    if (!m_decodedCodeCacheValid ||
        m_cachedVuCode != vuCode ||
        m_cachedMemory != memory ||
        m_cachedCodeSize != codeSize ||
        m_cachedCodeGeneration != generation)
    {
        rebuildDecodedCodeCache(vuCode, codeSize, memory, generation);
    }
    const uint32_t pairIndex = pc / 8u;
    if (pairIndex >= kMaxDecodedPairs)
        return m_uncachedPair = decodeInstructionPair(vuCode, pc);
    return m_decodedCodeCache[pairIndex];
}

void VU1Core::reportReservedInstruction(bool upper, uint32_t instruction)
{
    char rawText[48];
    {
        uint32_t w0 = 0, w1 = 0;
        if (m_cachedVuCode && m_state.pc + 8u <= m_cachedCodeSize)
        {
            std::memcpy(&w0, m_cachedVuCode + m_state.pc, 4);
            std::memcpy(&w1, m_cachedVuCode + m_state.pc + 4, 4);
        }
        std::snprintf(rawText, sizeof rawText, "%08x %08x", w0, w1);
    }
    RUNTIME_ERROR(
        "[VU" << (m_unit == Unit::VU1 ? "1" : "0")
              << " reserved " << (upper ? "upper" : "lower")
              << "] cycle=" << m_cycle
              << " pc=0x" << std::hex << m_state.pc
              << " instruction=0x" << instruction
              << " raw@pc=" << rawText
              << std::dec << '\n');
    m_stopRequested = true;
    m_blockBreak |= kBlockBreakStop;
}

// PS2X_VU_TRACE=<run>: print every instruction pair of the Nth VU1 program run.
int g_vuTraceRunGate = [] { const char *e = std::getenv("PS2X_VU_TRACE"); return e ? std::atoi(e) : 0; }();

void VU1Core::lookupCompiledPair(const uint8_t *vuCode, uint32_t codeSize, uint32_t pc)
{
    const uint32_t index = pc / 8u;
    if ((pc & 7u) != 0u || index >= kMaxDecodedPairs || m_compiledPairs[index] || m_image->compiledMissed.count(pc) != 0u)
        return;
    for (const VU1CompiledProgram *program : vu1CompiledProgramsAt(index))
    {
        if (!m_image->compiledChecked.insert(program).second)
            continue; // already checked against this code (it did not match, or it is merged)
        if (vu1CompiledProgramMatches(*program, vuCode, codeSize))
        {
            vu1MergeCompiledProgram(*program, m_compiledPairs);
            std::array<uint64_t, kMaxDecodedPairs / 64u> &reach = m_image->mergedReach.emplace_back();
            vu1CompiledProgramReach(*program, reach.data());
            m_image->knownEntrySource.fill(0xFFFFu);
        }
    }
    if (m_compiledPairs[index])
        return;
    m_image->compiledMissed.insert(pc);
    static const bool trace = [] { const char *e = std::getenv("PS2X_VU_COMPILED_TRACE"); return e && *e && *e != '0'; }();
    if (trace)
        std::fprintf(stderr, "[vu1c] no compiled program for pc 0x%04x (signature from there %016llx)\n", pc,
                     static_cast<unsigned long long>(vu1ProgramSignature(vuCode, codeSize, pc, nullptr)));
    if (g_vu1EntryHook)
        g_vu1EntryHook(pc);
}

bool VU1Core::stepGated(uint32_t pairIndex, StepArgs &args)
{
    const DecodedInstructionPair &decoded = m_decodedCodeCache[pairIndex];
    return stepPair<false, -1, kGateDecrement>(decoded, decoded.flagsLive, args);
}

void VU1Core::checkCompiledEntry(uint32_t pairIndex, uint32_t sequentialPc, uint32_t codeSize)
{
    const uint32_t pairs = std::min<uint32_t>(codeSize / 8u, kMaxDecodedPairs);
    if (pairs == 0u)
        return;
    // (x + pairs - 1) % pairs for x < pairs, without the two divisions (run() inlines this at every
    // taken branch). sequentialPc is a pc of this code (< codeSize) or 0.
    auto previousPair = [pairs](uint32_t x) { return x == 0u ? pairs - 1u : x - 1u; };
    const uint32_t sequentialPair = sequentialPc / 8u < pairs ? sequentialPc / 8u : (sequentialPc / 8u) % pairs;
    const uint32_t delaySlot = previousPair(sequentialPair); // the pair executed last
    const uint32_t branch = previousPair(delaySlot);
    if (m_image->knownEntrySource[pairIndex] == branch)
        return;
    for (const std::array<uint64_t, kMaxDecodedPairs / 64u> &reach : m_image->mergedReach)
    {
        auto has = [&reach](uint32_t i) { return ((reach[i / 64u] >> (i % 64u)) & 1u) != 0u; };
        if (has(pairIndex) && !(has(branch) && has(delaySlot)))
        {
            m_dynamicPairs = kGatePairs;
            m_normTrust = 0u;
            return;
        }
    }
    m_image->knownEntrySource[pairIndex] = static_cast<uint16_t>(branch);
}

// PS2X_VU_QUEUED_WRITES=1: VF/VI/ACC writes go through the delayed-commit queues (the reference model
// for the immediate writes in stepPair).
bool g_vuQueuedWrites = [] { const char *e = std::getenv("PS2X_VU_QUEUED_WRITES"); return e && *e && *e != '0'; }();

void VU1Core::waitUntilReady(const DecodedInstructionPair &decoded, uint64_t budgetEnd)
{
    uint64_t readyCycle = calculatePairReadyCycle(decoded);
    while (readyCycle > m_cycle)
    {
        if (readyCycle >= budgetEnd)
        {
            advanceTo(budgetEnd);
            break;
        }
        advanceTo(readyCycle);
        readyCycle = calculatePairReadyCycle(decoded);
    }
}

void VU1Core::retireQueuedWrites(const DecodedInstructionPair &decoded, const float *oldUpperVf, const float *oldLowerVf,
                                 const float *oldAcc, uint8_t writtenVi, int32_t oldVi)
{
    const VfAccess upperWrite = decoded.upperUsage.vfWrite;
    const VfAccess lowerWrite = decoded.lowerUsage.vfWrite;
    const bool hasUpperWrite = upperWrite.reg != 0u;
    const bool hasLowerWrite = lowerWrite.reg != 0u && decoded.suppressedLowerVf != lowerWrite.reg;
    const bool hasDistinctLowerWrite = hasLowerWrite && (!hasUpperWrite || lowerWrite.reg != upperWrite.reg);
    float newUpperVf[4]{};
    float newLowerVf[4]{};
    float newAcc[4]{};
    if (hasUpperWrite)
    {
        std::memcpy(newUpperVf, m_state.vf[upperWrite.reg], sizeof(newUpperVf));
        std::memcpy(m_state.vf[upperWrite.reg], oldUpperVf, sizeof(newUpperVf));
        const uint32_t latency =
            decoded.upperUsage.vfLatency != 0u
                ? decoded.upperUsage.vfLatency
                : decoded.upperUsage.latency;
        queueVfWrite(upperWrite.reg, upperWrite.lanes, newUpperVf, latency);
    }
    if (hasDistinctLowerWrite)
    {
        std::memcpy(newLowerVf, m_state.vf[lowerWrite.reg], sizeof(newLowerVf));
        std::memcpy(m_state.vf[lowerWrite.reg], oldLowerVf, sizeof(newLowerVf));
        const uint32_t latency = decoded.lowerUsage.vfLatency != 0u
                                     ? decoded.lowerUsage.vfLatency
                                     : decoded.lowerUsage.latency;
        queueVfWrite(lowerWrite.reg, lowerWrite.lanes, newLowerVf, latency);
    }
    if (decoded.upperUsage.accWrite != 0u)
    {
        std::memcpy(newAcc, m_state.acc, sizeof(newAcc));
        std::memcpy(m_state.acc, oldAcc, sizeof(newAcc));
        // ACC is forwarded to the next upper instruction. Its arithmetic
        // flags still use the normal four-cycle FMAC timeline.
        queueAccWrite(decoded.upperUsage.accWrite, newAcc,
                      kAccForwardLatency);
    }
    if (writtenVi != 0u)
    {
        const int32_t newVi = m_state.vi[writtenVi];
        m_state.vi[writtenVi] = oldVi;
        const uint32_t latency =
            decoded.lowerUsage.viLatency != 0u
                ? decoded.lowerUsage.viLatency
                : decoded.lowerUsage.latency;
        queueViWrite(writtenVi, newVi, latency);
    }
}

void VU1Core::tracePair(const DecodedInstructionPair &decoded)
{
    if (m_unit == Unit::VU1 && g_vuTraceRunIndex == static_cast<uint32_t>(g_vuTraceRunGate))
    {
        std::fprintf(stderr, "[vutrace] c=%llu pc=%04x lo=%08x up=%08x mac=%04x st=%03x clip=%06x vi:", (unsigned long long)m_cycle, m_state.pc, decoded.lower, decoded.upper,
                     (unsigned)m_state.mac, (unsigned)m_state.status, (unsigned)m_state.clip);
        for (int r = 1; r < 16; ++r)
            std::fprintf(stderr, " %d=%x", r, (unsigned)(uint16_t)m_state.vi[r]);
        std::fprintf(stderr, "\n");
    }
}
// debug: count of VU1 program runs (PS2X_VU_TRACE)

// ---------------------------------------------------------------------------------------------
// VU1 run recorder (PS2X_VU_RECORD=<file>,<startSec>,<runs>) and the hashes shared with the
// offline replay (ps2EntryRunner --vu-replay <file>, ps2_vu1_replay.cpp). Each recorded run keeps
// its inputs (entry state, data memory; code whenever it changed) and a hash of everything it
// produced, so interpreter changes can be checked for bit-identical results on real game runs.
// ---------------------------------------------------------------------------------------------
uint64_t ps2xVuHashBytes(uint64_t h, const void *data, size_t size)
{
    const uint8_t *p = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < size; ++i)
    {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

// Result of a run as five hashes: registers (except status), data memory, XGKICK packets, cycles
// taken, status register. Status is separate because its sticky bits are unobservable when the
// loaded microcode has no FSEQ/FSAND/FSOR, and the interpreter may then skip computing them.
void ps2xVuHashRunParts(const VU1State &s, const uint8_t *vuData, uint32_t dataSize, uint64_t kickHash, uint64_t cycles, uint64_t out[5])
{
    uint64_t h = 1469598103934665603ull;
    h = ps2xVuHashBytes(h, s.vf, sizeof(s.vf));
    h = ps2xVuHashBytes(h, s.vi, sizeof(s.vi));
    h = ps2xVuHashBytes(h, s.acc, sizeof(s.acc));
    const uint32_t words[] = {0, 0, 0, s.r, s.pc, s.mac, s.clip, 0u, s.top, s.itop, s.branchTarget, s.branchDelay};
    uint32_t w[12];
    std::memcpy(w, words, sizeof(w));
    std::memcpy(&w[0], &s.q, 4);
    std::memcpy(&w[1], &s.p, 4);
    std::memcpy(&w[2], &s.i, 4);
    h = ps2xVuHashBytes(h, w, sizeof(w));
    const uint8_t bits[] = {s.ebit, s.haltAfterDelaySlot, s.dBitEnabled, s.tBitEnabled, s.stoppedByD, s.stoppedByT, s.branchPending};
    h = ps2xVuHashBytes(h, bits, sizeof(bits));
    out[0] = h;
    out[1] = ps2xVuHashBytes(1469598103934665603ull, vuData, dataSize);
    out[2] = kickHash;
    out[3] = cycles;
    out[4] = s.status;
}

namespace
{
    struct VuRecorder
    {
        bool initialized = false;
        std::FILE *file = nullptr;
        double startSec = 0.0;
        uint32_t remaining = 0;
        uint64_t lastGeneration = ~0ull;
        uint64_t cycleAtStart = 0;
        std::chrono::steady_clock::time_point t0;
    };
    VuRecorder g_vuRecorder;

    bool vuRecorderWants()
    {
        VuRecorder &r = g_vuRecorder;
        if (!r.initialized)
        {
            r.initialized = true;
            r.t0 = std::chrono::steady_clock::now();
            if (const char *e = std::getenv("PS2X_VU_RECORD"))
            {
                std::string spec(e);
                const size_t c1 = spec.find(',');
                const size_t c2 = c1 == std::string::npos ? std::string::npos : spec.find(',', c1 + 1);
                const std::string path = spec.substr(0, c1);
                r.startSec = c1 == std::string::npos ? 0.0 : std::atof(spec.c_str() + c1 + 1);
                r.remaining = c2 == std::string::npos ? 5000u : static_cast<uint32_t>(std::strtoul(spec.c_str() + c2 + 1, nullptr, 0));
                r.file = std::fopen(path.c_str(), "wb");
                if (r.file)
                    std::fwrite("VUR3", 1, 4, r.file);
            }
        }
        if (!r.file || r.remaining == 0u)
            return false;
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - r.t0).count() >= r.startSec;
    }
}

void VU1Core::execute(uint8_t *vuCode, uint32_t codeSize,
                             uint8_t *vuData, uint32_t dataSize,
                             GS &gs, PS2Memory *memory,
                             uint32_t startPC, uint32_t top, uint32_t itop,
                             uint32_t maxCycles)
{
    resetScheduler();
    if (m_unit == Unit::VU1)
    {
        ++g_vuTraceRunIndex;
        g_vuRunPc = startPC;
    }
    {
        // PS2X_VU_DUMP=<run>: write that VU1 run's inputs to vu1_run_<run>.bin for offline replay/benchmarks.
        static const uint32_t dumpRun = [] { const char *e = std::getenv("PS2X_VU_DUMP"); return e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 0)) : 0u; }();
        // PS2X_VU_DUMP_PC=<pc> + PS2X_VU_DUMP_AFTER=<sec>: dump the first run starting at <pc> after <sec> seconds.
        static const int dumpPc = [] { const char *e = std::getenv("PS2X_VU_DUMP_PC"); return e ? static_cast<int>(std::strtol(e, nullptr, 0)) : -1; }();
        static const double dumpAfter = [] { const char *e = std::getenv("PS2X_VU_DUMP_AFTER"); return e ? std::atof(e) : 0.0; }();
        static const auto dumpStart = std::chrono::steady_clock::now();
        static const int dumpCount = [] { const char *e = std::getenv("PS2X_VU_DUMP_COUNT"); return e ? std::atoi(e) : 1; }();
        static int pcDumpedN = 0;
        bool pcHit = false;
        if (dumpPc >= 0 && pcDumpedN < dumpCount && m_unit == Unit::VU1 && static_cast<int>(startPC) == dumpPc &&
            std::chrono::duration<double>(std::chrono::steady_clock::now() - dumpStart).count() >= dumpAfter)
        {
            ++pcDumpedN;
            pcHit = true;
        }
        if ((dumpRun != 0u && m_unit == Unit::VU1 && g_vuTraceRunIndex == dumpRun) || pcHit)
        {
            char name[64];
            if (pcHit && dumpCount > 1) std::snprintf(name, sizeof(name), "vu1_pc%x_%03d.bin", dumpPc, pcDumpedN); else std::snprintf(name, sizeof(name), "vu1_run_%u.bin", pcHit ? 0xC0DE0000u + static_cast<uint32_t>(dumpPc) : dumpRun);
            if (FILE *f = std::fopen(name, "wb"))
            {
                const uint32_t header[6] = {codeSize, dataSize, startPC, top, itop, maxCycles};
                std::fwrite(header, sizeof(header), 1, f);
                std::fwrite(vuCode, 1, codeSize, f);
                std::fwrite(vuData, 1, dataSize, f);
                std::fwrite(&m_state, sizeof(m_state), 1, f);
                std::fclose(f);
                std::fprintf(stderr, "[vudump] wrote %s\n", name);
            }
        }
    }
    if (m_unit == Unit::VU1 && g_findDone && g_findNext > 0)
    {
        char name[64];
        std::snprintf(name, sizeof(name), "vu1_run_find_%d.bin", g_findNext--);
        if (FILE *f = std::fopen(name, "wb"))
        {
            const uint32_t header[6] = {codeSize, dataSize, startPC, top, itop, maxCycles};
            std::fwrite(header, sizeof(header), 1, f);
            std::fwrite(vuCode, 1, codeSize, f);
            std::fwrite(vuData, 1, dataSize, f);
            std::fwrite(&m_state, sizeof(m_state), 1, f);
            std::fclose(f);
        }
        // PS2X_VU_FIND_RAMQ=<qword>: with the last dump, list EE RAM addresses holding data qword <qword>
        // of this run (e.g. a matrix row uploaded by VIF), to put a write watch (PS2X_HOST_WATCH) on it.
        static const int ramQ = [] { const char *e = std::getenv("PS2X_VU_FIND_RAMQ"); return e ? std::atoi(e) : -1; }();
        if (g_findNext == 0 && ramQ >= 0 && memory && memory->getRDRAM() && static_cast<uint32_t>(ramQ) * 16u + 16u <= dataSize)
        {
            // The EE may already have rebuilt the values for a later frame: match 3 floats within 1%.
            const uint8_t *ram = memory->getRDRAM();
            for (int q = ramQ; q < ramQ + 4 && static_cast<uint32_t>(q) * 16u + 16u <= dataSize; ++q)
            {
                float want[3];
                std::memcpy(want, vuData + q * 16, sizeof(want));
                int hits = 0;
                for (uint32_t at = 0; at + 12u <= PS2_RAM_SIZE && hits < 16; at += 4u)
                {
                    float have[3];
                    std::memcpy(have, ram + at, sizeof(have));
                    bool close = std::fabs(want[0]) > 1e-30f && std::fabs(want[1]) > 1e-30f;
                    for (int k = 0; k < 3 && close; ++k)
                        close = std::fabs(have[k] - want[k]) <= 0.01f * std::fabs(want[k]) + 1e-12f;
                    if (close)
                    {
                        ++hits;
                        std::fprintf(stderr, "[vufind] qword %d (%g %g %g) ~ EE RAM 0x%08x (%g %g %g)\n", q, want[0], want[1], want[2], at, have[0], have[1], have[2]);
                    }
                }
            }
            std::fprintf(stderr, "[vufind] EE RAM scan for qwords %d..%d done\n", ramQ, ramQ + 3);
        }
    }
    if (m_unit == Unit::VU1 && findActive() && !g_findDone)
    {
        const uint32_t header[6] = {codeSize, dataSize, startPC, top, itop, maxCycles};
        g_findSnap.resize(sizeof(header) + codeSize + dataSize + sizeof(m_state));
        uint8_t *w = g_findSnap.data();
        std::memcpy(w, header, sizeof(header)); w += sizeof(header);
        std::memcpy(w, vuCode, codeSize); w += codeSize;
        std::memcpy(w, vuData, dataSize); w += dataSize;
        std::memcpy(w, &m_state, sizeof(m_state));
    }
    {
        // PS2X_VU_STATS=1: once per second print VU1 program runs, VU cycles and start PCs.
        static const bool stats = [] { const char *e = std::getenv("PS2X_VU_STATS"); return e && *e && *e != '0'; }();
        if (stats && m_unit == Unit::VU1)
        {
            static uint64_t runs = 0, lastCycle = 0;
            static uint32_t pcHist[2048] = {};
            static auto last = std::chrono::steady_clock::now();
            ++runs;
            ++pcHist[(startPC >> 3) & 2047u];
            const auto now = std::chrono::steady_clock::now();
            if (now - last >= std::chrono::seconds(1))
            {
                std::fprintf(stderr, "[vustats] runs=%llu vucycles=%llu rebuilds=%llu vu+gs=%llums gs=%llums top:", (unsigned long long)runs, (unsigned long long)(m_cycle - lastCycle), (unsigned long long)g_vuDecodeRebuilds, (unsigned long long)(g_vuRunNanos / 1000000u), (unsigned long long)(g_gsDrawNanos.exchange(0) / 1000000u)); g_vuDecodeRebuilds = 0; g_vuRunNanos = 0; static uint64_t lastCompiled = 0; std::fprintf(stderr, " pairs=%llu compiled=%llu steps=%llu jumps=%llu kicks=%llu kickbytes=%llu", (unsigned long long)g_vuPairs, (unsigned long long)(g_vuCompiledPairs - lastCompiled), (unsigned long long)g_vuSteps, (unsigned long long)g_vuJumps, (unsigned long long)g_vuKicks, (unsigned long long)g_vuKickBytes); lastCompiled = g_vuCompiledPairs; g_vuPairs = g_vuSteps = g_vuJumps = g_vuKicks = g_vuKickBytes = 0; std::fprintf(stderr, " kickpcs:"); for (uint32_t i = 0; i < 2048; ++i) if (g_kickHist[i]) std::fprintf(stderr, " %04x=%u", i << 3, g_kickHist[i]); std::memset(g_kickHist, 0, sizeof(g_kickHist));
                for (int k = 0; k < 4; ++k)
                {
                    uint32_t best = 0, bi = 0;
                    for (uint32_t i = 0; i < 2048; ++i) if (pcHist[i] > best) { best = pcHist[i]; bi = i; }
                    if (!best) break;
                    std::fprintf(stderr, " %04x=%u", bi << 3, best);
                    pcHist[bi] = 0;
                }
                std::fprintf(stderr, "\n");
                std::memset(pcHist, 0, sizeof(pcHist));
                runs = 0; lastCycle = m_cycle; last = now;
            }
        }
    }
    m_state.pc = startPC & microAddressMask();
    m_state.ebit = false;
    m_state.haltAfterDelaySlot = false;
    m_state.stoppedByD = false;
    m_state.stoppedByT = false;
    m_state.top = top;
    m_state.itop = itop;
    m_state.branchPending = false;
    m_state.branchTarget = 0;
    m_state.branchDelay = 0;
    m_state.vf[0][0] = 0.0f;
    m_state.vf[0][1] = 0.0f;
    m_state.vf[0][2] = 0.0f;
    m_state.vf[0][3] = 1.0f;
    const bool recording = m_unit == Unit::VU1 && memory && vuRecorderWants();
    if (recording)
    {
        VuRecorder &r = g_vuRecorder;
        const uint64_t generation = memory->getVU1CodeGeneration();
        if (generation != r.lastGeneration)
        {
            r.lastGeneration = generation;
            std::fputc('C', r.file);
            std::fwrite(&codeSize, sizeof(codeSize), 1, r.file);
            std::fwrite(vuCode, 1, codeSize, r.file);
        }
        const uint32_t header[6] = {startPC, top, itop, maxCycles, dataSize, 0u};
        std::fputc('R', r.file);
        std::fwrite(header, sizeof(header), 1, r.file);
        std::fwrite(&m_state, sizeof(m_state), 1, r.file);
        std::fwrite(vuData, 1, dataSize, r.file);
        g_vuRecKickHash = 1469598103934665603ull;
        g_vuRecActive = true;
        r.cycleAtStart = m_cycle;
    }
    static const bool timed = [] { const char *e = std::getenv("PS2X_VU_STATS"); return e && *e && *e != '0'; }();
    if (!timed)
    {
        run(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
    }
    else
    {
        const auto runStart = std::chrono::steady_clock::now();
        run(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
        if (m_unit == Unit::VU1)
            g_vuRunNanos += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - runStart).count());
    }
    if (recording)
    {
        VuRecorder &r = g_vuRecorder;
        g_vuRecActive = false;
        uint64_t parts[5];
        ps2xVuHashRunParts(m_state, vuData, dataSize, g_vuRecKickHash, m_cycle - r.cycleAtStart, parts);
        std::fwrite(parts, sizeof(parts), 1, r.file);
        if (--r.remaining == 0u)
        {
            std::fclose(r.file);
            r.file = nullptr;
            std::fprintf(stderr, "[vurecord] done\n");
        }
    }
}

// Read by the replay tool to report ns per instruction pair.
uint64_t ps2xVuPairsExecuted()
{
    return g_vuPairs;
}

void VU1Core::resume(uint8_t *vuCode, uint32_t codeSize,
                            uint8_t *vuData, uint32_t dataSize,
                            GS &gs, PS2Memory *memory,
                            uint32_t top, uint32_t itop, uint32_t maxCycles)
{
    m_state.top = top;
    m_state.itop = itop;
    m_state.stoppedByD = false;
    m_state.stoppedByT = false;
    run(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
}

void VU1Core::run(uint8_t *vuCode, uint32_t codeSize,
                         uint8_t *vuData, uint32_t dataSize,
                         GS &gs, PS2Memory *memory, uint32_t maxCycles)
{
    m_activeVuData = vuData;
    m_activeVuDataSize = dataSize;
    m_activeGs = &gs;
    m_activeMemory = memory;
    beginProgramRun(this, m_state.pc);

    // VU arithmetic rounds toward zero. Only the SSE rounding field (MXCSR bits 13-14) matters
    // here: the VU code is SSE only (long double is double on Windows x64). Set directly, as
    // std::fesetround/fegetround (which also rewrite the x87 control word) cost ~100s of cycles per run.
    const uint32_t previousCsr = _mm_getcsr();
    _mm_setcsr((previousCsr & ~0x6000u) | 0x6000u);
    const uint64_t budgetEnd = m_cycle + maxCycles;
    StepArgs args;
    args.vuData = vuData;
    args.dataSize = dataSize;
    args.gs = &gs;
    args.memory = memory;
    args.codeSize = codeSize;
    args.budgetEnd = budgetEnd;

    // VU code cannot change while a program runs: bring the decode cache up to date once, then
    // index it directly (getDecodedInstructionPairForPc does the same checks per pair).
    const bool trackedCode = memory != nullptr &&
                             ((m_unit == Unit::VU1 && vuCode == memory->getVU1Code()) ||
                              (m_unit == Unit::VU0 && vuCode == memory->getVU0Code()));
    if (trackedCode)
    {
        const uint64_t generation = m_unit == Unit::VU1 ? memory->getVU1CodeGeneration() : memory->getVU0CodeGeneration();
        if (!m_decodedCodeCacheValid || m_cachedVuCode != vuCode || m_cachedMemory != memory ||
            m_cachedCodeSize != codeSize || m_cachedCodeGeneration != generation)
            rebuildDecodedCodeCache(vuCode, codeSize, memory, generation);
    }
    m_slot->flagsObservableRun = VU1_FMAC_FLAGS_OBSERVABLE_EVAL();

    // A statically recompiled program for this code (tracked VU1 code only: its decode cache, which
    // the compiled pairs read flag liveness from, is current). PS2X_VU_COMPILED=0 turns them off.
    static const bool compiledEnabled = [] { const char *e = std::getenv("PS2X_VU_COMPILED"); return !(e && *e == '0'); }();
    bool compiledActive = compiledEnabled && trackedCode && m_unit == Unit::VU1;
    // Generated blocks use VF0 as the constant (0, 0, 0, 1) without normalizing it (every pair
    // restores it; reset and execute() set it). Should a run ever start with another VF0, it runs
    // in the interpreter.
    if (compiledActive && (std::bit_cast<uint32_t>(m_state.vf[0][0]) != 0u || std::bit_cast<uint32_t>(m_state.vf[0][1]) != 0u ||
                           std::bit_cast<uint32_t>(m_state.vf[0][2]) != 0u || std::bit_cast<uint32_t>(m_state.vf[0][3]) != 0x3F800000u))
    {
        static bool warned = false;
        if (!warned)
        {
            warned = true;
            std::fprintf(stderr, "[vu1c] VF0 is not (0, 0, 0, 1) at a run start: interpreter for such runs\n");
        }
        compiledActive = false;
    }
    if (compiledActive)
        lookupCompiledPair(vuCode, codeSize, m_state.pc);
    uint32_t sequentialPc = m_state.pc; // where the previous pair falls through to
    m_normTrust = 0u;

    // Entries due now are committed here once; afterwards every cycle step (advanceOneCycle,
    // advanceTo) commits at the cycle it reaches, so the loop needs no extra commit.
    commitReadyPipelines();
    while (m_cycle < budgetEnd && !m_stopRequested)
    {
        if (m_state.pc + 8u > codeSize)
            break;

        const uint32_t pc = m_state.pc;
        const uint32_t pairIndex = pc / 8u;
        const uint32_t nextSequential = pc + 8u >= codeSize ? 0u : pc + 8u;
        if (compiledActive && (pc & 7u) == 0u && pairIndex < kMaxDecodedPairs)
        {
            CompiledPairFn fn = m_compiledPairs[pairIndex];
            if (!fn && pc != sequentialPc)
            {
                // Jumped to a pair with no compiled code yet (a JR/JALR target the static walk of the
                // running program cannot follow): another program may contain it.
                lookupCompiledPair(vuCode, codeSize, pc);
                fn = m_compiledPairs[pairIndex];
            }
            if (fn)
            {
                if (pc != sequentialPc)
                    checkCompiledEntry(pairIndex, sequentialPc, codeSize);
                ++g_vuCompiledPairs;
                args.sequentialPc = nextSequential;
                const bool more = fn(*this, args); // a block may run several pairs
                sequentialPc = args.sequentialPc;
                if (!more)
                    break;
                continue;
            }
        }
        sequentialPc = nextSequential;
        m_normTrust = 0u; // outside every program's static flow
        const DecodedInstructionPair &decoded =
            (trackedCode && (m_state.pc & 7u) == 0u && pairIndex < kMaxDecodedPairs)
                ? m_decodedCodeCache[pairIndex]
                : getDecodedInstructionPairForPc(vuCode, codeSize, memory, m_state.pc);
        if (!stepPair<false>(decoded, decoded.flagsLive, args))
            break;
    }

    if (args.programEnded)
    {
        flushPipelines();
        m_state.ebit = false;
        m_state.haltAfterDelaySlot = false;
        m_pendingHaltD = false;
        m_pendingHaltT = false;
    }
    m_state.cycles = m_cycle;
    _mm_setcsr((_mm_getcsr() & ~0x6000u) | (previousCsr & 0x6000u));
    if (m_unit == Unit::VU1)
        if (VuDiag *d = vuDiag())
        {
            VuDiagRun &r = d->runs[d->count++ & (kVuDiagRing - 1u)];
            r.index = g_vuTraceRunIndex;
            r.startPc = g_vuRunPc;
            r.endPc = m_state.pc;
            r.top = m_state.top;
            r.itop = m_state.itop;
            r.cycles = static_cast<uint32_t>(m_cycle - (budgetEnd - maxCycles));
            r.ended = args.programEnded ? 1u : 0u;
            r.budget = (!args.programEnded && m_cycle >= budgetEnd) ? 1u : 0u;
            r.stopped = m_stopRequested ? 1u : 0u;
            std::memcpy(r.vi, m_state.vi, sizeof(r.vi));
            if (r.budget && ++d->budgetHits <= 200u)
                std::fprintf(stderr, "[vu1diag] run %u (start pc 0x%x) hit the %u-cycle budget at pc 0x%x\n", r.index, r.startPc, maxCycles, r.endPc);
        }
}

// Public facade (runtime/ps2_vu1.h).
VU1Interpreter::VU1Interpreter(Unit unit)
    : m_core(new VU1Core(unit)), m_state(&m_core->state())
{
}

VU1Interpreter::~VU1Interpreter()
{
    delete m_core;
}

void VU1Interpreter::reset()
{
    m_core->reset();
}

void VU1Interpreter::execute(uint8_t *vuCode, uint32_t codeSize,
                             uint8_t *vuData, uint32_t dataSize,
                             GS &gs, PS2Memory *memory,
                             uint32_t startPC, uint32_t top, uint32_t itop,
                             uint32_t maxCycles)
{
    m_core->execute(vuCode, codeSize, vuData, dataSize, gs, memory, startPC, top, itop, maxCycles);
}

void VU1Interpreter::resume(uint8_t *vuCode, uint32_t codeSize,
                            uint8_t *vuData, uint32_t dataSize,
                            GS &gs, PS2Memory *memory,
                            uint32_t top, uint32_t itop, uint32_t maxCycles)
{
    m_core->resume(vuCode, codeSize, vuData, dataSize, gs, memory, top, itop, maxCycles);
}
