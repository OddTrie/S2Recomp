// Based on Blackline Interactive implementation
#include "runtime/ps2_memory.h"
#include "ps2_vif1_ops.h"
#include <immintrin.h>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

enum VIFCmd : uint8_t
{
    VIF_NOP = 0x00,
    VIF_STCYCL = 0x01,
    VIF_OFFSET = 0x02,
    VIF_BASE = 0x03,
    VIF_ITOP = 0x04,
    VIF_STMOD = 0x05,
    VIF_MSKPATH3 = 0x06,
    VIF_MARK = 0x07,
    VIF_FLUSHE = 0x10,
    VIF_FLUSH = 0x11,
    VIF_FLUSHA = 0x13,
    VIF_MSCAL = 0x14,
    VIF_MSCALF = 0x15,
    VIF_MSCNT = 0x17,
    VIF_STMASK = 0x20,
    VIF_STROW = 0x30,
    VIF_STCOL = 0x31,
    VIF_MPG = 0x4A,
    VIF_DIRECT = 0x50,
    VIF_DIRECTHL = 0x51,
};

namespace
{
    // PS2X_VIF_TRACE=1: log VIF1 MPG uploads / MSCAL starts (first 64 of each, then powers of two).
    bool vifTraceOn()
    {
        static const bool on = [] { const char *e = std::getenv("PS2X_VIF_TRACE"); return e && *e && *e != '0'; }();
        return on;
    }
    bool vifTraceSample(uint32_t &counter)
    {
        ++counter;
        return counter <= 64u || (counter & (counter - 1u)) == 0u;
    }

    // PS2X_VU1_BADKICK=<dir>: a ring of the last VIF1 commands (+ the last transfers) for the dump written
    // at the first bad XGKICK (ps2_vu1_core.cpp), and a log line per VU1 code change.
    struct VifDiagCmd
    {
        uint64_t item;
        uint32_t pos, cmd, size;
        uint16_t tops, base, ofst, itops;
        uint8_t dbf;
    };
    constexpr uint32_t kVifDiagRing = 1u << 16;
    constexpr uint32_t kVifDiagItems = 8u;
    struct VifDiag
    {
        VifDiagCmd cmds[kVifDiagRing];
        uint64_t cmdCount = 0;
        uint64_t itemCount = 0;
        std::vector<uint8_t> items[kVifDiagItems];
        uint64_t itemIds[kVifDiagItems] = {};
    };
    VifDiag *vifDiag()
    {
        static VifDiag *d = []() -> VifDiag *
        {
            const char *e = std::getenv("PS2X_VU1_BADKICK");
            return (e && *e) ? new VifDiag() : nullptr;
        }();
        return d;
    }

    constexpr uint8_t kGifFmtImage = 2u;

    uint32_t pendingGifImageQwc(const uint8_t *data, uint32_t sizeBytes)
    {
        if (!data || sizeBytes < 16u)
            return 0u;

        uint32_t offset = 0u;
        while (offset + 16u <= sizeBytes)
        {
            uint64_t tagLo = 0u;
            std::memcpy(&tagLo, data + offset, sizeof(tagLo));
            offset += 16u;

            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;

            uint64_t payloadBytes = 0u;
            if (flg == 0u) // PACKED
            {
                payloadBytes = static_cast<uint64_t>(nloop) * nreg * 16ull;
            }
            else if (flg == 1u) // REGLIST, padded to a quadword
            {
                payloadBytes = static_cast<uint64_t>(nloop) * nreg * 8ull;
                payloadBytes = (payloadBytes + 15ull) & ~15ull;
            }
            else if (flg == kGifFmtImage)
            {
                payloadBytes = static_cast<uint64_t>(nloop) * 16ull;
                const uint64_t availableBytes = sizeBytes - offset;
                if (payloadBytes > availableBytes)
                {
                    return static_cast<uint32_t>((payloadBytes - availableBytes) / 16ull);
                }
            }
            else
            {
                return 0u;
            }

            if (payloadBytes > static_cast<uint64_t>(sizeBytes - offset))
                return 0u;
            offset += static_cast<uint32_t>(payloadBytes);
        }

        return 0u;
    }
}

void PS2Memory::processVIF0Data(uint32_t srcPhys, uint32_t sizeBytes)
{
    if (sizeBytes == 0u || srcPhys >= PS2_RAM_SIZE)
        return;

    const uint64_t requestedEnd = static_cast<uint64_t>(srcPhys) + static_cast<uint64_t>(sizeBytes);
    if (requestedEnd > static_cast<uint64_t>(PS2_RAM_SIZE))
        sizeBytes = PS2_RAM_SIZE - srcPhys;

    processVIF0Data(m_rdram + srcPhys, sizeBytes);
}

