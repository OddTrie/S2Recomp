#!/usr/bin/env python3
"""S2Recomp setup window: the .run opens it (launch.sh) on a desktop.

It asks for the install folder, the disc image, how to build and whether to add the game to Steam,
then runs install.sh with those answers and shows its progress. install.sh does the work; its
"STEP <text>" lines set the status, "PROGRESS <n>" lines the progress bar, and "ERROR <text>" ends it.
Only the standard library (Tk), so nothing has to be installed first.
"""
import os
import queue
import shutil
import signal
import subprocess
import sys
import threading
import webbrowser
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, ttk
from tkinter.scrolledtext import ScrolledText

HERE = Path(__file__).resolve().parent
FILES = HERE / "files"
TITLE = "S2Recomp Setup"
# ~/Games/<name>, like the other recomps; an install in the old default folder is updated where it is.
DEFAULT_PREFIX = Path.home() / "Games/S2Recomp"
OLD_PREFIX = Path(os.environ.get("XDG_DATA_HOME") or Path.home() / ".local/share") / "s2recomp"
if not DEFAULT_PREFIX.exists() and (OLD_PREFIX / "S2RecompLauncher").is_file():
    DEFAULT_PREFIX = OLD_PREFIX
GAME_EXE = "SLES_537.18"
BUILDS = (("Fast (uses the whole PC)", "fast"), ("In the background (PC stays usable)", "background"),
          ("Later (from the launcher)", "none"))


def has_steam():
    return bool(shutil.which("steam") or shutil.which("steamos-add-to-steam")
                or (Path.home() / ".var/app/com.valvesoftware.Steam").is_dir())


