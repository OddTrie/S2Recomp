#pragma once

#include "ps2_syscalls.h"

namespace ps2_syscalls
{
    void SifStopModule(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void SifLoadModule(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void SifInitRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void SifBindRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void SifCallRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void SifRegisterRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void SifCheckStatRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void SifSetRpcQueue(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void SifRemoveRpcQueue(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void SifRemoveRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceSifCallRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceSifSendCmd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceRpcGetPacket(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);

    // IOP -> EE RPC: run the EE server registered for `sid` as a queued EE invocation
    // (func(fno, buf, size)), then copy its reply into IOP RAM. Returns false if no
    // EE server is registered for `sid`.
    // Blocking EE sceSifRpcLoop: serves IOP->EE requests on the calling (server) thread.
    void sifRpcLoopBlocking(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);

    bool queueIopToEeRpc(uint8_t *rdram, PS2Runtime *runtime, uint32_t sid, uint32_t function,
                         const void *send, uint32_t sendSize, uint32_t iopReceiveAddress, uint32_t receiveSize,
                         uint64_t completionToken = 0u);
}
