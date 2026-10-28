#include "runtime/ps2_spu2_audio.h"
#include "runtime/ps2_netplay.h"

#include "ps2x/iop/spu2_audio.h"

#if !defined(PLATFORM_VITA)
#include "raylib.h"
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <chrono>
#include <thread>
#include <vector>

extern std::atomic<int64_t> g_ps2xHostRefreshNs; // EeScheduler.cpp: refresh the VBlank clock locks to (0 = none)

namespace ps2x_audio
{
    namespace
    {
        constexpr uint32_t kRate = 48000u;
        constexpr uint32_t kRingFrames = 1u << 15u; // ~680 ms

        // Single producer (the IOP / EE thread, emulated time) -> single consumer (the audio device
        // thread, real time). Frames are packed L | R << 16.
        struct Ring
        {
            std::vector<uint32_t> data = std::vector<uint32_t>(kRingFrames);
            std::atomic<uint64_t> write{0};
            std::atomic<uint64_t> read{0};
        };

#if !defined(PLATFORM_VITA)
        inline int32_t left(uint32_t f) { return static_cast<int16_t>(f & 0xFFFFu); }
        inline int32_t right(uint32_t f) { return static_cast<int16_t>(f >> 16u); }
        inline uint32_t pack(int32_t l, int32_t r)
        {
            l = std::clamp(l, -32768, 32767);
            r = std::clamp(r, -32768, 32767);
            return static_cast<uint16_t>(l) | (static_cast<uint32_t>(static_cast<uint16_t>(r)) << 16u);
        }

        // Real-time reader with WSOLA time stretching. Emulated time can run slower than real time
        // (heavy scenes), so the ring would run dry; instead of gaps, the reader then plays the input
        // slower without changing pitch: it outputs kHop frames per step, takes the next grain from
        // around `nominal + kHop * ratio` (best-matching offset within +-kSearch), and crossfades it
        // against the continuation of the previous grain. ratio follows the buffer fill (1.0 at or
        // above the target: then every grain is the exact continuation and the output is the input).
        struct Stretcher
        {
            static constexpr uint32_t kHop = 480u;    // 10 ms
            static constexpr int32_t kSearch = 240;   // +-5 ms
            bool running = false;
            uint64_t tail = 0;      // input position continuing the previous grain (faded out next)
            double nominal = 0.0;   // nominal start of the previous grain
            double ratio = 1.0;     // input frames consumed per output frame
            uint32_t out[kHop];
            uint32_t outPos = kHop; // frames of out[] already played
            uint32_t lastFrame = 0;

            uint32_t at(const Ring &ring, uint64_t pos) const { return ring.data[pos & (kRingFrames - 1u)]; }

            int64_t bestOffset(const Ring &ring, uint64_t base) const
            {
                // Normalized cross-correlation (both channels, every 2nd frame) of the candidate against
                // the tail. Not L+R: anti-phase content would cancel out.
                const auto score = [&](int64_t off) {
                    double dot = 0.0, energy = 1.0;
                    for (uint32_t i = 0; i < kHop; i += 2u)
                    {
                        const uint32_t a = at(ring, tail + i), b = at(ring, static_cast<uint64_t>(static_cast<int64_t>(base) + off) + i);
                        const double bl = left(b), br = right(b);
                        dot += left(a) * bl + right(a) * br;
                        energy += bl * bl + br * br;
                    }
                    return dot / std::sqrt(energy);
                };
                // The exact continuation of the tail (if in range) always wins on an exact match.
                const int64_t natural = static_cast<int64_t>(tail) - static_cast<int64_t>(base);
                int64_t best = 0;
                double bestScore = score(0);
                if (natural >= -kSearch && natural <= kSearch)
                    if (const double sc = score(natural); sc >= bestScore)
                    {
                        bestScore = sc;
                        best = natural;
                    }
                for (int64_t off = -kSearch; off <= kSearch; off += 8)
                    if (const double sc = score(off); sc > bestScore)
                    {
                        bestScore = sc;
                        best = off;
                    }
                const int64_t coarse = best;
                for (int64_t off = coarse - 7; off <= coarse + 7; ++off)
                    if (off >= -kSearch && off <= kSearch && off != coarse)
                        if (const double sc = score(off); sc > bestScore)
                        {
                            bestScore = sc;
                            best = off;
                        }
                return best;
            }

