// Texture replacement by upload content: a host-to-local GS transfer whose bytes hash to a file in
// the texture pack is uploaded again with that file's bytes (same size and format), right after the
// original. Used to swap the game's PS2 button icons for keyboard keys or Xbox buttons.
//
// Packs: PS2X_TEXPACK=<dir> (default "texpacks/kbm" next to the executable, "0" disables) applies only
// while the keycap icons are shown (keyboard and mouse in use), PS2X_TEXPACK_XBOX=<dir> (default
// "texpacks/xbox") only while the Xbox icons are (a non-PlayStation controller in use; ps2_pad.cpp picks
// the icons, PS2X_GLYPHS pins them); "texpacks/branding" (the S2Recomp logo,
// PS2X_BRANDING=0 disables) and each enabled mod's textures/ folder (ps2_mod_loader.h) always apply. Files are named <hash>_<w>x<h>_<psm>.bin and hold the
// raw transfer bytes, as the dump writes them.
// High-resolution pictures (GPU renderer only; PS2X_TEXPACK_HD=0 disables): <hash>_<w>x<h>_<psm>.png in a
// pack, or <hash>_<w>x<h>_<psm>_<cluthash>.png for one CLUT only, any size, row 0 = texel row 0 (Sims 2's UI
// textures are stored upside down), alpha 255 = opaque (0x80). Named after the upload as the dump names it;
// when a .bin replaces that upload, the picture stands for the replacement (so kbm pictures only show with
// the keycaps). The GPU renderer samples it instead of GS memory (gs_texture_hd.h).
// Dump: PS2X_TEX_DUMP=<dir> writes every distinct upload in that form, for making pack files, and
// pairs.txt: "<texture hash> <clut hash>" for each texture drawn with a CLUT (the uploads last
// written at TEX0's TBP0 and CBP).
#include "runtime/gs/gs_types.h"
#include "runtime/gs/ps2_gs_common.h"
#include "runtime/gs/gs_texture_hd.h"
#include "runtime/ps2_mod_loader.h"

#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <algorithm>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

extern std::atomic<int> g_ps2xGlyphSet; // ps2_pad.cpp: 0 the game's icons, 1 keycaps (kbm pack), 2 xbox pack
bool ps2xLoadPngRgba(const std::string &path, int &w, int &h, std::vector<uint8_t> &rgba); // gs_texture_hd_png.cpp

namespace
{
    constexpr uint64_t kXboxKeySalt = 0x5842ull << 48; // "XB": high-resolution keys of the xbox pack

    uint32_t transferBytes(const GSTransferCommand &command)
    {
        const uint8_t psm = command.bitbltbuf.dpsm;
        const uint32_t bits = (psm == GS_PSM_CT24 || psm == GS_PSM_Z24) ? 24u : GSInternal::bitsPerPixel(psm);
        const uint64_t pixels = static_cast<uint64_t>(command.trxreg.rrw) * command.trxreg.rrh;
        return static_cast<uint32_t>((pixels * bits + 7u) / 8u);
    }

    uint64_t hashBytes(const uint8_t *data, size_t size)
    {
        uint64_t h = 0xcbf29ce484222325ull;
        for (size_t i = 0; i < size; ++i)
            h = (h ^ data[i]) * 0x100000001b3ull;
        return h;
    }

    std::string shapeKey(uint32_t w, uint32_t h, uint32_t psm)
    {
        return std::to_string(w) + "x" + std::to_string(h) + "_" + std::to_string(psm);
    }

    std::filesystem::path exeDir()
    {
#if defined(_WIN32)
        wchar_t buf[MAX_PATH];
        const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
        if (n > 0 && n < MAX_PATH)
            return std::filesystem::path(buf).parent_path();
#endif
        return std::filesystem::current_path();
    }

    struct Pack
    {
        std::unordered_set<std::string> shapes; // transfer shapes that have a replacement
        std::unordered_map<uint64_t, std::vector<uint8_t>> files;
        std::unordered_map<uint64_t, std::vector<uint8_t>> glyphFiles[2]; // button icon packs: kbm, xbox
        std::vector<PS2XTexHd> hd;
        std::unordered_map<uint64_t, uint64_t> alias; // hash of a replacement file's bytes -> the upload it replaces
        std::unordered_set<uint64_t> hdKeys;          // textures and CLUTs pictures name
        std::unordered_set<std::string> hdShapes;
        std::filesystem::path dumpDir;
        std::set<uint64_t> dumped;
        std::unordered_map<uint32_t, uint64_t> uploadAt; // dump: hash of the last upload by base pointer
        std::set<std::pair<uint64_t, uint64_t>> pairs;
        std::FILE *pairsFile = nullptr;
    };

