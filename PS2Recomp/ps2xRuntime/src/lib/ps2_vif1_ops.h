// VIF1 -> VU1 operation list for the two-thread VU1 worker (PS2X_VU1_THREADS=2, ps2_memory.cpp).
// The front thread runs the VIF1 command stream (VIF1 registers, UNPACK decoding, DIRECT/IMAGE
// splitting) without touching VU1 memory, the GIF arbiter or VU1; everything that does goes into
// this list, in stream order, and the VU1 thread applies it. UNPACK results are memory-independent
// (lanes the format, mask or write-protect leave alone are a lane mask, not a read of VU memory), so
// the VU1 thread sees VU1 memory exactly as the serial interpreter would at every MSCAL. The one
// UNPACK that reads VU1 memory (MODE 1/2 row add on a lane the format does not supply) first syncs.
#pragma once

#include "runtime/ps2_memory.h"
#include <cstdint>
#include <vector>

enum class Vif1OpKind : uint8_t
{
    CopyVecs,  // ptr: count qwords to VU1 data qword `dest` (no wrap; the front splits runs)
    Lanes,     // inline: `count` Vif1LaneRec
    Bytes,     // ptr: `count` bytes to VU1 data byte offset `dest` (raw-copy fallback)
    Path2,     // ptr or inline: `count` bytes, DIRECT (mask = DIRECTHL)
    MaskPath3, // mask = new MSKPATH3 state
    Mscal,     // dest = start pc, lanes[0] = top, lanes[1] = itop
    Mscnt,     // lanes[0] = top, lanes[1] = itop
    Mpg,       // ptr: `count` bytes of VU1 code to byte offset `dest`
};

struct Vif1Op
{
    Vif1OpKind kind;
    uint8_t mask = 0;
    uint32_t dest = 0;
    uint32_t count = 0;
    uint32_t inlineOffset = UINT32_MAX; // != UINT32_MAX: the bytes are in Vif1OpList::inlineBytes
    const uint8_t *ptr = nullptr;       // into the item's payload (alive until the item's last list is applied)
    uint32_t lanes[2] = {};
};

// One UNPACK qword write: only the lanes in `mask` (bit n = lane n) change.
struct Vif1LaneRec
{
    uint16_t destVec;
    uint8_t mask;
    uint8_t pad;
    uint32_t lanes[4];
};

struct Vif1OpList
{
    std::vector<Vif1Op> ops;
    std::vector<uint8_t> inlineBytes; // Vif1LaneRec runs and packets the front builds (IMAGE continuation)

    const uint8_t *bytes(const Vif1Op &op) const { return op.inlineOffset != UINT32_MAX ? inlineBytes.data() + op.inlineOffset : op.ptr; }
    bool empty() const { return ops.empty(); }
};

// The VIF1 state the front owns (for PS2X_VU1_THREADS_VERIFY).
struct Vif1FrontState
{
    VIFRegisters regs;
    uint32_t pendingImageQwc;
    bool pendingDirectHl;
};
Vif1FrontState ps2xVif1FrontState(const PS2Memory &mem);

struct Vif1DecodeHooks
{
    // Hands the ops so far to the VU1 thread and returns once it has applied everything (then `out` is
    // empty and VU1 memory may be read): only for an UNPACK that reads VU1 memory.
    void (*sync)(Vif1OpList &) = nullptr;
    // Hands the ops so far to the VU1 thread without waiting (after MSCALs, so VU1 starts before the
    // whole transfer is decoded); nullptr = one list per transfer.
    void (*flush)(Vif1OpList &) = nullptr;
    uint32_t flushEveryMscal = 4;
    uint32_t syncs = 0; // out: how many syncs this transfer needed
};

// ps2_vif1_interpreter.cpp: the front half of PS2Memory::processVIF1Data.
void ps2xVif1DecodeOps(PS2Memory &mem, const uint8_t *data, uint32_t sizeBytes, Vif1OpList &out, Vif1DecodeHooks &hooks);
// ps2_vif1_interpreter.cpp: the back half (VU1 thread).
void ps2xVif1ApplyOps(PS2Memory &mem, const Vif1OpList &list);
// PS2X_VU1_THREADS_VERIFY=1 (VU1 thread): applies `list` (one whole transfer) like ps2xVif1ApplyOps while
// running the serial interpreter on a shadow copy of VU1 memory and the VIF1 state, comparing VU1 memory,
// MPG code, DIRECT packets, MSKPATH3 and MSCAL/MSCNT arguments at every one of those, and the VIF1 state
// at the end. Returns the number of mismatches.
uint32_t ps2xVif1VerifyOps(PS2Memory &mem, const uint8_t *data, uint32_t sizeBytes, const Vif1OpList &list,
                           const Vif1FrontState &start, const Vif1FrontState &end);
