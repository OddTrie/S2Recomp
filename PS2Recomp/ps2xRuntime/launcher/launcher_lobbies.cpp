#include "launcher_lobbies.h"

#if PS2X_EOS
#include "../include/runtime/ps2_lan.h"
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define NOMINMAX
#include <windows.h>
#endif
#include <eos_sdk.h>
#include <eos_connect.h>
#include <eos_lobby.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <thread>

namespace
{
    // Must match ps2_netplay.cpp.
    constexpr const char *kLobbyBucket = "s2r:v1";

    std::mutex g_mutex;
    std::vector<lobbies::Entry> g_list;
    std::string g_status = "Not connected.";
    bool g_failed = false;
    std::atomic<bool> g_busy{false};
    std::atomic<bool> g_refresh{false};
    std::atomic<bool> g_stopping{false};
    std::thread g_thread;
    std::filesystem::path g_ini;

    // ---- EOS thread ----
    EOS_HPlatform g_platform = nullptr;
    EOS_HConnect g_connect = nullptr;
    EOS_HLobby g_lobby = nullptr;
    EOS_ProductUserId g_localId = nullptr;
    EOS_ContinuanceToken g_continuance = nullptr;
    int g_step = 0;
    EOS_EResult g_stepResult = EOS_EResult::EOS_Success;

    void setStatus(std::string s, bool failed = false)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_status = std::move(s);
        g_failed = failed;
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

    // Same lookup as the game: PS2X_EOS_* env vars, else the eos.ini the launcher found.
    std::string config(const char *envName, const char *iniKey)
    {
        if (const char *e = std::getenv(envName); e && *e)
            return e;
        std::ifstream in(g_ini);
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

    std::string idString(EOS_ProductUserId id)
    {
        char buf[EOS_PRODUCTUSERID_MAX_LENGTH + 1] = {};
        int32_t len = sizeof(buf);
        if (!id || EOS_ProductUserId_ToString(id, buf, &len) != EOS_EResult::EOS_Success)
            return {};
        return buf;
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

    void EOS_CALL onFind(const EOS_LobbySearch_FindCallbackInfo *info)
    {
        g_stepResult = info->ResultCode;
        g_step = 1;
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
            setStatus(std::string("Couldn't sign in to Epic (device ID: ") + EOS_EResult_ToString(g_stepResult) + ").", true);
            return false;
        }

        EOS_Connect_Credentials cred{};
        cred.ApiVersion = EOS_CONNECT_CREDENTIALS_API_LATEST;
        cred.Type = EOS_EExternalCredentialType::EOS_ECT_DEVICEID_ACCESS_TOKEN;
        cred.Token = nullptr;
        EOS_Connect_UserLoginInfo user{};
        user.ApiVersion = EOS_CONNECT_USERLOGININFO_API_LATEST;
        user.DisplayName = "Player";
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
            setStatus(std::string("Couldn't sign in to Epic (") + EOS_EResult_ToString(g_stepResult) + ").", true);
            return false;
        }
        return true;
    }

    std::string stringAttr(EOS_HLobbyDetails details, const char *key, int64_t *asInt = nullptr)
    {
        EOS_LobbyDetails_CopyAttributeByKeyOptions o{};
        o.ApiVersion = EOS_LOBBYDETAILS_COPYATTRIBUTEBYKEY_API_LATEST;
        o.AttrKey = key;
        EOS_Lobby_Attribute *attr = nullptr;
        std::string out;
        if (EOS_LobbyDetails_CopyAttributeByKey(details, &o, &attr) == EOS_EResult::EOS_Success && attr && attr->Data)
        {
            if (attr->Data->ValueType == EOS_ELobbyAttributeType::EOS_AT_STRING && attr->Data->Value.AsUtf8)
                out = attr->Data->Value.AsUtf8;
            else if (attr->Data->ValueType == EOS_ELobbyAttributeType::EOS_AT_INT64 && asInt)
                *asInt = attr->Data->Value.AsInt64;
        }
        if (attr)
            EOS_Lobby_Attribute_Release(attr);
        return out;
    }

