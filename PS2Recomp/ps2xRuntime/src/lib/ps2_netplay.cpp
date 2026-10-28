#include "runtime/ps2_netplay.h"
#include "runtime/ps2_memory.h"
#include <cctype>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if PS2X_EOS
#include "runtime/ps2_lan.h"
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define NOMINMAX
#include <windows.h>
#endif
#include <eos_sdk.h>
#include <eos_logging.h>
#include <eos_connect.h>
#include <eos_p2p.h>
#include <eos_lobby.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "Kernel/Stubs/MemoryCard.h"

// Lockstep: the input sampled on frame F is applied on frame F + delay on both machines, so each frame
// only waits on the peer's input when it is more than `delay` frames late. The host is port 0, the joiner port 1.
// The frame clock is the port 0 pad read count; both machines boot the same build, so it lines up.
// EOS runs on its own thread (every EOS call stays there) and talks to the game thread through g_mutex.
namespace
{
    constexpr uint32_t kMagic = 0x4E523253u; // "S2RN"
    constexpr uint32_t kRing = 1024;
    constexpr const char *kSocketName = "s2rnet";
    // The host advertises an EOS lobby so the launcher's Lobbies tab can list it (launcher_lobbies.cpp
    // searches the same bucket and reads the same attributes). Joining still goes by the host's PUID.
    constexpr const char *kLobbyBucket = "s2r:v1";
    // Sent in every hello (Packet::hash). Builds from before the save transfer send 0 there.
    constexpr uint64_t kProtocol = 4;

    enum PacketType : uint8_t
    {
        kHello = 1,
        kInput = 2, // protocol 3 and older; now InputsPacket (kInputs)
        kInputs = 5,
        kHash = 3,
        kClock = 4, // host -> joiner: hash = the host's wall clock at boot (local time, as seconds since 1970)
        // Save transfer (channel 1): the host sends its memory cards so both games boot from the same save.
        kSaveBegin = 10, // a = file count, b = total bytes
        kSaveFile = 11,  // a = file size, payload = "<port>/<path in card>"
        kSaveChunk = 12, // payload = the next bytes of the current file
        kSaveEnd = 13,   // payload = 8-byte hash of every path and byte sent
    };

#pragma pack(push, 1)
    struct Packet
    {
        uint32_t magic;
        uint8_t type;
        uint8_t axes[4];
        uint16_t buttons;
        uint32_t frame;
        uint64_t hash;
    };

    // Pad input (channel 2, unreliable): the last kInputSpan frames up to `last`, so a lost or late
    // packet is covered by the next one instead of stalling both games for a resend.
    constexpr uint32_t kInputSpan = 16;
    struct InputsPacket
    {
        uint32_t magic;
        uint8_t type;
        uint32_t last;
        uint8_t count;
        struct
        {
            uint16_t buttons;
            uint8_t axes[4];
        } in[kInputSpan];
    };

    struct SaveHeader
    {
        uint32_t magic;
        uint8_t type;
        uint32_t a;
        uint32_t b;
    };
#pragma pack(pop)

    constexpr uint8_t kSaveChannel = 1;
    constexpr uint8_t kInputChannel = 2;
    constexpr uint32_t kSaveChunkBytes = 1100; // under EOS_P2P_MAX_PACKET_SIZE with the header

    struct SaveFile
    {
        std::string name; // "<port>/<path in card>"
        std::vector<uint8_t> data;
    };

    struct FrameInput
    {
        int64_t frame = -1;
        uint16_t buttons = 0xFFFFu;
        uint8_t axes[4] = {0x80, 0x80, 0x80, 0x80};
    };

    struct FrameHash
    {
        int64_t frame = -1;
        uint64_t hash = 0;
    };

    const char *netplayMode()
    {
        const char *e = std::getenv("PS2X_NETPLAY");
        return (e && *e && *e != '0') ? e : nullptr;
    }

    const bool g_isHost = [] { const char *m = netplayMode(); return m && std::strcmp(m, "join") != 0; }();
    const uint32_t g_delay = [] {
        const char *e = std::getenv("PS2X_NETPLAY_DELAY");
        const int v = e ? std::atoi(e) : 3;
        return static_cast<uint32_t>(v < 1 ? 1 : (v > 30 ? 30 : v));
    }();
    const uint32_t g_hashEvery = [] {
        const char *e = std::getenv("PS2X_NETPLAY_HASH");
        const int v = e ? std::atoi(e) : 0;
        return static_cast<uint32_t>(v < 0 ? 0 : v);
    }();

    std::mutex g_mutex;
    std::condition_variable g_cv;
    FrameInput g_local[kRing];
    FrameInput g_remote[kRing];
    FrameHash g_localHash[64];
    FrameHash g_remoteHash[64];
    std::deque<Packet> g_outbox;
    int64_t g_localTop = -1; // newest frame in g_local (sent by the network thread)
    bool g_localNew = false;
    bool g_peerGone = false;
    bool g_failed = false;
    std::string g_joinCode; // host's own code, shown while waiting
    std::atomic<bool> g_stopping{false};
    std::atomic<bool> g_waiting{false}; // game thread is blocked on the other player's input
    std::atomic<int64_t> g_frame{0}; // written by the game thread only

    // Shared wall clock: both games see the host's date and time at boot plus game frames / 60,
    // so the game's clock reads (and anything seeded from them) match on both machines.
    std::atomic<bool> g_clockKnown{false};
    std::atomic<int64_t> g_clockEpoch{0};

    int64_t localEpochNow()
    {
        const std::time_t now = std::time(nullptr);
        std::tm l{};
#ifdef _WIN32
        localtime_s(&l, &now);
#else
        localtime_r(&now, &l);
#endif
        // Days from civil (proleptic Gregorian), so the local broken-down time reads back with gmtime.
        int64_t y = l.tm_year + 1900;
        const int64_t m = l.tm_mon + 1;
        y -= m <= 2;
        const int64_t era = (y >= 0 ? y : y - 399) / 400;
        const int64_t yoe = y - era * 400;
        const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + l.tm_mday - 1;
        const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        const int64_t days = era * 146097 + doe - 719468;
        return days * 86400 + l.tm_hour * 3600 + l.tm_min * 60 + l.tm_sec;
    }
    uint32_t g_desyncs = 0;
    std::thread g_thread;

    // Save transfer, guarded by g_mutex. Host: g_saveFiles is the snapshot taken before the game starts.
    // Joiner: the received copy goes to <exe>/netplay_mc/mc0 and mc1, the player's own cards stay untouched.
    std::vector<SaveFile> g_saveFiles;
    bool g_saveSnapshotReady = false;
    bool g_saveDone = false;   // joiner: the whole copy arrived and checks out
    bool g_saveBad = false;    // joiner: the copy failed to write or its hash differs
    uint64_t g_saveTotal = 0;  // joiner: bytes announced / received, for the overlay
    uint64_t g_saveGot = 0;

    // A short message over the picture once the game is running (guarded by g_mutex).
    std::string g_notice;
    std::chrono::steady_clock::time_point g_noticeUntil;
    bool g_versionWarned = false;

    void noticeLocked(std::string text, int seconds)
    {
        g_notice = std::move(text);
        g_noticeUntil = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    }

    uint64_t fnv(uint64_t h, const void *data, size_t size)
    {
        const uint8_t *p = static_cast<const uint8_t *>(data);
        for (size_t i = 0; i < size; ++i)
            h = (h ^ p[i]) * 0x100000001b3ull;
        return h;
    }

