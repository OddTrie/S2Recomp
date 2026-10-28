// Instruction decoding for the VU core, constexpr so that generated VU1 programs (vu1gen/) decode
// their instruction pairs at compile time with the very same code the interpreter uses at run time.
// Included at the end of ps2_vu1_core.h.
#include <bit>

namespace vu1detail
{
    constexpr uint8_t laneForComponent(uint32_t component)
    {
        return static_cast<uint8_t>(1u << (3u - component));
    }

    // normalizeFmacResult's view of an upper instruction, decoded once per pair:
    // bit 0 product-sum (acc +/- a*b, incl. OPMSUB), bit 1 product-sum with a plain right operand
    // (eligible for the fast flag path), bits 2-3 right operand (0 vt[lane], 1 vt[bc], 2 Q, 3 I),
    // bits 4-5 the bc lane.
    constexpr uint8_t kFmacNormProductSum = 1u, kFmacNormProductSumFast = 2u;
    constexpr uint8_t computeFmacNorm(uint32_t upper)
    {
        const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
        const uint8_t special = op >= 0x3Cu ? static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu)) : 0xFFu;
        const bool productSum =
            (op >= 0x08u && op <= 0x0Fu) ||
            op == 0x21u || op == 0x23u || op == 0x25u || op == 0x27u ||
            op == 0x29u || op == 0x2Du || op == 0x2Eu ||
            (special >= 0x08u && special <= 0x0Fu) ||
            special == 0x21u || special == 0x23u || special == 0x25u ||
            special == 0x27u || special == 0x29u || special == 0x2Du || special == 0x2Eu;
        uint8_t norm = 0u;
        if (productSum)
            norm |= kFmacNormProductSum;
        if (productSum && op != 0x2Eu && special != 0x2Eu)
            norm |= kFmacNormProductSumFast;
        uint8_t right = 0u;
        if ((op >= 0x08u && op <= 0x0Fu) || (special >= 0x08u && special <= 0x0Fu))
            right = 1u;
        else if (op == 0x21u || op == 0x25u || special == 0x21u || special == 0x25u)
            right = 2u;
        else if (op == 0x23u || op == 0x27u || special == 0x23u || special == 0x27u)
            right = 3u;
        norm |= static_cast<uint8_t>(right << 2);
        norm |= static_cast<uint8_t>((((op >= 0x08u && op <= 0x0Fu) ? op : special) & 3u) << 4);
        return norm;
    }

    // Single add/sub/mul FMAC ops whose float result is exactly +/-0 (operands are normalized, so
    // never denormal): for add/sub the exact sum is then zero too, with the same sign; a product
    // is exactly zero iff an operand is zero, again with the float's sign. Then the flags are just
    // Z (+S) and the value stays, so the exact recomputation is not needed.
    // bits 0-1: 0 none, 1 add/sub, 2 mul; bits 2-3 mul right operand (0 vt[lane], 1 vt[bc], 2 Q,
    // 3 I); bits 4-5 the bc lane.
    constexpr uint8_t kFmacZeroAddSub = 1u, kFmacZeroMul = 2u;
    constexpr uint8_t computeFmacZero(uint32_t upper)
    {
        const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
        // Same op numbering for the main table and the ...A forms in the special table.
        const uint8_t code = op >= 0x3Cu ? static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu)) : op;
        if (code <= 0x07u || code == 0x20u || code == 0x22u || code == 0x24u || code == 0x26u || code == 0x28u || code == 0x2Cu)
            return kFmacZeroAddSub;
        if (code >= 0x18u && code <= 0x1Bu)
            return static_cast<uint8_t>(kFmacZeroMul | (1u << 2) | ((code & 3u) << 4));
        if (code == 0x1Cu)
            return static_cast<uint8_t>(kFmacZeroMul | (2u << 2));
        if (code == 0x1Eu)
            return static_cast<uint8_t>(kFmacZeroMul | (3u << 2));
        if (code == 0x2Au)
            return kFmacZeroMul;
        return 0u;
    }

    // For skipping operand normalization (VU1_UPPER_OPERANDS) in generated blocks: the lanes (VU lane
    // bits) of fs, ft and ACC that an upper instruction's written lanes and flags depend on, and
    // whether every lane it writes is normalized (normalizeLanes4 leaves it as is: exponent 1..254,
    // or +/-0) when those operand lanes were. FMAC ops: applyFmacDestFast's paths normalize or keep
    // exponent 2..253 / exact zeros; its out-of-line exact path can keep a denormal product-sum
    // result (two roundings), which kNormBreak handles. MAX/MINI pick an operand, ABS clears its
    // sign, ITOF converts an integer (exact, never denormal); FTOI writes integer bits.
    // Whether an upper instruction's handler reads m_currentFlagsLive (applyFmacDestFast and the exact
    // applyFmacDest behind it): every FMAC result op; not MAX/MINI, ITOF/FTOI, ABS, CLIP, NOP.
    constexpr bool upperReadsFlagsLive(uint32_t upper)
    {
        const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
        if (op < 0x3Cu)
            return !((op >= 0x10u && op <= 0x17u) || op == 0x1Du || op == 0x1Fu || op == 0x2Bu || op == 0x2Fu);
        const uint8_t code = static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu));
        return !((code >= 0x10u && code <= 0x17u) || code == 0x1Du || code == 0x1Fu || code == 0x2Fu || code == 0x30u);
    }

    // kFm bits 16+ (VU1_UPPER_OPERANDS): two bits per operand at these shifts, bit 0 = the lanes the
    // instruction depends on are known normalized, bit 1 = all four lanes are.
    constexpr uint32_t kNormShiftFs = 0u, kNormShiftFt = 2u, kNormShiftAcc = 4u;
    struct UpperNormInfo
    {
        uint8_t fsLanes = 0, ftLanes = 0, accLanes = 0;
        bool normalizedOutput = false;
    };
    constexpr UpperNormInfo upperNormInfo(uint32_t upper)
    {
        const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
        const uint8_t dest = static_cast<uint8_t>((upper >> 21) & 0xFu);
        const bool special = op >= 0x3Cu;
        const uint8_t code = special ? static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu)) : op;
        UpperNormInfo info;
        info.fsLanes = dest;
        info.ftLanes = dest;
        info.accLanes = dest;
        const bool bcForm = special ? (code <= 0x0Fu || (code >= 0x18u && code <= 0x1Bu)) : code <= 0x1Bu;
        if (bcForm)
            info.ftLanes = laneForComponent(code & 3u);
        if (code == 0x2Eu) // OPMSUB / OPMULA: crossed lanes
        {
            info.fsLanes = 0xFu;
            info.ftLanes = 0xFu;
        }
        if (!special)
            info.normalizedOutput = code <= 0x2Fu;
        else
            info.normalizedOutput = code <= 0x13u || (code >= 0x18u && code <= 0x1Eu) || (code >= 0x20u && code <= 0x2Au) ||
                                    (code >= 0x2Cu && code <= 0x2Eu);
        return info;
    }

}