void PS2Memory::processVIF0Data(const uint8_t *data, uint32_t sizeBytes)
{
    if (sizeBytes == 0u)
        return;

    uint32_t pos = 0;
    while (pos + 4 <= sizeBytes)
    {
        uint32_t cmd = 0u;
        std::memcpy(&cmd, data + pos, sizeof(cmd));
        pos += 4u;

        const uint8_t opcode = static_cast<uint8_t>((cmd >> 24) & 0x7Fu);
        const uint16_t imm = static_cast<uint16_t>(cmd & 0xFFFFu);
        const uint8_t num = static_cast<uint8_t>((cmd >> 16) & 0xFFu);
        const bool irq = (cmd & 0x80000000u) != 0u;

        vif0_regs.code = cmd;
        vif0_regs.num = num;
        if (irq)
            vif0_regs.stat |= (1u << 11);

        if (opcode == VIF_NOP)
        {
            continue;
        }
        else if (opcode == VIF_STCYCL)
        {
            vif0_regs.cycle = imm;
            continue;
        }
        else if (opcode == VIF_ITOP)
        {
            vif0_regs.itops = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_STMOD)
        {
            vif0_regs.mode = imm & 3u;
            continue;
        }
        else if (opcode == VIF_MARK)
        {
            vif0_regs.mark = imm;
            vif0_regs.stat |= (1u << 6);
            continue;
        }
        else if (opcode == VIF_FLUSHE || opcode == VIF_FLUSH || opcode == VIF_FLUSHA)
        {
            continue;
        }
        else if (opcode == VIF_STMASK)
        {
            if (pos + 4u > sizeBytes)
                break;
            std::memcpy(&vif0_regs.mask, data + pos, sizeof(vif0_regs.mask));
            pos += 4u;
            continue;
        }
        else if (opcode == VIF_STROW)
        {
            if (pos + 16u > sizeBytes)
                break;
            std::memcpy(vif0_regs.row, data + pos, 16u);
            pos += 16u;
            continue;
        }
        else if (opcode == VIF_STCOL)
        {
            if (pos + 16u > sizeBytes)
                break;
            std::memcpy(vif0_regs.col, data + pos, 16u);
            pos += 16u;
            continue;
        }
        else if (opcode == VIF_MPG)
        {
            const uint32_t destAddr = static_cast<uint32_t>(imm & 0x1FFu) * 8u;
            const uint32_t instructionCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            const uint32_t mpgBytes = instructionCount * 8u;
            uint32_t copyBytes = 0u;
            if (m_vu0Code && destAddr < PS2_VU0_CODE_SIZE && mpgBytes > 0u)
            {
                copyBytes = mpgBytes;
                if (destAddr + copyBytes > PS2_VU0_CODE_SIZE)
                    copyBytes = PS2_VU0_CODE_SIZE - destAddr;
                if (pos + copyBytes <= sizeBytes)
                {
                    std::memcpy(m_vu0Code + destAddr, data + pos, copyBytes);
                    markVU0CodeModified();
                }
            }

            pos += mpgBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else if ((opcode & 0x60u) == 0x60u)
        {
            const uint8_t vn = static_cast<uint8_t>((opcode >> 2) & 0x3u);
            const uint8_t vl = static_cast<uint8_t>(opcode & 0x3u);
            const int components = static_cast<int>(vn) + 1;
            int bitsPerComponent = 32;
            switch (vl)
            {
            case 0:
                bitsPerComponent = 32;
                break;
            case 1:
                bitsPerComponent = 16;
                break;
            case 2:
                bitsPerComponent = 8;
                break;
            case 3:
                bitsPerComponent = (vn == 3u) ? 4 : 16;
                break;
            default:
                break;
            }
            const int bitsPerVector = (vl == 3u && vn == 3u) ? 16 : (components * bitsPerComponent);
            uint32_t bytesPerVector = static_cast<uint32_t>((bitsPerVector + 7) / 8);
            const uint32_t writeVectorCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            uint32_t cl = vif0_regs.cycle & 0xFFu;
            uint32_t wl = (vif0_regs.cycle >> 8) & 0xFFu;
            if (cl == 0u)
                cl = 1u;
            if (wl == 0u)
                wl = 1u;
            uint32_t sourceVectorCount = writeVectorCount;
            if (cl < wl)
            {
                const uint32_t fullBlocks = writeVectorCount / wl;
                uint32_t remainder = writeVectorCount % wl;
                if (remainder > cl)
                    remainder = cl;
                sourceVectorCount = fullBlocks * cl + remainder;
            }
            uint32_t totalBytes = sourceVectorCount * bytesPerVector;
            totalBytes = (totalBytes + 3u) & ~3u;

            if (m_vu0Data && pos + totalBytes <= sizeBytes && vl == 0u)
            {
                uint32_t vuAddr = static_cast<uint32_t>(imm & 0x3FFu);
                if ((imm & 0x8000u) != 0u)
                    vuAddr = (vuAddr + (vif0_regs.tops & 0x3FFu)) & 0x3FFu;
                const uint8_t *srcBase = data + pos;
                uint32_t srcIndex = 0u;
                for (uint32_t writeIndex = 0; writeIndex < writeVectorCount; ++writeIndex)
                {
                    const uint32_t cyclePos = writeIndex % wl;
                    const bool sourceAvailable = (cl >= wl) || (cyclePos < cl);
                    uint32_t destVec = (cl >= wl) ? ((vuAddr + (writeIndex / wl) * cl + cyclePos) & 0x3FFu)
                                                  : ((vuAddr + writeIndex) & 0x3FFu);
                    const uint32_t destOff = destVec * 16u;
                    if (destOff + 16u > PS2_VU0_DATA_SIZE)
                    {
                        if (sourceAvailable && srcIndex < sourceVectorCount)
                            ++srcIndex;
                        continue;
                    }
                    if (!sourceAvailable || srcIndex >= sourceVectorCount)
                        continue;
                    const uint8_t *srcVec = srcBase + srcIndex * bytesPerVector;
                    ++srcIndex;
                    uint32_t lanes[4] = {0u, 0u, 0u, 0u};
                    std::memcpy(lanes, m_vu0Data + destOff, sizeof(lanes));
                    const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                    for (uint32_t c = 0; c < limit; ++c)
                    {
                        uint32_t scalar = 0u;
                        std::memcpy(&scalar, srcVec + c * 4u, sizeof(scalar));
                        lanes[c] = scalar;
                    }
                    _mm_storeu_si128(reinterpret_cast<__m128i *>(m_vu0Data + destOff), _mm_loadu_si128(reinterpret_cast<const __m128i *>(lanes)));
                }
            }
            pos += totalBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else
        {
            break;
        }
    }
}

void PS2Memory::processVIF1Data(uint32_t srcPhys, uint32_t sizeBytes)
{
    if (sizeBytes == 0u || srcPhys >= PS2_RAM_SIZE)
        return;

    const uint64_t requestedEnd = static_cast<uint64_t>(srcPhys) + static_cast<uint64_t>(sizeBytes);
    if (requestedEnd > static_cast<uint64_t>(PS2_RAM_SIZE))
        sizeBytes = PS2_RAM_SIZE - srcPhys;

    processVIF1Data(m_rdram + srcPhys, sizeBytes);
}

namespace
{
    // Where processVIF1Data's effects outside the VIF1 registers go. DirectSink = the serial
    // interpreter (VU1 memory, GIF arbiter and VU1 right away, as before); OpSink records them for
    // the VU1 thread (PS2X_VU1_THREADS=2). UNPACK writes are given as lanes + a mask of the lanes
    // written, so neither sink needs to read VU1 memory.
    struct DirectSink
    {
        static constexpr bool kDirect = true;
        PS2Memory &m;

        bool hasVuData() const { return m.m_vu1Data != nullptr; }
        bool hasVuCode() const { return m.m_vu1Code != nullptr; }
        void path2(const uint8_t *p, uint32_t n, bool hl, bool /*temporary*/)
        {
            m.submitGifPacket(GifPathId::Path2, p, n, true, hl);
        }
        void maskPath3(bool masked)
        {
            const bool wasMasked = m.m_path3Masked;
            m.m_path3Masked = masked;
            if (wasMasked && !m.m_path3Masked)
                m.flushMaskedPath3Packets();
        }
        void mscal(uint32_t startPC, uint32_t top, uint32_t itop)
        {
            if (m.m_vu1MscalCallback)
                m.m_vu1MscalCallback(startPC, top, itop);
        }
        void mscnt(uint32_t top, uint32_t itop)
        {
            if (m.m_vu1MscntCallback)
                m.m_vu1MscntCallback(top, itop);
        }
        void mpg(uint32_t destAddr, const uint8_t *src, uint32_t n)
        {
            // Games re-send the same microprogram constantly; only a real change needs
            // the VU1 interpreter to re-decode its code cache.
            if (std::memcmp(m.m_vu1Code + destAddr, src, n) != 0)
            {
                if (vifDiag())
                {
                    // Each distinct upload (dest, size, content) at its 1st, 2nd, 4th, 8th, ... time.
                    static std::unordered_map<uint64_t, uint64_t> s_seen;
                    uint64_t h = 1469598103934665603ull;
                    for (uint32_t i = 0; i < n; ++i)
                        h = (h ^ src[i]) * 1099511628211ull;
                    const uint64_t key = h ^ (static_cast<uint64_t>(destAddr) << 40) ^ (static_cast<uint64_t>(n) << 20);
                    const uint64_t count = ++s_seen[key];
                    if ((count & (count - 1u)) == 0u)
                        std::fprintf(stderr, "[vu1diag] MPG upload x%llu item %llu dest=0x%x bytes=%u hash=%016llx\n", (unsigned long long)count,
                                     (unsigned long long)vifDiag()->itemCount, destAddr, n, (unsigned long long)h);
                }
                std::memcpy(m.m_vu1Code + destAddr, src, n);
                m.markVU1CodeModified();
            }
        }
        void copyVecs(uint32_t destVec, const uint8_t *src, uint32_t count)
        {
            std::memcpy(m.m_vu1Data + destVec * 16u, src, count * 16u);
        }
        void lanes(uint32_t destOff, const uint32_t *lanes, uint8_t mask)
        {
            if (mask == 0xFu)
            {
                std::memcpy(m.m_vu1Data + destOff, lanes, 16u);
                return;
            }
            for (uint32_t f = 0; f < 4u; ++f)
                if ((mask >> f) & 1u)
                    std::memcpy(m.m_vu1Data + destOff + f * 4u, &lanes[f], 4u);
        }
        void bytes(uint32_t destOff, const uint8_t *src, uint32_t n)
        {
            std::memcpy(m.m_vu1Data + destOff, src, n);
        }
        uint32_t readLane(uint32_t destOff, uint32_t field) const
        {
            uint32_t v = 0;
            std::memcpy(&v, m.m_vu1Data + destOff + field * 4u, 4u);
            return v;
        }
    };

    struct OpSink
    {
        static constexpr bool kDirect = false;
        PS2Memory &m;
        Vif1OpList &out;
        Vif1DecodeHooks &hooks;
        uint32_t mscalsSinceFlush = 0;
        bool firstFlushDone = false;

        bool hasVuData() const { return m.m_vu1Data != nullptr; }
        bool hasVuCode() const { return m.m_vu1Code != nullptr; }
        Vif1Op &add(Vif1OpKind kind)
        {
            out.ops.emplace_back();
            out.ops.back().kind = kind;
            return out.ops.back();
        }
        void path2(const uint8_t *p, uint32_t n, bool hl, bool temporary)
        {
            Vif1Op &op = add(Vif1OpKind::Path2);
            op.mask = hl ? 1u : 0u;
            op.count = n;
            if (temporary)
            {
                op.inlineOffset = static_cast<uint32_t>(out.inlineBytes.size());
                out.inlineBytes.insert(out.inlineBytes.end(), p, p + n);
            }
            else
                op.ptr = p;
        }
        void maskPath3(bool masked) { add(Vif1OpKind::MaskPath3).mask = masked ? 1u : 0u; }
        // VU1 starts on the first MSCAL of a transfer, then every flushEveryMscal MSCALs get handed over.
        void afterRun()
        {
            if (!hooks.flush)
                return;
            if (!firstFlushDone || ++mscalsSinceFlush >= hooks.flushEveryMscal)
            {
                firstFlushDone = true;
                mscalsSinceFlush = 0;
                hooks.flush(out);
            }
        }
        void mscal(uint32_t startPC, uint32_t top, uint32_t itop)
        {
            Vif1Op &op = add(Vif1OpKind::Mscal);
            op.dest = startPC;
            op.lanes[0] = top;
            op.lanes[1] = itop;
            afterRun();
        }
        void mscnt(uint32_t top, uint32_t itop)
        {
            Vif1Op &op = add(Vif1OpKind::Mscnt);
            op.lanes[0] = top;
            op.lanes[1] = itop;
            afterRun();
        }
        void mpg(uint32_t destAddr, const uint8_t *src, uint32_t n)
        {
            Vif1Op &op = add(Vif1OpKind::Mpg);
            op.dest = destAddr;
            op.ptr = src;
            op.count = n;
        }
        void copyVecs(uint32_t destVec, const uint8_t *src, uint32_t count)
        {
            Vif1Op &op = add(Vif1OpKind::CopyVecs);
            op.dest = destVec;
            op.ptr = src;
            op.count = count;
        }
        void lanes(uint32_t destOff, const uint32_t *lanes, uint8_t mask)
        {
            if (mask == 0u)
                return;
            // Consecutive qword writes share one Lanes op (a run of Vif1LaneRec in inlineBytes).
            if (out.ops.empty() || out.ops.back().kind != Vif1OpKind::Lanes)
            {
                Vif1Op &op = add(Vif1OpKind::Lanes);
                op.inlineOffset = static_cast<uint32_t>(out.inlineBytes.size());
            }
            Vif1Op &op = out.ops.back();
            const size_t at = out.inlineBytes.size();
            out.inlineBytes.resize(at + sizeof(Vif1LaneRec));
            Vif1LaneRec rec;
            rec.destVec = static_cast<uint16_t>(destOff / 16u);
            rec.mask = mask;
            rec.pad = 0;
            std::memcpy(rec.lanes, lanes, 16u);
            std::memcpy(out.inlineBytes.data() + at, &rec, sizeof(rec));
            ++op.count;
        }
        void bytes(uint32_t destOff, const uint8_t *src, uint32_t n)
        {
            Vif1Op &op = add(Vif1OpKind::Bytes);
            op.dest = destOff;
            op.ptr = src;
            op.count = n;
        }
        // The old lane value: VU1 memory as the serial interpreter would see it here, so the VU1 thread
        // first applies everything before this point (rare: MODE 1/2 UNPACK of fewer than 4 components).
        uint32_t readLane(uint32_t destOff, uint32_t field)
        {
            hooks.sync(out);
            ++hooks.syncs;
            uint32_t v = 0;
            std::memcpy(&v, m.m_vu1Data + destOff + field * 4u, 4u);
            return v;
        }
    };

    // Applies one Lanes run: per qword, the masked lanes (a blend with the old qword: same bytes as
    // writing lane by lane).
    void applyLaneRun(uint8_t *vuData, const uint8_t *recs, uint32_t count)
    {
        static const __m128i kLaneMasks[16] = {
#define L(b) static_cast<int>(((b) & 1) ? -1 : 0), static_cast<int>(((b) & 2) ? -1 : 0), static_cast<int>(((b) & 4) ? -1 : 0), static_cast<int>(((b) & 8) ? -1 : 0)
            _mm_setr_epi32(L(0)), _mm_setr_epi32(L(1)), _mm_setr_epi32(L(2)), _mm_setr_epi32(L(3)),
            _mm_setr_epi32(L(4)), _mm_setr_epi32(L(5)), _mm_setr_epi32(L(6)), _mm_setr_epi32(L(7)),
            _mm_setr_epi32(L(8)), _mm_setr_epi32(L(9)), _mm_setr_epi32(L(10)), _mm_setr_epi32(L(11)),
            _mm_setr_epi32(L(12)), _mm_setr_epi32(L(13)), _mm_setr_epi32(L(14)), _mm_setr_epi32(L(15)),
#undef L
        };
        for (uint32_t i = 0; i < count; ++i)
        {
            Vif1LaneRec rec;
            std::memcpy(&rec, recs + static_cast<size_t>(i) * sizeof(Vif1LaneRec), sizeof(rec));
            uint8_t *dest = vuData + static_cast<uint32_t>(rec.destVec) * 16u;
            const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i *>(rec.lanes));
            if (rec.mask == 0xFu)
                _mm_storeu_si128(reinterpret_cast<__m128i *>(dest), v);
            else
            {
                const __m128i old = _mm_loadu_si128(reinterpret_cast<const __m128i *>(dest));
                _mm_storeu_si128(reinterpret_cast<__m128i *>(dest), _mm_blendv_epi8(old, v, kLaneMasks[rec.mask & 0xFu]));
            }
        }
    }

    // PS2X_VU1_THREADS_VERIFY: the serial interpreter on a shadow PS2Memory (own VU1 data/code copies,
    // VIF1 state from the front's snapshot), stepping the real op list along at every effect the
    // shadow has outside VU1 data memory.
    struct VerifySink
    {
        static constexpr bool kDirect = false;
        PS2Memory &shadow;
        PS2Memory &real;
        const Vif1OpList &list;
        size_t cursor = 0;
        uint32_t mismatches = 0;

        bool hasVuData() const { return shadow.m_vu1Data != nullptr; }
        bool hasVuCode() const { return shadow.m_vu1Code != nullptr; }

        void report(const char *what)
        {
            ++mismatches;
            static uint32_t s_reported = 0;
            if (++s_reported <= 40u)
                std::fprintf(stderr, "[vu1-verify] MISMATCH %s at op %zu of %zu\n", what, cursor, list.ops.size());
        }
        void applyReal(const Vif1Op &op)
        {
            switch (op.kind)
            {
            case Vif1OpKind::CopyVecs:
                std::memcpy(real.m_vu1Data + op.dest * 16u, op.ptr, op.count * 16u);
                break;
            case Vif1OpKind::Lanes:
                applyLaneRun(real.m_vu1Data, list.bytes(op), op.count);
                break;
            case Vif1OpKind::Bytes:
                std::memcpy(real.m_vu1Data + op.dest, op.ptr, op.count);
                break;
            default:
                break;
            }
        }
        // Applies the real list's memory writes up to the next effect, which must be `kind`; then VU1 data
        // memory must equal the shadow's.
        const Vif1Op *advance(Vif1OpKind kind)
        {
            while (cursor < list.ops.size())
            {
                const Vif1Op &op = list.ops[cursor];
                if (op.kind != Vif1OpKind::CopyVecs && op.kind != Vif1OpKind::Lanes && op.kind != Vif1OpKind::Bytes)
                    break;
                applyReal(op);
                ++cursor;
            }
            if (std::memcmp(real.m_vu1Data, shadow.m_vu1Data, PS2_VU1_DATA_SIZE) != 0)
            {
                report("VU1 data memory");
                std::memcpy(shadow.m_vu1Data, real.m_vu1Data, PS2_VU1_DATA_SIZE);
            }
            if (cursor >= list.ops.size() || list.ops[cursor].kind != kind)
            {
                report("op order");
                return nullptr;
            }
            return &list.ops[cursor++];
        }
        void path2(const uint8_t *p, uint32_t n, bool hl, bool)
        {
            if (const Vif1Op *op = advance(Vif1OpKind::Path2))
            {
                if (op->count != n || (op->mask != 0u) != hl || std::memcmp(list.bytes(*op), p, n) != 0)
                    report("DIRECT packet");
                real.submitGifPacket(GifPathId::Path2, list.bytes(*op), op->count, true, op->mask != 0u);
            }
        }
        void maskPath3(bool masked)
        {
            if (const Vif1Op *op = advance(Vif1OpKind::MaskPath3))
            {
                if ((op->mask != 0u) != masked)
                    report("MSKPATH3");
                DirectSink{real}.maskPath3(op->mask != 0u);
            }
        }
        void mscal(uint32_t startPC, uint32_t top, uint32_t itop)
        {
            if (const Vif1Op *op = advance(Vif1OpKind::Mscal))
            {
                if (op->dest != startPC || op->lanes[0] != top || op->lanes[1] != itop)
                    report("MSCAL arguments");
                DirectSink{real}.mscal(op->dest, op->lanes[0], op->lanes[1]);
                std::memcpy(shadow.m_vu1Data, real.m_vu1Data, PS2_VU1_DATA_SIZE); // what the program wrote
            }
        }
        void mscnt(uint32_t top, uint32_t itop)
        {
            if (const Vif1Op *op = advance(Vif1OpKind::Mscnt))
            {
                if (op->lanes[0] != top || op->lanes[1] != itop)
                    report("MSCNT arguments");
                DirectSink{real}.mscnt(op->lanes[0], op->lanes[1]);
                std::memcpy(shadow.m_vu1Data, real.m_vu1Data, PS2_VU1_DATA_SIZE);
            }
        }
        void mpg(uint32_t destAddr, const uint8_t *src, uint32_t n)
        {
            if (std::memcmp(shadow.m_vu1Code + destAddr, src, n) != 0)
                std::memcpy(shadow.m_vu1Code + destAddr, src, n);
            if (const Vif1Op *op = advance(Vif1OpKind::Mpg))
            {
                if (op->dest != destAddr || op->count != n || std::memcmp(op->ptr, src, n) != 0)
                    report("MPG");
                DirectSink{real}.mpg(op->dest, op->ptr, op->count);
                if (std::memcmp(real.m_vu1Code, shadow.m_vu1Code, PS2_VU1_CODE_SIZE) != 0)
                {
                    report("VU1 code memory");
                    std::memcpy(shadow.m_vu1Code, real.m_vu1Code, PS2_VU1_CODE_SIZE);
                }
            }
        }
        void copyVecs(uint32_t destVec, const uint8_t *src, uint32_t count) { DirectSink{shadow}.copyVecs(destVec, src, count); }
        void lanes(uint32_t destOff, const uint32_t *lanes, uint8_t mask) { DirectSink{shadow}.lanes(destOff, lanes, mask); }
        void bytes(uint32_t destOff, const uint8_t *src, uint32_t n) { DirectSink{shadow}.bytes(destOff, src, n); }
        uint32_t readLane(uint32_t destOff, uint32_t field) const { return DirectSink{shadow}.readLane(destOff, field); }
        // The rest of the list (memory writes only), then the end state.
        void finish()
        {
            while (cursor < list.ops.size())
            {
                const Vif1Op &op = list.ops[cursor];
                if (op.kind != Vif1OpKind::CopyVecs && op.kind != Vif1OpKind::Lanes && op.kind != Vif1OpKind::Bytes)
                {
                    report("extra op");
                    ps2xVif1ApplyOps(real, Vif1OpList{{op}, list.inlineBytes});
                }
                else
                    applyReal(op);
                ++cursor;
            }
            if (std::memcmp(real.m_vu1Data, shadow.m_vu1Data, PS2_VU1_DATA_SIZE) != 0)
                report("VU1 data memory at the end");
        }
    };

    template <class Sink>
    void vif1Process(PS2Memory &mem, const uint8_t *data, uint32_t sizeBytes, Sink &sink)
    {
        if (sizeBytes == 0u)
            return;

        VIFRegisters &vif1_regs = mem.vif1_regs;
        uint32_t pos = 0;
        VifDiag *diag = Sink::kDirect ? vifDiag() : nullptr;
        uint64_t diagItem = 0;
        if (diag)
        {
            diagItem = ++diag->itemCount;
            const uint32_t slot = static_cast<uint32_t>(diagItem % kVifDiagItems);
            diag->items[slot].assign(data, data + sizeBytes);
            diag->itemIds[slot] = diagItem;
        }

        while (pos + 4 <= sizeBytes)
        {
            if (diag)
            {
                VifDiagCmd &c = diag->cmds[diag->cmdCount++ & (kVifDiagRing - 1u)];
                c.item = diagItem;
                c.pos = pos;
                c.cmd = 0;
                if (pos + 4u <= sizeBytes)
                    std::memcpy(&c.cmd, data + pos, 4);
                c.size = sizeBytes;
                c.tops = static_cast<uint16_t>(vif1_regs.tops);
                c.base = static_cast<uint16_t>(vif1_regs.base);
                c.ofst = static_cast<uint16_t>(vif1_regs.ofst);
                c.itops = static_cast<uint16_t>(vif1_regs.itops);
                c.dbf = static_cast<uint8_t>((vif1_regs.stat >> 7) & 1u);
                if (mem.m_vif1PendingPath2ImageQwc != 0u)
                    c.dbf |= 0x80u;
            }
            if (mem.m_vif1PendingPath2ImageQwc != 0u)
            {
                const uint32_t availableQw = (sizeBytes - pos) / 16u;
                if (availableQw == 0u)
                {
                    break;
                }

                const uint32_t chunkQw = std::min<uint32_t>(mem.m_vif1PendingPath2ImageQwc, availableQw);
                std::vector<uint8_t> imagePacket(16u + static_cast<size_t>(chunkQw) * 16u, 0u);
                const uint64_t imageTag =
                    static_cast<uint64_t>(chunkQw & 0x7FFFu) |
                    ((mem.m_vif1PendingPath2ImageQwc == chunkQw) ? (1ull << 15) : 0ull) |
                    (static_cast<uint64_t>(kGifFmtImage) << 58);
                std::memcpy(imagePacket.data(), &imageTag, sizeof(imageTag));
                std::memcpy(imagePacket.data() + 16u, data + pos, static_cast<size_t>(chunkQw) * 16u);
                sink.path2(imagePacket.data(), static_cast<uint32_t>(imagePacket.size()), mem.m_vif1PendingPath2DirectHl, true);

                pos += chunkQw * 16u;
                mem.m_vif1PendingPath2ImageQwc -= chunkQw;
                if (mem.m_vif1PendingPath2ImageQwc == 0u)
                {
                    mem.m_vif1PendingPath2DirectHl = false;
                }
                continue;
            }

            uint32_t cmd;
            memcpy(&cmd, data + pos, 4);
            pos += 4;

            uint8_t opcode = (cmd >> 24) & 0x7F;
            uint16_t imm = cmd & 0xFFFF;
            uint8_t num = (cmd >> 16) & 0xFF;
            const bool irq = (cmd & 0x80000000u) != 0u;

            // Track most-recent command for VIFn_CODE emulation.
            vif1_regs.code = cmd;
            vif1_regs.num = num;
            if (irq)
                vif1_regs.stat |= (1u << 11); // INT

            if (opcode == VIF_NOP)
            {
                continue;
            }
            else if (opcode == VIF_STCYCL)
            {
                vif1_regs.cycle = imm;
                continue;
            }
            else if (opcode == VIF_OFFSET)
            {
                // VIF double-buffer setup. OFFSET clears DBF and resets TOPS to BASE.
                // Do not rewrite BASE from the previous TOPS value.
                vif1_regs.ofst = imm & 0x3FFu;
                vif1_regs.tops = vif1_regs.base & 0x3FFu;
                vif1_regs.stat &= ~(1u << 7); // clear DBF
                continue;
            }
            else if (opcode == VIF_BASE)
            {
                // BASE only updates the base register. TOPS changes on OFFSET/MSCAL.
                vif1_regs.base = imm & 0x3FFu;
                continue;
            }
            else if (opcode == VIF_ITOP)
            {
                // ITOP VIFcode writes pending ITOPS; VU XITOP observes it after MSCAL/MSCNT.
                vif1_regs.itops = imm & 0x3FFu;
                continue;
            }
            else if (opcode == VIF_STMOD)
            {
                vif1_regs.mode = imm & 3u;
                continue;
            }
            else if (opcode == VIF_MSKPATH3)
            {
                // VIF command docs: MSKPATH3 uses IMMEDIATE bit 15.
                sink.maskPath3((imm & 0x8000u) != 0u);
                continue;
            }
            else if (opcode == VIF_MARK)
            {
                vif1_regs.mark = imm;
                vif1_regs.stat |= (1u << 6); // MRK
                continue;
            }
            else if (opcode == VIF_FLUSHE || opcode == VIF_FLUSH || opcode == VIF_FLUSHA)
            {
                continue;
            }
            else if (opcode == VIF_MSCAL || opcode == VIF_MSCALF)
            {
                uint32_t startPC = (uint32_t)imm * 8u;
                if (vifTraceOn())
                {
                    static uint32_t s_mscalCount = 0u;
                    if (vifTraceSample(s_mscalCount))
                    {
                        // The VU1 memories belong to the VU1 thread in the two-thread mode: not read there.
                        uint32_t w0 = 0u, w1 = 0u;
                        uint64_t m3 = 0;
                        if (Sink::kDirect)
                        {
                            if (mem.m_vu1Code && startPC + 8u <= PS2_VU1_CODE_SIZE) { std::memcpy(&w0, mem.m_vu1Code + startPC, 4); std::memcpy(&w1, mem.m_vu1Code + startPC + 4, 4); }
                            if (mem.m_vu1Data) std::memcpy(&m3, mem.m_vu1Data + 0x30, 8);
                        }
                        std::fprintf(stderr, "[vif1] MSCAL #%u pc=0x%x code@pc=%08x %08x vu1data=%p mem[3].xy=%016llx\n", s_mscalCount, startPC, w0, w1, (void *)mem.m_vu1Data, (unsigned long long)m3);
                    }
                }

                const uint32_t runTop = vif1_regs.tops & 0x3FFu;
                const uint32_t runItop = vif1_regs.itops & 0x3FFu;
                vif1_regs.top = runTop;
                vif1_regs.itop = runItop;

                const bool dbf = (vif1_regs.stat & (1u << 7)) != 0u;
                if (dbf)
                    vif1_regs.tops = vif1_regs.base & 0x3FFu;
                else
                    vif1_regs.tops = (vif1_regs.base + vif1_regs.ofst) & 0x3FFu;
                vif1_regs.stat ^= (1u << 7); // toggle DBF

                sink.mscal(startPC, runTop, runItop);
                continue;
            }
            else if (opcode == VIF_MSCNT)
            {
                const uint32_t runTop = vif1_regs.tops & 0x3FFu;
                const uint32_t runItop = vif1_regs.itops & 0x3FFu;
                vif1_regs.top = runTop;
                vif1_regs.itop = runItop;

                const bool dbf = (vif1_regs.stat & (1u << 7)) != 0u;
                if (dbf)
                    vif1_regs.tops = vif1_regs.base & 0x3FFu;
                else
                    vif1_regs.tops = (vif1_regs.base + vif1_regs.ofst) & 0x3FFu;
                vif1_regs.stat ^= (1u << 7); // toggle DBF

                sink.mscnt(runTop, runItop);
                continue;
            }
            else if (opcode == VIF_STMASK)
            {
                if (pos + 4 > sizeBytes)
                    break;
                uint32_t maskValue = 0;
                std::memcpy(&maskValue, data + pos, sizeof(maskValue));
                vif1_regs.mask = maskValue;
                pos += 4;
                continue;
            }
            else if (opcode == VIF_STROW)
            {
                if (pos + 16 > sizeBytes)
                    break;
                std::memcpy(vif1_regs.row, data + pos, 16);
                pos += 16;
                continue;
            }
            else if (opcode == VIF_STCOL)
            {
                if (pos + 16 > sizeBytes)
                    break;
                std::memcpy(vif1_regs.col, data + pos, 16);
                pos += 16;
                continue;
            }
            else if (opcode == VIF_MPG)
            {
                uint32_t destAddr = (uint32_t)imm * 8u;
                const uint32_t instructionCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
                const uint32_t mpgBytes = instructionCount * 8u;
                if (vifTraceOn())
                {
                    static uint32_t s_mpgCount = 0u;
                    if (vifTraceSample(s_mpgCount))
                    {
                        uint32_t w0 = 0u, w1 = 0u;
                        if (pos + 8u <= sizeBytes) { std::memcpy(&w0, data + pos, 4); std::memcpy(&w1, data + pos + 4, 4); }
                        std::fprintf(stderr, "[vif1] MPG #%u dest=0x%x insns=%u pos=%u bufsize=%u fits=%d first=%08x %08x\n",
                                     s_mpgCount, destAddr, instructionCount, pos, sizeBytes, (pos + mpgBytes <= sizeBytes) ? 1 : 0, w0, w1);
                    }
                }
                if (sink.hasVuCode() && destAddr < PS2_VU1_CODE_SIZE && mpgBytes > 0)
                {
                    uint32_t copyBytes = mpgBytes;
                    if (destAddr + copyBytes > PS2_VU1_CODE_SIZE)
                        copyBytes = PS2_VU1_CODE_SIZE - destAddr;
                    if (pos + copyBytes <= sizeBytes)
                        sink.mpg(destAddr, data + pos, copyBytes);
                }
                pos += mpgBytes;
                if (pos > sizeBytes)
                    break;
                continue;
            }
            else if (opcode == VIF_DIRECT || opcode == VIF_DIRECTHL)
            {
                uint32_t qwCount = imm;
                if (qwCount == 0)
                    qwCount = 65536;
                const uint32_t availableQw = (sizeBytes - pos) / 16u;
                const bool truncated = qwCount > availableQw;
                if (qwCount > availableQw)
                    qwCount = availableQw;

                if (qwCount > 0)
                {
                    const bool directHl = (opcode == VIF_DIRECTHL);
                    sink.path2(data + pos, qwCount * 16, directHl, false);

                    const uint32_t pendingImageQw = pendingGifImageQwc(data + pos, qwCount * 16u);
                    if (pendingImageQw != 0u)
                    {
                        mem.m_vif1PendingPath2ImageQwc = pendingImageQw;
                        mem.m_vif1PendingPath2DirectHl = directHl;
                    }
                }

                pos += qwCount * 16;
                if (truncated)
                {
                    pos = sizeBytes;
                    break;
                }
                continue;
            }
            else if ((opcode & 0x60) == 0x60)
            {
                uint8_t vn = (opcode >> 2) & 0x3;
                uint8_t vl = opcode & 0x3;
                const bool maskEnable = (opcode & 0x10u) != 0u;
                int components = vn + 1;
                int bitsPerComponent = 32;
                switch (vl)
                {
                case 0:
                    bitsPerComponent = 32;
                    break;
                case 1:
                    bitsPerComponent = 16;
                    break;
                case 2:
                    bitsPerComponent = 8;
                    break;
                case 3:
                    bitsPerComponent = (vn == 3) ? 4 : 16;
                    break;
                default:
                    break;
                }
                int bitsPerVector = (vl == 3 && vn == 3) ? 16 : (components * bitsPerComponent);
                uint32_t bytesPerVector = (bitsPerVector + 7) / 8;
                // UNPACK semantics: NUM is 8-bit and NUM==0 means 256 vectors (writes).
                const uint32_t writeVectorCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);

                // STCYCL controls write cycles for UNPACK.
                uint32_t cl = vif1_regs.cycle & 0xFFu;
                uint32_t wl = (vif1_regs.cycle >> 8) & 0xFFu;
                if (cl == 0u)
                    cl = 1u;
                if (wl == 0u)
                    wl = 1u;

                uint32_t sourceVectorCount = writeVectorCount;
                if (cl < wl)
                {
                    const uint32_t fullBlocks = writeVectorCount / wl;
                    uint32_t remainder = writeVectorCount % wl;
                    if (remainder > cl)
                        remainder = cl;
                    sourceVectorCount = fullBlocks * cl + remainder;
                }

                uint32_t totalBytes = sourceVectorCount * bytesPerVector;
                totalBytes = (totalBytes + 3) & ~3u;

                uint32_t vuAddr = (uint32_t)imm & 0x3FFu;
                if ((imm & 0x8000u) != 0u)
                    vuAddr = (vuAddr + (vif1_regs.tops & 0x3FFu)) & 0x3FFu;

                const bool zeroExtend = (imm & 0x4000u) != 0u;

                if (vifTraceOn())
                {
                    static uint32_t s_unpackCount = 0u;
                    const bool low = vuAddr < 0x10u;
                    if (low || vifTraceSample(s_unpackCount))
                        std::fprintf(stderr, "[vif1] UNPACK #%u vn=%u vl=%u num=%u addr=0x%x flg=%d usn=%d mask=%d cl=%u wl=%u mode=%u pos=%u bytes=%u bufsize=%u fits=%d\n",
                                     s_unpackCount, vn, vl, writeVectorCount, vuAddr, (imm & 0x8000u) ? 1 : 0, zeroExtend ? 1 : 0, maskEnable ? 1 : 0,
                                     cl, wl, vif1_regs.mode & 3u, pos, totalBytes, sizeBytes, (pos + totalBytes <= sizeBytes) ? 1 : 0);
                }

                // Fast path for the common case: no write mask, MODE 0 (no row add) and no skipping writes
                // (CL >= WL, so every write has a source vector). The generic loop below then writes exactly
                // the decoded vector, with the old lanes kept for components the format does not supply.
                // PS2X_VIF_VERIFY=1 (serial interpreter only): also run the generic loop and compare VU1 data memory.
                static const bool verifyFast = [] { const char *e = std::getenv("PS2X_VIF_VERIFY"); return e && *e == '1'; }();
                const bool verify = Sink::kDirect && verifyFast;
                const bool fastOk = sink.hasVuData() && totalBytes > 0 && pos + totalBytes <= sizeBytes &&
                                    !maskEnable && (vif1_regs.mode & 3u) == 0u && cl >= wl && (vl != 3u || vn == 3u);
                static std::vector<uint8_t> verifyBefore, verifyFastResult;
                if (fastOk && verify)
                    verifyBefore.assign(mem.m_vu1Data, mem.m_vu1Data + PS2_VU1_DATA_SIZE);
                if (fastOk)
                {
                    const uint8_t *srcBase = data + pos;
                    if (vl == 0u && vn == 3u && cl == wl)
                    {
                        // V4-32 to consecutive vectors: a copy (wrapping at the end of VU1 data memory).
                        uint32_t done = 0u;
                        while (done < writeVectorCount)
                        {
                            const uint32_t destVec = (vuAddr + done) & 0x3FFu;
                            const uint32_t run = std::min(writeVectorCount - done, 0x400u - destVec);
                            sink.copyVecs(destVec, srcBase + done * 16u, run);
                            done += run;
                        }
                    }
                    else
                    {
                        // Lanes the format does not supply keep their old value (not written).
                        const uint8_t writeMask = (vl == 3u || components == 1) ? 0xFu : static_cast<uint8_t>((1u << components) - 1u);
                        for (uint32_t writeIndex = 0; writeIndex < writeVectorCount; ++writeIndex)
                        {
                            const uint32_t cyclePos = writeIndex % wl;
                            const uint32_t destVec = (vuAddr + (writeIndex / wl) * cl + cyclePos) & 0x3FFu;
                            const uint8_t *srcVec = srcBase + writeIndex * bytesPerVector;
                            uint32_t lanes[4] = {0u, 0u, 0u, 0u};
                            if (vl == 3u) // V4-5
                            {
                                uint16_t packed = 0;
                                std::memcpy(&packed, srcVec, sizeof(packed));
                                lanes[0] = (packed & 0x1Fu) << 3;
                                lanes[1] = ((packed >> 5) & 0x1Fu) << 3;
                                lanes[2] = ((packed >> 10) & 0x1Fu) << 3;
                                lanes[3] = ((packed >> 15) & 0x01u) << 7;
                            }
                            else
                            {
                                uint32_t value[4];
                                for (int c = 0; c < components; ++c)
                                {
                                    if (vl == 0u)
                                        std::memcpy(&value[c], srcVec + c * 4u, 4u);
                                    else if (vl == 1u)
                                    {
                                        uint16_t raw = 0;
                                        std::memcpy(&raw, srcVec + c * 2u, sizeof(raw));
                                        value[c] = zeroExtend ? raw : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(raw)));
                                    }
                                    else
                                    {
                                        const uint8_t raw = srcVec[c];
                                        value[c] = zeroExtend ? raw : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(raw)));
                                    }
                                }
                                if (components == 1)
                                    lanes[0] = lanes[1] = lanes[2] = lanes[3] = value[0];
                                else
                                    for (int c = 0; c < components; ++c)
                                        lanes[c] = value[c];
                            }
                            sink.lanes(destVec * 16u, lanes, writeMask);
                        }
                    }
                    if (verify)
                    {
                        verifyFastResult.assign(mem.m_vu1Data, mem.m_vu1Data + PS2_VU1_DATA_SIZE);
                        std::memcpy(mem.m_vu1Data, verifyBefore.data(), PS2_VU1_DATA_SIZE);
                    }
                }
                if ((!fastOk || verify) && sink.hasVuData() && totalBytes > 0 && pos + totalBytes <= sizeBytes)
                {
                    const uint8_t *srcBase = data + pos;
                    uint32_t srcIndex = 0u;
                    for (uint32_t writeIndex = 0; writeIndex < writeVectorCount; ++writeIndex)
                    {
                        const uint32_t cyclePos = writeIndex % wl;
                        const bool sourceAvailable = (cl >= wl) || (cyclePos < cl);

                        uint32_t destVec = 0;
                        if (cl >= wl)
                        {
                            destVec = (vuAddr + (writeIndex / wl) * cl + cyclePos) & 0x3FFu;
                        }
                        else
                        {
                            destVec = (vuAddr + writeIndex) & 0x3FFu;
                        }

                        uint32_t destOff = destVec * 16u;
                        if (destOff + 16u > PS2_VU1_DATA_SIZE)
                        {
                            if (sourceAvailable && srcIndex < sourceVectorCount)
                                ++srcIndex;
                            continue;
                        }

                        // lanes[] holds only what this write puts in a lane (`written` bits); the other lanes
                        // keep VU1 memory as it is.
                        uint32_t lanes[4] = {0u, 0u, 0u, 0u};
                        uint8_t written = 0u;
                        uint32_t decompressed[4] = {0u, 0u, 0u, 0u};
                        bool decoded = false;

                        const uint8_t *srcVec = nullptr;
                        if (sourceAvailable && srcIndex < sourceVectorCount)
                        {
                            srcVec = srcBase + srcIndex * bytesPerVector;
                            ++srcIndex;
                            decoded = true;
                        }

                        auto extend16 = [&](uint16_t raw) -> uint32_t
                        {
                            if (zeroExtend)
                                return static_cast<uint32_t>(raw);
                            return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(raw)));
                        };

                        auto extend8 = [&](uint8_t raw) -> uint32_t
                        {
                            if (zeroExtend)
                                return static_cast<uint32_t>(raw);
                            return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(raw)));
                        };

                        // decompressedSet: the lanes the format supplies (the rest keep the old lane, as before).
                        uint8_t decompressedSet = 0u;
                        bool handledFormat = true;
                        if (!decoded)
                        {
                            handledFormat = false;
                        }
                        else if (vl == 0u)
                        {
                            if (components == 1)
                            {
                                uint32_t scalar = 0;
                                std::memcpy(&scalar, srcVec, sizeof(scalar));
                                decompressed[0] = scalar;
                                decompressed[1] = scalar;
                                decompressed[2] = scalar;
                                decompressed[3] = scalar;
                                decompressedSet = 0xFu;
                            }
                            else
                            {
                                const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                                for (uint32_t c = 0; c < limit; ++c)
                                {
                                    uint32_t scalar = 0;
                                    std::memcpy(&scalar, srcVec + c * 4u, sizeof(scalar));
                                    decompressed[c] = scalar;
                                    decompressedSet |= static_cast<uint8_t>(1u << c);
                                }
                            }
                        }
                        else if (vl == 1u)
                        {
                            if (components == 1)
                            {
                                uint16_t raw = 0;
                                std::memcpy(&raw, srcVec, sizeof(raw));
                                const uint32_t scalar = extend16(raw);
                                decompressed[0] = scalar;
                                decompressed[1] = scalar;
                                decompressed[2] = scalar;
                                decompressed[3] = scalar;
                                decompressedSet = 0xFu;
                            }
                            else
                            {
                                const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                                for (uint32_t c = 0; c < limit; ++c)
                                {
                                    uint16_t raw = 0;
                                    std::memcpy(&raw, srcVec + c * 2u, sizeof(raw));
                                    decompressed[c] = extend16(raw);
                                    decompressedSet |= static_cast<uint8_t>(1u << c);
                                }
                            }
                        }
                        else if (vl == 2u)
                        {
                            if (components == 1)
                            {
                                const uint32_t scalar = extend8(srcVec[0]);
                                decompressed[0] = scalar;
                                decompressed[1] = scalar;
                                decompressed[2] = scalar;
                                decompressed[3] = scalar;
                                decompressedSet = 0xFu;
                            }
                            else
                            {
                                const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                                for (uint32_t c = 0; c < limit; ++c)
                                {
                                    decompressed[c] = extend8(srcVec[c]);
                                    decompressedSet |= static_cast<uint8_t>(1u << c);
                                }
                            }
                        }
                        else if (vl == 3u && vn == 3u)
                        {
                            // V4-5: packed color-like format in a single 16-bit value.
                            uint16_t packed = 0;
                            std::memcpy(&packed, srcVec, sizeof(packed));
                            // Hardware expands RGBA5551 to 8-bit lanes: R,G,B << 3, A << 7.
                            decompressed[0] = (packed & 0x1Fu) << 3;
                            decompressed[1] = ((packed >> 5) & 0x1Fu) << 3;
                            decompressed[2] = ((packed >> 10) & 0x1Fu) << 3;
                            decompressed[3] = ((packed >> 15) & 0x01u) << 7;
                            decompressedSet = 0xFu;
                        }
                        else
                        {
                            handledFormat = false;
                        }

                        // Unknown compressed format fallback: preserve legacy raw-copy behavior.
                        if (!handledFormat && decoded && !maskEnable && (vif1_regs.mode == 0u || vif1_regs.mode == 3u))
                        {
                            uint32_t copyBytes = (bytesPerVector < 16u) ? bytesPerVector : 16u;
                            sink.bytes(destOff, srcVec, copyBytes);
                            continue;
                        }

                        const bool canAdd = (vl != 3u || vn != 3u);
                        const uint32_t mode = vif1_regs.mode & 3u;
                        const uint32_t colIdx = (cyclePos > 3u) ? 3u : cyclePos;
                        const uint32_t maskCycle = (cyclePos > 3u) ? 3u : cyclePos;

                        for (uint32_t field = 0u; field < 4u; ++field)
                        {
                            uint32_t maskSpec = 0u;
                            if (maskEnable)
                            {
                                const uint32_t shift = ((maskCycle * 4u) + field) * 2u;
                                maskSpec = (vif1_regs.mask >> shift) & 0x3u;
                            }

                            // In fill-write cycles with suspended source reads, treat raw-data selections as row-fill.
                            if (!decoded && maskSpec == 0u)
                                maskSpec = 1u;

                            if (maskSpec == 0u)
                            {
                                // A lane the format does not supply keeps its old value; with MODE 1/2 the row is
                                // added to that old value (and MODE 2 stores the sum in the row register), the one
                                // case where UNPACK reads VU1 memory.
                                const bool supplied = ((decompressedSet >> field) & 1u) != 0u;
                                if (handledFormat && (supplied || (canAdd && (mode == 1u || mode == 2u))))
                                {
                                    uint32_t writeVal = supplied ? decompressed[field] : sink.readLane(destOff, field);
                                    if (canAdd && (mode == 1u || mode == 2u))
                                    {
                                        writeVal = writeVal + vif1_regs.row[field];
                                        if (mode == 2u)
                                            vif1_regs.row[field] = writeVal;
                                    }
                                    lanes[field] = writeVal;
                                    written |= static_cast<uint8_t>(1u << field);
                                }
                            }
                            else if (maskSpec == 1u)
                            {
                                lanes[field] = vif1_regs.row[field];
                                written |= static_cast<uint8_t>(1u << field);
                            }
                            else if (maskSpec == 2u)
                            {
                                lanes[field] = vif1_regs.col[colIdx];
                                written |= static_cast<uint8_t>(1u << field);
                            }
                            // maskSpec 3: write-protect
                        }

                        sink.lanes(destOff, lanes, written);
                    }
                }
                if (fastOk && verify)
                {
                    static uint64_t s_checked = 0, s_bad = 0;
                    ++s_checked;
                    if (std::memcmp(verifyFastResult.data(), mem.m_vu1Data, PS2_VU1_DATA_SIZE) != 0 && ++s_bad <= 20)
                        std::fprintf(stderr, "[vif-verify] MISMATCH vn=%u vl=%u num=%u cl=%u wl=%u usn=%d\n", vn, vl, writeVectorCount, cl, wl, zeroExtend ? 1 : 0);
                    if ((s_checked & (s_checked - 1)) == 0)
                        std::fprintf(stderr, "[vif-verify] %llu fast UNPACKs checked, %llu mismatches\n", (unsigned long long)s_checked, (unsigned long long)s_bad);
                }
                pos += totalBytes;

                if (pos > sizeBytes)
                    break;
                continue;
            }
            else
            {
                continue;
            }
        }
    }
}

