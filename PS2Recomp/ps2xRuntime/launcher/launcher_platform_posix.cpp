#include "launcher_platform.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace launcher
{
    namespace
    {
        bool haveCommand(const char *name)
        {
            const std::string cmd = std::string("command -v ") + name + " >/dev/null 2>&1";
            return std::system(cmd.c_str()) == 0;
        }

        // Single quotes for /bin/sh.
        std::string shellQuote(const std::string &s)
        {
            std::string out = "'";
            for (char c : s)
                out += c == '\'' ? std::string("'\\''") : std::string(1, c);
            return out + "'";
        }

        std::string runAndRead(const std::string &cmd)
        {
            std::string out;
            if (FILE *p = popen(cmd.c_str(), "r"))
            {
                char buf[4096];
                size_t n;
                while ((n = fread(buf, 1, sizeof(buf), p)) > 0)
                    out.append(buf, n);
                pclose(p);
            }
            while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
                out.pop_back();
            return out;
        }
    }

    struct GameProcess;
    // Starts args[0..] (args[0] is the program name) with its output going to `logPath` (and to stdout
    // when `showConsole`).
    GameProcess *startProcess(const std::filesystem::path &exe, const std::vector<std::string> &args,
                              const std::filesystem::path &cwd, const std::filesystem::path &logPath, bool showConsole,
                              std::string &error);

    struct GameProcess
    {
        pid_t pid = -1;
        int pipeRead = -1;
        int log = -1;
        bool console = false;
        bool exited = false;
        uint32_t exitCode = 0;
        std::thread writer;
    };

    std::filesystem::path exeDir()
    {
        std::error_code ec;
        const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", ec);
        return ec ? std::filesystem::current_path() : self.parent_path();
    }

    GameProcess *startGame(const std::filesystem::path &exe, const std::filesystem::path &elf,
                           const std::filesystem::path &cwd, const std::vector<EnvVar> &env,
                           const std::filesystem::path &logPath, bool showConsole, std::string &error)
    {
        // The child inherits the launcher's environment, so set ours (and drop inherited values for
        // settings left on "auto") before starting it. The game's FFmpeg libraries are in lib/ next to it.
        for (const EnvVar &v : env)
        {
            if (v.remove)
                unsetenv(v.name.c_str());
            else
                setenv(v.name.c_str(), v.value.c_str(), 1);
        }
        std::string libs = (exe.parent_path() / "lib").string();
        if (const char *old = std::getenv("LD_LIBRARY_PATH"); old && *old)
            libs += std::string(":") + old;
        setenv("LD_LIBRARY_PATH", libs.c_str(), 1);
        return startProcess(exe, {exe.string(), elf.string()}, cwd, logPath, showConsole, error);
    }

    GameProcess *startTool(const std::filesystem::path &exe, const std::vector<std::string> &args,
                           const std::filesystem::path &cwd, const std::filesystem::path &logPath, std::string &error)
    {
        // Output goes straight to the file (no pipe through the launcher), so the tool keeps running and
        // logging if the launcher is closed.
        std::vector<std::string> strs{exe.string()};
        strs.insert(strs.end(), args.begin(), args.end());
        std::vector<char *> argv;
        for (const std::string &a : strs)
            argv.push_back(const_cast<char *>(a.c_str()));
        argv.push_back(nullptr);
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_addopen(&actions, 1, logPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        posix_spawn_file_actions_adddup2(&actions, 1, 2);
        // Its own session, so closing the launcher (or its terminal) does not stop it.
        posix_spawnattr_t attr;
        posix_spawnattr_init(&attr);
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
        std::error_code ec;
        const std::filesystem::path oldCwd = std::filesystem::current_path(ec);
        if (!cwd.empty())
            std::filesystem::current_path(cwd, ec);
        pid_t pid = -1;
        const int rc = posix_spawn(&pid, strs[0].c_str(), &actions, &attr, argv.data(), environ);
        posix_spawnattr_destroy(&attr);
        posix_spawn_file_actions_destroy(&actions);
        if (!oldCwd.empty())
            std::filesystem::current_path(oldCwd, ec);
        if (rc != 0)
        {
            error = "Cannot start " + exe.filename().string() + ": " + std::strerror(rc);
            return nullptr;
        }
        auto *tool = new GameProcess();
        tool->pid = pid;
        return tool;
    }

    GameProcess *startProcess(const std::filesystem::path &exe, const std::vector<std::string> &args,
                              const std::filesystem::path &cwd, const std::filesystem::path &logPath, bool showConsole,
                              std::string &error)
    {
        int fds[2];
        if (pipe2(fds, O_CLOEXEC) != 0)
        {
            error = std::string("Cannot create the output pipe: ") + std::strerror(errno);
            return nullptr;
        }

        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_adddup2(&actions, fds[1], 1);
        posix_spawn_file_actions_adddup2(&actions, fds[1], 2);
        // posix_spawn has no portable chdir action before glibc 2.29's _np one, so the launcher changes its own
        // directory around the spawn instead (the UI never uses relative paths).
        std::error_code ec;
        const std::filesystem::path oldCwd = std::filesystem::current_path(ec);
        if (!cwd.empty())
            std::filesystem::current_path(cwd, ec);

        const std::string exeStr = exe.string();
        std::vector<char *> argv;
        for (const std::string &a : args)
            argv.push_back(const_cast<char *>(a.c_str()));
        argv.push_back(nullptr);
        pid_t pid = -1;
        const int rc = posix_spawn(&pid, exeStr.c_str(), &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        if (!oldCwd.empty())
            std::filesystem::current_path(oldCwd, ec);
        close(fds[1]);
        if (rc != 0)
        {
            close(fds[0]);
            error = "Cannot start " + exe.filename().string() + ": " + std::strerror(rc);
            return nullptr;
        }

        auto *game = new GameProcess();
        game->pid = pid;
        game->pipeRead = fds[0];
        game->log = open(logPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        // There is no console window to open on Linux; "Show console" copies the output to the launcher's own
        // stdout, which is the terminal when the launcher was started from one.
        game->console = showConsole;
        // Copy the game's output to run_log.txt (written through, so it survives a crash).
        game->writer = std::thread([game]
        {
            char buf[8192];
            ssize_t n;
            while ((n = read(game->pipeRead, buf, sizeof(buf))) > 0 || (n < 0 && errno == EINTR))
            {
                if (n <= 0)
                    continue;
                if (game->log >= 0)
                    (void)!write(game->log, buf, static_cast<size_t>(n));
                if (game->console)
                    (void)!write(1, buf, static_cast<size_t>(n));
            }
        });
        return game;
    }

    bool gameRunning(GameProcess *game, uint32_t &exitCode)
    {
        if (!game)
            return false;
        if (!game->exited)
        {
            int status = 0;
            const pid_t r = waitpid(game->pid, &status, WNOHANG);
            if (r == 0)
                return true;
            game->exited = true;
            if (r == game->pid)
                game->exitCode = WIFEXITED(status) ? static_cast<uint32_t>(WEXITSTATUS(status))
                                                   : 0x80000000u | static_cast<uint32_t>(WTERMSIG(status));
        }
        exitCode = game->exitCode;
        return false;
    }

    void closeGame(GameProcess *game)
    {
        if (!game)
            return;
        // The pipe ends once the game and anything it started have exited.
        if (game->writer.joinable())
            game->writer.join();
        close(game->pipeRead);
        if (game->log >= 0)
            close(game->log);
        delete game;
    }

    namespace
    {
        // Runs argv (searched on PATH) with no output and waits; true when it exits with 0.
        bool runQuiet(const std::vector<std::string> &args)
        {
            std::vector<char *> argv;
            for (const std::string &a : args)
                argv.push_back(const_cast<char *>(a.c_str()));
            argv.push_back(nullptr);
            posix_spawn_file_actions_t actions;
            posix_spawn_file_actions_init(&actions);
            posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
            posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
            posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
            pid_t pid = -1;
            const int rc = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
            posix_spawn_file_actions_destroy(&actions);
            if (rc != 0)
                return false;
            int status = 0;
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
            {
            }
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
    }

    bool httpGet(const std::string &url, std::string *body, const std::filesystem::path &to, std::string &error,
                 std::atomic<uint64_t> *got)
    {
        (void)got; // curl writes the file itself: the launcher watches its size instead
        // curl or wget: every desktop Linux and SteamOS has one of them.
        std::filesystem::path out = to;
        if (body)
            out = std::filesystem::temp_directory_path() / ("s2recomp-http-" + std::to_string(getpid()) + ".txt");
        const std::string file = out.string();
        bool ok;
        if (haveCommand("curl"))
            ok = runQuiet({"curl", "-fsSL", "-A", "S2Recomp-Launcher", "-H", "Accept: application/vnd.github+json, */*",
                           "-o", file, url});
        else if (haveCommand("wget"))
            ok = runQuiet({"wget", "-q", "-U", "S2Recomp-Launcher", "-O", file, url});
        else
        {
            error = "Neither curl nor wget is installed.";
            return false;
        }
        if (!ok)
            error = "Cannot download " + url;
        if (ok && body)
        {
            body->clear();
            if (FILE *f = std::fopen(file.c_str(), "rb"))
            {
                char buf[8192];
                size_t n;
                while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
                    body->append(buf, n);
                std::fclose(f);
            }
        }
        if (body)
            unlink(file.c_str());
        return ok;
    }

    bool startDetached(const std::filesystem::path &exe, const std::vector<std::string> &args, std::string &error)
    {
        chmod(exe.c_str(), 0755);
        std::vector<std::string> strs{exe.string()};
        strs.insert(strs.end(), args.begin(), args.end());
        std::vector<char *> argv;
        for (const std::string &a : strs)
            argv.push_back(const_cast<char *>(a.c_str()));
        argv.push_back(nullptr);
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
        posix_spawnattr_t attr;
        posix_spawnattr_init(&attr);
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
        pid_t pid = -1;
        const int rc = posix_spawn(&pid, strs[0].c_str(), &actions, &attr, argv.data(), environ);
        posix_spawnattr_destroy(&attr);
        posix_spawn_file_actions_destroy(&actions);
        if (rc == 0)
            return true;
        error = "Cannot start " + exe.filename().string() + ": " + std::strerror(rc);
        return false;
    }

    void openPath(const std::filesystem::path &path)
    {
        const std::string cmd = "xdg-open " + shellQuote(path.string()) + " >/dev/null 2>&1 &";
        (void)!std::system(cmd.c_str());
    }

    std::filesystem::path pickFile(const char *title)
    {
        std::string cmd;
        if (haveCommand("kdialog"))
            cmd = "kdialog --title " + shellQuote(title) + " --getopenfilename \"$HOME\" 2>/dev/null";
        else if (haveCommand("zenity"))
            cmd = "zenity --file-selection --title=" + shellQuote(title) + " 2>/dev/null";
        else
            return {};
        return std::filesystem::path(runAndRead(cmd));
    }

    void showError(const char *title, const std::string &text)
    {
        std::string cmd;
        if (haveCommand("kdialog"))
            cmd = "kdialog --title " + shellQuote(title) + " --sorry " + shellQuote(text);
        else if (haveCommand("zenity"))
            cmd = "zenity --warning --no-wrap --title=" + shellQuote(title) + " --text=" + shellQuote(text);
        if (cmd.empty() || std::system((cmd + " >/dev/null 2>&1").c_str()) != 0)
            std::fprintf(stderr, "%s: %s\n", title, text.c_str());
    }
}
