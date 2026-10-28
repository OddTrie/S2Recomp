#ifndef PS2_VU1_CORE_H
#define PS2_VU1_CORE_H

// The VU interpreter itself. Private to src/lib/vu: the public VU1Interpreter (runtime/ps2_vu1.h,
// part of the precompiled header of every recompiled file) only holds a pointer to it, so changing
// anything here rebuilds the VU files instead of all ~13k runner files.

#include "runtime/ps2_vu1.h"
#include "ps2_vu1_detail.h"

#include <array>
#include <cstdint>
#include <smmintrin.h>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class GS;
class PS2Memory;

#if defined(_MSC_VER)
#define VU1_FORCEINLINE __forceinline
#define VU1_NOINLINE __declspec(noinline)
#else
#define VU1_FORCEINLINE inline __attribute__((always_inline))
#define VU1_NOINLINE __attribute__((noinline))
#endif

#ifndef VU1_COMMIT_LEAF
#define VU1_COMMIT_LEAF 1 // 1: the per-pair flag-only commit calls a small leaf instead of inlining its loop
#endif

extern bool g_vuForceExactFlags; // PS2X_VU_EXACT_FLAGS=1 (ps2_vu1_core.cpp)
extern bool g_vuNoInlineLower;   // PS2X_VU_NO_INLINE_LOWER=1: out-of-line operand normalize + scalar MR32 (ps2_vu1_core.cpp)
extern bool g_vuQueuedWrites;    // PS2X_VU_QUEUED_WRITES=1 (ps2_vu1_core.cpp)

namespace vu1detail
{
    // Scheduler bookkeeping of one interpreter (VU1Core::m_slot).
    struct NextReadySlot
    {
        uint64_t next = 0;
        // Bit i set = entry i of that pipeline array is valid (kept in sync at every queue/commit).
        uint32_t flagMask = 0, efuMask = 0, storeMask = 0, vfMask = 0, viMask = 0, accMask = 0;
        bool masksValid = false; // false = rebuild from the arrays before use
        // Whether the loaded microcode can observe MAC/status flags (FSEQ/FSAND/FSOR/FMEQ/
        // FMAND/FMOR). If not, FMAC results only need value normalization.
        int flagsNeeded = -1;
        int statusNeeded = -1; // code has FSEQ/FSAND/FSOR: status (incl. sticky bits) is observable
        // Immediate register writes: the cycle by which every VF/VI/ACC write issued so far would
        // have committed in the queued model (the end-of-program flush waits for it).
        uint64_t regWritesUntil = 0;
        uint64_t flagsGeneration = 0;
        const void *flagsCode = nullptr;
        uint32_t flagsCodeSize = 0;
        // Per program run: whether code reachable from the run's start pc reads flags
        // (-1 = not computed yet), memoized per start pc for the current code generation.
        uint32_t runStartPc = 0;
        int runFlagsNeeded = -1;
        uint64_t perStartGeneration = 0;
        std::unordered_map<uint32_t, bool> perStartNeeded;
        std::unordered_set<uint32_t> forcedExact; // start pcs where the analysis missed a flag read
        // fmacFlagsObservableImpl() for the current run(), evaluated once at its start (its inputs
        // do not change during a run; ps2xVuNoteFlagRead sets it when it forces exact flags).
        bool flagsObservableRun = true;
    };

    // VU lane bits (x = 8 ... w = 1) as a per-component mask (all ones = lane written), for blends.
    inline __m128 destLaneMask(uint32_t lanes)
    {
        return _mm_castsi128_ps(_mm_set_epi32(-static_cast<int>(lanes & 1u), -static_cast<int>((lanes >> 1) & 1u),
                                              -static_cast<int>((lanes >> 2) & 1u), -static_cast<int>((lanes >> 3) & 1u)));
    }

