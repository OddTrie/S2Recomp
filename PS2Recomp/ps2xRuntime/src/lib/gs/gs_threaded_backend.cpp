#include "runtime/gs/gs_threaded_backend.h"
#include "runtime/gs/gs_gpu_backend.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "ThreadNaming.h"
#include "runtime/ps2_host_cpu.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <cstring>
#include <iterator>
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#include <immintrin.h>
#endif

namespace
{
    // The game can run far ahead of the rasterizer; past this many queued commands it waits.
    constexpr size_t kMaxPendingCommands = 32768;
}

std::unique_ptr<GSRasterBackend> ps2xMaybeRecordGsBackend(std::unique_ptr<GSRasterBackend> backend); // gs_record_replay.cpp

static std::unique_ptr<GSRasterBackend> makeRasterBackend();

std::unique_ptr<GSRasterBackend> ps2xMakeDefaultGsBackend()
{
    return ps2xMaybeRecordGsBackend(makeRasterBackend());
}

// Always the CPU renderer (the bit-exact reference). The game swaps in the GPU renderer at startup
// once its GL context works (PS2Runtime::initialize, selectGsRenderer in ps2_runtime.cpp).
static std::unique_ptr<GSRasterBackend> makeRasterBackend()
{
    const char *env = std::getenv("PS2X_GS_ASYNC");
    if (env && *env == '0')
        return std::make_unique<GSCpuBackend>();
    int bands = 0, baseBands = 0;
    if (const char *b = std::getenv("PS2X_GS_BANDS"))
    {
        bands = std::atoi(b);
        if (const char *e = std::getenv("PS2X_GS_BANDS_BASE")) // drawing bands while the GS keeps up
            baseBands = std::atoi(e);
    }
    else
    {
        // Heavy scenes (Sims 2 shower steam) are fill-rate bound and keep scaling past 8 bands:
        // shower.gsr 8 -> 16 bands 1.9 s -> 1.27 s. Leave room for the EE, VIF1, GS feed and IOP threads.
        // With 32+ hardware threads (i9-14900K: 8 P-cores x2 + 16 E-cores) 24 bands, 16 of them drawing
        // unless the GS falls behind: worst shower phase 12.5 -> 14.6 FPS with 24 vs 16 (20: no gain),
        // but 24 always drawing cost the VU1-bound neighbourhood pan 24 -> 19 FPS.
        const int threads = ps2xHostCpu().logical; // the process affinity mask counts, not the machine
        if (threads >= 32)
        {
            bands = std::min(24, threads * 3 / 4);
            baseBands = 16;
        }
        else
            // Half the logical processors (it was a quarter below 16). Simulated with `start /affinity`, shower
            // worst 10 s: 4 E-cores 2.6 -> 4.4 FPS (2 bands instead of 1), 2P/4T 5.0 -> 7.7 (2 vs 1), 4P/8T
            // 9.4 -> 12.7 (4 vs 2); menu/pan/lot up or equal. More than half cost the pan (8 bands on 4P/8T:
            // lows 20 -> 12). See the playbook, "Low-end PCs".
            bands = std::max(1, std::min(16, threads / 2));
    }
    if (bands > 1)
        return std::make_unique<GSBandedBackend>(std::min(bands, 32), baseBands);
    return std::make_unique<GSThreadedBackend>(std::make_unique<GSCpuBackend>());
}

GSThreadedBackend::GSThreadedBackend(std::unique_ptr<GSRasterBackend> inner)
    : m_inner(std::move(inner))
{
    m_pending.reserve(4096);
    m_worker = std::thread([this]() { workerLoop(); });
}

GSThreadedBackend::~GSThreadedBackend()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_workAvailable.notify_all();
    if (m_worker.joinable())
        m_worker.join();
}

GSThreadedBackend::Command &GSThreadedBackend::pushLocked(std::unique_lock<std::mutex> &lock, Op op)
{
    if (m_pending.size() >= kMaxPendingCommands)
        m_progress.wait(lock, [this]() { return m_pending.size() < kMaxPendingCommands || m_stop; });
    m_pending.emplace_back();
    Command &command = m_pending.back();
    command.op = op;
    ++m_enqueued;
    if (m_pending.size() == 1u)
        m_workAvailable.notify_one();
    return command;
}

