#pragma once

#include "ps2x/iop/iop_types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ps2x::iop
{
    enum class HostPathKind : uint32_t
    {
        ElfDirectory = 0,
        CdRoot = 1,
        CdImage = 2,
        HostRoot = 3,
        MemoryCardRoot = 4,
    };

    enum class LogLevel : uint32_t
    {
        Debug = 0,
        Info = 1,
        Warning = 2,
        Error = 3,
    };

    enum class MemoryCardOperation : uint32_t
    {
        Init,
        GetInfo,
        Open,
        Close,
        Seek,
        Read,
        Write,
        Flush,
        Chdir,
        GetDir,
        SetFileInfo,
        Delete,
        Format,
        Unformat,
        Mkdir,
    };

    struct MemoryCardRequest
    {
        MemoryCardOperation operation = MemoryCardOperation::Init;
        // The fifth argument is carried in $t0 by the EE n32 ABI
        std::array<uint32_t, 5> arguments{};
    };

    class IopHost
    {
    public:
        virtual ~IopHost() = default;

        virtual bool readGuest(uint32_t address, void *destination, size_t size) const = 0;
        virtual bool writeGuest(uint32_t address, const void *source, size_t size) = 0;
        virtual bool zeroGuest(uint32_t address, size_t size) = 0;
        virtual bool normalizeGuestAddress(uint32_t address, uint32_t &normalized) const = 0;

        // IOP RAM is a distinct address space from the EE guest. TODO remove this later
        virtual bool readIopMemory(uint32_t address, void *destination, size_t size) const
        {
            (void)address;
            (void)destination;
            (void)size;
            return false;
        }
        virtual bool writeIopMemory(uint32_t address, const void *source, size_t size)
        {
            (void)address;
            (void)source;
            (void)size;
            return false;
        }
        virtual bool zeroIopMemory(uint32_t address, size_t size)
        {
            (void)address;
            (void)size;
            return false;
        }
        virtual bool normalizeIopAddress(uint32_t address, uint32_t &normalized) const
        {
            (void)address;
            normalized = 0u;
            return false;
        }
        virtual uint32_t allocateIopHandle(IopHandleKind kind) = 0;
        virtual uint32_t allocateGuest(uint32_t size, uint32_t alignment) = 0;
        virtual void freeGuest(uint32_t address) = 0;

        virtual void audioCommand(uint32_t sid, uint32_t function, GuestBuffer send, GuestBuffer receive) = 0;

        virtual std::string hostPath(HostPathKind kind) const = 0;
        virtual std::string translateGuestPath(std::string_view path) const = 0;
        // Host file that replaces a disc file (a mod), or empty. relativePath is from the disc root.
        virtual std::string overrideCdFile(std::string_view relativePath) const
        {
            (void)relativePath;
            return {};
        }
        virtual uint64_t openHostFile(std::string_view path) = 0;
        virtual bool hostFileSize(uint64_t handle, uint64_t &size) const = 0;
        virtual bool readHostFile(uint64_t handle,
                                  uint64_t offset,
                                  void *destination,
                                  size_t size,
                                  size_t &bytesRead) = 0;
        virtual void closeHostFile(uint64_t handle) = 0;

        virtual int32_t memoryCard(const MemoryCardRequest &request) = 0;

        virtual bool hasGuestFunction(uint32_t address) const = 0;
        virtual bool invokeGuestFunction(uint64_t callToken,
                                         uint32_t address,
                                         uint32_t a0,
                                         uint32_t a1,
                                         uint32_t a2,
                                         uint32_t a3,
                                         uint32_t *resultAddress) = 0;

        // Deliver an IOP -> EE SIF command packet. The default keeps hosts
        // which do not emulate the EE command dispatcher source-compatible.
        virtual bool sendSifCommand(uint32_t commandId,
                                    const void *packet,
                                    size_t packetSize)
        {
            (void)commandId;
            (void)packet;
            (void)packetSize;
            return false;
        }

        // Deliver an IOP -> EE RPC call (IOP-side sceSifCallRpc on a client bound to an
        // EE-registered server). The host copies `send` into the EE server's buffer, runs the
        // server function on the EE, and writes up to `receiveSize` reply bytes back into IOP
        // RAM at `iopReceiveAddress`. Returns false if no EE server owns `sid`.
        // When `completionToken` is non-zero the IOP caller is blocked (sceSifCallRpc
        // without SIF_RPC_M_NOWAIT); the host must call IopSubsystem::completeEeRpc(token)
        // after the reply has been written.
        virtual bool callEeRpcServer(uint32_t sid,
                                     uint32_t function,
                                     const void *send,
                                     uint32_t sendSize,
                                     uint32_t iopReceiveAddress,
                                     uint32_t receiveSize,
                                     uint64_t completionToken)
        {
            (void)completionToken;
            (void)sid;
            (void)function;
            (void)send;
            (void)sendSize;
            (void)iopReceiveAddress;
            (void)receiveSize;
            return false;
        }

        virtual void log(LogLevel level, std::string_view message) = 0;
    };
}
