/*
 * Memory Tools: find where the game keeps a number (money, needs, time...) and change or freeze it.
 * The first step of most gameplay mods: once you know the address, a mod can read or write it.
 *
 * How to find a value, e.g. your household's money:
 *   1. Mods menu (F2) > Memory Tools. Type: 4 bytes. Value: the money shown in game. New search.
 *   2. Spend or earn some money in game, type the new amount, Next search. Repeat until a few are left.
 *   3. Set a new value, or Freeze it (written every frame, kept in mod.ini for next time).
 * Don't know the number (a need bar)? Search "Unknown value", then let it change and use
 * Increased / Decreased / Changed / Unchanged.
 *
 * Addresses differ between game versions (this is SLES-53718) and can move when the game loads a
 * lot, so search again after loading if a freeze stops working.
 */
#include "s2r_mod_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const S2RModApi *api;

enum { T_U8, T_U16, T_U32, T_FLOAT, T_COUNT };
static const char *typeNames[T_COUNT] = {"1 byte", "2 bytes", "4 bytes", "float"};
static const uint32_t typeSizes[T_COUNT] = {1, 2, 4, 4};

enum { CMP_EQUAL, CMP_CHANGED, CMP_UNCHANGED, CMP_INCREASED, CMP_DECREASED };

#define SEARCH_START 0x00100000u /* below this is the PS2 kernel */
#define MAX_RESULTS (4u * 1024u * 1024u)
#define SHOWN_RESULTS 40
#define MAX_FROZEN 32

static int type = T_U32;
static char valueText[32] = "";
static uint32_t *results;     /* addresses that still match */
static uint32_t resultCount;
static int searching;         /* a search is going (results or "every address") */
static int everyAddress;      /* after "Unknown value": every address matches so far */
static uint8_t *snapshot;     /* RAM at the last search, for changed/increased/... */
static char status[160] = "";

typedef struct
{
    uint32_t address;
    int type;
    double value;
    int on;
} Frozen;
static Frozen frozen[MAX_FROZEN];
static int frozenCount;
static char setText[SHOWN_RESULTS][24];

/* ---- values in RAM ---- */
static double readValue(const uint8_t *ram, uint32_t offset, int t)
{
    switch (t)
    {
    case T_U8: return ram[offset];
    case T_U16: { uint16_t v; memcpy(&v, ram + offset, 2); return v; }
    case T_U32: { uint32_t v; memcpy(&v, ram + offset, 4); return v; }
    default: { float v; memcpy(&v, ram + offset, 4); return v; }
    }
}

static void writeValue(uint32_t address, int t, double value)
{
    uint8_t u8 = (uint8_t)value;
    uint16_t u16 = (uint16_t)value;
    uint32_t u32 = (uint32_t)(int64_t)value;
    float f = (float)value;
    const void *data = t == T_U8 ? (const void *)&u8 : t == T_U16 ? (const void *)&u16 : t == T_U32 ? (const void *)&u32 : (const void *)&f;
    api->write(address, data, typeSizes[t]);
}

static void formatValue(char *out, size_t size, double v, int t)
{
    if (t == T_FLOAT)
        snprintf(out, size, "%g", v);
    else
        snprintf(out, size, "%.0f", v);
}

static int matches(double now, double before, double wanted, int cmp)
{
    switch (cmp)
    {
    case CMP_EQUAL: return type == T_FLOAT ? (now > wanted - 0.01 && now < wanted + 0.01) : now == wanted;
    case CMP_CHANGED: return now != before;
    case CMP_UNCHANGED: return now == before;
    case CMP_INCREASED: return now > before;
    default: return now < before;
    }
}

/* ---- searching ---- */
static void takeSnapshot(void)
{
    if (!snapshot)
        snapshot = (uint8_t *)malloc(api->ram_size);
    if (snapshot)
        memcpy(snapshot, api->ram, api->ram_size);
}