void GSThreadedBackend::waitIdle() const
{
    std::unique_lock<std::mutex> lock(m_mutex);
    const uint64_t target = m_enqueued;
    m_progress.wait(lock, [this, target]() { return m_completed >= target || m_stop; });
}

void GSThreadedBackend::workerLoop()
{
    ThreadNaming::SetCurrentThreadName("GSRaster");
    std::vector<Command> working;
    std::vector<uint8_t> workingData;
    working.reserve(4096);
    for (;;)
    {
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_workAvailable.wait(lock, [this]() { return !m_pending.empty() || m_stop; });
            if (m_pending.empty() && m_stop)
                return;
            working.swap(m_pending);
            workingData.swap(m_pendingData);
        }
        m_progress.notify_all(); // room in the queue again

        for (const Command &command : working)
        {
            switch (command.op)
            {
            case Op::Submit:
                m_inner->Submit(command.batch);
                break;
            case Op::LoadClut:
                m_inner->LoadClut(command.tex0, command.texclut);
                break;
            case Op::BeginTransfer:
                m_inner->BeginTransfer(command.transfer);
                break;
            case Op::UploadImage:
                m_inner->UploadImage(workingData.data() + command.dataOffset, command.dataSize);
                break;
            case Op::TextureFlush:
                m_inner->TextureFlush();
                break;
            }
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_completed += working.size();
        }
        m_progress.notify_all();
        working.clear();
        workingData.clear();
    }
}

void GSThreadedBackend::Initialize(uint8_t *vram, uint32_t vramSize)
{
    waitIdle();
    m_inner->Initialize(vram, vramSize);
}

void GSThreadedBackend::Reset()
{
    waitIdle();
    m_inner->Reset();
}

void GSThreadedBackend::Submit(const GSPrimitiveBatch &batch)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    pushLocked(lock, Op::Submit).batch = batch;
}

void GSThreadedBackend::LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    Command &command = pushLocked(lock, Op::LoadClut);
    command.tex0 = tex0;
    command.texclut = texclut;
}

void GSThreadedBackend::BeginTransfer(const GSTransferCommand &transfer)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    pushLocked(lock, Op::BeginTransfer).transfer = transfer;
}

void GSThreadedBackend::UploadImage(const uint8_t *data, uint32_t sizeBytes)
{
    if (!data || sizeBytes == 0u)
        return;
    std::unique_lock<std::mutex> lock(m_mutex);
    Command &command = pushLocked(lock, Op::UploadImage);
    command.dataOffset = static_cast<uint32_t>(m_pendingData.size());
    command.dataSize = sizeBytes;
    m_pendingData.insert(m_pendingData.end(), data, data + sizeBytes);
}

void GSThreadedBackend::Flush()
{
}

void GSThreadedBackend::TextureFlush()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    pushLocked(lock, Op::TextureFlush);
}

void GSThreadedBackend::Sync(GSSyncReason reason)
{
    if (reason == GSSyncReason::Presentation)
        return; // Present() waits briefly itself; a full wait here stalls the window loop
    waitIdle();
    m_inner->Sync(reason);
}

PresentationFrame GSThreadedBackend::Present(const GSPresentationRequest &request)
{
    // Called by the host window loop, which also polls input. Waiting for the whole queue made the
    // window drop to a few FPS when the rasterizer is behind. The synchronous backend never waited
    // for drawing either (it showed VRAM as it was), so give the worker a moment and then present.
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        const uint64_t target = m_enqueued;
        m_progress.wait_for(lock, std::chrono::milliseconds(4), [this, target]() { return m_completed >= target || m_stop; });
    }
    return m_inner->Present(request);
}

bool GSThreadedBackend::ClearFramebuffer(const GSContext &context, uint32_t rgba)
{
    waitIdle();
    return m_inner->ClearFramebuffer(context, rgba);
}

uint32_t GSThreadedBackend::ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
    waitIdle();
    return m_inner->ConsumeLocalToHostBytes(dst, maxBytes);
}

uint32_t GSThreadedBackend::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    waitIdle();
    return m_inner->ReadVram(psm, base, bw, x, y);
}

void GSThreadedBackend::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    waitIdle();
    m_inner->WriteVram(psm, base, bw, x, y, value);
}