    Pack &pack()
    {
        static Pack p = []
        {
            Pack out;
            std::filesystem::path kbmDir = exeDir() / "texpacks" / "kbm";
            if (const char *e = std::getenv("PS2X_TEXPACK"); e && *e)
                kbmDir = (e[0] == '0' && e[1] == 0) ? std::filesystem::path() : std::filesystem::path(e);
            std::filesystem::path brandingDir = exeDir() / "texpacks" / "branding";
            if (const char *e = std::getenv("PS2X_BRANDING"); e && e[0] == '0' && e[1] == 0)
                brandingDir.clear();
            std::filesystem::path xboxDir = exeDir() / "texpacks" / "xbox";
            if (const char *e = std::getenv("PS2X_TEXPACK_XBOX"); e && *e)
                xboxDir = (e[0] == '0' && e[1] == 0) ? std::filesystem::path() : std::filesystem::path(e);
            std::error_code ec;
            const char *hdEnv = std::getenv("PS2X_TEXPACK_HD");
            const bool hdOn = !(hdEnv && hdEnv[0] == '0' && hdEnv[1] == 0);
            // Second: the icon set the pack belongs to (g_ps2xGlyphSet), 0 = always applies.
            std::vector<std::pair<std::filesystem::path, int>> dirs = {{kbmDir, 1}, {xboxDir, 2}, {brandingDir, 0}};
            for (const std::filesystem::path &modDir : ps2x::mods::textureDirs())
                dirs.emplace_back(modDir, 0);
            for (const auto &[dir, glyphSet] : dirs)
            {
                if (dir.empty() || !std::filesystem::is_directory(dir, ec))
                    continue;
                auto &files = glyphSet ? out.glyphFiles[glyphSet - 1] : out.files;
                // Both icon packs replace the same uploads: the xbox pack's pictures get keys of their own.
                const uint64_t salt = glyphSet == 2 ? kXboxKeySalt : 0u;
                const size_t before = files.size(), hdBefore = out.hd.size();
                for (const auto &entry : std::filesystem::directory_iterator(dir, ec))
                {
                    const std::string name = entry.path().filename().string();
                    unsigned long long hash = 0, clut = 0;
                    unsigned w = 0, h = 0, psm = 0;
                    if (entry.path().extension() == ".png" && hdOn)
                    {
                        const int n = std::sscanf(name.c_str(), "%llx_%ux%u_%u_%llx", &hash, &w, &h, &psm, &clut);
                        PS2XTexHd pic;
                        if (n < 4 || w == 0 || h == 0 || !ps2xLoadPngRgba(entry.path().string(), pic.pw, pic.ph, pic.rgba))
                        {
                            std::fprintf(stderr, "[texpack] %s: not a <hash>_<w>x<h>_<psm>[_<cluthash>].png picture; skipped\n", name.c_str());
                            continue;
                        }
                        for (size_t i = 3; i < pic.rgba.size(); i += 4)
                            pic.rgba[i] = static_cast<uint8_t>((pic.rgba[i] * 128u + 127u) / 255u);
                        pic.tex = hash ^ salt;
                        pic.clut = n == 5 ? clut ^ salt : 0u;
                        pic.w = w;
                        pic.h = h;
                        pic.psm = psm;
                        out.hdKeys.insert(pic.tex);
                        if (pic.clut)
                            out.hdKeys.insert(pic.clut);
                        out.hdShapes.insert(shapeKey(w, h, psm));
                        out.hd.push_back(std::move(pic));
                        continue;
                    }
                    if (entry.path().extension() != ".bin" ||
                        std::sscanf(name.c_str(), "%llx_%ux%u_%u", &hash, &w, &h, &psm) != 4)
                        continue;
                    std::ifstream in(entry.path(), std::ios::binary);
                    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                    GSTransferCommand c{};
                    c.bitbltbuf.dpsm = static_cast<uint8_t>(psm);
                    c.trxreg.rrw = static_cast<uint16_t>(w);
                    c.trxreg.rrh = static_cast<uint16_t>(h);
                    if (bytes.size() != transferBytes(c))
                    {
                        std::fprintf(stderr, "[texpack] %s: %zu bytes, expected %u; skipped\n", name.c_str(), bytes.size(), transferBytes(c));
                        continue;
                    }
                    out.shapes.insert(shapeKey(w, h, psm));
                    out.alias[hashBytes(bytes.data(), bytes.size())] = hash ^ salt;
                    files[hash] = std::move(bytes);
                }
                std::fprintf(stderr, "[texpack] %zu replacement textures from %s\n", files.size() - before, dir.string().c_str());
                if (out.hd.size() != hdBefore)
                    std::fprintf(stderr, "[texpack] %zu high-resolution pictures from %s (GPU renderer)\n", out.hd.size() - hdBefore, dir.string().c_str());
            }
            if (const char *e = std::getenv("PS2X_TEX_DUMP"); e && *e)
            {
                out.dumpDir = e;
                std::filesystem::create_directories(out.dumpDir, ec);
                out.pairsFile = std::fopen((out.dumpDir / "pairs.txt").string().c_str(), "a");
            }
            return out;
        }();
        return p;
    }

