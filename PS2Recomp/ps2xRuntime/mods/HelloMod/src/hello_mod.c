/*
 * Hello Mod: the smallest useful code mod, to copy when you start your own.
 *
 * It shows every part of a mod: start-up (S2R_ModInit), something each frame (S2R_ModFrame), a section
 * in the Mods menu (S2R_ModMenu, F2 in game), text over the game (S2R_ModHud), a setting kept in
 * mod.ini, a hotkey and a message in the corner of the screen.
 *
 * Build it with:  mods\_sdk\build_mod.bat mods\HelloMod   (close the game first: the DLL is in use)
 */
#include "s2r_mod_api.h"

#include <stdio.h>
#include <stdlib.h>

static const S2RModApi *api; /* keep the API pointer: it is valid until the game closes */
static int showGreeting = 1; /* the setting kept in mod.ini ("show_greeting = 1") */
static int presses = 0;
static char greeting[64] = "Sul sul!"; /* "hello" in Simlish */

S2R_EXPORT int S2R_ModInit(const S2RModApi *modApi)
{
    api = modApi;
    if (api->version < 1)
        return 1; /* a game older than this mod: nonzero unloads the mod */

    showGreeting = atoi(api->ini_get("show_greeting", "1"));
    snprintf(greeting, sizeof(greeting), "%s", api->ini_get("greeting", "Sul sul!"));
    api->log("Hello from %s! The mod folder is %s", api->mod_name, api->mod_dir);
    return 0; /* 0 = keep the mod loaded */
}

S2R_EXPORT void S2R_ModFrame(void)
{
    /* F6 turns the greeting on and off. */
    if (api->key_pressed(S2R_KEY_F6))
    {
        showGreeting = !showGreeting;
        api->ini_set("show_greeting", showGreeting ? "1" : "0");
        api->notify(showGreeting ? "Greeting on" : "Greeting off");
    }
}

S2R_EXPORT void S2R_ModMenu(const S2RUi *ui)
{
    ui->text("This is Hello Mod's part of the Mods menu.");
    ui->text_disabled("mods/HelloMod/src/hello_mod.c draws it.");

    if (ui->checkbox("Show the greeting (F6)", &showGreeting))
        api->ini_set("show_greeting", showGreeting ? "1" : "0");

    if (ui->input_text("Greeting", greeting, sizeof(greeting)))
        api->ini_set("greeting", greeting);

    if (ui->button("Press me"))
    {
        presses++;
        api->notify("Pressed %d time%s", presses, presses == 1 ? "" : "s");
    }
    ui->same_line();
    ui->text("%d", presses);

    ui->text("The game has drawn %llu frames, %.0f seconds after start-up.",
             (unsigned long long)api->game_frames(), api->seconds());
}

S2R_EXPORT void S2R_ModHud(const S2RUi *ui)
{
    if (showGreeting)
        ui->hud_text(16.0f, 16.0f, 0xFFE070FFu, greeting); /* 0xRRGGBBAA: warm yellow */
}

S2R_EXPORT void S2R_ModShutdown(void)
{
    api->log("Goodbye after %d presses", presses);
}
