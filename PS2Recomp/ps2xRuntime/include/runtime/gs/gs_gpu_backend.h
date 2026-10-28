#pragma once

#include "runtime/gs/gs_backend.h"

#include <memory>
#include <string>

// GPU renderer for the emulated GS (the game's default, see PS2Runtime::initialize). Draws with OpenGL 3.3 on its own
// worker thread, in a context shared with the presenter window. GS local memory lives in a
// 2048x512 R32UI texture (one row per 8 KB page); frame and Z buffers are GL render targets kept
// coherent with it page by page. Transfers, CLUT loads and readbacks run on a CPU copy of local
// memory (GSCpuBackend), which is synchronised with the GPU only for the pages they touch.
// Anything the GPU path does not support is drawn by that CPU rasterizer instead.
// The CPU renderer (GSBandedBackend) stays the bit-exact reference: every backend made by
// ps2xMakeDefaultGsBackend, and every replay tool, uses it unless PS2X_GS_GPU=1 asks for the GPU.

// PS2X_GS_GPU set to something other than 0 (and built with GL support): the GPU is asked for explicitly.
bool ps2xGsGpuRequested();
// Built with GL support at all.
bool ps2xGsGpuAvailable();
std::unique_ptr<GSRasterBackend> ps2xMakeGsGpuBackend();

// Main thread: after the raylib window exists, creates the hidden shared GL context the renderer
// uses. standalone = no raylib window (replay tools): initialises GLFW itself.
void ps2xGsGpuHostInit(bool standalone);
// After ps2xGsGpuHostInit and ps2xMakeGsGpuBackend: waits until the renderer's worker has set up GL.
// false (with the reason) if it could not (no GL 3.3 context, missing entry points or features,
// shaders that do not compile) or did not finish within timeoutMs; the backend then only runs
// its single-threaded CPU fallback, so the caller should use the CPU renderer instead.
// PS2X_GS_GPU_FAIL_TEST=1 makes the setup fail (to test that path).
bool ps2xGsGpuWaitReady(int timeoutMs, std::string *reason);
// Main thread, before the window closes: the renderer releases its context and stops drawing.
void ps2xGsGpuHostShutdown();

// Internal resolution (PS2X_GS_SCALE=1..4, see gs_gpu_backend.cpp). Above 1 every present also renders
// the displayed picture at that resolution when it is in a GPU render target. Main thread (presenter
// context current), after the GS present of this host frame: the GL texture of that picture (shared
// context, rows top first), or false = draw the GS-resolution decode. A texture acquired must be released
// after the frame that draws it has been submitted (EndDrawing).
bool ps2xGsGpuAcquireHiresFrame(unsigned &tex, int &width, int &height);
void ps2xGsGpuReleaseHiresFrame();