using vu1detail::computeFmacNorm;
using vu1detail::computeFmacZero;
using vu1detail::laneForComponent;

constexpr void VU1Core::addVfRead(InstructionUsage &usage, uint8_t reg, uint8_t lanes)
{
    if (lanes == 0u)
        return;
    for (uint32_t index = 0; index < usage.vfReadCount; ++index)
    {
        if (usage.vfRead[index].reg == reg)
        {
            usage.vfRead[index].lanes |= lanes;
            return;
        }
    }
    if (usage.vfReadCount < usage.vfRead.size())
        usage.vfRead[usage.vfReadCount++] = {reg, lanes};
}

constexpr void VU1Core::addVfWrite(InstructionUsage &usage, uint8_t reg, uint8_t lanes)
{
    if (reg == 0u && lanes != 0u)
        usage.writesZeroReg = true;
    if (reg == 0u || lanes == 0u)
        return;
    if (usage.vfWrite.reg == 0u)
        usage.vfWrite = {reg, lanes};
    else if (usage.vfWrite.reg == reg)
        usage.vfWrite.lanes |= lanes;
}

constexpr uint8_t VU1Core::vfReadLanes(const InstructionUsage &usage, uint8_t reg)
{
    for (uint32_t index = 0; index < usage.vfReadCount; ++index)
    {
        if (usage.vfRead[index].reg == reg)
            return usage.vfRead[index].lanes;
    }
    return 0u;
}

