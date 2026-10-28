#include "Common.h"
#include "MemoryCard.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_mod_loader.h"
#include "runtime/ps2_netplay.h"

#include <map>

extern std::atomic<uint64_t> g_ps2xVsyncTickCount;
namespace ps2_stubs
{
    namespace
    {
        constexpr int32_t kMcCmdGetInfo = 0x01;
        constexpr int32_t kMcCmdOpen = 0x02;
        constexpr int32_t kMcCmdClose = 0x03;
        constexpr int32_t kMcCmdSeek = 0x04;
        constexpr int32_t kMcCmdRead = 0x05;
        constexpr int32_t kMcCmdWrite = 0x06;
        constexpr int32_t kMcCmdFlush = 0x0A;
        constexpr int32_t kMcCmdMkdir = 0x0B;
        constexpr int32_t kMcCmdChdir = 0x0C;
        constexpr int32_t kMcCmdGetDir = 0x0D;
        constexpr int32_t kMcCmdSetFileInfo = 0x0E;
        constexpr int32_t kMcCmdDelete = 0x0F;
        constexpr int32_t kMcCmdFormat = 0x10;
        constexpr int32_t kMcCmdUnformat = 0x11;
        constexpr int32_t kMcCmdGetEntSpace = 0x12;
        constexpr int32_t kMcCmdRename = 0x13;

        constexpr int32_t kMcResultSucceed = 0;
        constexpr int32_t kMcResultChangedCard = -1;
        constexpr int32_t kMcResultNoFormat = -2;
        constexpr int32_t kMcResultNoEntry = -4;
        constexpr int32_t kMcResultDeniedPermit = -5;
        constexpr int32_t kMcResultNotEmpty = -6;
        constexpr int32_t kMcResultUpLimitHandle = -7;

        constexpr int32_t kMcTypePs2 = 2;
        constexpr int32_t kMcFormatted = 1;
        constexpr int32_t kMcUnformatted = 0;
        constexpr int32_t kMcFreeClusters = 0x2000;
        constexpr size_t kMcMaxPathLen = 1024;
        constexpr size_t kMcMaxOpenFiles = 32;

        constexpr uint16_t kMcAttrReadable = 0x0001;
        constexpr uint16_t kMcAttrWriteable = 0x0002;
        constexpr uint16_t kMcAttrFile = 0x0010;
        constexpr uint16_t kMcAttrSubdir = 0x0020;
        constexpr uint16_t kMcAttrClosed = 0x0080;
        constexpr uint16_t kMcAttrExists = 0x8000;

        struct SceMcStDateTime
        {
            uint8_t Resv2 = 0;
            uint8_t Sec = 0;
            uint8_t Min = 0;
            uint8_t Hour = 0;
            uint8_t Day = 0;
            uint8_t Month = 0;
            uint16_t Year = 0;
        };

        struct SceMcTblGetDir
        {
            SceMcStDateTime _Create{};
            SceMcStDateTime _Modify{};
            uint32_t FileSizeByte = 0;
            uint16_t AttrFile = 0;
            uint16_t Reserve1 = 0;
            uint32_t Reserve2 = 0;
            uint32_t PdaAplNo = 0;
            char EntryName[32]{};
        };

        static_assert(sizeof(SceMcTblGetDir) == 64, "sceMcTblGetDir size mismatch");

        struct McOpenFile
        {
            FILE *file = nullptr;
            int32_t port = 0;
            std::filesystem::path hostPath;
        };

        struct McPortState
        {
            std::string currentDir = "/";
            bool formatted = true;
        };

        std::mutex g_mcStateMutex;
        int32_t g_mcNextFd = 1;
        int32_t g_mcLastCmd = 0;
        bool g_mcCommandPending = false;
        int32_t g_mcLastResult = 0;
        std::unordered_map<int32_t, McOpenFile> g_mcFiles;
        std::array<McPortState, 2> g_mcPorts{};
        int32_t g_cvMcFileCursor = 0;
        // Save slots (setMemoryCardFolder): a host folder per card instead of the default one, and
        // whether the game has looked at the card yet / must be told it was swapped.
        std::array<std::filesystem::path, 2> g_mcFolderOverride{};
        std::array<bool, 2> g_mcSeen{};
        std::array<bool, 2> g_mcSwapped{};
        // Save slots: slot 1 is the card's own folder, slot n is <card>_slots/<n>. Each time the game
        // starts to load or save, it is asked which slot to use (see sceMcGetDir).
        std::array<int, 2> g_mcActiveSlot{1, 1};
        std::array<bool, 2> g_mcSlotLoaded{};
        uint64_t g_mcLastFileCmdTick = 0;
        bool g_mcSlotPickSuppress = false; // GetDir re-run after a pick
        std::atomic<bool> g_slotPickActive{false};
        std::atomic<int32_t> g_slotPickPort{0};
        std::atomic<uint64_t> g_slotPickToken{0};
        std::atomic<PS2Runtime *> g_slotPickRuntime{nullptr};
        uint64_t g_slotPickNextToken = 1;
        constexpr uint32_t kMcSlotWaitType = 0x4D43;  // 'MC'
        constexpr uint64_t kMcSlotPickGapTicks = 120; // 2 s of vsyncs without card access = a new load/save

        constexpr int32_t kCvMcSaveFileBytes = 0x838;
        constexpr int32_t kCvMcConfigFileBytes = 0x34;
        constexpr int32_t kCvMcIconInfoBytes = 0x3C4;
        constexpr int32_t kCvMcIconFileBytes = 0xB3F8;

        constexpr int32_t cvMcKilobytes(int32_t bytes)
        {
            return (bytes + 1023) / 1024;
        }

        constexpr int32_t kCvMcRequiredFreeKb =
            cvMcKilobytes(kCvMcSaveFileBytes) * 15 +
            cvMcKilobytes(kCvMcConfigFileBytes) +
            cvMcKilobytes(kCvMcIconInfoBytes) +
            cvMcKilobytes(kCvMcIconFileBytes) + 11;

        bool isValidMcPortSlot(int32_t port, int32_t slot)
        {
            return port >= 0 && port < static_cast<int32_t>(g_mcPorts.size()) && slot == 0;
        }

        std::filesystem::path getMcBaseRootPath(int32_t port)
        {
            const PS2Runtime::IoPaths &paths = PS2Runtime::getIoPaths();
            std::filesystem::path root = paths.mcRoot;
            if (root.empty())
            {
                if (!paths.elfDirectory.empty())
                {
                    root = paths.elfDirectory / "mc0";
                }
                else
                {
                    std::error_code ec;
                    const std::filesystem::path cwd = std::filesystem::current_path(ec);
                    root = ec ? std::filesystem::path("mc0") : (cwd / "mc0");
                }
            }

            root = root.lexically_normal();
            if (port <= 0)
            {
                return root;
            }

            const std::filesystem::path parent = root.parent_path();
            const std::string leaf = root.filename().string();
            const std::string lowerLeaf = toLowerAscii(leaf);
            if (lowerLeaf == "mc0")
            {
                return (parent / "mc1").lexically_normal();
            }
            if (leaf.empty())
            {
                return (root / "mc1").lexically_normal();
            }

            return (parent / (leaf + "_slot" + std::to_string(port))).lexically_normal();
        }

