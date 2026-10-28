#include "runtime/ps2_memory.h"
#include "runtime/ps2_address.h"
#include "runtime/gs/gs_frontend.h"
#include "ps2_log.h"
#include <atomic>
#include <cstring>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <algorithm>
#include <string>
#include <vector>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <unordered_map>
#include <functional>
#include <immintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
#include <mutex>
#include <thread>
#include "ThreadNaming.h"
#include "ps2_vif1_ops.h"

// ---------------------------------------------------------------------------------------------
// Asynchronous GIF/VIF1 DMA (PS2X_VIF1_ASYNC=0 = everything on the EE thread, as before).
// DMA completion is reported to the game at kick time either way; the payload (already copied out
// of RAM at kick time) used to be processed there too, so the EE thread ran every VU1 microprogram
// the transfer started (~70% of its time in 3D scenes). Now the payloads go, in kick order, to one
// worker thread that runs VIF1 (UNPACK/MPG/MSCAL -> VU1 -> XGKICK) and PATH3 into the GIF arbiter,
// while the EE carries on. Everything the worker touches (VIF1 state, VU1 memory, PATH3 mask, GIF
// arbiter, GS packet stream) is only accessed by the EE after vif1AsyncDrain(): VU1 memory and
// VIF1/GIF register/FIFO access, GS CSR, the native GIF fast paths and the GS HLE stubs.
// ---------------------------------------------------------------------------------------------
void ps2xReserveCoreForCurrentThread(); // ps2_sampler.cpp: PS2X_VU1_OWN_CORE

// Host ns the VIF1 worker spent waiting for work, for the adaptive 60 FPS gate (ps2_runtime.cpp).
// g_ps2xVu1IdleSince: steady_clock ns when the current wait began, 0 while working, so a reader can count
// a wait still in progress (a long one would otherwise land entirely in the window where it ends).
std::atomic<int64_t> g_ps2xVu1IdleNs{0};
std::atomic<int64_t> g_ps2xVu1IdleSince{0};

int64_t ps2xVu1IdleNsNow()
{
    const int64_t since = g_ps2xVu1IdleSince.load(std::memory_order_acquire);
    const int64_t done = g_ps2xVu1IdleNs.load(std::memory_order_acquire);
    if (since == 0)
        return done;
    const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    return done + (now > since ? now - since : 0);
}

// PS2X_60FPS_LOG=1: host ns the EE spent waiting in vif1AsyncDrain (mid-frame syncs with the VIF1
// worker and the GS feed, not the VBlank wait), per call site; printed as a [drain] line per window.
namespace
{
    enum DrainSite : int
    {
        kDrainOther, kDrainVuMem, kDrainCsr, kDrainIoReg, kDrainVifFifo, kDrainGifPacket, kDrainNativeGif, kDrainHle,
        kDrainSiteCount
    };
    const char *const kDrainSiteNames[kDrainSiteCount] = {"other", "vumem", "csr", "ioreg", "viffifo", "gifpkt", "nativegif", "hle"};
    std::atomic<int64_t> g_drainNs[kDrainSiteCount];
    std::atomic<uint64_t> g_drainWaits[kDrainSiteCount];
}

// One window's drain waits as text ("csr 3.1 ms/12 ..."), sites without waits left out; resets the counters.
// Returns the total wait in ms.
double ps2xDrainStatsTake(char *buf, size_t size)
{
    double total = 0.0;
    size_t n = 0;
    if (size)
        buf[0] = 0;
    for (int i = 0; i < kDrainSiteCount; ++i)
    {
        const int64_t ns = g_drainNs[i].exchange(0, std::memory_order_relaxed);
        const uint64_t waits = g_drainWaits[i].exchange(0, std::memory_order_relaxed);
        if (!waits)
            continue;
        total += static_cast<double>(ns) / 1.0e6;
        if (n < size)
            n += std::snprintf(buf + n, size - n, " %s %.1f ms/%llu", kDrainSiteNames[i], static_cast<double>(ns) / 1.0e6,
                               static_cast<unsigned long long>(waits));
    }
    return total;
}

namespace
{
    void vu1IdleBegin(std::chrono::steady_clock::time_point t)
    {
        g_ps2xVu1IdleSince.store(std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count(), std::memory_order_release);
    }

    void vu1IdleEnd(int64_t ns)
    {
        g_ps2xVu1IdleNs.fetch_add(ns, std::memory_order_release);
        g_ps2xVu1IdleSince.store(0, std::memory_order_release);
    }
}

namespace
{
    enum class AsyncItemKind : uint8_t
    {
        Gif,
        Vif1,
        ArbiterDrain,
        CsrClear, // GS CSR SIGNAL/FINISH write-one-to-clear, in packet order
    };

    struct AsyncItem
    {
        AsyncItemKind kind;
        std::vector<uint8_t> data;
        uint64_t value = 0;
    };

    struct Vif1AsyncWorker
    {
        PS2Memory *owner = nullptr;
        std::thread thread;
        std::mutex mutex;
        std::condition_variable wake;
        std::condition_variable idle;
        std::deque<AsyncItem> queue;
        size_t queuedBytes = 0;
        bool busy = false;
        bool stop = false;
        std::atomic<uint32_t> pending{0}; // items queued or running
        uint64_t pushedSeq = 0;           // items pushed so far (EE thread)
        uint64_t doneSeq = 0;             // items finished (under mutex)
        uint64_t vblankMarks[4] = {};     // pushedSeq at the last VBlanks (ring, PS2X_VIF1_VBLANK_DEPTH)
        uint64_t vblankCount = 0;
        int64_t idleNs = 0;               // host time spent waiting for work (under mutex; PS2X_FRAME_LOG=2)
        // Backlog estimate for the 60 FPS pacing (ps2xVif1BacklogMs; one-thread mode, under mutex):
        size_t curBytes = 0;              // payload of the item running now
        int64_t curStartNs = 0;           // steady_clock ns when it started
        int64_t busyNsAcc = 0;            // run time of finished items since the last estimate
        uint64_t bytesAcc = 0;            // and their payload
        double nsPerByte = 0.0;           // smoothed run time per payload byte
        std::unordered_map<size_t, int64_t> nsBySize; // last run time of an item of about this size (sizeBucket)
    };

    // ~1.5% wide payload size classes: the game sends nearly the same transfers every frame.
    inline size_t sizeBucket(size_t bytes)
    {
        unsigned bits = 0;
        for (size_t v = bytes; v > 63u; v >>= 1)
            ++bits;
        return (static_cast<size_t>(bits) << 6) | (bytes >> bits);
    }

    inline int64_t steadyNs()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    constexpr size_t kVif1AsyncMaxQueuedBytes = 32u * 1024u * 1024u;

    Vif1AsyncWorker *g_vif1Async = nullptr;
    thread_local bool t_onVif1AsyncWorker = false;

    // PS2X_VU1_TIMING=1: where the VIF1 worker's time goes. rdtsc, exclusive per category (a nested
    // category's time is not counted in its parent); one [vu1time] line per second, in ms per second.
    enum WorkerCat : int
    {
        kCatLoop,     // queue handling, freeing payloads
        kCatIdle,     // waiting for work
        kCatVif,      // processVIF1Data itself (commands, UNPACK, MPG)
        kCatVu,       // VU1 microprograms (execute / resume)
        kCatKick,     // XGKICK packets: GIF arbiter submit + drain
        kCatPath2,    // VIF1 DIRECT packets
        kCatPath3,    // PATH3 (GIF DMA) packets
        kCatFeedPush, // copying a packet into the GS feed queue
        kCatFeedWait, // GS feed queue full
        kCatCount
    };
    struct WorkerTiming
    {
        uint64_t mark = 0;
        int stack[16] = {};
        int depth = 0;
        uint64_t ticks[kCatCount] = {};
        uint64_t enters[kCatCount] = {};
        uint64_t reportTsc = 0;
        std::chrono::steady_clock::time_point reportTime;
        const char *tag = "";
    };
    thread_local WorkerTiming *t_timing = nullptr;

    bool workerTimingEnabled()
    {
        static const bool on = [] { const char *e = std::getenv("PS2X_VU1_TIMING"); return e && *e == '1'; }();
        return on;
    }

    inline void timingEnter(int cat)
    {
        WorkerTiming *t = t_timing;
        if (!t || t->depth >= 15)
            return;
        const uint64_t now = __rdtsc();
        t->ticks[t->stack[t->depth]] += now - t->mark;
        t->mark = now;
        t->stack[++t->depth] = cat;
        ++t->enters[cat];
    }

    inline void timingLeave()
    {
        WorkerTiming *t = t_timing;
        if (!t || t->depth == 0)
            return;
        const uint64_t now = __rdtsc();
        t->ticks[t->stack[t->depth]] += now - t->mark;
        t->mark = now;
        --t->depth;
    }

    void timingStart(const char *tag)
    {
        if (!workerTimingEnabled())
            return;
        t_timing = new WorkerTiming();
        t_timing->mark = t_timing->reportTsc = __rdtsc();
        t_timing->reportTime = std::chrono::steady_clock::now();
        t_timing->tag = tag;
    }

    // PS2X_VU1_HASH=<n>: an exactness check for the worker modes. FNV-1a over every packet the worker
    // hands to the GS feed; every n-th finished item (default 20000) prints the item number, the packet
    // count, that hash and a hash of VU1 data memory. The EE side is deterministic (emulated clock,
    // PS2X_AUTOPAD_CLOCK=vblank), so runs and modes must print the same lines.
    uint64_t g_vu1HashEvery = []
    {
        const char *e = std::getenv("PS2X_VU1_HASH");
        if (!e || !*e || *e == '0')
            return uint64_t{0};
        const uint64_t n = std::strtoull(e, nullptr, 0);
        return n > 1u ? n : uint64_t{20000};
    }();
    uint64_t g_vu1PacketHash = 1469598103934665603ull;
    uint64_t g_vu1PacketCount = 0;

    inline uint64_t fnv1a(uint64_t h, const uint8_t *p, size_t n)
    {
        for (size_t i = 0; i < n; ++i)
            h = (h ^ p[i]) * 1099511628211ull;
        return h;
    }

    // Called where an item is finished (the VU1 side), doneSeq already counted.
    void vu1HashCheckpoint(PS2Memory &mem, uint64_t doneSeq)
    {
        if (g_vu1HashEvery == 0u || doneSeq % g_vu1HashEvery != 0u)
            return;
        const uint64_t dataHash = mem.m_vu1Data ? fnv1a(1469598103934665603ull, mem.m_vu1Data, PS2_VU1_DATA_SIZE) : 0u;
        std::fprintf(stderr, "[vu1hash] item %llu packets %llu gs %016llx vu1data %016llx\n", (unsigned long long)doneSeq,
                     (unsigned long long)g_vu1PacketCount, (unsigned long long)g_vu1PacketHash, (unsigned long long)dataHash);
    }

    struct TimingScope
    {
        explicit TimingScope(int cat) { timingEnter(cat); }
        ~TimingScope() { timingLeave(); }
    };

    // Between items: print and reset once a second has passed.
    void timingMaybeReport()
    {
        WorkerTiming *t = t_timing;
        if (!t)
            return;
        const auto now = std::chrono::steady_clock::now();
        const double sec = std::chrono::duration<double>(now - t->reportTime).count();
        if (sec < 1.0)
            return;
        const uint64_t tsc = __rdtsc();
        t->ticks[t->stack[t->depth]] += tsc - t->mark;
        t->mark = tsc;
        const double msPerTick = 1000.0 / static_cast<double>(tsc - t->reportTsc); // ms per second
        static const char *names[kCatCount] = {"loop", "idle", "vif", "vu", "kick", "path2", "path3", "feedpush", "feedwait"};
        char line[512];
        int n = std::snprintf(line, sizeof(line), "[vu1time%s] %.2f s:", t->tag, sec);
        double busy = 0.0;
        for (int c = 0; c < kCatCount; ++c)
        {
            const double ms = static_cast<double>(t->ticks[c]) * msPerTick;
            if (c != kCatIdle)
                busy += ms;
            n += std::snprintf(line + n, sizeof(line) - n, " %s %.1f", names[c], ms);
        }
        std::snprintf(line + n, sizeof(line) - n, " | busy %.1f | runs %llu kicks %llu items %llu\n", busy,
                      (unsigned long long)t->enters[kCatVu], (unsigned long long)t->enters[kCatKick], (unsigned long long)t->enters[kCatVif]);
        std::fputs(line, stderr);
        for (int c = 0; c < kCatCount; ++c)
            t->ticks[c] = t->enters[c] = 0;
        t->reportTsc = tsc;
        t->reportTime = now;
    }

    bool vif1AsyncEnabled()
    {
        static const bool enabled = []
        {
            const char *e = std::getenv("PS2X_VIF1_ASYNC");
            return !(e && *e == '0');
        }();
        return enabled;
    }

    void gsFeedCsrClear(PS2Memory &mem, uint64_t mask);
    void gsFeedFlushBatch();

    // PS2X_GIF_MOVE=0: copy worker PATH3 packets into the arbiter (old path) instead of moving them.
    bool gifPacketMove()
    {
        static const bool on = []
        {
            const char *e = std::getenv("PS2X_GIF_MOVE");
            return !(e && *e == '0');
        }();
        return on;
    }

    void runVif1AsyncItem(PS2Memory &mem, AsyncItem &item)
    {
        TimingScope scope(item.kind == AsyncItemKind::Vif1 ? kCatVif : kCatLoop);
        switch (item.kind)
        {
        case AsyncItemKind::Gif:
            if (gifPacketMove())
                mem.submitPath3PacketOwned(std::move(item.data));
            else
                mem.submitGifPacket(GifPathId::Path3, item.data.data(), static_cast<uint32_t>(item.data.size()), false);
            break;
        case AsyncItemKind::Vif1:
            mem.processVIF1Data(item.data.data(), static_cast<uint32_t>(item.data.size()));
            break;
        case AsyncItemKind::ArbiterDrain:
            if (mem.m_gifArbiter)
                mem.m_gifArbiter->drain();
            break;
        case AsyncItemKind::CsrClear:
            gsFeedCsrClear(mem, item.value); // after the packets still in the GS feed
            break;
        }
    }

    // ---------------------------------------------------------------------------------------------
    // Two-thread VU1 worker (PS2X_VU1_THREADS=2): the Vif1Worker thread (front) runs the VIF1 command
    // stream into an op list (ps2_vif1_ops.h) and the Vu1Worker thread (back) applies the lists in
    // order: UNPACK writes, MPG, DIRECT/PATH3 packets, the microprograms and their XGKICKs. The back
    // thread does exactly what the serial worker did, in the same order, minus VIF1 command decoding;
    // the front only owns the VIF1 registers, which the EE reads after a drain (which waits for the
    // back). An item counts as done (doneSeq, pending) when the back has finished it.
    // ---------------------------------------------------------------------------------------------
    struct Vu1Batch
    {
        AsyncItemKind kind;
        std::vector<uint8_t> data; // the item's payload (the ops point into it)
        uint64_t value = 0;
        Vif1OpList ops;
        bool hasOps = false;
        bool completesItem = true; // false: part of a transfer (streamed after MSCALs, or a sync)
        bool streamed = false;     // a list handed over after MSCALs (not a sync)
        bool verify = false;       // PS2X_VU1_THREADS_VERIFY: the whole transfer, check it against the serial interpreter
        Vif1FrontState start{}, end{};
        size_t bytes = 0;
    };

    struct Vu1Back
    {
        Vif1AsyncWorker *worker = nullptr;
        std::thread thread;
        std::mutex mutex;
        std::condition_variable wake;
        std::condition_variable idle;
        std::deque<Vu1Batch> queue;
        std::vector<Vu1Batch> garbage; // finished batches, freed by the front (keeps frees off the VU1 thread)
        size_t queuedBytes = 0;
        uint64_t pushed = 0, done = 0;
        bool stop = false;
    };

    constexpr size_t kVu1BackMaxQueuedBytes = 16u * 1024u * 1024u;

    Vu1Back *g_vu1Back = nullptr;

    // PS2X_VU1_THREADS=2: front + VU1 thread; 1 (default) = the serial worker (VIF1 and VU1 on one thread).
    // Default from measurements (2026-10-21): microprograms are ~90% of the worker, the VU1 thread did as
    // much work per frame as the serial worker on 4 E-cores, 2P/4T and the 14900K, so no FPS gain anywhere.
    bool vu1TwoThreads()
    {
        static const bool two = []
        {
            const char *e = std::getenv("PS2X_VU1_THREADS");
            if (e && *e)
                return std::atoi(e) >= 2;
            return false;
        }();
        return two;
    }

    // PS2X_VU1_THREADS_VERIFY=1: every transfer is checked on the VU1 thread (slow; one list per transfer).
    bool vu1VerifyOn()
    {
        static const bool on = [] { const char *e = std::getenv("PS2X_VU1_THREADS_VERIFY"); return e && *e == '1'; }();
        return on;
    }

    // PS2X_VU1_STREAM=<n>: the front hands the ops over after the first MSCAL of a transfer and then every
    // n MSCALs (default 4); 0 = one list per transfer.
    uint32_t vu1StreamEvery()
    {
        static const uint32_t n = [] { const char *e = std::getenv("PS2X_VU1_STREAM"); return e && *e ? static_cast<uint32_t>(std::atoi(e)) : 4u; }();
        return n;
    }

    void vu1BackPush(Vu1Back *b, Vu1Batch batch)
    {
        batch.bytes = batch.data.size() + batch.ops.ops.size() * sizeof(Vif1Op) + batch.ops.inlineBytes.size();
        std::vector<Vu1Batch> garbage;
        std::unique_lock<std::mutex> lock(b->mutex);
        garbage.swap(b->garbage);
        if (b->queuedBytes >= kVu1BackMaxQueuedBytes)
        {
            TimingScope waitScope(kCatFeedWait);
            b->idle.wait(lock, [b] { return b->queuedBytes < kVu1BackMaxQueuedBytes; });
        }
        b->queuedBytes += batch.bytes;
        b->queue.push_back(std::move(batch));
        ++b->pushed;
        b->wake.notify_one();
        lock.unlock();
        garbage.clear();
    }

