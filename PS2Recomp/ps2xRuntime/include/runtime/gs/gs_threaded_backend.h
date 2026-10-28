#pragma once

#include "runtime/gs/gs_backend.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// Runs another raster backend on its own thread. Commands that only change GS memory (draws,
// CLUT loads, transfers, image uploads, texture flushes) are queued in order and return at once;
// anything that reads GS memory or needs a result first waits until every queued command is done.
// The game (EE) thread then no longer pays for rasterizing. PS2X_GS_ASYNC=0 turns it off.
class GSThreadedBackend final : public GSRasterBackend
{
public:
    explicit GSThreadedBackend(std::unique_ptr<GSRasterBackend> inner);
    ~GSThreadedBackend() override;

    void Initialize(uint8_t *vram, uint32_t vramSize) override;
    void Reset() override;

    void Submit(const GSPrimitiveBatch &batch) override;
    void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) override;
    void BeginTransfer(const GSTransferCommand &command) override;
    void UploadImage(const uint8_t *data, uint32_t sizeBytes) override;

    void Flush() override;
    void TextureFlush() override;
    void Sync(GSSyncReason reason) override;
    PresentationFrame Present(const GSPresentationRequest &request) override;

    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override;
    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override;

    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override;
    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override;
    void SnapshotVram(std::vector<uint8_t> &out) const override;
    GSTransferSnapshot GetTransferSnapshot() const override;

private:
    enum class Op : uint8_t
    {
        Submit,
        LoadClut,
        BeginTransfer,
        UploadImage,
        TextureFlush,
    };

    struct Command
    {
        Op op = Op::Submit;
        uint32_t dataOffset = 0; // UploadImage: bytes in the data buffer of the same batch
        uint32_t dataSize = 0;
        GSPrimitiveBatch batch{};
        GSTex0Reg tex0{};
        GSTexClutReg texclut{};
        GSTransferCommand transfer{};
    };

    Command &pushLocked(std::unique_lock<std::mutex> &lock, Op op);
    void waitIdle() const;
    void workerLoop();

    std::unique_ptr<GSRasterBackend> m_inner;
    mutable std::mutex m_mutex;
    mutable std::condition_variable m_workAvailable;
    mutable std::condition_variable m_progress;
    std::vector<Command> m_pending;
    std::vector<uint8_t> m_pendingData;
    uint64_t m_enqueued = 0;
    uint64_t m_completed = 0;
    bool m_stop = false;
    std::thread m_worker;
};

class GSCpuBackend;

// Multi-core rasterization: N CPU rasterizer instances share VRAM; each runs the same ordered
// command stream but draws only its own interleaved bands of framebuffer rows, so no two write the
// same pixel and blending/Z stay exact. Transfers and uploads (which write VRAM outside that row
// ownership) run on instance 0 only, in stream order. Everything since the last barrier is tracked
// per 8 KB page (drawn into, uploaded into, read as texture/CLUT/transfer source); a command that
// would touch a page in a conflicting way (read after draw or upload, draw or upload after a read,
// draw after upload) gets a barrier first: each band waits there until every band has finished the
// newest command it conflicts with (not everything before it). PS2X_GS_BANDS=<n>.
class GSBandedBackend final : public GSRasterBackend
{
public:
    // bands = rasterizer threads. baseBands (0 = bands) = how many of them draw while the GS keeps up;
    // all of them draw while the command queue is full (see m_activeBands).
    explicit GSBandedBackend(int bands, int baseBands = 0);
    ~GSBandedBackend() override;

    void Initialize(uint8_t *vram, uint32_t vramSize) override;
    void Reset() override;

    void Submit(const GSPrimitiveBatch &batch) override;
    void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) override;
    void BeginTransfer(const GSTransferCommand &command) override;
    void UploadImage(const uint8_t *data, uint32_t sizeBytes) override;

    void Flush() override;
    void TextureFlush() override;
    void Sync(GSSyncReason reason) override;
    PresentationFrame Present(const GSPresentationRequest &request) override;

    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override;
    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override;

    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override;
    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override;
    void SnapshotVram(std::vector<uint8_t> &out) const override;
    GSTransferSnapshot GetTransferSnapshot() const override;

    // Testing (--gs-replay-check): in each batch one band (rotating) stalls before every draw, so
    // the others run far ahead of it and a missing hazard barrier shows up as wrong pixels.
    // Set before the first command.
    void SetBandSkewForTesting(bool on) { m_skew = on; }

