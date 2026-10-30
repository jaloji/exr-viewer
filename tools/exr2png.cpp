// exr2png: prints EXR header info and writes the image as PNG.
// Usage: exr2png <in.exr> [out.png]
#include "exr.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

uint32_t crc32(const uint8_t* p, size_t n, uint32_t c = 0xFFFFFFFFu) {
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return c;
}

void be32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(uint8_t(x >> 24)); v.push_back(uint8_t(x >> 16)); v.push_back(uint8_t(x >> 8)); v.push_back(uint8_t(x));
}

void chunk(std::vector<uint8_t>& png, const char* type, const std::vector<uint8_t>& body) {
    be32(png, uint32_t(body.size()));
    size_t start = png.size();
    png.insert(png.end(), type, type + 4);
    png.insert(png.end(), body.begin(), body.end());
    be32(png, crc32(&png[start], png.size() - start) ^ 0xFFFFFFFFu);
}

// RGBA8 PNG using stored (uncompressed) deflate blocks: no zlib needed.
bool writePng(const char* path, const exr::Image& img) {
    std::vector<uint8_t> raw;
    raw.reserve((size_t(img.width) * 4 + 1) * img.height);
    for (uint32_t y = 0; y < img.height; ++y) {
        raw.push_back(0);
        const uint8_t* row = &img.bgra[size_t(y) * img.width * 4];
        for (uint32_t x = 0; x < img.width; ++x) {
            raw.push_back(row[x * 4 + 2]); raw.push_back(row[x * 4 + 1]);
            raw.push_back(row[x * 4 + 0]); raw.push_back(row[x * 4 + 3]);
        }
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    for (size_t off = 0; off < raw.size(); off += 65535) {
        size_t n = raw.size() - off < 65535 ? raw.size() - off : 65535;
        z.push_back(off + n >= raw.size() ? 1 : 0);
        z.push_back(uint8_t(n)); z.push_back(uint8_t(n >> 8));
        z.push_back(uint8_t(~n)); z.push_back(uint8_t(~n >> 8));
        z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
    }
    be32(z, (b << 16) | a);

    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> ihdr;
    be32(ihdr, img.width); be32(ihdr, img.height);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});

    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    bool ok = std::fwrite(png.data(), 1, png.size(), f) == png.size();
    return std::fclose(f) == 0 && ok;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: exr2png <in.exr> [out.png]\n");
        return 2;
    }
    std::ifstream file(argv[1], std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (data.empty()) {
        std::fprintf(stderr, "%s: cannot read\n", argv[1]);
        return 1;
    }

    exr::Info info;
    exr::Image img;
    std::string err;
    if (!exr::parse(data.data(), data.size(), info, &err) || !exr::decode(data.data(), data.size(), info, img, &err)) {
        std::fprintf(stderr, "%s: %s\n", argv[1], err.c_str());
        return 1;
    }
    std::printf("%s: %ux%u, %s, %s", argv[1], info.width, info.height, exr::describe(info).c_str(),
                img.hdr ? "HDR" : "SDR");
    if (img.hdr) std::printf(" (white %.2f)", img.white);
    std::printf("%s%s\n", img.converted ? ", primaries converted to sRGB" : "",
                img.straightAlpha ? ", straight alpha" : "");
    if (argc >= 3 && !writePng(argv[2], img)) {
        std::fprintf(stderr, "%s: cannot write\n", argv[2]);
        return 1;
    }
    return 0;
}