    // Vif1DecodeHooks flush (front): the ops so far go to the back; the transfer's payload stays with the front
    // until its last list is pushed, which the back applies after this one.
    void vu1BackFlush(Vif1OpList &list)
    {
        if (list.empty())
            return;
        Vu1Batch batch{AsyncItemKind::Vif1};
        batch.ops = std::move(list);
        list = Vif1OpList{};
        batch.hasOps = true;
        batch.completesItem = false;
        batch.streamed = true;
        vu1BackPush(g_vu1Back, std::move(batch));
    }

    // PS2X_VU1_THREADS_VERIFY with streaming: the VU1 thread collects a transfer's lists and checks them
    // together when the last one arrives (ops point into the payload; inline offsets are rebased).
    void appendOps(Vif1OpList &to, const Vif1OpList &from)
    {
        const uint32_t base = static_cast<uint32_t>(to.inlineBytes.size());
        for (Vif1Op op : from.ops)
        {
            if (op.inlineOffset != UINT32_MAX)
                op.inlineOffset += base;
            to.ops.push_back(op);
        }
        to.inlineBytes.insert(to.inlineBytes.end(), from.inlineBytes.begin(), from.inlineBytes.end());
    }

    // Vif1OpList sync (front): the ops so far go to the back, then wait until it has applied everything.
    void vu1BackSync(Vif1OpList &list)
    {
        Vu1Back *b = g_vu1Back;
        Vu1Batch batch{AsyncItemKind::Vif1};
        batch.ops = std::move(list);
        list = Vif1OpList{};
        batch.hasOps = true;
        batch.completesItem = false;
        vu1BackPush(b, std::move(batch));
        {
            std::unique_lock<std::mutex> lock(b->mutex);
            b->idle.wait(lock, [b] { return b->done == b->pushed; });
        }
        static std::atomic<uint64_t> s_syncs{0};
        const uint64_t n = ++s_syncs;
        if ((n & (n - 1u)) == 0u)
            std::fprintf(stderr, "[vu1-threads] %llu front/VU1 syncs (MODE 1/2 UNPACK reading VU1 memory)\n", (unsigned long long)n);
    }

    void vu1BackLoop(Vu1Back *b)
    {
        ThreadNaming::SetCurrentThreadName("Vu1Worker");
        ThreadNaming::BoostCurrentThread();
        ps2xReserveCoreForCurrentThread(); // the VU1 thread is the one that limits the frame rate
        t_onVif1AsyncWorker = true;
        timingStart(" vu");
        Vif1AsyncWorker *w = b->worker;
        PS2Memory &mem = *w->owner;
        Vif1OpList verifyPending; // streamed lists of the current transfer, not applied yet (verify mode)
        bool verifyPendingValid = true;
        std::unique_lock<std::mutex> lock(b->mutex);
        for (;;)
        {
            if (b->queue.empty() && !b->stop)
            {
                const auto idleStart = std::chrono::steady_clock::now();
                vu1IdleBegin(idleStart);
                {
                    TimingScope idleScope(kCatIdle);
                    b->wake.wait(lock, [b] { return b->stop || !b->queue.empty(); });
                }
                const int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - idleStart).count();
                lock.unlock();
                {
                    std::lock_guard<std::mutex> wl(w->mutex);
                    w->idleNs += ns;
                }
                vu1IdleEnd(ns);
                lock.lock();
            }
            if (b->queue.empty())
                break;
            Vu1Batch batch = std::move(b->queue.front());
            b->queue.pop_front();
            lock.unlock();
            try
            {
                if (vu1VerifyOn() && batch.hasOps && !batch.completesItem)
                {
                    TimingScope scope(kCatVif);
                    if (batch.streamed && verifyPendingValid)
                        appendOps(verifyPending, batch.ops); // checked with the rest of the transfer
                    else
                    {
                        // A sync: the front reads VU1 memory next, so everything so far is applied now.
                        ps2xVif1ApplyOps(mem, verifyPending);
                        verifyPending = Vif1OpList{};
                        verifyPendingValid = false;
                        ps2xVif1ApplyOps(mem, batch.ops);
                    }
                }
                else if (vu1VerifyOn() && batch.hasOps && !batch.verify)
                {
                    TimingScope scope(kCatVif);
                    ps2xVif1ApplyOps(mem, verifyPending);
                    ps2xVif1ApplyOps(mem, batch.ops);
                    verifyPending = Vif1OpList{};
                    verifyPendingValid = true;
                }
                else if (batch.verify)
                {
                    TimingScope scope(kCatVif);
                    static uint64_t s_items = 0, s_bad = 0;
                    if (!verifyPending.empty())
                    {
                        appendOps(verifyPending, batch.ops);
                        batch.ops = std::move(verifyPending);
                    }
                    verifyPending = Vif1OpList{};
                    verifyPendingValid = true;
                    const uint32_t bad = ps2xVif1VerifyOps(mem, batch.data.data(), static_cast<uint32_t>(batch.data.size()), batch.ops, batch.start, batch.end);
                    s_bad += bad;
                    ++s_items;
                    if ((s_items & (s_items - 1u)) == 0u || (s_items % 5000u) == 0u)
                        std::fprintf(stderr, "[vu1-verify] %llu transfers checked, %llu mismatches\n", (unsigned long long)s_items, (unsigned long long)s_bad);
                }
                else if (batch.hasOps)
                {
                    TimingScope scope(kCatVif);
                    ps2xVif1ApplyOps(mem, batch.ops);
                }
                else
                {
                    AsyncItem item{batch.kind, std::move(batch.data), batch.value};
                    runVif1AsyncItem(mem, item);
                }
            }
            catch (const std::exception &e)
            {
                std::fprintf(stderr, "[vu1-threads] exception while processing a transfer: %s\n", e.what());
            }
            const bool completes = batch.completesItem;
            const size_t bytes = batch.bytes;
            if (completes)
                gsFeedFlushBatch();
            timingMaybeReport();
            if (completes)
            {
                std::lock_guard<std::mutex> wl(w->mutex);
                ++w->doneSeq;
                vu1HashCheckpoint(mem, w->doneSeq);
                w->pending.fetch_sub(1, std::memory_order_release);
                w->idle.notify_all();
            }
            lock.lock();
            b->garbage.push_back(std::move(batch));
            b->queuedBytes -= bytes;
            ++b->done;
            b->idle.notify_all();
        }
    }

    void vif1AsyncWorkerLoop(Vif1AsyncWorker *w)
    {
        ThreadNaming::SetCurrentThreadName("Vif1Worker");
        ThreadNaming::BoostCurrentThread(); // VU1 limits the frame rate in 3D scenes
        Vu1Back *back = nullptr;
        if (vu1TwoThreads())
        {
            back = new Vu1Back();
            back->worker = w;
            g_vu1Back = back;
            back->thread = std::thread(vu1BackLoop, back);
            std::fprintf(stderr, "[vu1-threads] VIF1 front + VU1 thread (PS2X_VU1_THREADS=1: one thread)\n");
        }
        else
            ps2xReserveCoreForCurrentThread();
        t_onVif1AsyncWorker = true;
        timingStart(back ? " front" : "");
        std::unique_lock<std::mutex> lock(w->mutex);
        for (;;)
        {
            if (w->queue.empty() && !w->stop)
            {
                const auto idleStart = std::chrono::steady_clock::now();
                if (!back)
                    vu1IdleBegin(idleStart);
                TimingScope idleScope(kCatIdle);
                w->wake.wait(lock, [w] { return w->stop || !w->queue.empty(); });
                if (!back)
                {
                    const int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - idleStart).count();
                    w->idleNs += ns;
                    vu1IdleEnd(ns);
                }
            }
            if (w->queue.empty())
                break; // stop requested and nothing left
            AsyncItem item = std::move(w->queue.front());
            w->queue.pop_front();
            w->queuedBytes -= item.data.size();
            w->busy = true;
            w->curBytes = item.data.size();
            w->curStartNs = steadyNs();
            lock.unlock();
            if (back)
            {
                Vu1Batch batch{item.kind, std::move(item.data), item.value};
                if (batch.kind == AsyncItemKind::Vif1)
                {
                    TimingScope scope(kCatVif);
                    const bool verify = vu1VerifyOn();
                    Vif1DecodeHooks hooks;
                    hooks.sync = vu1BackSync;
                    hooks.flush = vu1StreamEvery() == 0u ? nullptr : vu1BackFlush;
                    hooks.flushEveryMscal = vu1StreamEvery();
                    if (verify)
                        batch.start = ps2xVif1FrontState(*w->owner);
                    try
                    {
                        ps2xVif1DecodeOps(*w->owner, batch.data.data(), static_cast<uint32_t>(batch.data.size()), batch.ops, hooks);
                    }
                    catch (const std::exception &e)
                    {
                        std::fprintf(stderr, "[vu1-threads] exception while decoding a transfer: %s\n", e.what());
                    }
                    batch.hasOps = true;
                    if (verify && hooks.syncs == 0u)
                    {
                        batch.verify = true;
                        batch.end = ps2xVif1FrontState(*w->owner);
                    }
                }
                vu1BackPush(back, std::move(batch));
                timingMaybeReport();
                lock.lock();
                w->busy = false;
                continue;
            }
            try
            {
                runVif1AsyncItem(*w->owner, item);
            }
            catch (const std::exception &e)
            {
                std::fprintf(stderr, "[vif1-async] exception while processing a transfer: %s\n", e.what());
            }
            item.data = {};
            gsFeedFlushBatch();
            timingMaybeReport();
            lock.lock();
            {
                const int64_t ran = steadyNs() - w->curStartNs;
                w->busyNsAcc += ran;
                w->bytesAcc += w->curBytes;
                if (w->nsBySize.size() > 4096u)
                    w->nsBySize.clear();
                w->nsBySize[sizeBucket(w->curBytes)] = ran;
            }
            w->curBytes = 0;
            ++w->doneSeq;
            vu1HashCheckpoint(*w->owner, w->doneSeq);
            w->busy = false;
            w->pending.fetch_sub(1, std::memory_order_release);
            w->idle.notify_all();
        }
        if (back)
        {
            {
                std::lock_guard<std::mutex> bl(back->mutex);
                back->stop = true;
            }
            back->wake.notify_all();
            back->thread.join();
            g_vu1Back = nullptr;
            delete back;
        }
    }

    // ---------------------------------------------------------------------------------------------
    // GS feed (PS2X_GS_FEED_ASYNC=0 = off): the packets the GIF arbiter releases on the VIF1 worker
    // (PATH1 XGKICKs, PATH2, PATH3) go, in order, to one more thread that runs the GS frontend
    // (register writes, vertex kicks, draw batching, pushing to the raster backend), so the worker
    // only runs VIF1 and VU1. Packets are copied. Everything that waits for the worker (drains, CSR
    // clears) also waits for / goes through this queue.
    // ---------------------------------------------------------------------------------------------
    struct GsFeedItem
    {
        std::vector<uint8_t> packet;
        uint64_t csrClear = 0; // != 0: CSR SIGNAL/FINISH clear in packet order, no packet
        bool batched = false;  // packet = several packets, each [u32 size][bytes][pad to 4]
    };

    // PS2X_GS_FEED_BATCH=<bytes> (default 32768, 0 = one queue item per packet): the worker collects
    // the packets it hands to the GS feed and queues them together once that many bytes are waiting and
    // at the end of every worker item (before anything can wait for the worker), instead of one
    // allocation + lock + wake per packet (~55k XGKICK packets/s in a busy scene). Same packets, same order.
    size_t gsFeedBatchBytes()
    {
        static const size_t n = [] { const char *e = std::getenv("PS2X_GS_FEED_BATCH"); return e && *e ? static_cast<size_t>(std::strtoull(e, nullptr, 0)) : size_t{32768}; }();
        return n;
    }
    GsFeedItem g_gsFeedBatch; // the worker's packets not queued yet (only the VIF1/VU1 worker touches it)

    struct GsFeed
    {
        PS2Memory *owner = nullptr;
        std::function<void(const uint8_t *, uint32_t)> process;
        std::thread thread;
        std::mutex mutex;
        std::condition_variable wake;
        std::condition_variable idle;
        std::deque<GsFeedItem> queue;
        size_t queuedBytes = 0;
        bool busy = false;
        bool stop = false;
        std::atomic<uint32_t> pending{0};
        std::vector<std::vector<uint8_t>> spare; // processed batch buffers kept for reuse (under mutex)
    };

    constexpr size_t kGsFeedMaxQueuedBytes = 16u * 1024u * 1024u;

    // PS2X_GS_FEED_RECYCLE=0 = off: the feed thread hands processed batch buffers back to the worker
    // instead of freeing them, so the worker does not allocate (and fault in) a fresh ~96 KB buffer for
    // every 32 KB batch. Same packets, same order.
    bool gsFeedRecycle()
    {
        static const bool on = [] { const char *e = std::getenv("PS2X_GS_FEED_RECYCLE"); return !(e && *e == '0'); }();
        return on;
    }

    GsFeed *g_gsFeed = nullptr;
    std::function<void(const uint8_t *, uint32_t)> g_gsFeedProcessor; // set by the runtime
    thread_local bool t_onGsFeed = false;

    bool gsFeedEnabled()
    {
        static const bool enabled = []
        {
            const char *e = std::getenv("PS2X_GS_FEED_ASYNC");
            return !(e && *e == '0');
        }();
        return enabled;
    }

    void gsFeedLoop(GsFeed *f)
    {
        ThreadNaming::SetCurrentThreadName("GsFeed");
        t_onGsFeed = true;
        std::unique_lock<std::mutex> lock(f->mutex);
        // PS2X_GS_FEED_SPIN=n: pause loops to wait for the next packet before sleeping (0 = sleep at
        // once). The VIF1 worker pushes ~85k packets/s in busy scenes, and waking a sleeping feed
        // thread is a kernel call on the worker for most of them.
        static const int spin = [] { const char *e = std::getenv("PS2X_GS_FEED_SPIN"); return e ? std::atoi(e) : 2000; }();
        for (;;)
        {
            if (spin > 0 && f->queue.empty() && !f->stop)
            {
                lock.unlock();
                for (int i = 0; i < spin && f->pending.load(std::memory_order_acquire) == 0u; ++i)
                    _mm_pause();
                lock.lock();
            }
            f->wake.wait(lock, [f] { return f->stop || !f->queue.empty(); });
            if (f->queue.empty())
                break;
            GsFeedItem item = std::move(f->queue.front());
            f->queue.pop_front();
            f->queuedBytes -= item.packet.size();
            f->busy = true;
            lock.unlock();
            try
            {
                if (item.csrClear != 0u)
                    f->owner->gs_regs.csr.fetch_and(~item.csrClear, std::memory_order_acq_rel);
                else if (item.batched)
                {
                    for (size_t at = 0; at + 4u <= item.packet.size();)
                    {
                        uint32_t size = 0;
                        std::memcpy(&size, item.packet.data() + at, 4u);
                        f->process(item.packet.data() + at + 4u, size);
                        at += 4u + ((static_cast<size_t>(size) + 3u) & ~size_t{3});
                    }
                }
                else
                    f->process(item.packet.data(), static_cast<uint32_t>(item.packet.size()));
            }
            catch (const std::exception &e)
            {
                std::fprintf(stderr, "[gs-feed] exception while processing a packet: %s\n", e.what());
            }
            const bool keep = item.batched && gsFeedRecycle();
            if (!keep)
                item.packet = {};
            lock.lock();
            if (keep && f->spare.size() < 4u)
            {
                item.packet.clear();
                f->spare.push_back(std::move(item.packet));
            }
            f->busy = false;
            f->pending.fetch_sub(1, std::memory_order_release);
            f->idle.notify_all();
        }
    }

    // The feed, started on first use from the VIF1 worker (the only producer).
    GsFeed *gsFeedFor(PS2Memory *mem)
    {
        if (g_gsFeed)
            return g_gsFeed;
        if (!gsFeedEnabled() || !g_gsFeedProcessor)
            return nullptr;
        auto *f = new GsFeed();
        f->owner = mem;
        f->process = g_gsFeedProcessor;
        f->thread = std::thread(gsFeedLoop, f);
        g_gsFeed = f;
        return f;
    }

    void gsFeedPush(GsFeed *f, GsFeedItem item, std::vector<uint8_t> *refill = nullptr)
    {
        std::unique_lock<std::mutex> lock(f->mutex);
        if (refill && !f->spare.empty())
        {
            *refill = std::move(f->spare.back());
            f->spare.pop_back();
        }
        if (f->queuedBytes >= kGsFeedMaxQueuedBytes)
        {
            TimingScope waitScope(kCatFeedWait);
            f->idle.wait(lock, [f] { return f->queuedBytes < kGsFeedMaxQueuedBytes; });
        }
        f->queuedBytes += item.packet.size();
        f->queue.push_back(std::move(item));
        f->pending.fetch_add(1, std::memory_order_relaxed);
        f->wake.notify_one();
    }

    void gsFeedDrain()
    {
        GsFeed *f = g_gsFeed;
        if (!f || t_onGsFeed || f->pending.load(std::memory_order_acquire) == 0u)
            return;
        std::unique_lock<std::mutex> lock(f->mutex);
        f->idle.wait(lock, [f] { return f->queue.empty() && !f->busy; });
    }

    // Queues the worker's collected packets (PS2X_GS_FEED_BATCH).
    void gsFeedFlushBatch()
    {
        if (g_gsFeedBatch.packet.empty() || !g_gsFeed)
            return;
        TimingScope scope(kCatFeedPush);
        GsFeedItem item;
        item.packet.swap(g_gsFeedBatch.packet);
        item.batched = true;
        gsFeedPush(g_gsFeed, std::move(item), gsFeedRecycle() ? &g_gsFeedBatch.packet : nullptr);
    }

    void gsFeedCsrClear(PS2Memory &mem, uint64_t mask)
    {
        gsFeedFlushBatch(); // after the packets before it
        if (g_gsFeed)
            gsFeedPush(g_gsFeed, GsFeedItem{{}, mask});
        else
            mem.gs_regs.csr.fetch_and(~mask, std::memory_order_acq_rel);
    }

    bool gsFeedPending()
    {
        GsFeed *f = g_gsFeed;
        return f && f->pending.load(std::memory_order_acquire) != 0u;
    }

    // The worker for this memory, started on first use (only for the runtime's memory: tools that
    // run VU1 or VIF1 on their own PS2Memory without a GIF arbiter stay synchronous).
    Vif1AsyncWorker *vif1AsyncFor(PS2Memory *mem)
    {
        if (g_vif1Async)
            return g_vif1Async->owner == mem ? g_vif1Async : nullptr;
        if (!vif1AsyncEnabled() || !mem->m_gifArbiter)
            return nullptr;
        auto *w = new Vif1AsyncWorker();
        w->owner = mem;
        w->thread = std::thread(vif1AsyncWorkerLoop, w);
        g_vif1Async = w;
        return w;
    }

    void vif1AsyncPush(Vif1AsyncWorker *w, AsyncItemKind kind, std::vector<uint8_t> data, uint64_t value = 0)
    {
        std::unique_lock<std::mutex> lock(w->mutex);
        // Backpressure: the EE may run at most ~32 MB of payload ahead of the worker.
        w->idle.wait(lock, [w] { return w->queuedBytes < kVif1AsyncMaxQueuedBytes; });
        w->queuedBytes += data.size();
        w->queue.push_back(AsyncItem{kind, std::move(data), value});
        ++w->pushedSeq;
        w->pending.fetch_add(1, std::memory_order_relaxed);
        w->wake.notify_one();
    }

    void vif1AsyncDrain(const PS2Memory *mem, int site = kDrainOther)
    {
        Vif1AsyncWorker *w = g_vif1Async;
        if (!w || w->owner != mem || t_onVif1AsyncWorker || t_onGsFeed)
            return;
        const bool busy = w->pending.load(std::memory_order_acquire) != 0u || gsFeedPending();
        if (!busy)
            return;
        const auto start = std::chrono::steady_clock::now();
        if (w->pending.load(std::memory_order_acquire) != 0u)
        {
            // doneSeq counts items the VU1 side has finished (the back thread in the two-thread mode).
            std::unique_lock<std::mutex> lock(w->mutex);
            w->idle.wait(lock, [w] { return w->doneSeq == w->pushedSeq; });
        }
        gsFeedDrain(); // then the packets the worker handed on
        g_drainNs[site].fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count(),
                                  std::memory_order_relaxed);
        g_drainWaits[site].fetch_add(1, std::memory_order_relaxed);
    }

    // A CSR write that clears SIGNAL/FINISH (bits 0..1) while packets are still queued: the clear
    // must land after them (a queued FINISH must not survive it), so it is queued too; returns the
    // value to apply now (the clear bits removed). CSR reads drain, so the EE never sees the order differ.
    uint64_t vif1AsyncDeferCsrClear(PS2Memory *mem, uint64_t value)
    {
        Vif1AsyncWorker *w = g_vif1Async;
        if ((value & 0x3u) == 0u || !w || w->owner != mem || t_onVif1AsyncWorker ||
            (w->pending.load(std::memory_order_acquire) == 0u && !gsFeedPending()))
            return value;
        vif1AsyncPush(w, AsyncItemKind::CsrClear, {}, value & 0x3u);
        return value & ~0x3ull;
    }

    bool isVif1AsyncSyncedIo(uint32_t address)
    {
        return (address >= 0x10003000u && address < 0x10003100u) || // GIF registers
               (address >= 0x10003C00u && address < 0x10004000u) || // VIF1 registers
               (address >= 0x10005000u && address < 0x10007000u);   // VIF1 / GIF FIFOs
    }
}