private:
    enum class Op : uint8_t
    {
        Submit,
        LoadClut,
        TextureFlush,
        BeginTransfer, // instance 0 only
        UploadImage,   // instance 0 only
        Barrier,       // page hazard: wait until every band has finished command waitSeq
    };

    // Why a barrier was inserted (PS2X_GS_BARRIER_STATS=1 prints counts per second).
    enum Cause : uint8_t
    {
        CauseTexture,    // texture read from pages drawn/uploaded since the last barrier
        CauseClut,       // CLUT read from such pages
        CauseDrawTarget, // draw into pages read or uploaded since the last barrier
        CauseUpload,     // upload/transfer into pages drawn or read since the last barrier
        CauseTransferSrc,// local->local/host transfer reading pages drawn since the last barrier
        CauseCount,
    };

    struct Command
    {
        Op op = Op::Submit;
        uint32_t dataOffset = 0;
        uint32_t dataSize = 0;
        uint64_t waitSeq = 0; // Barrier: sequence number of the command every band must have finished
        GSPrimitiveBatch batch{};
        GSTex0Reg tex0{};
        GSTexClutReg texclut{};
        GSTransferCommand transfer{};
    };

    using PageSet = std::array<uint64_t, 8>; // 512 GS pages
    static PageSet pageRange(uint32_t firstPage, uint32_t pageCount); // wraps at 4 MB
    static void markPages(PageSet &set, const PageSet &pages);
    static bool overlaps(const PageSet &a, const PageSet &b);
    // Pages of the queued batch (m_pending), in runs of consecutive commands. A hazard waits for
    // the newest run that conflicts, not for everything queued before it.
    struct Segment
    {
        uint64_t firstSeq = 0, lastSeq = 0;
        PageSet drawn{}, uploaded{}, read{};
    };
    enum Mark : uint8_t { MarkDrawn, MarkUploaded, MarkRead };
    void markLocked(Mark kind, const PageSet &pages, uint64_t seq);
    // Sequence number of the newest queued command that touched `pages` in one of the given ways,
    // or false when none did.
    bool hazardLocked(bool drawn, bool uploaded, bool read, const PageSet &pages, uint64_t &seq) const;

    Command &pushLocked(std::unique_lock<std::mutex> &lock, Op op);
    void pushBarrierLocked(std::unique_lock<std::mutex> &lock, Cause cause, uint64_t waitSeq);
    void statsLocked();
    void waitIdle() const;
    void workerLoop(int index);
    void waitForBands(int index, uint64_t seq);

    int m_bands;
    std::vector<std::unique_ptr<GSCpuBackend>> m_instances;
    mutable std::mutex m_mutex;
    mutable std::condition_variable m_workAvailable;
    mutable std::condition_variable m_progress;
    std::vector<Command> m_pending;
    std::vector<uint8_t> m_pendingData;
    std::vector<Command> m_batch; // being processed by all workers
    std::vector<uint8_t> m_batchData;
    uint64_t m_batchGeneration = 0;
    int m_workersDone = 0;
    uint64_t m_enqueued = 0;
    uint64_t m_completed = 0;
    bool m_stop = false;
    mutable bool m_flushRequested = false; // someone waits for results: start the batch now
    uint64_t m_batchFirstSeq = 0;           // sequence number of m_batch[0]
    // Per band: sequence number of the next command it will run (all before it are done). A band
    // that waits for it (after spinning) puts the command it needs in want[waiter] and sleeps on
    // that slot; this band clears the slot to kNoWait and wakes it once it is past that command.
    static constexpr uint64_t kNoWait = ~0ull;
    struct alignas(64) BandProgress
    {
        std::atomic<uint64_t> next{0};
        std::atomic<int> waiters{0};
        std::unique_ptr<std::atomic<uint64_t>[]> want; // one slot per band
    };
    std::unique_ptr<BandProgress[]> m_bandProgress;
    void publishProgress(int index, uint64_t next);
    // Hazard tracking for m_pending only: the batch the workers are processing is complete before
    // the next one starts, so nothing queued has to wait for it. Union of m_segments since the
    // newest queued Barrier; segments a Barrier covers are dropped.
    PageSet m_drawn{};
    PageSet m_uploaded{};
    PageSet m_read{};
    std::vector<Segment> m_segments;
    bool m_hasWait = false;     // m_pending holds a Barrier ...
    uint64_t m_lastWaitSeq = 0; // ... waiting for this command (the newest one)
    bool m_fullBarriers = false; // PS2X_GS_FULL_BARRIERS=1: every hazard waits for all queued commands
    PageSet m_uploadPages{};     // destination of the current host->local transfer
    bool m_uploadActive = false; // (an UploadImage without one is marked as writing everything)
    static PageSet allPages() { PageSet set; set.fill(~0ull); return set; }
    bool m_skew = false; // SetBandSkewForTesting
    // Adaptive band count. Only the first m_activeBands instances run batches (rows split among
    // them); the others sleep, are not waited for, and on waking take instance 0's CLUT state.
    // Chosen per batch by worker 0 while every band is idle, so row ownership never changes inside
    // a batch: all bands when the producer found the queue full since the last batch (GS-bound,
    // e.g. shower steam), m_baseBands otherwise (more threads only cost the VU1 worker CPU there,
    // even idle ones woken per batch). PS2X_GS_BANDS_FLIP=1 alternates every batch (test).
    int m_baseBands = 0;
    int m_activeBands = 0;
    bool m_queueWasFull = false;
    uint64_t m_allBandsUntil = 0; // batch generation until which all bands stay on
    bool m_flipBands = false;
    void chooseActiveBandsLocked();
    // Barrier statistics (PS2X_GS_BARRIER_STATS=1).
    bool m_stats = false;
    uint64_t m_statBarriers[CauseCount]{};
    uint64_t m_statCommands = 0;
    uint64_t m_statBatches = 0;
    uint64_t m_statAllBandsBatches = 0;
    mutable uint64_t m_statDrains = 0;
    std::chrono::steady_clock::time_point m_statT0{};
    std::vector<std::thread> m_workers;
};

// The backend GS uses by default: the CPU rasterizer on its own thread(s). PS2X_GS_ASYNC=0 runs it
// on the caller's thread; PS2X_GS_BANDS=<n> sets the number of banded rasterizer threads
// (default min(8, cores/4); 1 = a single rasterizer thread).
std::unique_ptr<GSRasterBackend> ps2xMakeDefaultGsBackend();