        std::filesystem::path mcSlotsDir(int32_t port)
        {
            const std::filesystem::path base = getMcBaseRootPath(port);
            return (base.parent_path() / (base.filename().string() + "_slots")).lexically_normal();
        }

        std::filesystem::path mcSlotFolder(int32_t port, int slot)
        {
            if (slot <= 1)
            {
                return getMcBaseRootPath(port);
            }
            return (mcSlotsDir(port) / std::to_string(slot)).lexically_normal();
        }

        // Saves made with the retired MultipleSaves mod (mods/MultipleSaves/slots/<name>/mc0, mc1) are
        // copied once into the next free slots; the old folders are left as they were.
        void importOldModSlots(int32_t port)
        {
            const std::filesystem::path oldSlots = ps2x::mods::modsRoot() / "MultipleSaves" / "slots";
            const std::filesystem::path marker = mcSlotsDir(port) / "imported_multiplesaves.txt";
            std::error_code ec;
            if (ps2x::mods::modsRoot().empty() || !std::filesystem::is_directory(oldSlots, ec) ||
                std::filesystem::exists(marker, ec))
            {
                return;
            }
            std::vector<std::filesystem::path> names;
            for (const auto &entry : std::filesystem::directory_iterator(oldSlots, ec))
            {
                std::error_code entryEc;
                if (entry.is_directory(entryEc))
                    names.push_back(entry.path());
            }
            std::sort(names.begin(), names.end());
            std::filesystem::create_directories(mcSlotsDir(port), ec);
            std::ofstream log(marker);
            int next = 2;
            for (const std::filesystem::path &old : names)
            {
                const std::filesystem::path card = old / (port == 0 ? "mc0" : "mc1");
                if (!std::filesystem::is_directory(card, ec) || std::filesystem::is_empty(card, ec))
                    continue;
                while (std::filesystem::exists(mcSlotFolder(port, next), ec))
                    ++next;
                std::filesystem::copy(card, mcSlotFolder(port, next), std::filesystem::copy_options::recursive, ec);
                log << "slot " << next << " = " << old.filename().string() << "\n";
                std::fprintf(stderr, "[MC] card %d: imported old save slot '%s' as slot %d\n", port,
                             old.filename().string().c_str(), next);
                ++next;
            }
        }

        void loadActiveSlotLocked(int32_t port)
        {
            const size_t index = static_cast<size_t>(port);
            if (g_mcSlotLoaded[index])
            {
                return;
            }
            g_mcSlotLoaded[index] = true;
            importOldModSlots(port);
            int slot = 1;
            std::ifstream in(mcSlotsDir(port) / "active.txt");
            if (!(in >> slot) || slot < 1)
            {
                slot = 1;
            }
            g_mcActiveSlot[index] = slot;
            if (slot > 1 && g_mcFolderOverride[index].empty())
            {
                g_mcFolderOverride[index] = mcSlotFolder(port, slot);
            }
        }

        std::filesystem::path getMcRootPath(int32_t port)
        {
            if (port >= 0 && port < static_cast<int32_t>(g_mcFolderOverride.size()))
            {
                loadActiveSlotLocked(port);
                if (!g_mcFolderOverride[static_cast<size_t>(port)].empty())
                {
                    return g_mcFolderOverride[static_cast<size_t>(port)].lexically_normal();
                }
            }
            return getMcBaseRootPath(port);
        }

        void ensureMcRootExists(int32_t port)
        {
            std::error_code ec;
            std::filesystem::create_directories(getMcRootPath(port), ec);
        }

        std::vector<std::string> splitMcPathComponents(const std::string &value)
        {
            std::vector<std::string> parts;
            std::string current;
            for (char c : value)
            {
                if (c == '/' || c == '\\')
                {
                    if (!current.empty())
                    {
                        parts.push_back(current);
                        current.clear();
                    }
                }
                else
                {
                    current.push_back(c);
                }
            }

            if (!current.empty())
            {
                parts.push_back(current);
            }

            return parts;
        }

        std::string joinMcPathComponents(const std::vector<std::string> &parts)
        {
            if (parts.empty())
            {
                return "/";
            }

            std::string joined = "/";
            for (size_t i = 0; i < parts.size(); ++i)
            {
                if (i != 0u)
                {
                    joined.push_back('/');
                }
                joined.append(parts[i]);
            }
            return joined;
        }

        std::string normalizeGuestMcPathLocked(int32_t port, std::string path)
        {
            std::replace(path.begin(), path.end(), '\\', '/');
            const std::string lower = toLowerAscii(path);
            if (lower.rfind("mc0:", 0) == 0 || lower.rfind("mc1:", 0) == 0)
            {
                path = path.substr(4);
            }

            const bool absolute = !path.empty() && path.front() == '/';
            std::vector<std::string> parts;
            if (!absolute && port >= 0 && port < static_cast<int32_t>(g_mcPorts.size()))
            {
                parts = splitMcPathComponents(g_mcPorts[static_cast<size_t>(port)].currentDir);
            }

            for (const std::string &part : splitMcPathComponents(path))
            {
                if (part.empty() || part == ".")
                {
                    continue;
                }

                if (part == "..")
                {
                    if (!parts.empty())
                    {
                        parts.pop_back();
                    }
                    continue;
                }

                parts.push_back(part);
            }

            return joinMcPathComponents(parts);
        }

        std::filesystem::path guestMcPathToHostPath(int32_t port, const std::string &guestPath)
        {
            std::filesystem::path resolved = getMcRootPath(port);
            if (guestPath.size() > 1u)
            {
                resolved /= std::filesystem::path(guestPath.substr(1));
            }
            return resolved.lexically_normal();
        }

        bool localtimeSafeMc(const std::time_t *value, std::tm *out)
        {
#ifdef _WIN32
            return localtime_s(out, value) == 0;
#else
            return localtime_r(value, out) != nullptr;
#endif
        }

        std::time_t fileTimeToTimeTMc(std::filesystem::file_time_type value)
        {
            const auto systemTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                value - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
            return std::chrono::system_clock::to_time_t(systemTime);
        }

        void writeMcCString(uint8_t *rdram, uint32_t addr, const std::string &value)
        {
            if (addr == 0u)
            {
                return;
            }

            uint8_t *dst = getMemPtr(rdram, addr);
            if (!dst)
            {
                return;
            }

            std::memcpy(dst, value.c_str(), value.size() + 1u);
        }

        void writeMcDateTime(SceMcStDateTime &out, std::time_t value)
        {
            std::tm tm{};
            int64_t frame = 0, coopLocal = 0;
            bool ok;
            if (ps2NetplayClock(frame, coopLocal))
            {
                // Co-op: file times differ per machine, so every entry shows the shared clock.
                value = static_cast<std::time_t>(coopLocal);
#ifdef _WIN32
                ok = gmtime_s(&tm, &value) == 0;
#else
                ok = gmtime_r(&value, &tm) != nullptr;
#endif
            }
            else
                ok = localtimeSafeMc(&value, &tm);
            if (!ok)
            {
                std::memset(&out, 0, sizeof(out));
                return;
            }

            out.Resv2 = 0;
            out.Sec = static_cast<uint8_t>(tm.tm_sec);
            out.Min = static_cast<uint8_t>(tm.tm_min);
            out.Hour = static_cast<uint8_t>(tm.tm_hour);
            out.Day = static_cast<uint8_t>(tm.tm_mday);
            out.Month = static_cast<uint8_t>(tm.tm_mon + 1);
            out.Year = static_cast<uint16_t>(tm.tm_year + 1900);
        }