    // The bytes an upload is replaced with now: a pack that always applies (branding, mods) wins over
    // the button icon pack of the input in use.
    const std::vector<uint8_t> *replacementFor(const Pack &p, uint64_t hash)
    {
        if (const auto it = p.files.find(hash); it != p.files.end())
            return &it->second;
        const int set = g_ps2xGlyphSet.load(std::memory_order_relaxed);
        if (set < 1 || set > 2)
            return nullptr;
        const auto &glyphs = p.glyphFiles[set - 1];
        const auto it = glyphs.find(hash);
        return it == glyphs.end() ? nullptr : &it->second;
    }

    // The transfer being captured (the GS frontend calls these under its state lock).
    bool g_capturing = false;
    uint32_t g_dbp = 0;
    uint32_t g_expected = 0;
    std::string g_shape;
    std::vector<uint8_t> g_captured;
}

// Starts capturing a host-to-local transfer's bytes when its shape has a replacement (or dumping is on).
void ps2xTexReplaceBegin(const GSTransferCommand &command)
{
    g_capturing = false;
    if (command.direction != 0u)
        return;
    Pack &p = pack();
    if (p.files.empty() && p.glyphFiles[0].empty() && p.glyphFiles[1].empty() && p.dumpDir.empty())
        return;
    const std::string shape = shapeKey(command.trxreg.rrw, command.trxreg.rrh, command.bitbltbuf.dpsm);
    if (p.dumpDir.empty() && p.shapes.count(shape) == 0u)
        return;
    g_expected = transferBytes(command);
    if (g_expected == 0u || g_expected > 4u * 1024u * 1024u)
        return;
    g_shape = shape;
    g_dbp = command.bitbltbuf.dbp;
    g_captured.clear();
    g_captured.reserve(g_expected);
    g_capturing = true;
}

// Adds uploaded bytes; when the transfer is complete, returns its replacement bytes or nullptr.
const std::vector<uint8_t> *ps2xTexReplaceFeed(const uint8_t *data, uint32_t size)
{
    if (!g_capturing)
        return nullptr;
    const uint32_t take = std::min<uint32_t>(size, g_expected - static_cast<uint32_t>(g_captured.size()));
    g_captured.insert(g_captured.end(), data, data + take);
    if (g_captured.size() < g_expected)
        return nullptr;
    g_capturing = false;

    Pack &p = pack();
    const uint64_t hash = hashBytes(g_captured.data(), g_captured.size());
    if (!p.dumpDir.empty())
        p.uploadAt[g_dbp] = hash;
    if (!p.dumpDir.empty() && p.dumped.insert(hash).second && p.dumped.size() <= 50000u)
    {
        char name[96];
        std::snprintf(name, sizeof(name), "%016llx_%s.bin", static_cast<unsigned long long>(hash), g_shape.c_str());
        std::ofstream(p.dumpDir / name, std::ios::binary).write(reinterpret_cast<const char *>(g_captured.data()), g_captured.size());
    }
    return replacementFor(p, hash);
}

// Dump only: records which CLUT upload each texture upload is drawn with.
void ps2xTexReplaceNoteTex0(uint32_t tbp0, uint32_t cbp)
{
    Pack &p = pack();
    if (!p.pairsFile)
        return;
    const auto tex = p.uploadAt.find(tbp0);
    const auto clut = p.uploadAt.find(cbp);
    if (tex == p.uploadAt.end() || clut == p.uploadAt.end() || !p.pairs.insert({tex->second, clut->second}).second)
        return;
    std::fprintf(p.pairsFile, "%016llx %016llx\n", static_cast<unsigned long long>(tex->second), static_cast<unsigned long long>(clut->second));
    std::fflush(p.pairsFile);
}

const std::vector<PS2XTexHd> &ps2xTexHdPictures()
{
    return pack().hd;
}

bool ps2xTexHdTracks(uint32_t w, uint32_t h, uint32_t psm, uint32_t bytes)
{
    const Pack &p = pack();
    // CLUTs are at most 1 KB: hash every upload that small rather than guess their shapes.
    return !p.hd.empty() && (bytes <= 1024u || p.hdShapes.count(shapeKey(w, h, psm)) != 0u);
}

bool ps2xTexHdKey(const uint8_t *data, size_t size, uint64_t &key)
{
    const Pack &p = pack();
    const uint64_t hash = hashBytes(data, size);
    if (const auto it = p.alias.find(hash); it != p.alias.end())
        key = it->second;
    else if (p.files.count(hash) != 0u || p.glyphFiles[0].count(hash) != 0u || p.glyphFiles[1].count(hash) != 0u)
        return false; // the original of a replaceable upload: pictures stand for the replacements only (not
                      // the game's own icons while those are shown)
    else
        key = hash;
    return p.hdKeys.count(key) != 0u;
}
