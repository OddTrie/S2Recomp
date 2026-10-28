#pragma once

// Display-only frame generation (PS2X_FRAMEGEN=1): shows a motion-compensated in-between picture between two
// game frames, so a 30 FPS game is presented at the monitor's 60 Hz. Runs on the host window thread (plus one
// motion-estimation worker); it only reads the latched presentation pictures and never touches emulation state.

#include "raylib.h"
#include <cstdint>

namespace ps2x_framegen
{
    bool enabled();

    // After the window exists. textureFilter = the presenter's filter for drawing real frames.
    void init(int texWidth, int texHeight, int textureFilter);
    void shutdown();

    // No picture (the caller shows its own placeholder).
    void reset();

    // A freshly latched picture (texWidth x texHeight RGBA). A picture that differs from the last one is a new
    // source frame.
    void submit(const uint8_t *rgba, uint32_t width, uint32_t height);

    // The GPU renderer's internal-resolution picture (PS2X_GS_SCALE) of this present, right after submit(): kept
    // with a source frame that arrived this present, so present() shows and generates frames at that resolution.
    // `tex` is a GL texture (rows top first) that only has to stay valid until the frame is submitted.
    void submitHires(unsigned tex, int width, int height);

    // Once per host present, before BeginDrawing: picks what this present shows (renders an in-between picture
    // if needed) and returns the texture to draw plus its picture size (the internal-resolution size when the
    // frames have that picture), or nullptr if there is nothing yet.
    const Texture2D *present(uint32_t &width, uint32_t &height);
}