            // Produce the next kHop frames into out[]. Returns false when the ring has too little data.
            bool step(Ring &ring, uint64_t write, uint32_t target)
            {
                const uint64_t fill = write - tail;
                // Fill control: play at exactly 1.0 between half and twice the target (stretching is audible,
                // and online play dips into the buffer in bursts); slow down only when it runs low.
                double want = 1.0;
                if (fill < target / 2u)
                    want = std::max(0.5, 0.5 + static_cast<double>(fill) / target);
                else if (fill > 2u * target)
                    want = 1.05;
                ratio += (want - ratio) * 0.1;
                if (want == 1.0 && std::abs(ratio - 1.0) < 0.002)
                    ratio = 1.0;

                if (fill > 6u * target)
                {
                    // Far behind (a stall in real time): jump close to the newest data with a crossfade.
                    nominal = static_cast<double>(write - target);
                }
                else
                    nominal += kHop * ratio;
                uint64_t base = static_cast<uint64_t>(std::llround(nominal));
                if (ratio == 1.0 && base != tail && std::llabs(static_cast<int64_t>(tail - base)) <= kSearch)
                {
                    // Back at full speed: resume the plain continuation (output == input again).
                    nominal = static_cast<double>(tail);
                    base = tail;
                }
                const bool exact = base == tail;
                const uint64_t need = std::max<uint64_t>(tail + kHop, base + kSearch + kHop);
                if (need > write)
                {
                    nominal -= kHop * ratio;
                    return false;
                }
                const int64_t off = exact ? 0 : bestOffset(ring, base);
                const uint64_t grain = static_cast<uint64_t>(static_cast<int64_t>(base) + off);
                if (grain == tail)
                {
                    for (uint32_t i = 0; i < kHop; ++i)
                        out[i] = at(ring, tail + i);
                }
                else
                {
                    for (uint32_t i = 0; i < kHop; ++i)
                    {
                        const uint32_t a = at(ring, tail + i), b = at(ring, grain + i);
                        const int32_t w = static_cast<int32_t>(i * 32768u / kHop);
                        out[i] = pack((left(a) * (32768 - w) + left(b) * w) >> 15, (right(a) * (32768 - w) + right(b) * w) >> 15);
                    }
                }
                tail = grain + kHop;
                // Keep everything a later step may read (the tail and the search window) in the ring.
                const double lowest = std::min(static_cast<double>(tail), nominal + kHop * 0.5 - kSearch);
                const uint64_t keep = static_cast<uint64_t>(std::max(0.0, lowest));
                if (keep > ring.read.load(std::memory_order_relaxed))
                    ring.read.store(keep, std::memory_order_release);
                outPos = 0;
                return true;
            }
        };

#endif

        struct State
        {
            Ring ring;
            bool device = false;
            uint32_t targetFrames = 80u * kRate / 1000u;
            bool growOnUnderrun = false;
            uint64_t underruns = 0;
            uint64_t dropped = 0;
            // Periodic stats (online play, or PS2X_AUDIO_STATS=1): written by the device thread, printed by sink.
            bool stats = false;
            uint64_t statsFrames = 0;
            std::atomic<uint32_t> minFill{0xFFFFFFFFu};
            std::atomic<uint32_t> hops{0}, slowHops{0}, fastHops{0};
            std::atomic<uint32_t> underrunsSeen{0};
#if !defined(PLATFORM_VITA)
            AudioStream stream{};
            Stretcher stretcher;
            // PS2X_AUDIO_DEVICE_DUMP: what the device plays, in real time (for screen recordings).
            Ring deviceRing;
            std::FILE *deviceDump = nullptr;
            std::thread deviceWriter;
            std::atomic<bool> stopWriter{false};
            bool deviceDumpStarted = false;
#endif
            std::FILE *dump = nullptr;
            uint64_t dumpFrames = 0;
            uint64_t dumpFramesAtHeader = 0;
            std::mutex dumpMutex;
        };

        State *g_state = nullptr;

        void writeWavHeader(std::FILE *f, uint64_t frames)
        {
            const uint32_t dataBytes = static_cast<uint32_t>(std::min<uint64_t>(frames * 4u, 0xFFFFFFFFull - 44u));
            uint8_t h[44];
            const auto u32 = [&](int at, uint32_t v) { std::memcpy(h + at, &v, 4); };
            const auto u16 = [&](int at, uint16_t v) { std::memcpy(h + at, &v, 2); };
            std::memcpy(h, "RIFF", 4);
            u32(4, 36u + dataBytes);
            std::memcpy(h + 8, "WAVEfmt ", 8);
            u32(16, 16u);
            u16(20, 1u); // PCM
            u16(22, 2u);
            u32(24, kRate);
            u32(28, kRate * 4u);
            u16(32, 4u);
            u16(34, 16u);
            std::memcpy(h + 36, "data", 4);
            u32(40, dataBytes);
            const long pos = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            std::fwrite(h, 1, sizeof(h), f);
            if (pos > 0)
                std::fseek(f, pos, SEEK_SET);
        }

