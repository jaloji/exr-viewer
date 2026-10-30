// OpenEXR reader: the contract shared by the WIC codec and the tools.
// parse() checks the header itself; decode() uses the OpenEXR library, then converts the linear
// scene values to 8-bit sRGB (tone mapping HDR images).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace exr {

struct Info {
    uint32_t width = 0;   // display window size
    uint32_t height = 0;
    std::string compression;
    std::string channels;  // channel names of the decoded layer, e.g. "R,G,B,A"
    std::string layer;     // "" for the default layer, else the layer that is shown ("ViewLayer.Combined")
    bool tiled = false;
    bool multipart = false;
    int part = 0;            // part shown: the first one with colour or luminance
    bool deep = false;       // deep samples, composited front to back into a flat image
};

struct Image {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> bgra;  // top-down, straight alpha, stride = width * 4
    bool hdr = false;           // true when tone mapping was applied
    float white = 1.0f;         // level mapped to white (99.9th percentile of the brightest channel for HDR)
    bool converted = false;     // true when the file's chromaticities were converted to sRGB primaries
    bool straightAlpha = false; // true when the file stores straight alpha despite the specification
};

// Reads and fully validates the header. Rejects any file that is not a displayable OpenEXR image
// (no colour or luminance channel).
bool parse(const uint8_t* data, size_t size, Info& info, std::string* error = nullptr);

// Decodes the full image to 8-bit sRGB BGRA.
bool decode(const uint8_t* data, size_t size, const Info& info, Image& out, std::string* error = nullptr);

// Short description for the tools, e.g. "DWAB, R,G,B, scanlines".
std::string describe(const Info& info);

}  // namespace exr
