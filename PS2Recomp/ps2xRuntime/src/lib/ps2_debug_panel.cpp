#include "ps2_debug_panel.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "ps2_log.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_mod_loader.h"

#include <unordered_set>
#include "Kernel/Syscalls/Helpers/State.h"
#include "Kernel/Stubs/CD.h"
#include "Kernel/Stubs/MemoryCard.h"
#include "Kernel/Stubs/Pad.h"

#if defined(PS2X_ENABLE_DEBUG_UI) && !defined(PLATFORM_VITA)
#include "imgui.h"
#include "rlImGui.h"
#include "raylib.h"
#endif

#include <algorithm>
#include <cstdarg>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <exception>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace
{
#if defined(PS2X_ENABLE_DEBUG_UI) && !defined(PLATFORM_VITA)
    const char *threadStatusName(EeThreadStatus status)
    {
        switch (status)
        {
        case EeThreadStatus::Running:
            return "RUN";
        case EeThreadStatus::Ready:
            return "READY";
        case EeThreadStatus::Waiting:
            return "WAIT";
        case EeThreadStatus::Suspended:
            return "SUSPEND";
        case EeThreadStatus::WaitingSuspended:
            return "WAITSUSP";
        case EeThreadStatus::Dormant:
            return "DORMANT";
        default:
            return "?";
        }
    }

    const char *waitTypeName(EeWaitReason waitType)
    {
        switch (waitType)
        {
        case EeWaitReason::None:
            return "NONE";
        case EeWaitReason::Sleep:
            return "SLEEP";
        case EeWaitReason::Semaphore:
            return "SEMA";
        case EeWaitReason::EventFlag:
            return "EVENT";
        case EeWaitReason::VSync:
            return "VSYNC";
        case EeWaitReason::External:
            return "EXTERNAL";
        case EeWaitReason::Mpeg:
            return "MPEG";
        default:
            return "?";
        }
    }

    std::string iopRpcSidName(const ps2x::iop::DebugSnapshot &snapshot, uint32_t sid)
    {
        for (const ps2x::iop::DebugService &service : snapshot.services)
        {
            if (service.active && std::find(service.sids.begin(), service.sids.end(), sid) !=
                service.sids.end())
            {
                return service.name;
            }
        }
        return {};
    }

    std::string pressedPadButtons(uint16_t activeLowButtons)
    {
        struct ButtonName
        {
            uint16_t mask;
            const char *name;
        };
        static constexpr ButtonName buttons[] = {
            {1u << 0, "Select"},
            {1u << 1, "L3"},
            {1u << 2, "R3"},
            {1u << 3, "Start"},
            {1u << 4, "Up"},
            {1u << 5, "Right"},
            {1u << 6, "Down"},
            {1u << 7, "Left"},
            {1u << 8, "L2"},
            {1u << 9, "R2"},
            {1u << 10, "L1"},
            {1u << 11, "R1"},
            {1u << 12, "Triangle"},
            {1u << 13, "Circle"},
            {1u << 14, "Cross"},
            {1u << 15, "Square"},
        };

        std::string out;
        for (const ButtonName &button : buttons)
        {
            if ((activeLowButtons & button.mask) == 0u)
            {
                if (!out.empty())
                {
                    out += ", ";
                }
                out += button.name;
            }
        }
        return out.empty() ? std::string("none") : out;
    }

    std::string rpcDebugFlags(uint32_t flags)
    {
        struct FlagName
        {
            uint32_t mask;
            const char *name;
        };
        static constexpr FlagName names[] = {
            {kSifRpcDebugFlagNowait, "nowait"},
            {kSifRpcDebugFlagHandledByHle, "hle"},
            {kSifRpcDebugFlagCallback, "callback"},
            {kSifRpcDebugFlagMissingClient, "bad-client"},
            {kSifRpcDebugFlagServerDispatch, "server"},
            {kSifRpcDebugFlagUnhandled, "unhandled"},
            {kSifRpcDebugFlagFallbackCopy, "fallback-copy"},
            {kSifRpcDebugFlagFallbackZero, "fallback-zero"},
        };

        std::string out;
        for (const FlagName &name : names)
        {
            if ((flags & name.mask) != 0u)
            {
                if (!out.empty())
                {
                    out += ",";
                }
                out += name.name;
            }
        }
        return out;
    }

    std::string rpcPreviewBytes(const uint8_t *bytes, uint32_t count)
    {
        if (!bytes || count == 0u)
        {
            return "";
        }

        std::string out;
        char item[4] = {};
        for (uint32_t i = 0; i < count; ++i)
        {
            std::snprintf(item, sizeof(item), "%02X", bytes[i]);
            if (!out.empty())
            {
                out.push_back(' ');
            }
            out += item;
        }
        return out;
    }

    template <typename T>
    bool readGuestObjectNoThrow(uint8_t *rdram, uint32_t addr, T &out)
    {
        if (!rdram || addr == 0u)
        {
            return false;
        }

        uint8_t *ptr = getMemPtr(rdram, addr);
        if (!ptr)
        {
            return false;
        }

        std::memcpy(&out, ptr, sizeof(T));
        return true;
    }

    const char *primTypeName(GSPrimType type)
    {
        switch (type)
        {
        case GS_PRIM_POINT:
            return "POINT";
        case GS_PRIM_LINE:
            return "LINE";
        case GS_PRIM_LINESTRIP:
            return "LINE_STRIP";
        case GS_PRIM_TRIANGLE:
            return "TRI";
        case GS_PRIM_TRISTRIP:
            return "TRI_STRIP";
        case GS_PRIM_TRIFAN:
            return "TRI_FAN";
        case GS_PRIM_SPRITE:
            return "SPRITE";
        default:
            return "?";
        }
    }

    const char *gsDebugEventKindName(GSDebugEventKind kind)
    {
        switch (kind)
        {
        case GSDebugEventKind::GifTag:
            return "GIF";
        case GSDebugEventKind::Register:
            return "REG";
        case GSDebugEventKind::Draw:
            return "DRAW";
        case GSDebugEventKind::Transfer:
            return "XFER";
        case GSDebugEventKind::Present:
            return "PRESENT";
        default:
            return "?";
        }
    }

    const char *gifFormatName(uint8_t flg)
    {
        switch (flg)
        {
        case GIF_FMT_PACKED:
            return "PACKED";
        case GIF_FMT_REGLIST:
            return "REGLIST";
        case GIF_FMT_IMAGE:
            return "IMAGE";
        case GIF_FMT_DISABLED:
            return "DISABLED";
        default:
            return "?";
        }
    }

    const char *gsRegName(uint8_t reg)
    {
        switch (reg)
        {
        case GS_REG_PRIM:
            return "PRIM";
        case GS_REG_RGBAQ:
            return "RGBAQ";
        case GS_REG_ST:
            return "ST";
        case GS_REG_UV:
            return "UV";
        case GS_REG_XYZF2:
            return "XYZF2";
        case GS_REG_XYZ2:
            return "XYZ2";
        case GS_REG_TEX0_1:
            return "TEX0_1";
        case GS_REG_TEX0_2:
            return "TEX0_2";
        case GS_REG_CLAMP_1:
            return "CLAMP_1";
        case GS_REG_CLAMP_2:
            return "CLAMP_2";
        case GS_REG_XYZF3:
            return "XYZF3";
        case GS_REG_XYZ3:
            return "XYZ3";
        case GS_REG_TEX1_1:
            return "TEX1_1";
        case GS_REG_TEX1_2:
            return "TEX1_2";
        case GS_REG_TEX2_1:
            return "TEX2_1";
        case GS_REG_TEX2_2:
            return "TEX2_2";
        case GS_REG_XYOFFSET_1:
            return "XYOFFSET_1";
        case GS_REG_XYOFFSET_2:
            return "XYOFFSET_2";
        case GS_REG_PRMODECONT:
            return "PRMODECONT";
        case GS_REG_PRMODE:
            return "PRMODE";
        case GS_REG_TEXCLUT:
            return "TEXCLUT";
        case GS_REG_TEXA:
            return "TEXA";
        case GS_REG_SCISSOR_1:
            return "SCISSOR_1";
        case GS_REG_SCISSOR_2:
            return "SCISSOR_2";
        case GS_REG_ALPHA_1:
            return "ALPHA_1";
        case GS_REG_ALPHA_2:
            return "ALPHA_2";
        case GS_REG_TEST_1:
            return "TEST_1";
        case GS_REG_TEST_2:
            return "TEST_2";
        case GS_REG_FBA_1:
            return "FBA_1";
        case GS_REG_FBA_2:
            return "FBA_2";
        case GS_REG_FRAME_1:
            return "FRAME_1";
        case GS_REG_FRAME_2:
            return "FRAME_2";
        case GS_REG_ZBUF_1:
            return "ZBUF_1";
        case GS_REG_ZBUF_2:
            return "ZBUF_2";
        case GS_REG_BITBLTBUF:
            return "BITBLTBUF";
        case GS_REG_TRXPOS:
            return "TRXPOS";
        case GS_REG_TRXREG:
            return "TRXREG";
        case GS_REG_TRXDIR:
            return "TRXDIR";
        case GS_REG_HWREG:
            return "HWREG";
        case GS_REG_SIGNAL:
            return "SIGNAL";
        case GS_REG_FINISH:
            return "FINISH";
        case GS_REG_LABEL:
            return "LABEL";
        default:
            return "?";
        }
    }

    void textHex32(const char *label, uint32_t value)
    {
        ImGui::Text("%s: 0x%08X", label, value);
    }

    void textHex64(const char *label, uint64_t value)
    {
        ImGui::Text("%s: 0x%016llX", label, static_cast<unsigned long long>(value));
    }

    uint32_t readGuestU32NoThrow(uint8_t *rdram, uint32_t addr)
    {
        if (!rdram || addr == 0u)
        {
            return 0u;
        }

        uint8_t *ptr = getMemPtr(rdram, addr);
        if (!ptr)
        {
            return 0u;
        }

        uint32_t value = 0u;
        std::memcpy(&value, ptr, sizeof(value));
        return value;
    }

    std::string pathToString(const std::filesystem::path &path)
    {
        if (path.empty())
        {
            return std::string("<empty>");
        }
        return path.string();
    }

    void textPath(const char *label, const std::filesystem::path &path)
    {
        const std::string value = pathToString(path);
        ImGui::Text("%s: %s", label, value.c_str());
    }

    std::filesystem::path makeDebugDumpPath(const char *name, uint32_t frameIndex)
    {
        static uint32_t s_dumpCounter = 0u;
        std::filesystem::path dir = std::filesystem::current_path() / "ps2_debug_dumps";
        std::filesystem::create_directories(dir);

        char fileName[128]{};
        std::snprintf(fileName,
                      sizeof(fileName),
                      "%s_frame_%08X_%04u.txt",
                      name,
                      frameIndex,
                      ++s_dumpCounter);
        return dir / fileName;
    }

    void dumpGsContext(std::ostream &out, const char *label, const GSContext &ctx)
    {
        out << label << "\n";
        out << "  FRAME  fbp=" << ctx.frame.fbp
            << " fbw=" << static_cast<unsigned int>(ctx.frame.fbw)
            << " psm=0x" << std::hex << static_cast<unsigned int>(ctx.frame.psm)
            << " fbmsk=0x" << ctx.frame.fbmsk << std::dec << "\n";
        out << "  ZBUF   zbp=" << ctx.zbuf.zbp
            << " psm=0x" << std::hex << static_cast<unsigned int>(ctx.zbuf.psm) << std::dec
            << " zmask=" << (ctx.zbuf.zmask ? 1u : 0u) << "\n";
        out << "  SCISSOR x=" << ctx.scissor.x0 << ".." << ctx.scissor.x1
            << " y=" << ctx.scissor.y0 << ".." << ctx.scissor.y1 << "\n";
        out << "  TEX0   tbp0=" << ctx.tex0.tbp0
            << " tbw=" << static_cast<unsigned int>(ctx.tex0.tbw)
            << " psm=0x" << std::hex << static_cast<unsigned int>(ctx.tex0.psm) << std::dec
            << " size=" << (1u << ctx.tex0.tw) << "x" << (1u << ctx.tex0.th)
            << " tcc=" << static_cast<unsigned int>(ctx.tex0.tcc)
            << " tfx=" << static_cast<unsigned int>(ctx.tex0.tfx)
            << " cbp=" << ctx.tex0.cbp
            << " cpsm=0x" << std::hex << static_cast<unsigned int>(ctx.tex0.cpsm) << std::dec
            << " csm=" << static_cast<unsigned int>(ctx.tex0.csm)
            << " csa=" << static_cast<unsigned int>(ctx.tex0.csa)
            << " cld=" << static_cast<unsigned int>(ctx.tex0.cld) << "\n";
        out << "  XYOFFSET ofx=" << ctx.xyoffset.ofx << " ofy=" << ctx.xyoffset.ofy << "\n";
        out << "  TEST   0x" << std::hex << std::setw(16) << std::setfill('0') << ctx.test << std::setfill(' ') << std::dec
            << " ATE=" << ((ctx.test >> 0) & 1ull)
            << " ATST=" << ((ctx.test >> 1) & 7ull)
            << " AREF=0x" << std::hex << ((ctx.test >> 4) & 0xffull) << std::dec
            << " AFAIL=" << ((ctx.test >> 12) & 3ull)
            << " DATE=" << ((ctx.test >> 14) & 1ull)
            << " DATM=" << ((ctx.test >> 15) & 1ull)
            << " ZTE=" << ((ctx.test >> 16) & 1ull)
            << " ZTST=" << ((ctx.test >> 17) & 3ull) << "\n";
        out << "  ALPHA  0x" << std::hex << std::setw(16) << std::setfill('0') << ctx.alpha << std::setfill(' ') << std::dec << "\n";
        out << "  TEX1   0x" << std::hex << std::setw(16) << std::setfill('0') << ctx.tex1 << std::setfill(' ') << std::dec << "\n";
        out << "  CLAMP  0x" << std::hex << std::setw(16) << std::setfill('0') << ctx.clamp << std::setfill(' ') << std::dec << "\n";
        out << "  FBA    0x" << std::hex << std::setw(16) << std::setfill('0') << ctx.fba << std::setfill(' ') << std::dec << "\n";
    }

    bool writeGsDebugDump(const std::filesystem::path &path,
                          const GSRegisters &regs,
                          const GSDebugSnapshot &gs,
                          const std::vector<GSDebugHistoryEntry> &history,
                          bool currentFrameOnly,
                          bool drawsOnly,
                          uint32_t latestFrame,
                          std::string &error)
    {
        try
        {
            std::filesystem::create_directories(path.parent_path());
            std::ofstream out(path, std::ios::out | std::ios::trunc);
            if (!out)
            {
                error = "failed to open output file";
                return false;
            }

            out << "PS2 Runtime GS debug dump\n";
            out << "path: " << path.string() << "\n";
            out << "latestFrame: " << latestFrame << "\n";
            out << "filters: currentFrameOnly=" << (currentFrameOnly ? 1u : 0u)
                << " drawsOnly=" << (drawsOnly ? 1u : 0u) << "\n\n";

            out << "[GS private registers]\n";
            out << std::hex << std::setfill('0');
            out << "PMODE    0x" << std::setw(16) << regs.pmode << "\n";
            out << "SMODE2   0x" << std::setw(16) << regs.smode2 << "\n";
            out << "DISPFB1  0x" << std::setw(16) << regs.dispfb1 << "\n";
            out << "DISPLAY1 0x" << std::setw(16) << regs.display1 << "\n";
            out << "DISPFB2  0x" << std::setw(16) << regs.dispfb2 << "\n";
            out << "DISPLAY2 0x" << std::setw(16) << regs.display2 << "\n";
            out << "BGCOLOR  0x" << std::setw(16) << regs.bgcolor << "\n";
            out << "CSR      0x" << std::setw(16) << regs.csr.load() << "\n";
            out << "IMR      0x" << std::setw(16) << regs.imr << "\n";
            out << "BUSDIR   0x" << std::setw(16) << regs.busdir << "\n";
            out << "SIGLBLID 0x" << std::setw(16) << regs.siglblid << "\n\n";
            out << std::dec << std::setfill(' ');

            out << "[GS draw state]\n";
            out << "PRIM type=" << primTypeName(gs.prim.type)
                << " iip=" << gs.prim.iip
                << " tme=" << gs.prim.tme
                << " fge=" << gs.prim.fge
                << " abe=" << gs.prim.abe
                << " aa1=" << gs.prim.aa1
                << " fst=" << gs.prim.fst
                << " ctxt=" << gs.prim.ctxt
                << " fix=" << gs.prim.fix << "\n";
            out << "TEXA ta0=0x" << std::hex << static_cast<unsigned int>(gs.texa.ta0) << std::dec
                << " aem=" << (gs.texa.aem ? 1u : 0u)
                << " ta1=0x" << std::hex << static_cast<unsigned int>(gs.texa.ta1) << std::dec
                << " TEXCLUT cbw=" << gs.texclut.cbw
                << " cou=" << gs.texclut.cou
                << " cov=" << gs.texclut.cov << "\n";
            out << "BITBLT sbp=" << gs.bitbltbuf.sbp
                << " sbw=" << gs.bitbltbuf.sbw
                << " spsm=0x" << std::hex << static_cast<unsigned int>(gs.bitbltbuf.spsm) << std::dec
                << " dbp=" << gs.bitbltbuf.dbp
                << " dbw=" << gs.bitbltbuf.dbw
                << " dpsm=0x" << std::hex << static_cast<unsigned int>(gs.bitbltbuf.dpsm) << std::dec << "\n";
            out << "TRXPOS ss=(" << gs.trxpos.ssax << "," << gs.trxpos.ssay << ")"
                << " ds=(" << gs.trxpos.dsax << "," << gs.trxpos.dsay << ")"
                << " dir=" << gs.trxpos.dir
                << " TRXREG=" << gs.trxreg.rrw << "x" << gs.trxreg.rrh
                << " TRXDIR=" << gs.trxdir
                << " copied=" << gs.transferCopiedPixels << "/" << gs.transferTotalPixels
                << " at=(" << gs.transferX << "," << gs.transferY << ")\n";
            out << "Host presentation has=" << (gs.hasHostPresentationFrame ? 1u : 0u)
                << " size=" << gs.hostPresentationWidth << "x" << gs.hostPresentationHeight
                << " displayFbp=" << gs.hostPresentationDisplayFbp
                << " sourceFbp=" << gs.hostPresentationSourceFbp
                << " preferred=" << (gs.hostPresentationUsedPreferred ? 1u : 0u)
                << " lastDisplayBaseBytes=0x" << std::hex << gs.lastDisplayBaseBytes << std::dec
                << " localToHostPending=" << gs.localToHostPendingBytes << "\n";
            out << "Preferred source has=" << (gs.hasPreferredDisplaySource ? 1u : 0u)
                << " frame fbp=" << gs.preferredDisplaySourceFrame.fbp
                << " fbw=" << gs.preferredDisplaySourceFrame.fbw
                << " psm=0x" << std::hex << static_cast<unsigned int>(gs.preferredDisplaySourceFrame.psm) << std::dec
                << " destFbp=" << gs.preferredDisplayDestFbp << "\n\n";

            out << "[GS context state]\n";
            dumpGsContext(out, "Context 0", gs.ctx[0]);
            dumpGsContext(out, "Context 1", gs.ctx[1]);
            out << "\n";

            out << "[GS history]\n";
            out << "seq\tframe\ttick\tkind\tprim\tctx\tframe\tzbuf\ttest\talpha\ttex0\txy\tz\ta\tdetail\tpresent\n";
            for (auto it = history.rbegin(); it != history.rend(); ++it)
            {
                const GSDebugHistoryEntry &row = *it;
                if (currentFrameOnly && row.frameIndex != latestFrame)
                {
                    continue;
                }
                if (drawsOnly && row.kind != GSDebugEventKind::Draw)
                {
                    continue;
                }

                out << row.seq << '\t'
                    << row.frameIndex << '\t'
                    << row.vsyncTick << '\t'
                    << gsDebugEventKindName(row.kind) << '\t'
                    << primTypeName(row.prim.type) << '\t'
                    << (row.prim.ctxt ? 1u : 0u) << '\t'
                    << "fbp=" << row.frame.fbp << "/fbw=" << static_cast<unsigned int>(row.frame.fbw) << "/psm=0x" << std::hex << static_cast<unsigned int>(row.frame.psm) << std::dec << '\t'
                    << "zbp=" << row.zbuf.zbp << "/psm=0x" << std::hex << static_cast<unsigned int>(row.zbuf.psm) << std::dec << "/m=" << (row.zbuf.zmask ? 1u : 0u) << '\t'
                    << "0x" << std::hex << row.test << std::dec << '\t'
                    << "0x" << std::hex << row.alpha << std::dec << '\t'
                    << "tbp=" << row.tex0.tbp0 << "/psm=0x" << std::hex << static_cast<unsigned int>(row.tex0.psm) << std::dec << "/size=" << (1u << row.tex0.tw) << "x" << (1u << row.tex0.th) << "/cbp=" << row.tex0.cbp << '\t';

                if (row.kind == GSDebugEventKind::Draw)
                {
                    out << row.xMin << ".." << row.xMax << "/" << row.yMin << ".." << row.yMax << '\t'
                        << row.zMin << ".." << row.zMax << '\t'
                        << static_cast<unsigned int>(row.aMin) << ".." << static_cast<unsigned int>(row.aMax) << '\t';
                }
                else
                {
                    out << "-\t-\t-\t";
                }

                if (row.kind == GSDebugEventKind::GifTag)
                {
                    out << gifFormatName(row.gifFlg) << " nloop=" << row.gifNloop << " nreg=" << row.gifNreg << " size=" << row.gifSizeBytes;
                }
                else if (row.kind == GSDebugEventKind::Register)
                {
                    out << gsRegName(row.reg) << "(0x" << std::hex << static_cast<unsigned int>(row.reg) << ")=0x" << row.regValue << std::dec;
                }
                else if (row.kind == GSDebugEventKind::Transfer)
                {
                    out << "TRXDIR=" << row.trxdir << " " << row.trxreg.rrw << "x" << row.trxreg.rrh
                        << " pix=" << row.transferPixels << " sbp=" << row.bitbltbuf.sbp << " dbp=" << row.bitbltbuf.dbp;
                }
                else
                {
                    out << "-";
                }
                out << '\t';
                if (row.kind == GSDebugEventKind::Present)
                {
                    out << row.width << "x" << row.height
                        << " display=" << row.displayFbp
                        << " source=" << row.sourceFbp
                        << " pref=" << (row.usedPreferred ? 1u : 0u);
                }
                else
                {
                    out << "-";
                }
                out << '\n';
            }

            return true;
        }
        catch (const std::exception &ex)
        {
            error = ex.what();
            return false;
        }
    }

    void drawGsContext(const char *label, const GSContext &ctx)
    {
        if (!ImGui::TreeNode(label))
        {
            return;
        }

        ImGui::Text("FRAME fbp=%u fbw=%u psm=0x%02X fbmsk=0x%08X",
                    ctx.frame.fbp,
                    ctx.frame.fbw,
                    static_cast<unsigned int>(ctx.frame.psm),
                    ctx.frame.fbmsk);
        ImGui::Text("ZBUF  zbp=%u psm=0x%02X zmask=%u",
                    ctx.zbuf.zbp,
                    static_cast<unsigned int>(ctx.zbuf.psm),
                    ctx.zbuf.zmask ? 1u : 0u);
        ImGui::Text("SCISSOR x=%u..%u y=%u..%u",
                    ctx.scissor.x0,
                    ctx.scissor.x1,
                    ctx.scissor.y0,
                    ctx.scissor.y1);
        ImGui::Text("TEX0 tbp0=%u tbw=%u psm=0x%02X tw=%u th=%u tcc=%u tfx=%u cbp=%u cpsm=0x%02X csm=%u csa=%u cld=%u",
                    ctx.tex0.tbp0,
                    static_cast<unsigned int>(ctx.tex0.tbw),
                    static_cast<unsigned int>(ctx.tex0.psm),
                    static_cast<unsigned int>(ctx.tex0.tw),
                    static_cast<unsigned int>(ctx.tex0.th),
                    static_cast<unsigned int>(ctx.tex0.tcc),
                    static_cast<unsigned int>(ctx.tex0.tfx),
                    ctx.tex0.cbp,
                    static_cast<unsigned int>(ctx.tex0.cpsm),
                    static_cast<unsigned int>(ctx.tex0.csm),
                    static_cast<unsigned int>(ctx.tex0.csa),
                    static_cast<unsigned int>(ctx.tex0.cld));
        ImGui::Text("XYOFFSET ofx=%u ofy=%u", ctx.xyoffset.ofx, ctx.xyoffset.ofy);
        textHex64("TEST", ctx.test);
        ImGui::SameLine();
        ImGui::Text("ATE=%llu ATST=%llu AREF=0x%02llX AFAIL=%llu DATE=%llu DATM=%llu ZTE=%llu ZTST=%llu",
                    (ctx.test >> 0) & 1ull,
                    (ctx.test >> 1) & 7ull,
                    (ctx.test >> 4) & 0xffull,
                    (ctx.test >> 12) & 3ull,
                    (ctx.test >> 14) & 1ull,
                    (ctx.test >> 15) & 1ull,
                    (ctx.test >> 16) & 1ull,
                    (ctx.test >> 17) & 3ull);
        textHex64("ALPHA", ctx.alpha);
        textHex64("TEX1", ctx.tex1);
        textHex64("CLAMP", ctx.clamp);
        textHex64("FBA", ctx.fba);
        ImGui::TreePop();
    }

    void drawHexDump(uint8_t *rdram, uint32_t startAddr, uint32_t bytes)
    {
        bytes = std::clamp<uint32_t>(bytes, 16u, 0x1000u);
        const uint32_t alignedStart = startAddr & ~0x0fu;
        for (uint32_t off = 0u; off < bytes; off += 16u)
        {
            const uint32_t addr = alignedStart + off;
            uint8_t *ptr = getMemPtr(rdram, addr);
            if (!ptr)
            {
                ImGui::Text("%08X: <invalid>", addr);
                continue;
            }

            char hex[16 * 3 + 1]{};
            char ascii[17]{};
            for (uint32_t i = 0u; i < 16u; ++i)
            {
                const uint8_t b = ptr[i];
                std::snprintf(hex + i * 3, sizeof(hex) - i * 3, "%02X ", b);
                ascii[i] = (b >= 32u && b < 127u) ? static_cast<char>(b) : '.';
            }
            ImGui::Text("%08X: %s |%s|", addr, hex, ascii);
        }
    }

    void drawCpuTab(PS2Runtime &runtime, bool showRegisters)
    {
        const bool executingGuest = runtime.eeScheduler().isExecutingGuest();
        const EeKernelSnapshot schedulerSnapshot = runtime.eeScheduler().snapshot();
        const EeThreadSnapshot *selectedThread = nullptr;
        if (!executingGuest)
        {
            const auto running = std::find_if(schedulerSnapshot.threads.begin(),
                                              schedulerSnapshot.threads.end(),
                                              [&](const EeThreadSnapshot &thread)
                                              { return thread.id == schedulerSnapshot.runningThreadId; });
            if (running != schedulerSnapshot.threads.end())
            {
                selectedThread = &*running;
            }
            else
            {
                const auto blocked = std::find_if(schedulerSnapshot.threads.begin(),
                                                  schedulerSnapshot.threads.end(),
                                                  [](const EeThreadSnapshot &thread)
                                                  { return thread.status != EeThreadStatus::Dormant; });
                if (blocked != schedulerSnapshot.threads.end())
                {
                    selectedThread = &*blocked;
                }
                else if (!schedulerSnapshot.threads.empty())
                {
                    selectedThread = &schedulerSnapshot.threads.front();
                }
            }
        }

        const uint32_t pc = selectedThread ? selectedThread->pc : runtime.m_debugPc.load(std::memory_order_relaxed);
        const uint32_t ra = selectedThread ? selectedThread->ra : runtime.m_debugRa.load(std::memory_order_relaxed);
        const uint32_t sp = selectedThread ? selectedThread->sp : runtime.m_debugSp.load(std::memory_order_relaxed);
        const uint32_t gp = selectedThread ? selectedThread->contextGp : runtime.m_debugGp.load(std::memory_order_relaxed);

        ImGui::Text("Runtime: %s", runtime.isStopRequested() ? "stop requested" : "running");
        ImGui::Text("EE executor: %s", executingGuest ? "guest" : "scheduler");
        if (selectedThread)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("(thread %d: %s/%s)", selectedThread->id, threadStatusName(selectedThread->status), waitTypeName(selectedThread->waitReason));
        }
        ImGui::Separator();
        textHex32("PC", pc);
        ImGui::SameLine();
        textHex32("RA", ra);
        textHex32("SP", sp);
        ImGui::SameLine();
        textHex32("GP", gp);

        if (ImGui::Button("Copy PC/RA/SP/GP"))
        {
            char buf[256]{};
            std::snprintf(buf, sizeof(buf), "pc=0x%08X ra=0x%08X sp=0x%08X gp=0x%08X", pc, ra, sp, gp);
            ImGui::SetClipboardText(buf);
        }

        ImGui::TextDisabled("full context snapshot is best-effort while guest is running");

        if (showRegisters)
        {
            const R5900Context &cpu = runtime.cpu();
            if (ImGui::BeginTable("gpr", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
            {
                ImGui::TableSetupColumn("Reg");
                ImGui::TableSetupColumn("Lo32");
                ImGui::TableSetupColumn("Hi32[1]");
                ImGui::TableSetupColumn("Hi64[2:3]");
                ImGui::TableHeadersRow();
                for (int i = 0; i < 32; ++i)
                {
                    const uint32_t w0 = static_cast<uint32_t>(_mm_extract_epi32(cpu.r[i], 0));
                    const uint32_t w1 = static_cast<uint32_t>(_mm_extract_epi32(cpu.r[i], 1));
                    const uint32_t w2 = static_cast<uint32_t>(_mm_extract_epi32(cpu.r[i], 2));
                    const uint32_t w3 = static_cast<uint32_t>(_mm_extract_epi32(cpu.r[i], 3));
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("r%d", i);
                    ImGui::TableNextColumn();
                    ImGui::Text("0x%08X", w0);
                    ImGui::TableNextColumn();
                    ImGui::Text("0x%08X", w1);
                    ImGui::TableNextColumn();
                    ImGui::Text("0x%08X_%08X", w3, w2);
                }
                ImGui::EndTable();
            }

            ImGui::SeparatorText("COP0");
            ImGui::Text("Status=0x%08X Cause=0x%08X EPC=0x%08X Count=0x%08X Compare=0x%08X BadVAddr=0x%08X",
                        cpu.cop0_status,
                        cpu.cop0_cause,
                        cpu.cop0_epc,
                        cpu.cop0_count,
                        cpu.cop0_compare,
                        cpu.cop0_badvaddr);
        }
    }

    void drawThreadsTab(PS2Runtime &runtime)
    {
        const EeKernelSnapshot snapshot = runtime.eeScheduler().snapshot();
        ImGui::Text("Threads: %zu running=%d", snapshot.threads.size(), snapshot.runningThreadId);
        ImGui::Text("EE cycle: %llu  slice end: %llu  next event: %llu",
                    static_cast<unsigned long long>(snapshot.eeCycle),
                    static_cast<unsigned long long>(snapshot.sliceEndCycle),
                    static_cast<unsigned long long>(snapshot.nextEventCycle));
        if (ImGui::BeginTable("threads", 15, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0, 320)))
        {
            ImGui::TableSetupColumn("ID");
            ImGui::TableSetupColumn("Status");
            ImGui::TableSetupColumn("Wait");
            ImGui::TableSetupColumn("WaitId");
            ImGui::TableSetupColumn("PC");
            ImGui::TableSetupColumn("RA");
            ImGui::TableSetupColumn("SP");
            ImGui::TableSetupColumn("Ctx GP");
            ImGui::TableSetupColumn("Entry");
            ImGui::TableSetupColumn("Stack");
            ImGui::TableSetupColumn("GP");
            ImGui::TableSetupColumn("Prio");
            ImGui::TableSetupColumn("Wake");
            ImGui::TableSetupColumn("Susp");
            ImGui::TableSetupColumn("Inv");
            ImGui::TableHeadersRow();
            for (const EeThreadSnapshot &row : snapshot.threads)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%d", row.id);
                ImGui::TableNextColumn();
                ImGui::Text("%s", threadStatusName(row.status));
                ImGui::TableNextColumn();
                ImGui::Text("%s", waitTypeName(row.waitReason));
                ImGui::TableNextColumn();
                ImGui::Text("%d", row.waitId);
                ImGui::TableNextColumn();
                ImGui::Text("0x%08X", row.pc);
                ImGui::TableNextColumn();
                ImGui::Text("0x%08X", row.ra);
                ImGui::TableNextColumn();
                ImGui::Text("0x%08X", row.sp);
                ImGui::TableNextColumn();
                ImGui::Text("0x%08X", row.contextGp);
                ImGui::TableNextColumn();
                ImGui::Text("0x%08X", row.entry);
                ImGui::TableNextColumn();
                ImGui::Text("0x%08X/%u", row.stack, row.stackSize);
                ImGui::TableNextColumn();
                ImGui::Text("0x%08X", row.gp);
                ImGui::TableNextColumn();
                ImGui::Text("%d/%d", row.currentPriority, row.initialPriority);
                ImGui::TableNextColumn();
                ImGui::Text("%u", row.wakeupCount);
                ImGui::TableNextColumn();
                ImGui::Text("%d", row.suspendCount);
                ImGui::TableNextColumn();
                ImGui::Text("%u", row.invocationDepth);
            }
            ImGui::EndTable();
        }
    }

    void drawKernelTab(PS2Runtime &runtime)
    {
        const EeKernelSnapshot snapshot = runtime.eeScheduler().snapshot();
        ImGui::SeparatorText("Semaphores");
        {
            if (ImGui::BeginTable("semas", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
            {
                ImGui::TableSetupColumn("ID");
                ImGui::TableSetupColumn("Count");
                ImGui::TableSetupColumn("Max");
                ImGui::TableSetupColumn("Init");
                ImGui::TableSetupColumn("Waiters");
                ImGui::TableSetupColumn("Attr");
                ImGui::TableSetupColumn("Deleted");
                ImGui::TableHeadersRow();
                for (const EeSemaphoreSnapshot &sema : snapshot.semaphores)
                {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", sema.id);
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", sema.count);
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", sema.maxCount);
                    ImGui::TableNextColumn();
                    ImGui::Text("-");
                    ImGui::TableNextColumn();
                    ImGui::Text("%u", sema.waiters);
                    ImGui::TableNextColumn();
                    ImGui::Text("-");
                    ImGui::TableNextColumn();
                    ImGui::Text("0");
                }
                ImGui::EndTable();
            }
        }

        ImGui::SeparatorText("Event flags");
        {
            if (ImGui::BeginTable("evf", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
            {
                ImGui::TableSetupColumn("ID");
                ImGui::TableSetupColumn("Bits");
                ImGui::TableSetupColumn("Init");
                ImGui::TableSetupColumn("Waiters");
                ImGui::TableSetupColumn("Attr");
                ImGui::TableSetupColumn("Deleted");
                ImGui::TableHeadersRow();
                for (const EeEventFlagSnapshot &evf : snapshot.eventFlags)
                {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", evf.id);
                    ImGui::TableNextColumn();
                    ImGui::Text("0x%08X", evf.bits);
                    ImGui::TableNextColumn();
                    ImGui::Text("0x%08X", evf.initBits);
                    ImGui::TableNextColumn();
                    ImGui::Text("%u", evf.waiters);
                    ImGui::TableNextColumn();
                    ImGui::Text("0x%08X", evf.attr);
                    ImGui::TableNextColumn();
                    ImGui::Text("0");
                }
                ImGui::EndTable();
            }
        }
    }

    void drawIopTab(PS2Runtime &runtime)
    {
        uint8_t *rdram = runtime.memory().getRDRAM();
        const ps2x::iop::DebugSnapshot iopSnapshot = runtime.iopDebugSnapshot();

        struct ModuleRow
        {
            int32_t id = 0;
            std::string path;
            std::string pathKey;
            uint32_t refCount = 0;
            bool loaded = false;
        };

        std::vector<ModuleRow> modules;
        uint32_t moduleLogCount = 0;
        int32_t nextModuleId = 0;
        {
            std::lock_guard<std::mutex> lock(g_sif_module_mutex);
            modules.reserve(g_sif_modules_by_id.size());
            for (const auto &[id, record] : g_sif_modules_by_id)
            {
                ModuleRow row{};
                row.id = id;
                row.path = record.path;
                row.pathKey = record.pathKey;
                row.refCount = record.refCount;
                row.loaded = record.loaded;
                modules.push_back(std::move(row));
            }
            moduleLogCount = g_sif_module_log_count;
            nextModuleId = g_next_sif_module_id;
        }
        std::sort(modules.begin(), modules.end(), [](const ModuleRow &a, const ModuleRow &b)
                  { return a.id < b.id; });

        struct RpcServerRow
        {
            uint32_t sid = 0;
            uint32_t sdPtr = 0;
            bool readable = false;
            t_SifRpcServerData sd{};
        };

        struct RpcClientRow
        {
            uint32_t clientPtr = 0;
            bool busy = false;
            uint32_t lastRpc = 0;
            uint32_t sid = 0;
            bool readable = false;
            t_SifRpcClientData cd{};
        };

        std::vector<RpcServerRow> servers;
        std::vector<RpcClientRow> clients;
        bool rpcInitialized = false;
        uint32_t rpcNextId = 0;
        uint32_t rpcPacketIndex = 0;
        uint32_t rpcServerIndex = 0;
        uint32_t rpcActiveQueue = 0;
        {
            std::lock_guard<std::mutex> lock(g_rpc_mutex);
            rpcInitialized = g_rpc_initialized;
            rpcNextId = g_rpc_next_id;
            rpcPacketIndex = g_rpc_packet_index;
            rpcServerIndex = g_rpc_server_index;
            rpcActiveQueue = g_rpc_active_queue;
            servers.reserve(g_rpc_servers.size());
            for (const auto &[sid, state] : g_rpc_servers)
            {
                RpcServerRow row{};
                row.sid = sid;
                row.sdPtr = state.sd_ptr;
                row.readable = readGuestObjectNoThrow(rdram, state.sd_ptr, row.sd);
                servers.push_back(row);
            }

            clients.reserve(g_rpc_clients.size());
            for (const auto &[clientPtr, state] : g_rpc_clients)
            {
                RpcClientRow row{};
                row.clientPtr = clientPtr;
                row.busy = state.busy;
                row.lastRpc = state.last_rpc;
                row.sid = state.sid;
                row.readable = readGuestObjectNoThrow(rdram, clientPtr, row.cd);
                clients.push_back(row);
            }
        }
        std::sort(servers.begin(), servers.end(), [](const RpcServerRow &a, const RpcServerRow &b)
                  { return a.sid < b.sid; });
        std::sort(clients.begin(), clients.end(), [](const RpcClientRow &a, const RpcClientRow &b)
                  { return a.clientPtr < b.clientPtr; });

        auto hasServer = [&](uint32_t sid)
        {
            return std::any_of(servers.begin(), servers.end(), [sid](const RpcServerRow &row)
                               { return row.sid == sid; });
        };

        ImGui::SeparatorText("IOP / SIF modules");
        ImGui::Text("Tracked modules: %zu loaded=%zu nextId=%d moduleLogs=%u",
                    modules.size(),
                    static_cast<size_t>(std::count_if(modules.begin(), modules.end(), [](const ModuleRow &row)
                                                      { return row.loaded; })),
                    nextModuleId,
                    moduleLogCount);
        ImGui::TextDisabled("This table is fed by SifLoadModule/SifLoadModuleBuffer/SifStopModule tracking.");

        if (ImGui::BeginTable("iop_modules", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0, 180)))
        {
            ImGui::TableSetupColumn("ID");
            ImGui::TableSetupColumn("Loaded");
            ImGui::TableSetupColumn("Refs");
            ImGui::TableSetupColumn("Key");
            ImGui::TableSetupColumn("Path");
            ImGui::TableHeadersRow();
            for (const ModuleRow &row : modules)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%d", row.id);
                ImGui::TableNextColumn();
                ImGui::Text("%s", row.loaded ? "yes" : "no");
                ImGui::TableNextColumn();
                ImGui::Text("%u", row.refCount);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(row.pathKey.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(row.path.c_str());
            }
            ImGui::EndTable();
        }

        ImGui::SeparatorText("ps2xIOP HLE services");
        if (ImGui::BeginTable("iop_hle_services", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
        {
            ImGui::TableSetupColumn("Service");
            ImGui::TableSetupColumn("Active");
            ImGui::TableSetupColumn("SID");
            ImGui::TableSetupColumn("EE server");
            ImGui::TableSetupColumn("Metrics");
            ImGui::TableHeadersRow();
            for (const ps2x::iop::DebugService &service : iopSnapshot.services)
            {
                if (service.sids.empty())
                {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(service.name.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(service.active ? "yes" : "no");
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted("-");
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted("-");
                    ImGui::TableNextColumn();
                    ImGui::Text("%zu", service.metrics.size());
                    continue;
                }

                for (const uint32_t sid : service.sids)
                {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(service.name.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(service.active ? "yes" : "no");
                    ImGui::TableNextColumn();
                    ImGui::Text("0x%08X", sid);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(hasServer(sid) ? "yes" : "no");
                    ImGui::TableNextColumn();
                    if (service.metrics.empty())
                    {
                        ImGui::TextUnformatted("-");
                    }
                    else
                    {
                        std::string metrics;
                        for (const ps2x::iop::DebugMetric &metric : service.metrics)
                        {
                            if (!metrics.empty())
                            {
                                metrics += ", ";
                            }
                            std::ostringstream value;
                            value << metric.name << "=";
                            if (metric.hexadecimal)
                            {
                                value << "0x" << std::hex << metric.value;
                            }
                            else
                            {
                                value << std::dec << metric.value;
                            }
                            metrics += value.str();
                        }
                        ImGui::TextUnformatted(metrics.c_str());
                    }
                }
            }
            ImGui::EndTable();
        }

        for (const std::string &diagnostic : iopSnapshot.diagnostics)
        {
            ImGui::TextDisabled("%s", diagnostic.c_str());
        }

        ImGui::SeparatorText("SIF RPC state");
        ImGui::Text("initialized=%u servers=%zu clients=%zu nextId=%u packetIndex=%u serverIndex=%u activeQueue=0x%08X",
                    rpcInitialized ? 1u : 0u,
                    servers.size(),
                    clients.size(),
                    rpcNextId,
                    rpcPacketIndex,
                    rpcServerIndex,
                    rpcActiveQueue);

        if (ImGui::BeginTable("rpc_servers", 10, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0, 180)))
        {
            ImGui::TableSetupColumn("SID");
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("sd*");
            ImGui::TableSetupColumn("func");
            ImGui::TableSetupColumn("buf");
            ImGui::TableSetupColumn("size");
            ImGui::TableSetupColumn("cfunc");
            ImGui::TableSetupColumn("queue");
            ImGui::TableSetupColumn("recv");
            ImGui::TableSetupColumn("rpc#");
            ImGui::TableHeadersRow();
            for (const RpcServerRow &row : servers)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("0x%08X", row.sid);
                ImGui::TableNextColumn();
                const std::string serviceName = iopRpcSidName(iopSnapshot, row.sid);
                ImGui::TextUnformatted(serviceName.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("0x%08X", row.sdPtr);
                ImGui::TableNextColumn();
                ImGui::Text("0x%08X", row.readable ? row.sd.func : 0u);
                ImGui::TableNextColumn();
                ImGui::Text("0x%08X", row.readable ? row.sd.buf : 0u);
                ImGui::TableNextColumn();
                ImGui::Text("%d", row.readable ? row.sd.size : 0);
                ImGui::TableNextColumn();
                ImGui::Text("0x%08X", row.readable ? row.sd.cfunc : 0u);
                ImGui::TableNextColumn();
                ImGui::Text("0x%08X", row.readable ? row.sd.base : 0u);
                ImGui::TableNextColumn();
                ImGui::Text("0x%08X/%d", row.readable ? row.sd.recvbuf : 0u, row.readable ? row.sd.rsize : 0);
                ImGui::TableNextColumn();
                ImGui::Text("%d", row.readable ? row.sd.rpc_number : 0);
            }
            ImGui::EndTable();
        }

        if (ImGui::BeginTable("rpc_clients", 9, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0, 160)))
        {
            ImGui::TableSetupColumn("client*");
            ImGui::TableSetupColumn("SID");
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Busy");
            ImGui::TableSetupColumn("LastRpc");
            ImGui::TableSetupColumn("cmd");
            ImGui::TableSetupColumn("buf");
            ImGui::TableSetupColumn("cbuf");
            ImGui::TableSetupColumn("server*");
            ImGui::TableHeadersRow();
            for (const RpcClientRow &row : clients)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("0x%08X", row.clientPtr);
                ImGui::TableNextColumn();
                ImGui::Text("0x%08X", row.sid);
                ImGui::TableNextColumn();
                const std::string serviceName = iopRpcSidName(iopSnapshot, row.sid);
                ImGui::TextUnformatted(serviceName.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%u", row.busy ? 1u : 0u);
                ImGui::TableNextColumn();
                ImGui::Text("0x%08X", row.lastRpc);
                ImGui::TableNextColumn();
                ImGui::Text("0x%08X", row.readable ? row.cd.command : 0u);
                ImGui::TableNextColumn();
                ImGui::Text("0x%08X", row.readable ? row.cd.buf : 0u);
                ImGui::TableNextColumn();
                ImGui::Text("0x%08X", row.readable ? row.cd.cbuf : 0u);
                ImGui::TableNextColumn();
                ImGui::Text("0x%08X", row.readable ? row.cd.server : 0u);
            }
            ImGui::EndTable();
        }
    }

    void drawRpcHistoryTab(PS2Runtime &runtime)
    {
        const ps2x::iop::DebugSnapshot iopSnapshot = runtime.iopDebugSnapshot();
        std::vector<SifRpcDebugEvent> events;
        uint64_t nextSeq = 0;
        {
            std::lock_guard<std::mutex> lock(g_rpc_mutex);
            nextSeq = g_sif_rpc_debug_next_seq;
            events.reserve(kSifRpcDebugHistoryCount);
            for (size_t i = 0; i < kSifRpcDebugHistoryCount; ++i)
            {
                const SifRpcDebugEvent &event = g_sif_rpc_debug_history[i];
                if (event.seq != 0u)
                {
                    events.push_back(event);
                }
            }
        }
        std::sort(events.begin(), events.end(), [](const SifRpcDebugEvent &a, const SifRpcDebugEvent &b)
                  { return a.seq > b.seq; });

        ImGui::Text("RPC events: %zu captured, nextSeq=%llu", events.size(), static_cast<unsigned long long>(nextSeq));
        ImGui::TextDisabled("Ring buffer fed by SifInitRpc/SifBindRpc/SifRegisterRpc/SifCallRpc.");

        static bool onlyCalls = false;
        static bool hideBindNoise = false;
        ImGui::Checkbox("Only CallRpc", &onlyCalls);
        ImGui::SameLine();
        ImGui::Checkbox("Hide bind/register/init", &hideBindNoise);

        ImGui::SeparatorText("IOP/RPC tracer");
        ImGui::TextDisabled("Unhandled rows are RPC calls that no HLE service or EE server callback consumed; the runtime used copy/zero fallback.");
        static bool tracerOnlyUnhandled = true;
        ImGui::Checkbox("Only unhandled", &tracerOnlyUnhandled);
        if (ImGui::BeginTable("iop_rpc_tracer", 9,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                              ImVec2(0, 220)))
        {
            ImGui::TableSetupColumn("Seq");
            ImGui::TableSetupColumn("SID");
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Rpc#");
            ImGui::TableSetupColumn("PC");
            ImGui::TableSetupColumn("Send");
            ImGui::TableSetupColumn("Recv");
            ImGui::TableSetupColumn("Send[0..15]");
            ImGui::TableSetupColumn("Recv[0..15]");
            ImGui::TableHeadersRow();

            for (const SifRpcDebugEvent &event : events)
            {
                const char *op = event.op ? event.op : "";
                if (std::strcmp(op, "CallRpc") != 0)
                {
                    continue;
                }
                const bool unhandled = (event.flags & kSifRpcDebugFlagUnhandled) != 0u;
                if (tracerOnlyUnhandled && !unhandled)
                {
                    continue;
                }

                const std::string sendPreview = rpcPreviewBytes(event.sendPreview, event.sendPreviewSize);
                const std::string recvPreview = rpcPreviewBytes(event.recvPreview, event.recvPreviewSize);

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%llu", static_cast<unsigned long long>(event.seq));
                ImGui::TableNextColumn();
                ImGui::Text("0x%08X", event.sid);
                ImGui::TableNextColumn();
                const std::string serviceName = iopRpcSidName(iopSnapshot, event.sid);
                ImGui::TextUnformatted(serviceName.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("0x%08X", event.rpcNum);
                ImGui::TableNextColumn();
                ImGui::Text("0x%08X", event.pc);
                ImGui::TableNextColumn();
                ImGui::Text("0x%08X/%u", event.sendBuf, event.sendSize);
                ImGui::TableNextColumn();
                ImGui::Text("0x%08X/%u", event.recvBuf, event.recvSize);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(sendPreview.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(recvPreview.c_str());
            }
            ImGui::EndTable();
        }

        if (ImGui::BeginTable("rpc_history", 20,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                              ImVec2(0, 360)))
        {
            ImGui::TableSetupColumn("Seq");
            ImGui::TableSetupColumn("Op");
            ImGui::TableSetupColumn("TID");
            ImGui::TableSetupColumn("PC");
            ImGui::TableSetupColumn("RA");
            ImGui::TableSetupColumn("SID");
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Rpc#");
            ImGui::TableSetupColumn("Client");
            ImGui::TableSetupColumn("Server");
            ImGui::TableSetupColumn("Send");
            ImGui::TableSetupColumn("SSize");
            ImGui::TableSetupColumn("Recv");
            ImGui::TableSetupColumn("RSize");
            ImGui::TableSetupColumn("ResultPtr");
            ImGui::TableSetupColumn("Mode");
            ImGui::TableSetupColumn("EndFunc");
            ImGui::TableSetupColumn("EndParam");
            ImGui::TableSetupColumn("Sema");
            ImGui::TableSetupColumn("Flags");
            ImGui::TableHeadersRow();

            for (const SifRpcDebugEvent &event : events)
            {
                const char *op = event.op ? event.op : "";
                if (onlyCalls && std::strcmp(op, "CallRpc") != 0)
                {
                    continue;
                }
                if (hideBindNoise && std::strcmp(op, "CallRpc") != 0)
                {
                    continue;
                }

                const std::string flags = rpcDebugFlags(event.flags);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%llu", static_cast<unsigned long long>(event.seq));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(op);
                ImGui::TableNextColumn();
                ImGui::Text("%u", event.threadId);
                ImGui::TableNextColumn();
                ImGui::Text("0x%08X", event.pc);
                ImGui::TableNextColumn();
                ImGui::Text("0x%08X", event.ra);
                ImGui::TableNextColumn();
                ImGui::Text("0x%08X", event.sid);
                ImGui::TableNextColumn();
                const std::string serviceName = iopRpcSidName(iopSnapshot, event.sid);
                ImGui::TextUnformatted(serviceName.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("0x%08X", event.rpcNum);
                ImGui::TableNextColumn();
                ImGui::Text("0x%08X", event.clientPtr);
                ImGui::TableNextColumn();
                ImGui::Text("0x%08X", event.serverPtr);
                ImGui::TableNextColumn();
                ImGui::Text("0x%08X", event.sendBuf);
                ImGui::TableNextColumn();
                ImGui::Text("%u", event.sendSize);
                ImGui::TableNextColumn();
                ImGui::Text("0x%08X", event.recvBuf);
                ImGui::TableNextColumn();
                ImGui::Text("%u", event.recvSize);
                ImGui::TableNextColumn();
                ImGui::Text("0x%08X", event.resultPtr);
                ImGui::TableNextColumn();
                ImGui::Text("0x%08X", event.mode);
                ImGui::TableNextColumn();
                ImGui::Text("0x%08X", event.endFunc);
                ImGui::TableNextColumn();
                ImGui::Text("0x%08X", event.endParam);
                ImGui::TableNextColumn();
                ImGui::Text("0x%08X", event.semaId);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(flags.c_str());
            }
            ImGui::EndTable();
        }
    }

    void drawPadBytes(const uint8_t *data, size_t size)
    {
        char bytes[3 * 32 + 1]{};
        const size_t count = std::min<size_t>(size, 32u);
        for (size_t i = 0; i < count; ++i)
        {
            std::snprintf(bytes + (i * 3), sizeof(bytes) - (i * 3), "%02X ", data[i]);
        }
        ImGui::TextUnformatted(bytes);
    }

    void drawPadTab()
    {
        const ps2_stubs::PadDebugSnapshot snapshot = ps2_stubs::getPadDebugSnapshot();

        ImGui::SeparatorText("PAD global state");
        ImGui::Text("override=%u readLogCount=%d", snapshot.overrideEnabled ? 1u : 0u, snapshot.readLogCount);
        ImGui::SameLine();
        ImGui::Text("override buttons=0x%04X lx=%u ly=%u rx=%u ry=%u",
                    snapshot.overrideButtons,
                    snapshot.overrideLx,
                    snapshot.overrideLy,
                    snapshot.overrideRx,
                    snapshot.overrideRy);
        ImGui::Text("override pressed: %s", pressedPadButtons(snapshot.overrideButtons).c_str());

        if (ImGui::BeginTable("pad_ports", 15, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
        {
            ImGui::TableSetupColumn("Port");
            ImGui::TableSetupColumn("Slot");
            ImGui::TableSetupColumn("Open");
            ImGui::TableSetupColumn("Mode");
            ImGui::TableSetupColumn("Pressure");
            ImGui::TableSetupColumn("Req");
            ImGui::TableSetupColumn("DMA");
            ImGui::TableSetupColumn("Read#");
            ImGui::TableSetupColumn("ReadAddr");
            ImGui::TableSetupColumn("Source");
            ImGui::TableSetupColumn("Buttons raw");
            ImGui::TableSetupColumn("Pressed");
            ImGui::TableSetupColumn("LX/LY");
            ImGui::TableSetupColumn("RX/RY");
            ImGui::TableSetupColumn("Mask");
            ImGui::TableHeadersRow();

            for (size_t port = 0; port < ps2_stubs::kPadDebugPortCount; ++port)
            {
                for (size_t slot = 0; slot < ps2_stubs::kPadDebugSlotCount; ++slot)
                {
                    const ps2_stubs::PadDebugPortSnapshot &row = snapshot.ports[port][slot];
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%zu", port);
                    ImGui::TableNextColumn();
                    ImGui::Text("%zu", slot);
                    ImGui::TableNextColumn();
                    ImGui::Text("%u", row.open ? 1u : 0u);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(row.analogMode ? "analog" : "digital");
                    ImGui::TableNextColumn();
                    ImGui::Text("%u", row.pressureEnabled ? 1u : 0u);
                    ImGui::TableNextColumn();
                    ImGui::Text("0x%08X", row.reqState);
                    ImGui::TableNextColumn();
                    ImGui::Text("0x%08X", row.dmaAddr);
                    ImGui::TableNextColumn();
                    ImGui::Text("%u", row.readCount);
                    ImGui::TableNextColumn();
                    ImGui::Text("0x%08X", row.lastReadDataAddr);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(row.lastUsedOverride ? "override" : (row.lastUsedBackend ? "backend" : "fallback"));
                    ImGui::TableNextColumn();
                    ImGui::Text("0x%04X", row.lastButtons);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(pressedPadButtons(row.lastButtons).c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%u/%u", row.lx, row.ly);
                    ImGui::TableNextColumn();
                    ImGui::Text("%u/%u", row.rx, row.ry);
                    ImGui::TableNextColumn();
                    ImGui::Text("0x%04X", row.buttonMask);
                }
            }
            ImGui::EndTable();
        }

        ImGui::SeparatorText("Last scePadRead bytes");
        for (size_t port = 0; port < ps2_stubs::kPadDebugPortCount; ++port)
        {
            for (size_t slot = 0; slot < ps2_stubs::kPadDebugSlotCount; ++slot)
            {
                const ps2_stubs::PadDebugPortSnapshot &row = snapshot.ports[port][slot];
                ImGui::Text("port=%zu slot=%zu ok=%u data:", port, slot, row.lastReadOk ? 1u : 0u);
                ImGui::SameLine();
                drawPadBytes(row.lastData, ps2_stubs::kPadDebugDataSize);
            }
        }

        ImGui::SeparatorText("PS2 active-low button layout");
        ImGui::TextUnformatted("byte[2:3] raw is active-low: 0 means pressed, 1 means released.");
        ImGui::TextUnformatted("bits: Select,L3,R3,Start,Up,Right,Down,Left,L2,R2,L1,R1,Triangle,Circle,Cross,Square");
    }

    void drawModsTab()
    {
        const std::vector<ps2x::mods::ModInfo> mods = ps2x::mods::mods();
        const std::filesystem::path root = ps2x::mods::modsRoot();
        ImGui::Text("Mods folder: %s", root.empty() ? "(off, PS2X_MODS=0)" : root.string().c_str());
        ImGui::TextDisabled("Edit mods/<mod>/mod.ini and restart to enable, disable or reorder.");
        ImGui::Separator();
        if (mods.empty())
        {
            ImGui::TextUnformatted("No mods installed.");
            return;
        }
        if (ImGui::BeginTable("mods", 8, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit))
        {
            ImGui::TableSetupColumn("On");
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Version");
            ImGui::TableSetupColumn("Priority");
            ImGui::TableSetupColumn("Files");
            ImGui::TableSetupColumn("Textures");
            ImGui::TableSetupColumn("Opened");
            ImGui::TableSetupColumn("Code");
            ImGui::TableHeadersRow();
            for (const ps2x::mods::ModInfo &mod : mods)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(mod.enabled ? "yes" : "no");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(mod.name.c_str());
                if (ImGui::IsItemHovered() && (!mod.description.empty() || !mod.author.empty()))
                    ImGui::SetTooltip("%s\n%s%s",mod.description.c_str(), mod.author.empty() ? "" : "by ", mod.author.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(mod.version.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%d", mod.priority);
                ImGui::TableNextColumn();
                ImGui::Text("%zu", mod.fileCount);
                ImGui::TableNextColumn();
                ImGui::Text("%zu", mod.textureCount);
                ImGui::TableNextColumn();
                ImGui::Text("%zu", mod.overridesUsed);
                ImGui::TableNextColumn();
                if (!mod.code.empty())
                    ImGui::Text("%s (%s)", mod.code.c_str(), mod.codeStatus.empty() ? "not started" : mod.codeStatus.c_str());
            }
            ImGui::EndTable();
        }
    }

    void drawGsTab(PS2Runtime &runtime)
    {
        GSRegisters &regs = runtime.memory().gs();
        const GSDebugSnapshot gs = runtime.gs().getDebugSnapshot();

        ImGui::SeparatorText("GS private registers");
        if (ImGui::BeginTable("gspriv", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
        {
            auto row64 = [](const char *name, uint64_t value)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(name);
                ImGui::TableNextColumn();
                ImGui::Text("0x%016llX", static_cast<unsigned long long>(value));
                ImGui::TableNextColumn();
                ImGui::Text("%llu", static_cast<unsigned long long>(value));
                ImGui::TableNextColumn();
                ImGui::Text("lo=0x%08X hi=0x%08X", static_cast<uint32_t>(value), static_cast<uint32_t>(value >> 32));
            };
            ImGui::TableSetupColumn("Reg");
            ImGui::TableSetupColumn("Hex64");
            ImGui::TableSetupColumn("Dec");
            ImGui::TableSetupColumn("Split");
            ImGui::TableHeadersRow();
            row64("PMODE", regs.pmode);
            row64("SMODE2", regs.smode2);
            row64("DISPFB1", regs.dispfb1);
            row64("DISPLAY1", regs.display1);
            row64("DISPFB2", regs.dispfb2);
            row64("DISPLAY2", regs.display2);
            row64("BGCOLOR", regs.bgcolor);
            row64("CSR", regs.csr.load());
            row64("IMR", regs.imr);
            row64("BUSDIR", regs.busdir);
            row64("SIGLBLID", regs.siglblid);
            ImGui::EndTable();
        }

        ImGui::SeparatorText("GS draw state");
        ImGui::Text("PRIM type=%s iip=%u tme=%u fge=%u abe=%u aa1=%u fst=%u ctxt=%u fix=%u",
                    primTypeName(gs.prim.type), gs.prim.iip, gs.prim.tme, gs.prim.fge, gs.prim.abe,
                    gs.prim.aa1, gs.prim.fst, gs.prim.ctxt, gs.prim.fix);
        ImGui::Text("TEXA ta0=0x%02X aem=%u ta1=0x%02X | TEXCLUT cbw=%u cou=%u cov=%u",
                    gs.texa.ta0, gs.texa.aem ? 1u : 0u, gs.texa.ta1, gs.texclut.cbw, gs.texclut.cou, gs.texclut.cov);
        ImGui::Text("BITBLT sbp=%u sbw=%u spsm=0x%02X dbp=%u dbw=%u dpsm=0x%02X",
                    gs.bitbltbuf.sbp, gs.bitbltbuf.sbw, gs.bitbltbuf.spsm, gs.bitbltbuf.dbp, gs.bitbltbuf.dbw, gs.bitbltbuf.dpsm);
        ImGui::Text("TRXPOS ss=(%u,%u) ds=(%u,%u) dir=%u | TRXREG %ux%u TRXDIR=%u copied=%u/%u at=(%u,%u)",
                    gs.trxpos.ssax, gs.trxpos.ssay, gs.trxpos.dsax, gs.trxpos.dsay, gs.trxpos.dir,
                    gs.trxreg.rrw, gs.trxreg.rrh, gs.trxdir, gs.transferCopiedPixels, gs.transferTotalPixels,
                    gs.transferX, gs.transferY);
        ImGui::Text("Host presentation: has=%u size=%ux%u displayFbp=%u sourceFbp=%u preferred=%u lastDisplayBaseBytes=0x%08X localToHostPending=%zu",
                    gs.hasHostPresentationFrame ? 1u : 0u,
                    gs.hostPresentationWidth,
                    gs.hostPresentationHeight,
                    gs.hostPresentationDisplayFbp,
                    gs.hostPresentationSourceFbp,
                    gs.hostPresentationUsedPreferred ? 1u : 0u,
                    gs.lastDisplayBaseBytes,
                    gs.localToHostPendingBytes);
        ImGui::Text("Preferred source: has=%u frame fbp=%u fbw=%u psm=0x%02X destFbp=%u",
                    gs.hasPreferredDisplaySource ? 1u : 0u,
                    gs.preferredDisplaySourceFrame.fbp,
                    gs.preferredDisplaySourceFrame.fbw,
                    gs.preferredDisplaySourceFrame.psm,
                    gs.preferredDisplayDestFbp);

        drawGsContext("Context 0", gs.ctx[0]);
        drawGsContext("Context 1", gs.ctx[1]);

        ImGui::SeparatorText("GS history");
        bool gsHistoryPaused = runtime.gs().isDebugHistoryPaused();
        std::vector<GSDebugHistoryEntry> history = runtime.gs().getDebugHistory();
        uint32_t latestFrame = 0u;
        if (!history.empty())
        {
            latestFrame = history.back().frameIndex;
        }

        static bool s_gsHistoryCurrentFrameOnly = false;
        static bool s_gsHistoryDrawsOnly = false;
        static std::string s_lastGsDumpPath;
        static std::string s_lastGsDumpError;

        ImGui::Text("Ring entries: %zu / 512%s", history.size(), gsHistoryPaused ? " (paused)" : "");
        ImGui::SameLine();
        if (ImGui::Button("Clear GS history"))
        {
            runtime.gs().clearDebugHistory();
            history.clear();
            latestFrame = 0u;
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("Pause capture", &gsHistoryPaused))
        {
            runtime.gs().setDebugHistoryPaused(gsHistoryPaused);
        }
        ImGui::SameLine();
        if (ImGui::Button("Dump GS TXT"))
        {
            s_lastGsDumpError.clear();
            const std::filesystem::path dumpPath = makeDebugDumpPath("gs_history", latestFrame);
            if (writeGsDebugDump(dumpPath, regs, gs, history, s_gsHistoryCurrentFrameOnly, s_gsHistoryDrawsOnly, latestFrame, s_lastGsDumpError))
            {
                s_lastGsDumpPath = dumpPath.string();
                ImGui::SetClipboardText(s_lastGsDumpPath.c_str());
            }
            else
            {
                s_lastGsDumpPath.clear();
            }
        }
        if (gsHistoryPaused)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("history capture stopped");
        }

        ImGui::Checkbox("Current frame only", &s_gsHistoryCurrentFrameOnly);
        ImGui::SameLine();
        ImGui::Checkbox("Draws only", &s_gsHistoryDrawsOnly);
        if (!s_lastGsDumpPath.empty())
        {
            ImGui::TextDisabled("Last dump copied to clipboard: %s", s_lastGsDumpPath.c_str());
        }
        if (!s_lastGsDumpError.empty())
        {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "Dump failed: %s", s_lastGsDumpError.c_str());
        }

        if (ImGui::BeginTable("gs_history", 16, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX, ImVec2(0, 260)))
        {
            ImGui::TableSetupColumn("Seq");
            ImGui::TableSetupColumn("Frame");
            ImGui::TableSetupColumn("Tick");
            ImGui::TableSetupColumn("Kind");
            ImGui::TableSetupColumn("Prim");
            ImGui::TableSetupColumn("Ctx");
            ImGui::TableSetupColumn("FBP/PSM");
            ImGui::TableSetupColumn("ZBP/PSM");
            ImGui::TableSetupColumn("TEST");
            ImGui::TableSetupColumn("ALPHA");
            ImGui::TableSetupColumn("TEX0");
            ImGui::TableSetupColumn("XY");
            ImGui::TableSetupColumn("Z");
            ImGui::TableSetupColumn("A");
            ImGui::TableSetupColumn("GIF/REG/XFER");
            ImGui::TableSetupColumn("Present");
            ImGui::TableHeadersRow();

            for (auto it = history.rbegin(); it != history.rend(); ++it)
            {
                const GSDebugHistoryEntry &row = *it;
                if (s_gsHistoryCurrentFrameOnly && row.frameIndex != latestFrame)
                {
                    continue;
                }
                if (s_gsHistoryDrawsOnly && row.kind != GSDebugEventKind::Draw)
                {
                    continue;
                }

                const uint64_t test = row.test;
                const uint64_t alpha = row.alpha;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%llu", static_cast<unsigned long long>(row.seq));
                ImGui::TableNextColumn();
                ImGui::Text("%u", row.frameIndex);
                ImGui::TableNextColumn();
                ImGui::Text("%llu", static_cast<unsigned long long>(row.vsyncTick));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(gsDebugEventKindName(row.kind));
                ImGui::TableNextColumn();
                ImGui::Text("%s", primTypeName(row.prim.type));
                ImGui::TableNextColumn();
                ImGui::Text("%u", row.prim.ctxt ? 1u : 0u);
                ImGui::TableNextColumn();
                ImGui::Text("%u/%u/0x%02X", row.frame.fbp, row.frame.fbw, row.frame.psm);
                ImGui::TableNextColumn();
                ImGui::Text("%u/0x%02X m=%u", row.zbuf.zbp, row.zbuf.psm, row.zbuf.zmask ? 1u : 0u);
                ImGui::TableNextColumn();
                ImGui::Text("ATE=%llu ATST=%llu AREF=%02llX AFAIL=%llu ZTE=%llu ZTST=%llu",
                            (test >> 0) & 1ull,
                            (test >> 1) & 7ull,
                            (test >> 4) & 0xffull,
                            (test >> 12) & 3ull,
                            (test >> 16) & 1ull,
                            (test >> 17) & 3ull);
                ImGui::TableNextColumn();
                ImGui::Text("0x%016llX", static_cast<unsigned long long>(alpha));
                ImGui::TableNextColumn();
                ImGui::Text("tbp=%u psm=0x%02X %ux%u cbp=%u cpsm=0x%02X",
                            row.tex0.tbp0,
                            row.tex0.psm,
                            1u << row.tex0.tw,
                            1u << row.tex0.th,
                            row.tex0.cbp,
                            row.tex0.cpsm);
                ImGui::TableNextColumn();
                if (row.kind == GSDebugEventKind::Draw)
                    ImGui::Text("%.1f..%.1f / %.1f..%.1f", row.xMin, row.xMax, row.yMin, row.yMax);
                else
                    ImGui::TextUnformatted("-");
                ImGui::TableNextColumn();
                if (row.kind == GSDebugEventKind::Draw)
                    ImGui::Text("%.0f..%.0f", row.zMin, row.zMax);
                else
                    ImGui::TextUnformatted("-");
                ImGui::TableNextColumn();
                if (row.kind == GSDebugEventKind::Draw)
                    ImGui::Text("%u..%u", row.aMin, row.aMax);
                else
                    ImGui::TextUnformatted("-");
                ImGui::TableNextColumn();
                if (row.kind == GSDebugEventKind::GifTag)
                {
                    ImGui::Text("%s nloop=%u nreg=%u size=%u", gifFormatName(row.gifFlg), row.gifNloop, row.gifNreg, row.gifSizeBytes);
                }
                else if (row.kind == GSDebugEventKind::Register)
                {
                    ImGui::Text("%s(0x%02X)=0x%016llX", gsRegName(row.reg), row.reg, static_cast<unsigned long long>(row.regValue));
                }
                else if (row.kind == GSDebugEventKind::Transfer)
                {
                    ImGui::Text("TRXDIR=%u %ux%u pix=%u sbp=%u dbp=%u", row.trxdir, row.trxreg.rrw, row.trxreg.rrh, row.transferPixels, row.bitbltbuf.sbp, row.bitbltbuf.dbp);
                }
                else
                {
                    ImGui::TextUnformatted("-");
                }
                ImGui::TableNextColumn();
                if (row.kind == GSDebugEventKind::Present)
                {
                    ImGui::Text("%ux%u display=%u source=%u pref=%u", row.width, row.height, row.displayFbp, row.sourceFbp, row.usedPreferred ? 1u : 0u);
                }
                else
                {
                    ImGui::TextUnformatted("-");
                }
            }
            ImGui::EndTable();
        }
    }

    void drawFileCdTab(PS2Runtime &runtime)
    {
        (void)runtime;

        const PS2Runtime::IoPaths &ioPaths = PS2Runtime::getIoPaths();
        ImGui::SeparatorText("Runtime IO paths");
        textPath("ELF", ioPaths.elfPath);
        textPath("ELF dir", ioPaths.elfDirectory);
        textPath("Host root", ioPaths.hostRoot);
        textPath("CD root", ioPaths.cdRoot);
        textPath("CD image", ioPaths.cdImage);
        textPath("MC root", ioPaths.mcRoot);

        bool pathsInitialized = false;
        std::filesystem::path hostBase;
        std::filesystem::path cdromBase;
        std::filesystem::path hostCwd;
        std::filesystem::path cdromCwd;
        std::string cwdDevice;
        {
            std::lock_guard<std::mutex> lock(g_ps2_path_mutex);
            pathsInitialized = g_ps2_paths_initialized;
            hostBase = g_host_base;
            cdromBase = g_cdrom_base;
            hostCwd = g_host_cwd;
            cdromCwd = g_cdrom_cwd;
            cwdDevice = g_ps2_cwd_device;
        }

        ImGui::SeparatorText("PS2 path resolver");
        ImGui::Text("initialized=%u cwdDevice=%s", pathsInitialized ? 1u : 0u, cwdDevice.c_str());
        textPath("host base", hostBase);
        textPath("cdrom base", cdromBase);
        textPath("host cwd", hostCwd);
        textPath("cdrom cwd", cdromCwd);

        struct FdRow
        {
            int fd = 0;
            std::string device;
            std::string path;
        };

        std::vector<FdRow> fds;
        for (const PS2VfsDescriptorInfo &descriptor : runtime.vfs().descriptors())
        {
            fds.push_back({descriptor.descriptor, descriptor.device, descriptor.path});
        }
        std::sort(fds.begin(), fds.end(), [](const FdRow &a, const FdRow &b)
                  { return a.fd < b.fd; });

        ImGui::SeparatorText("FileIO descriptors");
        ImGui::Text("Open VFS descriptors: %zu", fds.size());
        if (ImGui::BeginTable("fileio_fds", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable, ImVec2(0, 90)))
        {
            ImGui::TableSetupColumn("FD");
            ImGui::TableSetupColumn("Device");
            ImGui::TableSetupColumn("Path");
            ImGui::TableHeadersRow();
            for (const FdRow &row : fds)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%d", row.fd);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(row.device.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(row.path.c_str());
            }
            ImGui::EndTable();
        }

        const ps2_stubs::CdDebugSnapshot cd = ps2_stubs::getCdDebugSnapshot();
        ImGui::SeparatorText("CDVD / sceCd state");
        ImGui::Text("initialized=%u lastError=%d mode=0x%08X streamingLbn=0x%08X endLbn=0x%08X nextPseudoLbn=0x%08X",
                    cd.initialized ? 1u : 0u,
                    cd.lastError,
                    cd.mode,
                    cd.streamingLbn,
                    cd.streamingEndLbn,
                    cd.nextPseudoLbn);
        ImGui::Text("imageValid=%u imageSize=%llu leafIndexBuilt=%u leafIndex=%zu looseIndex=%zu registeredFiles=%zu",
                    cd.imageSizeValid ? 1u : 0u,
                    static_cast<unsigned long long>(cd.imageSizeBytes),
                    cd.leafIndexBuilt ? 1u : 0u,
                    cd.leafIndexCount,
                    cd.loosePathIndexCount,
                    cd.files.size());
        textPath("CD resolved root", cd.cdRoot);
        textPath("CD image path", cd.cdImage);
        textPath("CD image size path", cd.imageSizePath);
        textPath("CD leaf index root", cd.leafIndexRoot);

        if (ImGui::BeginTable("cd_files", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0, 180)))
        {
            ImGui::TableSetupColumn("Key");
            ImGui::TableSetupColumn("Base LBN");
            ImGui::TableSetupColumn("End LBN");
            ImGui::TableSetupColumn("Sectors");
            ImGui::TableSetupColumn("Bytes");
            ImGui::TableSetupColumn("Host path");
            ImGui::TableHeadersRow();
            for (const ps2_stubs::CdDebugFileEntry &row : cd.files)
            {
                const uint32_t endLbn = row.baseLbn + row.sectors;
                const std::string hostPath = pathToString(row.hostPath);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(row.key.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("0x%08X", row.baseLbn);
                ImGui::TableNextColumn();
                ImGui::Text("0x%08X", endLbn);
                ImGui::TableNextColumn();
                ImGui::Text("%u", row.sectors);
                ImGui::TableNextColumn();
                ImGui::Text("%u", row.sizeBytes);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(hostPath.c_str());
            }
            ImGui::EndTable();
        }

        const ps2_stubs::MemoryCardDebugSnapshot mc = ps2_stubs::getMemoryCardDebugSnapshot();
        ImGui::SeparatorText("Memory card state");
        ImGui::Text("lastCmd=0x%X lastResult=%d nextFd=%d cvCursor=%d openFiles=%zu",
                    mc.lastCmd,
                    mc.lastResult,
                    mc.nextFd,
                    mc.cvFileCursor,
                    mc.openFiles.size());

        if (ImGui::BeginTable("mc_ports", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
        {
            ImGui::TableSetupColumn("Port");
            ImGui::TableSetupColumn("Formatted");
            ImGui::TableSetupColumn("Current dir");
            ImGui::TableSetupColumn("Host root");
            ImGui::TableHeadersRow();
            for (const ps2_stubs::MemoryCardDebugPort &port : mc.ports)
            {
                const std::string root = pathToString(port.rootPath);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%d", port.port);
                ImGui::TableNextColumn();
                ImGui::Text("%u", port.formatted ? 1u : 0u);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(port.currentDir.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(root.c_str());
            }
            ImGui::EndTable();
        }

        if (ImGui::BeginTable("mc_open_files", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0, 120)))
        {
            ImGui::TableSetupColumn("FD");
            ImGui::TableSetupColumn("Port");
            ImGui::TableSetupColumn("Host path");
            ImGui::TableHeadersRow();
            for (const ps2_stubs::MemoryCardDebugOpenFile &row : mc.openFiles)
            {
                const std::string path = pathToString(row.hostPath);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%d", row.fd);
                ImGui::TableNextColumn();
                ImGui::Text("%d", row.port);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(path.c_str());
            }
            ImGui::EndTable();
        }
    }

    void drawIoTab(PS2Runtime &runtime)
    {
        PS2Memory &mem = runtime.memory();
        ImGui::SeparatorText("DMA/VIF/GIF IO registers");
        struct RegRow
        {
            const char *name;
            uint32_t addr;
        };
        static constexpr RegRow rows[] = {
            {"D_STAT", 0x1000e010u},
            {"VIF0_STAT", 0x10003800u},
            {"VIF0_FBRST", 0x10003810u},
            {"VIF0_ERR", 0x10003830u},
            {"VIF1_STAT", 0x10003c00u},
            {"VIF1_FBRST", 0x10003c10u},
            {"VIF1_ERR", 0x10003c30u},
            {"VIF0_CHCR", 0x10008000u},
            {"VIF0_MADR", 0x10008010u},
            {"VIF0_QWC", 0x10008020u},
            {"VIF0_TADR", 0x10008030u},
            {"VIF1_CHCR", 0x10009000u},
            {"VIF1_MADR", 0x10009010u},
            {"VIF1_QWC", 0x10009020u},
            {"VIF1_TADR", 0x10009030u},
            {"GIF_CHCR", 0x1000a000u},
            {"GIF_MADR", 0x1000a010u},
            {"GIF_QWC", 0x1000a020u},
            {"GIF_TADR", 0x1000a030u},
            {"IPU_FROM_CHCR", 0x1000b000u},
            {"IPU_TO_CHCR", 0x1000b400u},
        };

        if (ImGui::BeginTable("ioregs", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
        {
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Addr");
            ImGui::TableSetupColumn("Value");
            ImGui::TableHeadersRow();
            for (const auto &row : rows)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(row.name);
                ImGui::TableNextColumn();
                ImGui::Text("0x%08X", row.addr);
                ImGui::TableNextColumn();
                ImGui::Text("0x%08X", mem.readIORegister(row.addr));
            }
            ImGui::EndTable();
        }
    }

    bool writeRuntimeLogDump(const std::filesystem::path &path,
                             const std::vector<ps2_log::RuntimeLogEntry> &entries,
                             ImGuiTextFilter *filter,
                             std::string &error)
    {
        try
        {
            std::ofstream out(path, std::ios::out | std::ios::trunc);
            if (!out.is_open())
            {
                error = "failed to open dump path";
                return false;
            }

            out << "PS2 runtime log dump\n";
            out << "entries=" << entries.size() << "\n";
            out << "filter=" << (filter && filter->IsActive() ? filter->InputBuf : "<none>") << "\n";
            out << "ps2_log_txt=" << ps2_log::log_path() << "\n";
            out << "\n";
            out << "seq\ttext\n";

            for (const ps2_log::RuntimeLogEntry &entry : entries)
            {
                if (filter && filter->IsActive() && !filter->PassFilter(entry.text.c_str()))
                {
                    continue;
                }
                out << entry.seq << '\t' << entry.text;
                if (entry.text.empty() || entry.text.back() != '\n')
                {
                    out << '\n';
                }
            }
            return true;
        }
        catch (const std::exception &e)
        {
            error = e.what();
            return false;
        }
    }

    void drawLogsTab()
    {
        static ImGuiTextFilter s_logFilter;
        static bool s_autoScroll = true;
        static bool s_wrapText = false;
        static bool s_latestFirst = false;
        static std::string s_lastLogDumpPath;
        static std::string s_lastLogDumpError;

        bool paused = ps2_log::is_runtime_log_paused();
        std::vector<ps2_log::RuntimeLogEntry> entries = ps2_log::snapshot_runtime_log_entries();

        ImGui::SeparatorText("Runtime log ring buffer");
        ImGui::Text("Entries: %zu / %zu%s", entries.size(), ps2_log::kMaxRuntimeLogEntries, paused ? " (paused)" : "");
        ImGui::TextDisabled("Captures RUNTIME_LOG(...) calls. Direct std::cerr/std::cout writes are still console-only unless routed through RUNTIME_LOG.");
        ImGui::TextDisabled("Aggressive call trace file: %s", ps2_log::log_path().c_str());

        if (ImGui::Button("Clear logs"))
        {
            ps2_log::clear_runtime_log_entries();
            entries.clear();
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("Pause capture", &paused))
        {
            ps2_log::set_runtime_log_paused(paused);
        }
        ImGui::SameLine();
        ImGui::Checkbox("Auto-scroll", &s_autoScroll);
        ImGui::SameLine();
        ImGui::Checkbox("Wrap", &s_wrapText);
        ImGui::SameLine();
        ImGui::Checkbox("Latest first", &s_latestFirst);

        s_logFilter.Draw("Filter", 260.0f);
        ImGui::SameLine();
        if (ImGui::Button("Copy visible"))
        {
            std::ostringstream out;
            auto appendEntry = [&](const ps2_log::RuntimeLogEntry &entry)
            {
                if (s_logFilter.IsActive() && !s_logFilter.PassFilter(entry.text.c_str()))
                {
                    return;
                }
                out << entry.seq << '\t' << entry.text;
                if (entry.text.empty() || entry.text.back() != '\n')
                {
                    out << '\n';
                }
            };

            if (s_latestFirst)
            {
                for (auto it = entries.rbegin(); it != entries.rend(); ++it)
                {
                    appendEntry(*it);
                }
            }
            else
            {
                for (const ps2_log::RuntimeLogEntry &entry : entries)
                {
                    appendEntry(entry);
                }
            }
            const std::string text = out.str();
            ImGui::SetClipboardText(text.c_str());
        }
        ImGui::SameLine();
        if (ImGui::Button("Dump logs TXT"))
        {
            s_lastLogDumpError.clear();
            const std::filesystem::path dumpPath = makeDebugDumpPath("runtime_log", 0u);
            if (writeRuntimeLogDump(dumpPath, entries, &s_logFilter, s_lastLogDumpError))
            {
                s_lastLogDumpPath = dumpPath.string();
                ImGui::SetClipboardText(s_lastLogDumpPath.c_str());
            }
            else
            {
                s_lastLogDumpPath.clear();
            }
        }

        if (!s_lastLogDumpPath.empty())
        {
            ImGui::TextDisabled("Last dump: %s", s_lastLogDumpPath.c_str());
        }
        if (!s_lastLogDumpError.empty())
        {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "Dump failed: %s", s_lastLogDumpError.c_str());
        }

        ImGui::Separator();
        const ImGuiTableFlags tableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX;
        if (ImGui::BeginTable("runtime_logs", 2, tableFlags, ImVec2(0, 430)))
        {
            ImGui::TableSetupColumn("Seq", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("Text", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            auto drawEntry = [&](const ps2_log::RuntimeLogEntry &entry)
            {
                if (s_logFilter.IsActive() && !s_logFilter.PassFilter(entry.text.c_str()))
                {
                    return;
                }

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%llu", static_cast<unsigned long long>(entry.seq));
                ImGui::TableNextColumn();
                if (s_wrapText)
                {
                    ImGui::TextWrapped("%s", entry.text.c_str());
                }
                else
                {
                    ImGui::TextUnformatted(entry.text.c_str());
                }
            };

            if (s_latestFirst)
            {
                for (auto it = entries.rbegin(); it != entries.rend(); ++it)
                {
                    drawEntry(*it);
                }
            }
            else
            {
                for (const ps2_log::RuntimeLogEntry &entry : entries)
                {
                    drawEntry(entry);
                }
            }

            if (s_autoScroll && !s_latestFirst && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f)
            {
                ImGui::SetScrollHereY(1.0f);
            }
            ImGui::EndTable();
        }
    }

#endif
}
#if defined(PS2X_ENABLE_DEBUG_UI) && !defined(PLATFORM_VITA)
namespace
{
    // S2RUi (s2r_mod_api.h) on top of Dear ImGui, for code mods.
    std::string uiFormat(const char *fmt, va_list args)
    {
        char buffer[2048];
        const int n = std::vsnprintf(buffer, sizeof(buffer), fmt ? fmt : "", args);
        return n < 0 ? std::string() : std::string(buffer, std::min<size_t>(static_cast<size_t>(n), sizeof(buffer) - 1));
    }

    ImU32 uiColor(uint32_t rgba)
    {
        return IM_COL32((rgba >> 24) & 0xFF, (rgba >> 16) & 0xFF, (rgba >> 8) & 0xFF, rgba & 0xFF);
    }

    void uiText(const char *fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        const std::string text = uiFormat(fmt, args);
        va_end(args);
        ImGui::TextUnformatted(text.c_str());
    }

    void uiTextDisabled(const char *fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        const std::string text = uiFormat(fmt, args);
        va_end(args);
        ImGui::TextDisabled("%s", text.c_str());
    }

    void uiTextWrapped(const char *fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        const std::string text = uiFormat(fmt, args);
        va_end(args);
        ImGui::TextWrapped("%s", text.c_str());
    }

    S2RUi makeModUi(bool menuOpen)
    {
        S2RUi ui{};
        ui.text = uiText;
        ui.text_disabled = uiTextDisabled;
        ui.text_wrapped = uiTextWrapped;
        ui.button = [](const char *label) -> int { return ImGui::Button(label) ? 1 : 0; };
        ui.checkbox = [](const char *label, int *value) -> int
        {
            bool b = value && *value != 0;
            const bool changed = ImGui::Checkbox(label, &b);
            if (changed && value)
                *value = b ? 1 : 0;
            return changed ? 1 : 0;
        };
        ui.input_text = [](const char *label, char *buffer, int size) -> int
        { return buffer && size > 0 && ImGui::InputText(label, buffer, static_cast<size_t>(size)) ? 1 : 0; };
        ui.input_int = [](const char *label, int *value) -> int { return value && ImGui::InputInt(label, value) ? 1 : 0; };
        ui.slider_int = [](const char *label, int *value, int min, int max) -> int
        { return value && ImGui::SliderInt(label, value, min, max) ? 1 : 0; };
        ui.slider_float = [](const char *label, float *value, float min, float max) -> int
        { return value && ImGui::SliderFloat(label, value, min, max) ? 1 : 0; };
        ui.selectable = [](const char *label, int selected) -> int { return ImGui::Selectable(label, selected != 0) ? 1 : 0; };
        ui.same_line = [] { ImGui::SameLine(); };
        ui.separator = [] { ImGui::Separator(); };
        ui.collapsing_header = [](const char *label) -> int
        { return ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen) ? 1 : 0; };
        ui.push_id = [](int id) { ImGui::PushID(id); };
        ui.pop_id = [] { ImGui::PopID(); };
        ui.begin = [](const char *title, int *open) -> int
        {
            bool b = !open || *open != 0;
            const bool visible = ImGui::Begin(title, open ? &b : nullptr, ImGuiWindowFlags_AlwaysAutoResize);
            if (open)
                *open = b ? 1 : 0;
            return visible ? 1 : 0;
        };
        ui.end = [] { ImGui::End(); };
        ui.hud_text = [](float x, float y, uint32_t color, const char *text)
        {
            if (!text)
                return;
            ImDrawList *draw = ImGui::GetForegroundDrawList();
            draw->AddText(ImVec2(x + 1.0f, y + 1.0f), IM_COL32(0, 0, 0, (color & 0xFF) * 3 / 4), text);
            draw->AddText(ImVec2(x, y), uiColor(color), text);
        };
        ui.hud_rect = [](float x, float y, float w, float h, uint32_t color)
        { ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), uiColor(color), 4.0f); };
        ui.screen_width = [] { return ImGui::GetIO().DisplaySize.x; };
        ui.screen_height = [] { return ImGui::GetIO().DisplaySize.y; };
        ui.menu_open = menuOpen ? 1 : 0;
        return ui;
    }

    // api->notify messages, bottom left, fading out over their last second.
    void drawModNotices()
    {
        const std::vector<ps2x::mods::Notice> notices = ps2x::mods::notices();
        if (notices.empty())
            return;
        ImDrawList *draw = ImGui::GetForegroundDrawList();
        const float lineHeight = ImGui::GetTextLineHeight() + 14.0f;
        float y = ImGui::GetIO().DisplaySize.y - 16.0f - lineHeight * static_cast<float>(notices.size());
        for (const ps2x::mods::Notice &notice : notices)
        {
            const float alpha = notice.age > 3.0 ? static_cast<float>(4.0 - notice.age) : 1.0f;
            const ImVec2 size = ImGui::CalcTextSize(notice.text.c_str());
            const int a = static_cast<int>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f);
            draw->AddRectFilled(ImVec2(12.0f, y), ImVec2(28.0f + size.x, y + size.y + 8.0f), IM_COL32(20, 24, 32, a * 4 / 5), 5.0f);
            draw->AddText(ImVec2(20.0f, y + 4.0f), IM_COL32(255, 255, 255, a), notice.text.c_str());
            y += lineHeight;
        }
    }
}
#endif

#if defined(PS2X_ENABLE_DEBUG_UI) && !defined(PLATFORM_VITA)
// Rounded system font for the save slot picker (null = ImGui's default font).
static ImFont *s_saveSlotFont = nullptr;
extern std::atomic<int> g_ps2xGlyphSet; // ps2_pad.cpp: 0 the game's icons, 1 keycaps, 2 xbox
#endif

void PS2DebugPanel::initialize()
{
#if defined(PS2X_ENABLE_DEBUG_UI) && !defined(PLATFORM_VITA)
    if (!m_initialized)
    {
        rlImGuiSetup(true);
        for (const char *path : {"C:/Windows/Fonts/segoeuib.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
                                 "/System/Library/Fonts/Supplemental/Arial Rounded Bold.ttf"})
        {
            std::error_code ec;
            if (std::filesystem::exists(path, ec))
            {
                s_saveSlotFont = ImGui::GetIO().Fonts->AddFontFromFileTTF(path, 20.0f);
                break;
            }
        }
        m_initialized = true;
    }
#endif
}

void PS2DebugPanel::shutdown()
{
#if defined(PS2X_ENABLE_DEBUG_UI) && !defined(PLATFORM_VITA)
    if (m_initialized)
    {
        rlImGuiShutdown();
        m_initialized = false;
    }
#endif
}

// Built-in save slots: when the game starts to load or save, it waits until a slot is picked here.
static void drawSaveSlotPicker()
{
    static bool s_open = false;
    static int s_port = 0;
    static int s_cursor = 0;
    static uint16_t s_lastPad = 0;
    static std::vector<ps2_stubs::SaveSlotInfo> s_slots;
    static int s_renameRow = -1; // slot row whose name is being typed; -1 = none
    static bool s_renameFocus = false;
    static char s_renameText[48] = {};

    const ps2_stubs::SaveSlotPickRequest request = ps2_stubs::pendingSaveSlotPick();
    if (!request.active)
    {
        if (s_open)
        {
            s_open = false;
            ps2PadSetBlocked(false);
        }
        return;
    }
    if (!s_open || s_port != request.port)
    {
        s_open = true;
        s_port = request.port;
        s_slots = ps2_stubs::listSaveSlots(request.port);
        const int active = ps2_stubs::activeSaveSlot(request.port);
        s_cursor = 0;
        for (size_t i = 0; i < s_slots.size(); ++i)
        {
            if (s_slots[i].index == active)
                s_cursor = static_cast<int>(i);
        }
        s_renameRow = -1;
        s_lastPad = ps2PadHostPressed();
        ps2PadSetBlocked(true);
    }

    // Rows: the slots, then "New empty slot", then "Cancel".
    const int newRow = static_cast<int>(s_slots.size());
    const int cancelRow = newRow + 1;
    const int newSlot = s_slots.empty() ? 1 : s_slots.back().index + 1;
    const int activeSlot = ps2_stubs::activeSaveSlot(s_port);
    const auto rowChoice = [&](int row) {
        return row < newRow ? s_slots[static_cast<size_t>(row)].index : row == newRow ? newSlot : -1;
    };
    const uint16_t pad = ps2PadHostPressed();
    uint16_t pressed = static_cast<uint16_t>(pad & ~s_lastPad);
    s_lastPad = pad;
    // The game's keys (ps2_pad.cpp) as pad bits: Space Cross, Backspace Circle, F Triangle, arrows and W/S
    // the d-pad. Read here as well since the game's own key mapping is off while this window has the
    // keyboard. Enter and Esc are Start in the game; here they choose and go back. A bit the pad state
    // shows while its key is held came from that key, so it is not counted twice.
    if (s_renameRow < 0)
    {
        if (IsKeyDown(KEY_SPACE))
            pressed &= ~0x4000;
        if (IsKeyDown(KEY_BACKSPACE))
            pressed &= ~0x2000;
        if (IsKeyDown(KEY_F))
            pressed &= ~0x1000;
        if (IsKeyDown(KEY_UP))
            pressed &= ~0x0010;
        if (IsKeyDown(KEY_DOWN))
            pressed &= ~0x0040;
        if (IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER))
            pressed |= 0x4000;
        if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressed(KEY_ESCAPE))
            pressed |= 0x2000;
        if (IsKeyPressed(KEY_F))
            pressed |= 0x1000;
        if (IsKeyPressed(KEY_UP) || IsKeyPressed(KEY_W))
            pressed |= 0x0010;
        if (IsKeyPressed(KEY_DOWN) || IsKeyPressed(KEY_S))
            pressed |= 0x0040;
    }
    const auto startRename = [&](int row) {
        s_cursor = row;
        s_renameRow = row;
        s_renameFocus = true;
        std::snprintf(s_renameText, sizeof(s_renameText), "%s", s_slots[static_cast<size_t>(row)].name.c_str());
    };
    // Enter (or Cross) keeps the typed name, Esc (or Circle) drops it; an empty name clears it.
    bool renameDone = false, renameCancel = false;
    bool cursorMoved = false;
    int choice = 0; // 0 = still choosing
    if (s_renameRow >= 0)
    {
        renameDone = (pressed & 0x4000) != 0;
        renameCancel = IsKeyPressed(KEY_ESCAPE) || (pressed & 0x2000);
    }
    else
    {
        // Cross chooses, Circle goes back, Triangle renames, the d-pad moves.
        if (pressed & 0x0010)
        {
            s_cursor = (s_cursor + cancelRow) % (cancelRow + 1);
            cursorMoved = true;
        }
        if (pressed & 0x0040)
        {
            s_cursor = (s_cursor + 1) % (cancelRow + 1);
            cursorMoved = true;
        }
        if (pressed & 0x4000)
            choice = rowChoice(s_cursor);
        if (pressed & 0x2000)
            choice = -1;
        if ((pressed & 0x1000) && s_cursor < newRow)
            startRename(s_cursor);
    }
    // The button icons the game shows now: 0 PlayStation, 1 keycaps, 2 Xbox.
    const int glyphSet = g_ps2xGlyphSet.load(std::memory_order_relaxed);

    // Drawn in the style of the game's own dialogs: lavender panel, dark blue frame, navy text.
    const ImU32 kFrame = IM_COL32(52, 62, 170, 255);
    const ImU32 kPanelTop = IM_COL32(168, 186, 240, 250);
    const ImU32 kPanelBottom = IM_COL32(122, 144, 222, 250);
    const ImU32 kInk = IM_COL32(26, 34, 112, 255);
    const ImU32 kInkSoft = IM_COL32(54, 66, 150, 255);
    const ImU32 kCard = IM_COL32(186, 202, 246, 255);
    const ImU32 kCardHot = IM_COL32(244, 247, 255, 255);
    const ImU32 kGreen = IM_COL32(74, 182, 72, 255);
    const ImU32 kWhite = IM_COL32(255, 255, 255, 255);

    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    const float s = std::clamp(viewport->Size.y / 720.0f, 0.9f, 2.4f);
    const float width = std::min(620.0f * s, viewport->Size.x - 32.0f);
    const float margin = 26.0f * s;
    const float rowH = 74.0f * s;
    const float rowGap = 10.0f * s;
    const float titleSize = 30.0f * s, bodySize = 22.0f * s, smallSize = 17.0f * s;

    ImGui::GetBackgroundDrawList()->AddRectFilled(viewport->Pos, ImVec2(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y),
                                                  IM_COL32(8, 12, 40, 150));

    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowFocus();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(margin, margin));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, rowGap));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushFont(s_saveSlotFont, bodySize);
    if (ImGui::Begin("Choose a save slot", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoScrollbar))
    {
        ImDrawList *draw = ImGui::GetWindowDrawList();
        draw->ChannelsSplit(2);
        draw->ChannelsSetCurrent(1);
        const float innerW = ImGui::GetContentRegionAvail().x;
        const auto centredText = [&](const char *text, float size, ImU32 col) {
            ImGui::PushFont(nullptr, size);
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const ImVec2 ts = ImGui::CalcTextSize(text);
            draw->AddText(ImVec2(at.x + (innerW - ts.x) * 0.5f, at.y), col, text);
            ImGui::Dummy(ImVec2(innerW, ts.y));
            ImGui::PopFont();
        };

        centredText("Choose a save slot", titleSize, kInk);
        char sub[64];
        std::snprintf(sub, sizeof(sub), "Memory card (8MB) in slot %d", s_port + 1);
        centredText(sub, smallSize, kInkSoft);
        ImGui::Dummy(ImVec2(innerW, 2.0f * s));

        // Slot list; scrolls once there are more slots than fit on screen.
        const float listH = std::min((rowH + rowGap) * static_cast<float>(newRow + 1) - rowGap, viewport->Size.y * 0.55f);
        ImGui::BeginChild("##slots", ImVec2(innerW, listH), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
        ImDrawList *list = ImGui::GetWindowDrawList();
        const bool mouseMoved = ImGui::GetIO().MouseDelta.x != 0.0f || ImGui::GetIO().MouseDelta.y != 0.0f;
        for (int row = 0; row < cancelRow; ++row)
        {
            ImGui::PushID(row);
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            const ImVec2 p1(p0.x + innerW, p0.y + rowH);
            const bool renaming = s_renameRow == row;
            ImGui::SetNextItemAllowOverlap(); // the Rename button and name field sit on the card
            if (ImGui::InvisibleButton("##row", ImVec2(innerW, rowH)) && s_renameRow < 0)
            {
                s_cursor = row;
                choice = rowChoice(row);
            }
            if (ImGui::IsItemHovered() && mouseMoved && s_renameRow < 0)
                s_cursor = row;
            const bool hot = s_cursor == row;
            if (hot && cursorMoved)
                ImGui::SetScrollHereY(0.5f);

            const bool isNew = row == newRow;
            const ps2_stubs::SaveSlotInfo *info = isNew ? nullptr : &s_slots[static_cast<size_t>(row)];
            const float r = 14.0f * s;
            if (hot)
                list->AddRect(ImVec2(p0.x - 1.0f, p0.y - 1.0f), ImVec2(p1.x + 1.0f, p1.y + 1.0f), IM_COL32(255, 255, 255, 120), r + 1.0f, 0, 5.0f * s);
            list->AddRectFilled(p0, p1, hot ? kCardHot : kCard, r);
            list->AddRect(p0, p1, hot ? kWhite : IM_COL32(96, 112, 200, 255), r, 0, (hot ? 3.0f : 1.5f) * s);

            // Number badge (a "+" for the new slot); green on the highlighted row, like the game's tabs.
            const float br = 22.0f * s;
            const ImVec2 bc(p0.x + 20.0f * s + br, p0.y + rowH * 0.5f);
            list->AddCircleFilled(bc, br, hot ? kGreen : kFrame);
            list->AddCircle(bc, br, kWhite, 0, 2.5f * s);
            ImGui::PushFont(nullptr, bodySize * 1.1f);
            const std::string badge = isNew ? "+" : std::to_string(info->index);
            const ImVec2 bs = ImGui::CalcTextSize(badge.c_str());
            list->AddText(ImVec2(bc.x - bs.x * 0.5f, bc.y - bs.y * 0.5f), kWhite, badge.c_str());
            ImGui::PopFont();

            const float tx = bc.x + br + 18.0f * s;
            // A named slot shows its name as the title and its number with the date.
            const bool named = !isNew && !info->name.empty();
            const std::string slotText = isNew ? std::string() : "Slot " + std::to_string(info->index);
            const std::string title = isNew ? "New slot" : named ? info->name : slotText;
            const std::string saved = isNew ? std::string() : info->saved.empty() ? std::string("Empty")
                                                                                   : "Last saved  " + info->saved;
            const std::string detail = isNew ? "Start fresh in empty slot " + std::to_string(newSlot)
                                       : named ? slotText + "  \xC2\xB7  " + saved
                                               : saved;
            const float titleH = ImGui::CalcTextSize(title.c_str()).y;
            ImGui::PushFont(nullptr, smallSize);
            const float detailH = ImGui::CalcTextSize(detail.c_str()).y;
            ImGui::PopFont();
            const float ty = p0.y + (rowH - titleH - detailH - 2.0f * s) * 0.5f;
            float rightX = p1.x - 18.0f * s; // the pills on the right are laid out leftwards from here
            ImGui::PushFont(nullptr, smallSize);
            if (!isNew && info->index == activeSlot)
            {
                const char *tag = "IN USE";
                const ImVec2 ts = ImGui::CalcTextSize(tag);
                const ImVec2 t1(rightX, p0.y + rowH * 0.5f + ts.y * 0.5f + 5.0f * s);
                const ImVec2 t0(t1.x - ts.x - 22.0f * s, t1.y - ts.y - 10.0f * s);
                list->AddRectFilled(t0, t1, kGreen, (t1.y - t0.y) * 0.5f);
                list->AddText(ImVec2(t0.x + 11.0f * s, t0.y + 5.0f * s), kWhite, tag);
                rightX = t0.x - 10.0f * s;
            }
            if (!isNew && hot && !renaming)
            {
                const char *text = "Rename";
                const ImVec2 ts = ImGui::CalcTextSize(text);
                const ImVec2 size(ts.x + 22.0f * s, ts.y + 10.0f * s);
                const ImVec2 b0(rightX - size.x, p0.y + (rowH - size.y) * 0.5f);
                ImGui::SetCursorScreenPos(b0);
                if (ImGui::InvisibleButton("##rename", size))
                    startRename(row);
                const bool over = ImGui::IsItemHovered();
                list->AddRectFilled(b0, ImVec2(b0.x + size.x, b0.y + size.y), over ? kFrame : kCard, size.y * 0.5f);
                list->AddRect(b0, ImVec2(b0.x + size.x, b0.y + size.y), kFrame, size.y * 0.5f, 0, 1.5f * s);
                list->AddText(ImVec2(b0.x + 11.0f * s, b0.y + 5.0f * s), over ? kWhite : kInk, text);
            }
            if (renaming)
            {
                // The name field takes the title's place; Enter keeps it.
                ImGui::PopFont();
                ImGui::SetCursorScreenPos(ImVec2(tx - 6.0f * s, ty - 3.0f * s));
                ImGui::SetNextItemWidth(rightX - tx - 10.0f * s);
                ImGui::PushStyleColor(ImGuiCol_FrameBg, kWhite);
                ImGui::PushStyleColor(ImGuiCol_Text, kInk);
                ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, IM_COL32(150, 200, 255, 255));
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f * s, 3.0f * s));
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f * s);
                if (s_renameFocus)
                {
                    ImGui::SetKeyboardFocusHere();
                    s_renameFocus = false;
                }
                if (ImGui::InputText("##name", s_renameText, sizeof(s_renameText),
                                     ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll))
                    renameDone = true;
                ImGui::PopStyleVar(2);
                ImGui::PopStyleColor(3);
                ImGui::PushFont(nullptr, smallSize);
            }
            else
            {
                ImGui::PopFont();
                list->AddText(ImVec2(tx, ty), kInk, title.c_str());
                ImGui::PushFont(nullptr, smallSize);
            }
            const char *renameHint = glyphSet == 0   ? "Type a name, Enter or Cross to keep, Esc or Circle to cancel"
                                     : glyphSet == 2 ? "Type a name, Enter or A to keep, Esc or B to cancel"
                                                     : "Type a name, Enter to keep, Esc to cancel";
            list->AddText(ImVec2(tx, ty + titleH + 2.0f * s), kInkSoft, renaming ? renameHint : detail.c_str());
            ImGui::PopFont();
            ImGui::SetCursorScreenPos(p0); // lay the row out again so the list goes on below it
            ImGui::Dummy(ImVec2(innerW, rowH));
            ImGui::PopID();
        }
        ImGui::EndChild();
        if (s_renameRow >= 0 && (renameDone || renameCancel))
        {
            if (renameDone)
            {
                ps2_stubs::setSaveSlotName(s_port, s_slots[static_cast<size_t>(s_renameRow)].index, s_renameText);
                s_slots = ps2_stubs::listSaveSlots(s_port);
            }
            s_renameRow = -1;
        }

        // Cancel: a smaller pill under the list.
        {
            ImGui::PushFont(nullptr, smallSize * 1.1f);
            const char *text = "Cancel";
            const ImVec2 ts = ImGui::CalcTextSize(text);
            const ImVec2 size(ts.x + 64.0f * s, ts.y + 18.0f * s);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (innerW - size.x) * 0.5f);
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            const ImVec2 p1(p0.x + size.x, p0.y + size.y);
            if (ImGui::InvisibleButton("##cancel", size))
            {
                s_cursor = cancelRow;
                choice = -1;
            }
            if (ImGui::IsItemHovered() && mouseMoved)
                s_cursor = cancelRow;
            const bool hot = s_cursor == cancelRow;
            draw->AddRectFilled(p0, p1, hot ? kCardHot : kCard, size.y * 0.5f);
            draw->AddRect(p0, p1, hot ? kWhite : IM_COL32(96, 112, 200, 255), size.y * 0.5f, 0, (hot ? 3.0f : 1.5f) * s);
            draw->AddText(ImVec2(p0.x + (size.x - ts.x) * 0.5f, p0.y + (size.y - ts.y) * 0.5f), kInk, text);
            ImGui::PopFont();
        }

        // Button hints for Cross / Circle / Triangle, drawn as the icons the game shows for the input in
        // use: PlayStation symbols, keycaps (Space / Backspace / F) or Xbox A / B / Y.
        {
            ImGui::PushFont(nullptr, smallSize);
            const float ir = 11.0f * s;
            const char *labels[3] = {"Choose", "Back", "Rename"};
            const char *keycaps[3] = {"Space", "Backspace", "F"};
            const char *xboxLetters[3] = {"A", "B", "Y"};
            const ImU32 xboxColours[3] = {IM_COL32(110, 200, 80, 255), IM_COL32(235, 75, 65, 255), IM_COL32(245, 195, 40, 255)};
            const float capPad = 7.0f * s;
            // Width of button b's icon; draws it at x when asked.
            const auto glyph = [&](int b, float x, float cy, bool drawIt) -> float {
                if (glyphSet == 1)
                {
                    ImGui::PushFont(nullptr, smallSize * 0.85f);
                    const ImVec2 ts = ImGui::CalcTextSize(keycaps[b]);
                    const float w = std::max(ir * 2.0f, ts.x + capPad * 2.0f);
                    if (drawIt)
                    {
                        const ImVec2 c0(x, cy - ir), c1(x + w, cy + ir);
                        draw->AddRectFilled(ImVec2(c0.x, c0.y + 2.0f * s), ImVec2(c1.x, c1.y + 2.0f * s), kFrame, 5.0f * s);
                        draw->AddRectFilled(c0, c1, kWhite, 5.0f * s);
                        draw->AddRect(c0, c1, kFrame, 5.0f * s, 0, 1.5f * s);
                        draw->AddText(ImVec2(x + (w - ts.x) * 0.5f, cy - ts.y * 0.5f), kInk, keycaps[b]);
                    }
                    ImGui::PopFont();
                    return w;
                }
                if (!drawIt)
                    return ir * 2.0f;
                const ImVec2 c(x + ir, cy);
                if (glyphSet == 2)
                {
                    draw->AddCircleFilled(c, ir, IM_COL32(36, 38, 46, 255));
                    draw->AddCircle(c, ir, IM_COL32(90, 94, 110, 255), 0, 1.2f * s);
                    ImGui::PushFont(nullptr, smallSize * 0.9f);
                    const ImVec2 ts = ImGui::CalcTextSize(xboxLetters[b]);
                    draw->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), xboxColours[b], xboxLetters[b]);
                    ImGui::PopFont();
                    return ir * 2.0f;
                }
                draw->AddCircleFilled(c, ir, kFrame);
                if (b == 0)
                {
                    const float k = ir * 0.45f;
                    draw->AddLine(ImVec2(c.x - k, cy - k), ImVec2(c.x + k, cy + k), IM_COL32(150, 200, 255, 255), 2.2f * s);
                    draw->AddLine(ImVec2(c.x - k, cy + k), ImVec2(c.x + k, cy - k), IM_COL32(150, 200, 255, 255), 2.2f * s);
                }
                else if (b == 1)
                    draw->AddCircle(c, ir * 0.5f, IM_COL32(255, 120, 140, 255), 0, 2.2f * s);
                else
                {
                    const float tr = ir * 0.5f;
                    draw->AddTriangle(ImVec2(c.x, cy - tr), ImVec2(c.x + tr * 0.87f, cy + tr * 0.5f),
                                      ImVec2(c.x - tr * 0.87f, cy + tr * 0.5f), IM_COL32(120, 230, 200, 255), 2.2f * s);
                }
                return ir * 2.0f;
            };
            const float gap = 8.0f * s, group = 24.0f * s;
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const float lineH = std::max(ir * 2.0f + 2.0f * s, ImGui::GetFontSize());
            const float cy = at.y + lineH * 0.5f;
            const float textY = cy - ImGui::GetFontSize() * 0.5f;
            float total = group * 2.0f;
            for (int b = 0; b < 3; ++b)
                total += glyph(b, 0.0f, cy, false) + gap + ImGui::CalcTextSize(labels[b]).x;
            float x = at.x + (innerW - total) * 0.5f;
            for (int b = 0; b < 3; ++b)
            {
                x += glyph(b, x, cy, true) + gap;
                draw->AddText(ImVec2(x, textY), kInk, labels[b]);
                x += ImGui::CalcTextSize(labels[b]).x + group;
            }
            ImGui::Dummy(ImVec2(innerW, lineH));
            ImGui::PopFont();
        }

        // Panel behind everything, sized to what was laid out above.
        draw->ChannelsSetCurrent(0);
        const ImVec2 w0 = ImGui::GetWindowPos();
        const ImVec2 w1(w0.x + ImGui::GetWindowWidth(), ImGui::GetCursorScreenPos().y - rowGap + margin);
        const float pr = 22.0f * s;
        draw->AddRectFilled(ImVec2(w0.x + 4.0f * s, w0.y + 6.0f * s), ImVec2(w1.x + 4.0f * s, w1.y + 6.0f * s), IM_COL32(0, 0, 30, 90), pr);
        draw->AddRectFilledMultiColor(ImVec2(w0.x + pr * 0.3f, w0.y + pr * 0.3f), ImVec2(w1.x - pr * 0.3f, w1.y - pr * 0.3f),
                                      kPanelTop, kPanelTop, kPanelBottom, kPanelBottom);
        draw->AddRect(w0, w1, kFrame, pr, 0, 10.0f * s);
        draw->AddRect(ImVec2(w0.x + 5.0f * s, w0.y + 5.0f * s), ImVec2(w1.x - 5.0f * s, w1.y - 5.0f * s), IM_COL32(200, 212, 255, 200), pr - 5.0f * s, 0, 2.0f * s);
        draw->ChannelsMerge();
    }
    ImGui::End();
    ImGui::PopFont();
    ImGui::PopStyleVar(3);

    if (choice != 0)
    {
        ps2_stubs::resolveSaveSlotPick(choice);
        s_open = false;
        ps2PadSetBlocked(false);
    }
}