void GSThreadedBackend::SnapshotVram(std::vector<uint8_t> &out) const
{
    waitIdle();
    m_inner->SnapshotVram(out);
}

GSTransferSnapshot GSThreadedBackend::GetTransferSnapshot() const
{
    waitIdle();
    return m_inner->GetTransferSnapshot();
}

// =============================================================================================
// GSBandedBackend
// =============================================================================================
namespace
{
    constexpr uint32_t kGsPages = 512; // 4 MB of GS memory in 8 KB pages
    constexpr size_t kMaxBandedPending = 32768;
    // Commands handed to the band workers at once. Waking 8 threads per primitive cost more than
    // drawing it; a flush request (anyone waiting for results) or 1 ms ends a batch early.
    constexpr size_t kMinBandedBatch = 512;
    // Pause loops a worker spins at a barrier before it sleeps. Barriers come in bursts, and
    // waking sleeping threads at each one cost more than the drawing between them.
    constexpr int kSyncSpins = 4096;
    // PS2X_GS_BAND_SPIN=<n> overrides kSyncSpins (timing only).
    int bandSyncSpins()
    {
        static const int spins = [] {
            const char *e = std::getenv("PS2X_GS_BAND_SPIN");
            return e && *e ? std::max(0, std::atoi(e)) : kSyncSpins;
        }();
        return spins;
    }

    inline void cpuRelax()
    {
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
        _mm_pause();
#else
        std::this_thread::yield();
#endif
    }

    // Page geometry (pixels per 8 KB page) for a pixel storage mode.
    void pageSize(uint32_t psm, uint32_t &w, uint32_t &h)
    {
        switch (psm)
        {
        case GS_PSM_CT16: case GS_PSM_CT16S: case GS_PSM_Z16: case GS_PSM_Z16S:
            w = 64; h = 64; return;
        case GS_PSM_T8:
            w = 128; h = 64; return;
        case GS_PSM_T4:
            w = 128; h = 128; return;
        default: // CT32/CT24/Z32/Z24 and the 8H/4HL/4HH modes stored in 32-bit pages
            w = 64; h = 32; return;
        }
    }

    // Pages an image (width in 64-pixel units x rows) occupies from its first page.
    uint32_t imagePages(uint32_t psm, uint32_t width64, uint32_t rows)
    {
        uint32_t pw = 0, ph = 0;
        pageSize(psm, pw, ph);
        const uint32_t widthPx = std::max<uint32_t>(width64, 1u) * 64u;
        const uint32_t across = (widthPx + pw - 1u) / pw;
        const uint32_t down = (std::max<uint32_t>(rows, 1u) + ph - 1u) / ph;
        return std::min<uint32_t>(across * down, kGsPages);
    }

    // Pages a transfer rectangle can touch from its base (in 256-byte blocks, so +1 page when the
    // base is not page aligned).
    uint32_t transferPages(uint32_t psm, uint32_t width64, uint32_t y, uint32_t rows)
    {
        return std::min<uint32_t>(imagePages(psm, width64, y + rows) + 1u, kGsPages);
    }
}

GSBandedBackend::PageSet GSBandedBackend::pageRange(uint32_t firstPage, uint32_t pageCount)
{
    PageSet set{};
    if (pageCount >= kGsPages)
    {
        set.fill(~0ull);
        return set;
    }
    uint32_t page = firstPage % kGsPages;
    while (pageCount != 0u)
    {
        const uint32_t bit = page & 63u;
        const uint32_t n = std::min<uint32_t>(pageCount, 64u - bit);
        set[page >> 6] |= (n == 64u ? ~0ull : ((1ull << n) - 1ull)) << bit;
        pageCount -= n;
        page = (page + n) % kGsPages;
    }
    return set;
}

void GSBandedBackend::markPages(PageSet &set, const PageSet &pages)
{
    for (size_t w = 0; w < pages.size(); ++w)
        set[w] |= pages[w];
}

bool GSBandedBackend::overlaps(const PageSet &a, const PageSet &b)
{
    uint64_t any = 0;
    for (size_t w = 0; w < a.size(); ++w)
        any |= a[w] & b[w];
    return any != 0u;
}

