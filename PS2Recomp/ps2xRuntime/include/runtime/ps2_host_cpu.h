#pragma once

// The host processors this process may run on (ps2_sampler.cpp). On Windows only the logical processors in the
// process affinity mask count, so `start /affinity F ps2EntryRunner.exe` behaves like a 2-core/4-thread PC
// (std::thread::hardware_concurrency() reports the whole machine). Thread-count decisions (GS bands, the VU1
// worker's own core, frame generation, the GS feed spin) use this. Timing only.
struct Ps2xHostCpu
{
    int logical = 1;  // usable logical processors
    int physical = 1; // usable physical cores
    int fast = 1;     // usable physical cores of the fastest efficiency class (P-cores on a hybrid CPU)
};

const Ps2xHostCpu &ps2xHostCpu();