// Called by the EE scheduler at every VBlank: the EE may run at most one VBlank period ahead of the
// worker (what the previous VBlank had queued must be done), like a GPU's one-frame queue. Without
// it the EE, no longer slowed down by VU1, ran up to the 32 MB backpressure limit ahead of the
// picture (many frames of input lag).
void ps2xVif1AsyncVblank()
{
    Vif1AsyncWorker *w = g_vif1Async;
    if (!w || t_onVif1AsyncWorker)
        return;
    // PS2X_VIF1_VBLANK_DEPTH=n (1..4, default 1): wait for what was queued n VBlanks ago.
    static const uint64_t depth = []
    {
        const char *e = std::getenv("PS2X_VIF1_VBLANK_DEPTH");
        const int n = e ? std::atoi(e) : 1;
        return static_cast<uint64_t>(n < 1 ? 1 : (n > 4 ? 4 : n));
    }();
    std::unique_lock<std::mutex> lock(w->mutex);
    const uint64_t mark = w->vblankMarks[(w->vblankCount + 4u - depth) & 3u];
    w->idle.wait(lock, [w, mark] { return w->doneSeq >= mark; });
    w->vblankMarks[w->vblankCount & 3u] = w->pushedSeq;
    ++w->vblankCount;
}

// Estimated ms the worker still needs for what is queued now (queued payload x smoothed ns per byte,
// plus what is left of the running item). -1 without the one-thread worker. Called once per VBlank.
double ps2xVif1BacklogMs()
{
    Vif1AsyncWorker *w = g_vif1Async;
    if (!w || vu1TwoThreads())
        return -1.0;
    std::lock_guard<std::mutex> lock(w->mutex);
    if (w->bytesAcc >= 16384u)
    {
        const double rate = static_cast<double>(w->busyNsAcc) / static_cast<double>(w->bytesAcc);
        w->nsPerByte = w->nsPerByte > 0.0 ? 0.75 * w->nsPerByte + 0.25 * rate : rate;
        w->busyNsAcc = 0;
        w->bytesAcc = 0;
    }
    auto itemNs = [w](size_t bytes)
    {
        const auto it = w->nsBySize.find(sizeBucket(bytes));
        return it != w->nsBySize.end() ? static_cast<double>(it->second) : static_cast<double>(bytes) * w->nsPerByte;
    };
    double ns = 0.0;
    for (const AsyncItem &item : w->queue)
        ns += itemNs(item.data.size());
    if (w->curBytes)
    {
        const double left = itemNs(w->curBytes) - static_cast<double>(steadyNs() - w->curStartNs);
        if (left > 0.0)
            ns += left;
    }
    static const bool dbg = [] { const char *e = std::getenv("PS2X_60FPS_HOLD_LOG"); return e && *e && *e != '0'; }();
    if (dbg)
        std::fprintf(stderr, "[backlog] %.2f ms q %zu B items %llu cur %zu B run %.2f ms nsB %.3f\n", ns / 1.0e6, w->queuedBytes,
                     (unsigned long long)(w->pushedSeq - w->doneSeq), w->curBytes,
                     w->curBytes ? (steadyNs() - w->curStartNs) / 1.0e6 : 0.0, w->nsPerByte);
    return ns / 1.0e6;
}

// PS2X_FRAME_LOG=2: the worker's idle ms since the last call and the items still queued.
void ps2xVif1AsyncStats(double &idleMs, uint64_t &backlog)
{
    idleMs = 0.0;
    backlog = 0;
    Vif1AsyncWorker *w = g_vif1Async;
    if (!w)
        return;
    std::lock_guard<std::mutex> lock(w->mutex);
    idleMs = static_cast<double>(w->idleNs) / 1.0e6;
    w->idleNs = 0;
    backlog = w->pushedSeq - w->doneSeq;
}

// For code outside this file that reads or feeds the GS / VIF1 directly (GS HLE stubs).
void ps2xVif1AsyncDrain(PS2Memory &mem)
{
    vif1AsyncDrain(&mem, kDrainHle);
}

void ps2xVif1AsyncDrainAll()
{
    if (Vif1AsyncWorker *w = g_vif1Async)
        vif1AsyncDrain(w->owner, kDrainHle);
}

bool ps2xOnVif1AsyncWorker()
{
    return t_onVif1AsyncWorker;
}

// PS2X_VU1_TIMING: the runtime's MSCAL/MSCNT callbacks time the microprogram itself.
void ps2xVu1TimingEnterVu()
{
    timingEnter(kCatVu);
}

void ps2xVu1TimingLeave()
{
    timingLeave();
}

// The GS frontend call the feed thread makes for each packet (the runtime registers it once).
void ps2xGsFeedSetProcessor(std::function<void(const uint8_t *, uint32_t)> fn)
{
    g_gsFeedProcessor = std::move(fn);
}

// From the GIF arbiter's process callback: on the VIF1 worker, queue a copy of the packet for the
// feed thread and return true; elsewhere return false (the caller processes it itself, the queues
// having been drained by whoever touches the GS from the EE).
bool ps2xGsFeedSubmit(const uint8_t *data, uint32_t size)
{
    if (!t_onVif1AsyncWorker || !g_vif1Async)
        return false;
    GsFeed *f = gsFeedFor(g_vif1Async->owner);
    if (!f)
        return false;
    TimingScope scope(kCatFeedPush);
    if (g_vu1HashEvery != 0u)
    {
        g_vu1PacketHash = fnv1a(g_vu1PacketHash, data, size);
        ++g_vu1PacketCount;
    }
    if (gsFeedBatchBytes() != 0u)
    {
        std::vector<uint8_t> &b = g_gsFeedBatch.packet;
        if (b.capacity() == 0u)
            b.reserve(gsFeedBatchBytes() + 65536u);
        const size_t at = b.size();
        b.resize(at + 4u + ((static_cast<size_t>(size) + 3u) & ~size_t{3}));
        std::memcpy(b.data() + at, &size, 4u);
        std::memcpy(b.data() + at + 4u, data, size);
        if (b.size() >= gsFeedBatchBytes())
            gsFeedFlushBatch();
        return true;
    }
    gsFeedPush(f, GsFeedItem{std::vector<uint8_t>(data, data + size), 0u});
    return true;
}

// Hands a large drained arbiter packet's buffer to the GS feed without copying it into the batch.
// PS2X_GS_FEED_OWNED_MIN=bytes (default 16384, 0 = off): smaller packets stay batched.
bool ps2xGsFeedSubmitOwned(std::vector<uint8_t> &data)
{
    static const size_t minBytes = []
    {
        const char *e = std::getenv("PS2X_GS_FEED_OWNED_MIN");
        return e ? static_cast<size_t>(std::strtoull(e, nullptr, 10)) : size_t{16384};
    }();
    if (minBytes == 0u || data.size() < minBytes || data.size() > 0xFFFFFFFFull)
        return false;
    if (!t_onVif1AsyncWorker || !g_vif1Async)
        return false;
    GsFeed *f = gsFeedFor(g_vif1Async->owner);
    if (!f)
        return false;
    TimingScope scope(kCatFeedPush);
    if (g_vu1HashEvery != 0u)
    {
        g_vu1PacketHash = fnv1a(g_vu1PacketHash, data.data(), data.size());
        ++g_vu1PacketCount;
    }
    gsFeedFlushBatch(); // after the packets before it
    gsFeedPush(f, GsFeedItem{std::move(data), 0u});
    return true;
}

namespace
{
    inline void inRange(uint32_t offset, size_t bytes, size_t regionSize, const char *op, uint32_t address)
    {
        if (static_cast<uint64_t>(offset) + static_cast<uint64_t>(bytes) > static_cast<uint64_t>(regionSize))
        {
            throw std::runtime_error(std::string(op) + " out-of-bounds at address: 0x" + std::to_string(address));
        }
    }

    template <typename T>
    inline T loadScalar(const uint8_t *base, uint32_t offset, size_t regionSize, const char *op, uint32_t address)
    {
        inRange(offset, sizeof(T), regionSize, op, address);
        T value{};
        std::memcpy(&value, base + offset, sizeof(T));
        return value;
    }

    template <typename T>
    inline void storeScalar(uint8_t *base, uint32_t offset, size_t regionSize, T value, const char *op, uint32_t address)
    {
        inRange(offset, sizeof(T), regionSize, op, address);
        std::memcpy(base + offset, &value, sizeof(T));
    }

    inline bool isGsPrivReg(uint32_t addr)
    {
        return Ps2AddressInRange(addr, PS2_GS_PRIV_REG_BASE, PS2_GS_PRIV_REG_SIZE);
    }

    inline bool isIoRegister(uint32_t addr)
    {
        return Ps2AddressInRange(addr, PS2_IO_BASE, PS2_IO_SIZE);
    }

    inline uint64_t *gsRegPtr(GSRegisters &gs, uint32_t addr)
    {
        // Support both 64-bit base offsets and +4 dword aliases.
        uint32_t off = (addr - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        switch (off)
        {
        case 0x0000:
            return &gs.pmode;
        case 0x0010:
            return &gs.smode1;
        case 0x0020:
            return &gs.smode2;
        case 0x0030:
            return &gs.srfsh;
        case 0x0040:
            return &gs.synch1;
        case 0x0050:
            return &gs.synch2;
        case 0x0060:
            return &gs.syncv;
        case 0x0070:
            return &gs.dispfb1;
        case 0x0080:
            return &gs.display1;
        case 0x0090:
            return &gs.dispfb2;
        case 0x00A0:
            return &gs.display2;
        case 0x00B0:
            return &gs.extbuf;
        case 0x00C0:
            return &gs.extdata;
        case 0x00D0:
            return &gs.extwrite;
        case 0x00E0:
            return &gs.bgcolor;
        // CSR (offset 0x1000) is intentionally not handled here: it is
        // std::atomic<uint64_t> and no longer converts to uint64_t*. Callers must
        // check for offset 0x1000 themselves and go through writeCsrHalf/
        // writeCsrFull/gs.csr.load() instead of gsRegPtr().
        case 0x1010:
            return &gs.imr;
        case 0x1040:
            return &gs.busdir;
        case 0x1080:
            return &gs.siglblid;
        default:
            return nullptr;
        }
    }

    constexpr uint32_t kGsCsrRegOffset = 0x1000u;

    // Atomically apply a 32-bit write to one half (off=0 low dword, off=4 high
    // dword) of the GS CSR register. Bits 0..1 of the low dword (SIGNAL/FINISH) are
    // write-one-to-clear; everything else is a plain merge. Uses compare_exchange
    // so the whole read-modify-write is a single atomic step -- this register is
    // also touched by the vsync worker (FIELD bit) and the GIF (SIGNAL/FINISH) on
    // other threads, so a load-then-store here would race with them.
    inline void writeCsrHalf(std::atomic<uint64_t> &csr, uint32_t off, uint32_t value)
    {
        constexpr uint32_t kW1cMask = 0x3u;
        uint64_t expected = csr.load();
        uint64_t desired;
        do
        {
            if (off == 0u)
            {
                uint32_t oldLow = static_cast<uint32_t>(expected & 0xFFFFFFFFull);
                uint32_t mergedLow = (oldLow & kW1cMask) | (value & ~kW1cMask);
                desired = (expected & 0xFFFFFFFF00000000ull) | static_cast<uint64_t>(mergedLow);
                desired &= ~static_cast<uint64_t>(value & kW1cMask);
            }
            else
            {
                uint64_t mask = 0xFFFFFFFFull << (off * 8u);
                desired = (expected & ~mask) | (static_cast<uint64_t>(value) << (off * 8u));
            }
        } while (!csr.compare_exchange_weak(expected, desired));
    }

    // Same as writeCsrHalf but for a full 64-bit CSR write (bits 0..1 are still
    // write-one-to-clear against the current value).
    inline void writeCsrFull(std::atomic<uint64_t> &csr, uint64_t value)
    {
        constexpr uint64_t kW1cMask = 0x3ull;
        uint64_t expected = csr.load();
        uint64_t desired;
        do
        {
            desired = (expected & kW1cMask) | (value & ~kW1cMask);
            desired &= ~(value & kW1cMask);
        } while (!csr.compare_exchange_weak(expected, desired));
    }

    constexpr std::array<uint32_t, 4> kEeTimerBases = {
        0x10000000u,
        0x10000800u,
        0x10001000u,
        0x10001800u,
    };
    constexpr uint32_t kEeTimerCountOffset = 0x00u;
    constexpr uint32_t kEeTimerModeOffset = 0x10u;
    constexpr uint32_t kEeTimerCompareOffset = 0x20u;
    constexpr uint32_t kEeTimerHoldOffset = 0x30u;
    constexpr uint32_t kEeTimerModeClksMask = 0x3u;
    constexpr uint32_t kEeTimerModeConfigMask = 0x3FFu;
    constexpr uint32_t kEeTimerModeStatusMask = 0xC00u;
    constexpr uint32_t kEeTimerModeZret = 1u << 6;
    constexpr uint32_t kEeTimerModeCue = 1u << 7;
    constexpr uint32_t kEeTimerModeCmpe = 1u << 8;
    constexpr uint32_t kEeTimerModeOvfe = 1u << 9;
    constexpr uint32_t kEeTimerModeEquf = 1u << 10;
    constexpr uint32_t kEeTimerModeOvff = 1u << 11;
    constexpr uint64_t kEeClockHz = 294912000ull;
    constexpr std::array<uint64_t, 4> kEeTimerClockHz = {
        147456000ull,
        9216000ull,
        576000ull,
        15734ull,
    };

    inline bool decodeEeTimerRegister(uint32_t address, size_t &timerIndex, uint32_t &offset)
    {
        for (size_t index = 0; index < kEeTimerBases.size(); ++index)
        {
            const uint32_t candidateOffset = address - kEeTimerBases[index];
            if (candidateOffset == kEeTimerCountOffset ||
                candidateOffset == kEeTimerModeOffset ||
                candidateOffset == kEeTimerCompareOffset ||
                (index < 2u && candidateOffset == kEeTimerHoldOffset))
            {
                timerIndex = index;
                offset = candidateOffset;
                return true;
            }
        }
        return false;
    }

    constexpr uint64_t ticksUntilMatch(uint32_t count, uint32_t target)
    {
        const uint32_t distance = (target - count) & 0xFFFFu;
        return distance == 0u ? 0x10000ull : static_cast<uint64_t>(distance);
    }

    struct DmaTagView
    {
        uint16_t qwc = 0;
        uint8_t id = 0;
        bool irq = false;
        uint32_t addr = 0;
        uint32_t upper = 0;
    };

    inline DmaTagView decodeDmaTag(uint64_t tag)
    {
        DmaTagView out{};
        out.qwc = static_cast<uint16_t>(tag & 0xFFFFu);
        out.id = static_cast<uint8_t>((tag >> 28u) & 0x7u);
        out.irq = ((tag >> 31u) & 0x1ull) != 0ull;
        out.addr = static_cast<uint32_t>((tag >> 32u) & 0x7FFFFFFFu);
        out.upper = static_cast<uint32_t>((tag >> 16u) & 0xFFFFu);
        return out;
    }

    inline uint32_t gifTagNloop(uint64_t tagLo)
    {
        return static_cast<uint32_t>(tagLo & 0x7FFFu);
    }

    inline uint8_t gifTagFlg(uint64_t tagLo)
    {
        return static_cast<uint8_t>((tagLo >> 58u) & 0x3u);
    }

    inline uint32_t gifTagNreg(uint64_t tagLo)
    {
        uint32_t nreg = static_cast<uint32_t>((tagLo >> 60u) & 0xFu);
        return nreg == 0u ? 16u : nreg;
    }

}

// Helpers for GS VRAM addressing (PSMCT32 path).
static inline uint32_t gs_vram_offset(uint32_t basePage, uint32_t x, uint32_t y, uint32_t fbw)
{
    // basePage is in 2048-byte units; fbw is in blocks of 64 pixels.
    uint32_t strideBytes = fbw * 64 * 4;
    return basePage * 2048 + y * strideBytes + x * 4;
}

PS2Memory::PS2Memory()
    : m_rdram(nullptr), m_scratchpad(nullptr), iop_ram(nullptr), m_seenGifCopy(false), m_gsVRAM(nullptr)
{
    ps2SetScratchpadHostPtr(nullptr);
}

PS2Memory::~PS2Memory()
{
    if (Vif1AsyncWorker *w = g_vif1Async; w && w->owner == this)
    {
        {
            std::lock_guard<std::mutex> lock(w->mutex);
            w->stop = true;
        }
        w->wake.notify_all();
        if (w->thread.joinable())
            w->thread.join();
        g_vif1Async = nullptr;
        delete w;
        if (GsFeed *f = g_gsFeed)
        {
            {
                std::lock_guard<std::mutex> lock(f->mutex);
                f->stop = true;
            }
            f->wake.notify_all();
            if (f->thread.joinable())
                f->thread.join();
            g_gsFeed = nullptr;
            delete f;
        }
    }

    if (m_rdram)
    {
        delete[] m_rdram;
        m_rdram = nullptr;
    }

    if (m_scratchpad)
    {
        ps2SetScratchpadHostPtr(nullptr);
        delete[] m_scratchpad;
        m_scratchpad = nullptr;
    }

    if (m_gsVRAM)
    {
        delete[] m_gsVRAM;
        m_gsVRAM = nullptr;
    }

    if (m_vu1Code)
    {
        delete[] m_vu1Code;
        m_vu1Code = nullptr;
    }
    if (m_vu1Data)
    {
        delete[] m_vu1Data;
        m_vu1Data = nullptr;
    }
    if (m_vu0Code)
    {
        delete[] m_vu0Code;
        m_vu0Code = nullptr;
    }
    if (m_vu0Data)
    {
        delete[] m_vu0Data;
        m_vu0Data = nullptr;
    }

    if (iop_ram)
    {
        delete[] iop_ram;
        iop_ram = nullptr;
    }
}

bool PS2Memory::initialize(size_t ramSize)
{
    vif1AsyncDrain(this);
    auto cleanup = [this]()
    {
        delete[] m_rdram;
        delete[] m_scratchpad;
        delete[] iop_ram;
        delete[] m_gsVRAM;
        delete[] m_vu0Code;
        delete[] m_vu0Data;
        delete[] m_vu1Code;
        delete[] m_vu1Data;
        m_rdram = nullptr;
        m_scratchpad = nullptr;
        ps2SetScratchpadHostPtr(nullptr);
        iop_ram = nullptr;
        m_gsVRAM = nullptr;
        m_vu0Code = nullptr;
        m_vu0Data = nullptr;
        m_vu1Code = nullptr;
        m_vu1Data = nullptr;
    };

    cleanup();
    m_seenGifCopy = false;
    m_dmaStartCount.store(0, std::memory_order_relaxed);
    m_gifCopyCount.store(0, std::memory_order_relaxed);
    m_gsWriteCount.store(0, std::memory_order_relaxed);
    m_vifWriteCount.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(m_completedDmacMutex);
        m_completedDmacCauses.clear();
    }
    m_codeRegions.clear();
    m_path3Masked = false;
    m_path3MaskedFifo.clear();
    m_vif1PendingPath2ImageQwc = 0u;
    m_vif1PendingPath2DirectHl = false;
    resetEeTimers();