        void fillMcDirTableEntry(SceMcTblGetDir &entry,
                                 const std::string &name,
                                 bool isDirectory,
                                 uint32_t sizeBytes,
                                 std::time_t modifiedTime)
        {
            std::memset(&entry, 0, sizeof(entry));
            writeMcDateTime(entry._Create, modifiedTime);
            writeMcDateTime(entry._Modify, modifiedTime);
            entry.FileSizeByte = isDirectory ? 0u : sizeBytes;
            entry.AttrFile = static_cast<uint16_t>(kMcAttrReadable |
                                                   kMcAttrWriteable |
                                                   (isDirectory ? kMcAttrSubdir : kMcAttrFile) |
                                                   kMcAttrClosed |
                                                   kMcAttrExists);
            std::strncpy(entry.EntryName, name.c_str(), sizeof(entry.EntryName) - 1u);
            entry.EntryName[sizeof(entry.EntryName) - 1u] = '\0';
        }

        bool wildcardMatch(const std::string &pattern, const std::string &value)
        {
            size_t patternPos = 0u;
            size_t valuePos = 0u;
            size_t starPos = std::string::npos;
            size_t matchPos = 0u;

            while (valuePos < value.size())
            {
                if (patternPos < pattern.size() &&
                    (pattern[patternPos] == '?' || pattern[patternPos] == value[valuePos]))
                {
                    ++patternPos;
                    ++valuePos;
                }
                else if (patternPos < pattern.size() && pattern[patternPos] == '*')
                {
                    starPos = patternPos++;
                    matchPos = valuePos;
                }
                else if (starPos != std::string::npos)
                {
                    patternPos = starPos + 1u;
                    valuePos = ++matchPos;
                }
                else
                {
                    return false;
                }
            }

            while (patternPos < pattern.size() && pattern[patternPos] == '*')
            {
                ++patternPos;
            }

            return patternPos == pattern.size();
        }

        void setMcCommandResultLocked(int32_t cmd, int32_t result)
        {
            g_mcLastCmd = cmd;
            g_mcLastResult = result;
            g_mcCommandPending = true;
            if (cmd != kMcCmdGetInfo)
            {
                g_mcLastFileCmdTick = g_ps2xVsyncTickCount.load(std::memory_order_relaxed);
            }
        }

        // When a slot was last saved ("" = empty slot).
        std::string mcSlotSavedText(const std::filesystem::path &folder)
        {
            bool any = false;
            std::filesystem::file_time_type newest{};
            std::error_code ec;
            for (std::filesystem::recursive_directory_iterator it(
                     folder, std::filesystem::directory_options::skip_permission_denied, ec), end;
                 !ec && it != end; it.increment(ec))
            {
                std::error_code entryEc;
                if (!it->is_regular_file(entryEc))
                {
                    continue;
                }
                const auto time = it->last_write_time(entryEc);
                if (!entryEc && (!any || time > newest))
                {
                    newest = time;
                    any = true;
                }
            }
            if (!any)
            {
                return {};
            }
            std::time_t value = fileTimeToTimeTMc(newest);
            std::tm tm{};
            char text[64] = "saved";
            if (localtimeSafeMc(&value, &tm))
            {
                std::strftime(text, sizeof(text), "%d %b %Y %H:%M", &tm);
            }
            return text;
        }

