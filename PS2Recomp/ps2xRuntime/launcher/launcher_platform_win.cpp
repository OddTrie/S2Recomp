#include "launcher_platform.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <winhttp.h>

#include <atomic>
#include <thread>

namespace launcher
{
    namespace
    {
        std::wstring widen(const std::string &s)
        {
            if (s.empty())
                return {};
            const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
            std::wstring out(static_cast<size_t>(n), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
            return out;
        }

        std::string lastErrorText()
        {
            const DWORD code = GetLastError();
            wchar_t *buf = nullptr;
            FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                           nullptr, code, 0, reinterpret_cast<wchar_t *>(&buf), 0, nullptr);
            std::string out = "error " + std::to_string(code);
            if (buf)
            {
                const std::wstring w(buf);
                LocalFree(buf);
                const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
                std::string text(static_cast<size_t>(n), '\0');
                WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), text.data(), n, nullptr, nullptr);
                while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == '.'))
                    text.pop_back();
                out = text + " (" + out + ")";
            }
            return out;
        }
    }

    struct GameProcess
    {
        PROCESS_INFORMATION pi{};
        HANDLE pipeRead = nullptr;
        HANDLE log = INVALID_HANDLE_VALUE;
        HANDLE console = INVALID_HANDLE_VALUE;
        std::thread writer;
    };

    std::filesystem::path exeDir()
    {
        std::wstring buf(MAX_PATH, L'\0');
        for (;;)
        {
            const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
            if (n < buf.size())
            {
                buf.resize(n);
                break;
            }
            buf.resize(buf.size() * 2);
        }
        return std::filesystem::path(buf).parent_path();
    }

    // Starts `cmd` (exe and its arguments, quoted) with its output going to `logPath` and, when
    // `showConsole`, to a console window.
    static GameProcess *startProcess(const std::filesystem::path &exe, std::wstring cmd, const std::filesystem::path &cwd,
                                     const std::filesystem::path &logPath, bool showConsole, std::string &error)
    {
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE pipeRead = nullptr, pipeWrite = nullptr;
        if (!CreatePipe(&pipeRead, &pipeWrite, &sa, 1 << 16))
        {
            error = "Cannot create the output pipe: " + lastErrorText();
            return nullptr;
        }
        SetHandleInformation(pipeRead, HANDLE_FLAG_INHERIT, 0);
        HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = nul;
        si.hStdOutput = pipeWrite;
        si.hStdError = pipeWrite;

        auto *game = new GameProcess();
        const BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                       cwd.empty() ? nullptr : cwd.c_str(), &si, &game->pi);
        const std::string startError = ok ? std::string() : lastErrorText();
        CloseHandle(pipeWrite);
        if (nul != INVALID_HANDLE_VALUE)
            CloseHandle(nul);
        if (!ok)
        {
            CloseHandle(pipeRead);
            delete game;
            error = "Cannot start " + exe.filename().string() + ": " + startError;
            return nullptr;
        }

        game->pipeRead = pipeRead;
        game->log = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (showConsole && AllocConsole())
        {
            SetConsoleTitleW(L"S2Recomp - console (closing this window also closes the launcher)");
            game->console = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        }
        // Copy the game's output to run_log.txt (written through, so it survives a crash) and the console.
        game->writer = std::thread([game]
        {
            char buf[8192];
            DWORD n = 0;
            while (ReadFile(game->pipeRead, buf, sizeof(buf), &n, nullptr) && n > 0)
            {
                DWORD written = 0;
                if (game->log != INVALID_HANDLE_VALUE)
                    WriteFile(game->log, buf, n, &written, nullptr);
                if (game->console != INVALID_HANDLE_VALUE)
                    WriteFile(game->console, buf, n, &written, nullptr);
            }
        });
        return game;
    }

    GameProcess *startGame(const std::filesystem::path &exe, const std::filesystem::path &elf,
                           const std::filesystem::path &cwd, const std::vector<EnvVar> &env,
                           const std::filesystem::path &logPath, bool showConsole, std::string &error)
    {
        // The child inherits the launcher's environment, so set ours (and drop inherited values for
        // settings left on "auto") before starting it.
        for (const EnvVar &v : env)
            SetEnvironmentVariableW(widen(v.name).c_str(), v.remove ? nullptr : widen(v.value).c_str());
        return startProcess(exe, L"\"" + exe.wstring() + L"\" \"" + elf.wstring() + L"\"", cwd, logPath, showConsole, error);
    }

    GameProcess *startTool(const std::filesystem::path &exe, const std::vector<std::string> &args,
                           const std::filesystem::path &cwd, const std::filesystem::path &logPath, std::string &error)
    {
        // Output goes straight to the file (no pipe through the launcher), so the tool keeps running and
        // logging if the launcher is closed.
        std::wstring cmd = L"\"" + exe.wstring() + L"\"";
        for (const std::string &a : args)
            cmd += L" \"" + widen(a) + L"\"";
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE log = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
        HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = nul;
        si.hStdOutput = log;
        si.hStdError = log;
        auto *tool = new GameProcess();
        const BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                       cwd.empty() ? nullptr : cwd.c_str(), &si, &tool->pi);
        const std::string startError = ok ? std::string() : lastErrorText();
        if (log != INVALID_HANDLE_VALUE)
            CloseHandle(log);
        if (nul != INVALID_HANDLE_VALUE)
            CloseHandle(nul);
        if (!ok)
        {
            delete tool;
            error = "Cannot start " + exe.filename().string() + ": " + startError;
            return nullptr;
        }
        return tool;
    }

    bool gameRunning(GameProcess *game, uint32_t &exitCode)
    {
        if (!game)
            return false;
        if (WaitForSingleObject(game->pi.hProcess, 0) == WAIT_TIMEOUT)
            return true;
        DWORD code = 0;
        GetExitCodeProcess(game->pi.hProcess, &code);
        exitCode = code;
        return false;
    }

    void closeGame(GameProcess *game)
    {
        if (!game)
            return;
        // The pipe ends once the game and anything it started have exited.
        if (game->writer.joinable())
            game->writer.join();
        if (game->pipeRead) // tools write their log file themselves
            CloseHandle(game->pipeRead);
        if (game->log != INVALID_HANDLE_VALUE)
            CloseHandle(game->log);
        if (game->console != INVALID_HANDLE_VALUE)
        {
            CloseHandle(game->console);
            FreeConsole();
        }
        CloseHandle(game->pi.hThread);
        CloseHandle(game->pi.hProcess);
        delete game;
    }

    bool httpGet(const std::string &url, std::string *body, const std::filesystem::path &to, std::string &error,
                 std::atomic<uint64_t> *got)
    {
        const std::wstring wurl = widen(url);
        URL_COMPONENTS uc{};
        uc.dwStructSize = sizeof(uc);
        uc.dwHostNameLength = static_cast<DWORD>(-1);
        uc.dwUrlPathLength = static_cast<DWORD>(-1);
        uc.dwExtraInfoLength = static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc))
        {
            error = "Bad address: " + url;
            return false;
        }
        const std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
        const std::wstring path = std::wstring(uc.lpszUrlPath, uc.dwUrlPathLength) + std::wstring(uc.lpszExtraInfo, uc.dwExtraInfoLength);

        HINTERNET session = WinHttpOpen(L"S2Recomp-Launcher", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                        WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) // before Windows 8.1 there is no automatic proxy
            session = WinHttpOpen(L"S2Recomp-Launcher", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        HINTERNET connect = session ? WinHttpConnect(session, host.c_str(), uc.nPort, 0) : nullptr;
        HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                         WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                         uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                                    : nullptr;
        bool ok = request && WinHttpSendRequest(request, L"Accept: application/vnd.github+json, */*\r\n", static_cast<DWORD>(-1),
                                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                  WinHttpReceiveResponse(request, nullptr);
        if (!ok)
            error = "Cannot reach " + std::string(host.begin(), host.end()) + ": " + lastErrorText();
        DWORD status = 0, size = sizeof(status);
        if (ok && WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                      &status, &size, WINHTTP_NO_HEADER_INDEX) && status != 200)
        {
            error = "The server answered " + std::to_string(status) + ".";
            ok = false;
        }

        HANDLE file = INVALID_HANDLE_VALUE;
        if (ok && !body)
        {
            file = CreateFileW(to.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
            {
                error = "Cannot write " + to.string() + ": " + lastErrorText();
                ok = false;
            }
        }
        std::string buf(1 << 16, '\0');
        while (ok)
        {
            DWORD n = 0;
            if (!WinHttpReadData(request, buf.data(), static_cast<DWORD>(buf.size()), &n))
            {
                error = "The download stopped: " + lastErrorText();
                ok = false;
                break;
            }
            if (n == 0)
                break;
            DWORD wrote = 0;
            if (body)
                body->append(buf.data(), n);
            else if (!WriteFile(file, buf.data(), n, &wrote, nullptr) || wrote != n)
            {
                error = "Cannot write " + to.string() + ": " + lastErrorText();
                ok = false;
            }
            if (got)
                *got += n;
        }
        if (file != INVALID_HANDLE_VALUE)
            CloseHandle(file);
        if (request)
            WinHttpCloseHandle(request);
        if (connect)
            WinHttpCloseHandle(connect);
        if (session)
            WinHttpCloseHandle(session);
        return ok;
    }

    bool startDetached(const std::filesystem::path &exe, const std::vector<std::string> &args, std::string &error)
    {
        std::wstring params;
        for (const std::string &a : args)
            params += (params.empty() ? L"" : L" ") + widen(a);
        // ShellExecute, not CreateProcess: a setup program may ask for elevation in its manifest.
        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_NOASYNC;
        sei.lpVerb = L"open";
        sei.lpFile = exe.c_str();
        sei.lpParameters = params.c_str();
        sei.nShow = SW_SHOWNORMAL;
        if (ShellExecuteExW(&sei))
            return true;
        error = "Cannot start " + exe.filename().string() + ": " + lastErrorText();
        return false;
    }

    void openPath(const std::filesystem::path &path)
    {
        ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    std::filesystem::path pickFile(const char *title)
    {
        wchar_t file[MAX_PATH * 4] = L"";
        const std::wstring wtitle = widen(title);
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = GetActiveWindow();
        ofn.lpstrFilter = L"PS2 game executable (SLES_*, SLUS_*, ...)\0*.*\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = static_cast<DWORD>(std::size(file));
        ofn.lpstrTitle = wtitle.c_str();
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetOpenFileNameW(&ofn))
            return {};
        return std::filesystem::path(file);
    }

    void showError(const char *title, const std::string &text)
    {
        MessageBoxW(GetActiveWindow(), widen(text).c_str(), widen(title).c_str(), MB_OK | MB_ICONWARNING);
    }
}