    std::string exeDir()
    {
#ifdef _WIN32
        wchar_t buf[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        return std::filesystem::path(buf).parent_path().string();
#else
        std::error_code ec;
        return std::filesystem::read_symlink("/proc/self/exe", ec).parent_path().string();
#endif
    }

    // Credentials from the Epic Developer Portal: PS2X_EOS_* env vars, else eos.ini next to the exe.
    std::string config(const char *envName, const char *iniKey)
    {
        if (const char *e = std::getenv(envName); e && *e)
            return e;
        std::ifstream in(std::filesystem::path(exeDir()) / "eos.ini");
        std::string line;
        while (std::getline(in, line))
        {
            const size_t eq = line.find('=');
            if (eq == std::string::npos || line[0] == '#' || line[0] == ';')
                continue;
            auto trim = [](std::string s) {
                const size_t a = s.find_first_not_of(" \t\r\"");
                const size_t b = s.find_last_not_of(" \t\r\"");
                return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
            };
            if (trim(line.substr(0, eq)) == iniKey)
                return trim(line.substr(eq + 1));
        }
        return {};
    }

    void fail(const char *what, EOS_EResult r)
    {
        std::fprintf(stderr, "[netplay] %s failed: %s. Netplay is off, the game runs on its own.\n", what, EOS_EResult_ToString(r));
        std::lock_guard<std::mutex> lock(g_mutex);
        g_failed = true;
        g_cv.notify_all();
    }

    void dropPeer(const char *why)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_peerGone)
            std::fprintf(stderr, "[netplay] the other player left (%s); player %d now stands still.\n", why, g_isHost ? 2 : 1);
        g_peerGone = true;
        g_cv.notify_all();
    }

    void compareHashLocked(uint32_t slot)
    {
        const FrameHash &a = g_localHash[slot];
        const FrameHash &b = g_remoteHash[slot];
        if (a.frame < 0 || a.frame != b.frame)
            return;
        if (a.hash != b.hash && g_desyncs++ < 8)
            std::fprintf(stderr, "[netplay] DESYNC at frame %lld: the two games' memory differs (%016llx vs %016llx).\n",
                         static_cast<long long>(a.frame), static_cast<unsigned long long>(a.hash),
                         static_cast<unsigned long long>(b.hash));
        g_localHash[slot].frame = -1;
    }

    // ---- EOS thread ----
    EOS_HPlatform g_platform = nullptr;
    EOS_HConnect g_connect = nullptr;
    EOS_HP2P g_p2p = nullptr;
    EOS_ProductUserId g_localId = nullptr;
    EOS_ProductUserId g_remoteId = nullptr;
    EOS_P2P_SocketId g_socket{};
    bool g_established = false;
    int g_step = 0; // async call results: 0 pending, 1 done
    EOS_EResult g_stepResult = EOS_EResult::EOS_Success;
    EOS_ContinuanceToken g_continuance = nullptr;
    EOS_HLobby g_lobbies = nullptr;
    std::string g_lobbyId;

    std::string idString(EOS_ProductUserId id)
    {
        char buf[EOS_PRODUCTUSERID_MAX_LENGTH + 1] = {};
        int32_t len = sizeof(buf);
        if (!id || EOS_ProductUserId_ToString(id, buf, &len) != EOS_EResult::EOS_Success)
            return "?";
        return buf;
    }

    void EOS_CALL onLog(const EOS_LogMessage *msg)
    {
        std::fprintf(stderr, "[netplay] EOS %s: %s\n", msg->Category, msg->Message);
    }

    bool tickUntil(const int &flag)
    {
        while (!flag && !g_stopping.load())
        {
            EOS_Platform_Tick(g_platform);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return flag != 0;
    }

    void EOS_CALL onDeviceId(const EOS_Connect_CreateDeviceIdCallbackInfo *info)
    {
        g_stepResult = info->ResultCode;
        g_step = 1;
    }

    void EOS_CALL onLogin(const EOS_Connect_LoginCallbackInfo *info)
    {
        g_stepResult = info->ResultCode;
        g_localId = info->LocalUserId;
        g_continuance = info->ContinuanceToken;
        g_step = 1;
    }

    void EOS_CALL onCreateUser(const EOS_Connect_CreateUserCallbackInfo *info)
    {
        g_stepResult = info->ResultCode;
        g_localId = info->LocalUserId;
        g_step = 1;
    }

    void EOS_CALL onConnectionRequest(const EOS_P2P_OnIncomingConnectionRequestInfo *info)
    {
        const std::string from = idString(info->RemoteUserId);
        if (g_remoteId && idString(g_remoteId) != from)
        {
            std::fprintf(stderr, "[netplay] ignored a connection from %s (already playing with %s).\n", from.c_str(),
                         idString(g_remoteId).c_str());
            return;
        }
        g_remoteId = info->RemoteUserId;
        EOS_P2P_AcceptConnectionOptions o{};
        o.ApiVersion = EOS_P2P_ACCEPTCONNECTION_API_LATEST;
        o.LocalUserId = g_localId;
        o.RemoteUserId = info->RemoteUserId;
        o.SocketId = &g_socket;
        const EOS_EResult r = EOS_P2P_AcceptConnection(g_p2p, &o);
        std::fprintf(stderr, "[netplay] connection request from %s: %s\n", from.c_str(), EOS_EResult_ToString(r));
    }

    void EOS_CALL onEstablished(const EOS_P2P_OnPeerConnectionEstablishedInfo *info)
    {
        g_established = true;
        std::fprintf(stderr, "[netplay] connected to %s (%s).\n", idString(info->RemoteUserId).c_str(),
                     info->NetworkType == EOS_ENetworkConnectionType::EOS_NCT_DirectConnection ? "direct" : "relayed");
        Packet p{};
        p.magic = kMagic;
        p.type = kHello;
        p.frame = g_delay;
        p.hash = kProtocol;
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_isHost)
        {
            Packet c{};
            c.magic = kMagic;
            c.type = kClock;
            c.hash = static_cast<uint64_t>(g_clockEpoch.load());
            g_outbox.push_front(c);
        }
        g_outbox.push_front(p);
    }

    void EOS_CALL onClosed(const EOS_P2P_OnRemoteConnectionClosedInfo *info)
    {
        std::fprintf(stderr, "[netplay] connection closed (reason %d).\n", static_cast<int>(info->Reason));
        if (g_established)
            dropPeer("connection closed");
        g_established = false;
    }

    void send(const Packet &p)
    {
        EOS_P2P_SendPacketOptions o{};
        o.ApiVersion = EOS_P2P_SENDPACKET_API_LATEST;
        o.LocalUserId = g_localId;
        o.RemoteUserId = g_remoteId;
        o.SocketId = &g_socket;
        o.Channel = 0;
        o.DataLengthBytes = sizeof(p);
        o.Data = &p;
        o.bAllowDelayedDelivery = EOS_TRUE;
        o.Reliability = EOS_EPacketReliability::EOS_PR_ReliableOrdered;
        o.bDisableAutoAcceptConnection = EOS_FALSE;
        const EOS_EResult r = EOS_P2P_SendPacket(g_p2p, &o);
        if (r != EOS_EResult::EOS_Success)
            std::fprintf(stderr, "[netplay] send failed: %s\n", EOS_EResult_ToString(r));
    }

    // Called under g_mutex. Unreliable on purpose: each packet repeats the last kInputSpan frames,
    // so a dropped one never holds the game waiting for a resend.
    void sendInputs()
    {
        if (g_localTop < 0)
            return;
        InputsPacket p{};
        p.magic = kMagic;
        p.type = kInputs;
        p.last = static_cast<uint32_t>(g_localTop);
        uint32_t n = 0;
        for (; n < kInputSpan && g_localTop - static_cast<int64_t>(n) >= 0; ++n)
        {
            const FrameInput &in = g_local[(g_localTop - n) % kRing];
            if (in.frame != g_localTop - static_cast<int64_t>(n))
                break;
            p.in[n].buttons = in.buttons;
            std::memcpy(p.in[n].axes, in.axes, 4);
        }
        if (!n)
            return;
        p.count = static_cast<uint8_t>(n);
        EOS_P2P_SendPacketOptions o{};
        o.ApiVersion = EOS_P2P_SENDPACKET_API_LATEST;
        o.LocalUserId = g_localId;
        o.RemoteUserId = g_remoteId;
        o.SocketId = &g_socket;
        o.Channel = kInputChannel;
        o.DataLengthBytes = sizeof(p);
        o.Data = &p;
        o.bAllowDelayedDelivery = EOS_TRUE;
        o.Reliability = EOS_EPacketReliability::EOS_PR_UnreliableUnordered;
        o.bDisableAutoAcceptConnection = EOS_FALSE;
        const EOS_EResult r = EOS_P2P_SendPacket(g_p2p, &o);
        if (r != EOS_EResult::EOS_Success && r != EOS_EResult::EOS_LimitExceeded)
            std::fprintf(stderr, "[netplay] input send failed: %s\n", EOS_EResult_ToString(r));
    }

    void receiveInputs(const InputsPacket &p)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const uint32_t n = p.count < kInputSpan ? p.count : kInputSpan;
        for (uint32_t i = 0; i < n && i <= p.last; ++i)
        {
            const int64_t frame = static_cast<int64_t>(p.last) - i;
            FrameInput &in = g_remote[frame % kRing];
            if (in.frame >= frame)
                continue;
            in.frame = frame;
            in.buttons = p.in[i].buttons;
            std::memcpy(in.axes, p.in[i].axes, 4);
        }
        g_cv.notify_all();
    }

    // False when EOS's send queue is full (try the same packet again next tick).
    bool sendSave(uint8_t type, uint32_t a, uint32_t b, const void *payload = nullptr, uint32_t size = 0)
    {
        uint8_t buf[sizeof(SaveHeader) + kSaveChunkBytes];
        const SaveHeader h{kMagic, type, a, b};
        std::memcpy(buf, &h, sizeof(h));
        if (size)
            std::memcpy(buf + sizeof(h), payload, size);
        EOS_P2P_SendPacketOptions o{};
        o.ApiVersion = EOS_P2P_SENDPACKET_API_LATEST;
        o.LocalUserId = g_localId;
        o.RemoteUserId = g_remoteId;
        o.SocketId = &g_socket;
        o.Channel = kSaveChannel;
        o.DataLengthBytes = static_cast<uint32_t>(sizeof(h) + size);
        o.Data = buf;
        o.bAllowDelayedDelivery = EOS_TRUE;
        o.Reliability = EOS_EPacketReliability::EOS_PR_ReliableOrdered;
        o.bDisableAutoAcceptConnection = EOS_FALSE;
        const EOS_EResult r = EOS_P2P_SendPacket(g_p2p, &o);
        if (r != EOS_EResult::EOS_Success && r != EOS_EResult::EOS_LimitExceeded)
            std::fprintf(stderr, "[netplay] save send failed: %s\n", EOS_EResult_ToString(r));
        return r == EOS_EResult::EOS_Success;
    }

    // Host side, run every tick once connected: feeds the snapshot out a few dozen packets at a time.
    struct SaveSender
    {
        size_t file = 0;
        size_t offset = 0;
        bool begun = false, fileHeader = false, done = false;
        uint64_t hash = 0xcbf29ce484222325ull;

        void pump()
        {
            if (done)
                return;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                if (!g_saveSnapshotReady)
                    return;
            }
            const std::vector<SaveFile> &files = g_saveFiles; // fixed once the snapshot is ready
            if (!begun)
            {
                uint64_t total = 0;
                for (const SaveFile &f : files)
                    total += f.data.size();
                if (!sendSave(kSaveBegin, static_cast<uint32_t>(files.size()), static_cast<uint32_t>(total)))
                    return;
                std::fprintf(stderr, "[netplay] sending the save to player 2 (%zu files, %llu KB)...\n", files.size(),
                             static_cast<unsigned long long>(total / 1024));
                begun = true;
            }
            for (int budget = 32; budget > 0 && file < files.size(); --budget)
            {
                const SaveFile &f = files[file];
                if (!fileHeader)
                {
                    if (!sendSave(kSaveFile, static_cast<uint32_t>(f.data.size()), 0, f.name.data(),
                                  static_cast<uint32_t>(f.name.size())))
                        return;
                    hash = fnv(hash, f.name.data(), f.name.size());
                    fileHeader = true;
                    offset = 0;
                    continue;
                }
                if (offset < f.data.size())
                {
                    const uint32_t n = static_cast<uint32_t>(std::min<size_t>(kSaveChunkBytes, f.data.size() - offset));
                    if (!sendSave(kSaveChunk, 0, 0, f.data.data() + offset, n))
                        return;
                    hash = fnv(hash, f.data.data() + offset, n);
                    offset += n;
                }
                if (offset >= f.data.size())
                {
                    ++file;
                    fileHeader = false;
                }
            }
            if (file < files.size())
                return;
            if (sendSave(kSaveEnd, 0, 0, &hash, sizeof(hash)))
            {
                std::fprintf(stderr, "[netplay] save sent.\n");
                done = true;
            }
        }
    };

    // Joiner side: writes what arrives into <exe>/netplay_mc.
    struct SaveReceiver
    {
        std::filesystem::path root;
        std::ofstream out;
        uint64_t remaining = 0;
        uint64_t hash = 0xcbf29ce484222325ull;
        bool bad = false;

        void finish(bool ok)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_saveDone = ok;
            g_saveBad = !ok;
            g_cv.notify_all();
        }

        void receive(const uint8_t *buf, uint32_t bytes)
        {
            if (g_isHost || bytes < sizeof(SaveHeader))
                return;
            SaveHeader h;
            std::memcpy(&h, buf, sizeof(h));
            if (h.magic != kMagic)
                return;
            const uint8_t *payload = buf + sizeof(h);
            const uint32_t size = bytes - static_cast<uint32_t>(sizeof(h));
            if (h.type == kSaveBegin)
            {
                root = std::filesystem::path(exeDir()) / "netplay_mc";
                std::error_code ec;
                std::filesystem::remove_all(root, ec);
                std::filesystem::create_directories(root / "mc0", ec);
                std::filesystem::create_directories(root / "mc1", ec);
                std::fprintf(stderr, "[netplay] receiving the host's save (%u files, %u KB)...\n", h.a, h.b / 1024);
                std::lock_guard<std::mutex> lock(g_mutex);
                g_saveTotal = h.b;
                g_saveGot = 0;
            }
            else if (h.type == kSaveFile)
            {
                out.close();
                const std::string name(reinterpret_cast<const char *>(payload), size);
                hash = fnv(hash, name.data(), name.size());
                remaining = h.a;
                // "<port>/<path>": refuse anything that would land outside netplay_mc.
                bool safe = name.size() > 2 && (name[0] == '0' || name[0] == '1') && name[1] == '/';
                const std::filesystem::path rel = std::filesystem::path(safe ? name.substr(2) : std::string());
                safe = safe && !rel.has_root_path(); // no "C:", "/" or "\\" (those would replace netplay_mc)
                for (const std::filesystem::path &part : rel)
                    safe = safe && part != "..";
                if (!safe)
                {
                    std::fprintf(stderr, "[netplay] the host sent a bad save path: %s\n", name.c_str());
                    bad = true;
                    return;
                }
                const std::filesystem::path dest = root / ("mc" + name.substr(0, 1)) / rel;
                std::error_code ec;
                std::filesystem::create_directories(dest.parent_path(), ec);
                out.open(dest, std::ios::binary | std::ios::trunc);
                if (!out)
                {
                    std::fprintf(stderr, "[netplay] can't write %s\n", dest.string().c_str());
                    bad = true;
                }
            }
            else if (h.type == kSaveChunk)
            {
                hash = fnv(hash, payload, size);
                remaining -= std::min<uint64_t>(remaining, size);
                if (out)
                    out.write(reinterpret_cast<const char *>(payload), size);
                std::lock_guard<std::mutex> lock(g_mutex);
                g_saveGot += size;
            }
            else if (h.type == kSaveEnd)
            {
                out.close();
                uint64_t theirs = 0;
                if (size >= sizeof(theirs))
                    std::memcpy(&theirs, payload, sizeof(theirs));
                const bool ok = !bad && theirs == hash;
                if (ok)
                    std::fprintf(stderr, "[netplay] got the host's save.\n");
                else
                    std::fprintf(stderr, "[netplay] the host's save didn't arrive intact; playing on with your own (the games may drift apart).\n");
                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    noticeLocked(ok ? "Using the host's save" : "The host's save didn't arrive - using your own", ok ? 6 : 20);
                }
                finish(ok);
            }
        }
    };

    void receive(const Packet &p)
    {
        if (p.magic != kMagic)
            return;
        std::lock_guard<std::mutex> lock(g_mutex);
        if (p.type == kHello)
        {
            if (p.frame != g_delay)
                std::fprintf(stderr, "[netplay] WARNING: the other player uses PS2X_NETPLAY_DELAY=%u, this one %u. Use the same value or the games drift apart.\n",
                             p.frame, g_delay);
            if (p.hash != kProtocol && !g_versionWarned)
            {
                g_versionWarned = true;
                std::fprintf(stderr, "[netplay] WARNING: the other player runs a %s S2Recomp build (protocol %llu, this one %llu). Update both, or saves won't be shared.\n",
                             p.hash < kProtocol ? "older" : "newer", static_cast<unsigned long long>(p.hash),
                             static_cast<unsigned long long>(kProtocol));
                noticeLocked(p.hash < kProtocol ? "The other player's S2Recomp is out of date - update it so saves are shared"
                                                : "Your S2Recomp is out of date - update it so saves are shared",
                             30);
            }
        }
        else if (p.type == kInput) // older builds
        {
            FrameInput &in = g_remote[p.frame % kRing];
            in.frame = p.frame;
            in.buttons = p.buttons;
            std::memcpy(in.axes, p.axes, 4);
            g_cv.notify_all();
        }
        else if (p.type == kClock)
        {
            if (!g_isHost)
            {
                g_clockEpoch.store(static_cast<int64_t>(p.hash));
                g_clockKnown.store(true);
                g_cv.notify_all();
            }
        }
        else if (p.type == kHash)
        {
            const uint32_t slot = p.frame % 64;
            g_remoteHash[slot] = {p.frame, p.hash};
            compareHashLocked(slot);
        }
    }

    bool login()
    {
        g_connect = EOS_Platform_GetConnectInterface(g_platform);

        EOS_Connect_CreateDeviceIdOptions dev{};
        dev.ApiVersion = EOS_CONNECT_CREATEDEVICEID_API_LATEST;
#ifdef _WIN32
        dev.DeviceModel = "PC Windows";
#else
        dev.DeviceModel = "PC Linux";
#endif
        g_step = 0;
        EOS_Connect_CreateDeviceId(g_connect, &dev, nullptr, onDeviceId);
        if (!tickUntil(g_step))
            return false;
        if (g_stepResult != EOS_EResult::EOS_Success && g_stepResult != EOS_EResult::EOS_DuplicateNotAllowed)
        {
            fail("EOS device ID", g_stepResult);
            return false;
        }

        const std::string name = [] { const char *e = std::getenv("PS2X_NETPLAY_NAME"); return std::string(e && *e ? e : "Player"); }();
        EOS_Connect_Credentials cred{};
        cred.ApiVersion = EOS_CONNECT_CREDENTIALS_API_LATEST;
        cred.Type = EOS_EExternalCredentialType::EOS_ECT_DEVICEID_ACCESS_TOKEN;
        cred.Token = nullptr;
        EOS_Connect_UserLoginInfo user{};
        user.ApiVersion = EOS_CONNECT_USERLOGININFO_API_LATEST;
        user.DisplayName = name.c_str();
        EOS_Connect_LoginOptions lo{};
        lo.ApiVersion = EOS_CONNECT_LOGIN_API_LATEST;
        lo.Credentials = &cred;
        lo.UserLoginInfo = &user;
        g_step = 0;
        EOS_Connect_Login(g_connect, &lo, nullptr, onLogin);
        if (!tickUntil(g_step))
            return false;
        if (g_stepResult == EOS_EResult::EOS_InvalidUser && g_continuance)
        {
            EOS_Connect_CreateUserOptions cu{};
            cu.ApiVersion = EOS_CONNECT_CREATEUSER_API_LATEST;
            cu.ContinuanceToken = g_continuance;
            g_step = 0;
            EOS_Connect_CreateUser(g_connect, &cu, nullptr, onCreateUser);
            if (!tickUntil(g_step))
                return false;
        }
        if (g_stepResult != EOS_EResult::EOS_Success || !g_localId)
        {
            fail("EOS login", g_stepResult);
            return false;
        }
        return true;
    }

    // The short join code: 8 letters/digits hashed from the host's PUID, so it stays the same on one PC.
    // The host's lobby carries it (CODE) and the joiner looks the PUID up by it.
    std::string shortCode(const std::string &puid)
    {
        uint64_t h = 1469598103934665603ull;
        for (unsigned char c : puid)
            h = (h ^ c) * 1099511628211ull;
        static const char kDigits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        std::string out(8, 'A');
        for (char &c : out)
        {
            c = kDigits[h % 36];
            h /= 36;
        }
        return out;
    }

    std::string prettyCode(const std::string &code) { return code.substr(0, 4) + "-" + code.substr(4); }

    // "eoss-dk18" or "EOSSDK18" -> "EOSSDK18"; anything that isn't 8 letters/digits comes back empty.
    std::string normalizeCode(const char *in)
    {
        std::string out;
        for (; in && *in; ++in)
        {
            const unsigned char c = static_cast<unsigned char>(*in);
            if (std::isalnum(c))
                out += static_cast<char>(std::toupper(c));
            else if (c != '-' && !std::isspace(c))
                return {};
        }
        return out.size() == 8 ? out : std::string();
    }

    void EOS_CALL onLobbyFound(const EOS_LobbySearch_FindCallbackInfo *info)
    {
        g_stepResult = info->ResultCode;
        g_step = 1;
    }

    // Finds the open lobby whose CODE matches and returns its owner (the host), or null.
    EOS_ProductUserId resolveCode(const std::string &code)
    {
        if (!g_lobbies)
            g_lobbies = EOS_Platform_GetLobbyInterface(g_platform);
        EOS_Lobby_CreateLobbySearchOptions co{};
        co.ApiVersion = EOS_LOBBY_CREATELOBBYSEARCH_API_LATEST;
        co.MaxResults = 1;
        EOS_HLobbySearch search = nullptr;
        if (EOS_Lobby_CreateLobbySearch(g_lobbies, &co, &search) != EOS_EResult::EOS_Success || !search)
            return nullptr;
        EOS_Lobby_AttributeData params[2]{};
        params[0].ApiVersion = EOS_LOBBY_ATTRIBUTEDATA_API_LATEST;
        params[0].Key = EOS_LOBBY_SEARCH_BUCKET_ID;
        params[0].Value.AsUtf8 = kLobbyBucket;
        params[0].ValueType = EOS_EAttributeType::EOS_AT_STRING;
        params[1].ApiVersion = EOS_LOBBY_ATTRIBUTEDATA_API_LATEST;
        params[1].Key = "CODE";
        params[1].Value.AsUtf8 = code.c_str();
        params[1].ValueType = EOS_EAttributeType::EOS_AT_STRING;
        for (const EOS_Lobby_AttributeData &a : params)
        {
            EOS_LobbySearch_SetParameterOptions po{};
            po.ApiVersion = EOS_LOBBYSEARCH_SETPARAMETER_API_LATEST;
            po.Parameter = &a;
            po.ComparisonOp = EOS_EComparisonOp::EOS_CO_EQUAL;
            EOS_LobbySearch_SetParameter(search, &po);
        }
        EOS_LobbySearch_FindOptions fo{};
        fo.ApiVersion = EOS_LOBBYSEARCH_FIND_API_LATEST;
        fo.LocalUserId = g_localId;
        g_step = 0;
        EOS_LobbySearch_Find(search, &fo, nullptr, onLobbyFound);
        EOS_ProductUserId owner = nullptr;
        if (tickUntil(g_step) && g_stepResult == EOS_EResult::EOS_Success)
        {
            EOS_LobbySearch_CopySearchResultByIndexOptions io{};
            io.ApiVersion = EOS_LOBBYSEARCH_COPYSEARCHRESULTBYINDEX_API_LATEST;
            io.LobbyIndex = 0;
            EOS_HLobbyDetails details = nullptr;
            if (EOS_LobbySearch_CopySearchResultByIndex(search, &io, &details) == EOS_EResult::EOS_Success && details)
            {
                EOS_LobbyDetails_GetLobbyOwnerOptions oo{};
                oo.ApiVersion = EOS_LOBBYDETAILS_GETLOBBYOWNER_API_LATEST;
                owner = EOS_LobbyDetails_GetLobbyOwner(details, &oo);
                EOS_LobbyDetails_Release(details);
            }
        }
        EOS_LobbySearch_Release(search);
        return owner;
    }

    void EOS_CALL onLobbyCreated(const EOS_Lobby_CreateLobbyCallbackInfo *info)
    {
        g_stepResult = info->ResultCode;
        if (info->ResultCode == EOS_EResult::EOS_Success && info->LobbyId)
            g_lobbyId = info->LobbyId;
        g_step = 1;
    }

    void EOS_CALL onLobbyStep(const EOS_Lobby_UpdateLobbyCallbackInfo *info)
    {
        g_stepResult = info->ResultCode;
        g_step = 1;
    }

    void EOS_CALL onLobbyDestroyed(const EOS_Lobby_DestroyLobbyCallbackInfo *info)
    {
        g_stepResult = info->ResultCode;
        g_step = 1;
    }

    // A two-player lobby holding the host's short join code, name and input delay. PS2X_NETPLAY_PRIVATE keeps
    // it off the launcher's LAN list (LIST=0) but the code still finds it. Without a lobby (Lobbies not
    // allowed in the client policy on the Dev Portal) only the full PUID works as a join code.
    bool advertise(const std::string &code)
    {
        const char *priv = std::getenv("PS2X_NETPLAY_PRIVATE");
        const bool listed = !(priv && *priv && *priv != '0');
        g_lobbies = EOS_Platform_GetLobbyInterface(g_platform);
        EOS_Lobby_CreateLobbyOptions co{};
        co.ApiVersion = EOS_LOBBY_CREATELOBBY_API_LATEST;
        co.LocalUserId = g_localId;
        co.MaxLobbyMembers = 2;
        co.PermissionLevel = EOS_ELobbyPermissionLevel::EOS_LPL_PUBLICADVERTISED;
        co.bPresenceEnabled = EOS_FALSE;
        co.bAllowInvites = EOS_FALSE;
        co.BucketId = kLobbyBucket;
        co.bDisableHostMigration = EOS_TRUE;
        co.bEnableRTCRoom = EOS_FALSE;
        co.bEnableJoinById = EOS_FALSE;
        g_step = 0;
        EOS_Lobby_CreateLobby(g_lobbies, &co, nullptr, onLobbyCreated);
        if (!tickUntil(g_step))
            return false;
        if (g_stepResult != EOS_EResult::EOS_Success || g_lobbyId.empty())
        {
            std::fprintf(stderr, "[netplay] could not open a lobby (%s), so the short join code won't work. "
                                 "Allow Lobbies in the client policy on the Epic Dev Portal.\n",
                         EOS_EResult_ToString(g_stepResult));
            return false;
        }

        const std::string name = [] { const char *e = std::getenv("PS2X_NETPLAY_NAME"); return std::string(e && *e ? e : "Host"); }();
        EOS_Lobby_UpdateLobbyModificationOptions mo{};
        mo.ApiVersion = EOS_LOBBY_UPDATELOBBYMODIFICATION_API_LATEST;
        mo.LocalUserId = g_localId;
        mo.LobbyId = g_lobbyId.c_str();
        EOS_HLobbyModification mod = nullptr;
        if (EOS_Lobby_UpdateLobbyModification(g_lobbies, &mo, &mod) != EOS_EResult::EOS_Success || !mod)
            return false;
        const std::string lan = ps2lan::format(ps2lan::localNets());
        EOS_Lobby_AttributeData attrs[5]{};
        attrs[0].ApiVersion = EOS_LOBBY_ATTRIBUTEDATA_API_LATEST;
        attrs[0].Key = "NAME";
        attrs[0].Value.AsUtf8 = name.c_str();
        attrs[0].ValueType = EOS_EAttributeType::EOS_AT_STRING;
        attrs[1].ApiVersion = EOS_LOBBY_ATTRIBUTEDATA_API_LATEST;
        attrs[1].Key = "DELAY";
        attrs[1].Value.AsInt64 = g_delay;
        attrs[1].ValueType = EOS_EAttributeType::EOS_AT_INT64;
        attrs[2].ApiVersion = EOS_LOBBY_ATTRIBUTEDATA_API_LATEST;
        attrs[2].Key = "CODE";
        attrs[2].Value.AsUtf8 = code.c_str();
        attrs[2].ValueType = EOS_EAttributeType::EOS_AT_STRING;
        attrs[3].ApiVersion = EOS_LOBBY_ATTRIBUTEDATA_API_LATEST;
        attrs[3].Key = "LIST";
        attrs[3].Value.AsInt64 = listed ? 1 : 0;
        attrs[3].ValueType = EOS_EAttributeType::EOS_AT_INT64;
        attrs[4].ApiVersion = EOS_LOBBY_ATTRIBUTEDATA_API_LATEST;
        attrs[4].Key = "LAN";
        attrs[4].Value.AsUtf8 = lan.c_str();
        attrs[4].ValueType = EOS_EAttributeType::EOS_AT_STRING;
        for (const EOS_Lobby_AttributeData &a : attrs)
        {
            EOS_LobbyModification_AddAttributeOptions ao{};
            ao.ApiVersion = EOS_LOBBYMODIFICATION_ADDATTRIBUTE_API_LATEST;
            ao.Attribute = &a;
            ao.Visibility = EOS_ELobbyAttributeVisibility::EOS_LAT_PUBLIC;
            EOS_LobbyModification_AddAttribute(mod, &ao);
        }
        EOS_Lobby_UpdateLobbyOptions uo{};
        uo.ApiVersion = EOS_LOBBY_UPDATELOBBY_API_LATEST;
        uo.LobbyModificationHandle = mod;
        g_step = 0;
        EOS_Lobby_UpdateLobby(g_lobbies, &uo, nullptr, onLobbyStep);
        tickUntil(g_step);
        EOS_LobbyModification_Release(mod);
        if (g_stepResult != EOS_EResult::EOS_Success)
        {
            std::fprintf(stderr, "[netplay] could not set the lobby's join code (%s).\n", EOS_EResult_ToString(g_stepResult));
            return false;
        }
        if (listed)
            std::fprintf(stderr, "[netplay] listed on LAN as \"%s\" (%s).\n", name.c_str(), lan.empty() ? "no private network found" : lan.c_str());
        return true;
    }

    // Takes the lobby down once player 2 is in (or the game closes), so it leaves the Lobbies list.
    void unadvertise()
    {
        if (g_lobbyId.empty())
            return;
        EOS_Lobby_DestroyLobbyOptions d{};
        d.ApiVersion = EOS_LOBBY_DESTROYLOBBY_API_LATEST;
        d.LocalUserId = g_localId;
        d.LobbyId = g_lobbyId.c_str();
        g_step = 0;
        EOS_Lobby_DestroyLobby(g_lobbies, &d, nullptr, onLobbyDestroyed);
        g_lobbyId.clear();
        for (int i = 0; i < 200 && !g_step; ++i) // also runs while stopping, so not tickUntil
        {
            EOS_Platform_Tick(g_platform);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    void netThread()
    {
        EOS_InitializeOptions init{};
        init.ApiVersion = EOS_INITIALIZE_API_LATEST;
        init.ProductName = "Sims2Recomp";
        init.ProductVersion = "1.0";
        EOS_EResult r = EOS_Initialize(&init);
        if (r != EOS_EResult::EOS_Success && r != EOS_EResult::EOS_AlreadyConfigured)
        {
            fail("EOS_Initialize", r);
            return;
        }
        EOS_Logging_SetCallback(onLog);
        EOS_Logging_SetLogLevel(EOS_ELogCategory::EOS_LC_ALL_CATEGORIES, EOS_ELogLevel::EOS_LOG_Warning);

        const std::string productId = config("PS2X_EOS_PRODUCT_ID", "ProductId");
        const std::string sandboxId = config("PS2X_EOS_SANDBOX_ID", "SandboxId");
        const std::string deploymentId = config("PS2X_EOS_DEPLOYMENT_ID", "DeploymentId");
        const std::string clientId = config("PS2X_EOS_CLIENT_ID", "ClientId");
        const std::string clientSecret = config("PS2X_EOS_CLIENT_SECRET", "ClientSecret");
        if (productId.empty() || sandboxId.empty() || deploymentId.empty() || clientId.empty() || clientSecret.empty())
        {
            std::fprintf(stderr, "[netplay] missing EOS credentials: put ProductId, SandboxId, DeploymentId, ClientId and ClientSecret in eos.ini next to the exe.\n");
            fail("EOS setup", EOS_EResult::EOS_InvalidParameters);
            EOS_Shutdown();
            return;
        }
        std::error_code ec;
        const std::string cacheDir = (std::filesystem::path(exeDir()) / "eos_cache").string();
        std::filesystem::create_directories(cacheDir, ec);

        EOS_Platform_Options po{};
        po.ApiVersion = EOS_PLATFORM_OPTIONS_API_LATEST;
        po.ProductId = productId.c_str();
        po.SandboxId = sandboxId.c_str();
        po.DeploymentId = deploymentId.c_str();
        po.ClientCredentials.ClientId = clientId.c_str();
        po.ClientCredentials.ClientSecret = clientSecret.c_str();
        po.Flags = EOS_PF_DISABLE_OVERLAY;
        po.CacheDirectory = cacheDir.c_str();
        po.RTCOptions = nullptr;
        g_platform = EOS_Platform_Create(&po);
        if (!g_platform)
        {
            fail("EOS_Platform_Create (check the IDs in eos.ini)", EOS_EResult::EOS_InvalidParameters);
            EOS_Shutdown();
            return;
        }

        if (login())
        {
            g_p2p = EOS_Platform_GetP2PInterface(g_platform);
            g_socket.ApiVersion = EOS_P2P_SOCKETID_API_LATEST;
            std::strncpy(g_socket.SocketName, kSocketName, sizeof(g_socket.SocketName) - 1);

            EOS_P2P_AddNotifyPeerConnectionRequestOptions rq{};
            rq.ApiVersion = EOS_P2P_ADDNOTIFYPEERCONNECTIONREQUEST_API_LATEST;
            rq.LocalUserId = g_localId;
            rq.SocketId = &g_socket;
            const EOS_NotificationId nReq = EOS_P2P_AddNotifyPeerConnectionRequest(g_p2p, &rq, nullptr, onConnectionRequest);
            EOS_P2P_AddNotifyPeerConnectionEstablishedOptions es{};
            es.ApiVersion = EOS_P2P_ADDNOTIFYPEERCONNECTIONESTABLISHED_API_LATEST;
            es.LocalUserId = g_localId;
            es.SocketId = &g_socket;
            const EOS_NotificationId nEst = EOS_P2P_AddNotifyPeerConnectionEstablished(g_p2p, &es, nullptr, onEstablished);
            EOS_P2P_AddNotifyPeerConnectionClosedOptions cl{};
            cl.ApiVersion = EOS_P2P_ADDNOTIFYPEERCONNECTIONCLOSED_API_LATEST;
            cl.LocalUserId = g_localId;
            cl.SocketId = &g_socket;
            const EOS_NotificationId nCl = EOS_P2P_AddNotifyPeerConnectionClosed(g_p2p, &cl, nullptr, onClosed);

            const std::string myId = idString(g_localId);
            if (g_isHost)
            {
                const std::string code = advertise(shortCode(myId)) ? prettyCode(shortCode(myId)) : myId;
                std::fprintf(stderr, "[netplay] ==== JOIN CODE: %s ==== (also in netplay_code.txt) send it to your friend.\n", code.c_str());
                std::ofstream(std::filesystem::path(exeDir()) / "netplay_code.txt") << code << "\n";
                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    g_joinCode = code;
                }
            }
            else
            {
                const char *code = std::getenv("PS2X_NETPLAY_CODE");
                const std::string shortForm = normalizeCode(code);
                if (!shortForm.empty())
                {
                    // The host may still be starting, so look for its lobby for up to a minute.
                    std::fprintf(stderr, "[netplay] looking for host %s...\n", prettyCode(shortForm).c_str());
                    for (int tries = 0; tries < 20 && !g_remoteId && !g_stopping.load(); ++tries)
                    {
                        if (tries)
                            for (int i = 0; i < 600 && !g_stopping.load(); ++i)
                            {
                                EOS_Platform_Tick(g_platform);
                                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                            }
                        g_remoteId = resolveCode(shortForm);
                    }
                }
                else
                {
                    // The full product user ID: lowercase hex (the launcher's code box types in caps).
                    std::string id;
                    for (const char *c = code; c && *c; ++c)
                        if (std::isalnum(static_cast<unsigned char>(*c)))
                            id += static_cast<char>(std::tolower(static_cast<unsigned char>(*c)));
                    g_remoteId = id.empty() ? nullptr : EOS_ProductUserId_FromString(id.c_str());
                }
                if (!g_remoteId || !EOS_ProductUserId_IsValid(g_remoteId))
                {
                    std::fprintf(stderr, shortForm.empty() ? "[netplay] PS2X_NETPLAY_CODE is missing or not a valid join code.\n"
                                                           : "[netplay] no one is hosting with that code.\n");
                    fail("join", EOS_EResult::EOS_InvalidParameters);
                }
                else
                    std::fprintf(stderr, "[netplay] logged in as %s, joining %s...\n", myId.c_str(), code);
            }

            auto lastHello = std::chrono::steady_clock::now() - std::chrono::seconds(10);
            auto lastInputs = std::chrono::steady_clock::now();
            uint8_t buf[EOS_P2P_MAX_PACKET_SIZE];
            SaveSender saveOut;
            SaveReceiver saveIn;
            while (!g_stopping.load() && !g_failed)
            {
                EOS_Platform_Tick(g_platform);
                if (g_established && !g_lobbyId.empty())
                    unadvertise();
                // The joiner knocks until the host accepts (the first send opens the connection).
                if (!g_isHost && !g_established && std::chrono::steady_clock::now() - lastHello > std::chrono::seconds(3))
                {
                    Packet p{};
                    p.magic = kMagic;
                    p.type = kHello;
                    p.frame = g_delay;
                    p.hash = kProtocol;
                    send(p);
                    lastHello = std::chrono::steady_clock::now();
                }
                if (g_remoteId && g_established)
                {
                    std::deque<Packet> out;
                    {
                        std::lock_guard<std::mutex> lock(g_mutex);
                        out.swap(g_outbox);
                        // New input goes out at once; otherwise repeat the latest every 15 ms in case it was lost.
                        const auto now = std::chrono::steady_clock::now();
                        if (g_localNew || now - lastInputs > std::chrono::milliseconds(15))
                        {
                            sendInputs();
                            g_localNew = false;
                            lastInputs = now;
                        }
                    }
                    for (const Packet &p : out)
                        send(p);
                    if (g_isHost)
                        saveOut.pump();
                }
                for (;;)
                {
                    EOS_P2P_ReceivePacketOptions ro{};
                    ro.ApiVersion = EOS_P2P_RECEIVEPACKET_API_LATEST;
                    ro.LocalUserId = g_localId;
                    ro.MaxDataSizeBytes = sizeof(buf);
                    ro.RequestedChannel = nullptr;
                    EOS_ProductUserId peer = nullptr;
                    EOS_P2P_SocketId sock{};
                    uint8_t channel = 0;
                    uint32_t bytes = 0;
                    if (EOS_P2P_ReceivePacket(g_p2p, &ro, &peer, &sock, &channel, buf, &bytes) != EOS_EResult::EOS_Success)
                        break;
                    if (channel == kSaveChannel)
                        saveIn.receive(buf, bytes);
                    else if (channel == kInputChannel)
                    {
                        InputsPacket p;
                        if (bytes == sizeof(p))
                        {
                            std::memcpy(&p, buf, sizeof(p));
                            if (p.magic == kMagic && p.type == kInputs)
                                receiveInputs(p);
                        }
                    }
                    else if (bytes == sizeof(Packet))
                    {
                        Packet p;
                        std::memcpy(&p, buf, sizeof(p));
                        receive(p);
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            unadvertise();
            EOS_P2P_CloseConnectionsOptions cc{};
            cc.ApiVersion = EOS_P2P_CLOSECONNECTIONS_API_LATEST;
            cc.LocalUserId = g_localId;
            cc.SocketId = &g_socket;
            EOS_P2P_CloseConnections(g_p2p, &cc);
            EOS_P2P_RemoveNotifyPeerConnectionRequest(g_p2p, nReq);
            EOS_P2P_RemoveNotifyPeerConnectionEstablished(g_p2p, nEst);
            EOS_P2P_RemoveNotifyPeerConnectionClosed(g_p2p, nCl);
            for (int i = 0; i < 20; ++i) // let the close go out
            {
                EOS_Platform_Tick(g_platform);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        EOS_Platform_Release(g_platform);
        g_platform = nullptr;
        EOS_Shutdown();
    }

    void queue(const Packet &p)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_outbox.push_back(p);
    }
}

bool ps2NetplayActive()
{
    static const bool active = [] {
        if (!netplayMode())
            return false;
        if (g_isHost)
        {
            g_clockEpoch.store(localEpochNow());
            g_clockKnown.store(true);
        }
        std::fprintf(stderr, "[netplay] %s, input delay %u frames%s.\n", g_isHost ? "hosting (player 1)" : "joining (player 2)",
                     g_delay, g_hashEvery ? ", desync check on" : "");
        g_thread = std::thread(netThread);
        return true;
    }();
    return active;
}

int ps2NetplayLocalPlayer()
{
    return ps2NetplayActive() ? (g_isHost ? 0 : 1) : -1;
}

void ps2NetplayShareSave()
{
    if (!ps2NetplayActive())
        return;
    if (g_isHost)
    {
        // Snapshot the cards now, before the game can touch them; the EOS thread sends it once player 2 connects.
        std::vector<SaveFile> files;
        for (int32_t port = 0; port < 2; ++port)
        {
            const std::filesystem::path card = ps2_stubs::memoryCardFolder(port);
            std::error_code ec;
            for (auto it = std::filesystem::recursive_directory_iterator(card, ec);
                 !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
            {
                if (!it->is_regular_file(ec))
                    continue;
                SaveFile f;
                f.name = std::to_string(port) + "/" + it->path().lexically_relative(card).generic_string();
                std::ifstream in(it->path(), std::ios::binary);
                f.data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
                files.push_back(std::move(f));
            }
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        g_saveFiles = std::move(files);
        g_saveSnapshotReady = true;
        return;
    }

    // Joiner: hold the game until the host's save is here, then boot from that copy.
    std::unique_lock<std::mutex> lock(g_mutex);
    g_waiting.store(true);
    g_cv.wait(lock, [] { return g_saveDone || g_saveBad || g_failed || g_peerGone || g_stopping.load(); });
    // The clock comes right after the hello; a host build without it never sends one.
    g_cv.wait_for(lock, std::chrono::seconds(5), [] { return g_clockKnown.load() || g_failed || g_peerGone || g_stopping.load(); });
    if (!g_clockKnown.load())
        std::fprintf(stderr, "[netplay] the host sent no clock (older build?); the games may drift apart.\n");
    g_waiting.store(false);
    if (!g_saveDone)
        return;
    lock.unlock();
    const std::filesystem::path root = std::filesystem::path(exeDir()) / "netplay_mc";
    if (!ps2_stubs::setMemoryCardFolder(0, root / "mc0") || !ps2_stubs::setMemoryCardFolder(1, root / "mc1"))
        std::fprintf(stderr, "[netplay] couldn't switch to the host's save; playing on with your own.\n");
}

extern std::atomic<int64_t> g_ps2xNetplayWaitNs; // EeScheduler.cpp
extern std::atomic<int64_t> g_ps2xVblDroppedNs;  // EeScheduler.cpp

// Every 10 s (PS2X_NETPLAY_STATS=0 turns it off): how often the game exchanged input and how long it waited for the peer.
static void netplayStats(std::chrono::steady_clock::time_point now, int64_t waitedNs)
{
    static const bool on = [] { const char *e = std::getenv("PS2X_NETPLAY_STATS"); return !(e && *e == '0'); }();
    if (!on)
        return;
    static auto start = now;
    static int64_t count = 0, waits = 0, totalNs = 0, maxNs = 0, dropped0 = 0;
    ++count;
    if (waitedNs > 2000000)
        ++waits;
    totalNs += waitedNs;
    if (waitedNs > maxNs)
        maxNs = waitedNs;
    const double secs = std::chrono::duration<double>(now - start).count();
    if (secs < 10.0)
        return;
    const int64_t dropped = g_ps2xVblDroppedNs.load(std::memory_order_relaxed);
    std::fprintf(stderr, "[netplay] stats: %.1f exchanges/s, %lld waits >2 ms, waited %.0f ms (max %.1f ms), VBlank time dropped %.0f ms\n",
                 count / secs, static_cast<long long>(waits), totalNs / 1e6, maxNs / 1e6, (dropped - dropped0) / 1e6);
    start = now;
    count = waits = totalNs = maxNs = 0;
    dropped0 = dropped;
}

void ps2NetplayExchange(uint16_t localButtons, const uint8_t localAxes[4], uint16_t outButtons[2], uint8_t outAxes[2][4])
{
    const int64_t f = g_frame++;
    const int me = g_isHost ? 0 : 1;

    const int64_t sendFrame = f + g_delay;

    std::unique_lock<std::mutex> lock(g_mutex);
    FrameInput &mine = g_local[sendFrame % kRing];
    mine.frame = sendFrame;
    mine.buttons = localButtons;
    std::memcpy(mine.axes, localAxes, 4);
    g_localTop = sendFrame; // the network thread sends it (with the frames before it) on its next tick
    g_localNew = true;

    FrameInput neutral;
    const FrameInput &local = (f >= g_delay && g_local[f % kRing].frame == f) ? g_local[f % kRing] : neutral;
    const FrameInput *remote = &neutral;
    if (f >= g_delay)
    {
        auto ready = [&] { return g_remote[f % kRing].frame == f || g_peerGone || g_failed || g_stopping.load(); };
        const auto waitStart = std::chrono::steady_clock::now();
        // Past a short hitch the window shows a waiting message (the picture stays still meanwhile).
        if (!g_cv.wait_for(lock, std::chrono::milliseconds(500), ready))
        {
            g_waiting.store(true);
            while (!g_cv.wait_for(lock, std::chrono::seconds(5), ready))
                std::fprintf(stderr, "[netplay] waiting for the other player (frame %lld)...\n", static_cast<long long>(f));
            g_waiting.store(false);
        }
        const int64_t waitedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - waitStart).count();
        // The scheduler catches this time up instead of dropping it (EeScheduler.cpp), so audio keeps pace.
        g_ps2xNetplayWaitNs.fetch_add(waitedNs, std::memory_order_relaxed);
        netplayStats(waitStart, waitedNs);
        if (g_remote[f % kRing].frame == f)
            remote = &g_remote[f % kRing];
    }
    outButtons[me] = local.buttons;
    std::memcpy(outAxes[me], local.axes, 4);
    outButtons[1 - me] = remote->buttons;
    std::memcpy(outAxes[1 - me], remote->axes, 4);
    if (g_failed && !g_peerGone)
    {
        // Login or connect failed: no lockstep, this pad drives player 1 right away.
        outButtons[0] = localButtons;
        std::memcpy(outAxes[0], localAxes, 4);
        outButtons[1] = 0xFFFFu;
        std::memset(outAxes[1], 0x80, 4);
    }
}

bool ps2NetplayClock(int64_t &frame, int64_t &localEpoch)
{
    if (!ps2NetplayActive() || !g_clockKnown.load())
        return false;
    frame = g_frame.load();
    localEpoch = g_clockEpoch.load() + frame / 60;
    return true;
}

void ps2NetplayCheckState(const uint8_t *rdram)
{
    if (!g_hashEvery || !rdram || !ps2NetplayActive())
        return;
    const int64_t f = g_frame - 1;
    if (f < 0 || f % g_hashEvery != 0)
        return;
    uint64_t h = 0xcbf29ce484222325ull;
    const uint64_t *words = reinterpret_cast<const uint64_t *>(rdram);
    for (uint32_t i = 0; i < PS2_RAM_SIZE / 8; ++i)
        h = (h ^ words[i]) * 0x100000001b3ull;

    Packet p{};
    p.magic = kMagic;
    p.type = kHash;
    p.frame = static_cast<uint32_t>(f);
    p.hash = h;
    std::lock_guard<std::mutex> lock(g_mutex);
    const uint32_t slot = p.frame % 64;
    g_localHash[slot] = {f, h};
    compareHashLocked(slot);
    g_outbox.push_back(p);
}

std::string ps2NetplayOverlayText()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_waiting.load())
        return std::chrono::steady_clock::now() < g_noticeUntil ? g_notice : std::string();
    if (!g_isHost && g_saveTotal && !g_saveDone && !g_saveBad)
        return "Receiving the host's save... " + std::to_string(g_saveGot / 1024) + " / " +
               std::to_string(g_saveTotal / 1024) + " KB";
    if (!g_isHost)
        return "Connecting to the host...";
    if (g_joinCode.empty())
        return "Co-op: signing in to Epic...";
    return "Waiting for player 2 to join\nJoin code: " + g_joinCode + "\n(also in netplay_code.txt)";
}

void ps2NetplayShutdown()
{
    if (!g_thread.joinable())
        return;
    g_stopping.store(true);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_cv.notify_all();
    }
    g_thread.join();
}

#else

bool ps2NetplayActive()
{
    static const bool warned = [] {
        const char *e = std::getenv("PS2X_NETPLAY");
        if (e && *e && *e != '0')
            std::fprintf(stderr, "[netplay] this build has no EOS SDK, PS2X_NETPLAY is ignored.\n");
        return true;
    }();
    (void)warned;
    return false;
}

void ps2NetplayExchange(uint16_t localButtons, const uint8_t localAxes[4], uint16_t outButtons[2], uint8_t outAxes[2][4])
{
    outButtons[0] = localButtons;
    std::memcpy(outAxes[0], localAxes, 4);
    outButtons[1] = 0xFFFFu;
    std::memset(outAxes[1], 0x80, 4);
}

void ps2NetplayShareSave() {}
int ps2NetplayLocalPlayer() { return -1; }
bool ps2NetplayClock(int64_t &, int64_t &) { return false; }
void ps2NetplayCheckState(const uint8_t *) {}
std::string ps2NetplayOverlayText() { return {}; }
void ps2NetplayShutdown() {}

#endif
