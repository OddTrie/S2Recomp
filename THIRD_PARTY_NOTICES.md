# Third-party notices

S2Recomp is built on [PS2Recomp](https://github.com/ran-j/PS2Recomp) and is licensed under the GNU GPL v3 (see `LICENSE`).

The libraries below are not stored in this repository. CMake downloads them at build time (FFmpeg comes from the system or a prebuilt package), and the installers bundle their source. Each one keeps its own license.

| Library | Used for | License |
|---|---|---|
| [raylib](https://github.com/raysan5/raylib) 5.5 | Window, graphics, audio | zlib |
| [Dear ImGui](https://github.com/ocornut/imgui) 1.92.7 (docking) | Menus and overlays | MIT |
| [rlImGui](https://github.com/raylib-extras/rlImGui) | ImGui on raylib | zlib |
| [SDL2](https://github.com/libsdl-org/SDL) 2.32.10 | Controllers | zlib |
| [FFmpeg](https://ffmpeg.org/) | Game videos | LGPL 2.1+ |
| [ELFIO](https://github.com/serge1/ELFIO) 3.12 | Reading ELF files (recompiler) | MIT |
| [toml11](https://github.com/ToruNiina/toml11) 4.4.0 | Config files (recompiler) | MIT |
| [{fmt}](https://github.com/fmtlib/fmt) 12.1.0 | Text formatting (recompiler) | MIT |
| [libdwarf](https://github.com/davea42/libdwarf-code) 2.2.0 | Debug info (recompiler) | LGPL 2.1 |
| [rabbitizer](https://github.com/Decompollaborate/rabbitizer) 1.14.3 | MIPS disassembly (recompiler) | MIT |
| [sse2neon](https://github.com/DLTcollab/sse2neon) 1.9.1 | ARM builds only | MIT |

## Epic Online Services SDK

Online co-op uses the Epic Online Services (EOS) SDK. The SDK is not in this repository. It is © Epic Games, Inc. and is used under the [Epic Online Services SDK license](https://dev.epicgames.com/en-US/services/terms/agreements). The installers include only the EOS runtime library. Without the SDK, the game still builds and runs, but without online co-op.

## Game data

This repository has no game files and no code generated from the game. The installers build the game on your PC from your own copy of The Sims 2 (PS2). The Sims 2 is © Electronic Arts Inc. This project is not affiliated with or endorsed by Electronic Arts or Maxis.