constexpr VU1Core::InstructionUsage VU1Core::decodeUpperUsage(uint32_t upper)
{
    InstructionUsage usage;
    usage.pipeline = PipelineFmac;
    usage.latency = kFmacLatency;

    const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
    const uint8_t dest = DEST(upper);
    const uint8_t fs = FS(upper);
    const uint8_t ft = FT(upper);
    const uint8_t fd = FD(upper);

    if (op <= 0x2Fu)
    {
        addVfRead(usage, fs, dest);
        addVfWrite(usage, fd, dest);
        if (op <= 0x1Bu)
            addVfRead(usage, ft, laneForComponent(op & 3u));
        else if (op >= 0x28u)
            addVfRead(usage, ft, op == 0x2Eu ? 0xEu : dest);
        if (op == 0x08u || op == 0x09u || op == 0x0Au || op == 0x0Bu ||
            op == 0x0Cu || op == 0x0Du || op == 0x0Eu || op == 0x0Fu ||
            op == 0x21u || op == 0x23u || op == 0x25u || op == 0x27u ||
            op == 0x29u || op == 0x2Du || op == 0x2Eu)
        {
            usage.accRead = dest;
        }
        return usage;
    }

    if (op >= 0x3Cu)
    {
        const uint8_t special = static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu));
        const bool writesAcc =
            special <= 0x0Fu ||
            (special >= 0x18u && special <= 0x1Cu) ||
            special == 0x1Eu ||
            (special >= 0x20u && special <= 0x2Au) ||
            (special >= 0x2Cu && special <= 0x2Eu);
        if (writesAcc)
        {
            addVfRead(usage, fs, dest);
            if (special <= 0x1Bu)
                addVfRead(usage, ft, laneForComponent(special & 3u));
            else if ((special >= 0x28u && special <= 0x2Eu))
                addVfRead(usage, ft, special == 0x2Eu ? 0xEu : dest);
            usage.accWrite = dest;
            if ((special >= 0x08u && special <= 0x0Fu) ||
                special == 0x21u || special == 0x23u || special == 0x25u ||
                special == 0x27u || special == 0x29u || special == 0x2Du)
            {
                usage.accRead = dest;
            }
        }
        else if (special >= 0x10u && special <= 0x17u)
        {
            addVfRead(usage, fs, dest);
            addVfWrite(usage, ft, dest);
        }
        else if (special == 0x1Du)
        {
            addVfRead(usage, fs, dest);
            addVfWrite(usage, ft, dest);
        }
        else if (special == 0x1Fu)
        {
            addVfRead(usage, fs, 0xEu);
            addVfRead(usage, ft, 0x1u);
            usage.writesClip = true;
        }
        else if (special != 0x2Fu && special != 0x30u)
        {
            usage.reserved = true;
        }
        return usage;
    }

    usage.reserved = true;
    return usage;
}

