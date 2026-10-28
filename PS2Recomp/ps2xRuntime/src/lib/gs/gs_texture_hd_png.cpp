// PNG decoding for the high-resolution texture pictures (gs_texture_replace.cpp), through raylib. Its own
// file: raylib.h and windows.h do not mix.
#include "raylib.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

bool ps2xLoadPngRgba(const std::string &path, int &w, int &h, std::vector<uint8_t> &rgba)
{
    Image img = LoadImage(path.c_str());
    if (!img.data)
        return false;
    ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    w = img.width;
    h = img.height;
    rgba.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
    std::memcpy(rgba.data(), img.data, rgba.size());
    UnloadImage(img);
    return w > 0 && h > 0;
}
