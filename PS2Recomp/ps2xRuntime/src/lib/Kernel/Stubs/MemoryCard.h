#pragma once

#include "ps2_stubs.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ps2_stubs
{

    struct MemoryCardDebugOpenFile
    {
        int32_t fd = 0;
        int32_t port = 0;
        std::filesystem::path hostPath;
    };

    struct MemoryCardDebugPort
    {
        int32_t port = 0;
        std::string currentDir;
        bool formatted = false;
        std::filesystem::path rootPath;
    };

    struct MemoryCardDebugSnapshot
    {
        int32_t nextFd = 0;
        int32_t lastCmd = 0;
        int32_t lastResult = 0;
        int32_t cvFileCursor = 0;
        std::array<MemoryCardDebugPort, 2> ports{};
        std::vector<MemoryCardDebugOpenFile> openFiles;
    };

    MemoryCardDebugSnapshot getMemoryCardDebugSnapshot();

    // Host folder of memory card <port> (save slots, see ps2_mod_loader.h). An empty folder goes back to
    // the default (PS2X_MC_ROOT or mc0 next to the ELF; card 1 is mc1 beside it). Returns false while
    // the game has a file open on that card. After a swap the game's next sceMcGetInfo on the card
    // returns "changed card", as on a PS2 when another card is pushed in, so it reads the card again.
    bool setMemoryCardFolder(int32_t port, const std::filesystem::path &folder);
    std::filesystem::path memoryCardFolder(int32_t port);
    bool memoryCardBusy(); // any memory card file open

    // Save slots, built in: slot 1 is the card's own folder, slot n is <card>_slots/<n>, and the
    // choice is kept in <card>_slots/active.txt. Each time the game starts to load or save, it waits
    // in sceMcGetDir until resolveSaveSlotPick (the slot picker; PS2X_SAVE_SLOT_AUTOPICK=<n> picks
    // without asking).
    struct SaveSlotInfo
    {
        int index = 1;
        std::filesystem::path folder;
        std::string saved; // when it was last saved; empty = empty slot
        std::string name;  // given in the slot picker (<card>_slots/names.txt); empty = none
    };
    struct SaveSlotPickRequest
    {
        bool active = false;
        int32_t port = 0;
    };
    std::vector<SaveSlotInfo> listSaveSlots(int32_t port);
    int activeSaveSlot(int32_t port);
    void setSaveSlotName(int32_t port, int slot, const std::string &name); // empty = clear the name
    SaveSlotPickRequest pendingSaveSlotPick();
    void resolveSaveSlotPick(int slot); // slot < 1 = cancel (keep the current slot)
    void sceMcChangeThreadPriority(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceMcChdir(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceMcClose(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceMcDelete(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceMcEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceMcFlush(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceMcFormat(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceMcGetDir(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceMcGetEntSpace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceMcGetInfo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceMcGetSlotMax(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceMcInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceMcMkdir(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceMcOpen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceMcRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceMcRename(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceMcSeek(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceMcSetFileInfo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceMcSync(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceMcUnformat(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceMcWrite(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void mcCallMessageTypeSe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void mcCheckReadStartConfigFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void mcCheckReadStartSaveFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void mcCheckWriteStartConfigFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void mcCheckWriteStartSaveFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void mcCreateConfigInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void mcCreateFileSelectWindow(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void mcCreateIconInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void mcCreateSaveFileInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void mcDispFileName(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void mcDispFileNumber(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void mcDisplayFileSelectWindow(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void mcDisplaySelectFileInfo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void mcDisplaySelectFileInfoMesCount(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void mcDispWindowCurSol(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void mcDispWindowFoundtion(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void mceGetInfoApdx(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void mceIntrReadFixAlign(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void mceStorePwd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void mcGetConfigCapacitySize(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void mcGetFileSelectWindowCursol(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void mcGetFreeCapacitySize(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void mcGetIconCapacitySize(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void mcGetIconFileCapacitySize(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void mcGetPortSelectDirInfo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void mcGetSaveFileCapacitySize(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void mcGetStringEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void mcMoveFileSelectWindowCursor(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void mcNewCreateConfigFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void mcNewCreateIcon(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void mcNewCreateSaveFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void mcReadIconData(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void mcReadStartConfigFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void mcReadStartSaveFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void mcSelectFileInfoInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void mcSelectSaveFileCheck(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void mcSetFileSelectWindowCursol(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void mcSetFileSelectWindowCursolInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void mcSetStringSaveFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void mcSetTyepWriteMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void mcWriteIconData(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void mcWriteStartConfigFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void mcWriteStartSaveFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
}