    // stepPair's decoded pair read at run time (the interpreter). Generated VU1 pairs use
    // vu1detail::StaticPair (ps2_vu1_step.inl), whose decoded pair is a compile-time constant.
    struct DynamicPair
    {
        static constexpr bool kStatic = false;
    };
}

class VU1Core
{
public:
    using Unit = VU1Interpreter::Unit;

    explicit VU1Core(Unit unit = Unit::VU1);
    ~VU1Core();
    VU1Core(const VU1Core &) = delete;
    VU1Core &operator=(const VU1Core &) = delete;

    void reset();

    void execute(uint8_t *vuCode, uint32_t codeSize,
                 uint8_t *vuData, uint32_t dataSize,
                 GS &gs, PS2Memory *memory = nullptr,
                 uint32_t startPC = 0, uint32_t top = 0, uint32_t itop = 0,
                 uint32_t maxCycles = 65536);

    void resume(uint8_t *vuCode, uint32_t codeSize,
                uint8_t *vuData, uint32_t dataSize,
                GS &gs, PS2Memory *memory = nullptr,
                uint32_t top = 0, uint32_t itop = 0, uint32_t maxCycles = 65536);

    VU1State &state() { return m_state; }
    const VU1State &state() const { return m_state; }
    vu1detail::NextReadySlot &sched() const { return *m_slot; }

    // Everything below is public too: this header is private to src/lib/vu, and the generated VU1
    // programs (vu1gen/) call straight into the interpreter's own per-pair step.
    enum Pipeline : uint8_t
    {
        PipelineNone = 0,
        PipelineFmac,
        PipelineLsu,
        PipelineFdiv,
        PipelineEfu,
        PipelineIalu,
        PipelineBranch,
        PipelineXgkick
    };

    struct VfAccess
    {
        uint8_t reg = 0;
        uint8_t lanes = 0;
    };

    struct InstructionUsage
    {
        std::array<VfAccess, 2> vfRead{};
        VfAccess vfWrite{};
        uint8_t vfReadCount = 0;
        uint16_t viRead = 0;
        uint16_t viWrite = 0;
        uint8_t accRead = 0;
        uint8_t accWrite = 0;
        uint8_t latency = 0;
        uint8_t vfLatency = 0;
        uint8_t viLatency = 0;
        Pipeline pipeline = PipelineNone;
        bool waitQ = false;
        bool waitP = false;
        bool readsClip = false;
        bool writesClip = false;
        bool delaysNextBranchRead = false;
        bool reserved = false;
        bool writesZeroReg = false; // a VF or VI write to register 0 (left out of vfWrite / viWrite)
    };

    struct DecodedInstructionPair
    {
        uint32_t lower = 0;
        uint32_t upper = 0;
        InstructionUsage lowerUsage{};
        InstructionUsage upperUsage{};
        bool iBit = false;
        bool eBit = false;
        bool mBit = false;
        bool dBit = false;
        bool tBit = false;
        uint8_t upperVfShadowReg = 0;
        uint8_t suppressedLowerVf = 0;
        // Static MAC-flag liveness (computed per code image): false when this pair's upper FMAC
        // op is followed by >= 4 flag-writing FMAC ops on every path before anything can observe
        // the MAC flags, so its flag bookkeeping can be skipped (results stay exact).
        bool flagsLive = true;
        // Precomputed at decode for the interpreter's hot paths (same results as decoding live):
        // the register-ready checks of both instructions, flattened for calculatePairReadyCycle.
        std::array<uint8_t, 16> readyVf{}; // reg * 4 + component, into m_vfReady
        std::array<uint8_t, 8> readyVi{};  // VI registers
        uint8_t readyVfCount = 0;
        uint8_t readyViCount = 0;
        uint8_t readyAccComponents = 0; // bit c = ACC component c
        uint8_t readyMisc = 0;          // kReady* bits
        uint8_t fmacNorm = 0;           // normalizeFmacResult operand class (kFmacNorm* / computeFmacNorm)
        uint8_t fmacZero = 0;           // normalizeFmacResult zero-result shortcut class (computeFmacZero)
    };