    void search()
    {
        setStatus("Looking for games on your network...");
        const std::vector<ps2lan::Net> mine = ps2lan::localNets();
        if (mine.empty())
        {
            setStatus("This PC isn't on a home network (no 10.x, 172.16-31.x or 192.168.x address).", true);
            return;
        }
        EOS_Lobby_CreateLobbySearchOptions co{};
        co.ApiVersion = EOS_LOBBY_CREATELOBBYSEARCH_API_LATEST;
        co.MaxResults = 100;
        EOS_HLobbySearch handle = nullptr;
        EOS_EResult r = EOS_Lobby_CreateLobbySearch(g_lobby, &co, &handle);
        if (r != EOS_EResult::EOS_Success)
        {
            setStatus(std::string("Lobby search failed: ") + EOS_EResult_ToString(r) + ".", true);
            return;
        }

        EOS_Lobby_AttributeData bucket{};
        bucket.ApiVersion = EOS_LOBBY_ATTRIBUTEDATA_API_LATEST;
        bucket.Key = EOS_LOBBY_SEARCH_BUCKET_ID;
        bucket.Value.AsUtf8 = kLobbyBucket;
        bucket.ValueType = EOS_ELobbyAttributeType::EOS_AT_STRING;
        EOS_LobbySearch_SetParameterOptions po{};
        po.ApiVersion = EOS_LOBBYSEARCH_SETPARAMETER_API_LATEST;
        po.Parameter = &bucket;
        po.ComparisonOp = EOS_EComparisonOp::EOS_CO_EQUAL;
        EOS_LobbySearch_SetParameter(handle, &po);

        EOS_LobbySearch_FindOptions fo{};
        fo.ApiVersion = EOS_LOBBYSEARCH_FIND_API_LATEST;
        fo.LocalUserId = g_localId;
        g_step = 0;
        EOS_LobbySearch_Find(handle, &fo, nullptr, onFind);
        if (!tickUntil(g_step))
        {
            EOS_LobbySearch_Release(handle);
            return;
        }

        std::vector<lobbies::Entry> found;
        // NotFound just means nobody is hosting.
        if (g_stepResult == EOS_EResult::EOS_Success)
        {
            const std::string me = idString(g_localId);
            EOS_LobbySearch_GetSearchResultCountOptions no{};
            no.ApiVersion = EOS_LOBBYSEARCH_GETSEARCHRESULTCOUNT_API_LATEST;
            const uint32_t count = EOS_LobbySearch_GetSearchResultCount(handle, &no);
            for (uint32_t i = 0; i < count; i++)
            {
                EOS_LobbySearch_CopySearchResultByIndexOptions io{};
                io.ApiVersion = EOS_LOBBYSEARCH_COPYSEARCHRESULTBYINDEX_API_LATEST;
                io.LobbyIndex = i;
                EOS_HLobbyDetails details = nullptr;
                if (EOS_LobbySearch_CopySearchResultByIndex(handle, &io, &details) != EOS_EResult::EOS_Success || !details)
                    continue;
                EOS_LobbyDetails_CopyInfoOptions info{};
                info.ApiVersion = EOS_LOBBYDETAILS_COPYINFO_API_LATEST;
                EOS_LobbyDetails_Info *li = nullptr;
                if (EOS_LobbyDetails_CopyInfo(details, &info, &li) == EOS_EResult::EOS_Success && li)
                {
                    lobbies::Entry e;
                    e.owner = idString(li->LobbyOwnerUserId);
                    e.slots = static_cast<int>(li->AvailableSlots);
                    e.name = stringAttr(details, "NAME");
                    e.code = stringAttr(details, "CODE");
                    int64_t list = 1;
                    stringAttr(details, "LIST", &list);
                    int64_t delay = 3;
                    stringAttr(details, "DELAY", &delay);
                    e.delay = static_cast<int>(delay < 1 ? 1 : (delay > 10 ? 10 : delay));
                    if (e.name.empty())
                        e.name = "Player";
                    e.mine = !me.empty() && e.owner == me;
                    // LAN only: the host's private networks must overlap ours (hosts from older builds have none).
                    const bool local = e.mine || ps2lan::shareNetwork(ps2lan::parse(stringAttr(details, "LAN")), mine);
                    if (!e.owner.empty() && e.slots > 0 && list != 0 && local)
                        found.push_back(std::move(e));
                    EOS_LobbyDetails_Info_Release(li);
                }
                EOS_LobbyDetails_Release(details);
            }
        }
        else if (g_stepResult != EOS_EResult::EOS_NotFound)
        {
            EOS_LobbySearch_Release(handle);
            setStatus(std::string("Lobby search failed: ") + EOS_EResult_ToString(g_stepResult) +
                          ". Lobbies must be allowed in the client policy on the Epic Dev Portal.",
                      true);
            return;
        }
        EOS_LobbySearch_Release(handle);

        const size_t n = found.size();
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_list = std::move(found);
        }
        setStatus(n == 0 ? "No one on your network is hosting right now." : (std::to_string(n) + (n == 1 ? " game on your network." : " games on your network.")));
    }

    void eosThread()
    {
        setStatus("Signing in to Epic Online Services...");
        EOS_InitializeOptions init{};
        init.ApiVersion = EOS_INITIALIZE_API_LATEST;
        init.ProductName = "Sims2Recomp";
        init.ProductVersion = "1.0";
        const EOS_EResult r = EOS_Initialize(&init);
        if (r != EOS_EResult::EOS_Success && r != EOS_EResult::EOS_AlreadyConfigured)
        {
            setStatus(std::string("EOS failed to start: ") + EOS_EResult_ToString(r) + ".", true);
            g_busy = false;
            return;
        }

        const std::string productId = config("PS2X_EOS_PRODUCT_ID", "ProductId");
        const std::string sandboxId = config("PS2X_EOS_SANDBOX_ID", "SandboxId");
        const std::string deploymentId = config("PS2X_EOS_DEPLOYMENT_ID", "DeploymentId");
        const std::string clientId = config("PS2X_EOS_CLIENT_ID", "ClientId");
        const std::string clientSecret = config("PS2X_EOS_CLIENT_SECRET", "ClientSecret");
        if (productId.empty() || sandboxId.empty() || deploymentId.empty() || clientId.empty() || clientSecret.empty())
        {
            setStatus("No Epic Online Services keys: eos.ini is missing or incomplete.", true);
            EOS_Shutdown();
            g_busy = false;
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
        g_platform = EOS_Platform_Create(&po);
        if (!g_platform)
        {
            setStatus("EOS couldn't start: check the IDs in eos.ini.", true);
            EOS_Shutdown();
            g_busy = false;
            return;
        }

        if (login())
        {
            g_lobby = EOS_Platform_GetLobbyInterface(g_platform);
            g_refresh = true;
            while (!g_stopping.load())
            {
                if (g_refresh.exchange(false))
                {
                    g_busy = true;
                    search();
                }
                g_busy = false;
                EOS_Platform_Tick(g_platform);
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        g_busy = false;
        EOS_Platform_Release(g_platform);
        g_platform = nullptr;
        EOS_Shutdown();
    }
}

namespace lobbies
{
    bool available() { return true; }

    void start(const std::filesystem::path &ini)
    {
        if (g_thread.joinable())
            return;
        g_ini = ini;
        g_busy = true;
        g_thread = std::thread(eosThread);
    }

    void refresh() { g_refresh = true; }
    bool busy() { return g_busy.load() || g_refresh.load(); }

    std::string status()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_status;
    }

    bool failed()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_failed;
    }

    std::vector<Entry> list()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_list;
    }

    void shutdown()
    {
        g_stopping = true;
        if (g_thread.joinable())
            g_thread.join();
    }
}

#else

namespace lobbies
{
    bool available() { return false; }
    void start(const std::filesystem::path &) {}
    void refresh() {}
    bool busy() { return false; }
    std::string status() { return "This build has no online co-op (built without the EOS SDK)."; }
    bool failed() { return true; }
    std::vector<Entry> list() { return {}; }
    void shutdown() {}
}

#endif