        // Names given to slots in the slot picker: one "<slot>\t<name>" line each in <card>_slots/names.txt.
        std::map<int, std::string> readSlotNames(int32_t port)
        {
            std::map<int, std::string> names;
            std::ifstream in(mcSlotsDir(port) / "names.txt");
            std::string line;
            while (std::getline(in, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                const size_t tab = line.find('\t');
                const int slot = tab == std::string::npos ? 0 : std::atoi(line.substr(0, tab).c_str());
                if (slot >= 1 && tab + 1 < line.size())
                    names[slot] = line.substr(tab + 1);
            }
            return names;
        }

        void applySaveSlotLocked(int32_t port, int slot)
        {
            const size_t index = static_cast<size_t>(port);
            loadActiveSlotLocked(port);
            g_mcActiveSlot[index] = slot;
            g_mcFolderOverride[index] = slot > 1 ? mcSlotFolder(port, slot) : std::filesystem::path{};
            g_mcPorts[index].currentDir = "/";
            g_mcPorts[index].formatted = true;
            std::error_code ec;
            std::filesystem::create_directories(getMcRootPath(port), ec);
            std::filesystem::create_directories(mcSlotsDir(port), ec);
            std::ofstream(mcSlotsDir(port) / "active.txt") << slot << "\n";
            std::fprintf(stderr, "[MC] card %d: save slot %d (%s)\n", port, slot, getMcRootPath(port).string().c_str());
        }

        bool mcPortHasOpenFilesLocked(int32_t port)
        {
            for (const auto &[fd, file] : g_mcFiles)
            {
                if (file.port == port)
                {
                    return true;
                }
            }
            return false;
        }

        int saveSlotAutoPick()
        {
            const char *value = std::getenv("PS2X_SAVE_SLOT_AUTOPICK");
            return value ? std::atoi(value) : 0;
        }

        void closeMcFilesLocked()
        {
            for (auto &[fd, openFile] : g_mcFiles)
            {
                if (openFile.file)
                {
                    std::fclose(openFile.file);
                    openFile.file = nullptr;
                }
            }
            g_mcFiles.clear();
        }

        void closeMcFilesForPortLocked(int32_t port)
        {
            for (auto it = g_mcFiles.begin(); it != g_mcFiles.end();)
            {
                if (it->second.port == port)
                {
                    if (it->second.file)
                    {
                        std::fclose(it->second.file);
                    }
                    it = g_mcFiles.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        int32_t allocateMcFdLocked(FILE *file, int32_t port, const std::filesystem::path &hostPath)
        {
            if (!file)
            {
                return kMcResultDeniedPermit;
            }
            if (g_mcFiles.size() >= kMcMaxOpenFiles)
            {
                return kMcResultUpLimitHandle;
            }

            for (int attempt = 0; attempt < 0x10000; ++attempt)
            {
                if (g_mcNextFd <= 0)
                {
                    g_mcNextFd = 1;
                }

                const int32_t fd = g_mcNextFd++;
                if (g_mcFiles.find(fd) != g_mcFiles.end())
                {
                    continue;
                }

                g_mcFiles.emplace(fd, McOpenFile{file, port, hostPath});
                return fd;
            }

            return kMcResultUpLimitHandle;
        }

        FILE *openMcHostFile(const std::filesystem::path &hostPath, uint32_t flags)
        {
            const uint32_t access = flags & PS2_FIO_O_RDWR;
            const bool read = (access == PS2_FIO_O_RDONLY) || (access == PS2_FIO_O_RDWR);
            const bool write = (access == PS2_FIO_O_WRONLY) || (access == PS2_FIO_O_RDWR);
            const bool append = (flags & PS2_FIO_O_APPEND) != 0u;
            const bool create = (flags & PS2_FIO_O_CREAT) != 0u;
            const bool truncate = (flags & PS2_FIO_O_TRUNC) != 0u;

            std::error_code ec;
            const bool exists = std::filesystem::exists(hostPath, ec) && !ec;

            const char *mode = "rb";
            if (read && write)
            {
                if (append)
                {
                    mode = exists ? "a+b" : "w+b";
                }
                else if (truncate || (create && !exists))
                {
                    mode = "w+b";
                }
                else
                {
                    mode = "r+b";
                }
            }
            else if (write)
            {
                if (append)
                {
                    mode = exists ? "ab" : "wb";
                }
                else if (truncate || (create && !exists))
                {
                    mode = "wb";
                }
                else
                {
                    mode = "r+b";
                }
            }

            return std::fopen(hostPath.string().c_str(), mode);
        }
    }

    bool setMemoryCardFolder(int32_t port, const std::filesystem::path &folder)
    {
        std::lock_guard<std::mutex> lock(g_mcStateMutex);
        if (port < 0 || port >= static_cast<int32_t>(g_mcPorts.size()))
            return false;
        for (const auto &[fd, file] : g_mcFiles)
        {
            if (file.port == port)
                return false; // the game is reading or writing this card right now
        }
        const size_t index = static_cast<size_t>(port);
        if (g_mcFolderOverride[index] == folder)
            return true;
        g_mcFolderOverride[index] = folder;
        g_mcPorts[index].currentDir = "/";
        g_mcPorts[index].formatted = true;
        // Like pulling one card and pushing in another: the game's next GetInfo says "changed card".
        g_mcSwapped[index] = g_mcSeen[index];
        std::error_code ec;
        std::filesystem::create_directories(getMcRootPath(port), ec);
        std::fprintf(stderr, "[MC] card %d -> %s\n", port, getMcRootPath(port).string().c_str());
        return true;
    }

    std::filesystem::path memoryCardFolder(int32_t port)
    {
        std::lock_guard<std::mutex> lock(g_mcStateMutex);
        return getMcRootPath(port);
    }

    bool memoryCardBusy()
    {
        std::lock_guard<std::mutex> lock(g_mcStateMutex);
        return !g_mcFiles.empty();
    }

    std::vector<SaveSlotInfo> listSaveSlots(int32_t port)
    {
        std::vector<SaveSlotInfo> slots;
        std::lock_guard<std::mutex> lock(g_mcStateMutex);
        if (port < 0 || port >= static_cast<int32_t>(g_mcPorts.size()))
            return slots;
        loadActiveSlotLocked(port);
        int lastSlot = std::max(1, g_mcActiveSlot[static_cast<size_t>(port)]);
        std::error_code ec;
        for (const auto &entry : std::filesystem::directory_iterator(mcSlotsDir(port), ec))
        {
            std::error_code entryEc;
            const std::string name = entry.path().filename().string();
            if (entry.is_directory(entryEc) && !name.empty() && name.size() < 6 &&
                name.find_first_not_of("0123456789") == std::string::npos)
                lastSlot = std::max(lastSlot, std::atoi(name.c_str()));
        }
        const std::map<int, std::string> names = readSlotNames(port);
        for (int slot = 1; slot <= lastSlot; ++slot)
        {
            const std::filesystem::path folder = mcSlotFolder(port, slot);
            const auto name = names.find(slot);
            slots.push_back(SaveSlotInfo{slot, folder, mcSlotSavedText(folder), name == names.end() ? std::string() : name->second});
        }
        return slots;
    }

    void setSaveSlotName(int32_t port, int slot, const std::string &name)
    {
        std::lock_guard<std::mutex> lock(g_mcStateMutex);
        if (port < 0 || port >= static_cast<int32_t>(g_mcPorts.size()) || slot < 1)
            return;
        std::string clean;
        for (char c : name)
        {
            if (c != '\t' && c != '\n' && c != '\r')
                clean += c;
        }
        clean.erase(0, clean.find_first_not_of(' '));
        clean.erase(clean.find_last_not_of(' ') + 1);
        std::map<int, std::string> names = readSlotNames(port);
        if (clean.empty())
            names.erase(slot);
        else
            names[slot] = clean;
        std::error_code ec;
        std::filesystem::create_directories(mcSlotsDir(port), ec);
        std::ofstream out(mcSlotsDir(port) / "names.txt", std::ios::trunc);
        for (const auto &[index, text] : names)
            out << index << '\t' << text << '\n';
    }

    int activeSaveSlot(int32_t port)
    {
        std::lock_guard<std::mutex> lock(g_mcStateMutex);
        if (port < 0 || port >= static_cast<int32_t>(g_mcPorts.size()))
            return 1;
        loadActiveSlotLocked(port);
        return g_mcActiveSlot[static_cast<size_t>(port)];
    }

    SaveSlotPickRequest pendingSaveSlotPick()
    {
        SaveSlotPickRequest request;
        request.active = g_slotPickActive.load(std::memory_order_acquire);
        request.port = g_slotPickPort.load();
        return request;
    }

    void resolveSaveSlotPick(int slot)
    {
        if (!g_slotPickActive.exchange(false))
            return;
        const int32_t port = g_slotPickPort.load();
        if (slot >= 1)
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            applySaveSlotLocked(port, slot);
        }
        if (PS2Runtime *runtime = g_slotPickRuntime.load())
            runtime->postEeEvent(EeEvent{EeEventType::ExternalWake, kMcSlotWaitType, g_slotPickToken.load()});
    }

    MemoryCardDebugSnapshot getMemoryCardDebugSnapshot()
    {
        MemoryCardDebugSnapshot snapshot{};
        std::lock_guard<std::mutex> lock(g_mcStateMutex);
        snapshot.nextFd = g_mcNextFd;
        snapshot.lastCmd = g_mcLastCmd;
        snapshot.lastResult = g_mcLastResult;
        snapshot.cvFileCursor = g_cvMcFileCursor;

        for (size_t i = 0; i < g_mcPorts.size(); ++i)
        {
            snapshot.ports[i].port = static_cast<int32_t>(i);
            snapshot.ports[i].currentDir = g_mcPorts[i].currentDir;
            snapshot.ports[i].formatted = g_mcPorts[i].formatted;
            snapshot.ports[i].rootPath = getMcRootPath(static_cast<int32_t>(i));
        }

        snapshot.openFiles.reserve(g_mcFiles.size());
        for (const auto &[fd, file] : g_mcFiles)
        {
            MemoryCardDebugOpenFile row{};
            row.fd = fd;
            row.port = file.port;
            row.hostPath = file.hostPath;
            snapshot.openFiles.push_back(std::move(row));
        }
        std::sort(snapshot.openFiles.begin(), snapshot.openFiles.end(), [](const MemoryCardDebugOpenFile &a, const MemoryCardDebugOpenFile &b)
                  { return a.fd < b.fd; });
        return snapshot;
    }

    void sceMcChangeThreadPriority(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceMcChdir(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t port = static_cast<int32_t>(getRegU32(ctx, 4));
        const int32_t slot = static_cast<int32_t>(getRegU32(ctx, 5));
        const uint32_t pathAddr = getRegU32(ctx, 6);
        const uint32_t currentDirAddr = getRegU32(ctx, 7);
        const std::string requestedDir =
            (pathAddr != 0u) ? readPs2CStringBounded(rdram, pathAddr, kMcMaxPathLen) : std::string{};

        std::string currentDir = "/";
        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            if (isValidMcPortSlot(port, slot))
            {
                McPortState &state = g_mcPorts[static_cast<size_t>(port)];
                currentDir = state.currentDir;
                if (!state.formatted)
                {
                    result = kMcResultNoFormat;
                }
                else
                {
                    ensureMcRootExists(port);
                    const std::string resolvedDir =
                        requestedDir.empty() ? state.currentDir : normalizeGuestMcPathLocked(port, requestedDir);
                    const std::filesystem::path hostDir = guestMcPathToHostPath(port, resolvedDir);
                    std::error_code ec;
                    if (std::filesystem::exists(hostDir, ec) && !ec &&
                        std::filesystem::is_directory(hostDir, ec))
                    {
                        state.currentDir = resolvedDir;
                        currentDir = resolvedDir;
                        result = kMcResultSucceed;
                    }
                }
            }

            setMcCommandResultLocked(kMcCmdChdir, result);
        }

        RUNTIME_LOG("[MC] Chdir port=" << port << " '" << requestedDir
                                       << "' -> result=" << result << " cwd='" << currentDir << "'");
        writeMcCString(rdram, currentDirAddr, currentDir);
        setReturnS32(ctx, 0);
    }

    void sceMcClose(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t fd = static_cast<int32_t>(getRegU32(ctx, 4));
        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            auto it = g_mcFiles.find(fd);
            if (it != g_mcFiles.end())
            {
                if (!it->second.file || std::fclose(it->second.file) == 0)
                {
                    result = kMcResultSucceed;
                }
                g_mcFiles.erase(it);
            }
            setMcCommandResultLocked(kMcCmdClose, result);
        }
        setReturnS32(ctx, 0);
    }

    void sceMcDelete(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t port = static_cast<int32_t>(getRegU32(ctx, 4));
        const int32_t slot = static_cast<int32_t>(getRegU32(ctx, 5));
        const std::string path = readPs2CStringBounded(rdram, getRegU32(ctx, 6), kMcMaxPathLen);

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            if (isValidMcPortSlot(port, slot))
            {
                McPortState &state = g_mcPorts[static_cast<size_t>(port)];
                if (!state.formatted)
                {
                    result = kMcResultNoFormat;
                }
                else
                {
                    const std::string guestPath = normalizeGuestMcPathLocked(port, path);
                    if (guestPath != "/")
                    {
                        const std::filesystem::path hostPath = guestMcPathToHostPath(port, guestPath);
                        std::error_code ec;
                        if (std::filesystem::exists(hostPath, ec) && !ec)
                        {
                            if (std::filesystem::is_directory(hostPath, ec) &&
                                !std::filesystem::is_empty(hostPath, ec))
                            {
                                result = kMcResultNotEmpty;
                            }
                            else if (std::filesystem::remove(hostPath, ec) && !ec)
                            {
                                result = kMcResultSucceed;
                            }
                            else
                            {
                                result = kMcResultDeniedPermit;
                            }
                        }
                    }
                }
            }

            setMcCommandResultLocked(kMcCmdDelete, result);
        }
        setReturnS32(ctx, 0);
    }

