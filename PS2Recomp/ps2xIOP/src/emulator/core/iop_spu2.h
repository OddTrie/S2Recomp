#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace ps2x::iop::detail
{
    // SPU2: a voice clock plus (when an audio sink is set, see ps2x/iop/spu2_audio.h) the voice
    // renderer and mixer. The voice clock plays through ADPCM data at the right speed so the guest
    // sees what real hardware reports (NAX moving, ENDX bits, the IRQA interrupt). Audio streamers
    // (Sims 2's multi_streamer) and movie players pace themselves on exactly these, and without
    // them they wait forever. Everything the guest can read comes from the clock alone, so the
    // renderer (ADPCM decode, Gaussian interpolation, ADSR, volumes, core mixing, reverb) cannot
    // change guest behaviour whether audio is on or off.
    bool spu2RegLogActive();

    class Spu2
    {
    public:
        static constexpr uint32_t Base = 0x1F900000u;
        static constexpr uint32_t End = 0x1F900800u;
        static constexpr uint32_t IrqInfoAddress = 0x1F9007C2u;
        static constexpr uint32_t RamHalfwords = 0x100000u; // 2 MB
        static constexpr int Irq = 9;

        Spu2();
        void reset();

        // Called after the generic register store has already been updated.
        void onWrite16(uint32_t phys, uint16_t value);
        // Returns a value for registers the voice engine owns (NAX, ENDX, IRQINFO).
        [[nodiscard]] std::optional<uint16_t> read16(uint32_t phys) const;
        // SPU DMA from IOP RAM into SPU RAM at the core's TSA (or into the AutoDMA input when ADMAS is set).
        void dmaWrite(int core, const uint8_t *source, uint32_t bytes);
        // Advance all voices to `cycle` (IOP cycles, 36.864 MHz). Returns true if the SPU2 IRQ fired.
        bool advance(uint64_t cycle);
        // First cycle at which advance() would produce a sample (until then it only moves its
        // remainder, so skipping those calls is exact); 0 = before the first advance.
        [[nodiscard]] uint64_t nextSampleCycle() const noexcept;

        // True when a DMA write to `core` is an AutoDMA stream that completes at playback speed.
        [[nodiscard]] bool autoDmaPaced(int core) const;

        [[nodiscard]] static bool contains(uint32_t phys) noexcept { return phys >= Base && phys < End; }

    private:
        // ADSR envelope / volume sweep stepper (cycles counted at 48 kHz).
        struct Envelope
        {
            enum Phase : uint8_t { Off, Attack, Decay, Sustain, Release };
            int32_t level = 0;
            uint32_t counter = 0;
            Phase phase = Off;
        };

        struct Volume
        {
            uint16_t reg = 0;
            int32_t level = 0; // -0x8000..0x7FFF
            uint32_t counter = 0;
            void set(uint16_t value);
            void step();
        };

        struct Voice
        {
            // --- voice clock (guest visible) ---
            bool playing = false;
            bool lsaxWritten = false;
            bool releasing = false;
            uint16_t pitch = 0;
            uint16_t adsr2 = 0;
            uint16_t envx = 0;       // envelope level as the guest reads it (ENVX)
            uint32_t releaseStep = 0; // ENVX decrease per sample while releasing
            uint32_t ssa = 0;
            uint32_t lsax = 0;
            uint32_t block = 0;   // halfword address of the current 16-byte ADPCM block
            uint64_t counter = 0; // progress inside the block, in 1/4096 samples

            // --- renderer (host only) ---
            uint16_t adsr1 = 0;
            Envelope env;
            Volume volL, volR;
            bool decoded = false; // samples[] holds the current block
            int32_t prev1 = 0, prev2 = 0; // ADPCM filter history
            int16_t samples[31] = {}; // [0..2] last three of the previous block, [3..30] current block
        };

        struct Core
        {
            Voice voices[24];
            uint32_t endx = 0;
            uint32_t irqa = 0;
            uint32_t tsa = 0;
            uint16_t attr = 0;
            uint16_t kon[2] = {0, 0};
            uint16_t koff[2] = {0, 0};

            // mixer state
            uint32_t pmon = 0, non = 0;
            uint32_t vmixl = 0, vmixr = 0, vmixel = 0, vmixer = 0;
            uint16_t mmix = 0;
            uint16_t admas = 0;
            Volume mvolL, mvolR;
            int16_t evolL = 0, evolR = 0, avolL = 0, avolR = 0, bvolL = 0, bvolR = 0;
            // reverb
            uint32_t esa = 0, eea = 0;
            uint32_t revAddr[22] = {}; // 0x2E4..0x338 (hi/lo pairs)
            int16_t revCoef[10] = {};  // 0x774.. (+0x28 per core)
            uint32_t reverbX = 0;
            bool reverbOdd = false;
            int32_t revInL = 0, revInR = 0;
            int32_t revOutL = 0, revOutR = 0, revPrevL = 0, revPrevR = 0;
            // noise
            int32_t noiseTimer = 0;
            uint16_t noiseLevel = 1;
            // AutoDMA input (interleaved L,R)
            std::vector<int16_t> input;
            size_t inputRead = 0;
        };

        static uint32_t setHi(uint32_t addr, uint16_t v) { return (addr & 0xFFFFu) | ((static_cast<uint32_t>(v) & 0xFu) << 16u); }
        static uint32_t setLo(uint32_t addr, uint16_t v) { return (addr & 0xF0000u) | v; }
        void keyOn(Core &core, uint32_t mask);
        void keyOff(Core &core, uint32_t mask);
        bool stepBlock(int coreIndex, Voice &voice);
        void decodeBlock(Voice &voice);
        bool clockVoiceBulk(int coreIndex, Voice &voice, uint64_t samples);
        bool renderVoice(int coreIndex, int v, uint32_t n, const int16_t *modIn, const int16_t *noiseIn, int16_t *outx,
                         int32_t *dryL, int32_t *dryR, int32_t *wetL, int32_t *wetR);
        void stepEnvelope(Voice &voice);
        void stepNoise(Core &core);
        void reverb(Core &core, int32_t inL, int32_t inR, int32_t &outL, int32_t &outR);
        void mixChunk(uint32_t n, bool &irq);
        bool voiceAudible(const Core &core, int v) const;

        std::vector<uint16_t> m_ram;
        Core m_cores[2];
        uint16_t m_irqInfo = 0;
        uint64_t m_lastCycle = 0;
        uint64_t m_cycleRemainder = 0;
        bool m_started = false;
        struct Stats
        {
            uint32_t samples = 0;
            uint64_t voiceSamples[2] = {0, 0};
            uint32_t inputFrames[2] = {0, 0};
            int32_t peak = 0;
        };
        void printStats(uint32_t samples);
        Stats m_stats;
        std::vector<int16_t> m_out;           // mixed output of one advance() (interleaved L,R)
        bool m_render[2][24] = {};            // voices rendered per sample in this advance()
        uint32_t m_samplesAfterChunk = 0;     // samples of this advance() after the chunk being mixed
    };
}