constexpr VU1Core::InstructionUsage VU1Core::decodeLowerUsage(uint32_t lower, Unit unit)
{
    InstructionUsage usage;
    if (lower == 0u || lower == 0x8000033Cu)
        return usage;

    const uint8_t opHi = static_cast<uint8_t>((lower >> 25) & 0x7Fu);
    const uint8_t vfT = FT(lower);
    const uint8_t vfS = FS(lower);
    const uint8_t viT = VIT(lower);
    const uint8_t viS = VIS(lower);
    const uint8_t viD = VID(lower);
    const uint8_t dest = DEST(lower);
    auto readVi = [&](uint8_t reg)
    {
        if (reg != 0u)
            usage.viRead |= static_cast<uint16_t>(1u << reg);
    };
    auto writeVi = [&](uint8_t reg)
    {
        if (reg != 0u)
            usage.viWrite |= static_cast<uint16_t>(1u << reg);
        else
            usage.writesZeroReg = true;
    };

    switch (opHi)
    {
    case 0x00:
        usage.pipeline = PipelineLsu;
        usage.latency = 4u;
        readVi(viS);
        addVfWrite(usage, vfT, dest);
        return usage;
    case 0x01:
        usage.pipeline = PipelineLsu;
        usage.latency = 1u;
        readVi(viT);
        addVfRead(usage, vfS, dest);
        return usage;
    case 0x04:
        usage.pipeline = PipelineLsu;
        usage.latency = 4u;
        readVi(viS);
        writeVi(viT);
        return usage;
    case 0x05:
        usage.pipeline = PipelineLsu;
        usage.latency = 1u;
        readVi(viS);
        readVi(viT);
        return usage;
    case 0x08:
    case 0x09:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viS);
        writeVi(viT);
        return usage;
    case 0x10:
    case 0x12:
    case 0x13:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.readsClip = true;
        writeVi(1u);
        return usage;
    case 0x11:
        usage.pipeline = PipelineFmac;
        usage.latency = kFmacLatency;
        usage.writesClip = true;
        return usage;
    case 0x14:
    case 0x16:
    case 0x17:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        writeVi(viT);
        return usage;
    case 0x15:
        usage.pipeline = PipelineFmac;
        usage.latency = kFmacLatency;
        return usage;
    case 0x18:
    case 0x1A:
    case 0x1B:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        readVi(viS);
        writeVi(viT);
        return usage;
    case 0x1C:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.readsClip = true;
        writeVi(viT);
        return usage;
    case 0x20:
        usage.pipeline = PipelineBranch;
        return usage;
    case 0x21:
        usage.pipeline = PipelineBranch;
        usage.latency = 1u;
        writeVi(viT);
        return usage;
    case 0x24:
        usage.pipeline = PipelineBranch;
        readVi(viS);
        return usage;
    case 0x25:
        usage.pipeline = PipelineBranch;
        usage.latency = 1u;
        readVi(viS);
        writeVi(viT);
        return usage;
    case 0x28:
    case 0x29:
        usage.pipeline = PipelineBranch;
        readVi(viS);
        readVi(viT);
        return usage;
    case 0x2C:
    case 0x2D:
    case 0x2E:
    case 0x2F:
        usage.pipeline = PipelineBranch;
        readVi(viS);
        return usage;
    case 0x40:
        break;
    default:
        usage.reserved = true;
        return usage;
    }

    const uint8_t direct = static_cast<uint8_t>(lower & 0x3Fu);
    if (direct == 0x30u || direct == 0x31u || direct == 0x34u || direct == 0x35u)
    {
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viS);
        readVi(viT);
        writeVi(viD);
        return usage;
    }
    if (direct == 0x32u)
    {
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viS);
        writeVi(viT);
        return usage;
    }
    if (direct < 0x3Cu)
    {
        usage.reserved = true;
        return usage;
    }

    const uint8_t special = static_cast<uint8_t>((lower & 3u) | ((lower >> 4) & 0x7Cu));
    switch (special)
    {
    case 0x30:
    case 0x31:
        usage.pipeline = PipelineFmac;
        usage.latency = 4u;
        addVfRead(usage, vfS, special == 0x31u ? 0xFu : dest);
        addVfWrite(usage, vfT, dest);
        break;
    case 0x34:
    case 0x36:
        usage.pipeline = PipelineLsu;
        usage.latency = 4u;
        usage.viLatency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viS);
        writeVi(viS);
        addVfWrite(usage, vfT, dest);
        break;
    case 0x35:
    case 0x37:
        usage.pipeline = PipelineLsu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viT);
        writeVi(viT);
        addVfRead(usage, vfS, dest);
        break;
    case 0x38:
        usage.pipeline = PipelineFdiv;
        usage.latency = 7u;
        addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        addVfRead(usage, vfT, laneForComponent((lower >> 23) & 3u));
        break;
    case 0x39:
        usage.pipeline = PipelineFdiv;
        usage.latency = 7u;
        addVfRead(usage, vfT, laneForComponent((lower >> 23) & 3u));
        break;
    case 0x3A:
        usage.pipeline = PipelineFdiv;
        usage.latency = 13u;
        addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        addVfRead(usage, vfT, laneForComponent((lower >> 23) & 3u));
        break;
    case 0x3B:
        usage.pipeline = PipelineFdiv;
        usage.waitQ = true;
        break;
    case 0x3C:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        writeVi(viT);
        break;
    case 0x3D:
        usage.pipeline = PipelineFmac;
        usage.latency = 4u;
        readVi(viS);
        addVfWrite(usage, vfT, dest);
        break;
    case 0x3E:
        usage.pipeline = PipelineLsu;
        usage.latency = 4u;
        readVi(viS);
        writeVi(viT);
        break;
    case 0x3F:
        usage.pipeline = PipelineLsu;
        usage.latency = 1u;
        readVi(viS);
        readVi(viT);
        break;
    case 0x40:
    case 0x41:
        usage.pipeline = PipelineFmac;
        usage.latency = 4u;
        addVfWrite(usage, vfT, dest);
        break;
    case 0x42:
    case 0x43:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        break;
    case 0x64:
        if (unit == Unit::VU0)
        {
            usage.reserved = true;
            break;
        }
        usage.pipeline = PipelineFmac;
        usage.latency = 4u;
        addVfWrite(usage, vfT, dest);
        break;
    case 0x68:
    case 0x69:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        writeVi(viT);
        break;
    case 0x6C:
        if (unit == Unit::VU0)
        {
            usage.reserved = true;
            break;
        }
        usage.pipeline = PipelineXgkick;
        usage.latency = 2u;
        readVi(viS);
        break;
    case 0x70:
    case 0x71:
    case 0x72:
    case 0x73:
    case 0x74:
    case 0x75:
    case 0x76:
    case 0x77:
    case 0x78:
    case 0x79:
    case 0x7A:
    case 0x7C:
    case 0x7D:
        if (unit == Unit::VU0)
        {
            usage.reserved = true;
            break;
        }
        usage.pipeline = PipelineEfu;
        switch (special)
        {
        case 0x70:
            usage.latency = 11u;
            break;
        case 0x71:
        case 0x72:
        case 0x77:
            usage.latency = 18u;
            break;
        case 0x73:
            usage.latency = 24u;
            break;
        case 0x74:
        case 0x75:
        case 0x7C:
            usage.latency = 54u;
            break;
        case 0x76:
        case 0x78:
        case 0x7A:
            usage.latency = 12u;
            break;
        case 0x79:
            usage.latency = 29u;
            break;
        case 0x7D:
            usage.latency = 44u;
            break;
        default:
            break;
        }
        if (special >= 0x70u && special <= 0x73u)
            addVfRead(usage, vfS, 0xEu);
        else if (special == 0x74u)
            addVfRead(usage, vfS, 0xCu);
        else if (special == 0x75u)
            addVfRead(usage, vfS, 0xAu);
        else if (special == 0x76u)
            addVfRead(usage, vfS, 0xFu);
        else
            addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        break;
    case 0x7B:
        if (unit == Unit::VU0)
        {
            usage.reserved = true;
            break;
        }
        usage.pipeline = PipelineEfu;
        usage.waitP = true;
        break;
    default:
        usage.reserved = true;
        break;
    }
    return usage;
}

