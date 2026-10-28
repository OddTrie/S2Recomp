#pragma once

#include "ps2x/iop/iop_host.h"
#include "ps2x/iop/iop_types.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ps2x::iop
{
    class IopSubsystem
    {
    public:
        explicit IopSubsystem(IopHost &host);
        ~IopSubsystem();

        IopSubsystem(const IopSubsystem &) = delete;
        IopSubsystem &operator=(const IopSubsystem &) = delete;
        IopSubsystem(IopSubsystem &&) noexcept;
        IopSubsystem &operator=(IopSubsystem &&) noexcept;

        void reset();

        [[nodiscard]] ModuleLoadResult loadModule(std::string_view path, const void *arguments = nullptr, uint32_t argumentSize = 0);
        [[nodiscard]] ModuleLoadResult loadModuleBuffer(uint32_t guestAddress, const void *arguments = nullptr, uint32_t argumentSize = 0);
        // Wake the IOP thread blocked on an IOP->EE RPC once its reply is in IOP RAM.
        void completeEeRpc(uint64_t token);
        [[nodiscard]] bool stopModule(int32_t moduleId, int32_t *result = nullptr);
        // Called at every EE checkpoint. While the emulated IOP is idle with nothing due (see
        // IopEmulator::eeCyclesIdle) the cycles are only summed here and handed over in one call
        // later: before any other entry into the IOP, or once they would reach its next event.
        void runEeCycles(uint64_t eeCycles) noexcept
        {
            if (eeCycles <= m_eeAbsorb - m_eePending)
            {
                if (m_verify) [[unlikely]]
                    verifyAbsorbed(eeCycles);
                m_eePending += eeCycles;
                return;
            }
            runEeCyclesSlow(eeCycles);
        }

        [[nodiscard]] RpcAbi selectRpcAbi(const RpcAbiRequest &request) const;
        [[nodiscard]] bool canBindRpc(uint32_t sid) const noexcept;
        [[nodiscard]] RpcResult handleRpc(const RpcRequest &request);
        void onSifTransfer(const SifTransfer &transfer);

        // Physical IOP RAM access shared by the emulator, SIF DMA, and HLE services. Addresses are IOP addresses.
        [[nodiscard]] uint32_t allocateMemory(uint32_t size, uint32_t alignment = 16u);
        [[nodiscard]] bool freeMemory(uint32_t address);
        [[nodiscard]] bool readMemory(uint32_t address, void *destination, size_t size) const;
        [[nodiscard]] bool writeMemory(uint32_t address, const void *source, size_t size);
        [[nodiscard]] bool zeroMemory(uint32_t address, size_t size);
        [[nodiscard]] bool isMemoryRange(uint32_t address, size_t size) const;

        [[nodiscard]] DebugSnapshot debugSnapshot() const;

    private:
        void runEeCyclesSlow(uint64_t eeCycles) noexcept;
        void flushEeCycles() noexcept; // hand over m_eePending; the next entry may change IOP state
        void verifyAbsorbed(uint64_t eeCycles) noexcept; // PS2X_EE_VERIFY=1: feed it now, check it was idle

        class Impl;
        std::unique_ptr<Impl> m_impl;
        uint64_t m_eePending = 0; // EE cycles not yet given to the emulator (always <= m_eeAbsorb)
        uint64_t m_eeAbsorb = 0;  // EE cycles the idle emulator can take without doing anything
        bool m_verify = false;
    };
}
