/*
 * Performance HUD: frames per second of the game (it aims for 30) and of the window, in a corner.
 * F8 shows or hides it; the corner and size are in mod.ini (corner = top-right, top-left,
 * bottom-left or bottom-right).
 *
 * An example of S2R_ModFrame doing a little work every frame and S2R_ModHud drawing over the game.
 */
#include "s2r_mod_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const S2RModApi *api;
static int visible = 1;
static int corner = 1; /* 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right */

/* Measured over half a second. */
static double windowStart, gameFps, windowFps;
static uint64_t gameFramesStart;
static int windowFrames;
static double worstFrameMs, worstShown, lastFrame;

static const char *cornerNames[] = {"top-left", "top-right", "bottom-left", "bottom-right"};

S2R_EXPORT int S2R_ModInit(const S2RModApi *modApi)
{
    int i;
    api = modApi;
    if (api->version < 1)
        return 1;
    visible = atoi(api->ini_get("visible", "1"));
    for (i = 0; i < 4; ++i)
        if (strcmp(api->ini_get("corner", "top-right"), cornerNames[i]) == 0)
            corner = i;
    windowStart = lastFrame = api->seconds();
    gameFramesStart = api->game_frames();
    return 0;
}

S2R_EXPORT void S2R_ModFrame(void)
{
    const double now = api->seconds();
    const double frameMs = (now - lastFrame) * 1000.0;
    lastFrame = now;
    if (frameMs > worstFrameMs)
        worstFrameMs = frameMs;
    windowFrames++;

    if (now - windowStart >= 0.5)
    {
        const uint64_t frames = api->game_frames();
        gameFps = (double)(frames - gameFramesStart) / (now - windowStart);
        windowFps = windowFrames / (now - windowStart);
        worstShown = worstFrameMs;
        gameFramesStart = frames;
        windowStart = now;
        windowFrames = 0;
        worstFrameMs = 0.0;
    }

    if (api->key_pressed(S2R_KEY_F8))
    {
        visible = !visible;
        api->ini_set("visible", visible ? "1" : "0");
    }
}

S2R_EXPORT void S2R_ModMenu(const S2RUi *ui)
{
    int i;
    if (ui->checkbox("Show (F8)", &visible))
        api->ini_set("visible", visible ? "1" : "0");
    for (i = 0; i < 4; ++i)
    {
        if (i % 2)
            ui->same_line();
        if (ui->selectable(cornerNames[i], corner == i))
        {
            corner = i;
            api->ini_set("corner", cornerNames[i]);
        }
    }
}

S2R_EXPORT void S2R_ModHud(const S2RUi *ui)
{
    char line1[64], line2[64];
    const float w = 200.0f, h = 46.0f, margin = 12.0f;
    float x, y;
    uint32_t color;
    if (!visible)
        return;

    snprintf(line1, sizeof(line1), "Game %4.1f FPS", gameFps);
    snprintf(line2, sizeof(line2), "Window %3.0f FPS  worst %3.0f ms", windowFps, worstShown);
    x = (corner & 1) ? ui->screen_width() - w - margin : margin;
    y = (corner & 2) ? ui->screen_height() - h - margin : margin;
    /* green at full speed, yellow below 27, red below 20 */
    color = gameFps >= 27.0 ? 0x80FF80FFu : gameFps >= 20.0 ? 0xFFE060FFu : 0xFF6060FFu;

    ui->hud_rect(x, y, w, h, 0x101820B0u);
    ui->hud_text(x + 8.0f, y + 6.0f, color, line1);
    ui->hud_text(x + 8.0f, y + 25.0f, 0xD0D0D0FFu, line2);
}