    static constexpr uint8_t kReadyFdiv = 1u, kReadyEfu = 2u, kReadyWaitQ = 4u, kReadyWaitP = 8u, kReadyXgkick = 16u;

    // Per-pair bookkeeping a generated pair may leave out (stepPair's kElide), each proven
    // unnecessary by the generator's static analysis of the program (ps2_vu1_compiled.cpp):
    // forward facts (no reader inside a write's latency window on any path) hold however the pair
    // was entered; backward facts (no writer inside the window before a read, no branch / E bit /
    // halting branch before the pair) only for entries along the program's static edges. Other
    // entries (JR/JALR, branches from code outside the program) set m_dynamicPairs, and while it
    // is non-zero the generated pairs with backward facts take the interpreter's full step.
    static constexpr uint32_t kElideUpperMark = 1u;   // no ready-table entry for the upper VF write
    static constexpr uint32_t kElideLowerMark = 2u;   // ... for the lower VF write
    static constexpr uint32_t kElideViMark = 4u;      // ... for the VI write
    static constexpr uint32_t kElideAccMark = 8u;     // ... for the ACC write
    static constexpr uint32_t kElideReady = 16u;      // reads filtered (filterReady): backward
    static constexpr uint32_t kElideRegUntil = 32u;   // regWritesUntil raised by a later write: backward
    static constexpr uint32_t kElideBranch = 64u;     // no branch pending at this pair: backward
    static constexpr uint32_t kElidePrevEnd = 128u;   // no E bit / halting branch before: backward
    static constexpr uint32_t kElideBackward = kElideReady | kElideRegUntil | kElideBranch | kElidePrevEnd;
    static constexpr uint32_t kGateDecrement = 256u;  // full step that counts down m_dynamicPairs
    // Block-internal (runBlockFrom): the previous pair of the block cannot have set
    // m_viBranchBackupValid, so it is still false and this pair need not clear it.
    static constexpr uint32_t kElideViBackupClear = 512u;
    // Longest VF/VI write latency (the generator checks every pair against it) and the pairs that
    // run with every check after an entry the analysis does not know.
    static constexpr uint32_t kMaxRegLatency = 4u;
    static constexpr uint32_t kGatePairs = kMaxRegLatency + 1u;

    struct FlagPipelineEntry
    {
        uint64_t readyCycle = 0;
        uint64_t issueCycle = 0;
        uint32_t mac = 0;
        uint32_t status = 0;
        uint32_t extraSticky = 0;
        uint32_t clip = 0;
        bool valid = false;
        bool writesMac = false;
        bool writesStatus = false;
        bool writesSticky = false;
        bool writesClip = false;
    };

    struct ScalarPipelineEntry
    {
        uint64_t readyCycle = 0;
        float value = 0.0f;
        uint32_t statusDi = 0;
        bool valid = false;
    };

    struct PendingStore
    {
        uint64_t readyCycle = 0;
        uint32_t address = 0;
        std::array<uint32_t, 4> words{};
        uint8_t laneMask = 0;
        bool valid = false;
    };

    struct PendingVfWrite
    {
        uint64_t readyCycle = 0;
        uint64_t sequence = 0;
        std::array<float, 4> value{};
        uint8_t reg = 0;
        uint8_t laneMask = 0;
        bool valid = false;
    };

    struct PendingViWrite
    {
        uint64_t readyCycle = 0;
        uint64_t sequence = 0;
        int32_t value = 0;
        uint8_t reg = 0;
        bool valid = false;
    };

    struct PendingAccWrite
    {
        uint64_t readyCycle = 0;
        uint64_t sequence = 0;
        std::array<float, 4> value{};
        uint8_t laneMask = 0;
        bool valid = false;
    };