void GSBandedBackend::markLocked(Mark kind, const PageSet &pages, uint64_t seq)
{
    // A new segment every 64 commands: a hazard then waits for at most 63 commands after the one
    // it really depends on, and the scan below stays short.
    constexpr uint64_t kSegmentCommands = 64;
    if (m_segments.empty() || seq >= m_segments.back().firstSeq + kSegmentCommands)
    {
        m_segments.emplace_back();
        m_segments.back().firstSeq = seq;
    }
    Segment &segment = m_segments.back();
    segment.lastSeq = std::max(segment.lastSeq, seq);
    switch (kind)
    {
    case MarkDrawn: markPages(m_drawn, pages); markPages(segment.drawn, pages); break;
    case MarkUploaded: markPages(m_uploaded, pages); markPages(segment.uploaded, pages); break;
    case MarkRead: markPages(m_read, pages); markPages(segment.read, pages); break;
    }
}

bool GSBandedBackend::hazardLocked(bool drawn, bool uploaded, bool read, const PageSet &pages, uint64_t &seq) const
{
    // Cheap test against the union first; the segment scan runs only for real hazards.
    if (!(drawn && overlaps(m_drawn, pages)) && !(uploaded && overlaps(m_uploaded, pages)) && !(read && overlaps(m_read, pages)))
        return false;
    for (size_t i = m_segments.size(); i-- > 0;)
    {
        const Segment &segment = m_segments[i];
        if ((drawn && overlaps(segment.drawn, pages)) || (uploaded && overlaps(segment.uploaded, pages)) ||
            (read && overlaps(segment.read, pages)))
        {
            seq = m_fullBarriers ? m_enqueued - 1u : segment.lastSeq;
            return true;
        }
    }
    return false;
}

GSBandedBackend::GSBandedBackend(int bands, int baseBands)
    : m_bands(std::max(2, bands))
{
    m_baseBands = baseBands > 1 ? std::min(baseBands, m_bands) : m_bands;
    m_activeBands = m_baseBands;
    if (const char *e = std::getenv("PS2X_GS_BANDS_FLIP"))
        m_flipBands = *e && *e != '0';
    for (int i = 0; i < m_bands; ++i)
    {
        m_instances.push_back(std::make_unique<GSCpuBackend>());
        m_instances.back()->SetBand(i, m_activeBands);
    }
    if (const char *e = std::getenv("PS2X_GS_BARRIER_STATS"))
        m_stats = *e && *e != '0';
    if (const char *e = std::getenv("PS2X_GS_FULL_BARRIERS"))
        m_fullBarriers = *e && *e != '0';
    m_bandProgress = std::make_unique<BandProgress[]>(static_cast<size_t>(m_bands));
    for (int i = 0; i < m_bands; ++i)
    {
        m_bandProgress[i].want = std::make_unique<std::atomic<uint64_t>[]>(static_cast<size_t>(m_bands));
        for (int w = 0; w < m_bands; ++w)
            m_bandProgress[i].want[w].store(kNoWait, std::memory_order_relaxed);
    }
    m_segments.reserve(kMaxBandedPending / 64u + 2u);
    m_statT0 = std::chrono::steady_clock::now();
    m_pending.reserve(4096);
    for (int i = 0; i < m_bands; ++i)
        m_workers.emplace_back([this, i]() { workerLoop(i); });
    if (m_baseBands < m_bands)
        std::fprintf(stderr, "[gs] banded rasterizer: %d threads (%d draw unless the GS queue is full)\n", m_bands, m_baseBands);
    else
        std::fprintf(stderr, "[gs] banded rasterizer: %d threads\n", m_bands);
}

GSBandedBackend::~GSBandedBackend()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_workAvailable.notify_all();
    m_progress.notify_all();
    for (std::thread &t : m_workers)
        if (t.joinable())
            t.join();
}

