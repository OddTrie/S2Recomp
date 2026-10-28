// s2r_setup: copies the game files out of the player's own disc image for S2Recomp.
//
//   s2r_setup <disc.iso | disc folder> <game folder>
//   s2r_setup --check <disc.iso | disc folder>      (only checks the disc)
//
// The source is a 2048-byte-sector ISO 9660 image, or a folder (a mounted disc or files already
// copied off one). The disc is checked first: SLES_537.18 must match the build S2Recomp was
// recompiled from, because the recompiled code is tied to that exact executable.
// Files already in the game folder that are not on the disc (the mc0/mc1 memory cards) are kept.
//
// Prints "PROGRESS <0-100>" lines while copying. Exit codes: 0 done, 1 usage or I/O error,
// 2 not a readable ISO 9660 image, 3 not the supported disc.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    constexpr const char *kElfName = "SLES_537.18";
    constexpr const char *kElfSha1 = "6859ad895ca4442bb8b87ced927f1dcdfe16bdb8";
    constexpr uint32_t kSector = 2048;

    class Sha1
    {
    public:
        void update(const uint8_t *data, size_t len)
        {
            total_ += len;
            while (len > 0)
            {
                const size_t n = std::min(len, sizeof(buf_) - used_);
                std::memcpy(buf_ + used_, data, n);
                used_ += n;
                data += n;
                len -= n;
                if (used_ == sizeof(buf_))
                {
                    block(buf_);
                    used_ = 0;
                }
            }
        }

        std::string hex()
        {
            const uint64_t bits = total_ * 8;
            const uint8_t pad = 0x80;
            update(&pad, 1);
            const uint8_t zero = 0;
            while (used_ != 56)
                update(&zero, 1);
            uint8_t len[8];
            for (int i = 0; i < 8; ++i)
                len[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
            update(len, 8);
            char out[41];
            for (int i = 0; i < 5; ++i)
                std::snprintf(out + i * 8, 9, "%08x", h_[i]);
            return out;
        }

    private:
        static uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

        void block(const uint8_t *p)
        {
            uint32_t w[80];
            for (int i = 0; i < 16; ++i)
                w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) | (uint32_t(p[i * 4 + 2]) << 8) | p[i * 4 + 3];
            for (int i = 16; i < 80; ++i)
                w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
            uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
            for (int i = 0; i < 80; ++i)
            {
                uint32_t f, k;
                if (i < 20)
                    f = (b & c) | (~b & d), k = 0x5A827999;
                else if (i < 40)
                    f = b ^ c ^ d, k = 0x6ED9EBA1;
                else if (i < 60)
                    f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
                else
                    f = b ^ c ^ d, k = 0xCA62C1D6;
                const uint32_t t = rol(a, 5) + f + e + k + w[i];
                e = d, d = c, c = rol(b, 30), b = a, a = t;
            }
            h_[0] += a, h_[1] += b, h_[2] += c, h_[3] += d, h_[4] += e;
        }

        uint32_t h_[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
        uint8_t buf_[64];
        size_t used_ = 0;
        uint64_t total_ = 0;
    };

    struct Entry
    {
        std::string path; // relative, '/'-separated
        bool dir = false;
        uint64_t offset = 0; // byte offset in the image (ISO) ...
        uint64_t size = 0;
        fs::path source; // ... or the file on disk (folder source)
    };

    uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }

    // Disc names are plain ISO 9660 ("NAME.EXT;1"). Anything that could leave the game folder is refused.
    bool cleanName(std::string &name)
    {
        const size_t semi = name.find(';');
        if (semi != std::string::npos)
            name.resize(semi);
        if (!name.empty() && name.back() == '.')
            name.pop_back();
        if (name.empty() || name == "." || name == "..")
            return false;
        for (char c : name)
            if (c == '/' || c == '\\' || c == ':' || static_cast<unsigned char>(c) < 0x20)
                return false;
        return true;
    }

    class IsoReader
    {
    public:
        bool open(const fs::path &path)
        {
            in_.open(path, std::ios::binary);
            if (!in_)
                return false;
            uint8_t pvd[kSector];
            if (!read(16ull * kSector, pvd, kSector))
                return false;
            if (pvd[0] != 1 || std::memcmp(pvd + 1, "CD001", 5) != 0)
                return false;
            if ((pvd[128] | (pvd[129] << 8)) != kSector)
                return false;
            const uint8_t *root = pvd + 156;
            return walk(le32(root + 2), le32(root + 10), "", 0);
        }

        bool read(uint64_t offset, void *dst, size_t len)
        {
            in_.clear();
            in_.seekg(static_cast<std::streamoff>(offset));
            in_.read(static_cast<char *>(dst), static_cast<std::streamsize>(len));
            return static_cast<size_t>(in_.gcount()) == len;
        }

        std::vector<Entry> entries;

    private:
        bool walk(uint32_t lba, uint32_t size, const std::string &prefix, int depth)
        {
            if (depth > 32 || size > 64u * 1024 * 1024)
                return false;
            std::vector<uint8_t> data(size);
            if (!read(uint64_t(lba) * kSector, data.data(), size))
                return false;
            for (uint32_t pos = 0; pos < size;)
            {
                const uint8_t len = data[pos];
                if (len == 0)
                {
                    pos = (pos / kSector + 1) * kSector; // records never cross a sector; the rest is padding
                    continue;
                }
                if (len < 34 || pos + len > size)
                    return false;
                const uint8_t *rec = data.data() + pos;
                const uint8_t nameLen = rec[32];
                if (33u + nameLen > len)
                    return false;
                std::string name(reinterpret_cast<const char *>(rec + 33), nameLen);
                pos += len;
                if (nameLen == 1 && (name[0] == 0 || name[0] == 1))
                    continue;
                if (!cleanName(name))
                    return false;
                Entry e;
                e.path = prefix.empty() ? name : prefix + "/" + name;
                e.dir = (rec[25] & 0x02) != 0;
                e.offset = uint64_t(le32(rec + 2)) * kSector;
                e.size = le32(rec + 10);
                entries.push_back(e);
                if (e.dir && !walk(le32(rec + 2), le32(rec + 10), e.path, depth + 1))
                    return false;
            }
            return true;
        }

        std::ifstream in_;
    };

    bool listFolder(const fs::path &root, std::vector<Entry> &out)
    {
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        {
            Entry e;
            e.path = fs::relative(it->path(), root, ec).generic_string();
            e.dir = it->is_directory(ec);
            e.source = it->path();
            if (!e.dir)
                e.size = it->file_size(ec);
            out.push_back(e);
        }
        return !ec;
    }

    // Reads a file's bytes from either source in chunks.
    template <typename Sink>
    bool readEntry(IsoReader *iso, const Entry &e, Sink &&sink)
    {
        std::vector<uint8_t> buf(4u * 1024 * 1024);
        std::ifstream file;
        if (!iso)
        {
            file.open(e.source, std::ios::binary);
            if (!file)
                return false;
        }
        for (uint64_t done = 0; done < e.size;)
        {
            const size_t n = static_cast<size_t>(std::min<uint64_t>(buf.size(), e.size - done));
            if (iso)
            {
                if (!iso->read(e.offset + done, buf.data(), n))
                    return false;
            }
            else
            {
                file.read(reinterpret_cast<char *>(buf.data()), static_cast<std::streamsize>(n));
                if (static_cast<size_t>(file.gcount()) != n)
                    return false;
            }
            if (!sink(buf.data(), n))
                return false;
            done += n;
        }
        return true;
    }
}

