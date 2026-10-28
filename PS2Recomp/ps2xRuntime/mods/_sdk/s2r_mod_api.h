/*
 * S2Recomp code mods: the API a mod DLL gets from the game.
 *
 * A code mod is a mod folder (mods/<MyMod>/) with a DLL in it: mod.dll, or the file named by
 * "code = <file>.dll" in its mod.ini. The game loads it at start-up, before the game itself runs, and
 * calls the functions below that the DLL exports. Only S2R_ModInit is required.
 *
 *   S2R_EXPORT int  S2R_ModInit(const S2RModApi *api);  once at start-up; return 0 to stay loaded,
 *                                                       anything else unloads the mod again
 *   S2R_EXPORT void S2R_ModFrame(void);                 once per frame shown (host thread)
 *   S2R_EXPORT void S2R_ModMenu(const S2RUi *ui);       the mod's section of the Mods menu (F2)
 *   S2R_EXPORT void S2R_ModHud(const S2RUi *ui);        every frame, over the game (on-screen text)
 *   S2R_EXPORT void S2R_ModShutdown(void);              when the game closes
 *
 * Every callback runs on the host (window) thread while the game runs on its own threads, so game
 * memory can change between two reads. Keep the API pointer from S2R_ModInit; it stays valid until
 * S2R_ModShutdown. Call api functions only from inside these callbacks.
 *
 * This header is plain C and needs nothing else: copy it next to your mod's source (mods/_sdk has a
 * copy, plus a build script and example mods).
 */
#ifndef S2R_MOD_API_H
#define S2R_MOD_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define S2R_EXPORT __declspec(dllexport)
#else
#define S2R_EXPORT __attribute__((visibility("default")))
#endif

#define S2R_MOD_API_VERSION 1

/* Key codes for key_pressed / key_down (the same numbers as GLFW and raylib). */
enum
{
    S2R_KEY_SPACE = 32, S2R_KEY_0 = 48, S2R_KEY_1, S2R_KEY_2, S2R_KEY_3, S2R_KEY_4, S2R_KEY_5, S2R_KEY_6,
    S2R_KEY_7, S2R_KEY_8, S2R_KEY_9,
    S2R_KEY_A = 65, /* ... S2R_KEY_A + n for the n-th letter */
    S2R_KEY_ESCAPE = 256, S2R_KEY_ENTER = 257, S2R_KEY_TAB = 258, S2R_KEY_BACKSPACE = 259,
    S2R_KEY_INSERT = 260, S2R_KEY_DELETE = 261, S2R_KEY_RIGHT = 262, S2R_KEY_LEFT = 263, S2R_KEY_DOWN = 264,
    S2R_KEY_UP = 265, S2R_KEY_PAGE_UP = 266, S2R_KEY_PAGE_DOWN = 267, S2R_KEY_HOME = 268, S2R_KEY_END = 269,
    S2R_KEY_F1 = 290, S2R_KEY_F2, S2R_KEY_F3, S2R_KEY_F4, S2R_KEY_F5, S2R_KEY_F6, S2R_KEY_F7, S2R_KEY_F8,
    S2R_KEY_F9, S2R_KEY_F10, S2R_KEY_F11, S2R_KEY_F12
};
/* F1 = debug panel, F2 = Mods menu, F10 = GS recording, F11 = fullscreen; the game uses WASD, IJKL,
 * the arrows, Space, Backspace, R, F, Q, E, Z, C, Shift, Ctrl, Enter, Esc and Tab. F5-F9 and F12 are free. */

/* A small immediate-mode UI (Dear ImGui underneath), for S2R_ModMenu and S2R_ModHud. Functions that
 * return int return nonzero when the user just changed or clicked the item. Labels must be unique
 * within a window; "Label##id" shows "Label" and uses the rest as the id. */