void GSBandedBackend::waitForBands(int index, uint64_t seq)
{
    // Every band runs the stream in order, so a band past `seq` has finished it (and everything
    // before it). This band itself is already past it.
    auto done = [&](int band) { return m_bandProgress[band].next.load(std::memory_order_seq_cst) > seq; };
    for (int band = 0; band < m_activeBands; ++band) // the others sleep and draw nothing
    {
        if (band == index || done(band))
            continue;
        int spins = 0;
        const int maxSpins = bandSyncSpins();
        while (!done(band) && spins < maxSpins)
        {
            cpuRelax();
            ++spins;
        }
        if (done(band))
            continue;
        // Sleep until that band is past `seq`. The slot and the waiter count are set before the
        // re-check, and the band publishes its progress before reading the count (all seq_cst):
        // either this re-check sees the progress or the band sees the slot and clears it.
        BandProgress &other = m_bandProgress[band];
        std::atomic<uint64_t> &slot = other.want[index];
        slot.store(seq, std::memory_order_seq_cst);
        other.waiters.fetch_add(1, std::memory_order_seq_cst);
        while (!done(band))
            slot.wait(seq, std::memory_order_seq_cst);
        other.waiters.fetch_sub(1, std::memory_order_seq_cst);
        slot.store(kNoWait, std::memory_order_relaxed);
    }
}

void GSBandedBackend::publishProgress(int index, uint64_t next)
{
    BandProgress &self = m_bandProgress[index];
    self.next.store(next, std::memory_order_seq_cst);
    if (self.waiters.load(std::memory_order_seq_cst) == 0)
        return;
    for (int w = 0; w < m_bands; ++w)
    {
        std::atomic<uint64_t> &slot = self.want[w];
        if (slot.load(std::memory_order_seq_cst) < next)
        {
            slot.store(kNoWait, std::memory_order_seq_cst);
            slot.notify_one();
        }
    }
}

void GSBandedBackend::statsLocked()
{
    const auto now = std::chrono::steady_clock::now();
    if (now - m_statT0 < std::chrono::seconds(1))
        return;
    uint64_t total = 0;
    for (uint64_t n : m_statBarriers)
        total += n;
    std::fprintf(stderr, "[gs-bands] %llu cmds, %llu batches, %llu barriers (texture %llu, clut %llu, draw-target %llu, "
                         "upload %llu, transfer-src %llu), %llu drains, %llu batches with all bands\n",
                 (unsigned long long)m_statCommands, (unsigned long long)m_statBatches, (unsigned long long)total,
                 (unsigned long long)m_statBarriers[CauseTexture], (unsigned long long)m_statBarriers[CauseClut],
                 (unsigned long long)m_statBarriers[CauseDrawTarget], (unsigned long long)m_statBarriers[CauseUpload],
                 (unsigned long long)m_statBarriers[CauseTransferSrc], (unsigned long long)m_statDrains,
                 (unsigned long long)m_statAllBandsBatches);
    m_statAllBandsBatches = 0;
    m_statT0 = now;
    m_statCommands = m_statBatches = m_statDrains = 0;
    std::fill(std::begin(m_statBarriers), std::end(m_statBarriers), 0ull);
}

GSBandedBackend::Command &GSBandedBackend::pushLocked(std::unique_lock<std::mutex> &lock, Op op)
{
    if (m_pending.size() >= kMaxBandedPending)
    {
        m_queueWasFull = true;
        m_progress.wait(lock, [this]() { return m_pending.size() < kMaxBandedPending || m_stop; });
    }
    m_pending.emplace_back();
    Command &command = m_pending.back();
    command.op = op;
    ++m_enqueued;
    if (m_stats && (++m_statCommands & 1023u) == 0u)
        statsLocked();
    if (m_pending.size() == kMinBandedBatch)
        m_workAvailable.notify_all();
    return command;
}

void GSBandedBackend::pushBarrierLocked(std::unique_lock<std::mutex> &lock, Cause cause, uint64_t waitSeq)
{
    // An earlier queued Barrier waiting for this command or a later one already covers it.
    if (m_hasWait && waitSeq <= m_lastWaitSeq)
        return;
    pushLocked(lock, Op::Barrier).waitSeq = waitSeq;
    m_hasWait = true;
    m_lastWaitSeq = waitSeq;
    ++m_statBarriers[cause];
    // Every command after the Barrier runs after all bands finished waitSeq: segments up to it
    // can no longer conflict. pushLocked may have waited for queue room, i.e. for a batch swap
    // that cleared the segments; then there is nothing to drop.
    size_t keep = 0;
    while (keep < m_segments.size() && m_segments[keep].lastSeq <= waitSeq)
        ++keep;
    if (keep == 0u)
        return;
    m_segments.erase(m_segments.begin(), m_segments.begin() + static_cast<std::ptrdiff_t>(keep));
    m_drawn.fill(0u);
    m_uploaded.fill(0u);
    m_read.fill(0u);
    for (const Segment &segment : m_segments)
    {
        markPages(m_drawn, segment.drawn);
        markPages(m_uploaded, segment.uploaded);
        markPages(m_read, segment.read);
    }
}