void PS2Memory::processVIF1Data(const uint8_t *data, uint32_t sizeBytes)
{
    DirectSink sink{*this};
    vif1Process(*this, data, sizeBytes, sink);
}

Vif1FrontState ps2xVif1FrontState(const PS2Memory &mem)
{
    return Vif1FrontState{mem.vif1_regs, mem.m_vif1PendingPath2ImageQwc, mem.m_vif1PendingPath2DirectHl};
}

void ps2xVif1DecodeOps(PS2Memory &mem, const uint8_t *data, uint32_t sizeBytes, Vif1OpList &out, Vif1DecodeHooks &hooks)
{
    OpSink sink{mem, out, hooks};
    vif1Process(mem, data, sizeBytes, sink);
}

void ps2xVif1ApplyOps(PS2Memory &mem, const Vif1OpList &list)
{
    DirectSink sink{mem};
    for (const Vif1Op &op : list.ops)
    {
        switch (op.kind)
        {
        case Vif1OpKind::CopyVecs:
            sink.copyVecs(op.dest, op.ptr, op.count);
            break;
        case Vif1OpKind::Lanes:
            applyLaneRun(mem.m_vu1Data, list.bytes(op), op.count);
            break;
        case Vif1OpKind::Bytes:
            sink.bytes(op.dest, op.ptr, op.count);
            break;
        case Vif1OpKind::Path2:
            sink.path2(list.bytes(op), op.count, op.mask != 0u, false);
            break;
        case Vif1OpKind::MaskPath3:
            sink.maskPath3(op.mask != 0u);
            break;
        case Vif1OpKind::Mscal:
            sink.mscal(op.dest, op.lanes[0], op.lanes[1]);
            break;
        case Vif1OpKind::Mscnt:
            sink.mscnt(op.lanes[0], op.lanes[1]);
            break;
        case Vif1OpKind::Mpg:
            sink.mpg(op.dest, op.ptr, op.count);
            break;
        }
    }
}