    try
    {
        // Allocate main RAM
        m_rdram = new uint8_t[ramSize];
        std::memset(m_rdram, 0, ramSize);

        // Allocate scratchpad
        m_scratchpad = new uint8_t[PS2_SCRATCHPAD_SIZE];
        std::memset(m_scratchpad, 0, PS2_SCRATCHPAD_SIZE);
        ps2SetScratchpadHostPtr(m_scratchpad);

        // Initialize EE TLB entries (R5900 has 48 entries).
        m_tlbEntries.assign(48, TLBEntry{0, 0, 0, false});

        // Allocate IOP RAM
        iop_ram = new uint8_t[2 * 1024 * 1024]; // 2MB

        // Initialize IOP RAM with zeros
        std::memset(iop_ram, 0, 2 * 1024 * 1024);

        // Initialize I/O registers
        m_ioRegisters.clear();
        m_gifStatReg = nullptr;

        // Initialize GS registers
        memset(&gs_regs, 0, sizeof(gs_regs));
        // memset zero-fills std::atomic<uint64_t>::csr's bytes, which is not itself
        // a guaranteed-valid atomic store; make the zero-initialization explicit.
        gs_regs.csr.store(0);
        gs_regs.dispfb1 = (0ULL << 0) | (10ULL << 9) | (0ULL << 15) | (0ULL << 32) | (0ULL << 43);
        gs_regs.display1 = (0ULL << 0) | (0ULL << 12) | (0ULL << 23) | (0ULL << 27) | (639ULL << 32) | (447ULL << 44);
        gs_regs.dispfb2 = gs_regs.dispfb1;
        gs_regs.display2 = gs_regs.display1;

        // Allocate GS VRAM (4MB)
        m_gsVRAM = new uint8_t[PS2_GS_VRAM_SIZE];
        std::memset(m_gsVRAM, 0, PS2_GS_VRAM_SIZE);

        m_vu0Code = new uint8_t[PS2_VU0_CODE_SIZE];
        m_vu0Data = new uint8_t[PS2_VU0_DATA_SIZE];
        std::memset(m_vu0Code, 0, PS2_VU0_CODE_SIZE);
        std::memset(m_vu0Data, 0, PS2_VU0_DATA_SIZE);

        m_vu1Code = new uint8_t[PS2_VU1_CODE_SIZE];
        m_vu1Data = new uint8_t[PS2_VU1_DATA_SIZE];
        std::memset(m_vu1Code, 0, PS2_VU1_CODE_SIZE);
        std::memset(m_vu1Data, 0, PS2_VU1_DATA_SIZE);
        markVU0CodeModified();
        markVU1CodeModified();

        // Initialize VIF registers
        memset(&vif0_regs, 0, sizeof(vif0_regs));
        memset(&vif1_regs, 0, sizeof(vif1_regs));

        // Initialize DMA registers
        memset(dma_regs, 0, sizeof(dma_regs));

        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error initializing PS2 memory: " << e.what() << std::endl;
        cleanup();
        return false;
    }
}

template <class Timers>
static uint32_t stepEeTimerArray(Timers &timers, uint64_t eeCycles) noexcept
{
    if (eeCycles == 0u)
    {
        return 0u;
    }

    uint32_t interruptMask = 0u;
    for (size_t index = 0; index < timers.size(); ++index)
    {
        auto &timer = timers[index];
        if ((timer.mode & kEeTimerModeCue) == 0u)
        {
            continue;
        }

        const uint64_t clockHz = kEeTimerClockHz[timer.mode & kEeTimerModeClksMask];
        const uint64_t wholeSeconds = eeCycles / kEeClockHz;
        const uint64_t remainingCycles = eeCycles % kEeClockHz;
        const uint64_t scaled = remainingCycles * clockHz + timer.clockRemainder;
        const uint64_t ticks = wholeSeconds * clockHz + scaled / kEeClockHz;
        timer.clockRemainder = scaled % kEeClockHz;
        if (ticks == 0u)
        {
            continue;
        }

        const uint32_t oldCount = timer.count & 0xFFFFu;
        const uint32_t compare = timer.compare & 0xFFFFu;
        const uint64_t compareDistance = ticksUntilMatch(oldCount, compare);
        const uint64_t overflowDistance = 0x10000ull - oldCount;
        const bool zeroReturn = (timer.mode & kEeTimerModeZret) != 0u;
        const bool compareReached = ticks >= compareDistance;
        bool overflowReached = false;

        if (zeroReturn)
        {
            overflowReached = ticks >= overflowDistance && overflowDistance <= compareDistance;
            if (compareReached)
            {
                const uint64_t remaining = ticks - compareDistance;
                timer.count = compare == 0u
                                  ? static_cast<uint32_t>(remaining & 0xFFFFu)
                                  : static_cast<uint32_t>(remaining % compare);
            }
            else
            {
                timer.count = static_cast<uint32_t>((oldCount + ticks) & 0xFFFFu);
            }
        }
        else
        {
            overflowReached = ticks >= overflowDistance;
            timer.count = static_cast<uint32_t>((oldCount + ticks) & 0xFFFFu);
        }

        if (compareReached && (timer.mode & kEeTimerModeCmpe) != 0u && (timer.mode & kEeTimerModeEquf) == 0u)
        {
            timer.mode |= kEeTimerModeEquf;
            interruptMask |= 1u << index;
        }
        if (overflowReached && (timer.mode & kEeTimerModeOvfe) != 0u && (timer.mode & kEeTimerModeOvff) == 0u)
        {
            timer.mode |= kEeTimerModeOvff;
            interruptMask |= 1u << index;
        }
    }
    return interruptMask;
}

// PS2X_EE_VERIFY=1: also step a shadow copy of the timers eagerly (the old way, every checkpoint) and
// count every difference from the lazy timers: an interrupt returned at another checkpoint, or a
// different state after a flush/sync. Printed as [eeverify] lines.
static const bool g_eeVerify = [] { const char *e = std::getenv("PS2X_EE_VERIFY"); return e && *e && *e != '0'; }();
template <class T> T g_eeShadowTimers{};
static uint64_t g_eeTimerChecks = 0, g_eeTimerFails = 0, g_eeTimerCalls = 0, g_eeTimerFlushes = 0;
// The thread that steps the timers (EeScheduler's). Only it may sync them: other readers (the debug
// panel) see the state of the last step, at most one deferred window old.
static std::thread::id g_eeTimerThread{};
static inline bool onEeTimerThread() { return g_eeTimerThread == std::this_thread::get_id(); }

template <class Timers>
static bool sameEeTimers(const Timers &a, const Timers &b)
{
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].count != b[i].count || a[i].mode != b[i].mode || a[i].compare != b[i].compare ||
            a[i].hold != b[i].hold || a[i].clockRemainder != b[i].clockRemainder)
            return false;
    return true;
}

void PS2Memory::resetEeTimers() noexcept
{
    m_eeTimers = {};
    m_eeTimerPending = 0u;
    m_eeTimerDeferLimit = std::numeric_limits<uint64_t>::max();
    if (g_eeVerify)
        g_eeShadowTimers<decltype(m_eeTimers)> = m_eeTimers;
}

// Called at every EE checkpoint (EeScheduler::accountCycles). The timers are stepped lazily:
// cycles are added to m_eeTimerPending while they cannot bring any counting timer to its
// compare value or to overflow (m_eeTimerDeferLimit), so the deferred part is just count +=
// ticks, and stepping n cycles at once gives the same count, remainder and flags as n single
// steps. Register reads/writes and cyclesUntilNextEeTimerInterrupt() sync first.
uint32_t PS2Memory::advanceEeTimers(uint64_t eeCycles) noexcept
{
    if (eeCycles == 0u)
    {
        return 0u;
    }

    // Each advance clears GIF_STAT.FQC (as before: map lookup until the register exists; elements of
    // m_ioRegisters are never erased, so the pointer stays valid).
    constexpr uint32_t kGifStat = 0x10003020u;
    constexpr uint32_t kGifFqcMask = 0x1F000000u;
    if (m_gifStatReg == nullptr)
    {
        auto gifStatIt = m_ioRegisters.find(kGifStat);
        if (gifStatIt != m_ioRegisters.end())
            m_gifStatReg = &gifStatIt->second;
    }
    if (m_gifStatReg != nullptr)
        *m_gifStatReg &= ~kGifFqcMask;

    uint32_t shadowMask = 0u;
    if (g_eeVerify) [[unlikely]]
    {
        shadowMask = stepEeTimerArray(g_eeShadowTimers<decltype(m_eeTimers)>, eeCycles);
        if ((++g_eeTimerCalls & ((1u << 22) - 1u)) == 0u)
            std::fprintf(stderr, "[eeverify] timers: %llu calls, %llu flushes, %llu checks, %llu fails\n",
                         (unsigned long long)g_eeTimerCalls, (unsigned long long)g_eeTimerFlushes,
                         (unsigned long long)g_eeTimerChecks, (unsigned long long)g_eeTimerFails);
    }

    if (g_eeTimerThread == std::thread::id{}) [[unlikely]]
        g_eeTimerThread = std::this_thread::get_id();
    m_eeTimerPending += eeCycles;
    if (m_eeTimerPending <= m_eeTimerDeferLimit)
    {
        if (g_eeVerify && shadowMask != 0u) [[unlikely]]
            ++g_eeTimerFails; // the eager timers raised an interrupt inside the deferred window
        return 0u;
    }
    const uint64_t pending = m_eeTimerPending;
    m_eeTimerPending = 0u;
    const uint32_t mask = stepEeTimers(pending);
    updateEeTimerDeferLimit();
    if (g_eeVerify) [[unlikely]]
    {
        ++g_eeTimerFlushes;
        ++g_eeTimerChecks;
        if (mask != shadowMask || !sameEeTimers(m_eeTimers, g_eeShadowTimers<decltype(m_eeTimers)>))
            ++g_eeTimerFails;
    }
    return mask;
}

void PS2Memory::syncEeTimers() noexcept
{
    if (m_eeTimerPending != 0u)
    {
        const uint64_t pending = m_eeTimerPending;
        m_eeTimerPending = 0u;
        if (stepEeTimers(pending) != 0u && g_eeVerify) // no event inside the deferred window, so no interrupt bits
            ++g_eeTimerFails;
        updateEeTimerDeferLimit(); // the limit counts from the last step
    }
    if (g_eeVerify) [[unlikely]]
    {
        ++g_eeTimerChecks;
        if (!sameEeTimers(m_eeTimers, g_eeShadowTimers<decltype(m_eeTimers)>))
            ++g_eeTimerFails;
    }
}

void PS2Memory::updateEeTimerDeferLimit() noexcept
{
    // Largest cycle count after which no counting timer has reached its compare value or
    // overflowed (whatever the enable/flag bits: ZRET resets the count at compare regardless).
    uint64_t limit = std::numeric_limits<uint64_t>::max();
    for (const EeTimer &timer : m_eeTimers)
    {
        if ((timer.mode & kEeTimerModeCue) == 0u)
            continue;
        const uint32_t count = timer.count & 0xFFFFu;
        const uint64_t distance = std::min<uint64_t>(ticksUntilMatch(count, timer.compare & 0xFFFFu), 0x10000ull - count);
        const uint64_t clockHz = kEeTimerClockHz[timer.mode & kEeTimerModeClksMask];
        // ticks(n) = floor((n * clockHz + remainder) / kEeClockHz) < distance  <=>  n * clockHz + remainder < distance * kEeClockHz
        const uint64_t bound = distance * kEeClockHz - timer.clockRemainder; // > 0: remainder < kEeClockHz
        limit = std::min(limit, (bound - 1u) / clockHz);
    }
    // PS2X_NO_TIMER_DEFER=1: step the timers at every checkpoint as before.
    static const bool off = [] { const char *e = std::getenv("PS2X_NO_TIMER_DEFER"); return e && *e && *e != '0'; }();
    m_eeTimerDeferLimit = off ? 0u : limit;
}


uint32_t PS2Memory::stepEeTimers(uint64_t eeCycles) noexcept
{
    return stepEeTimerArray(m_eeTimers, eeCycles);
}

uint64_t PS2Memory::cyclesUntilNextEeTimerInterrupt() const noexcept
{
    const_cast<PS2Memory *>(this)->syncEeTimers();
    uint64_t nearest = std::numeric_limits<uint64_t>::max();
    for (const EeTimer &timer : m_eeTimers)
    {
        if ((timer.mode & kEeTimerModeCue) == 0u)
        {
            continue;
        }

        const uint32_t count = timer.count & 0xFFFFu;
        const uint32_t compare = timer.compare & 0xFFFFu;
        const uint64_t compareDistance = ticksUntilMatch(count, compare);
        const uint64_t overflowDistance = 0x10000ull - count;
        uint64_t eventTicks = std::numeric_limits<uint64_t>::max();

        if ((timer.mode & kEeTimerModeCmpe) != 0u &&
            (timer.mode & kEeTimerModeEquf) == 0u)
        {
            eventTicks = compareDistance;
        }
        const bool overflowCanOccur = (timer.mode & kEeTimerModeZret) == 0u ||
                                      overflowDistance <= compareDistance;
        if (overflowCanOccur &&
            (timer.mode & kEeTimerModeOvfe) != 0u &&
            (timer.mode & kEeTimerModeOvff) == 0u)
        {
            eventTicks = std::min(eventTicks, overflowDistance);
        }
        if (eventTicks == std::numeric_limits<uint64_t>::max())
        {
            continue;
        }

        const uint64_t clockHz = kEeTimerClockHz[timer.mode & kEeTimerModeClksMask];
        const uint64_t numerator = eventTicks * kEeClockHz - timer.clockRemainder;
        const uint64_t cycles = (numerator + clockHz - 1u) / clockHz;
        nearest = std::min(nearest, std::max<uint64_t>(1u, cycles));
    }
    return nearest;
}

bool PS2Memory::isScratchpad(uint32_t address) const
{
    return ps2IsScratchpadAddress(address);
}