void GSBandedBackend::waitIdle() const
{
    std::unique_lock<std::mutex> lock(m_mutex);
    const uint64_t target = m_enqueued;
    if (m_completed >= target)
        return;
    ++m_statDrains;
    m_flushRequested = true;
    m_workAvailable.notify_all();
    m_progress.wait(lock, [this, target]() { return m_completed >= target || m_stop; });
}

void GSBandedBackend::workerLoop(int index)
{
    ThreadNaming::SetCurrentThreadName("GSBand" + std::to_string(index));
    GSCpuBackend &self = *m_instances[static_cast<size_t>(index)];
    uint64_t seen = 0;
    for (;;)
    {
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            if (index == 0)
            {
                m_workAvailable.wait(lock, [this]() { return !m_pending.empty() || m_stop; });
                if (m_pending.empty())
                {
                    m_workAvailable.notify_all(); // stopping: wake the others
                    return;
                }
                m_workAvailable.wait_for(lock, std::chrono::milliseconds(1), [this]()
                                         { return m_pending.size() >= kMinBandedBatch || m_flushRequested || m_stop; });
                m_flushRequested = false;
                m_batch.swap(m_pending);
                m_batchData.swap(m_pendingData);
                m_pending.clear();
                m_pendingData.clear();
                m_batchFirstSeq = m_enqueued - m_batch.size();
                // What was pending is now in flight, and it is complete before the next batch
                // starts: commands queued from now on never wait for it.
                m_drawn.fill(0u);
                m_uploaded.fill(0u);
                m_read.fill(0u);
                m_segments.clear();
                m_hasWait = false;
                ++m_statBatches;
                m_workersDone = 0;
                ++m_batchGeneration;
                chooseActiveBandsLocked();
                if (m_activeBands == m_bands)
                    ++m_statAllBandsBatches;
                m_workAvailable.notify_all();
                m_progress.notify_all(); // room in the queue again
            }
            else
            {
                // Bands beyond m_activeBands sleep through batches (no wake-up per batch).
                auto runs = [this, &seen, index]() { return m_batchGeneration != seen && index < m_activeBands; };
                m_workAvailable.wait(lock, [&]() { return runs() || m_stop; });
                if (!runs())
                    return; // stopping, no new batch
            }
            seen = m_batchGeneration;
        }

        const bool lag = m_skew && static_cast<int>(seen % static_cast<uint64_t>(m_bands)) == index;
        const std::vector<Command> &batch = m_batch;
        const uint64_t firstSeq = m_batchFirstSeq; // set before the generation this band saw
        for (size_t k = 0; k < batch.size(); ++k)
        {
            if (k != 0u)
            {
                // Command k - 1 is done.
                publishProgress(index, firstSeq + k);
            }
            const Command &command = batch[k];
            switch (command.op)
            {
            case Op::Submit:
                if (lag)
                    for (int i = 0; i < 256; ++i)
                        cpuRelax();
                self.Submit(command.batch);
                break;
            case Op::LoadClut:
                self.LoadClut(command.tex0, command.texclut);
                break;
            case Op::TextureFlush:
                self.TextureFlush();
                break;
            case Op::BeginTransfer:
                if (index == 0)
                    self.BeginTransfer(command.transfer);
                else
                    self.NoteSkippedTransfer(command.transfer); // drop decoded textures it overwrites
                break;
            case Op::UploadImage:
                if (index == 0)
                    self.UploadImage(m_batchData.data() + command.dataOffset, command.dataSize);
                else
                    self.NoteSkippedUpload();
                break;
            case Op::Barrier:
                waitForBands(index, command.waitSeq);
                break;
            }
        }
        // The whole batch is done (the next batch only starts once every band got here).
        publishProgress(index, firstSeq + batch.size());

        {
            std::unique_lock<std::mutex> lock(m_mutex);
            if (++m_workersDone == m_activeBands)
            {
                m_completed += m_batch.size();
                m_progress.notify_all();
            }
            if (index == 0)
            {
                // Don't hand out the next batch until every worker is done with this one.
                m_progress.wait(lock, [this]() { return m_workersDone == m_activeBands || m_stop; });
                m_batch.clear();
                m_batchData.clear();
            }
        }
    }
}