    struct XgkickPipeline
    {
        static constexpr uint32_t kBufferSize = 0x10000u;
        std::array<uint8_t, kBufferSize> packet{};
        uint32_t sourceAddress = 0;
        uint32_t totalBytes = 0;
        uint32_t copiedBytes = 0;
        uint32_t currentTagEnd = 0;
        uint32_t cycleCredit = 0;
        uint64_t issueCycle = 0;
        bool active = false;
        bool currentTagEop = false;
    };

    static constexpr uint32_t kFmacLatency = 4u;
    static constexpr uint32_t kAccForwardLatency = 1u;
    static constexpr uint32_t kMaxFlagEntries = 8u;
    static constexpr uint32_t kMaxPendingStores = 8u;
    static constexpr uint32_t kMaxPendingVfWrites = 16u;
    static constexpr uint32_t kMaxPendingViWrites = 8u;
    static constexpr uint32_t kMaxPendingAccWrites = 8u;
    static constexpr uint32_t kMaxDecodedPairs = 0x4000u / 8u;

    Unit m_unit;
    vu1detail::NextReadySlot *m_slot;
    VU1State m_state;
    DecodedInstructionPair *m_decodedCodeCache = nullptr; // the current code image's pairs (m_image->pairs)
    const uint8_t *m_cachedVuCode = nullptr;
    const PS2Memory *m_cachedMemory = nullptr;
    uint32_t m_cachedCodeSize = 0;
    uint64_t m_cachedCodeGeneration = 0;
    bool m_decodedCodeCacheValid = false;

    std::array<FlagPipelineEntry, kMaxFlagEntries> m_flagPipeline{};
    ScalarPipelineEntry m_fdiv{};
    std::array<ScalarPipelineEntry, 2> m_efu{};
    std::array<PendingStore, kMaxPendingStores> m_storePipeline{};
    std::array<PendingVfWrite, kMaxPendingVfWrites> m_vfWritePipeline{};
    std::array<PendingViWrite, kMaxPendingViWrites> m_viWritePipeline{};
    std::array<PendingAccWrite, kMaxPendingAccWrites> m_accWritePipeline{};
    XgkickPipeline m_xgkick{};
    std::array<std::array<uint64_t, 4>, 32> m_vfReady{};
    std::array<uint64_t, 16> m_viReady{};
    std::array<uint64_t, 4> m_accReady{};
    std::array<std::array<uint64_t, 4>, 32> m_vfLatestWrite{};
    std::array<uint64_t, 16> m_viLatestWrite{};
    std::array<uint64_t, 4> m_accLatestWrite{};

    uint64_t m_cycle = 0;
    uint64_t m_nextWriteSequence = 0;
    uint64_t m_efuResourceReady = 0;
    uint32_t m_workingClip = 0;
    uint32_t m_currentUpperInstruction = 0;
    bool m_currentFlagsLive = true; // DecodedInstructionPair::flagsLive of the executing pair
    uint8_t m_currentFmacNorm = 0;  // DecodedInstructionPair::fmacNorm of the executing pair
    uint8_t m_currentFmacZero = 0;  // DecodedInstructionPair::fmacZero of the executing pair
    uint64_t m_readyHorizon = 0;    // no m_vfReady/m_viReady/m_accReady entry is later than this
    uint32_t m_dynamicPairs = 0;    // > 0: generated pairs with backward elisions take the full step
    DecodedInstructionPair m_uncachedPair{}; // getDecodedInstructionPairForPc outside the cache
    int32_t m_viBranchBackupValue = 0;
    uint8_t m_viBranchBackupReg = 0;
    bool m_viBranchBackupValid = false;
    uint8_t *m_activeVuData = nullptr;
    uint32_t m_activeVuDataSize = 0;
    GS *m_activeGs = nullptr;
    PS2Memory *m_activeMemory = nullptr;
    bool m_stopRequested = false;
    // Checked after every pair of a generated block: kBlockBreakStop mirrors m_stopRequested,
    // kBlockBreakNorm = applyFmacDest left an FMAC result unnormalized (see runBlockFrom).
    static constexpr uint8_t kBlockBreakStop = 1u, kBlockBreakNorm = 2u;
    uint8_t m_blockBreak = 0u;
    // The program (signature) whose generated blocks may trust their entry lanes (vu1detail::NormLanes
    // the generator found known normalized on every path into them): set by its blocks entered with
    // nothing known, cleared by anything its static flow does not cover (run start, interpreter
    // pairs, JR/JALR, entries checkCompiledEntry gates) and by an unnormalized FMAC result.
    uint64_t m_normTrust = 0u;
    bool m_pendingHaltD = false;
    bool m_pendingHaltT = false;