uint8_t *PS2Memory::mapVuMemory(uint32_t physAddr, uint32_t size, uint32_t &offset, uint32_t &limit)
{
    uint8_t *mem = const_cast<uint8_t *>(static_cast<const PS2Memory *>(this)->mapVuMemory(physAddr, size, offset, limit));
    if (mem && (mem == m_vu1Code || mem == m_vu1Data))
        vif1AsyncDrain(this, kDrainVuMem); // EE access to VU1 memory sees every queued UNPACK/MPG/microprogram
    return mem;
}

const uint8_t *PS2Memory::mapVuMemory(uint32_t physAddr, uint32_t size, uint32_t &offset, uint32_t &limit) const
{
    auto mapRange = [&](uint32_t base, uint32_t rangeSize, const uint8_t *ptr) -> const uint8_t *
    {
        if (!ptr || physAddr < base)
        {
            return nullptr;
        }
        const uint32_t local = physAddr - base;
        if (local >= rangeSize || size > (rangeSize - local))
        {
            return nullptr;
        }
        offset = local;
        limit = rangeSize;
        return ptr;
    };

    if (const uint8_t *ptr = mapRange(PS2_VU0_CODE_BASE, PS2_VU0_CODE_SIZE, m_vu0Code))
    {
        return ptr;
    }
    if (const uint8_t *ptr = mapRange(PS2_VU0_DATA_BASE, PS2_VU0_DATA_SIZE, m_vu0Data))
    {
        return ptr;
    }
    if (const uint8_t *ptr = mapRange(PS2_VU1_CODE_BASE, PS2_VU1_CODE_SIZE, m_vu1Code))
    {
        return ptr;
    }
    return mapRange(PS2_VU1_DATA_BASE, PS2_VU1_DATA_SIZE, m_vu1Data);
}

uint32_t PS2Memory::translateAddress(uint32_t virtualAddress)
{
    if (isScratchpad(virtualAddress))
    {
        return ps2ScratchpadOffset(virtualAddress);
    }

    // EE uncached aliases of main RAM (per PS2 memory map):
    //   0x20000000-0x3FFFFFFF -> 32MB mirror of RDRAM
    // This includes the accelerated window rooted at 0x30100000.
    if (Ps2IsUncachedRamMirrorAddress(virtualAddress))
    {
        return virtualAddress & PS2_RAM_MASK;
    }

    // KSEG0/KSEG1 direct-mapped window.
    if (Ps2IsKseg01Address(virtualAddress))
    {
        return Ps2DirectMappedPhysicalAddress(virtualAddress);
    }

    // In this runtime, low segments are treated as physical-style addresses already.
    if (virtualAddress < 0x80000000)
    {
        return virtualAddress;
    }

    // KSEG2/KSEG3 are TLB mapped.
    if (Ps2IsKseg23Address(virtualAddress))
    {
        for (const auto &entry : m_tlbEntries)
        {
            if (entry.valid)
            {
                // PageMask uses bits [24:13]. Build an address-level mask (plus 4KB base page bits).
                const uint32_t mask = entry.mask & 0x01FFE000u;
                const uint32_t compareMask = ~(mask | 0xFFFu);
                if ((virtualAddress & compareMask) == (entry.vpn & compareMask))
                {
                    // TLB hit
                    const uint32_t pageOffsetMask = mask | 0xFFFu;
                    const uint32_t physBase = entry.pfn << 12;
                    return physBase | (virtualAddress & pageOffsetMask);
                }
            }
        }
        throw std::runtime_error("TLB miss for address: 0x" + std::to_string(virtualAddress));
    }

    return virtualAddress;
}

bool PS2Memory::tlbRead(uint32_t index, uint32_t &vpn, uint32_t &pfn, uint32_t &mask, bool &valid) const
{
    if (index >= m_tlbEntries.size())
    {
        return false;
    }

    const TLBEntry &entry = m_tlbEntries[index];
    vpn = entry.vpn;
    pfn = entry.pfn;
    mask = entry.mask;
    valid = entry.valid;
    return true;
}

bool PS2Memory::tlbWrite(uint32_t index, uint32_t vpn, uint32_t pfn, uint32_t mask, bool valid)
{
    if (index >= m_tlbEntries.size())
    {
        return false;
    }

    TLBEntry &entry = m_tlbEntries[index];
    entry.vpn = vpn & 0xFFFFF000u;
    entry.pfn = pfn & 0x000FFFFFu;
    entry.mask = mask & 0x01FFE000u;
    entry.valid = valid;
    return true;
}

int32_t PS2Memory::tlbProbe(uint32_t vpn) const
{
    const uint32_t normalizedVpn = vpn & 0xFFFFF000u;
    for (uint32_t i = 0; i < static_cast<uint32_t>(m_tlbEntries.size()); ++i)
    {
        const TLBEntry &entry = m_tlbEntries[i];
        if (!entry.valid)
        {
            continue;
        }

        const uint32_t mask = entry.mask & 0x01FFE000u;
        const uint32_t compareMask = ~(mask | 0xFFFu);
        if ((normalizedVpn & compareMask) == (entry.vpn & compareMask))
        {
            return static_cast<int32_t>(i);
        }
    }

    return -1;
}

uint8_t PS2Memory::read8(uint32_t address)
{
    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        return m_scratchpad[physAddr];
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        return m_rdram[physAddr];
    }
    uint32_t vuOffset = 0;
    uint32_t vuLimit = 0;
    if (const uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint8_t), vuOffset, vuLimit))
    {
        (void)vuLimit;
        return vuMem[vuOffset];
    }
    else if (isIoRegister(physAddr))
    {
        uint32_t regAddr = physAddr & ~0x3;
        uint32_t value = readIORegister(regAddr);
        uint32_t shift = (physAddr & 3) * 8;
        return static_cast<uint8_t>((value >> shift) & 0xFF);
    }

    return 0;
}

uint16_t PS2Memory::read16(uint32_t address)
{
    if (address & 1)
    {
        throw std::runtime_error("Unaligned 16-bit read at address: 0x" + std::to_string(address));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        return loadScalar<uint16_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, "read16 scratchpad", address);
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        return loadScalar<uint16_t>(m_rdram, physAddr, PS2_RAM_SIZE, "read16 rdram", address);
    }
    uint32_t vuOffset = 0;
    uint32_t vuLimit = 0;
    if (const uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint16_t), vuOffset, vuLimit))
    {
        return loadScalar<uint16_t>(vuMem, vuOffset, vuLimit, "read16 vu", address);
    }
    else if (isIoRegister(physAddr))
    {
        uint32_t regAddr = physAddr & ~0x3;
        uint32_t value = readIORegister(regAddr);
        uint32_t shift = (physAddr & 2) * 8;
        return static_cast<uint16_t>((value >> shift) & 0xFFFF);
    }

    return 0;
}

uint32_t PS2Memory::read32(uint32_t address)
{
    if (address & 3)
    {
        throw std::runtime_error("Unaligned 32-bit read at address: 0x" + std::to_string(address));
    }

    if (isGsPrivReg(address))
    {
        uint32_t off = address & 7;
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            vif1AsyncDrain(this, kDrainCsr); // FINISH/SIGNAL from queued packets
            uint64_t val = gs_regs.csr.load();
            return (uint32_t)(val >> (off * 8));
        }
        uint64_t *reg = gsRegPtr(gs_regs, address);
        if (!reg)
            return 0;
        uint64_t val = *reg;
        return (uint32_t)(val >> (off * 8));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        return loadScalar<uint32_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, "read32 scratchpad", address);
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        return loadScalar<uint32_t>(m_rdram, physAddr, PS2_RAM_SIZE, "read32 rdram", address);
    }
    uint32_t vuOffset = 0;
    uint32_t vuLimit = 0;
    if (const uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint32_t), vuOffset, vuLimit))
    {
        return loadScalar<uint32_t>(vuMem, vuOffset, vuLimit, "read32 vu", address);
    }
    else if (isIoRegister(physAddr))
    {
        return readIORegister(physAddr);
    }

    return 0;
}

uint64_t PS2Memory::read64(uint32_t address)
{
    if (address & 7)
    {
        throw std::runtime_error("Unaligned 64-bit read at address: 0x" + std::to_string(address));
    }

    if (isGsPrivReg(address))
    {
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            vif1AsyncDrain(this, kDrainCsr); // FINISH/SIGNAL from queued packets
            return gs_regs.csr.load();
        }
        uint64_t *reg = gsRegPtr(gs_regs, address);
        return reg ? *reg : 0;
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        return loadScalar<uint64_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, "read64 scratchpad", address);
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        return loadScalar<uint64_t>(m_rdram, physAddr, PS2_RAM_SIZE, "read64 rdram", address);
    }
    uint32_t vuOffset = 0;
    uint32_t vuLimit = 0;
    if (const uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint64_t), vuOffset, vuLimit))
    {
        return loadScalar<uint64_t>(vuMem, vuOffset, vuLimit, "read64 vu", address);
    }

    // 64-bit IO read: compose from the two adjacent 32-bit IO register slots
    // to avoid any side-effects from read32 handlers.
    if (isIoRegister(address))
    {
        uint32_t lo = m_ioRegisters.count(address) ? m_ioRegisters[address] : 0u;
        uint32_t hi = m_ioRegisters.count(address + 4) ? m_ioRegisters[address + 4] : 0u;
        return static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
    }
    return (uint64_t)read32(address) | ((uint64_t)read32(address + 4) << 32);
}

__m128i PS2Memory::read128(uint32_t address)
{
    if (address & 15)
    {
        throw std::runtime_error("Unaligned 128-bit read at address: 0x" + std::to_string(address));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        inRange(physAddr, sizeof(__m128i), PS2_SCRATCHPAD_SIZE, "read128 scratchpad", address);
        return _mm_loadu_si128(reinterpret_cast<__m128i *>(&m_scratchpad[physAddr]));
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        inRange(physAddr, sizeof(__m128i), PS2_RAM_SIZE, "read128 rdram", address);
        return _mm_loadu_si128(reinterpret_cast<__m128i *>(&m_rdram[physAddr]));
    }
    uint32_t vuOffset = 0;
    uint32_t vuLimit = 0;
    if (const uint8_t *vuMem = mapVuMemory(physAddr, sizeof(__m128i), vuOffset, vuLimit))
    {
        inRange(vuOffset, sizeof(__m128i), vuLimit, "read128 vu", address);
        return _mm_loadu_si128(reinterpret_cast<const __m128i *>(vuMem + vuOffset));
    }

    // 128-bit reads are primarily for quad-word loads in the EE, which are only valid for RAM areas
    // Return zeroes for unsupported areas
    return _mm_setzero_si128();
}

void PS2Memory::write8(uint32_t address, uint8_t value)
{
    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        m_scratchpad[physAddr] = value;
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        m_rdram[physAddr] = value;
    }
    else
    {
        uint32_t vuOffset = 0;
        uint32_t vuLimit = 0;
        if (uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint8_t), vuOffset, vuLimit))
        {
            (void)vuLimit;
            vuMem[vuOffset] = value;
            if (vuMem == m_vu0Code)
                markVU0CodeModified();
            else if (vuMem == m_vu1Code)
            {
                static const bool diag = [] { const char *e = std::getenv("PS2X_VU1_BADKICK"); return e && *e; }();
                static uint32_t s_logged = 0;
                if (diag && ++s_logged <= 200u)
                    std::fprintf(stderr, "[vu1diag] EE write to VU1 code at 0x%x\n", physAddr);
                markVU1CodeModified();
            }
            return;
        }
    }
    if (isIoRegister(physAddr))
    {
        // IO registers - handle byte writes by modifying the appropriate byte in the word
        uint32_t regAddr = physAddr & ~0x3;
        uint32_t shift = (physAddr & 3) * 8;
        uint32_t mask = ~(0xFF << shift);
        uint32_t newValue = (m_ioRegisters[regAddr] & mask) | ((uint32_t)value << shift);
        writeIORegister(regAddr, newValue);
    }
}

void PS2Memory::write16(uint32_t address, uint16_t value)
{
    if (address & 1)
    {
        throw std::runtime_error("Unaligned 16-bit write at address: 0x" + std::to_string(address));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        storeScalar<uint16_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, value, "write16 scratchpad", address);
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        storeScalar<uint16_t>(m_rdram, physAddr, PS2_RAM_SIZE, value, "write16 rdram", address);
    }
    else
    {
        uint32_t vuOffset = 0;
        uint32_t vuLimit = 0;
        if (uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint16_t), vuOffset, vuLimit))
        {
            storeScalar<uint16_t>(vuMem, vuOffset, vuLimit, value, "write16 vu", address);
            if (vuMem == m_vu0Code)
                markVU0CodeModified();
            else if (vuMem == m_vu1Code)
            {
                static const bool diag = [] { const char *e = std::getenv("PS2X_VU1_BADKICK"); return e && *e; }();
                static uint32_t s_logged = 0;
                if (diag && ++s_logged <= 200u)
                    std::fprintf(stderr, "[vu1diag] EE write to VU1 code at 0x%x\n", physAddr);
                markVU1CodeModified();
            }
            return;
        }
    }
    if (isIoRegister(physAddr))
    {
        uint32_t regAddr = physAddr & ~0x3;
        uint32_t shift = (physAddr & 2) * 8;
        uint32_t mask = ~(0xFFFF << shift);
        uint32_t newValue = (m_ioRegisters[regAddr] & mask) | ((uint32_t)value << shift);
        writeIORegister(regAddr, newValue);
    }
}

void PS2Memory::write32(uint32_t address, uint32_t value)
{
    if (address & 3)
    {
        throw std::runtime_error("Unaligned 32-bit write at address: 0x" + std::to_string(address));
    }

    if (isGsPrivReg(address))
    {
        uint32_t off = address & 7;
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            // CSR: bits 0..1 of the low dword are write-one-to-clear status bits.
            // Done as a single atomic RMW -- see writeCsrHalf's comment.
            if (off == 0u)
                value = static_cast<uint32_t>(vif1AsyncDeferCsrClear(this, value));
            writeCsrHalf(gs_regs.csr, off, value);
        }
        else if (uint64_t *reg = gsRegPtr(gs_regs, address))
        {
            uint64_t mask = 0xFFFFFFFFULL << (off * 8);
            uint64_t newVal = (*reg & ~mask) | ((uint64_t)value << (off * 8));
            *reg = newVal;
        }
        return;
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        storeScalar<uint32_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, value, "write32 scratchpad", address);
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        // Check if this might be code modification
        markModified(address, 4);

        storeScalar<uint32_t>(m_rdram, physAddr, PS2_RAM_SIZE, value, "write32 rdram", address);
    }
    else
    {
        uint32_t vuOffset = 0;
        uint32_t vuLimit = 0;
        if (uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint32_t), vuOffset, vuLimit))
        {
            storeScalar<uint32_t>(vuMem, vuOffset, vuLimit, value, "write32 vu", address);
            if (vuMem == m_vu0Code)
                markVU0CodeModified();
            else if (vuMem == m_vu1Code)
            {
                static const bool diag = [] { const char *e = std::getenv("PS2X_VU1_BADKICK"); return e && *e; }();
                static uint32_t s_logged = 0;
                if (diag && ++s_logged <= 200u)
                    std::fprintf(stderr, "[vu1diag] EE write to VU1 code at 0x%x\n", physAddr);
                markVU1CodeModified();
            }
            return;
        }
    }
    if (isIoRegister(physAddr))
    {
        writeIORegister(physAddr, value);
    }
}

void PS2Memory::write64(uint32_t address, uint64_t value)
{
    if (address & 7)
    {
        throw std::runtime_error("Unaligned 64-bit write at address: 0x" + std::to_string(address));
    }

    if (isGsPrivReg(address))
    {
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            // CSR: bits 0..1 are write-one-to-clear status bits. Done as a single
            // atomic RMW -- see writeCsrFull's comment.
            value = vif1AsyncDeferCsrClear(this, value);
            writeCsrFull(gs_regs.csr, value);
        }
        else if (uint64_t *reg = gsRegPtr(gs_regs, address))
        {
            *reg = value;
        }
        return;
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        storeScalar<uint64_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, value, "write64 scratchpad", address);
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        markModified(address, 8);
        storeScalar<uint64_t>(m_rdram, physAddr, PS2_RAM_SIZE, value, "write64 rdram", address);
    }
    else
    {
        uint32_t vuOffset = 0;
        uint32_t vuLimit = 0;
        if (uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint64_t), vuOffset, vuLimit))
        {
            storeScalar<uint64_t>(vuMem, vuOffset, vuLimit, value, "write64 vu", address);
            if (vuMem == m_vu0Code)
                markVU0CodeModified();
            else if (vuMem == m_vu1Code)
            {
                static const bool diag = [] { const char *e = std::getenv("PS2X_VU1_BADKICK"); return e && *e; }();
                static uint32_t s_logged = 0;
                if (diag && ++s_logged <= 200u)
                    std::fprintf(stderr, "[vu1diag] EE write to VU1 code at 0x%x\n", physAddr);
                markVU1CodeModified();
            }
            return;
        }
    }
    if (isIoRegister(physAddr))
    {
        write32(address, (uint32_t)value);
        write32(address + 4, (uint32_t)(value >> 32));
    }
}