constexpr VU1Core::DecodedInstructionPair VU1Core::decodePairWords(uint32_t lower, uint32_t upper, Unit unit)
{
    DecodedInstructionPair decoded;
    decoded.lower = lower;
    decoded.upper = upper;
    decoded.iBit = (decoded.upper & 0x80000000u) != 0u;
    decoded.eBit = (decoded.upper & 0x40000000u) != 0u;
    decoded.mBit = (decoded.upper & 0x20000000u) != 0u;
    decoded.dBit = (decoded.upper & 0x10000000u) != 0u;
    decoded.tBit = (decoded.upper & 0x08000000u) != 0u;
    decoded.upperUsage = decodeUpperUsage(decoded.upper);
    if (!decoded.iBit)
        decoded.lowerUsage = decodeLowerUsage(decoded.lower, unit);

    const uint8_t upperWriteReg = decoded.upperUsage.vfWrite.reg;
    if (upperWriteReg != 0u && (vfReadLanes(decoded.lowerUsage, upperWriteReg) != 0u || decoded.lowerUsage.vfWrite.reg == upperWriteReg))
    {
        decoded.upperVfShadowReg = upperWriteReg;
        if (decoded.lowerUsage.vfWrite.reg == upperWriteReg)
            decoded.suppressedLowerVf = upperWriteReg;
    }

    // Flattened interlock checks (calculatePairReadyCycle), same registers and lanes as the usages.
    // (A plain array: the VS 2022 compiler (v143) crashes on an initializer_list range-for here.)
    const InstructionUsage *const usages[2] = {&decoded.upperUsage, &decoded.lowerUsage};
    for (const InstructionUsage *usage : usages)
    {
        for (uint32_t index = 0; index < usage->vfReadCount; ++index)
        {
            const VfAccess &access = usage->vfRead[index];
            for (uint32_t component = 0; component < 4u; ++component)
                if ((access.lanes & laneForComponent(component)) != 0u && decoded.readyVfCount < decoded.readyVf.size())
                    decoded.readyVf[decoded.readyVfCount++] = static_cast<uint8_t>(access.reg * 4u + component);
        }
        for (uint32_t bits = static_cast<uint32_t>(usage->viRead) & 0xFFFEu; bits != 0u; bits &= bits - 1u)
            if (decoded.readyViCount < decoded.readyVi.size())
                decoded.readyVi[decoded.readyViCount++] = static_cast<uint8_t>(std::countr_zero(bits));
        for (uint32_t component = 0; component < 4u; ++component)
            if ((usage->accRead & laneForComponent(component)) != 0u)
                decoded.readyAccComponents |= static_cast<uint8_t>(1u << component);
    }
    if (decoded.lowerUsage.pipeline == PipelineFdiv)
        decoded.readyMisc |= kReadyFdiv;
    if (decoded.lowerUsage.pipeline == PipelineEfu)
        decoded.readyMisc |= kReadyEfu;
    if (decoded.lowerUsage.waitQ)
        decoded.readyMisc |= kReadyWaitQ;
    if (decoded.lowerUsage.waitP)
        decoded.readyMisc |= kReadyWaitP;
    if (decoded.lowerUsage.pipeline == PipelineXgkick)
        decoded.readyMisc |= kReadyXgkick;
    decoded.fmacNorm = computeFmacNorm(decoded.upper);
    decoded.fmacZero = computeFmacZero(decoded.upper);
    return decoded;
}