    void run(uint8_t *vuCode, uint32_t codeSize,
             uint8_t *vuData, uint32_t dataSize,
             GS &gs, PS2Memory *memory, uint32_t maxCycles);

    // What one run() hands to every pair step (interpreted or generated).
    struct StepArgs
    {
        uint8_t *vuData = nullptr;
        uint32_t dataSize = 0;
        GS *gs = nullptr;
        PS2Memory *memory = nullptr;
        uint32_t codeSize = 0;
        uint64_t budgetEnd = 0;
        bool programEnded = false;
        // Where the last executed pair falls through to (run() sets it before a compiled function;
        // a generated block that runs several pairs sets it when it returns).
        uint32_t sequentialPc = 0;
    };
    // One instruction pair at m_state.pc (stall, execute, retire, one cycle): the interpreter loop
    // body (ps2_vu1_step.inl). false = run() stops (budget, program end, reserved instruction).
    // kInlineExec: execute through the force-inlined semantics (generated programs, constant words).
    // kUpperHandler >= 0: the upper instruction's handler (vu1UpperHandler, ps2_vu1_upper.inl),
    // known at compile time in generated programs; -1: dispatch at run time (interpreter).
    // Src: vu1detail::DynamicPair, or a generated pair's vu1detail::StaticPair (then `decoded` is
    // Src::d and every value read from it is folded at compile time).
    // kBlockNextPc != 0: a pair inside a generated block that passed runBlock's entry guard, with
    // kBlockNextPc its fall-through pc (see runBlock for what it leaves out).
    // kNormBits (blocks): operands known normalized (vu1detail::normPairBits). kStorePc = false (blocks):
    // leave m_state.pc for runBlockFrom (nothing before the next pair reads it).
    template <bool kInlineExec, int kUpperHandler = -1, uint32_t kElide = 0u, class Src = vu1detail::DynamicPair, uint32_t kBlockNextPc = 0u,
              uint32_t kNormBits = 0u, bool kStorePc = true>
    inline bool stepPair(const DecodedInstructionPair &decoded, bool flagsLive, StepArgs &args);
    // A generated pair with elisions (kElide); `decoded` has its proven-ready reads removed
    // (filterReady). While m_dynamicPairs is set, pairs with backward elisions run stepGated.
    template <int kUpperHandler, uint32_t kElide, class Src>
    inline bool stepCompiledPair(const DecodedInstructionPair &decoded, uint32_t pairIndex, StepArgs &args);
    bool stepGated(uint32_t pairIndex, StepArgs &args); // the interpreter's step, counting m_dynamicPairs down
    // Inside a generated block (VU1_COMPILED_BLOCK_*), before its pair at pairIndex: the checks run()
    // makes before every compiled pair. false = return to run(), which continues from here itself.
    inline bool continueBlock(uint32_t pairIndex, StepArgs &args); // ps2_vu1_step.inl
    // A generated block (VU1_COMPILED_BLOCK_*): Infos = its pairs' VU1PairInfo, consecutive pair
    // indices. Runs exactly the pairs run() would, with the block-wide checks made once at entry.
    // kTrust != 0 (the program's signature): see m_normTrust; kNormLo/Hi = the lanes known at entry.
    template <uint64_t kTrust, uint64_t kNormLo, uint64_t kNormHi, class... Infos>
    inline bool runBlock(StepArgs &args); // ps2_vu1_step.inl
    template <uint32_t kPos, uint32_t kCount, bool kPrevMaySetViBackup, uint64_t kNormLo, uint64_t kNormHi, class Info, class... Rest>
    inline bool runBlockFrom(StepArgs &args);
    template <class Info>
    inline bool stepInfo(StepArgs &args); // one pair of a block the way run() would run it
    // Non-sequential entry into a generated pair: m_dynamicPairs unless every merged program that
    // contains the pair also contains the branch and its delay slot (so it knows the edge).
    void checkCompiledEntry(uint32_t pairIndex, uint32_t sequentialPc, uint32_t codeSize);
    static constexpr DecodedInstructionPair filterReady(DecodedInstructionPair decoded, uint32_t vfKeep, uint32_t viKeep, uint32_t accKeep)
    {
        DecodedInstructionPair out = decoded;
        out.readyVfCount = 0;
        out.readyViCount = 0;
        for (uint32_t i = 0; i < decoded.readyVfCount; ++i)
            if ((vfKeep >> i) & 1u)
                out.readyVf[out.readyVfCount++] = decoded.readyVf[i];
        for (uint32_t i = 0; i < decoded.readyViCount; ++i)
            if ((viKeep >> i) & 1u)
                out.readyVi[out.readyViCount++] = decoded.readyVi[i];
        out.readyAccComponents = static_cast<uint8_t>(decoded.readyAccComponents & accKeep);
        return out;
    }
    // kFm: a generated pair's fmacNorm | fmacZero << 8 for applyFmacDestFast, -1 = from the members.
    template <int kUpperHandler, int kFm = -1>
    inline void execUpperStep(uint32_t instr);
    template <int kHandler, int kFm = -1>
    inline void callUpperHandler(uint32_t instr); // ps2_vu1_upper.inl
    template <bool kInlineExec>
    inline void execLowerStep(uint32_t instr, StepArgs &args, uint32_t upperInstr);
    void tracePair(const DecodedInstructionPair &decoded); // PS2X_VU_TRACE
    // Cold parts of stepPair, out of line so every generated pair stays small.
    void waitUntilReady(const DecodedInstructionPair &decoded, uint64_t budgetEnd);
    void retireQueuedWrites(const DecodedInstructionPair &decoded, const float *oldUpperVf, const float *oldLowerVf,
                            const float *oldAcc, uint8_t writtenVi, int32_t oldVi);