void PS2Memory::write128(uint32_t address, __m128i value)
{
    if (address & 15)
    {
        throw std::runtime_error("Unaligned 128-bit write at address: 0x" + std::to_string(address));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (!scratch && physAddr == 0x10004000u) // VIF0_FIFO
    {
        alignas(16) uint8_t fifoData[16];
        _mm_storeu_si128(reinterpret_cast<__m128i *>(fifoData), value);
        processVIF0Data(fifoData, sizeof(fifoData));
        return;
    }
    if (!scratch && physAddr == 0x10005000u) // VIF1_FIFO
    {
        vif1AsyncDrain(this, kDrainVifFifo);
        alignas(16) uint8_t fifoData[16];
        _mm_storeu_si128(reinterpret_cast<__m128i *>(fifoData), value);
        processVIF1Data(fifoData, sizeof(fifoData));
        return;
    }

    if (scratch)
    {
        inRange(physAddr, sizeof(__m128i), PS2_SCRATCHPAD_SIZE, "write128 scratchpad", address);
        _mm_storeu_si128(reinterpret_cast<__m128i *>(&m_scratchpad[physAddr]), value);
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        markModified(address, 16);
        inRange(physAddr, sizeof(__m128i), PS2_RAM_SIZE, "write128 rdram", address);
        _mm_storeu_si128(reinterpret_cast<__m128i *>(&m_rdram[physAddr]), value);
    }
    else
    {
        uint32_t vuOffset = 0;
        uint32_t vuLimit = 0;
        if (uint8_t *vuMem = mapVuMemory(physAddr, sizeof(__m128i), vuOffset, vuLimit))
        {
            inRange(vuOffset, sizeof(__m128i), vuLimit, "write128 vu", address);
            _mm_storeu_si128(reinterpret_cast<__m128i *>(vuMem + vuOffset), value);
            if (vuMem == m_vu0Code)
                markVU0CodeModified();
            else if (vuMem == m_vu1Code)
            {
                static const bool diag = [] { const char *e = std::getenv("PS2X_VU1_BADKICK"); return e && *e; }();
                static uint32_t s_logged = 0;
                if (diag && ++s_logged <= 200u)
                    std::fprintf(stderr, "[vu1diag] EE write to VU1 code at 0x%x\n", physAddr);
                markVU1CodeModified();
            }
            return;
        }
    }
    if (isIoRegister(physAddr))
    {
        // Non-RAM 128-bit stores are modeled as two 64-bit stores.
        uint64_t lo = _mm_extract_epi64(value, 0);
        uint64_t hi = _mm_extract_epi64(value, 1);

        write64(address, lo);
        write64(address + 8, hi);
    }
}

bool PS2Memory::writeIORegister(uint32_t address, uint32_t value)
{
    if (isVif1AsyncSyncedIo(address))
        vif1AsyncDrain(this, kDrainIoReg);
    size_t timerIndex = 0u;
    uint32_t timerOffset = 0u;
    if (decodeEeTimerRegister(address, timerIndex, timerOffset))
    {
        syncEeTimers();
        struct DeferLimitUpdate
        {
            PS2Memory &memory;
            ~DeferLimitUpdate()
            {
                memory.updateEeTimerDeferLimit();
                if (g_eeVerify) // the shadow takes the same register write
                    g_eeShadowTimers<decltype(memory.m_eeTimers)> = memory.m_eeTimers;
            }
        } deferLimitUpdate{*this};
        EeTimer &timer = m_eeTimers[timerIndex];
        switch (timerOffset)
        {
        case kEeTimerCountOffset:
            timer.count = value & 0xFFFFu;
            timer.clockRemainder = 0u;
            break;
        case kEeTimerModeOffset:
        {
            const uint32_t previousMode = timer.mode;
            const uint32_t status = (previousMode & kEeTimerModeStatusMask) & ~(value & kEeTimerModeStatusMask);
            timer.mode = (value & kEeTimerModeConfigMask) | status;
            if (((previousMode ^ timer.mode) & (kEeTimerModeClksMask | kEeTimerModeCue)) != 0u)
            {
                timer.clockRemainder = 0u;
            }
            break;
        }
        case kEeTimerCompareOffset:
            timer.compare = value & 0xFFFFu;
            break;
        case kEeTimerHoldOffset:
            timer.hold = value & 0xFFFFu;
            break;
        default:
            return false;
        }
        return true;
    }

    if (isGsPrivReg(address))
    {
        // NB: unreachable from write8/16/32/64 today since those all funnel IO
        // register writes through addresses in PS2_IO_BASE's range, which is
        // disjoint from PS2_GS_PRIV_REG_BASE; kept correct for direct callers.
        m_ioRegisters[address] = value;
        const uint32_t off = address & 7u;
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            writeCsrHalf(gs_regs.csr, off, value);
        }
        else if (uint64_t *reg = gsRegPtr(gs_regs, address))
        {
            const uint64_t mask = 0xFFFFFFFFull << (off * 8u);
            *reg = (*reg & ~mask) | (static_cast<uint64_t>(value) << (off * 8u));
        }
        m_gsWriteCount.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    if (address >= 0x10002000 && address <= 0x10002030)
    {
        if (address == 0x10002010)
        {
            m_ioRegisters[address] = value & ~(1u << 31);
            if (value & (1u << 30))
            {
                m_ioRegisters[0x10002000] = 0;
                m_ioRegisters[0x10002020] = 0;
                m_ioRegisters[0x10002030] = 0;
            }
        }
        else
        {
            m_ioRegisters[address] = value;
        }
        return true;
    }

    if (address == 0x1000E010u)
    {
        const uint32_t current = m_ioRegisters.count(address) ? m_ioRegisters[address] : 0u;
        uint32_t status = current & 0x3FFu;
        uint32_t mask = (current >> 16) & 0x3FFu;

        // D_STAT low bits are W1C status, high bits [16..25] toggle masks on write-one.
        status &= ~(value & 0x3FFu);
        mask ^= ((value >> 16) & 0x3FFu);

        uint32_t next = (current & ~((0x3FFu) | (0x3FFu << 16) | (1u << 31)));
        next |= status | (mask << 16);
        if ((status & mask) != 0u)
            next |= (1u << 31);
        m_ioRegisters[address] = next;
        return true;
    }

    m_ioRegisters[address] = value;

    if (address >= 0x10003C00u && address < 0x10003E00u)
    {
        m_vifWriteCount.fetch_add(1, std::memory_order_relaxed);
        {
            static const bool diag = [] { const char *e = std::getenv("PS2X_VU1_BADKICK"); return e && *e; }();
            static uint32_t s_logged = 0;
            if (diag && ++s_logged <= 2000u)
                std::fprintf(stderr, "[vu1diag] EE write VIF1 reg 0x%x = 0x%x\n", address, value);
        }

        switch (address)
        {
        case 0x10003C10u:     // VIF1_FBRST
            if (value & 0x1u) // RST
            {
                const bool wasPath3Masked = m_path3Masked;
                std::memset(&vif1_regs, 0, sizeof(vif1_regs));
                m_vif1PendingPath2ImageQwc = 0u;
                m_vif1PendingPath2DirectHl = false;
                m_path3Masked = false;
                if (wasPath3Masked)
                    flushMaskedPath3Packets();
            }
            if (value & 0x8u) // STC
            {
                vif1_regs.stat &= ~((1u << 8) | (1u << 9) | (1u << 10) | (1u << 11) | (1u << 12) | (1u << 13));
            }
            break;
        case 0x10003C30u:
            vif1_regs.mark = value & 0xFFFFu;
            vif1_regs.stat &= ~(1u << 6); // clear MRK flag on CPU write
            break;
        case 0x10003C40u:
            vif1_regs.cycle = value & 0xFFFFu;
            break;
        case 0x10003C50u:
            vif1_regs.mode = value & 0x3u;
            break;
        case 0x10003C60u:
            vif1_regs.num = value & 0xFFu;
            break;
        case 0x10003C70u:
            vif1_regs.mask = value;
            break;
        case 0x10003C80u:
            vif1_regs.code = value;
            break;
        case 0x10003C90u:
            vif1_regs.itops = value & 0x3FFu;
            break;
        case 0x10003CA0u:
            vif1_regs.base = value & 0x3FFu;
            break;
        case 0x10003CB0u:
            vif1_regs.ofst = value & 0x3FFu;
            break;
        case 0x10003CC0u:
            vif1_regs.tops = value & 0x3FFu;
            break;
        case 0x10003CD0u:
            vif1_regs.itop = value & 0x3FFu;
            break;
        case 0x10003CE0u:
            vif1_regs.top = value & 0x3FFu;
            break;
        default:
            break;
        }

        return true;
    }

    if (address >= 0x10003800u && address < 0x10003A00u)
    {
        m_vifWriteCount.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    if (address >= 0x10008000 && address < 0x1000F000)
    {
        if ((address & 0xFF) == 0x00 && (value & 0x100))
        {
            const auto dctrlIt = m_ioRegisters.find(0x1000E000u);
            const bool dmacEnabled = (dctrlIt == m_ioRegisters.end()) || ((dctrlIt->second & 0x1u) != 0u);
            if (!dmacEnabled)
            {
                return true;
            }

            const uint32_t channelBase = address & 0xFFFFFF00;
            const uint32_t madr = m_ioRegisters[channelBase + 0x10];
            const uint32_t qwc = m_ioRegisters[channelBase + 0x20];
            m_dmaStartCount.fetch_add(1, std::memory_order_relaxed);

            if (tryProcessScratchpadDma(channelBase, value))
            {
                return true;
            }

            if ((channelBase == 0x1000A000u || channelBase == 0x10009000u || channelBase == 0x10008000u) && (m_gsVRAM || channelBase == 0x10008000u))
            {
                auto enqueueTransfer = [&](uint32_t srcAddr, uint32_t qwCount)
                {
                    if (qwCount == 0)
                        return;
                    const bool scratch = isScratchpad(srcAddr);
                    PendingTransfer pt;
                    pt.fromScratchpad = scratch;
                    pt.srcAddr = srcAddr;
                    pt.qwc = qwCount;
                    if (channelBase == 0x1000A000u)
                        m_pendingGifTransfers.push_back(pt);
                    else if (channelBase == 0x10009000u)
                        m_pendingVif1Transfers.push_back(pt);
                    else if (channelBase == 0x10008000u)
                        m_pendingVif0Transfers.push_back(pt);
                };

                uint32_t chcr = value;
                uint32_t mode = (chcr >> 2) & 0x3;
                // PS2X_DMA_TRACE=<seconds>: log GIF DMA kicks (and their chain tags) from that time on.
                static const double dmaTraceStart = [] { const char *e = std::getenv("PS2X_DMA_TRACE"); return e ? std::atof(e) : -1.0; }();
                static const auto dmaT0 = std::chrono::steady_clock::now();
                static uint32_t dmaTraceCount = 0;
                const bool traceThis = dmaTraceStart >= 0.0 && channelBase == 0x1000A000u && dmaTraceCount < 200u &&
                                       std::chrono::duration<double>(std::chrono::steady_clock::now() - dmaT0).count() >= dmaTraceStart;
                if (traceThis)
                {
                    ++dmaTraceCount;
                    std::fprintf(stderr, "[dma] GIF kick chcr=0x%x mode=%u madr=0x%x qwc=%u tadr=0x%x\n", chcr, mode, madr, qwc,
                                 m_ioRegisters[channelBase + 0x30]);
                }

                if (mode == 0 && qwc > 0)
                {
                    enqueueTransfer(madr, qwc);
                }
                else if (mode == 1)
                {
                    uint32_t tagAddr = m_ioRegisters[channelBase + 0x30];
                    uint32_t asr0 = m_ioRegisters[channelBase + 0x40];
                    uint32_t asr1 = m_ioRegisters[channelBase + 0x50];
                    uint32_t asp = (chcr >> 4) & 0x3u;
                    const bool tieEnabled = (chcr & (1u << 7)) != 0u;
                    // Only a guard against a chain that never ends (real hardware has no limit). It was
                    // 4096: The Sims 2's lot frames have more tags than that, so the rest of the frame
                    // (draws and VU1 microcode uploads) was dropped; a dropped upload left the next list
                    // running another program's code (bad XGKICKs, VU1 data trashed for good).
                    const int kMaxChainTags = 1 << 20;
                    std::vector<uint8_t> chainBuf;

                    auto appendData = [&](uint32_t srcAddr, uint32_t qwCount)
                    {
                        const uint64_t bytes64 = static_cast<uint64_t>(qwCount) * 16ull;
                        uint32_t bytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
                        const bool scratch = isScratchpad(srcAddr);
                        uint32_t src = 0;
                        src = translateAddress(srcAddr);
                        const uint8_t *base2;
                        uint32_t maxSz2;
                        if (scratch)
                        {
                            base2 = m_scratchpad;
                            maxSz2 = PS2_SCRATCHPAD_SIZE;
                        }
                        else
                        {
                            base2 = m_rdram;
                            maxSz2 = PS2_RAM_SIZE;
                        }

                        while (bytes > 0)
                        {
                            if (src >= maxSz2)
                                src = 0;
                            uint32_t chunk = bytes;
                            if (src + chunk > maxSz2)
                                chunk = maxSz2 - src;
                            if (chunk == 0)
                                break;
                            chainBuf.insert(chainBuf.end(), base2 + src, base2 + src + chunk);
                            bytes -= chunk;
                            src += chunk;
                        }
                    };

                    auto appendVifTagData = [&](uint32_t localTagAddr)
                    {
                        uint32_t tagPhys = 0u;
                        const bool tagScratch = isScratchpad(localTagAddr);
                        tagPhys = translateAddress(localTagAddr);

                        const uint8_t *localBase = tagScratch ? m_scratchpad : m_rdram;
                        const uint32_t localMax = tagScratch ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
                        if (tagPhys + 16u > localMax)
                            return;

                        // CHCR.TTE sends the DMAtag's upper 64 bits to the channel before
                        // the tag payload. VIF chains use those bytes for two VIFcodes.
                        chainBuf.insert(chainBuf.end(), localBase + tagPhys + 8u, localBase + tagPhys + 16u);
                    };

                    const bool isVifChannel =
                        channelBase == 0x10009000u || channelBase == 0x10008000u;
                    const bool transferTagData = isVifChannel && (chcr & 0x40u) != 0u;

                    int tagsProcessed = 0;
                    uint32_t lastTagUpper = (chcr >> 16) & 0xFFFFu;

                    while (tagsProcessed < kMaxChainTags)
                    {
                        const uint32_t currentTagAddr = tagAddr;
                        const bool tagInSPR = isScratchpad(tagAddr);
                        uint32_t physTag = 0;
                        try
                        {
                            physTag = translateAddress(tagAddr);
                        }
                        catch (...)
                        {
                            break;
                        }
                        const uint8_t *tagBase;
                        uint32_t tagMax;
                        if (tagInSPR)
                        {
                            tagBase = m_scratchpad;
                            tagMax = PS2_SCRATCHPAD_SIZE;
                        }
                        else
                        {
                            tagBase = m_rdram;
                            tagMax = PS2_RAM_SIZE;
                        }
                        if (physTag + 16 > tagMax)
                            break;

                        const uint8_t *tp = tagBase + physTag;
                        uint64_t tag = loadScalar<uint64_t>(tp, 0, 16, "dma chain tag", tagAddr);
                        uint16_t tagQwc = static_cast<uint16_t>(tag & 0xFFFF);
                        uint32_t id = static_cast<uint32_t>((tag >> 28) & 0x7);
                        const bool irq = ((tag >> 31) & 0x1ull) != 0ull;
                        uint32_t addr = static_cast<uint32_t>((tag >> 32) & 0x7FFFFFFF);
                        lastTagUpper = static_cast<uint32_t>((tag >> 16) & 0xFFFFu);
                        ++tagsProcessed;
                        if (traceThis && tagsProcessed <= 12)
                        {
                            std::fprintf(stderr, "[dma]   tag@0x%x id=%u qwc=%u addr=0x%x\n", currentTagAddr, (unsigned)((tag >> 28) & 7), (unsigned)(tag & 0xFFFF), (unsigned)((tag >> 32) & 0x7FFFFFFF));
                            const uint32_t id = static_cast<uint32_t>((tag >> 28) & 7);
                            const uint32_t q = static_cast<uint32_t>(tag & 0xFFFF);
                            if (dmaTraceCount <= 1u && (id == 1u || id == 2u) && q <= 16u)
                                for (uint32_t i = 0; i < q; ++i)
                                {
                                    uint64_t a = 0, b = 0;
                                    std::memcpy(&a, tp + 16 + i * 16, 8);
                                    std::memcpy(&b, tp + 24 + i * 16, 8);
                                    std::fprintf(stderr, "[dma]      q%u %016llx %016llx\n", i, (unsigned long long)a, (unsigned long long)b);
                                }
                        }

                        uint32_t dataAddr = 0;
                        bool hasPayload = (tagQwc > 0);
                        bool endChain = false;

                        switch (id)
                        {
                        case 0:
                            dataAddr = addr;
                            tagAddr = tagAddr + 16;
                            endChain = true;
                            break;
                        case 1:
                            dataAddr = tagAddr + 16;
                            tagAddr = dataAddr + static_cast<uint32_t>(tagQwc) * 16u;
                            break;
                        case 2:
                            dataAddr = tagAddr + 16;
                            tagAddr = addr;
                            break;
                        case 3:
                        case 4:
                            dataAddr = addr;
                            tagAddr = tagAddr + 16;
                            break;
                        case 5:
                            dataAddr = tagAddr + 16;
                            {
                                const uint32_t retAddr = dataAddr + static_cast<uint32_t>(tagQwc) * 16u;
                                if (asp == 0u)
                                {
                                    asr0 = retAddr;
                                    asp = 1u;
                                }
                                else if (asp == 1u)
                                {
                                    asr1 = retAddr;
                                    asp = 2u;
                                }
                            }
                            tagAddr = addr;
                            break;
                        case 6:
                            dataAddr = tagAddr + 16;
                            if (asp == 2u)
                            {
                                tagAddr = asr1;
                                asp = 1u;
                            }
                            else if (asp == 1u)
                            {
                                tagAddr = asr0;
                                asp = 0u;
                            }
                            else
                            {
                                endChain = true;
                            }
                            break;
                        case 7:
                            dataAddr = tagAddr + 16;
                            endChain = true;
                            break;
                        default:
                            hasPayload = false;
                            endChain = true;
                            break;
                        }

                        if (transferTagData)
                            appendVifTagData(currentTagAddr);

                        if (hasPayload)
                            appendData(dataAddr, tagQwc);
                        if (irq && tieEnabled)
                            endChain = true;
                        if (endChain)
                            break;
                    }
                    {
                        static const bool diag = [] { const char *e = std::getenv("PS2X_VU1_BADKICK"); return e && *e; }();
                        static int maxTags = 0;
                        if (diag && tagsProcessed > maxTags)
                        {
                            maxTags = tagsProcessed;
                            std::fprintf(stderr, "[vu1diag] DMA chain on 0x%x: new max %d tags, %zu bytes\n", channelBase, tagsProcessed, chainBuf.size());
                        }
                    }
                    if (tagsProcessed >= kMaxChainTags)
                    {
                        static bool warned = false;
                        if (!warned)
                        {
                            warned = true;
                            std::fprintf(stderr, "[dma] chain on channel 0x%x still going after %d tags: rest dropped\n", channelBase, kMaxChainTags);
                        }
                    }

                    m_ioRegisters[channelBase + 0x30] = tagAddr;
                    m_ioRegisters[channelBase + 0x40] = asr0;
                    m_ioRegisters[channelBase + 0x50] = asr1;
                    chcr = (chcr & ~(0x3u << 4)) | ((asp & 0x3u) << 4);
                    chcr = (chcr & 0x0000FFFFu) | (lastTagUpper << 16);
                    m_ioRegisters[channelBase + 0x00] = chcr;

                    if (!chainBuf.empty())
                    {
                        PendingTransfer pt;
                        pt.fromScratchpad = false;
                        pt.srcAddr = 0;
                        pt.qwc = 0;
                        pt.chainData = std::move(chainBuf);
                        if (channelBase == 0x1000A000)
                        {
                            m_pendingGifTransfers.push_back(std::move(pt));
                        }
                        else if (channelBase == 0x10009000u)
                        {
                            m_pendingVif1Transfers.push_back(std::move(pt));
                        }
                        else if (channelBase == 0x10008000u)
                        {
                            m_pendingVif0Transfers.push_back(std::move(pt));
                        }
                    }
                    // else if (channelBase == 0x10009000u)
                    // {

                    // }
                }
                else if (qwc > 0)
                {
                    enqueueTransfer(madr, qwc);
                }

                const bool autoProcessTransfers =
                    (channelBase == 0x1000A000u) ? (m_gifPacketCallback || m_gifArbiter != nullptr) : true;
                if (autoProcessTransfers)
                {
                    processPendingTransfers();
                }
            }
        }
        return true;
    }

    if (address >= 0x10000000 && address < 0x10010000)
    {
        if (address >= 0x10000200 && address < 0x10000300)
        {
            return true;
        }
        if (address >= 0x10000000 && address < 0x10000100)
        {
            return true;
        }
    }

    return false;
}

bool PS2Memory::tryProcessScratchpadDma(uint32_t channelBase, uint32_t chcr)
{
    static constexpr uint32_t kSprFromChannel = 0x1000D000u;
    static constexpr uint32_t kSprToChannel = 0x1000D400u;
    if (channelBase != kSprFromChannel && channelBase != kSprToChannel)
        return false;

    const uint32_t mode = (chcr >> 2u) & 0x3u;
    if (mode != 0u)
        return false;

    const uint32_t qwc = m_ioRegisters[channelBase + 0x20u] & 0xFFFFu;
    const uint32_t byteCount = qwc * 16u;
    const uint32_t originalMadr = m_ioRegisters[channelBase + 0x10u] & 0x7FFFFFF0u;
    const uint32_t originalSadr = m_ioRegisters[channelBase + 0x80u] & 0x3FF0u;

    uint32_t mainOffset = 0u;
    try
    {
        mainOffset = translateAddress(originalMadr);
    }
    catch (const std::exception &)
    {
        return false;
    }

    if (mainOffset > PS2_RAM_SIZE || byteCount > PS2_RAM_SIZE - mainOffset)
        return false;

    const bool fromScratchpad = channelBase == kSprFromChannel;
    uint32_t scratchOffset = originalSadr;
    uint32_t bytesLeft = byteCount;
    uint32_t copied = 0u;
    while (bytesLeft != 0u)
    {
        const uint32_t scratchChunk = PS2_SCRATCHPAD_SIZE - scratchOffset;
        const uint32_t chunk = std::min(bytesLeft, scratchChunk);
        if (fromScratchpad)
        {
            std::memcpy(m_rdram + mainOffset + copied, m_scratchpad + scratchOffset, chunk);
            markModified(mainOffset + copied, chunk);
        }
        else
        {
            std::memcpy(m_scratchpad + scratchOffset, m_rdram + mainOffset + copied, chunk);
        }

        copied += chunk;
        bytesLeft -= chunk;
        scratchOffset = (scratchOffset + chunk) & (PS2_SCRATCHPAD_SIZE - 1u);
    }

    m_ioRegisters[channelBase + 0x10u] = (originalMadr + byteCount) & 0x7FFFFFF0u;
    m_ioRegisters[channelBase + 0x20u] = 0u;
    m_ioRegisters[channelBase + 0x80u] = (originalSadr + byteCount) & 0x3FF0u;
    completeDmacChannel(channelBase, fromScratchpad ? 8u : 9u);
    return true;
}

void PS2Memory::completeDmacChannel(uint32_t channelBase, uint32_t cause)
{
    static constexpr uint32_t kDStat = 0x1000E010u;
    m_ioRegisters[channelBase] &= ~0x100u;

    uint32_t dstat = m_ioRegisters.count(kDStat) ? m_ioRegisters[kDStat] : 0u;
    dstat |= 1u << cause;
    const uint32_t status = dstat & 0x3FFu;
    const uint32_t mask = (dstat >> 16u) & 0x3FFu;
    if ((status & mask) != 0u)
        dstat |= 1u << 31u;
    else
        dstat &= ~(1u << 31u);
    m_ioRegisters[kDStat] = dstat;
    queueCompletedDmacCause(cause);
}

void PS2Memory::processPendingTransfers()
{
    // GIF/VIF1 payloads go to the async worker in this same order (copies; chain data is moved).
    Vif1AsyncWorker *async = vif1AsyncFor(this);
    auto emitGif = [&](const uint8_t *data, uint32_t size)
    {
        if (async)
            vif1AsyncPush(async, AsyncItemKind::Gif, std::vector<uint8_t>(data, data + size));
        else
            submitGifPacket(GifPathId::Path3, data, size, false);
    };
    auto emitVif1 = [&](const uint8_t *data, uint32_t size)
    {
        if (async)
            vif1AsyncPush(async, AsyncItemKind::Vif1, std::vector<uint8_t>(data, data + size));
        else
            processVIF1Data(data, size);
    };
    const bool hadGif = !m_pendingGifTransfers.empty();
    uint32_t observedGifQwc = 0u;
    for (const auto &transfer : m_pendingGifTransfers)
    {
        const uint64_t transferQwc = !transfer.chainData.empty()
                                         ? (transfer.chainData.size() / 16u)
                                         : transfer.qwc;
        observedGifQwc = static_cast<uint32_t>(std::min<uint64_t>(16u, static_cast<uint64_t>(observedGifQwc) + transferQwc));
    }
    if (observedGifQwc != 0u)
    {
        constexpr uint32_t kGifStat = 0x10003020u;
        constexpr uint32_t kGifFqcMask = 0x1F000000u;
        uint32_t &gifStat = m_ioRegisters[kGifStat];
        gifStat = (gifStat & ~kGifFqcMask) | (observedGifQwc << 24u);
    }

    for (size_t idx = 0; idx < m_pendingGifTransfers.size(); ++idx)
    {
        auto &p = m_pendingGifTransfers[idx];
        if (!p.chainData.empty())
        {
            m_seenGifCopy = true;
            m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
            if (async)
                vif1AsyncPush(async, AsyncItemKind::Gif, std::move(p.chainData));
            else
                submitGifPacket(GifPathId::Path3, p.chainData.data(), static_cast<uint32_t>(p.chainData.size()), false);
        }
        else if (p.qwc > 0)
        {
            const uint64_t bytes64 = static_cast<uint64_t>(p.qwc) * 16ull;
            uint32_t sizeBytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
            uint32_t srcPhys = 0;
            try
            {
                srcPhys = translateAddress(p.srcAddr);
            }
            catch (const std::exception &)
            {
                continue;
            }
            if (p.fromScratchpad)
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft >= 16)
                {
                    if (srcPhys >= PS2_SCRATCHPAD_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_SCRATCHPAD_SIZE)
                        chunk = PS2_SCRATCHPAD_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    m_seenGifCopy = true;
                    m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
                    emitGif(m_scratchpad + srcPhys, chunk);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
            else
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft >= 16)
                {
                    if (srcPhys >= PS2_RAM_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_RAM_SIZE)
                        chunk = PS2_RAM_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    m_seenGifCopy = true;
                    m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
                    emitGif(m_rdram + srcPhys, chunk);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
        }
    }
    m_pendingGifTransfers.clear();

    const bool hadVif0 = !m_pendingVif0Transfers.empty();
    for (auto &p : m_pendingVif0Transfers)
    {
        if (!p.chainData.empty())
        {
            processVIF0Data(p.chainData.data(), static_cast<uint32_t>(p.chainData.size()));
        }
        else if (p.qwc > 0)
        {
            uint32_t srcPhys = 0;
            const uint64_t bytes64 = static_cast<uint64_t>(p.qwc) * 16ull;
            uint32_t sizeBytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
            try
            {
                srcPhys = translateAddress(p.srcAddr);
            }
            catch (const std::exception &)
            {
                continue;
            }
            if (p.fromScratchpad)
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft > 0)
                {
                    if (srcPhys >= PS2_SCRATCHPAD_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_SCRATCHPAD_SIZE)
                        chunk = PS2_SCRATCHPAD_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    processVIF0Data(m_scratchpad + srcPhys, chunk);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
            else
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft > 0)
                {
                    if (srcPhys >= PS2_RAM_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_RAM_SIZE)
                        chunk = PS2_RAM_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    processVIF0Data(srcPhys, chunk);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
        }
    }
    m_pendingVif0Transfers.clear();

    const bool hadVif1 = !m_pendingVif1Transfers.empty();
    for (auto &p : m_pendingVif1Transfers)
    {
        if (!p.chainData.empty())
        {
            if (async)
                vif1AsyncPush(async, AsyncItemKind::Vif1, std::move(p.chainData));
            else
                processVIF1Data(p.chainData.data(), static_cast<uint32_t>(p.chainData.size()));
        }
        else if (p.qwc > 0)
        {
            uint32_t srcPhys = 0;
            const uint64_t bytes64 = static_cast<uint64_t>(p.qwc) * 16ull;
            uint32_t sizeBytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
            try
            {
                srcPhys = translateAddress(p.srcAddr);
            }
            catch (const std::exception &)
            {
                continue;
            }
            if (p.fromScratchpad)
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft > 0)
                {
                    if (srcPhys >= PS2_SCRATCHPAD_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_SCRATCHPAD_SIZE)
                        chunk = PS2_SCRATCHPAD_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    emitVif1(m_scratchpad + srcPhys, chunk);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
            else
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft > 0)
                {
                    if (srcPhys >= PS2_RAM_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_RAM_SIZE)
                        chunk = PS2_RAM_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    emitVif1(m_rdram + srcPhys, chunk);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
        }
    }
    m_pendingVif1Transfers.clear();

    if (async && (hadGif || hadVif1))
        vif1AsyncPush(async, AsyncItemKind::ArbiterDrain, {});
    else if (!async && m_gifArbiter)
        m_gifArbiter->drain();

    static constexpr uint32_t GIF_CHANNEL = 0x1000A000;
    static constexpr uint32_t VIF0_CHANNEL = 0x10008000;
    static constexpr uint32_t VIF1_CHANNEL = 0x10009000;
    static constexpr uint32_t D_STAT = 0x1000E010u;

    auto raiseDStatChannel = [&](uint32_t channelBit)
    {
        uint32_t dstat = m_ioRegisters.count(D_STAT) ? m_ioRegisters[D_STAT] : 0u;
        dstat |= (1u << channelBit);

        const uint32_t status = dstat & 0x3FFu;
        const uint32_t mask = (dstat >> 16) & 0x3FFu;
        if ((status & mask) != 0u)
            dstat |= (1u << 31);
        else
            dstat &= ~(1u << 31);

        m_ioRegisters[D_STAT] = dstat;
    };

    if (hadGif)
    {
        raiseDStatChannel(2u); // GIF channel
        queueCompletedDmacCause(2u);
        m_ioRegisters[GIF_CHANNEL + 0x00] &= ~0x100u;
        m_ioRegisters[GIF_CHANNEL + 0x20] = 0;
    }
    if (hadVif0)
    {
        raiseDStatChannel(0u); // VIF0 channel
        queueCompletedDmacCause(0u);
        m_ioRegisters[VIF0_CHANNEL + 0x00] &= ~0x100u;
        m_ioRegisters[VIF0_CHANNEL + 0x20] = 0;
    }
    if (hadVif1)
    {
        raiseDStatChannel(1u); // VIF1 channel
        queueCompletedDmacCause(1u);
        m_ioRegisters[VIF1_CHANNEL + 0x00] &= ~0x100u;
        m_ioRegisters[VIF1_CHANNEL + 0x20] = 0;
    }
}

void PS2Memory::queueCompletedDmacCause(uint32_t cause)
{
    std::lock_guard<std::mutex> lock(m_completedDmacMutex);
    m_completedDmacCauses.push_back(cause);
}

std::vector<uint32_t> PS2Memory::consumeCompletedDmacCauses()
{
    std::lock_guard<std::mutex> lock(m_completedDmacMutex);
    std::vector<uint32_t> causes;
    causes.swap(m_completedDmacCauses);
    return causes;
}

void PS2Memory::flushMaskedPath3Packets(bool drainImmediately)
{
    if (m_path3Masked || m_path3MaskedFifo.empty())
        return;

    auto emit = [&](const uint8_t *packetData, uint32_t packetSize)
    {
        if (m_gifArbiter)
            m_gifArbiter->submit(GifPathId::Path3, packetData, packetSize, false);
        else if (m_gifPacketCallback)
            m_gifPacketCallback(packetData, packetSize);
    };

    for (const auto &packet : m_path3MaskedFifo)
    {
        if (packet.size() >= 16u)
            emit(packet.data(), static_cast<uint32_t>(packet.size()));
    }
    m_path3MaskedFifo.clear();

    if (m_gifArbiter && drainImmediately)
        m_gifArbiter->drain();
}

void PS2Memory::submitGifPacket(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes, bool drainImmediately, bool path2DirectHl)
{
    if (!data || sizeBytes < 16)
        return;
    TimingScope scope(pathId == GifPathId::Path1 ? kCatKick : (pathId == GifPathId::Path2 ? kCatPath2 : kCatPath3));

    if (pathId == GifPathId::Path3)
    {
        if (m_path3Masked)
        {
            m_path3MaskedFifo.emplace_back(data, data + sizeBytes);
            return;
        }
        flushMaskedPath3Packets(false);
    }

    // On the VIF1/VU1 worker with nothing queued in the arbiter, submit + drain would hand exactly
    // this packet to the runtime's arbiter callback, which queues it for the GS feed: skip the
    // arbiter's copy and sort (XGKICKs: ~55k packets/s in a busy scene).
    if (t_onVif1AsyncWorker && g_vif1Async && g_vif1Async->owner == this && m_gifArbiter && drainImmediately &&
        m_gifArbiter->empty() && ps2xGsFeedSubmit(data, sizeBytes))
        return;

    if (m_gifArbiter)
        m_gifArbiter->submit(pathId, data, sizeBytes, path2DirectHl);
    else if (m_gifPacketCallback)
        m_gifPacketCallback(data, sizeBytes);

    if (m_gifArbiter && drainImmediately)
        m_gifArbiter->drain();
}

void PS2Memory::submitPath3PacketOwned(std::vector<uint8_t> &&data)
{
    if (data.size() < 16u)
        return;
    TimingScope scope(kCatPath3);
    if (m_path3Masked)
    {
        m_path3MaskedFifo.emplace_back(std::move(data));
        return;
    }
    flushMaskedPath3Packets(false);
    if (m_gifArbiter)
        m_gifArbiter->submit(GifPathId::Path3, std::move(data));
    else if (m_gifPacketCallback)
        m_gifPacketCallback(data.data(), static_cast<uint32_t>(data.size()));
}

void PS2Memory::processGIFPacket(uint32_t srcPhysAddr, uint32_t qwCount)
{
    vif1AsyncDrain(this, kDrainGifPacket);
    if (!m_rdram || qwCount == 0)
        return;
    const uint64_t bytes64 = static_cast<uint64_t>(qwCount) * 16ull;
    uint32_t sizeBytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
    uint32_t bytesLeft = sizeBytes;
    while (bytesLeft >= 16)
    {
        if (srcPhysAddr >= PS2_RAM_SIZE)
            srcPhysAddr = 0;
        uint32_t chunk = bytesLeft;
        if (srcPhysAddr + chunk > PS2_RAM_SIZE)
            chunk = PS2_RAM_SIZE - srcPhysAddr;
        if (chunk == 0)
            break;

        m_seenGifCopy = true;
        m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
        submitGifPacket(GifPathId::Path3, m_rdram + srcPhysAddr, chunk);

        bytesLeft -= chunk;
        srcPhysAddr += chunk;
    }
}

void PS2Memory::processGIFPacket(const uint8_t *data, uint32_t sizeBytes)
{
    vif1AsyncDrain(this, kDrainGifPacket);
    if (m_gifArbiter)
        submitGifPacket(GifPathId::Path3, data, sizeBytes);
    else if (m_gifPacketCallback && data && sizeBytes >= 16)
        m_gifPacketCallback(data, sizeBytes);
}

bool PS2Memory::tryProcessNativeGifImageUploadChain(GS &gs, uint32_t tadr, uint32_t chcr)
{
    vif1AsyncDrain(this, kDrainNativeGif); // writes into the GS directly, after every queued packet
    static constexpr uint32_t GIF_CHANNEL = 0x1000A000u;
    static constexpr uint32_t D_STAT = 0x1000E010u;
    static constexpr uint32_t D_CTRL = 0x1000E000u;

    if (!m_rdram || !m_gsVRAM || m_path3Masked)
        return false;
    if (m_gifArbiter && !m_gifArbiter->empty())
        return false;
    if ((chcr & 0x100u) == 0u || ((chcr >> 2u) & 0x3u) != 1u)
        return false;
    if ((chcr & (1u << 7u)) != 0u || ((chcr >> 4u) & 0x3u) != 0u)
        return false;

    const auto dctrlIt = m_ioRegisters.find(D_CTRL);
    if (dctrlIt != m_ioRegisters.end() && ((dctrlIt->second & 0x1u) == 0u))
        return false;

    auto resolveContiguous = [&](uint32_t guestAddr, uint32_t bytes, const uint8_t *&out) -> bool
    {
        try
        {
            const bool scratch = isScratchpad(guestAddr);
            const uint32_t phys = translateAddress(guestAddr);
            const uint8_t *base = scratch ? m_scratchpad : m_rdram;
            const uint32_t limit = scratch ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
            if (!base || phys > limit || bytes > limit - phys)
                return false;
            out = base + phys;
            return true;
        }
        catch (const std::exception &)
        {
            return false;
        }
    };

    auto loadDmaTagAt = [&](uint32_t guestAddr, DmaTagView &out) -> bool
    {
        const uint8_t *ptr = nullptr;
        if (!resolveContiguous(guestAddr, 16u, ptr))
            return false;
        out = decodeDmaTag(loadScalar<uint64_t>(ptr, 0u, 16u, "native gif dma tag", guestAddr));
        return true;
    };

    auto decodeSetupPayload = [&](const uint8_t *payload, uint64_t (&regs)[4]) -> bool
    {
        const uint64_t tagLo = loadScalar<uint64_t>(payload, 0u, 80u, "native gif setup tag", 0u);
        const uint64_t tagHi = loadScalar<uint64_t>(payload, 8u, 80u, "native gif setup regs", 0u);
        if (gifTagNloop(tagLo) != 4u ||
            gifTagFlg(tagLo) != GIF_FMT_PACKED ||
            gifTagNreg(tagLo) != 1u ||
            (tagHi & 0xFull) != 0x0Eull)
        {
            return false;
        }

        static constexpr uint8_t kExpectedRegs[4] = {
            GS_REG_BITBLTBUF,
            GS_REG_TRXPOS,
            GS_REG_TRXREG,
            GS_REG_TRXDIR,
        };

        uint32_t offset = 16u;
        for (uint32_t i = 0; i < 4u; ++i)
        {
            regs[i] = loadScalar<uint64_t>(payload, offset, 80u, "native gif setup value", 0u);
            const uint64_t reg = loadScalar<uint64_t>(payload, offset + 8u, 80u, "native gif setup register", 0u);
            if ((reg & 0xFFu) != kExpectedRegs[i])
                return false;
            offset += 16u;
        }

        const uint32_t trxdirMode = static_cast<uint32_t>(regs[3] & 0x3ull);
        const uint32_t rrw = static_cast<uint32_t>(regs[2] & 0xFFFull);
        const uint32_t rrh = static_cast<uint32_t>((regs[2] >> 32u) & 0xFFFull);
        return trxdirMode == 0u && rrw != 0u && rrh != 0u;
    };

    DmaTagView setupTag{};
    if (!loadDmaTagAt(tadr, setupTag) ||
        setupTag.id != 1u ||
        setupTag.qwc != 5u ||
        setupTag.irq)
    {
        return false;
    }

    const uint8_t *setupPayload = nullptr;
    const uint32_t setupPayloadAddr = tadr + 16u;
    if (!resolveContiguous(setupPayloadAddr, 5u * 16u, setupPayload))
        return false;

    uint64_t setupRegs[4] = {};
    if (!decodeSetupPayload(setupPayload, setupRegs))
        return false;

    uint32_t imageTagDmaAddr = setupPayloadAddr + 5u * 16u;
    DmaTagView imageTagDma{};
    if (!loadDmaTagAt(imageTagDmaAddr, imageTagDma) ||
        imageTagDma.id != 1u ||
        imageTagDma.qwc != 1u ||
        imageTagDma.irq)
    {
        return false;
    }

    const uint8_t *imageGifTag = nullptr;
    if (!resolveContiguous(imageTagDmaAddr + 16u, 16u, imageGifTag))
        return false;

    const uint64_t imageTagLo = loadScalar<uint64_t>(imageGifTag, 0u, 16u, "native gif image tag", imageTagDmaAddr + 16u);
    if (gifTagFlg(imageTagLo) != GIF_FMT_IMAGE)
        return false;

    const uint32_t imageQwc = gifTagNloop(imageTagLo);
    if (imageQwc == 0u)
        return false;

    const uint64_t imageBytes64 = static_cast<uint64_t>(imageQwc) * 16ull;
    if (imageBytes64 > 0xFFFFFFFFull)
        return false;
    const uint32_t imageBytes = static_cast<uint32_t>(imageBytes64);

    const uint32_t payloadTagAddr = imageTagDmaAddr + 32u;
    DmaTagView payloadTag{};
    if (!loadDmaTagAt(payloadTagAddr, payloadTag) ||
        payloadTag.qwc != imageQwc ||
        payloadTag.irq)
    {
        return false;
    }

    uint32_t imageDataAddr = 0u;
    uint32_t finalTadr = payloadTagAddr;
    uint32_t lastTagUpper = payloadTag.upper;
    if (payloadTag.id == 3u || payloadTag.id == 4u)
    {
        imageDataAddr = payloadTag.addr;
        const uint32_t terminalTagAddr = payloadTagAddr + 16u;
        DmaTagView terminalTag{};
        if (!loadDmaTagAt(terminalTagAddr, terminalTag) ||
            terminalTag.qwc != 0u ||
            terminalTag.irq ||
            (terminalTag.id != 0u && terminalTag.id != 7u))
        {
            return false;
        }
        finalTadr = (terminalTag.id == 0u) ? (terminalTagAddr + 16u) : terminalTagAddr;
        lastTagUpper = terminalTag.upper;
    }
    else if (payloadTag.id == 7u)
    {
        imageDataAddr = payloadTagAddr + 16u;
        finalTadr = payloadTagAddr;
    }
    else
    {
        return false;
    }

    const uint8_t *imageData = nullptr;
    if (!resolveContiguous(imageDataAddr, imageBytes, imageData))
        return false;

    m_dmaStartCount.fetch_add(1, std::memory_order_relaxed);
    m_seenGifCopy = true;
    m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
    gs.uploadImageNative(setupRegs[0], setupRegs[1], setupRegs[2], setupRegs[3], imageData, imageBytes);

    m_ioRegisters[GIF_CHANNEL + 0x30u] = finalTadr;
    m_ioRegisters[GIF_CHANNEL + 0x40u] = 0u;
    m_ioRegisters[GIF_CHANNEL + 0x50u] = 0u;
    m_ioRegisters[GIF_CHANNEL + 0x00u] = ((chcr & 0x0000FFFFu) | (lastTagUpper << 16u)) & ~0x100u;
    m_ioRegisters[GIF_CHANNEL + 0x20u] = 0u;

    uint32_t dstat = m_ioRegisters.count(D_STAT) ? m_ioRegisters[D_STAT] : 0u;
    dstat |= (1u << 2u);
    const uint32_t status = dstat & 0x3FFu;
    const uint32_t mask = (dstat >> 16u) & 0x3FFu;
    if ((status & mask) != 0u)
        dstat |= (1u << 31u);
    else
        dstat &= ~(1u << 31u);
    m_ioRegisters[D_STAT] = dstat;
    queueCompletedDmacCause(2u);
    return true;
}

bool PS2Memory::tryProcessNativeGifPackedChain(GS &gs, uint32_t tadr, uint32_t chcr)
{
    vif1AsyncDrain(this, kDrainNativeGif); // writes into the GS directly, after every queued packet
    static constexpr uint32_t GIF_CHANNEL = 0x1000A000u;
    static constexpr uint32_t D_STAT = 0x1000E010u;
    static constexpr uint32_t D_CTRL = 0x1000E000u;

    if (!m_rdram || !m_gsVRAM || m_path3Masked)
        return false;
    if (m_gifArbiter && !m_gifArbiter->empty())
        return false;
    if ((chcr & 0x100u) == 0u || ((chcr >> 2u) & 0x3u) != 1u)
        return false;
    if ((chcr & (1u << 7u)) != 0u || ((chcr >> 4u) & 0x3u) != 0u)
        return false;

    const auto dctrlIt = m_ioRegisters.find(D_CTRL);
    if (dctrlIt != m_ioRegisters.end() && ((dctrlIt->second & 0x1u) == 0u))
        return false;

    auto resolveContiguous = [&](uint32_t guestAddr, uint32_t bytes, const uint8_t *&out) -> bool
    {
        try
        {
            const bool scratch = isScratchpad(guestAddr);
            const uint32_t phys = translateAddress(guestAddr);
            const uint8_t *base = scratch ? m_scratchpad : m_rdram;
            const uint32_t limit = scratch ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
            if (!base || phys > limit || bytes > limit - phys)
                return false;
            out = base + phys;
            return true;
        }
        catch (const std::exception &)
        {
            return false;
        }
    };

    const uint8_t *tagPtr = nullptr;
    if (!resolveContiguous(tadr, 16u, tagPtr))
        return false;

    const DmaTagView tag = decodeDmaTag(loadScalar<uint64_t>(tagPtr, 0u, 16u, "native packed gif dma tag", tadr));
    if (tag.id != 7u || tag.qwc == 0u || tag.irq)
        return false;

    const uint64_t payloadBytes64 = static_cast<uint64_t>(tag.qwc) * 16ull;
    if (payloadBytes64 > 0xFFFFFFFFull)
        return false;
    const uint32_t payloadBytes = static_cast<uint32_t>(payloadBytes64);

    const uint8_t *payload = nullptr;
    if (!resolveContiguous(tadr + 16u, payloadBytes, payload))
        return false;
    if (!gs.processNativePackedGIFPacket(payload, payloadBytes))
        return false;

    m_dmaStartCount.fetch_add(1, std::memory_order_relaxed);
    m_seenGifCopy = true;
    m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);

    m_ioRegisters[GIF_CHANNEL + 0x30u] = tadr;
    m_ioRegisters[GIF_CHANNEL + 0x40u] = 0u;
    m_ioRegisters[GIF_CHANNEL + 0x50u] = 0u;
    m_ioRegisters[GIF_CHANNEL + 0x00u] = ((chcr & 0x0000FFFFu) | (tag.upper << 16u)) & ~0x100u;
    m_ioRegisters[GIF_CHANNEL + 0x20u] = 0u;

    uint32_t dstat = m_ioRegisters.count(D_STAT) ? m_ioRegisters[D_STAT] : 0u;
    dstat |= (1u << 2u);
    const uint32_t status = dstat & 0x3FFu;
    const uint32_t mask = (dstat >> 16u) & 0x3FFu;
    if ((status & mask) != 0u)
        dstat |= (1u << 31u);
    else
        dstat &= ~(1u << 31u);
    m_ioRegisters[D_STAT] = dstat;
    queueCompletedDmacCause(2u);
    return true;
}

int PS2Memory::pollDmaRegisters()
{
    return 0;
}

uint32_t PS2Memory::readIORegister(uint32_t address)
{
    if (isVif1AsyncSyncedIo(address))
        vif1AsyncDrain(this, kDrainIoReg);
    size_t timerIndex = 0u;
    uint32_t timerOffset = 0u;
    if (decodeEeTimerRegister(address, timerIndex, timerOffset))
    {
        if (onEeTimerThread())
            syncEeTimers();
        const EeTimer &timer = m_eeTimers[timerIndex];
        switch (timerOffset)
        {
        case kEeTimerCountOffset:
            return timer.count & 0xFFFFu;
        case kEeTimerModeOffset:
            return timer.mode & (kEeTimerModeConfigMask | kEeTimerModeStatusMask);
        case kEeTimerCompareOffset:
            return timer.compare & 0xFFFFu;
        case kEeTimerHoldOffset:
            return timer.hold & 0xFFFFu;
        default:
            return 0u;
        }
    }

    if (isGsPrivReg(address))
    {
        // NB: unreachable from read8/16/32/64 today, same reasoning as the write
        // path above; kept correct for direct callers.
        const uint32_t off = address & 7u;
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            return static_cast<uint32_t>((gs_regs.csr.load() >> (off * 8u)) & 0xFFFFFFFFull);
        }
        if (uint64_t *reg = gsRegPtr(gs_regs, address))
        {
            return static_cast<uint32_t>((*reg >> (off * 8u)) & 0xFFFFFFFFull);
        }
        return 0u;
    }

    if (address >= 0x10002000 && address <= 0x10002030)
    {
        uint32_t val = 0;
        switch (address)
        {
        case 0x10002000:
            val = m_ioRegisters[address];
            break;
        case 0x10002010:
            val = m_ioRegisters[address] & ~(1u << 31);
            break;
        case 0x10002020:
        case 0x10002030:
            val = m_ioRegisters[address];
            break;
        default:
            val = 0;
            break;
        }
        return val;
    }

    if (address == 0x10003020u) // GIF_STAT
    {
        uint32_t stat = m_ioRegisters.count(address) ? m_ioRegisters[address] : 0u;
        const uint32_t mode = m_ioRegisters.count(0x10003010u) ? m_ioRegisters[0x10003010u] : 0u;
        const uint32_t ctrl = m_ioRegisters.count(0x10003000u) ? m_ioRegisters[0x10003000u] : 0u;

        // M3R and IMT mirror GIF_MODE, PSE mirrors GIF_CTRL, and M3P is the
        // effective PATH3 mask controlled by the VIF1 MSKPATH3 command.
        stat = (stat & ~0xFu) |
               (mode & 0x1u) |
               (m_path3Masked ? 0x2u : 0u) |
               (mode & 0x4u) |
               (ctrl & 0x8u);
        return stat;
    }

    if (address >= 0x10000000 && address < 0x10010000)
    {
        if (address >= 0x10008000 && address < 0x1000F000)
        {
            if ((address & 0xFF) == 0x00)
            {
                uint32_t channelStatus = m_ioRegisters[address] & ~0x100u;
                m_ioRegisters[address] = channelStatus;
                return channelStatus;
            }
        }

        if (address >= 0x10000200 && address < 0x10000300)
        {
            return 0;
        }

        if (address >= 0x1000F200 && address <= 0x1000F260)
        {
            if (address == 0x1000F230)
            {
                return 0x60000;
            }
            if (address == 0x1000F240)
            {
                return 0xF0000002;
            }
            return 0;
        }
    }

    auto it = m_ioRegisters.find(address);
    if (it != m_ioRegisters.end())
    {
        return it->second;
    }

    return 0;
}