        void sink(const int16_t *interleaved, uint32_t frames, void *user)
        {
            State &s = *static_cast<State *>(user);
            if (s.device)
            {
                Ring &ring = s.ring;
                const uint64_t w = ring.write.load(std::memory_order_relaxed);
                const uint64_t r = ring.read.load(std::memory_order_acquire);
                const uint64_t room = kRingFrames - (w - r);
                const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(frames, room));
                for (uint32_t i = 0; i < n; ++i)
                {
                    uint32_t packed;
                    std::memcpy(&packed, interleaved + 2u * i, 4);
                    ring.data[(w + i) & (kRingFrames - 1u)] = packed;
                }
                ring.write.store(w + n, std::memory_order_release);
                s.dropped += frames - n;
                if (s.stats && (s.statsFrames += frames) >= 10u * kRate)
                {
                    s.statsFrames = 0;
                    const uint32_t mf = s.minFill.exchange(0xFFFFFFFFu);
                    const uint32_t hops = s.hops.exchange(0), slow = s.slowHops.exchange(0), fast = s.fastHops.exchange(0);
                    // Rate: 10 s of emulated audio over the real time it took (100% = in step with the device;
                    // above = emulated time runs fast). Display: the refresh the VBlank clock locks to (0 = none).
                    static auto s_statsStart = std::chrono::steady_clock::now();
                    const auto now = std::chrono::steady_clock::now();
                    const double secs = std::chrono::duration<double>(now - s_statsStart).count();
                    s_statsStart = now;
                    const int64_t refresh = g_ps2xHostRefreshNs.load(std::memory_order_relaxed);
                    std::fprintf(stderr, "[audio] stats: underruns %u (total %llu), dropped %llu, buffer %u ms, lowest fill %d ms, stretched %u/%u slow %u fast, rate %.1f%%, display %.2f Hz\n",
                                 s.underrunsSeen.exchange(0), static_cast<unsigned long long>(s.underruns), static_cast<unsigned long long>(s.dropped),
                                 s.targetFrames * 1000u / kRate, mf == 0xFFFFFFFFu ? -1 : static_cast<int>(mf * 1000u / kRate), slow, hops, fast,
                                 secs > 1.0 ? 1000.0 / secs : 0.0, refresh > 0 ? 1.0e9 / static_cast<double>(refresh) : 0.0);
                }
            }
            if (s.dump)
            {
                std::lock_guard<std::mutex> lock(s.dumpMutex);
                if (s.dump)
                {
                    std::fwrite(interleaved, 4, frames, s.dump);
                    s.dumpFrames += frames;
                    // Keep the header valid about once a second, so a killed process leaves a playable file.
                    if (s.dumpFrames - s.dumpFramesAtHeader >= kRate)
                    {
                        writeWavHeader(s.dump, s.dumpFrames);
                        std::fflush(s.dump);
                        s.dumpFramesAtHeader = s.dumpFrames;
                    }
                }
            }
        }

#if !defined(PLATFORM_VITA)
        void renderDevice(void *buffer, unsigned int frames);

