#pragma once
// High-resolution texture pictures (gs_texture_replace.cpp), drawn by the GPU GS renderer in place of a
// texture whose upload matches. The CPU renderer and GS memory keep the original (or .bin replacement).
#include <cstddef>
#include <cstdint>
#include <vector>

struct PS2XTexHd
{
    uint64_t tex = 0;               // hash of the texture upload it stands for
    uint64_t clut = 0;              // hash of the CLUT upload it is drawn with, 0 = any CLUT
    uint32_t w = 0, h = 0, psm = 0; // that upload's size and format (the picture covers w x h texels)
    int pw = 0, ph = 0;             // picture size
    std::vector<uint8_t> rgba;      // row 0 = texel row 0; alpha in GS units (0x80 = opaque)
};

// Every picture of the enabled packs (loaded once).
const std::vector<PS2XTexHd> &ps2xTexHdPictures();
// An upload of this shape could be one the pictures name (texture or CLUT).
bool ps2xTexHdTracks(uint32_t w, uint32_t h, uint32_t psm, uint32_t bytes);
// The upload's bytes, as they land in GS memory: false if no picture names them, else the key pictures
// use (a .bin replacement's bytes map to the hash of the upload it replaces).
bool ps2xTexHdKey(const uint8_t *data, size_t size, uint64_t &key);