void PS2Memory::registerCodeRegion(uint32_t start, uint32_t end)
{
    if (end <= start)
    {
        std::cerr << "Ignoring invalid code region: start=0x" << std::hex << start
                  << " end=0x" << end << std::dec << std::endl;
        return;
    }

    if ((end - start) > PS2_RAM_SIZE)
    {
        std::cerr << "Ignoring oversized code region: start=0x" << std::hex << start
                  << " end=0x" << end << std::dec << std::endl;
        return;
    }

    for (const auto &existing : m_codeRegions)
    {
        if (existing.start == start && existing.end == end)
        {
            return;
        }
    }

    CodeRegion region;
    region.start = start;
    region.end = end;

    size_t sizeInWords = (end - start + 3u) / 4u;
    region.modified.resize(sizeInWords, false);

    m_codeRegions.push_back(region);
    RUNTIME_LOG("Registered code region: " << std::hex << start << " - " << end << std::dec);
}

bool PS2Memory::isAddressInRegion(uint32_t address, const CodeRegion &region)
{
    return (address >= region.start && address < region.end);
}

bool PS2Memory::isCodeAddress(uint32_t address) const
{
    for (const auto &region : m_codeRegions)
    {
        if (address >= region.start && address < region.end)
        {
            return true;
        }
    }
    return false;
}