uint32_t ps2xVif1VerifyOps(PS2Memory &mem, const uint8_t *data, uint32_t sizeBytes, const Vif1OpList &list,
                           const Vif1FrontState &start, const Vif1FrontState &end)
{
    // One shadow, never freed (PS2Memory's destructor would free the VU buffers it does not own).
    static PS2Memory *shadow = []
    {
        auto *m = new PS2Memory();
        m->m_vu1Data = new uint8_t[PS2_VU1_DATA_SIZE];
        m->m_vu1Code = new uint8_t[PS2_VU1_CODE_SIZE];
        return m;
    }();
    shadow->vif1_regs = start.regs;
    shadow->m_vif1PendingPath2ImageQwc = start.pendingImageQwc;
    shadow->m_vif1PendingPath2DirectHl = start.pendingDirectHl;
    std::memcpy(shadow->m_vu1Data, mem.m_vu1Data, PS2_VU1_DATA_SIZE);
    std::memcpy(shadow->m_vu1Code, mem.m_vu1Code, PS2_VU1_CODE_SIZE);
    VerifySink sink{*shadow, mem, list};
    vif1Process(*shadow, data, sizeBytes, sink);
    sink.finish();
    if (std::memcmp(&shadow->vif1_regs, &end.regs, sizeof(VIFRegisters)) != 0 ||
        shadow->m_vif1PendingPath2ImageQwc != end.pendingImageQwc || shadow->m_vif1PendingPath2DirectHl != end.pendingDirectHl)
        sink.report("VIF1 registers at the end");
    return sink.mismatches;
}