    // Statically recompiled VU1 programs (ps2_vu1_compiled.h): one function per pair.
    using CompiledPairFn = bool (*)(VU1Core &core, StepArgs &args);
    // After a block returned true: run()'s next loop iteration up to its call, i.e. the compiled
    // function run() would call next (the block tail-calls it), or nullptr = return to run().
    inline CompiledPairFn chainNext(StepArgs &args); // ps2_vu1_step.inl
    // Makes the pair at pc compiled if a registered program that contains it matches the code
    // (ps2_vu1_compiled.cpp); every pair of a matching program is merged into m_compiledPairs.
    void lookupCompiledPair(const uint8_t *vuCode, uint32_t codeSize, uint32_t pc);
    // One decoded code image (rebuildDecodedCodeCache). Everything in it depends only on the code
    // bytes: the decoded pairs with their flag liveness, and the compiled pair functions by pair
    // index, the programs already checked against this code, the pcs where none matched. Images are
    // kept (keyed by hash + full compare of the code), so microprograms that alternate in VU1 code
    // memory do not decode, analyse and look up their compiled programs again on every switch.
    struct DecodedImage
    {
        std::vector<uint8_t> code;
        uint64_t hash = 0;
        uint32_t codeSize = 0;
        uint64_t lastUse = 0;
        std::array<DecodedInstructionPair, kMaxDecodedPairs> pairs{};
        std::array<CompiledPairFn, kMaxDecodedPairs> compiledPairs{};
        std::unordered_set<const void *> compiledChecked;
        std::unordered_set<uint32_t> compiledMissed;
        // Pair sets of the merged programs (bit i = pair i), for checkCompiledEntry, and per pair
        // the last branch pair found known to all of them (cache, reset on every merge).
        std::vector<std::array<uint64_t, kMaxDecodedPairs / 64u>> mergedReach;
        std::array<uint16_t, kMaxDecodedPairs> knownEntrySource{};
    };
    static constexpr uint32_t kMaxDecodedImages = 16u;
    std::vector<std::unique_ptr<DecodedImage>> m_decodedImages;
    uint64_t m_decodedImageUse = 0;
    DecodedImage *m_image = nullptr;          // the current code image (valid with m_decodedCodeCacheValid)
    CompiledPairFn *m_compiledPairs = nullptr; // m_image->compiledPairs