void GSBandedBackend::chooseActiveBandsLocked()
{
    int want = m_activeBands;
    if (m_flipBands)
        want = (m_batchGeneration & 1u) ? m_bands : m_baseBands;
    else if (m_queueWasFull)
    {
        want = m_bands;
        m_allBandsUntil = m_batchGeneration + 16u; // hysteresis: a few calm batches before dropping
    }
    else if (m_batchGeneration >= m_allBandsUntil)
        want = m_baseBands;
    m_queueWasFull = false;
    if (want == m_activeBands)
        return;
    // Every band is idle here (worker 0 hands out a batch only once all finished the last one).
    for (int i = 0; i < want; ++i)
    {
        GSCpuBackend &instance = *m_instances[static_cast<size_t>(i)];
        if (i >= m_activeBands)
        {
            // It slept through batches: take instance 0's CLUT state (the only per-instance state
            // the skipped commands change), drop decoded textures their draws may have overwritten,
            // and resume its progress at this batch.
            instance.CopyClutStateFrom(*m_instances[0]);
            instance.InvalidateDecodedTextures();
            m_bandProgress[i].next.store(m_batchFirstSeq, std::memory_order_seq_cst);
        }
        instance.SetBand(i, want);
    }
    m_activeBands = want;
}

void GSBandedBackend::Initialize(uint8_t *vram, uint32_t vramSize)
{
    waitIdle();
    for (auto &instance : m_instances)
        instance->Initialize(vram, vramSize);
}

void GSBandedBackend::Reset()
{
    waitIdle();
    for (auto &instance : m_instances)
        instance->Reset();
}

void GSBandedBackend::Submit(const GSPrimitiveBatch &batch)
{
    // Page sets are built before taking the lock; the EE thread pays for this per primitive.
    const GSContext &ctx = batch.state.context;
    const bool textured = batch.state.prim.tme;
    const uint32_t rows = static_cast<uint32_t>(ctx.scissor.y1) + 1u;
    // The CPU rasterizer samples mip level 0 only (TEX0); the MIPTBP levels are never read.
    PageSet texture{};
    if (textured)
        texture = pageRange(ctx.tex0.tbp0 / 32u, imagePages(ctx.tex0.psm, ctx.tex0.tbw, 1u << std::min<uint32_t>(ctx.tex0.th, 11u)) + 1u);
    PageSet target{};
    if (ctx.frame.fbmsk != 0xFFFFFFFFu)
        target = pageRange(ctx.frame.fbp, imagePages(ctx.frame.psm, ctx.frame.fbw, rows));
    if (!ctx.zbuf.zmask)
        markPages(target, pageRange(ctx.zbuf.zbp, imagePages(ctx.zbuf.psm, ctx.frame.fbw, rows)));

    std::unique_lock<std::mutex> lock(m_mutex);
    uint64_t waitSeq = 0;
    if (textured && hazardLocked(true, true, false, texture, waitSeq))
        pushBarrierLocked(lock, CauseTexture, waitSeq);
    // Drawing into pages another band may still read, or that instance 0 may still upload into.
    // Bands own disjoint rows, so draw after draw needs nothing.
    if (hazardLocked(false, true, true, target, waitSeq))
        pushBarrierLocked(lock, CauseDrawTarget, waitSeq);
    pushLocked(lock, Op::Submit).batch = batch;
    const uint64_t seq = m_enqueued - 1u;
    if (textured)
        markLocked(MarkRead, texture, seq);
    markLocked(MarkDrawn, target, seq);
}

void GSBandedBackend::LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    PageSet clut{};
    if (tex0.cld != 0u)
    {
        clut = pageRange(tex0.cbp / 32u, 2u);
        uint64_t waitSeq = 0;
        if (hazardLocked(true, true, false, clut, waitSeq))
            pushBarrierLocked(lock, CauseClut, waitSeq);
    }
    Command &command = pushLocked(lock, Op::LoadClut);
    command.tex0 = tex0;
    command.texclut = texclut;
    if (tex0.cld != 0u)
        markLocked(MarkRead, clut, m_enqueued - 1u);
}

