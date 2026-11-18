// A PNG writer with no library: stored (uncompressed) deflate blocks, so pictures are big but need nothing to write.
#include "emu.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace emu {
namespace {
uint32_t crc(const unsigned char* p, size_t n, uint32_t c = 0xFFFFFFFFu) {
    static uint32_t t[256];
    static bool ready = false;
    if (!ready) { for (uint32_t i = 0; i < 256; ++i) { uint32_t k = i; for (int j = 0; j < 8; ++j) k = (k & 1) ? 0xEDB88320u ^ (k >> 1) : k >> 1; t[i] = k; } ready = true; }
    for (size_t i = 0; i < n; ++i) c = t[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c;
}
void be32(std::vector<unsigned char>& v, uint32_t x) { for (int i = 3; i >= 0; --i) v.push_back((unsigned char)(x >> (8 * i))); }
void chunk(std::vector<unsigned char>& png, const char* type, const std::vector<unsigned char>& data) {
    be32(png, (uint32_t)data.size());
    std::vector<unsigned char> td(type, type + 4);
    td.insert(td.end(), data.begin(), data.end());
    png.insert(png.end(), td.begin(), td.end());
    be32(png, crc(td.data(), td.size()) ^ 0xFFFFFFFFu);
}
} // namespace

bool writePng(const std::string& path, const unsigned char* rgba, int w, int h) {
    std::vector<unsigned char> raw;
    raw.reserve((size_t)h * (w * 3 + 1));
    for (int y = h - 1; y >= 0; --y) {   // GL's rows go up
        raw.push_back(0);
        const unsigned char* row = rgba + (size_t)y * w * 4;
        for (int x = 0; x < w; ++x) { raw.push_back(row[x * 4]); raw.push_back(row[x * 4 + 1]); raw.push_back(row[x * 4 + 2]); }
    }
    std::vector<unsigned char> z = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (unsigned char c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    for (size_t i = 0; i < raw.size(); i += 65535) {
        const size_t n = std::min<size_t>(65535, raw.size() - i);
        z.push_back(i + n >= raw.size() ? 1 : 0);
        z.push_back((unsigned char)(n & 0xFF)); z.push_back((unsigned char)(n >> 8));
        z.push_back((unsigned char)(~n & 0xFF)); z.push_back((unsigned char)((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + i, raw.begin() + i + n);
    }
    be32(z, (b << 16) | a);
    std::vector<unsigned char> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'}, ihdr;
    be32(ihdr, (uint32_t)w); be32(ihdr, (uint32_t)h);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fwrite(png.data(), 1, png.size(), f);
    std::fclose(f);
    return true;
}

} // namespace emu
