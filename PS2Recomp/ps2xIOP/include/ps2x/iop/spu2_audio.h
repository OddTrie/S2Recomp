#pragma once

#include <cstdint>

namespace ps2x::iop
{
    // Mixed SPU2 output: 48 kHz stereo, interleaved int16 (L, R), produced on the IOP timeline as
    // emulated time advances (called on the thread that runs the IOP). The sink must never block.
    // With no sink set the SPU2 only runs its voice clock (no decoding or mixing).
    using Spu2AudioSink = void (*)(const int16_t *interleaved, uint32_t frames, void *user);
    void setSpu2AudioSink(Spu2AudioSink sink, void *user);
}