typedef struct S2RUi
{
    void (*text)(const char *fmt, ...);
    void (*text_disabled)(const char *fmt, ...); /* greyed out */
    void (*text_wrapped)(const char *fmt, ...);
    int (*button)(const char *label);
    int (*checkbox)(const char *label, int *value);
    int (*input_text)(const char *label, char *buffer, int bufferSize);
    int (*input_int)(const char *label, int *value);
    int (*slider_int)(const char *label, int *value, int min, int max);
    int (*slider_float)(const char *label, float *value, float min, float max);
    int (*selectable)(const char *label, int selected);
    void (*same_line)(void);
    void (*separator)(void);
    int (*collapsing_header)(const char *label);
    void (*push_id)(int id);
    void (*pop_id)(void);
    /* Own windows (in S2R_ModHud too). begin returns 0 when the window is collapsed; call end either
     * way. open may be NULL; otherwise the window has a close button that sets *open to 0. */
    int (*begin)(const char *title, int *open);
    void (*end)(void);
    /* Text straight on the screen, in window pixels from the top left; color is 0xRRGGBBAA. */
    void (*hud_text)(float x, float y, uint32_t color, const char *text);
    void (*hud_rect)(float x, float y, float w, float h, uint32_t color);
    float (*screen_width)(void);
    float (*screen_height)(void);
    int menu_open; /* the Mods menu (F2) is showing */
} S2RUi;

typedef struct S2RModApi
{
    int version; /* S2R_MOD_API_VERSION the game was built with */
    size_t size; /* sizeof(S2RModApi) in the game: newer games append fields at the end */

    /* This mod. */
    const char *mod_dir;  /* full path of mods/<MyMod>, UTF-8 */
    const char *mod_name; /* name from mod.ini (or the folder name) */

    /* Writes a line to the console and run_log.txt, prefixed with the mod's name. printf format. */
    void (*log)(const char *fmt, ...);

    /* Your mod.ini: ini_get returns the value of "key = value" (or fallback when it is missing); the
     * string stays valid until the next ini_set. ini_set adds or replaces the key and saves the file. */
    const char *(*ini_get)(const char *key, const char *fallback);
    int (*ini_set)(const char *key, const char *value);

    /* The PS2's 32 MB of main RAM. Addresses are PS2 addresses as the game sees them (0x00100000,
     * 0x20100000 and 0x80100000 are the same byte). read/write return 0 when the range is outside RAM. */
    uint8_t *ram;      /* ram + (address & 0x01FFFFFF) is the byte at a PS2 address */
    uint32_t ram_size; /* 0x02000000 */
    int (*read)(uint32_t address, void *out, uint32_t size);
    int (*write)(uint32_t address, const void *data, uint32_t size);

    /* Keyboard, polled on the host thread (the game reads the same keys). */
    int (*key_pressed)(int key); /* went down since the last frame */
    int (*key_down)(int key);

    /* Memory cards: the host folder for card 0 or 1 (UTF-8; valid until the next call). set_memcard_folder
     * swaps the card in that slot for the folder (created if missing; NULL or "" = the default card)
     * and tells the game the card was changed, like pushing in another card; it returns 0 and changes
     * nothing while the game is reading or writing that card. memcard_busy is nonzero while it is. */
    const char *(*memcard_folder)(int port);
    int (*set_memcard_folder)(int port, const char *folder);
    int (*memcard_busy)(void);

    /* Time. */
    uint64_t (*game_frames)(void); /* frames the game has drawn so far (about 30 a second) */
    double (*seconds)(void);       /* seconds since the game started */

    /* Shows a short message in the corner of the screen for a few seconds. */
    void (*notify)(const char *fmt, ...);
} S2RModApi;

typedef int (*S2RModInitFn)(const S2RModApi *api);
typedef void (*S2RModFrameFn)(void);
typedef void (*S2RModUiFn)(const S2RUi *ui);
typedef void (*S2RModShutdownFn)(void);

#ifdef __cplusplus
}
#endif

#endif /* S2R_MOD_API_H */