    static constexpr InstructionUsage decodeUpperUsage(uint32_t upper);
    static constexpr InstructionUsage decodeLowerUsage(uint32_t lower, Unit unit);
    static constexpr void addVfRead(InstructionUsage &usage, uint8_t reg, uint8_t lanes);
    static constexpr void addVfWrite(InstructionUsage &usage, uint8_t reg, uint8_t lanes);
    static constexpr uint8_t vfReadLanes(const InstructionUsage &usage, uint8_t reg);
    // The interpreter's decode of one pair (lower word at pc, upper at pc + 4); flagsLive stays true
    // (computeFlagLiveness sets it per code image).
    static constexpr DecodedInstructionPair decodePairWords(uint32_t lower, uint32_t upper, Unit unit);
    DecodedInstructionPair decodeInstructionPair(const uint8_t *vuCode, uint32_t pc) const;
    const DecodedInstructionPair &getDecodedInstructionPairForPc(const uint8_t *vuCode, uint32_t codeSize, PS2Memory *memory, uint32_t pc);
    void rebuildDecodedCodeCache(const uint8_t *vuCode, uint32_t codeSize, const PS2Memory *memory, uint64_t generation);

    void execUpper(uint32_t instr);
    void execLower(uint32_t instr, uint8_t *vuData, uint32_t dataSize, GS &gs, PS2Memory *memory, uint32_t upperInstr);
    // The same, force-inlined (ps2_vu1_upper.inl / ps2_vu1_lower.inl).
    inline void execUpperInline(uint32_t instr);
    inline void execLowerInline(uint32_t instr, uint8_t *vuData, uint32_t dataSize, GS &gs, PS2Memory *memory, uint32_t upperInstr);
#include "ps2_vu1_upper_ops.inc"

