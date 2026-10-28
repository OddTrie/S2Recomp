#include "iop_stdio.h"

#include "../core/iop_cpu.h"
#include "../core/iop_memory.h"
#include "ps2x/iop/iop_host.h"

#include <cstdio>
#include <string>

namespace ps2x::iop::detail
{
    namespace
    {
        // Minimal MIPS o32 varargs printf for IOP diagnostics: args start at
        // register index firstArg (a0=4) and continue on the stack at sp+16.
        std::string formatIopPrintf(const IopMemory &memory, const IopCpuState &cpu, uint32_t fmtAddress, uint32_t firstArg)
        {
            const std::string fmt = memory.readString(fmtAddress, 2048u);
            uint32_t argIndex = firstArg;
            const auto nextArg = [&]() -> uint32_t
            {
                const uint32_t i = argIndex++;
                if (i < 8u)
                    return cpu.gpr[i];
                return memory.read32(cpu.gpr[29] + 16u + (i - 8u) * 4u);
            };
            std::string out;
            out.reserve(fmt.size() + 32u);
            for (size_t i = 0; i < fmt.size(); ++i)
            {
                const char c = fmt[i];
                if (c != '%')
                {
                    out.push_back(c);
                    continue;
                }
                size_t j = i + 1u;
                std::string spec = "%";
                while (j < fmt.size() && std::string_view("-+ #0").find(fmt[j]) != std::string_view::npos)
                    spec.push_back(fmt[j++]);
                while (j < fmt.size() && fmt[j] >= '0' && fmt[j] <= '9')
                    spec.push_back(fmt[j++]);
                if (j < fmt.size() && fmt[j] == '.')
                {
                    spec.push_back(fmt[j++]);
                    while (j < fmt.size() && fmt[j] >= '0' && fmt[j] <= '9')
                        spec.push_back(fmt[j++]);
                }
                while (j < fmt.size() && (fmt[j] == 'l' || fmt[j] == 'h'))
                    ++j;
                if (j >= fmt.size())
                    break;
                const char conv = fmt[j];
                char buffer[256];
                buffer[0] = '\0';
                switch (conv)
                {
                case 'd':
                case 'i':
                    std::snprintf(buffer, sizeof(buffer), (spec + "d").c_str(), static_cast<int32_t>(nextArg()));
                    break;
                case 'u':
                case 'x':
                case 'X':
                case 'o':
                    std::snprintf(buffer, sizeof(buffer), (spec + conv).c_str(), nextArg());
                    break;
                case 'p':
                    std::snprintf(buffer, sizeof(buffer), "0x%08x", nextArg());
                    break;
                case 'c':
                    buffer[0] = static_cast<char>(nextArg() & 0xFFu);
                    buffer[1] = '\0';
                    break;
                case 's':
                {
                    const std::string str = memory.readString(nextArg(), 512u);
                    std::snprintf(buffer, sizeof(buffer), (spec + "s").c_str(), str.c_str());
                    break;
                }
                case '%':
                    buffer[0] = '%';
                    buffer[1] = '\0';
                    break;
                default:
                    out.append(fmt, i, j - i + 1u);
                    i = j;
                    continue;
                }
                out += buffer;
                i = j;
            }
            return out;
        }
    }

    IopStdio::IopStdio(IopHost &host, IopMemory &memory) noexcept
        : m_host(host), m_memory(memory)
    {
    }

    bool IopStdio::dispatchImport(uint16_t ordinal, IopCpuState &cpu)
    {
        const uint32_t a0 = cpu.gpr[4];
        const uint32_t a1 = cpu.gpr[5];
        const auto setV0 = [&](uint32_t value)
        {
            cpu.gpr[2] = value;
        };
        const auto logString = [&](std::string_view prefix, uint32_t address, uint32_t resultBias = 0u)
        {
            const std::string text = m_memory.readString(address, 2048u);
            m_host.log(LogLevel::Info, std::string(prefix) + text);
            setV0(static_cast<uint32_t>(text.size()) + resultBias);
        };

        switch (ordinal)
        {
        case 4: // printf
        {
            const std::string text = formatIopPrintf(m_memory, cpu, a0, 5u);
            std::string line = text;
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
                line.pop_back();
            if (!line.empty())
                m_host.log(LogLevel::Info, "[IOP printf] " + line);
            setV0(static_cast<uint32_t>(text.size()));
            return true;
        }
        case 5: // getchar
        case 10:
            setV0(0xFFFFFFFFu);
            return true;
        case 6: // putchar
            m_host.log(LogLevel::Info, std::string("[IOP putchar] ") + static_cast<char>(a0 & 0xFFu));
            setV0(a0 & 0xFFu);
            return true;
        case 7: // puts
            logString("[IOP puts] ", a0, 1u);
            return true;
        case 8: // gets
        case 13:
            setV0(0u);
            return true;
        case 9: // fdprintf
            logString("[IOP fdprintf] ", a1);
            return true;
        case 11:
            setV0(a0 & 0xFFu);
            return true;
        case 12: // fdputs
            logString("[IOP fdputs] ", a0);
            return true;
        case 14: // vfdprintf
            logString("[IOP vfdprintf] ", a1);
            return true;
        default:
            return false;
        }
    }
}
