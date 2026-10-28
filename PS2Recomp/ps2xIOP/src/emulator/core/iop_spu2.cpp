#include "iop_spu2.h"

#include "ps2x/iop/spu2_audio.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace ps2x::iop
{
    namespace
    {
        std::atomic<Spu2AudioSink> g_sink{nullptr};
        std::atomic<void *> g_sinkUser{nullptr};
    }

    void setSpu2AudioSink(Spu2AudioSink sink, void *user)
    {
        // The user pointer stays when the sink is cleared: the IOP thread may be mid-advance with it.
        if (sink)
            g_sinkUser.store(user);
        g_sink.store(sink);
    }
}

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr uint64_t kCyclesPerSample = 768u; // 36.864 MHz / 48 kHz
        constexpr uint64_t kBlockUnits = 28u * 4096u; // 28 samples per ADPCM block, pitch 0x1000 = 1.0
        constexpr uint64_t kMaxSamplesPerAdvance = 48000u; // cap catch-up after long stalls (1 s)
        constexpr uint32_t kChunk = 256u; // samples mixed per pass
        constexpr size_t kInputCapFrames = 48000u; // AutoDMA input kept at most (1 s)
        bool traceOn()
        {
            static const bool on = [] { const char *e = std::getenv("PS2X_SPU2_TRACE"); return e && *e && *e != '0'; }();
            return on;
        }
        bool statsOn()
        {
            static const bool on = [] { const char *e = std::getenv("PS2X_SPU2_STATS"); return e && *e == '1'; }();
            return on;
        }
        bool traceAll()
        {
            static const bool on = [] { const char *e = std::getenv("PS2X_SPU2_TRACE"); return e && *e == '2'; }();
            return on;
        }
        uint32_t g_traceCount = 0;

        // 4-point Gaussian interpolation table (PS1/PS2 SPU hardware, psx-spx).
        constexpr int16_t kGauss[512] = {
            -1, -1, -1, -1, -1, -1, -1, -1,
            -1, -1, -1, -1, -1, -1, -1, -1,
            0, 0, 0, 0, 0, 0, 0, 1,
            1, 1, 1, 2, 2, 2, 3, 3,
            3, 4, 4, 5, 5, 6, 7, 7,
            8, 9, 9, 10, 11, 12, 13, 14,
            15, 16, 17, 18, 19, 21, 22, 24,
            25, 27, 28, 30, 32, 33, 35, 37,
            39, 41, 44, 46, 48, 51, 53, 56,
            58, 61, 64, 67, 70, 73, 77, 80,
            84, 87, 91, 95, 99, 103, 107, 111,
            116, 120, 125, 130, 135, 140, 145, 150,
            156, 161, 167, 173, 179, 186, 192, 199,
            205, 212, 219, 227, 234, 242, 250, 257,
            266, 274, 283, 291, 300, 309, 319, 328,
            338, 348, 358, 369, 379, 390, 401, 412,
            424, 436, 448, 460, 473, 485, 498, 512,
            525, 539, 553, 567, 582, 597, 612, 627,
            643, 659, 675, 692, 708, 726, 743, 761,
            779, 797, 816, 835, 854, 874, 894, 914,
            935, 956, 977, 999, 1020, 1043, 1066, 1089,
            1112, 1136, 1160, 1184, 1209, 1234, 1260, 1286,
            1312, 1339, 1366, 1394, 1422, 1450, 1479, 1508,
            1537, 1567, 1598, 1628, 1660, 1691, 1723, 1756,
            1789, 1822, 1856, 1890, 1924, 1959, 1995, 2031,
            2067, 2104, 2141, 2179, 2217, 2256, 2295, 2334,
            2374, 2415, 2456, 2497, 2539, 2582, 2624, 2668,
            2712, 2756, 2801, 2846, 2892, 2938, 2985, 3032,
            3079, 3128, 3176, 3225, 3275, 3325, 3376, 3427,
            3479, 3531, 3584, 3637, 3691, 3745, 3799, 3855,
            3910, 3967, 4023, 4081, 4138, 4197, 4255, 4315,
            4374, 4435, 4495, 4557, 4619, 4681, 4744, 4807,
            4871, 4935, 5000, 5065, 5131, 5197, 5264, 5332,
            5399, 5468, 5536, 5606, 5676, 5746, 5817, 5888,
            5959, 6032, 6104, 6177, 6251, 6325, 6400, 6475,
            6550, 6626, 6702, 6779, 6856, 6934, 7012, 7091,
            7170, 7249, 7329, 7409, 7490, 7571, 7653, 7735,
            7817, 7900, 7983, 8066, 8150, 8234, 8319, 8404,
            8489, 8575, 8661, 8748, 8834, 8922, 9009, 9097,
            9185, 9273, 9362, 9451, 9541, 9630, 9720, 9811,
            9901, 9992, 10083, 10174, 10266, 10358, 10450, 10542,
            10635, 10727, 10820, 10913, 11007, 11100, 11194, 11288,
            11382, 11476, 11571, 11665, 11760, 11855, 11950, 12045,
            12140, 12236, 12331, 12427, 12522, 12618, 12714, 12809,
            12905, 13001, 13097, 13193, 13289, 13385, 13481, 13577,
            13673, 13769, 13865, 13961, 14056, 14152, 14248, 14343,
            14439, 14534, 14630, 14725, 14820, 14915, 15010, 15104,
            15199, 15293, 15387, 15481, 15575, 15669, 15762, 15855,
            15948, 16041, 16133, 16226, 16317, 16409, 16500, 16592,
            16682, 16773, 16863, 16953, 17042, 17131, 17220, 17308,
            17396, 17484, 17571, 17658, 17744, 17830, 17916, 18001,
            18086, 18170, 18254, 18337, 18420, 18502, 18584, 18665,
            18746, 18826, 18905, 18985, 19063, 19141, 19219, 19295,
            19372, 19447, 19522, 19597, 19671, 19744, 19816, 19888,
            19959, 20030, 20100, 20169, 20238, 20306, 20373, 20439,
            20505, 20570, 20634, 20698, 20760, 20822, 20884, 20944,
            21004, 21063, 21121, 21178, 21235, 21290, 21345, 21399,
            21452, 21505, 21556, 21607, 21657, 21706, 21754, 21801,
            21848, 21893, 21938, 21982, 22025, 22066, 22107, 22148,
            22187, 22225, 22262, 22299, 22334, 22369, 22402, 22435,
            22467, 22498, 22527, 22556, 22584, 22611, 22637, 22662,
            22686, 22709, 22731, 22752, 22772, 22791, 22809, 22826,
            22842, 22857, 22872, 22885, 22897, 22908, 22918, 22927,
            22935, 22942, 22948, 22953, 22957, 22960, 22962, 22963,
        };

        constexpr int32_t kAdpcmPos[5] = {0, 60, 115, 98, 122};
        constexpr int32_t kAdpcmNeg[5] = {0, 0, -52, -55, -60};

        inline int32_t clamp16(int32_t v) { return v < -32768 ? -32768 : (v > 32767 ? 32767 : v); }

        // One envelope / sweep step (psx-spx "Envelope Operation"), cycles counted at 48 kHz.
        inline void envelopeTick(int32_t &level, uint32_t &counter, uint32_t shift, uint32_t stepIndex, bool exponential, bool decrease)
        {
            uint32_t cycles = 1u << (shift > 11u ? shift - 11u : 0u);
            int32_t step = decrease ? -8 + static_cast<int32_t>(stepIndex) : 7 - static_cast<int32_t>(stepIndex);
            step *= 1 << (shift < 11u ? 11u - shift : 0u);
            if (exponential && !decrease && level > 0x6000)
                cycles *= 4u;
            if (exponential && decrease)
                step = (step * level) >> 15;
            if (++counter < cycles)
                return;
            counter = 0;
            level = std::clamp(level + step, 0, 0x7FFF);
        }
    }

    // PS2X_SPU2_REGLOG=<sec>: after that many seconds of wall time, log the next 6000 SPU2
    // register reads/writes (address, value) to see what a sound driver polls.
    bool spu2RegLogActive()
    {
        static const double after = [] { const char *e = std::getenv("PS2X_SPU2_REGLOG"); return e ? std::atof(e) : -1.0; }();
        if (after < 0.0)
            return false;
        static const auto start = std::chrono::steady_clock::now();
        static uint32_t count = 0;
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < after)
            return false;
        return ++count <= 6000u;
    }
    namespace
    {
        bool traceSample() { ++g_traceCount; return traceAll() || g_traceCount <= 200u || (g_traceCount & (g_traceCount - 1u)) == 0u; }
    }

    void Spu2::Volume::set(uint16_t value)
    {
        reg = value;
        counter = 0;
        if ((value & 0x8000u) == 0u)
            level = static_cast<int16_t>(static_cast<uint16_t>(value << 1u));
    }

    void Spu2::Volume::step()
    {
        if ((reg & 0x8000u) == 0u)
            return;
        const bool negative = (reg & 0x1000u) != 0u;
        int32_t magnitude = std::clamp(negative ? -level : level, 0, 0x7FFF);
        envelopeTick(magnitude, counter, (reg >> 2u) & 0x1Fu, reg & 3u, (reg & 0x4000u) != 0u, (reg & 0x2000u) != 0u);
        level = negative ? -magnitude : magnitude;
    }

    Spu2::Spu2() : m_ram(RamHalfwords, 0u)
    {
        reset();
    }

    void Spu2::reset()
    {
        std::fill(m_ram.begin(), m_ram.end(), uint16_t{0});
        for (Core &core : m_cores)
            core = Core{};
        m_irqInfo = 0;
        m_lastCycle = 0;
        m_cycleRemainder = 0;
        m_started = false;
    }

    void Spu2::keyOn(Core &core, uint32_t mask)
    {
        for (int v = 0; v < 24; ++v)
        {
            if ((mask & (1u << v)) == 0u)
                continue;
            Voice &voice = core.voices[v];
            if (traceOn() && traceSample())
                std::fprintf(stderr, "[spu2] KON core=%d v=%d ssa=0x%x pitch=0x%x flags0=%02x adsr=%04x/%04x vol=%04x/%04x\n",
                             static_cast<int>(&core - m_cores), v, voice.ssa, voice.pitch,
                             m_ram[voice.ssa & (RamHalfwords - 1u) & ~7u] >> 8u, voice.adsr1, voice.adsr2, voice.volL.reg, voice.volR.reg);
            voice.playing = true;
            // No real ADSR for the guest: the attack is treated as instant, so ENVX reads non-zero
            // right after KON. Sound drivers (EA MultiStream) poll ENVX and treat 0 as "voice
            // finished"; with ENVX stuck at 0 they stopped and restarted streams forever. (The
            // renderer runs the real envelope in voice.env.)
            voice.envx = 0x7FFF;
            voice.releasing = false;
            voice.block = voice.ssa & (RamHalfwords - 1u) & ~7u;
            voice.counter = 0;
            voice.lsaxWritten = false;
            core.endx &= ~(1u << v);

            voice.env = Envelope{};
            voice.env.phase = Envelope::Attack;
            voice.decoded = false;
            voice.prev1 = voice.prev2 = 0;
            std::fill(std::begin(voice.samples), std::end(voice.samples), int16_t{0});
        }
    }

    void Spu2::keyOff(Core &core, uint32_t mask)
    {
        // Real hardware runs the release part of the envelope and keeps reading sample data.
        // The voice keeps advancing here too; ENVX falls to 0 over roughly the release time
        // (ADSR2 bits 0-4, 0 = fastest). Approximation: 8 << (rate / 2) samples.
        for (int v = 0; v < 24; ++v)
        {
            if ((mask & (1u << v)) == 0u)
                continue;
            Voice &voice = core.voices[v];
            const uint32_t rate = voice.adsr2 & 0x1Fu;
            const uint32_t samples = 8u << (rate / 2u);
            voice.releasing = true;
            voice.releaseStep = std::max<uint32_t>(1u, (0x7FFFu + samples - 1u) / samples);
            if (voice.env.phase != Envelope::Off)
            {
                voice.env.phase = Envelope::Release;
                voice.env.counter = 0;
            }
            if (traceOn() && traceSample())
                std::fprintf(stderr, "[spu2] KOFF core=%d v=%d release=%u samples\n", static_cast<int>(&core - m_cores), v, samples);
        }
    }

    void Spu2::onWrite16(uint32_t phys, uint16_t value)
    {
        if (spu2RegLogActive())
            std::fprintf(stderr, "[spureg] W %08x %04x\n", phys, value);
        if (phys == IrqInfoAddress)
        {
            m_irqInfo = 0; // writing acknowledges
            return;
        }
        if (phys >= Base + 0x760u)
        {
            // Per-core master/effect/input volumes and reverb coefficients (0x28 bytes per core).
            const uint32_t rel = phys - (Base + 0x760u);
            if (rel >= 2u * 0x28u)
                return;
            Core &core = m_cores[rel / 0x28u];
            const uint32_t r = rel % 0x28u;
            const int16_t s = static_cast<int16_t>(value);
            switch (r)
            {
            case 0x00: core.mvolL.set(value); break;
            case 0x02: core.mvolR.set(value); break;
            case 0x04: core.evolL = s; break;
            case 0x06: core.evolR = s; break;
            case 0x08: core.avolL = s; break;
            case 0x0A: core.avolR = s; break;
            case 0x0C: core.bvolL = s; break;
            case 0x0E: core.bvolR = s; break;
            default:
                if (r >= 0x14u && r < 0x28u)
                    core.revCoef[(r - 0x14u) / 2u] = s;
                break;
            }
            return;
        }

        const int coreIndex = phys >= Base + 0x400u ? 1 : 0;
        Core &core = m_cores[coreIndex];
        const uint32_t off = phys - (Base + static_cast<uint32_t>(coreIndex) * 0x400u);

        if (off < 0x180u)
        {
            Voice &voice = core.voices[off >> 4u];
            switch (off & 0xFu)
            {
            case 0x0: voice.volL.set(value); break;
            case 0x2: voice.volR.set(value); break;
            case 0x4: voice.pitch = value; break;
            case 0x6: voice.adsr1 = value; break;
            case 0x8: voice.adsr2 = value; break;
            case 0xA:
                voice.envx = value & 0x7FFFu; // ENVX is writable
                if (voice.env.phase != Envelope::Off)
                    voice.env.level = value & 0x7FFFu;
                break;
            default: break;
            }
            return;
        }
        if (off >= 0x1C0u && off < 0x1C0u + 24u * 0xCu)
        {
            const uint32_t rel = off - 0x1C0u;
            Voice &voice = core.voices[rel / 0xCu];
            switch (rel % 0xCu)
            {
            case 0x0: voice.ssa = setHi(voice.ssa, value); break;
            case 0x2: voice.ssa = setLo(voice.ssa, value); break;
            case 0x4: voice.lsax = setHi(voice.lsax, value); voice.lsaxWritten = true; break;
            case 0x6: voice.lsax = setLo(voice.lsax, value); voice.lsaxWritten = true; break;
            default: break; // NAX is read-only in practice
            }
            return;
        }
        if (off >= 0x2E4u && off < 0x33Cu)
        {
            const uint32_t k = (off - 0x2E4u) / 4u;
            core.revAddr[k] = ((off - 0x2E4u) & 2u) == 0u ? setHi(core.revAddr[k], value) : setLo(core.revAddr[k], value);
            return;
        }
        const auto setLo24 = [](uint32_t &mask, uint16_t v) { mask = (mask & 0xFF0000u) | v; };
        const auto setHi24 = [](uint32_t &mask, uint16_t v) { mask = (mask & 0xFFFFu) | (static_cast<uint32_t>(v & 0xFFu) << 16u); };
        switch (off)
        {
        case 0x180: setLo24(core.pmon, value); break;
        case 0x182: setHi24(core.pmon, value); break;
        case 0x184: setLo24(core.non, value); break;
        case 0x186: setHi24(core.non, value); break;
        case 0x188: setLo24(core.vmixl, value); break;
        case 0x18A: setHi24(core.vmixl, value); break;
        case 0x18C: setLo24(core.vmixel, value); break;
        case 0x18E: setHi24(core.vmixel, value); break;
        case 0x190: setLo24(core.vmixr, value); break;
        case 0x192: setHi24(core.vmixr, value); break;
        case 0x194: setLo24(core.vmixer, value); break;
        case 0x196: setHi24(core.vmixer, value); break;
        case 0x198:
            core.mmix = value;
            if (traceOn() && traceSample())
                std::fprintf(stderr, "[spu2] MMIX core=%d 0x%x\n", coreIndex, value);
            break;
        case 0x19A:
            core.attr = value;
            if (traceOn() && traceSample())
                std::fprintf(stderr, "[spu2] ATTR core=%d 0x%x\n", coreIndex, value);
            break;
        case 0x19C: core.irqa = setHi(core.irqa, value); break;
        case 0x19E: core.irqa = setLo(core.irqa, value); break;
        case 0x1A0: keyOn(core, value); break;
        case 0x1A2: keyOn(core, static_cast<uint32_t>(value & 0xFFu) << 16u); break;
        case 0x1A4: keyOff(core, value); break;
        case 0x1A6: keyOff(core, static_cast<uint32_t>(value & 0xFFu) << 16u); break;
        case 0x1A8: core.tsa = setHi(core.tsa, value); break;
        case 0x1AA: core.tsa = setLo(core.tsa, value); break;
        case 0x1AC: // manual transfer data port
            m_ram[core.tsa & (RamHalfwords - 1u)] = value;
            core.tsa = (core.tsa + 1u) & (RamHalfwords - 1u);
            break;
        case 0x1B0:
            core.admas = value;
            if (traceOn() && traceSample())
                std::fprintf(stderr, "[spu2] ADMAS core=%d 0x%x\n", coreIndex, value);
            break;
        case 0x2E0: core.esa = setHi(core.esa, value); break;
        case 0x2E2: core.esa = setLo(core.esa, value); break;
        case 0x33C:
            core.eea = (static_cast<uint32_t>(value & 0xFu) << 16u) | 0xFFFFu;
            if (traceOn() && traceSample())
                std::fprintf(stderr, "[spu2] reverb core=%d esa=0x%x eea=0x%x\n", coreIndex, core.esa, core.eea);
            break;
        case 0x340: core.endx &= 0xFF0000u; break;
        case 0x342: core.endx &= 0x00FFFFu; break;
        default: break;
        }
    }

    std::optional<uint16_t> Spu2::read16(uint32_t phys) const
    {
        if (phys == IrqInfoAddress)
            return m_irqInfo;
        if (phys >= Base + 0x760u)
            return std::nullopt;
        const int coreIndex = phys >= Base + 0x400u ? 1 : 0;
        const Core &core = m_cores[coreIndex];
        const uint32_t off = phys - (Base + static_cast<uint32_t>(coreIndex) * 0x400u);

        if (off < 0x180u && (off & 0xFu) == 0xAu)
            return core.voices[off >> 4u].envx;
        if (off >= 0x1C0u && off < 0x1C0u + 24u * 0xCu)
        {
            const uint32_t rel = off - 0x1C0u;
            const Voice &voice = core.voices[rel / 0xCu];
            if ((rel % 0xCu) == 0x8u || (rel % 0xCu) == 0xAu)
            {
                const uint32_t sampleInBlock = static_cast<uint32_t>(voice.counter >> 12u);
                const uint32_t nax = (voice.block + 1u + std::min<uint32_t>(sampleInBlock / 4u, 6u)) & (RamHalfwords - 1u);
                return (rel % 0xCu) == 0x8u ? static_cast<uint16_t>(nax >> 16u) : static_cast<uint16_t>(nax & 0xFFFFu);
            }
            return std::nullopt;
        }
        if (off == 0x340u)
            return static_cast<uint16_t>(core.endx & 0xFFFFu);
        if (off == 0x342u)
            return static_cast<uint16_t>(core.endx >> 16u);
        return std::nullopt;
    }

    namespace
    {
        // PS2X_SPU2_ADMA_LEGACY=1: the old AutoDMA handling (data written at TSA like a plain upload,
        // DMA completes at once). Only for comparisons: it overwrites sample banks and races the streamer.
        bool admaLegacy()
        {
            static const bool on = [] { const char *e = std::getenv("PS2X_SPU2_ADMA_LEGACY"); return e && *e == '1'; }();
            return on;
        }
    }

    bool Spu2::autoDmaPaced(int coreIndex) const
    {
        return !admaLegacy() && (m_cores[coreIndex & 1].admas & static_cast<uint16_t>((coreIndex & 1) + 1)) != 0u;
    }

    void Spu2::dmaWrite(int coreIndex, const uint8_t *source, uint32_t bytes)
    {
        Core &core = m_cores[coreIndex & 1];
        const uint32_t halfwords = bytes / 2u;
        const bool autoDma = (core.admas & static_cast<uint16_t>((coreIndex & 1) + 1)) != 0u;
        if (traceOn() && traceSample())
            std::fprintf(stderr, "[spu2] DMA core=%d tsa=0x%x bytes=%u%s\n", coreIndex, core.tsa, bytes, autoDma ? " (AutoDMA)" : "");
        if (!autoDma || admaLegacy())
        {
            for (uint32_t i = 0; i < halfwords; ++i)
            {
                uint16_t value;
                std::memcpy(&value, source + i * 2u, sizeof(value));
                m_ram[core.tsa & (RamHalfwords - 1u)] = value;
                core.tsa = (core.tsa + 1u) & (RamHalfwords - 1u);
            }
        }
        // AutoDMA data does not go to TSA: on hardware it lands in the core's input buffer
        // (0x2000 + core * 0x400), which nothing here reads; the mixer takes it from the FIFO below.
        if (!autoDma || ps2x::iop::g_sink.load(std::memory_order_relaxed) == nullptr)
            return;
        // AutoDMA: the stream is blocks of 256 left samples then 256 right samples, played at 48 kHz
        // through the core's sound data input (BVOL, MMIX). Kept as a FIFO for the mixer.
        if (core.inputRead > 0u && core.inputRead * 2u >= core.input.size())
        {
            core.input.erase(core.input.begin(), core.input.begin() + static_cast<std::ptrdiff_t>(core.inputRead));
            core.inputRead = 0;
        }
        for (uint32_t blockStart = 0; blockStart + 512u <= halfwords; blockStart += 512u)
        {
            for (uint32_t i = 0; i < 256u; ++i)
            {
                int16_t l, r;
                std::memcpy(&l, source + (blockStart + i) * 2u, 2u);
                std::memcpy(&r, source + (blockStart + 256u + i) * 2u, 2u);
                core.input.push_back(l);
                core.input.push_back(r);
            }
        }
        const size_t frames = (core.input.size() - core.inputRead) / 2u;
        if (frames > kInputCapFrames)
            core.inputRead += (frames - kInputCapFrames) * 2u;
    }

    bool Spu2::stepBlock(int coreIndex, Voice &voice)
    {
        Core &core = m_cores[coreIndex];
        const uint32_t block = voice.block & (RamHalfwords - 1u);
        const uint8_t flags = static_cast<uint8_t>(m_ram[block] >> 8u);
        bool irq = false;
        if ((core.attr & 0x40u) != 0u && core.irqa >= block && core.irqa < block + 8u)
        {
            m_irqInfo |= static_cast<uint16_t>(4u << coreIndex);
            irq = true;
            if (traceOn() && traceSample())
                std::fprintf(stderr, "[spu2] IRQ core=%d irqa=0x%x block=0x%x\n", coreIndex, core.irqa, block);
        }
        if ((flags & 0x4u) != 0u && !voice.lsaxWritten)
            voice.lsax = block;
        if ((flags & 0x1u) != 0u)
        {
            const int v = static_cast<int>(&voice - core.voices);
            core.endx |= 1u << v;
            if ((flags & 0x2u) != 0u)
                voice.block = voice.lsax & ~7u;
            else
            {
                voice.playing = false;
                voice.envx = 0; // end without loop: the voice goes silent at once
                voice.releasing = false;
                voice.env.phase = Envelope::Off;
                voice.env.level = 0;
            }
        }
        else
        {
            voice.block = (block + 8u) & (RamHalfwords - 1u);
        }
        return irq;
    }

    void Spu2::decodeBlock(Voice &voice)
    {
        const uint32_t block = voice.block & (RamHalfwords - 1u);
        const uint16_t header = m_ram[block];
        uint32_t shift = header & 0xFu;
        if (shift > 12u)
            shift = 9u;
        const uint32_t filter = std::min<uint32_t>((header >> 4u) & 7u, 4u);
        const int32_t f0 = kAdpcmPos[filter];
        const int32_t f1 = kAdpcmNeg[filter];
        int32_t p1 = voice.prev1, p2 = voice.prev2;
        int16_t *out = voice.samples + 3;
        for (uint32_t h = 1; h < 8u; ++h)
        {
            const uint16_t word = m_ram[(block + h) & (RamHalfwords - 1u)];
            for (uint32_t n = 0; n < 4u; ++n)
            {
                const int32_t nibble = static_cast<int16_t>(static_cast<uint16_t>(((word >> (n * 4u)) & 0xFu) << 12u));
                int32_t s = (nibble >> shift) + ((p1 * f0 + p2 * f1 + 32) >> 6);
                s = clamp16(s);
                *out++ = static_cast<int16_t>(s);
                p2 = p1;
                p1 = s;
            }
        }
        voice.prev1 = p1;
        voice.prev2 = p2;
        voice.decoded = true;
    }

    // Voice clock only (no sound): the original timing model, used for silent voices and with audio off.
    bool Spu2::clockVoiceBulk(int coreIndex, Voice &voice, uint64_t samples)
    {
        bool irq = false;
        if (!voice.playing || voice.pitch == 0u)
            return false;
        voice.counter += samples * std::min<uint64_t>(voice.pitch, 0x3FFFu);
        while (voice.playing && voice.counter >= kBlockUnits)
        {
            voice.counter -= kBlockUnits;
            irq |= stepBlock(coreIndex, voice);
        }
        return irq;
    }

    void Spu2::stepEnvelope(Voice &voice)
    {
        Envelope &e = voice.env;
        switch (e.phase)
        {
        case Envelope::Attack:
            envelopeTick(e.level, e.counter, (voice.adsr1 >> 10u) & 0x1Fu, (voice.adsr1 >> 8u) & 3u, (voice.adsr1 & 0x8000u) != 0u, false);
            if (e.level >= 0x7FFF)
            {
                e.phase = Envelope::Decay;
                e.counter = 0;
            }
            break;
        case Envelope::Decay:
            envelopeTick(e.level, e.counter, (voice.adsr1 >> 4u) & 0xFu, 0u, true, true);
            if (e.level <= static_cast<int32_t>(((voice.adsr1 & 0xFu) + 1u) * 0x800u))
            {
                e.phase = Envelope::Sustain;
                e.counter = 0;
            }
            break;
        case Envelope::Sustain:
            envelopeTick(e.level, e.counter, (voice.adsr2 >> 8u) & 0x1Fu, (voice.adsr2 >> 6u) & 3u, (voice.adsr2 & 0x8000u) != 0u,
                         (voice.adsr2 & 0x4000u) != 0u);
            break;
        case Envelope::Release:
            envelopeTick(e.level, e.counter, voice.adsr2 & 0x1Fu, 0u, (voice.adsr2 & 0x20u) != 0u, true);
            if (e.level <= 0)
            {
                e.level = 0;
                e.phase = Envelope::Off;
            }
            break;
        case Envelope::Off:
            break;
        }
    }

    void Spu2::stepNoise(Core &core)
    {
        const uint32_t clock = (core.attr >> 8u) & 0x3Fu;
        const int32_t reload = 0x20000 >> (clock >> 2u);
        core.noiseTimer -= 4 + static_cast<int32_t>(clock & 3u);
        const uint32_t l = core.noiseLevel;
        const uint32_t parity = ((l >> 15u) ^ (l >> 12u) ^ (l >> 11u) ^ (l >> 10u) ^ 1u) & 1u;
        if (core.noiseTimer < 0)
        {
            core.noiseLevel = static_cast<uint16_t>((l << 1u) | parity);
            core.noiseTimer += reload;
            if (core.noiseTimer < 0)
                core.noiseTimer += reload;
        }
    }

    bool Spu2::voiceAudible(const Core &core, int v) const
    {
        return core.voices[v].env.phase != Envelope::Off;
    }

    // Per-sample voice: Gaussian-interpolated ADPCM (or noise) x ADSR x volume into the core's dry/wet
    // buses, advancing the same voice clock as clockVoiceBulk (identical block steps, flags and IRQs).
    bool Spu2::renderVoice(int coreIndex, int v, uint32_t n, const int16_t *modIn, const int16_t *noiseIn, int16_t *outx,
                           int32_t *dryL, int32_t *dryR, int32_t *wetL, int32_t *wetR)
    {
        Core &core = m_cores[coreIndex];
        Voice &voice = core.voices[v];
        bool irq = false;
        if (voice.playing && !voice.decoded)
            decodeBlock(voice);
        const bool clocked = voice.playing && voice.pitch != 0u;
        const uint32_t baseStep = std::min<uint32_t>(voice.pitch, 0x3FFFu);
        const bool toDryL = ((core.vmixl >> v) & 1u) != 0u, toDryR = ((core.vmixr >> v) & 1u) != 0u;
        const bool toWetL = ((core.vmixel >> v) & 1u) != 0u, toWetR = ((core.vmixer >> v) & 1u) != 0u;
        const bool sweep = ((voice.volL.reg | voice.volR.reg) & 0x8000u) != 0u;
        for (uint32_t i = 0; i < n; ++i)
        {
            int32_t s = 0;
            if (noiseIn)
                s = noiseIn[i];
            else if (voice.playing)
            {
                const uint32_t idx = static_cast<uint32_t>(voice.counter >> 12u);
                const uint32_t g = static_cast<uint32_t>(voice.counter >> 4u) & 0xFFu;
                const int16_t *p = voice.samples + idx;
                s = (kGauss[0xFFu - g] * p[0]) >> 15;
                s += (kGauss[0x1FFu - g] * p[1]) >> 15;
                s += (kGauss[0x100u + g] * p[2]) >> 15;
                s += (kGauss[g] * p[3]) >> 15;
            }
            const int32_t out = (s * voice.env.level) >> 15;
            outx[i] = static_cast<int16_t>(out);
            stepEnvelope(voice);
            if (sweep)
            {
                voice.volL.step();
                voice.volR.step();
            }
            const int32_t l = (out * voice.volL.level) >> 15;
            const int32_t r = (out * voice.volR.level) >> 15;
            if (toDryL) dryL[i] += l;
            if (toDryR) dryR[i] += r;
            if (toWetL) wetL[i] += l;
            if (toWetR) wetR[i] += r;

            if (!clocked || !voice.playing)
                continue;
            uint32_t step = baseStep;
            if (modIn)
            {
                const int32_t factor = static_cast<int32_t>(modIn[i]) + 0x8000;
                step = static_cast<uint32_t>((static_cast<int32_t>(static_cast<int16_t>(voice.pitch)) * factor) >> 15) & 0xFFFFu;
                step = std::min<uint32_t>(step, 0x3FFFu);
            }
            voice.counter += step;
            if (voice.counter >= kBlockUnits)
            {
                voice.counter -= kBlockUnits;
                irq |= stepBlock(coreIndex, voice);
                if (voice.playing)
                {
                    voice.samples[0] = voice.samples[28];
                    voice.samples[1] = voice.samples[29];
                    voice.samples[2] = voice.samples[30];
                    decodeBlock(voice);
                }
                else
                {
                    // The bulk clock adds the whole interval's pitch before it notices the end:
                    // keep the same counter (only NAX of a stopped voice depends on it).
                    voice.counter += static_cast<uint64_t>(baseStep) * (n - i - 1u + m_samplesAfterChunk);
                }
            }
        }
        return irq;
    }

    void Spu2::reverb(Core &core, int32_t inL, int32_t inR, int32_t &outL, int32_t &outR)
    {
        outL = outR = 0;
        if (core.eea <= core.esa)
            return;
        const uint32_t size = core.eea - core.esa + 1u;
        const uint32_t x = core.reverbX;
        core.reverbX = (core.reverbX + 1u) % size;
        if ((core.attr & 0x80u) == 0u)
            return;
        // Offsets are relative to the moving buffer position and wrap inside ESA..EEA.
        const auto at = [&](int64_t offset) -> uint16_t & {
            int64_t pos = (static_cast<int64_t>(x) + offset) % static_cast<int64_t>(size);
            if (pos < 0)
                pos += size;
            return m_ram[(core.esa + static_cast<uint32_t>(pos)) & (RamHalfwords - 1u)];
        };
        const auto rd = [&](int64_t offset) -> int32_t { return static_cast<int16_t>(at(offset)); };
        const auto wr = [&](int64_t offset, int32_t value) { at(offset) = static_cast<uint16_t>(clamp16(value)); };
        const uint32_t *a = core.revAddr;
        const int16_t *c = core.revCoef;
        const int32_t vIIR = c[0], vWALL = c[5], vAPF1 = c[6], vAPF2 = c[7];
        const int64_t dAPF1 = a[0], dAPF2 = a[1];
        const int64_t mLSAME = a[2], mRSAME = a[3], dLSAME = a[8], dRSAME = a[9];
        const int64_t mLDIFF = a[10], mRDIFF = a[11], dLDIFF = a[16], dRDIFF = a[17];
        const int32_t lin = (inL * c[8]) >> 15;
        const int32_t rin = (inR * c[9]) >> 15;
        const auto iir = [&](int32_t input, int64_t src, int64_t dst) {
            const int32_t prev = rd(dst - 1);
            wr(dst, ((((input + ((rd(src) * vWALL) >> 15)) - prev) * vIIR) >> 15) + prev);
        };
        iir(lin, dLSAME, mLSAME);
        iir(rin, dRSAME, mRSAME);
        iir(lin, dRDIFF, mLDIFF);
        iir(rin, dLDIFF, mRDIFF);
        int32_t l = (c[1] * rd(a[4]) + c[2] * rd(a[6]) + c[3] * rd(a[12]) + c[4] * rd(a[14])) >> 15;
        int32_t r = (c[1] * rd(a[5]) + c[2] * rd(a[7]) + c[3] * rd(a[13]) + c[4] * rd(a[15])) >> 15;
        const auto apf = [&](int32_t value, int64_t m, int64_t d, int32_t coef) {
            const int32_t delayed = rd(m - d);
            value -= (coef * delayed) >> 15;
            wr(m, value);
            return ((clamp16(value) * coef) >> 15) + delayed;
        };
        l = apf(l, a[18], dAPF1, vAPF1);
        r = apf(r, a[19], dAPF1, vAPF1);
        l = apf(l, a[20], dAPF2, vAPF2);
        r = apf(r, a[21], dAPF2, vAPF2);
        outL = clamp16(l);
        outR = clamp16(r);
    }

    void Spu2::mixChunk(uint32_t n, bool &irq)
    {
        int32_t bus[2][4][kChunk];
        int16_t outxA[kChunk], outxB[kChunk];
        int16_t noise[2][kChunk];
        std::memset(bus, 0, sizeof(bus));
        for (int c = 0; c < 2; ++c)
        {
            Core &core = m_cores[c];
            // One noise generator per core, shared by its noise voices (NON).
            if (core.non != 0u)
                for (uint32_t i = 0; i < n; ++i)
                {
                    stepNoise(core);
                    noise[c][i] = static_cast<int16_t>(core.noiseLevel);
                }
            int16_t *prev = outxA, *cur = outxB;
            std::memset(prev, 0, sizeof(int16_t) * n);
            for (int v = 0; v < 24; ++v)
            {
                if (!m_render[c][v])
                {
                    std::memset(cur, 0, sizeof(int16_t) * n);
                }
                else
                {
                    const bool mod = v > 0 && ((core.pmon >> v) & 1u) != 0u;
                    const bool isNoise = ((core.non >> v) & 1u) != 0u;
                    irq |= renderVoice(c, v, n, mod ? prev : nullptr, isNoise ? noise[c] : nullptr, cur,
                                       bus[c][0], bus[c][1], bus[c][2], bus[c][3]);
                }
                std::swap(prev, cur);
            }
        }

        const size_t base = m_out.size();
        m_out.resize(base + 2u * n);
        int16_t *out = m_out.data() + base;
        for (uint32_t i = 0; i < n; ++i)
        {
            int32_t extL = 0, extR = 0;
            for (int c = 0; c < 2; ++c)
            {
                Core &core = m_cores[c];
                const uint16_t mmix = c == 0 ? static_cast<uint16_t>(core.mmix & 0xFF0u) : core.mmix;
                int32_t inL = 0, inR = 0;
                if (core.inputRead + 2u <= core.input.size())
                {
                    ++m_stats.inputFrames[c];
                    inL = (core.input[core.inputRead] * core.bvolL) >> 15;
                    inR = (core.input[core.inputRead + 1u] * core.bvolR) >> 15;
                    core.inputRead += 2u;
                }
                const int32_t vdL = clamp16(bus[c][0][i]), vdR = clamp16(bus[c][1][i]);
                const int32_t vwL = clamp16(bus[c][2][i]), vwR = clamp16(bus[c][3][i]);
                int32_t dL = 0, dR = 0, wL = 0, wR = 0;
                if (mmix & 0x001u) wR += extR;
                if (mmix & 0x002u) wL += extL;
                if (mmix & 0x004u) dR += extR;
                if (mmix & 0x008u) dL += extL;
                if (mmix & 0x010u) wR += inR;
                if (mmix & 0x020u) wL += inL;
                if (mmix & 0x040u) dR += inR;
                if (mmix & 0x080u) dL += inL;
                if (mmix & 0x100u) wR += vwR;
                if (mmix & 0x200u) wL += vwL;
                if (mmix & 0x400u) dR += vdR;
                if (mmix & 0x800u) dL += vdL;

                // Reverb runs at 24 kHz: average two inputs, interpolate between outputs.
                core.revInL += wL;
                core.revInR += wR;
                if (core.reverbOdd)
                {
                    int32_t rl, rr;
                    reverb(core, core.revInL / 2, core.revInR / 2, rl, rr);
                    core.revPrevL = core.revOutL;
                    core.revPrevR = core.revOutR;
                    core.revOutL = rl;
                    core.revOutR = rr;
                    core.revInL = core.revInR = 0;
                }
                const int32_t rvL = core.reverbOdd ? core.revOutL : (core.revPrevL + core.revOutL) / 2;
                const int32_t rvR = core.reverbOdd ? core.revOutR : (core.revPrevR + core.revOutR) / 2;
                core.reverbOdd = !core.reverbOdd;

                core.mvolL.step();
                core.mvolR.step();
                const int32_t mixL = dL + ((rvL * core.evolL) >> 15);
                const int32_t mixR = dR + ((rvR * core.evolR) >> 15);
                int32_t outL = clamp16((mixL * core.mvolL.level) >> 15);
                int32_t outR = clamp16((mixR * core.mvolR.level) >> 15);
                if (c == 0)
                {
                    // Core 0's output is core 1's external input (AVOL; BVOL is the AutoDMA sound data volume).
                    Core &next = m_cores[1];
                    extL = (outL * next.avolL) >> 15;
                    extR = (outR * next.avolR) >> 15;
                }
                else
                {
                    out[2u * i] = static_cast<int16_t>(outL);
                    out[2u * i + 1u] = static_cast<int16_t>(outR);
                }
            }
        }
    }

    // PS2X_SPU2_STATS=1: once per emulated second (with audio on), what the mixer did.
    void Spu2::printStats(uint32_t samples)
    {
        for (int c = 0; c < 2; ++c)
            for (int v = 0; v < 24; ++v)
                if (m_render[c][v])
                    m_stats.voiceSamples[c] += samples;
        for (size_t i = 0; i < m_out.size(); ++i)
            m_stats.peak = std::max<int32_t>(m_stats.peak, std::abs(static_cast<int32_t>(m_out[i])));
        m_stats.samples += samples;
        if (m_stats.samples < 48000u)
            return;
        const Core &a = m_cores[0], &b = m_cores[1];
        std::fprintf(stderr,
                     "[spu2 stats] voices %.1f/%.1f input %u/%u frames peak %d | c0 attr=%04x mmix=%03x mvol=%04x/%04x avol=%04x bvol=%04x evol=%04x"
                     " | c1 attr=%04x mmix=%03x mvol=%04x/%04x avol=%04x bvol=%04x/%04x evol=%04x\n",
                     m_stats.voiceSamples[0] / static_cast<double>(m_stats.samples), m_stats.voiceSamples[1] / static_cast<double>(m_stats.samples),
                     m_stats.inputFrames[0], m_stats.inputFrames[1], m_stats.peak, a.attr, a.mmix, a.mvolL.reg, a.mvolR.reg,
                     static_cast<uint16_t>(a.avolL), static_cast<uint16_t>(a.bvolL), static_cast<uint16_t>(a.evolL), b.attr, b.mmix, b.mvolL.reg,
                     b.mvolR.reg, static_cast<uint16_t>(b.avolL), static_cast<uint16_t>(b.bvolL), static_cast<uint16_t>(b.bvolR),
                     static_cast<uint16_t>(b.evolL));
        for (int c = 0; c < 2; ++c)
        {
            const Core &core = m_cores[c];
            std::fprintf(stderr, "[spu2 stats]   c%d vmix dry %06x/%06x wet %06x/%06x pmon %06x non %06x:", c, core.vmixl, core.vmixr,
                         core.vmixel, core.vmixer, core.pmon, core.non);
            for (int v = 0; v < 24; ++v)
                if (m_render[c][v])
                {
                    uint32_t hash = 2166136261u; // FNV of the 64 blocks behind the voice
                    for (uint32_t i = 0; i < 512u; ++i)
                        hash = (hash ^ m_ram[(core.voices[v].block - 512u + i) & (RamHalfwords - 1u)]) * 16777619u;
                    std::fprintf(stderr, " v%d vol=%04x/%04x env=%04x pitch=%04x blk=%05x data=%08x", v, core.voices[v].volL.reg,
                                 core.voices[v].volR.reg, core.voices[v].env.level, core.voices[v].pitch, core.voices[v].block, hash);
                }
            std::fprintf(stderr, "\n");
        }
        m_stats = Stats{};
    }

    uint64_t Spu2::nextSampleCycle() const noexcept
    {
        return m_started ? m_lastCycle + (kCyclesPerSample - m_cycleRemainder) : 0u;
    }

    bool Spu2::advance(uint64_t cycle)
    {
        if (!m_started)
        {
            m_started = true;
            m_lastCycle = cycle;
            return false;
        }
        if (cycle <= m_lastCycle)
            return false;
        uint64_t elapsed = cycle - m_lastCycle + m_cycleRemainder;
        m_lastCycle = cycle;
        uint64_t samples = elapsed / kCyclesPerSample;
        m_cycleRemainder = elapsed % kCyclesPerSample;
        if (samples == 0u)
            return false;
        samples = std::min(samples, kMaxSamplesPerAdvance);

        const Spu2AudioSink sink = ps2x::iop::g_sink.load(std::memory_order_acquire);
        bool irq = false;
        for (int c = 0; c < 2; ++c)
        {
            for (int v = 0; v < 24; ++v)
            {
                Voice &voice = m_cores[c].voices[v];
                if (voice.releasing)
                {
                    const uint64_t drop = samples * voice.releaseStep;
                    voice.envx = drop >= voice.envx ? 0u : static_cast<uint16_t>(voice.envx - drop);
                    if (voice.envx == 0u)
                        voice.releasing = false;
                }
                m_render[c][v] = sink != nullptr && voiceAudible(m_cores[c], v);
                if (m_render[c][v])
                    continue;
                voice.decoded = false;
                irq |= clockVoiceBulk(c, voice, samples);
            }
        }
        if (sink == nullptr)
            return irq;

        m_out.clear();
        for (uint64_t left = samples; left > 0u;)
        {
            const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(left, kChunk));
            left -= n;
            m_samplesAfterChunk = static_cast<uint32_t>(left);
            mixChunk(n, irq);
        }
        if (statsOn())
            printStats(static_cast<uint32_t>(samples));
        if (!m_out.empty())
            sink(m_out.data(), static_cast<uint32_t>(m_out.size() / 2u), ps2x::iop::g_sinkUser.load(std::memory_order_relaxed));
        return irq;
    }
}