static void search(int cmp, int fresh)
{
    const uint32_t step = typeSizes[type];
    const uint32_t end = api->ram_size - typeSizes[type];
    const double wanted = atof(valueText);
    const uint8_t *ram = api->ram;
    uint32_t kept = 0, i, offset;

    if (!results)
        results = (uint32_t *)malloc(MAX_RESULTS * sizeof(uint32_t));
    if (!results)
        return;
    if (cmp != CMP_EQUAL && !snapshot)
    {
        snprintf(status, sizeof(status), "Search \"Unknown value\" first, then compare.");
        return;
    }

    if (fresh || everyAddress)
    {
        /* Every aligned address from SEARCH_START is a candidate. */
        for (offset = SEARCH_START; offset <= end && kept < MAX_RESULTS; offset += step)
        {
            const double before = snapshot ? readValue(snapshot, offset, type) : 0.0;
            if (matches(readValue(ram, offset, type), before, wanted, cmp))
                results[kept++] = offset;
        }
    }
    else
    {
        for (i = 0; i < resultCount; ++i)
        {
            offset = results[i];
            if (matches(readValue(ram, offset, type), readValue(snapshot ? snapshot : ram, offset, type), wanted, cmp))
                results[kept++] = offset;
        }
    }
    resultCount = kept;
    everyAddress = 0;
    searching = 1;
    takeSnapshot();
    snprintf(status, sizeof(status), "%u address%s match%s%s", kept, kept == 1 ? "" : "es", kept == 1 ? "es" : "",
             kept == MAX_RESULTS ? " (list full: search again to narrow it down)" : ".");
}

static void searchUnknown(void)
{
    takeSnapshot();
    everyAddress = 1;
    searching = 1;
    resultCount = 0;
    snprintf(status, sizeof(status), "Remembered every value. Let it change in game, then compare.");
}

/* ---- frozen values, kept in mod.ini as freeze1 = <address> <type> <value> ---- */
static void saveFrozen(void)
{
    char key[16], text[64], value[32];
    int i;
    for (i = 0; i < MAX_FROZEN; ++i)
    {
        snprintf(key, sizeof(key), "freeze%d", i + 1);
        if (i < frozenCount)
        {
            formatValue(value, sizeof(value), frozen[i].value, frozen[i].type);
            snprintf(text, sizeof(text), "0x%08X %d %s %d", frozen[i].address, frozen[i].type, value, frozen[i].on);
            api->ini_set(key, text);
        }
        else if (api->ini_get(key, NULL))
        {
            api->ini_set(key, "");
        }
    }
}

static void loadFrozen(void)
{
    char key[16];
    int i;
    for (i = 0; i < MAX_FROZEN; ++i)
    {
        unsigned int address;
        int t, on = 1;
        double value;
        const char *text;
        snprintf(key, sizeof(key), "freeze%d", i + 1);
        text = api->ini_get(key, "");
        if (sscanf(text, "%x %d %lf %d", &address, &t, &value, &on) >= 3 && t >= 0 && t < T_COUNT)
        {
            Frozen *f = &frozen[frozenCount++];
            f->address = address;
            f->type = t;
            f->value = value;
            f->on = on;
        }
    }
}

static void freeze(uint32_t address, double value)
{
    int i;
    for (i = 0; i < frozenCount; ++i)
    {
        if (frozen[i].address == address)
        {
            frozen[i].value = value;
            frozen[i].on = 1;
            saveFrozen();
            return;
        }
    }
    if (frozenCount == MAX_FROZEN)
    {
        snprintf(status, sizeof(status), "%d values are frozen already: remove one first.", MAX_FROZEN);
        return;
    }
    frozen[frozenCount].address = address;
    frozen[frozenCount].type = type;
    frozen[frozenCount].value = value;
    frozen[frozenCount].on = 1;
    frozenCount++;
    saveFrozen();
}