    void sceMcEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            closeMcFilesLocked();
            g_mcNextFd = 1;
            g_mcLastCmd = 0;
            g_mcLastResult = 0;
            g_mcCommandPending = false;
            for (McPortState &state : g_mcPorts)
            {
                state.currentDir = "/";
            }
        }

        // libmc teardown is just a local state reset in this immediate runtime model.
        setReturnS32(ctx, 0);
    }

    void sceMcFlush(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t fd = static_cast<int32_t>(getRegU32(ctx, 4));
        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            auto it = g_mcFiles.find(fd);
            if (it != g_mcFiles.end() && it->second.file)
            {
                result = (std::fflush(it->second.file) == 0) ? kMcResultSucceed : kMcResultDeniedPermit;
            }
            setMcCommandResultLocked(kMcCmdFlush, result);
        }
        setReturnS32(ctx, 0);
    }

    void sceMcFormat(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t port = static_cast<int32_t>(getRegU32(ctx, 4));
        const int32_t slot = static_cast<int32_t>(getRegU32(ctx, 5));

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            if (isValidMcPortSlot(port, slot))
            {
                closeMcFilesForPortLocked(port);
                const std::filesystem::path root = getMcRootPath(port);
                std::error_code ec;
                std::filesystem::remove_all(root, ec);
                ec.clear();
                std::filesystem::create_directories(root, ec);
                if (!ec)
                {
                    McPortState &state = g_mcPorts[static_cast<size_t>(port)];
                    state.currentDir = "/";
                    state.formatted = true;
                    result = kMcResultSucceed;
                }
            }

            setMcCommandResultLocked(kMcCmdFormat, result);
        }
        setReturnS32(ctx, 0);
    }

    void sceMcGetDir(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t port = static_cast<int32_t>(getRegU32(ctx, 4));
        const int32_t slot = static_cast<int32_t>(getRegU32(ctx, 5));
        const std::string rawPath = readPs2CStringBounded(rdram, getRegU32(ctx, 6), kMcMaxPathLen);
        const int32_t maxEntries = static_cast<int32_t>(getRegU32(ctx, 8));
        const uint32_t tableAddr = getRegU32(ctx, 9);

        // Every PS2 load or save starts by looking for the save's icon.sys: ask which slot to use
        // first, unless the card was in use moments ago (still the same load/save).
        const std::string lowerPath = toLowerAscii(rawPath);
        if (!g_mcSlotPickSuppress && runtime && isValidMcPortSlot(port, slot) && lowerPath.size() >= 8u &&
            lowerPath.compare(lowerPath.size() - 8u, 8u, "icon.sys") == 0)
        {
            bool ask = false;
            {
                std::lock_guard<std::mutex> lock(g_mcStateMutex);
                const uint64_t now = g_ps2xVsyncTickCount.load(std::memory_order_relaxed);
                ask = !mcPortHasOpenFilesLocked(port) && now - g_mcLastFileCmdTick > kMcSlotPickGapTicks;
                // Co-op: both games already use the same card (the host's, sent at boot); a picker would
                // stall the lockstep and could put each player on a different slot.
                if (ps2NetplayActive())
                    ask = false;
                const int autoSlot = ask ? saveSlotAutoPick() : 0;
                if (autoSlot >= 1)
                {
                    applySaveSlotLocked(port, autoSlot);
                    g_mcLastFileCmdTick = now;
                    ask = false;
                }
            }
            if (ask)
            {
                const uint64_t token = g_slotPickNextToken++;
                g_slotPickPort.store(port);
                g_slotPickToken.store(token);
                g_slotPickRuntime.store(runtime);
                g_slotPickActive.store(true, std::memory_order_release);
                std::fprintf(stderr, "[MC] card %d: asking which save slot to use\n", port);
                runtime->eeScheduler().waitExternal(
                    EeWaitReason::External, kMcSlotWaitType, token,
                    [rdram, runtime](R5900Context &resumeContext)
                    {
                        if (static_cast<int32_t>(getRegU32(&resumeContext, 2)) < 0)
                        {
                            return;
                        }
                        g_mcSlotPickSuppress = true;
                        sceMcGetDir(rdram, &resumeContext, runtime);
                        g_mcSlotPickSuppress = false;
                    });
            }
        }

        std::vector<SceMcTblGetDir> entries;
        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            if (isValidMcPortSlot(port, slot))
            {
                McPortState &state = g_mcPorts[static_cast<size_t>(port)];
                if (!state.formatted)
                {
                    result = kMcResultNoFormat;
                }
                else
                {
                    ensureMcRootExists(port);
                    const std::string guestQuery =
                        normalizeGuestMcPathLocked(port, rawPath.empty() ? "." : rawPath);
                    const bool hasWildcard =
                        guestQuery.find('*') != std::string::npos || guestQuery.find('?') != std::string::npos;

                    const std::filesystem::path queryRel =
                        (guestQuery.size() > 1u) ? std::filesystem::path(guestQuery.substr(1)) : std::filesystem::path{};

                    std::filesystem::path parentRel;
                    std::string pattern;
                    if (hasWildcard)
                    {
                        parentRel = queryRel.parent_path();
                        pattern = queryRel.filename().string();
                    }
                    else
                    {
                        const std::filesystem::path queryHostPath = guestMcPathToHostPath(port, guestQuery);
                        std::error_code queryEc;
                        if (std::filesystem::exists(queryHostPath, queryEc) && !queryEc &&
                            std::filesystem::is_directory(queryHostPath, queryEc))
                        {
                            parentRel = queryRel;
                            pattern = "*";
                        }
                        else
                        {
                            parentRel = queryRel.parent_path();
                            pattern = queryRel.filename().string();
                        }
                    }

                    if (pattern.empty())
                    {
                        pattern = "*";
                    }

                    std::filesystem::path hostDir = getMcRootPath(port);
                    if (!parentRel.empty())
                    {
                        hostDir /= parentRel;
                    }
                    hostDir = hostDir.lexically_normal();

                    std::error_code ec;
                    if (std::filesystem::exists(hostDir, ec) && !ec &&
                        std::filesystem::is_directory(hostDir, ec))
                    {
                        const std::time_t now = std::time(nullptr);
                        auto appendSpecial = [&](const std::string &name)
                        {
                            if (!wildcardMatch(pattern, name))
                            {
                                return;
                            }
                            SceMcTblGetDir entry{};
                            fillMcDirTableEntry(entry, name, true, 0u, now);
                            entries.push_back(entry);
                        };

                        appendSpecial(".");
                        appendSpecial("..");

                        std::vector<std::filesystem::directory_entry> dirEntries;
                        for (const auto &entry : std::filesystem::directory_iterator(
                                 hostDir, std::filesystem::directory_options::skip_permission_denied, ec))
                        {
                            if (ec)
                            {
                                break;
                            }
                            dirEntries.push_back(entry);
                        }

                        std::sort(dirEntries.begin(), dirEntries.end(),
                                  [](const std::filesystem::directory_entry &lhs,
                                     const std::filesystem::directory_entry &rhs)
                                  {
                                      return toLowerAscii(lhs.path().filename().string()) <
                                             toLowerAscii(rhs.path().filename().string());
                                  });

                        for (const auto &entry : dirEntries)
                        {
                            const std::string name = entry.path().filename().string();
                            if (!wildcardMatch(pattern, name))
                            {
                                continue;
                            }

                            std::error_code entryEc;
                            const bool isDirectory = entry.is_directory(entryEc) && !entryEc;
                            const uint32_t sizeBytes =
                                isDirectory ? 0u : static_cast<uint32_t>(entry.file_size(entryEc));
                            entryEc.clear();
                            const std::time_t modifiedTime = fileTimeToTimeTMc(entry.last_write_time(entryEc));
                            SceMcTblGetDir tableEntry{};
                            fillMcDirTableEntry(tableEntry,
                                                name,
                                                isDirectory,
                                                sizeBytes,
                                                entryEc ? now : modifiedTime);
                            entries.push_back(tableEntry);
                        }

                        const size_t entryCount =
                            std::min(entries.size(), maxEntries > 0 ? static_cast<size_t>(maxEntries) : 0u);
                        if (entryCount == 0u || tableAddr == 0u)
                        {
                            result = static_cast<int32_t>(entryCount);
                        }
                        else if (uint8_t *dst = getMemPtr(rdram, tableAddr))
                        {
                            std::memcpy(dst, entries.data(), entryCount * sizeof(SceMcTblGetDir));
                            result = static_cast<int32_t>(entryCount);
                        }
                        else
                        {
                            result = kMcResultDeniedPermit;
                        }
                    }
                }
            }

            setMcCommandResultLocked(kMcCmdGetDir, result);
        }
        RUNTIME_LOG("[MC] GetDir port=" << port << " '" << rawPath
                                        << "' maxent=" << maxEntries << " -> result=" << result);
        setReturnS32(ctx, 0);
    }

    void sceMcGetEntSpace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            setMcCommandResultLocked(kMcCmdGetEntSpace, 1024);
        }
        setReturnS32(ctx, 0);
    }

    void sceMcGetInfo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t port = static_cast<int32_t>(getRegU32(ctx, 4));
        const int32_t slot = static_cast<int32_t>(getRegU32(ctx, 5));
        const uint32_t typePtr = getRegU32(ctx, 6);
        const uint32_t freePtr = getRegU32(ctx, 7);
        const uint32_t formatPtr = getRegU32(ctx, 8);

        int32_t cardType = 0;
        int32_t freeBlocks = 0;
        int32_t format = kMcUnformatted;
        int32_t result = kMcResultNoEntry;

        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            if (isValidMcPortSlot(port, slot))
            {
                McPortState &state = g_mcPorts[static_cast<size_t>(port)];
                cardType = kMcTypePs2;
                freeBlocks = state.formatted ? kMcFreeClusters : 0;
                format = state.formatted ? kMcFormatted : kMcUnformatted;
                result = state.formatted ? kMcResultSucceed : kMcResultNoFormat;
                g_mcSeen[static_cast<size_t>(port)] = true;
                if (g_mcSwapped[static_cast<size_t>(port)] && state.formatted)
                {
                    g_mcSwapped[static_cast<size_t>(port)] = false;
                    result = kMcResultChangedCard;
                }
            }

            setMcCommandResultLocked(kMcCmdGetInfo, result);
        }

        if (typePtr != 0u)
        {
            if (uint8_t *out = getMemPtr(rdram, typePtr))
            {
                std::memcpy(out, &cardType, sizeof(cardType));
            }
        }
        if (freePtr != 0u)
        {
            if (uint8_t *out = getMemPtr(rdram, freePtr))
            {
                std::memcpy(out, &freeBlocks, sizeof(freeBlocks));
            }
        }
        if (formatPtr != 0u)
        {
            if (uint8_t *out = getMemPtr(rdram, formatPtr))
            {
                std::memcpy(out, &format, sizeof(format));
            }
        }

        RUNTIME_LOG("[MC] GetInfo port=" << port << " type=" << cardType
                                         << " free=" << freeBlocks << " format=" << format
                                         << " result=" << result);
        setReturnS32(ctx, 0);
    }

    void sceMcGetSlotMax(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void sceMcInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            closeMcFilesLocked();
            g_mcNextFd = 1;
            g_mcLastCmd = 0;
            g_mcLastResult = 0;
            g_mcCommandPending = false;
            for (McPortState &state : g_mcPorts)
            {
                state.currentDir = "/";
                state.formatted = true;
            }
        }
        ensureMcRootExists(0);
        ensureMcRootExists(1);
        setReturnS32(ctx, 0);
    }

    void sceMcMkdir(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t port = static_cast<int32_t>(getRegU32(ctx, 4));
        const int32_t slot = static_cast<int32_t>(getRegU32(ctx, 5));
        const std::string path = readPs2CStringBounded(rdram, getRegU32(ctx, 6), kMcMaxPathLen);

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            if (isValidMcPortSlot(port, slot))
            {
                McPortState &state = g_mcPorts[static_cast<size_t>(port)];
                if (!state.formatted)
                {
                    result = kMcResultNoFormat;
                }
                else
                {
                    ensureMcRootExists(port);
                    const std::string guestPath = normalizeGuestMcPathLocked(port, path);
                    const std::filesystem::path hostPath = guestMcPathToHostPath(port, guestPath);
                    std::error_code ec;
                    if (std::filesystem::exists(hostPath, ec) && !ec)
                    {
                        result = std::filesystem::is_directory(hostPath, ec) ? kMcResultSucceed : kMcResultDeniedPermit;
                    }
                    else if (std::filesystem::create_directory(hostPath, ec) && !ec)
                    {
                        result = kMcResultSucceed;
                    }
                    else
                    {
                        result = std::filesystem::exists(hostPath.parent_path(), ec) && !ec
                                     ? kMcResultDeniedPermit
                                     : kMcResultNoEntry;
                    }
                }
            }

            setMcCommandResultLocked(kMcCmdMkdir, result);
        }
        setReturnS32(ctx, 0);
    }

    void sceMcOpen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t port = static_cast<int32_t>(getRegU32(ctx, 4));
        const int32_t slot = static_cast<int32_t>(getRegU32(ctx, 5));
        const std::string path = readPs2CStringBounded(rdram, getRegU32(ctx, 6), kMcMaxPathLen);
        const uint32_t flags = getRegU32(ctx, 7);

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            if (isValidMcPortSlot(port, slot))
            {
                McPortState &state = g_mcPorts[static_cast<size_t>(port)];
                if (!state.formatted)
                {
                    result = kMcResultNoFormat;
                }
                else
                {
                    const std::string guestPath = normalizeGuestMcPathLocked(port, path);
                    const std::filesystem::path hostPath = guestMcPathToHostPath(port, guestPath);
                    std::error_code ec;
                    const bool create = (flags & PS2_FIO_O_CREAT) != 0u;
                    const bool exists = std::filesystem::exists(hostPath, ec) && !ec;
                    if (guestPath == "/")
                    {
                        result = kMcResultDeniedPermit;
                    }
                    else if (exists && std::filesystem::is_directory(hostPath, ec))
                    {
                        result = kMcResultDeniedPermit;
                    }
                    else if (!exists && !create)
                    {
                        result = kMcResultNoEntry;
                    }
                    else if (!std::filesystem::exists(hostPath.parent_path(), ec) || ec)
                    {
                        result = kMcResultNoEntry;
                    }
                    else
                    {
                        FILE *file = openMcHostFile(hostPath, flags);
                        if (!file)
                        {
                            result = exists ? kMcResultDeniedPermit : kMcResultNoEntry;
                        }
                        else
                        {
                            result = allocateMcFdLocked(file, port, hostPath);
                            if (result < 0)
                            {
                                std::fclose(file);
                            }
                        }
                    }
                }
            }
            setMcCommandResultLocked(kMcCmdOpen, result);
        }
        setReturnS32(ctx, 0);
    }

    void sceMcRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t fd = static_cast<int32_t>(getRegU32(ctx, 4));
        const uint32_t dstAddr = getRegU32(ctx, 5);
        const int32_t size = static_cast<int32_t>(getRegU32(ctx, 6));
        uint8_t *dst = (size > 0) ? getMemPtr(rdram, dstAddr) : nullptr;

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            auto it = g_mcFiles.find(fd);
            if (size <= 0)
            {
                result = 0;
            }
            else if (it == g_mcFiles.end() || !it->second.file)
            {
                result = kMcResultNoEntry;
            }
            else if (!dst)
            {
                result = kMcResultDeniedPermit;
            }
            else
            {
                const size_t bytesRead = std::fread(dst, 1u, static_cast<size_t>(size), it->second.file);
                result = std::ferror(it->second.file) ? kMcResultDeniedPermit : static_cast<int32_t>(bytesRead);
                if (std::ferror(it->second.file))
                {
                    std::clearerr(it->second.file);
                }
            }

            setMcCommandResultLocked(kMcCmdRead, result);
        }
        setReturnS32(ctx, 0);
    }

    void sceMcRename(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t port = static_cast<int32_t>(getRegU32(ctx, 4));
        const int32_t slot = static_cast<int32_t>(getRegU32(ctx, 5));
        const std::string oldPath = readPs2CStringBounded(rdram, getRegU32(ctx, 6), kMcMaxPathLen);
        const std::string newPath = readPs2CStringBounded(rdram, getRegU32(ctx, 7), kMcMaxPathLen);

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            if (isValidMcPortSlot(port, slot))
            {
                McPortState &state = g_mcPorts[static_cast<size_t>(port)];
                if (!state.formatted)
                {
                    result = kMcResultNoFormat;
                }
                else
                {
                    const std::filesystem::path oldHostPath =
                        guestMcPathToHostPath(port, normalizeGuestMcPathLocked(port, oldPath));
                    const std::filesystem::path newHostPath =
                        guestMcPathToHostPath(port, normalizeGuestMcPathLocked(port, newPath));
                    std::error_code ec;
                    if (std::filesystem::exists(oldHostPath, ec) && !ec &&
                        std::filesystem::exists(newHostPath.parent_path(), ec) && !ec)
                    {
                        std::filesystem::rename(oldHostPath, newHostPath, ec);
                        result = ec ? kMcResultDeniedPermit : kMcResultSucceed;
                    }
                }
            }

            setMcCommandResultLocked(kMcCmdRename, result);
        }
        setReturnS32(ctx, 0);
    }

    void sceMcSeek(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t fd = static_cast<int32_t>(getRegU32(ctx, 4));
        const int32_t offset = static_cast<int32_t>(getRegU32(ctx, 5));
        const int32_t origin = static_cast<int32_t>(getRegU32(ctx, 6));

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            auto it = g_mcFiles.find(fd);
            if (it != g_mcFiles.end() && it->second.file)
            {
                int whence = SEEK_SET;
                if (origin == PS2_FIO_SEEK_CUR)
                {
                    whence = SEEK_CUR;
                }
                else if (origin == PS2_FIO_SEEK_END)
                {
                    whence = SEEK_END;
                }

                if (std::fseek(it->second.file, offset, whence) == 0)
                {
                    const long position = std::ftell(it->second.file);
                    result = (position >= 0) ? static_cast<int32_t>(position) : kMcResultDeniedPermit;
                }
                else
                {
                    result = kMcResultDeniedPermit;
                }
            }

            setMcCommandResultLocked(kMcCmdSeek, result);
        }
        setReturnS32(ctx, 0);
    }

    void sceMcSetFileInfo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t port = static_cast<int32_t>(getRegU32(ctx, 4));
        const int32_t slot = static_cast<int32_t>(getRegU32(ctx, 5));
        const std::string path = readPs2CStringBounded(rdram, getRegU32(ctx, 6), kMcMaxPathLen);

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            if (isValidMcPortSlot(port, slot))
            {
                McPortState &state = g_mcPorts[static_cast<size_t>(port)];
                if (!state.formatted)
                {
                    result = kMcResultNoFormat;
                }
                else
                {
                    const std::filesystem::path hostPath =
                        guestMcPathToHostPath(port, normalizeGuestMcPathLocked(port, path));
                    std::error_code ec;
                    if (std::filesystem::exists(hostPath, ec) && !ec)
                    {
                        result = kMcResultSucceed;
                    }
                }
            }

            setMcCommandResultLocked(kMcCmdSetFileInfo, result);
        }
        setReturnS32(ctx, 0);
    }

    void sceMcSync(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t cmdPtr = getRegU32(ctx, 5);
        const uint32_t resultPtr = getRegU32(ctx, 6);
        int32_t cmd = 0;
        int32_t result = 0;
        bool hadPending = false;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            hadPending = g_mcCommandPending;
            g_mcCommandPending = false;
            cmd = g_mcLastCmd;
            result = g_mcLastResult;
        }

        // libmc semantics: -1 means no async operation was executing; games rely
        // on it to tell idle polling apart from command completion.
        if (!hadPending)
        {
            setReturnS32(ctx, -1);
            return;
        }

        RUNTIME_LOG("[MC] Sync cmd=" << cmd << " result=" << result);

        if (cmdPtr != 0u)
        {
            if (uint8_t *out = getMemPtr(rdram, cmdPtr))
            {
                std::memcpy(out, &cmd, sizeof(cmd));
            }
        }
        if (resultPtr != 0u)
        {
            if (uint8_t *out = getMemPtr(rdram, resultPtr))
            {
                std::memcpy(out, &result, sizeof(result));
            }
        }

        // 1 = command finished in this runtime's immediate model.
        setReturnS32(ctx, 1);
    }

    void sceMcUnformat(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t port = static_cast<int32_t>(getRegU32(ctx, 4));
        const int32_t slot = static_cast<int32_t>(getRegU32(ctx, 5));

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            if (isValidMcPortSlot(port, slot))
            {
                closeMcFilesForPortLocked(port);
                const std::filesystem::path root = getMcRootPath(port);
                std::error_code ec;
                std::filesystem::remove_all(root, ec);
                ec.clear();
                std::filesystem::create_directories(root, ec);
                if (!ec)
                {
                    McPortState &state = g_mcPorts[static_cast<size_t>(port)];
                    state.currentDir = "/";
                    state.formatted = false;
                    result = kMcResultSucceed;
                }
            }

            setMcCommandResultLocked(kMcCmdUnformat, result);
        }
        setReturnS32(ctx, 0);
    }

    void sceMcWrite(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t fd = static_cast<int32_t>(getRegU32(ctx, 4));
        const uint32_t srcAddr = getRegU32(ctx, 5);
        const int32_t size = static_cast<int32_t>(getRegU32(ctx, 6));
        const uint8_t *src = (size > 0) ? getConstMemPtr(rdram, srcAddr) : nullptr;

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            auto it = g_mcFiles.find(fd);
            if (size <= 0)
            {
                result = 0;
            }
            else if (it == g_mcFiles.end() || !it->second.file)
            {
                result = kMcResultNoEntry;
            }
            else if (!src)
            {
                result = kMcResultDeniedPermit;
            }
            else
            {
                const size_t bytesWritten = std::fwrite(src, 1u, static_cast<size_t>(size), it->second.file);
                result = std::ferror(it->second.file) ? kMcResultDeniedPermit : static_cast<int32_t>(bytesWritten);
                if (!std::ferror(it->second.file))
                {
                    std::fflush(it->second.file);
                }
                else
                {
                    std::clearerr(it->second.file);
                }
            }

            setMcCommandResultLocked(kMcCmdWrite, result);
        }
        setReturnS32(ctx, 0);
    }

    void mcCallMessageTypeSe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcCheckReadStartConfigFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcCheckReadStartSaveFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcCheckWriteStartConfigFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcCheckWriteStartSaveFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcCreateConfigInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcCreateFileSelectWindow(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcCreateIconInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcCreateSaveFileInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcDispFileName(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcDispFileNumber(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcDisplayFileSelectWindow(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcDisplaySelectFileInfo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcDisplaySelectFileInfoMesCount(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcDispWindowCurSol(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcDispWindowFoundtion(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mceGetInfoApdx(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mceIntrReadFixAlign(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mceStorePwd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcGetConfigCapacitySize(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, kCvMcConfigFileBytes);
    }

    void mcGetFileSelectWindowCursol(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, g_cvMcFileCursor);
    }

    void mcGetFreeCapacitySize(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, kCvMcRequiredFreeKb);
    }

    void mcGetIconCapacitySize(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, kCvMcIconInfoBytes);
    }

    void mcGetIconFileCapacitySize(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, kCvMcIconFileBytes);
    }

    void mcGetPortSelectDirInfo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcGetSaveFileCapacitySize(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, kCvMcSaveFileBytes);
    }

    void mcGetStringEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t strAddr = getRegU32(ctx, 4);
        const std::string value = readPs2CStringBounded(rdram, runtime, strAddr, 1024);
        setReturnU32(ctx, strAddr + static_cast<uint32_t>(value.size()));
    }

    void mcMoveFileSelectWindowCursor(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t delta = static_cast<int32_t>(getRegU32(ctx, 5));
        g_cvMcFileCursor += delta;
        g_cvMcFileCursor = std::clamp(g_cvMcFileCursor, -1, 15);
        setReturnS32(ctx, 0);
    }

    void mcNewCreateConfigFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcNewCreateIcon(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcNewCreateSaveFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcReadIconData(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcReadStartConfigFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcReadStartSaveFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcSelectFileInfoInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_cvMcFileCursor = 0;
        setReturnS32(ctx, 1);
    }

    void mcSelectSaveFileCheck(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcSetFileSelectWindowCursol(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_cvMcFileCursor = static_cast<int32_t>(getRegU32(ctx, 5));
        g_cvMcFileCursor = std::clamp(g_cvMcFileCursor, -1, 15);
        setReturnS32(ctx, 0);
    }

    void mcSetFileSelectWindowCursolInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_cvMcFileCursor = 0;
        setReturnS32(ctx, 0);
    }

    void mcSetStringSaveFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcSetTyepWriteMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcWriteIconData(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcWriteStartConfigFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcWriteStartSaveFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }
}