// PS2X_VU1_BADKICK: writes the VIF1 command ring and the last transfers to <dir> (called once, at the first bad XGKICK).
void ps2xVif1DiagDump(const char *dir)
{
    VifDiag *d = vifDiag();
    if (!d || !dir)
        return;
    std::string base(dir);
    if (std::FILE *f = std::fopen((base + "/vifcmds.txt").c_str(), "w"))
    {
        const uint64_t n = std::min<uint64_t>(d->cmdCount, uint64_t{kVifDiagRing});
        for (uint64_t i = d->cmdCount - n; i < d->cmdCount; ++i)
        {
            const VifDiagCmd &c = d->cmds[i & (kVifDiagRing - 1u)];
            std::fprintf(f, "item %llu size %u pos %u cmd %08x op %02x num %u imm %04x | tops %x base %x ofst %x itops %x dbf %u%s\n",
                         (unsigned long long)c.item, c.size, c.pos, c.cmd, (c.cmd >> 24) & 0x7Fu, (c.cmd >> 16) & 0xFFu, c.cmd & 0xFFFFu,
                         c.tops, c.base, c.ofst, c.itops, c.dbf & 1u, (c.dbf & 0x80u) ? " PATH2-IMAGE-PENDING" : "");
        }
        std::fclose(f);
    }
    for (uint32_t s = 0; s < kVifDiagItems; ++s)
    {
        if (d->items[s].empty())
            continue;
        char name[64];
        std::snprintf(name, sizeof(name), "/item_%llu.bin", (unsigned long long)d->itemIds[s]);
        if (std::FILE *f = std::fopen((base + name).c_str(), "wb"))
        {
            std::fwrite(d->items[s].data(), 1, d->items[s].size(), f);
            std::fclose(f);
        }
    }
}