/* ---- the mod ---- */
S2R_EXPORT int S2R_ModInit(const S2RModApi *modApi)
{
    api = modApi;
    if (api->version < 1)
        return 1;
    loadFrozen();
    if (frozenCount)
        api->log("%d frozen value%s from mod.ini", frozenCount, frozenCount == 1 ? "" : "s");
    return 0;
}

S2R_EXPORT void S2R_ModFrame(void)
{
    int i;
    for (i = 0; i < frozenCount; ++i)
        if (frozen[i].on)
            writeValue(frozen[i].address, frozen[i].type, frozen[i].value);
}

S2R_EXPORT void S2R_ModMenu(const S2RUi *ui)
{
    char label[96], value[32];
    uint32_t i, shown;
    int t;

    ui->text("Find a value");
    for (t = 0; t < T_COUNT; ++t)
    {
        if (t)
            ui->same_line();
        snprintf(label, sizeof(label), "%s##type", typeNames[t]);
        if (ui->selectable(label, type == t) && type != t)
        {
            type = t;
            searching = everyAddress = 0;
            resultCount = 0;
            status[0] = 0;
        }
    }
    ui->input_text("Value", valueText, sizeof(valueText));
    if (ui->button("New search"))
        search(CMP_EQUAL, 1);
    ui->same_line();
    if (ui->button("Unknown value"))
        searchUnknown();
    if (searching)
    {
        if (ui->button("Next search"))
            search(CMP_EQUAL, 0);
        ui->same_line();
        if (ui->button("Changed"))
            search(CMP_CHANGED, 0);
        ui->same_line();
        if (ui->button("Unchanged"))
            search(CMP_UNCHANGED, 0);
        ui->same_line();
        if (ui->button("Increased"))
            search(CMP_INCREASED, 0);
        ui->same_line();
        if (ui->button("Decreased"))
            search(CMP_DECREASED, 0);
    }
    if (status[0])
        ui->text_disabled("%s", status);

    shown = resultCount < SHOWN_RESULTS ? resultCount : SHOWN_RESULTS;
    if (searching && !everyAddress && shown < resultCount)
        ui->text_disabled("Showing the first %u. Search again to narrow the list down.", shown);
    for (i = 0; i < shown; ++i)
    {
        const uint32_t address = results[i];
        formatValue(value, sizeof(value), readValue(api->ram, address, type), type);
        ui->push_id((int)i);
        ui->text("%08X  =  %s", address, value);
        ui->same_line();
        ui->input_text("##set", setText[i], sizeof(setText[i]));
        ui->same_line();
        if (ui->button("Set") && setText[i][0])
            writeValue(address, type, atof(setText[i]));
        ui->same_line();
        if (ui->button("Freeze"))
            freeze(address, setText[i][0] ? atof(setText[i]) : readValue(api->ram, address, type));
        ui->pop_id();
    }

    ui->separator();
    ui->text("Frozen values (written every frame, kept in mod.ini)");
    if (!frozenCount)
        ui->text_disabled("None yet: Freeze a search result.");
    for (t = 0; t < frozenCount; ++t)
    {
        Frozen *f = &frozen[t];
        formatValue(value, sizeof(value), f->value, f->type);
        snprintf(label, sizeof(label), "%08X (%s) = %s", f->address, typeNames[f->type], value);
        ui->push_id(1000 + t);
        if (ui->checkbox(label, &f->on))
            saveFrozen();
        ui->same_line();
        if (ui->button("Remove"))
        {
            memmove(&frozen[t], &frozen[t + 1], (size_t)(frozenCount - t - 1) * sizeof(Frozen));
            frozenCount--;
            saveFrozen();
            ui->pop_id();
            break;
        }
        ui->pop_id();
    }
}

S2R_EXPORT void S2R_ModShutdown(void)
{
    free(results);
    free(snapshot);
    results = NULL;
    snapshot = NULL;
}
