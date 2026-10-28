#include "iop_rpc.h"

#include "../core/iop_cpu.h"
#include "../core/iop_kernel.h"
#include "../core/iop_memory.h"
#include "ps2x/iop/iop_host.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

namespace ps2x::iop::detail
{
    IopRpcBridge::IopRpcBridge(IopHost &host, IopMemory &memory, IopKernel &kernel) noexcept
        : m_host(host), m_memory(memory), m_kernel(kernel)
    {
    }

    void IopRpcBridge::reset()
    {
        m_servers.clear();
        m_nextDmaId = 1u;
        m_sifInitialized = false;
        m_loggedEeBinds.clear();
        m_eeBoundClients.clear();
        m_eeRpcWaiters.clear();
        m_loggedEeCalls = 0u;
    }

    bool IopRpcBridge::dispatchSifManImport(uint16_t ordinal, IopCpuState &cpu)
    {
        const auto setV0 = [&](uint32_t value)
        {
            cpu.gpr[2] = value;
        };
        switch (ordinal)
        {
        case 4: // sceSifDma2Init
        case 5: // sceSifInit
            m_sifInitialized = true;
            setV0(0u);
            return true;
        case 7: // sceSifSetDma
        {
            constexpr uint32_t kDescriptorSize = 16u;
            constexpr uint32_t kMaxDescriptors = 32u;
            const uint32_t descriptorAddress = cpu.gpr[4];
            const uint32_t descriptorCount = cpu.gpr[5];
            if (descriptorAddress == 0u || descriptorCount == 0u || descriptorCount > kMaxDescriptors)
            {
                setV0(0u);
                return true;
            }

            struct PendingTransfer
            {
                uint32_t source = 0u;
                uint32_t destination = 0u;
                uint32_t size = 0u;
            };

            std::array<uint32_t, kMaxDescriptors * 4u> descriptorWords{};
            const size_t descriptorBytes = static_cast<size_t>(descriptorCount) * kDescriptorSize;
            if (!m_memory.readRam(descriptorAddress, descriptorWords.data(), descriptorBytes))
            {
                setV0(0u);
                return true;
            }

            std::array<PendingTransfer, kMaxDescriptors> pending{};
            uint32_t pendingCount = 0u;
            uint32_t largestTransfer = 0u;
            for (uint32_t i = 0u; i < descriptorCount; ++i)
            {
                const uint32_t source = descriptorWords[i * 4u + 0u];
                const uint32_t destination = descriptorWords[i * 4u + 1u];
                const int32_t signedSize = static_cast<int32_t>(descriptorWords[i * 4u + 2u]);
                if (signedSize <= 0)
                    continue;

                const uint32_t size = static_cast<uint32_t>(signedSize);
                if (!m_memory.ownsRamRange(source, size))
                {
                    setV0(0u);
                    return true;
                }
                pending[pendingCount++] = {source, destination, size};
                largestTransfer = std::max(largestTransfer, size);
            }

            // IOP-side sceSifSetDma sends IOP RAM to the EE. Validate all EE
            // destinations before committing any write so a bad chain cannot
            // partially update guest memory, but maybe we could skip this check if we trust the EE-side SIF driver to validate the chain ?!
            // TODO check later
            std::vector<uint8_t> scratch(largestTransfer);
            for (uint32_t i = 0u; i < pendingCount; ++i)
            {
                const PendingTransfer &transfer = pending[i];
                if (!m_host.readGuest(transfer.destination, scratch.data(), transfer.size))
                {
                    setV0(0u);
                    return true;
                }
            }

            static const bool s_traceDma = []()
            {
                const char *env = std::getenv("PS2X_IOP_TRACE");
                return env && *env && *env != '0';
            }();
            for (uint32_t i = 0u; i < pendingCount; ++i)
            {
                const PendingTransfer &transfer = pending[i];
                if (s_traceDma && m_loggedDmaToEe < 400u)
                {
                    ++m_loggedDmaToEe;
                    char line[128];
                    std::snprintf(line, sizeof(line), "[IOP->EE dma] iop=0x%x ee=0x%x size=0x%x ra=0x%x", transfer.source, transfer.destination, transfer.size, cpu.gpr[31]);
                    m_host.log(LogLevel::Info, line);
                }
                if (!m_memory.readRam(transfer.source, scratch.data(), transfer.size) || !m_host.writeGuest(transfer.destination, scratch.data(), transfer.size))
                {
                    setV0(0u);
                    return true;
                }
            }

            const uint32_t dmaId = m_nextDmaId++;
            if (m_nextDmaId == 0u || m_nextDmaId > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
            {
                m_nextDmaId = 1u;
            }
            setV0(dmaId);
            return true;
        }
        case 8: // sceSifDmaStat
            setV0(0xFFFFFFFFu);
            return true;
        case 29: // sceSifCheckInit
            setV0(m_sifInitialized ? 1u : 0u);
            return true;
        default:
            setV0(0u);
            return true;
        }
    }

    bool IopRpcBridge::dispatchSifCmdImport(uint16_t ordinal, IopCpuState &cpu)
    {
        const auto setV0 = [&](uint32_t value)
        {
            cpu.gpr[2] = value;
        };
        switch (ordinal)
        {
        case 4: // InitCmd
        case 5:
        case 6:
        case 7:
        case 8:
        case 9:
        case 10:
        case 11:
        case 14: // InitRpc
            setV0(0);
            return true;
        case 15: // sceSifBindRpc(sceSifClientData *client, int sid, int mode)
        {
            // IOP -> EE RPC. Modules (e.g. EA's multi_streamer) bind a client to an
            // EE-side server and then spin until client->serve (offset 0x24) is non-zero.
            // We have no IOP-visible EE server descriptor, so publish a stable non-zero
            // token derived from the SID. Calls on this client are accepted below.
            const uint32_t client = cpu.gpr[4];
            const uint32_t sid = cpu.gpr[5];
            if (client != 0u && m_memory.ownsRamRange(client, 0x28u))
            {
                m_memory.write32(client + 0x10u, 0u);                  // command
                m_memory.write32(client + 0x24u, 0x80000000u | (sid & 0x7FFFFFF0u) | 0x4u); // serve (opaque, non-zero)
                m_eeBoundClients[client] = sid;
                if (m_loggedEeBinds.insert(sid).second)
                {
                    char line[96];
                    std::snprintf(line, sizeof(line), "[IOP] sceSifBindRpc to EE sid=0x%08x client=0x%x (stub-bound)", sid, client);
                    m_host.log(LogLevel::Info, line);
                }
            }
            setV0(0);
            return true;
        }
        case 16: // sceSifCallRpc(client, fno, mode, send, ssize, recv, rsize, endfunc, endparam)
        {
            // IOP -> EE RPC: forward to the EE server registered for the bound SID. The EE
            // server runs asynchronously (queued EE invocation) and its reply is copied into
            // IOP RAM when it finishes, so the IOP caller continues immediately.
            const uint32_t client = cpu.gpr[4];
            const uint32_t fno = cpu.gpr[5];
            const uint32_t mode = cpu.gpr[6];
            const uint32_t sendAddress = cpu.gpr[7];
            const uint32_t sp = cpu.gpr[29];
            const uint32_t sendSize = m_memory.read32(sp + 16u);
            const uint32_t recvAddress = m_memory.read32(sp + 20u);
            const uint32_t recvSize = m_memory.read32(sp + 24u);
            const uint32_t endFunc = m_memory.read32(sp + 28u);
            const auto bound = m_eeBoundClients.find(client);
            bool dispatched = false;
            // Blocking call (no SIF_RPC_M_NOWAIT): the caller reads the reply right after the
            // call returns, so park the IOP thread until the EE server has answered.
            const int callerThread = m_kernel.currentThreadId();
            const bool blocking = (mode & 1u) == 0u && callerThread != 0;
            const uint64_t token = blocking ? ++m_nextEeRpcToken : 0u;
            if (bound != m_eeBoundClients.end())
            {
                std::vector<uint8_t> payload;
                if (sendAddress != 0u && sendSize != 0u && sendSize <= 0x10000u && m_memory.ownsRamRange(sendAddress, sendSize))
                {
                    payload.resize(sendSize);
                    (void)m_memory.readRam(sendAddress, payload.data(), payload.size());
                }
                dispatched = m_host.callEeRpcServer(bound->second, fno,
                                                    payload.empty() ? nullptr : payload.data(),
                                                    static_cast<uint32_t>(payload.size()),
                                                    recvAddress, recvSize, token);
                if (dispatched && blocking)
                {
                    m_eeRpcWaiters[token] = callerThread;
                    m_kernel.sleepCurrent(cpu);
                }
            }
            if (m_loggedEeCalls < 16u)
            {
                ++m_loggedEeCalls;
                char line[200];
                std::snprintf(line, sizeof(line),
                              "[IOP] sceSifCallRpc to EE sid=0x%08x fno=%u mode=%u send=%u recv=%u endfunc=0x%x -> %s",
                              bound != m_eeBoundClients.end() ? bound->second : 0u, fno, mode, sendSize, recvSize, endFunc,
                              dispatched ? (blocking ? "queued on EE, caller waits" : "queued on EE") : "NOT dispatched");
                m_host.log(dispatched ? LogLevel::Info : LogLevel::Warning, line);
            }
            setV0(0);
            return true;
        }
        case 12: // sceSifSendCmd
        case 13: // isceSifSendCmd
        {
            constexpr uint32_t kHeaderSize = 16u;
            constexpr uint32_t kMaxPacketSize = 112u;
            const uint32_t commandId = cpu.gpr[4];
            const uint32_t packetAddress = cpu.gpr[5];
            const uint32_t packetSize = cpu.gpr[6];
            const uint32_t extraSource = cpu.gpr[7];
            const uint32_t stackPointer = cpu.gpr[29];
            const uint32_t extraDestination = m_memory.read32(stackPointer + 16u);
            const int32_t signedExtraSize = static_cast<int32_t>(m_memory.read32(stackPointer + 20u));

            if (packetAddress == 0u || packetSize < kHeaderSize || packetSize > kMaxPacketSize ||
                !m_memory.ownsRamRange(packetAddress, packetSize))
            {
                setV0(0u);
                return true;
            }

            std::array<uint8_t, kMaxPacketSize> packet{};
            if (!m_memory.readRam(packetAddress, packet.data(), packetSize))
            {
                setV0(0u);
                return true;
            }

            uint32_t extraSize = 0u;
            if (signedExtraSize > 0)
            {
                extraSize = static_cast<uint32_t>(signedExtraSize);
                if (extraSource == 0u || extraDestination == 0u ||
                    !m_memory.ownsRamRange(extraSource, extraSize) ||
                    !m_host.writeGuest(extraDestination, m_memory.ram().data() + IopMemory::physicalAddress(extraSource), extraSize))
                {
                    setV0(0u);
                    return true;
                }
            }

            const uint32_t sizeWord = packetSize | (extraSize << 8u);
            std::memcpy(packet.data() + 0u, &sizeWord, sizeof(sizeWord));
            std::memcpy(packet.data() + 4u, &extraDestination, sizeof(extraDestination));
            std::memcpy(packet.data() + 8u, &commandId, sizeof(commandId));

            if (!m_host.sendSifCommand(commandId, packet.data(), packetSize))
            {
                // A command without an EE handler is still a completed DMA on  real hardware. Only malformed packets fail above.
            }

            const uint32_t dmaId = m_nextDmaId++;
            if (m_nextDmaId == 0u || m_nextDmaId > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
                m_nextDmaId = 1u;
            setV0(dmaId);
            return true;
        }
        case 17: // sceSifRegisterRpc
        {
            RpcServer server;
            server.serverData = cpu.gpr[4];
            server.sid = cpu.gpr[5];
            server.function = cpu.gpr[6];
            server.gp = cpu.gpr[28];
            server.buffer = cpu.gpr[7];
            const uint32_t stackPointer = cpu.gpr[29];
            server.callback = m_memory.read32(stackPointer + 16u);
            server.callbackBuffer = m_memory.read32(stackPointer + 20u);
            server.queue = m_memory.read32(stackPointer + 24u);
            m_servers[server.sid] = server;
            {
                char line[160];
                std::snprintf(line, sizeof(line), "[IOP] sceSifRegisterRpc sid=0x%08x func=0x%x buf=0x%x cfunc=0x%x", server.sid, server.function, server.buffer, server.callback);
                m_host.log(LogLevel::Info, line);
            }
            if (server.serverData != 0u)
            {
                m_memory.write32(server.serverData + 0x20u, server.sid);
                m_memory.write32(server.serverData + 0x28u, server.function);
                m_memory.write32(server.serverData + 0x2Cu, server.buffer);
            }
            setV0(server.serverData);
            return true;
        }
        case 18:
            setV0(0);
            return true;
        case 19: // SetRpcQueue
            setV0(cpu.gpr[4]);
            return true;
        case 20:
        case 21:
            setV0(0);
            return true;
        case 22: // RpcLoop
            m_kernel.sleepCurrent(cpu);
            setV0(0);
            return true;
        case 23:
            setV0(0);
            return true;
        case 24: // RemoveRpc
        {
            const uint32_t serverData = cpu.gpr[4];
            for (auto server = m_servers.begin(); server != m_servers.end(); ++server)
            {
                if (server->second.serverData == serverData)
                {
                    m_servers.erase(server);
                    break;
                }
            }
            setV0(0);
            return true;
        }
        case 25:
        case 26:
        case 27:
        case 28:
        case 29:
            setV0(0);
            return true;
        default:
            return false;
        }
    }

    void IopRpcBridge::completeEeRpc(uint64_t token)
    {
        const auto it = m_eeRpcWaiters.find(token);
        if (it == m_eeRpcWaiters.end())
            return;
        m_kernel.wakeSleepingThread(it->second);
        m_eeRpcWaiters.erase(it);
    }

    RpcResult IopRpcBridge::handleRpc(const RpcRequest &request, IopGuestExecutor &executor)
    {
        RpcResult result{};
        const auto serverIt = m_servers.find(request.sid);
        if (serverIt == m_servers.end() || serverIt->second.function == 0u)
            return result;

        RpcServer &server = serverIt->second;
        if (m_loggedServerCalls < 24u)
        {
            ++m_loggedServerCalls;
            char line[160];
            std::snprintf(line, sizeof(line), "[IOP] EE->IOP rpc sid=0x%08x fno=%u send=%u recv=%u -> func 0x%x", request.sid, request.function, request.send.size, request.receive.size, server.function);
            m_host.log(LogLevel::Info, line);
        }
        if (request.send.size != 0u && server.buffer != 0u)
        {
            const uint32_t copySize = std::min<uint32_t>(request.send.size, IopMemory::RamSize - std::min(server.buffer, IopMemory::RamSize));
            if (copySize != 0u)
            {
                std::vector<uint8_t> payload(copySize);
                if (m_host.readGuest(request.send.address, payload.data(), payload.size()))
                    (void)m_memory.writeRam(server.buffer, payload.data(), payload.size());
            }
        }

        uint32_t returnPointer = executor.executeGuestFunction(server.function,
                                                               request.function,
                                                               server.buffer,
                                                               request.send.size,
                                                               0u,
                                                               server.gp);
        if (returnPointer == 0u)
            returnPointer = server.buffer;
        if (request.receive.address != 0u && request.receive.size != 0u && returnPointer != 0u)
        {
            const uint32_t physical = IopMemory::physicalAddress(returnPointer);
            if (physical < IopMemory::RamSize)
            {
                const uint32_t copySize = std::min<uint32_t>(request.receive.size, IopMemory::RamSize - physical);
                (void)m_host.writeGuest(request.receive.address, m_memory.ram().data() + physical, copySize);
                if (copySize < request.receive.size)
                    (void)m_host.zeroGuest(request.receive.address + copySize, request.receive.size - copySize);
            }
        }

        result.handled = true;
        result.resultAddress = request.receive.address;
        result.serverDispatchPolicy = ServerDispatchPolicy::Suppress;
        result.signalNowaitCompletion = true;
        result.signalCompletion = true;
        return result;
    }

    void IopRpcBridge::onSifTransfer(const SifTransfer &transfer)
    {
        // The EE SIF transport owns the actual directional memory movement.
        // Services still receive both phases through IopSubsystem, but mirroring
        // IOP bytes through an equal-numbered EE address would alias two distinct
        // PS2 address spaces and can overwrite live game data.
        (void)transfer;
    }

    void IopRpcBridge::removeServersInRange(uint32_t base, uint32_t size)
    {
        for (auto server = m_servers.begin(); server != m_servers.end();)
        {
            const uint32_t function = IopMemory::physicalAddress(server->second.function);
            if (function >= base && function < base + size)
                server = m_servers.erase(server);
            else
                ++server;
        }
    }

    bool IopRpcBridge::hasServer(uint32_t sid) const noexcept
    {
        const auto server = m_servers.find(sid);
        return server != m_servers.end() && server->second.function != 0u;
    }
}