int main(int argc, char **argv)
{
    const bool checkOnly = argc == 3 && std::strcmp(argv[1], "--check") == 0;
    if (argc != 3)
    {
        std::cerr << "usage: s2r_setup <disc.iso | disc folder> <game folder>\n"
                     "       s2r_setup --check <disc.iso | disc folder>\n";
        return 1;
    }
    const fs::path source = fs::u8path(argv[checkOnly ? 2 : 1]);
    const fs::path dest = checkOnly ? fs::path() : fs::u8path(argv[2]);

    IsoReader isoReader;
    IsoReader *iso = nullptr;
    std::vector<Entry> entries;
    std::error_code ec;
    if (fs::is_directory(source, ec))
    {
        if (fs::equivalent(source, dest, ec))
        {
            std::cerr << "The disc folder and the game folder are the same folder.\n";
            return 1;
        }
        if (!listFolder(source, entries))
        {
            std::cerr << "Could not read the folder " << source.u8string() << "\n";
            return 1;
        }
    }
    else
    {
        if (!isoReader.open(source))
        {
            std::cerr << "This is not a readable PS2 disc image (ISO 9660, 2048-byte sectors): " << source.u8string() << "\n";
            return 2;
        }
        iso = &isoReader;
        entries = std::move(isoReader.entries);
    }

    const Entry *elf = nullptr;
    uint64_t total = 0;
    for (const Entry &e : entries)
    {
        total += e.size;
        if (!e.dir && e.path == kElfName)
            elf = &e;
    }
    if (!elf)
    {
        std::cerr << "This disc has no " << kElfName << ". S2Recomp needs The Sims 2 for PlayStation 2, PAL (Europe) release.\n";
        return 3;
    }
    Sha1 sha;
    if (!readEntry(iso, *elf, [&](const uint8_t *p, size_t n) { sha.update(p, n); return true; }))
    {
        std::cerr << "Could not read " << kElfName << " from the disc.\n";
        return 1;
    }
    if (sha.hex() != kElfSha1)
    {
        std::cerr << kElfName << " on this disc is a different version than the one S2Recomp supports.\n";
        return 3;
    }
    std::cout << "Disc OK: The Sims 2 (Europe, " << kElfName << ")\n";
    if (checkOnly)
        return 0;

    fs::create_directories(dest, ec);
    if (ec)
    {
        std::cerr << "Could not create " << dest.u8string() << ": " << ec.message() << "\n";
        return 1;
    }

    uint64_t copied = 0;
    int lastPct = -1;
    for (const Entry &e : entries)
    {
        const fs::path target = dest / fs::u8path(e.path);
        if (e.dir)
        {
            fs::create_directories(target, ec);
            if (ec)
            {
                std::cerr << "Could not create " << target.u8string() << ": " << ec.message() << "\n";
                return 1;
            }
            continue;
        }
        fs::create_directories(target.parent_path(), ec);
        const fs::path partial = target.u8string() + ".part";
        {
            std::ofstream out(partial, std::ios::binary | std::ios::trunc);
            if (!out)
            {
                std::cerr << "Could not write " << target.u8string() << "\n";
                return 1;
            }
            const bool ok = readEntry(iso, e, [&](const uint8_t *p, size_t n) {
                out.write(reinterpret_cast<const char *>(p), static_cast<std::streamsize>(n));
                copied += n;
                const int pct = total ? static_cast<int>(copied * 100 / total) : 100;
                if (pct != lastPct)
                {
                    std::cout << "PROGRESS " << pct << std::endl;
                    lastPct = pct;
                }
                return static_cast<bool>(out);
            });
            if (!ok)
            {
                std::cerr << "Copying " << e.path << " failed (disc image truncated or disk full?)\n";
                out.close();
                fs::remove(partial, ec);
                return 1;
            }
        }
        fs::rename(partial, target, ec);
        if (ec)
        {
            std::cerr << "Could not write " << target.u8string() << ": " << ec.message() << "\n";
            return 1;
        }
    }
    fs::create_directories(dest / "mc0", ec);
    fs::create_directories(dest / "mc1", ec);
    std::cout << "PROGRESS 100" << std::endl;
    std::cout << "Game files ready in " << dest.u8string() << "\n";
    return 0;
}