void PS2Memory::markModified(uint32_t address, uint32_t size)
{
    if (size == 0)
    {
        return;
    }

    const uint64_t writeEnd = static_cast<uint64_t>(address) + static_cast<uint64_t>(size);
    for (auto &region : m_codeRegions)
    {
        const uint64_t regionStart = region.start;
        const uint64_t regionEnd = region.end;
        if (writeEnd <= regionStart || static_cast<uint64_t>(address) >= regionEnd)
        {
            continue;
        }

        uint32_t overlapStart = static_cast<uint32_t>(std::max<uint64_t>(address, regionStart));
        uint32_t overlapEnd = static_cast<uint32_t>(std::min<uint64_t>(writeEnd, regionEnd));

        for (uint32_t addr = overlapStart; addr < overlapEnd; addr += 4)
        {
            size_t bitIndex = (addr - region.start) / 4;
            if (bitIndex < region.modified.size())
            {
                region.modified[bitIndex] = true;
                RUNTIME_LOG("Marked code at " << std::hex << addr << std::dec << " as modified");
            }
        }
    }
}

bool PS2Memory::isCodeModified(uint32_t address, uint32_t size)
{
    if (size == 0)
    {
        return false;
    }

    const uint64_t writeEnd = static_cast<uint64_t>(address) + static_cast<uint64_t>(size);
    for (const auto &region : m_codeRegions)
    {
        const uint64_t regionStart = region.start;
        const uint64_t regionEnd = region.end;
        if (writeEnd <= regionStart || static_cast<uint64_t>(address) >= regionEnd)
        {
            continue;
        }

        uint32_t overlapStart = static_cast<uint32_t>(std::max<uint64_t>(address, regionStart));
        uint32_t overlapEnd = static_cast<uint32_t>(std::min<uint64_t>(writeEnd, regionEnd));

        for (uint32_t addr = overlapStart; addr < overlapEnd; addr += 4)
        {
            size_t bitIndex = (addr - region.start) / 4;
            if (bitIndex < region.modified.size() && region.modified[bitIndex])
            {
                return true; // Found modified code
            }
        }
    }

    return false; // No modifications found
}

void PS2Memory::clearModifiedFlag(uint32_t address, uint32_t size)
{
    if (size == 0)
    {
        return;
    }

    const uint64_t writeEnd = static_cast<uint64_t>(address) + static_cast<uint64_t>(size);
    for (auto &region : m_codeRegions)
    {
        const uint64_t regionStart = region.start;
        const uint64_t regionEnd = region.end;
        if (writeEnd <= regionStart || static_cast<uint64_t>(address) >= regionEnd)
        {
            continue;
        }

        uint32_t overlapStart = static_cast<uint32_t>(std::max<uint64_t>(address, regionStart));
        uint32_t overlapEnd = static_cast<uint32_t>(std::min<uint64_t>(writeEnd, regionEnd));

        for (uint32_t addr = overlapStart; addr < overlapEnd; addr += 4)
        {
            size_t bitIndex = (addr - region.start) / 4;
            if (bitIndex < region.modified.size())
            {
                region.modified[bitIndex] = false;
            }
        }
    }
}