        // Audio device thread: copy what was just handed to the device into the device dump ring
        // (never blocks; the writer thread saves it).
        void deviceCallback(void *buffer, unsigned int frames)
        {
            renderDevice(buffer, frames);
            State *sp = g_state;
            if (!sp || !sp->deviceDump)
                return;
            State &s = *sp;
            if (!s.deviceDumpStarted)
            {
                s.deviceDumpStarted = true;
                const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::system_clock::now().time_since_epoch()).count();
                std::fprintf(stderr, "[audio] device dump starts at unix_ms=%lld (buffer %u ms)\n", ms, s.targetFrames * 1000u / kRate);
            }
            Ring &ring = s.deviceRing;
            const uint64_t w = ring.write.load(std::memory_order_relaxed);
            const uint64_t r = ring.read.load(std::memory_order_acquire);
            const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(frames, kRingFrames - (w - r)));
            const uint32_t *in = static_cast<const uint32_t *>(buffer);
            for (uint32_t i = 0; i < n; ++i)
                ring.data[(w + i) & (kRingFrames - 1u)] = in[i];
            ring.write.store(w + n, std::memory_order_release);
        }

        void deviceWriterLoop(State *s)
        {
            uint64_t frames = 0, framesAtHeader = 0;
            std::vector<uint32_t> chunk;
            for (bool last = false; !last;)
            {
                last = s->stopWriter.load();
                Ring &ring = s->deviceRing;
                const uint64_t w = ring.write.load(std::memory_order_acquire);
                const uint64_t r = ring.read.load(std::memory_order_relaxed);
                chunk.resize(static_cast<size_t>(w - r));
                for (uint64_t i = r; i < w; ++i)
                    chunk[static_cast<size_t>(i - r)] = ring.data[i & (kRingFrames - 1u)];
                ring.read.store(w, std::memory_order_release);
                if (!chunk.empty())
                {
                    std::fwrite(chunk.data(), 4, chunk.size(), s->deviceDump);
                    frames += chunk.size();
                }
                if (frames - framesAtHeader >= kRate || last)
                {
                    writeWavHeader(s->deviceDump, frames);
                    std::fflush(s->deviceDump);
                    framesAtHeader = frames;
                }
                if (!last)
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            std::fclose(s->deviceDump);
            s->deviceDump = nullptr;
        }

        // Play what emulated time has produced, about targetFrames behind.
        void renderDevice(void *buffer, unsigned int frames)
        {
            State *sp = g_state;
            uint32_t *out = static_cast<uint32_t *>(buffer);
            if (!sp)
            {
                std::memset(out, 0, frames * 4u);
                return;
            }
            State &s = *sp;
            Ring &ring = s.ring;
            Stretcher &st = s.stretcher;
            const uint64_t write = ring.write.load(std::memory_order_acquire);
            if (!st.running)
            {
                const uint64_t read = ring.read.load(std::memory_order_relaxed);
                if (write - read < s.targetFrames)
                {
                    std::memset(out, 0, frames * 4u);
                    return;
                }
                st.running = true;
                st.tail = read;
                st.nominal = static_cast<double>(read) - Stretcher::kHop;
                st.ratio = 1.0;
                st.outPos = Stretcher::kHop;
            }
            for (unsigned int i = 0; i < frames;)
            {
                if (st.outPos == Stretcher::kHop && s.stats)
                {
                    const uint32_t fill = static_cast<uint32_t>(write - st.tail);
                    if (fill < s.minFill.load(std::memory_order_relaxed))
                        s.minFill.store(fill, std::memory_order_relaxed);
                }
                if (st.outPos == Stretcher::kHop && !st.step(ring, write, s.targetFrames))
                {
                    // Underrun even when stretched: fade the last sample out, then refill.
                    int32_t l = left(st.lastFrame), r = right(st.lastFrame);
                    for (; i < frames; ++i)
                    {
                        l = l * 15 / 16;
                        r = r * 15 / 16;
                        out[i] = pack(l, r);
                    }
                    ring.read.store(std::max(ring.read.load(std::memory_order_relaxed), st.tail), std::memory_order_release);
                    st.running = false;
                    st.lastFrame = 0;
                    ++s.underruns;
                    s.underrunsSeen.fetch_add(1, std::memory_order_relaxed);
                    // Online the game waits on the other player now and then; buffer more after each gap.
                    if (s.growOnUnderrun)
                        s.targetFrames = std::min(s.targetFrames + 40u * kRate / 1000u, 400u * kRate / 1000u);
                    return;
                }
                if (st.outPos == 0 && s.stats)
                {
                    s.hops.fetch_add(1, std::memory_order_relaxed);
                    if (st.ratio < 1.0)
                        s.slowHops.fetch_add(1, std::memory_order_relaxed);
                    else if (st.ratio > 1.0)
                        s.fastHops.fetch_add(1, std::memory_order_relaxed);
                }
                const unsigned int n = std::min<unsigned int>(frames - i, Stretcher::kHop - st.outPos);
                std::memcpy(out + i, st.out + st.outPos, n * 4u);
                st.outPos += n;
                i += n;
                st.lastFrame = out[i - 1u];
            }
        }
#endif

        bool envOff(const char *name)
        {
            const char *e = std::getenv(name);
            return e && *e == '0';
        }
    }

    void startSpu2Output(bool deviceReady)
    {
        if (g_state)
            return;
        auto *s = new State();
        if (const char *lat = std::getenv("PS2X_AUDIO_LATENCY"); lat && *lat)
        {
            const int ms = std::atoi(lat);
            if (ms >= 10 && ms <= 500)
                s->targetFrames = static_cast<uint32_t>(ms) * kRate / 1000u;
        }
        else if (ps2NetplayActive())
        {
            s->targetFrames = 200u * kRate / 1000u;
            s->growOnUnderrun = true;
            s->stats = true;
        }
        if (const char *e = std::getenv("PS2X_AUDIO_STATS"); e && *e)
            s->stats = *e != '0';
        if (const char *path = std::getenv("PS2X_AUDIO_DUMP"); path && *path)
        {
            s->dump = std::fopen(path, "wb");
            if (s->dump)
            {
                std::setvbuf(s->dump, nullptr, _IOFBF, 1u << 20u);
                writeWavHeader(s->dump, 0);
                std::fprintf(stderr, "[audio] dumping SPU2 output to %s\n", path);
            }
            else
                std::fprintf(stderr, "[audio] cannot open PS2X_AUDIO_DUMP file %s\n", path);
        }
#if !defined(PLATFORM_VITA)
        if (deviceReady && !envOff("PS2X_AUDIO"))
        {
            SetAudioStreamBufferSizeDefault(1024);
            s->stream = LoadAudioStream(kRate, 16, 2);
            if (IsAudioStreamValid(s->stream))
            {
                s->device = true;
                if (const char *path = std::getenv("PS2X_AUDIO_DEVICE_DUMP"); path && *path)
                {
                    s->deviceDump = std::fopen(path, "wb");
                    if (s->deviceDump)
                    {
                        writeWavHeader(s->deviceDump, 0);
                        s->deviceWriter = std::thread(deviceWriterLoop, s);
                        std::fprintf(stderr, "[audio] dumping device output (real time) to %s\n", path);
                    }
                    else
                        std::fprintf(stderr, "[audio] cannot open PS2X_AUDIO_DEVICE_DUMP file %s\n", path);
                }
                g_state = s;
                SetAudioStreamCallback(s->stream, deviceCallback);
                // PS2X_AUDIO_VOLUME=<0-100> (default 50): speaker volume only. The game mixes near full
                // scale like a real PS2, which is loud on a PC; dumps keep the full level. Scripted test runs
                // (PS2X_AUTOPAD set) default to 1%.
                const char *autopad = std::getenv("PS2X_AUTOPAD");
                int volume = (autopad && *autopad) ? 1 : 50;
                if (const char *v = std::getenv("PS2X_AUDIO_VOLUME"); v && *v)
                    volume = std::clamp(std::atoi(v), 0, 100);
                SetAudioStreamVolume(s->stream, static_cast<float>(volume) / 100.0f);
                std::fprintf(stderr, "[audio] volume %d%%\n", volume);
                PlayAudioStream(s->stream);
            }
            else
                std::fprintf(stderr, "[audio] LoadAudioStream failed; no sound output\n");
        }
#else
        (void)deviceReady;
#endif
        if (!s->device && !s->dump)
        {
            std::fprintf(stderr, "[audio] SPU2 output off\n");
            delete s;
            return;
        }
        g_state = s;
        std::fprintf(stderr, "[audio] SPU2 output: %s%s, %u ms buffer\n", s->device ? "device" : "no device",
                     s->dump ? " + WAV dump" : "", s->targetFrames * 1000u / kRate);
        ps2x::iop::setSpu2AudioSink(&sink, s);
    }

    void stopSpu2Output()
    {
        State *s = g_state;
        if (!s)
            return;
        ps2x::iop::setSpu2AudioSink(nullptr, nullptr);
#if !defined(PLATFORM_VITA)
        if (s->device)
        {
            StopAudioStream(s->stream);
            UnloadAudioStream(s->stream);
            if (s->deviceWriter.joinable())
            {
                s->stopWriter.store(true);
                s->deviceWriter.join();
            }
        }
#endif
        g_state = nullptr;
        {
            std::lock_guard<std::mutex> lock(s->dumpMutex);
            if (s->dump)
            {
                writeWavHeader(s->dump, s->dumpFrames);
                std::fclose(s->dump);
                s->dump = nullptr;
            }
        }
        if (s->underruns || s->dropped)
            std::fprintf(stderr, "[audio] underruns=%llu dropped=%llu frames\n", static_cast<unsigned long long>(s->underruns),
                         static_cast<unsigned long long>(s->dropped));
        // Not deleted: the IOP thread may still be inside sink() for this state.
    }
}
