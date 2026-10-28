#pragma once

// OS side of the launcher (process start, folders, dialogs). Kept apart from launcher_main.cpp so
// windows.h and raylib.h never meet in one translation unit.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace launcher
{
    std::filesystem::path exeDir();

    // One variable of the game's environment: set to `value`, or removed (inherited value dropped) when `remove`.
    struct EnvVar
    {
        std::string name;
        std::string value;
        bool remove = false;
    };

    struct GameProcess;

    // Starts the game with `env` applied, its working directory `cwd`, and its console output written to
    // `logPath` (and to a console window when `showConsole`). Returns nullptr and fills `error` on failure.
    GameProcess *startGame(const std::filesystem::path &exe, const std::filesystem::path &elf,
                           const std::filesystem::path &cwd, const std::vector<EnvVar> &env,
                           const std::filesystem::path &logPath, bool showConsole, std::string &error);
    // Starts a helper program (the game build script) with `args`, no window, its output written to `logPath`.
    GameProcess *startTool(const std::filesystem::path &exe, const std::vector<std::string> &args,
                           const std::filesystem::path &cwd, const std::filesystem::path &logPath, std::string &error);
    // True while the game (or tool) runs; once it has exited, `exitCode` holds its exit code.
    bool gameRunning(GameProcess *game, uint32_t &exitCode);
    void closeGame(GameProcess *game); // after it exited: frees the handles and the log writer

    // Fetches an https `url` (following redirects) into `body`, or into the file `to` when `body` is null.
    // Blocks; returns false and fills `error` on failure. `got` (when set) counts the bytes received so far.
    bool httpGet(const std::string &url, std::string *body, const std::filesystem::path &to, std::string &error,
                 std::atomic<uint64_t> *got = nullptr);
    // Starts a program that outlives the launcher (the update's installer).
    bool startDetached(const std::filesystem::path &exe, const std::vector<std::string> &args, std::string &error);

    void openPath(const std::filesystem::path &path); // folder in Explorer, file in its default app
    std::filesystem::path pickFile(const char *title);  // empty when cancelled
    void showError(const char *title, const std::string &text);
}