    // The dest lanes of result into dst as one 16-byte store (unwritten lanes keep dst's bits).
    // Lane-by-lane stores made the next 16-byte load of the register (an operand, or a pair's old
    // value) wait for them to retire: a load spanning several smaller stores can't be forwarded.
    void applyDest(float *dst, const float *result, uint8_t dest)
    {
        if (dest == 0u)
            return;
        const __m128 value = _mm_loadu_ps(result);
        if (dest == 0xFu)
        {
            _mm_storeu_ps(dst, value);
            return;
        }
        _mm_storeu_ps(dst, _mm_blendv_ps(_mm_loadu_ps(dst), value, vu1detail::destLaneMask(dest)));
    }
    void applyDestAcc(const float *result, uint8_t dest) { applyDest(m_state.acc, result, dest); }
    // applyFmacDest with the common cases inline (ps2_vu1_upper.inl): dead flags of a single-rounded
    // op, and live flags whose lanes all pass the fast checks. Everything else goes to applyFmacDest.
    // vs/vt/acc: the instruction's normalized operands (VU1_UPPER_OPERANDS), q/i normalized too.
    template <int kFm = -1>
    inline void applyFmacDestFast(float *dst, float *result, uint8_t dest, uint32_t instr,
                                  const float *vs, const float *vt, const float *acc, float q, float i);
    void applyFmacDest(float *dst, float *result, uint8_t dest);
    void applyFmacDestAcc(float *result, uint8_t dest);
    void applyFmacDestExact(float *dst, float *result, uint8_t dest);
    void applyFmacDestAccExact(float *result, uint8_t dest);
    void normalizeFmacResult(float *result, uint8_t dest, uint8_t laneFlags[4]);
    bool calculateFmacExactResult(uint32_t component, long double &result) const;
    uint8_t normalizeFmacExactResult(float &value, long double exactResult) const;
    uint32_t calculateFmacProductSticky(uint8_t dest) const;
    void updateFmacFlags(const uint8_t laneFlags[4], uint8_t dest, uint32_t extraSticky);
    void queueFsset(uint16_t immediate);
    void queueClip(uint32_t clip);
    void queueFcset(uint32_t clip);
    void queueQ(float value, uint32_t latency, uint32_t statusDi);
    void queueP(float value, uint32_t latency);
    void queueStore(uint32_t address, const uint32_t words[4], uint8_t laneMask);
    inline void storeQword(uint32_t address, const uint32_t words[4], uint8_t laneMask); // ps2_vu1_lower.inl
    void queueVfWrite(uint8_t reg, uint8_t laneMask, const float value[4], uint32_t latency);
    void queueViWrite(uint8_t reg, int32_t value, uint32_t latency);
    void queueAccWrite(uint8_t laneMask, const float value[4], uint32_t latency);
    void startXgkick(uint32_t qwordAddress);

    void resetScheduler();
    VU1_NOINLINE void commitReadyPipelines();
    // Per-pair commit (ps2_vu1_step.inl): only flag entries pending -> commitFlagsOnly, else the
    // noinline commitReadyPipelines, so the common case skips its register save/restore.
    VU1_FORCEINLINE void commitDue();
    VU1_FORCEINLINE void commitFlagsOnly();
    VU1_NOINLINE void commitFlagPipeline(); // commitFlagsOnly out of line (VU1_COMMIT_LEAF=1)
    void advanceOneCycle();
    void advanceTo(uint64_t targetCycle);
    void flushPipelines();
    bool storePending() const;
    uint64_t latestPendingReadyCycle() const;
    void progressXgkick();
    void finishXgkick();
    template <class Src = vu1detail::DynamicPair>
    inline uint64_t calculatePairReadyCycle(const DecodedInstructionPair &decoded) const; // ps2_vu1_step.inl
    template <uint32_t kElide = 0u, class Src = vu1detail::DynamicPair>
    inline void markPairWrites(const DecodedInstructionPair &decoded);
    bool pipelinesPending() const;

    float normalizeOperand(float value) const;
    float normalizeResult(float value, uint32_t &laneFlags) const;
    uint32_t microAddressMask() const { return m_unit == Unit::VU1 ? 0x3FFFu : 0x0FFFu; }
    // Inline: generated pairs call these on every VI write feeding a branch and every branch.
    int32_t readBranchVi(uint8_t reg) const
    {
        if (reg == 0u)
            return 0;
        if (m_viBranchBackupValid && m_viBranchBackupReg == reg)
            return m_viBranchBackupValue;
        return m_state.vi[reg];
    }
    void recordViWriteForBranch(uint8_t reg, int32_t oldValue)
    {
        if (reg == 0u)
            return;
        m_viBranchBackupValue = oldValue;
        m_viBranchBackupReg = reg;
        m_viBranchBackupValid = true;
    }
    void reportReservedInstruction(bool upper, uint32_t instruction);
    void badKickDiag(const char *why, uint64_t tagLo); // PS2X_VU1_BADKICK
    float broadcast(const float *vf, uint8_t bc);
};

#include "ps2_vu1_decode.inl"

#endif