void GSBandedBackend::BeginTransfer(const GSTransferCommand &transfer)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    const GSBitBltBuf &b = transfer.bitbltbuf;
    const uint32_t rrh = transfer.trxreg.rrh;
    const bool readsVram = transfer.direction == 1u || transfer.direction == 2u;
    const bool writesVram = transfer.direction == 0u || transfer.direction == 2u;
    const PageSet src = pageRange(b.sbp / 32u, transferPages(b.spsm, b.sbw, transfer.trxpos.ssay, rrh));
    const PageSet dst = pageRange(b.dbp / 32u, transferPages(b.dpsm, b.dbw, transfer.trxpos.dsay, rrh));
    // Instance 0 runs every transfer in stream order, so its own earlier uploads need no barrier.
    uint64_t waitSeq = 0;
    if (readsVram && hazardLocked(true, false, false, src, waitSeq))
        pushBarrierLocked(lock, CauseTransferSrc, waitSeq);
    if (writesVram && hazardLocked(true, false, true, dst, waitSeq))
        pushBarrierLocked(lock, CauseUpload, waitSeq);
    pushLocked(lock, Op::BeginTransfer).transfer = transfer;
    const uint64_t seq = m_enqueued - 1u;
    if (readsVram)
        markLocked(MarkRead, src, seq);
    if (writesVram)
        markLocked(MarkUploaded, dst, seq);
    // The data of a host->local transfer comes in UploadImage commands; they write these pages.
    m_uploadPages = dst;
    m_uploadActive = transfer.direction == 0u;
}

void GSBandedBackend::UploadImage(const uint8_t *data, uint32_t sizeBytes)
{
    if (!data || sizeBytes == 0u)
        return;
    std::unique_lock<std::mutex> lock(m_mutex);
    Command &command = pushLocked(lock, Op::UploadImage);
    command.dataOffset = static_cast<uint32_t>(m_pendingData.size());
    command.dataSize = sizeBytes;
    m_pendingData.insert(m_pendingData.end(), data, data + sizeBytes);
    // Readers must wait for this chunk, not only for its BeginTransfer.
    markLocked(MarkUploaded, m_uploadActive ? m_uploadPages : allPages(), m_enqueued - 1u);
}

void GSBandedBackend::Flush()
{
}

void GSBandedBackend::TextureFlush()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    pushLocked(lock, Op::TextureFlush);
}

void GSBandedBackend::Sync(GSSyncReason reason)
{
    if (reason == GSSyncReason::Presentation)
        return; // Present() waits briefly itself
    waitIdle();
}

PresentationFrame GSBandedBackend::Present(const GSPresentationRequest &request)
{
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        const uint64_t target = m_enqueued;
        if (m_completed < target)
        {
            m_flushRequested = true;
            m_workAvailable.notify_all();
            m_progress.wait_for(lock, std::chrono::milliseconds(4), [this, target]() { return m_completed >= target || m_stop; });
        }
    }
    return m_instances[0]->Present(request);
}

bool GSBandedBackend::ClearFramebuffer(const GSContext &context, uint32_t rgba)
{
    waitIdle();
    for (size_t i = 1; i < m_instances.size(); ++i)
        m_instances[i]->InvalidateDecodedTextures();
    return m_instances[0]->ClearFramebuffer(context, rgba);
}

uint32_t GSBandedBackend::ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
    waitIdle();
    return m_instances[0]->ConsumeLocalToHostBytes(dst, maxBytes);
}

uint32_t GSBandedBackend::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    waitIdle();
    return m_instances[0]->ReadVram(psm, base, bw, x, y);
}

void GSBandedBackend::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    waitIdle();
    for (size_t i = 1; i < m_instances.size(); ++i)
        m_instances[i]->InvalidateDecodedTextures();
    m_instances[0]->WriteVram(psm, base, bw, x, y, value);
}

void GSBandedBackend::SnapshotVram(std::vector<uint8_t> &out) const
{
    waitIdle();
    m_instances[0]->SnapshotVram(out);
}

GSTransferSnapshot GSBandedBackend::GetTransferSnapshot() const
{
    waitIdle();
    return m_instances[0]->GetTransferSnapshot();
}