class Setup:
    def __init__(self, root):
        self.root = root
        self.proc = None
        self.events = queue.Queue()
        root.title(TITLE)
        root.minsize(640, 560)
        ttk.Style(root).theme_use("clam")

        frame = ttk.Frame(root, padding=16)
        frame.pack(fill="both", expand=True)
        frame.columnconfigure(1, weight=1)

        # Header
        head = ttk.Frame(frame)
        head.grid(row=0, column=0, columnspan=3, sticky="we", pady=(0, 12))
        self.icon = None
        icon_file = FILES / "s2recomp.png"
        if icon_file.is_file():
            try:
                img = tk.PhotoImage(file=str(icon_file))
                root.iconphoto(True, img)
                self.icon = img.subsample(max(1, img.width() // 64))
                ttk.Label(head, image=self.icon).pack(side="left", padx=(0, 12))
            except tk.TclError:
                pass
        text = ttk.Frame(head)
        text.pack(side="left", fill="x", expand=True)
        ttk.Label(text, text="S2Recomp", font=("TkDefaultFont", 22, "bold")).pack(anchor="w")
        ttk.Label(text, wraplength=480, justify="left",
                  text="Builds The Sims 2 (PlayStation 2, PAL / Europe, SLES-53718) as a native Linux game "
                       "from your own disc image. No game files or game code come with S2Recomp.").pack(anchor="w")

        # Install folder
        ttk.Label(frame, text="Install folder").grid(row=1, column=0, sticky="w", pady=4)
        self.prefix = tk.StringVar(value=str(DEFAULT_PREFIX))
        self.prefix.trace_add("write", lambda *_: self.refresh())
        ttk.Entry(frame, textvariable=self.prefix).grid(row=1, column=1, sticky="we", padx=6)
        ttk.Button(frame, text="Browse...", command=self.pick_prefix).grid(row=1, column=2)

        # Disc image
        ttk.Label(frame, text="Disc image (.iso)").grid(row=2, column=0, sticky="w", pady=4)
        self.iso = tk.StringVar()
        ttk.Entry(frame, textvariable=self.iso).grid(row=2, column=1, sticky="we", padx=6)
        ttk.Button(frame, text="Browse...", command=self.pick_iso).grid(row=2, column=2)
        self.iso_note = ttk.Label(frame, foreground="#666")
        self.iso_note.grid(row=3, column=1, columnspan=2, sticky="w", padx=6)

        # Build
        build = ttk.LabelFrame(frame, text="Build the game on this PC (about 30 minutes to a few hours, about 5 GB)",
                               padding=8)
        build.grid(row=4, column=0, columnspan=3, sticky="we", pady=(10, 4))
        self.build = tk.StringVar(value="fast")
        for label, value in BUILDS:
            ttk.Radiobutton(build, text=label, value=value, variable=self.build).pack(anchor="w")

        # Options
        self.steam = tk.BooleanVar(value=has_steam())
        ttk.Checkbutton(frame, text="Add to Steam (library, Big Picture, Game Mode)",
                        variable=self.steam).grid(row=5, column=0, columnspan=3, sticky="w", pady=(6, 0))
        self.deps = tk.BooleanVar(value=bool(shutil.which("pkexec")))
        ttk.Checkbutton(frame, text="Install missing system libraries (asks for your password)",
                        variable=self.deps).grid(row=6, column=0, columnspan=3, sticky="w")

        # Progress
        self.status = ttk.Label(frame, text="Ready.")
        self.status.grid(row=7, column=0, columnspan=3, sticky="w", pady=(12, 2))
        self.bar = ttk.Progressbar(frame, maximum=100)
        self.bar.grid(row=8, column=0, columnspan=3, sticky="we")
        self.log = ScrolledText(frame, height=10, state="disabled", font=("TkFixedFont", 9), wrap="word")
        self.log.grid(row=9, column=0, columnspan=3, sticky="nsew", pady=(6, 0))
        frame.rowconfigure(9, weight=1)

        # Buttons
        buttons = ttk.Frame(frame)
        buttons.grid(row=10, column=0, columnspan=3, sticky="e", pady=(12, 0))
        self.install_btn = ttk.Button(buttons, text="Install", command=self.install)
        self.play_btn = ttk.Button(buttons, text="Play", command=self.play)
        self.steam_btn = ttk.Button(buttons, text="Add to Steam", command=self.add_to_steam)
        self.stop_btn = ttk.Button(buttons, text="Stop", command=self.stop)
        self.close_btn = ttk.Button(buttons, text="Close", command=self.close)
        for b in (self.install_btn, self.play_btn, self.steam_btn, self.stop_btn, self.close_btn):
            b.pack(side="left", padx=(6, 0))
        discord = ttk.Label(frame, text="Join the S2Recomp Discord", foreground="#2a6fdb", cursor="hand2",
                            font=("TkDefaultFont", 10, "underline"))
        discord.grid(row=11, column=0, columnspan=3, sticky="w", pady=(8, 0))
        discord.bind("<Button-1>", lambda _e: webbrowser.open("https://discord.com/invite/YwAx7DrrFY"))

        root.protocol("WM_DELETE_WINDOW", self.close)
        self.refresh()
        root.after(100, self.poll)

    # ---- State ----
    def prefix_path(self):
        return Path(os.path.expanduser(self.prefix.get().strip() or str(DEFAULT_PREFIX))).resolve()

    def installed(self):
        return (self.prefix_path() / "S2RecompLauncher").is_file()

    def has_game_files(self):
        return (self.prefix_path() / "extracted" / GAME_EXE).is_file()

    def refresh(self):
        running = self.proc is not None
        self.install_btn.configure(text="Update" if self.installed() else "Install",
                                   state="disabled" if running else "normal")
        self.play_btn.configure(state="normal" if self.installed() and not running else "disabled")
        self.steam_btn.configure(state="normal" if self.installed() and not running else "disabled")
        self.stop_btn.configure(state="normal" if running else "disabled")
        self.iso_note.configure(text="Game files are already set up; leave this empty to keep them."
                                if self.has_game_files() else
                                "Your own copy of the game, made from your disc.")

    def pick_prefix(self):
        d = filedialog.askdirectory(parent=self.root, title="Install folder", initialdir=str(self.prefix_path().parent))
        if d:
            p = Path(d)
            self.prefix.set(str(p if p.name == "s2recomp" or (p / "S2RecompLauncher").exists() else p / "s2recomp"))

    def pick_iso(self):
        f = filedialog.askopenfilename(parent=self.root, title="The Sims 2 disc image",
                                       filetypes=(("Disc images", "*.iso *.ISO"), ("All files", "*")))
        if f:
            self.iso.set(f)

    def write(self, line):
        self.log.configure(state="normal")
        self.log.insert("end", line + "\n")
        self.log.see("end")
        self.log.configure(state="disabled")

    def set_status(self, text):
        self.status.configure(text=text)

    # ---- Install ----
    def install(self):
        prefix = self.prefix_path()
        if prefix in (Path("/"), Path.home()) or str(prefix).startswith(("/usr", "/etc")):
            messagebox.showerror(TITLE, f"Choose a folder of its own, not {prefix}.", parent=self.root)
            return
        iso = os.path.expanduser(self.iso.get().strip())
        if iso:
            if not Path(iso).is_file():
                messagebox.showerror(TITLE, f"The disc image was not found:\n{iso}", parent=self.root)
                return
            check = subprocess.run([str(FILES / "s2r_setup"), "--check", iso], capture_output=True, text=True)
            if check.returncode != 0:
                why = (check.stdout + check.stderr).strip() or "It is not The Sims 2 (PAL, SLES-53718)."
                messagebox.showerror(TITLE, f"This disc image cannot be used.\n\n{why}", parent=self.root)
                return
        elif not self.has_game_files():
            if not messagebox.askyesno(TITLE, "No disc image is chosen, so the game cannot be built yet.\n\n"
                                       "Install S2Recomp anyway and set up the disc later from the launcher?",
                                       parent=self.root):
                return

        cmd = ["bash", str(HERE / "install.sh"), "--yes", "--prefix", str(prefix), "--build", self.build.get(),
               "--steam" if self.steam.get() else "--no-steam"]
        if iso:
            cmd += ["--iso", iso]
        env = dict(os.environ, S2R_GUI="1")
        if not self.deps.get():
            cmd.append("--no-deps")
        elif shutil.which("pkexec"):
            env["S2R_SUDO"] = "pkexec"

        self.log.configure(state="normal")
        self.log.delete("1.0", "end")
        self.log.configure(state="disabled")
        self.bar.configure(mode="indeterminate")
        self.bar.start(15)
        self.set_status("Starting...")
        self.error = None
        try:
            self.proc = subprocess.Popen(cmd, cwd=str(HERE), env=env, stdin=subprocess.DEVNULL,
                                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                         bufsize=1, start_new_session=True)
        except OSError as e:
            self.finish_bar()
            messagebox.showerror(TITLE, f"Could not start the installer: {e}", parent=self.root)
            return
        self.refresh()
        threading.Thread(target=self.read_output, args=(self.proc,), daemon=True).start()

    def read_output(self, proc):
        for line in proc.stdout:
            self.events.put(("line", line.rstrip("\n")))
        self.events.put(("done", proc.wait()))

    def poll(self):
        try:
            while True:
                kind, value = self.events.get_nowait()
                if kind == "line":
                    self.on_line(value)
                elif kind == "done":
                    self.on_done(value)
                elif kind == "steam":
                    self.on_steam(*value)
        except queue.Empty:
            pass
        self.root.after(100, self.poll)

    def on_line(self, line):
        if line.startswith("PROGRESS "):
            try:
                n = float(line.split()[1])
            except (IndexError, ValueError):
                return
            if str(self.bar.cget("mode")) != "determinate":
                self.bar.stop()
                self.bar.configure(mode="determinate")
            self.bar["value"] = n
            return
        if line.startswith("STEP "):
            text = line[5:].strip()
            self.set_status(text + "...")
            self.write("== " + text)
            # A new step starts unmeasured until it sends PROGRESS.
            self.bar.configure(mode="indeterminate")
            self.bar["value"] = 0
            self.bar.start(15)
            return
        if line.startswith("ERROR "):
            self.error = line[6:].strip()
        self.write(line)

    def finish_bar(self, value=0):
        self.bar.stop()
        self.bar.configure(mode="determinate")
        self.bar["value"] = value

    def on_done(self, code):
        self.proc = None
        self.refresh()
        if code == 0:
            self.finish_bar(100)
            self.set_status("S2Recomp is installed.")
            build = self.build.get()
            if not self.has_game_files():
                more = "Choose your disc image in the launcher (or here) to build the game."
            elif build == "fast":
                more = "The game is built: press Play." if (self.prefix_path() / "ps2EntryRunner").exists() else \
                    "Building the game did not finish; see the log, or use Build game in the launcher."
            elif build == "background":
                more = "The game is being built in the background; play once it has finished (build_log.txt)."
            else:
                more = "Build the game from the launcher when you are ready."
            messagebox.showinfo(TITLE, f"S2Recomp is installed in\n{self.prefix_path()}\n\n{more}\n\n"
                                "It is in your application menu under Games.", parent=self.root)
        elif code < 0 or code in (130, 143):
            self.finish_bar()
            self.set_status("Stopped.")
        else:
            self.finish_bar()
            self.set_status("Setup failed.")
            messagebox.showerror(TITLE, (self.error or "Setup failed; the log shows why.") +
                                 f"\n\n(exit code {code})", parent=self.root)

    def stop(self):
        if self.proc:
            try:
                os.killpg(self.proc.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass

    def close(self):
        if self.proc:
            if not messagebox.askyesno(TITLE, "Setup is still running. Stop it and close?", parent=self.root):
                return
            self.stop()
        self.root.destroy()

    # ---- After installing ----
    def play(self):
        prefix = self.prefix_path()
        try:
            subprocess.Popen([str(prefix / "S2RecompLauncher")], cwd=str(prefix), start_new_session=True,
                             stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except OSError as e:
            messagebox.showerror(TITLE, f"Could not start S2Recomp: {e}", parent=self.root)

    def add_to_steam(self):
        script = self.prefix_path() / "s2r-steam.sh"
        if not script.is_file():
            script = FILES / "s2r-steam.sh"
        self.steam_btn.configure(state="disabled")
        self.set_status("Adding S2Recomp to Steam...")

        def run():
            try:
                r = subprocess.run(["bash", str(script), str(self.prefix_path())], capture_output=True, text=True,
                                   stdin=subprocess.DEVNULL, timeout=60)
                self.events.put(("steam", (r.returncode, (r.stdout + r.stderr).strip())))
            except (OSError, subprocess.TimeoutExpired) as e:
                self.events.put(("steam", (1, str(e))))
        threading.Thread(target=run, daemon=True).start()

    def on_steam(self, code, text):
        self.refresh()
        self.write(text)
        if code == 0:
            self.set_status("Added to Steam.")
            messagebox.showinfo(TITLE, text or "Added S2Recomp to Steam.", parent=self.root)
        elif code == 2:
            self.set_status("Add it to Steam by hand.")
            messagebox.showinfo(TITLE, text, parent=self.root)
        else:
            self.set_status("Steam was not found.")
            messagebox.showwarning(TITLE, text or "Steam was not found.", parent=self.root)


def main():
    try:
        root = tk.Tk(className="s2recomp-setup")
    except tk.TclError as e:
        print(f"Cannot open the setup window ({e}); run the .run with --cli for the terminal installer.",
              file=sys.stderr)
        return 1
    Setup(root)
    root.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
