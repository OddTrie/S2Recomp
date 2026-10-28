// "S2Recomp Disc Setup.exe": the "Change game disc" shortcut. Asks for the player's disc image and runs
// s2r_setup.exe in a console window to copy the game files into extracted\ next to S2RecompLauncher.exe,
// where the launcher finds them.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>

#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace
{
    const wchar_t *kTitle = L"S2Recomp";

    fs::path exeDir()
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
        return fs::path(buf).parent_path();
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    const fs::path dir = exeDir();
    const fs::path gameDir = dir / L"extracted";

    wchar_t file[MAX_PATH * 4] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"Disc image (*.iso)\0*.iso\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = static_cast<DWORD>(std::size(file));
    ofn.lpstrTitle = L"Choose your The Sims 2 (PS2, Europe) disc image";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn))
        return 1;

    // The console shows the progress and stays open on an error so it can be read.
    const fs::path setup = dir / L"s2r_setup.exe";
    std::wstring cmd = L"cmd.exe /c \"\"" + setup.wstring() + L"\" \"" + file + L"\" \"" + gameDir.wstring() +
                       L"\" || pause\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, dir.c_str(), &si, &pi))
    {
        MessageBoxW(nullptr, (L"Cannot start " + setup.wstring()).c_str(), kTitle, MB_OK | MB_ICONWARNING);
        return 1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (code == 0)
        MessageBoxW(nullptr, L"The game files are ready. Start S2Recomp from its launcher.", kTitle, MB_OK | MB_ICONINFORMATION);
    return static_cast<int>(code);
}