void PS2DebugPanel::draw(PS2Runtime &runtime)
{
#if defined(PS2X_ENABLE_DEBUG_UI) && !defined(PLATFORM_VITA)
    if (!m_initialized)
    {
        return;
    }

    if (IsKeyPressed(KEY_F1))
    {
        toggleVisible();
    }
    // F2: the Mods menu (code mods' settings).
    static bool s_modsMenu = false;
    if (IsKeyPressed(KEY_F2))
    {
        s_modsMenu = !s_modsMenu;
    }

    const bool slotPick = ps2_stubs::pendingSaveSlotPick().active;
    if (!m_visible && !s_modsMenu && !slotPick && !ps2x::mods::codeModsHaveUi())
    {
        drawSaveSlotPicker(); // closes a picker that was just resolved elsewhere
        ps2x::mods::setUiCapture(false, false);
        return;
    }

    rlImGuiBegin();
    {
        // With only mod HUDs over the game, ImGui must not touch the (maybe captured) mouse cursor.
        ImGuiIO &io = ImGui::GetIO();
        if (m_visible || s_modsMenu || slotPick)
            io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
        else
            io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    }

    ps2x::mods::drawCodeModsHud(makeModUi(s_modsMenu));
    drawModNotices();
    if (s_modsMenu)
    {
        ImGui::SetNextWindowPos(ImVec2(40.0f, 40.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(480.0f, 440.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Mods  (F2)", &s_modsMenu))
        {
            ps2x::mods::drawCodeModsMenu(makeModUi(true));
        }
        ImGui::End();
    }
    drawSaveSlotPicker();

    if (!m_visible)
    {
        const ImGuiIO &io = ImGui::GetIO();
        ps2x::mods::setUiCapture(io.WantCaptureKeyboard, io.WantCaptureMouse);
        rlImGuiEnd();
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(780.0f, 620.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Runtime Debugger", &m_visible, ImGuiWindowFlags_MenuBar))
    {
        if (ImGui::BeginMenuBar())
        {
            ImGui::MenuItem("Registers", nullptr, &m_showRegisters);
            ImGui::EndMenuBar();
        }

        if (ImGui::BeginTabBar("debug-tabs"))
        {
            if (ImGui::BeginTabItem("CPU"))
            {
                drawCpuTab(runtime, m_showRegisters);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Threads"))
            {
                drawThreadsTab(runtime);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Kernel"))
            {
                drawKernelTab(runtime);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("IOP/SIF"))
            {
                drawIopTab(runtime);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("RPC History"))
            {
                drawRpcHistoryTab(runtime);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("PAD"))
            {
                drawPadTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("File/CD"))
            {
                drawFileCdTab(runtime);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Mods"))
            {
                drawModsTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("GS"))
            {
                drawGsTab(runtime);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("DMA/IO"))
            {
                drawIoTab(runtime);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Logs"))
            {
                drawLogsTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Memory"))
            {
                uint32_t bytes = m_memoryBytes;
                ImGui::InputScalar("Address", ImGuiDataType_U32, &m_memoryAddress, nullptr, nullptr, "%08X", ImGuiInputTextFlags_CharsHexadecimal);
                ImGui::InputScalar("Bytes", ImGuiDataType_U32, &bytes, nullptr, nullptr, "%u");
                m_memoryBytes = std::clamp<unsigned int>(bytes, 16u, 0x1000u);
                ImGui::SameLine();
                if (ImGui::Button("PC"))
                    m_memoryAddress = runtime.m_debugPc.load(std::memory_order_relaxed);
                ImGui::SameLine();
                if (ImGui::Button("SP"))
                    m_memoryAddress = runtime.m_debugSp.load(std::memory_order_relaxed);
                ImGui::SameLine();
                if (ImGui::Button("GP"))
                    m_memoryAddress = runtime.m_debugGp.load(std::memory_order_relaxed);
                ImGui::Separator();
                drawHexDump(runtime.memory().getRDRAM(), m_memoryAddress, m_memoryBytes);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();

    {
        const ImGuiIO &io = ImGui::GetIO();
        ps2x::mods::setUiCapture(io.WantCaptureKeyboard, io.WantCaptureMouse);
    }
    rlImGuiEnd();
#else
    (void)runtime;
#endif
}
