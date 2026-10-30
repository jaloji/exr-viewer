#include "exr.h"

#include <ImfArray.h>
#include <ImfChannelList.h>
#include <ImfChromaticities.h>
#include <ImfDeepFrameBuffer.h>
#include <ImfDeepScanLineInputPart.h>
#include <ImfDeepTiledInputPart.h>
#include <ImfHeader.h>
#include <ImfIO.h>
#include <ImfMultiPartInputFile.h>
#include <ImfPartType.h>
#include <ImfRgbaFile.h>
#include <ImfStandardAttributes.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <map>
#include <memory>
#include <numeric>
#include <set>
#include <stdexcept>

namespace exr {

namespace {

bool fail(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}

const char* const kCompressions[] = {"NONE", "RLE",      "ZIPS",    "ZIP",  "PIZ",  "PXR24", "B44",
                                     "B44A", "DWAA",     "DWAB",    "HTJ2K256", "HTJ2K32", "LJ2K", "ZSTD"};
const size_t kCompressionCount = sizeof(kCompressions) / sizeof(kCompressions[0]);

// Largest image decoded: 2^27 pixels (e.g. 16384 x 8192), 1 GB of half RGBA.
const uint64_t kMaxPixels = uint64_t(1) << 27;
// Largest deep image: 2^25 samples in all (about 800 MB of float channels).
const uint64_t kMaxDeepSamples = uint64_t(1) << 25;

// ---- Header parsing (independent of the library, so that QueryCapability stays cheap) ------

class Reader {
public:
    Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    bool cstring(std::string& out, size_t maxLength) {
        size_t start = pos_;
        while (pos_ < size_ && data_[pos_] != 0) ++pos_;
        if (pos_ >= size_ || pos_ - start > maxLength) return false;
        out.assign(reinterpret_cast<const char*>(data_ + start), pos_ - start);
        ++pos_;
        return true;
    }
    bool i32(int32_t& v) {
        if (size_ - pos_ < 4) return false;
        std::memcpy(&v, data_ + pos_, 4);  // little endian, like every Windows target
        pos_ += 4;
        return true;
    }
    bool bytes(const uint8_t*& p, size_t n) {
        if (size_ - pos_ < n) return false;
        p = data_ + pos_;
        pos_ += n;
        return true;
    }
    bool atEndMarker() const { return pos_ < size_ && data_[pos_] == 0; }
    bool skip(size_t n) {
        if (size_ - pos_ < n) return false;
        pos_ += n;
        return true;
    }

private:
    const uint8_t* data_;
    size_t size_;
    size_t pos_ = 0;
};

int32_t le32(const uint8_t* p) {
    int32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

bool readChannels(const uint8_t* p, size_t n, std::vector<std::string>& names, size_t maxName) {
    Reader r(p, n);
    while (!r.atEndMarker()) {
        std::string name;
        const uint8_t* rest;
        if (!r.cstring(name, maxName) || name.empty() || !r.bytes(rest, 16)) return false;
        int32_t type = le32(rest);
        int32_t xs = le32(rest + 8), ys = le32(rest + 12);
        if (type < 0 || type > 2 || xs < 1 || ys < 1) return false;
        names.push_back(name);
    }
    return !names.empty();
}

// Picks the layer to show: the default one when it has colour or luminance, else the first layer
// that has them, preferring a "Combined" pass (Blender multilayer renders).
bool pickLayer(const std::vector<std::string>& names, std::string& layer, std::string& channels) {
    static const std::set<std::string> kColour = {"R", "G", "B", "Y"};
    std::map<std::string, std::vector<std::string>> layers;
    for (const std::string& n : names) {
        size_t dot = n.rfind('.');
        if (dot == std::string::npos) layers[""].push_back(n);
        else layers[n.substr(0, dot)].push_back(n.substr(dot + 1));
    }
    const std::string* best = nullptr;
    for (const auto& entry : layers) {
        bool colour = std::any_of(entry.second.begin(), entry.second.end(),
                                  [](const std::string& c) { return kColour.count(c) != 0; });
        if (!colour) continue;
        if (entry.first.empty()) {
            best = &entry.first;
            break;
        }
        if (!best || (best->find("Combined") == std::string::npos && entry.first.find("Combined") != std::string::npos))
            best = &entry.first;
    }
    if (!best) return false;
    layer = *best;
    channels.clear();
    for (const std::string& c : layers[layer]) channels += (channels.empty() ? "" : ",") + c;
    return true;
}

// ---- Stream over the file bytes for the library -----------------------------------------------

class MemoryStream : public Imf::IStream {
public:
    MemoryStream(const uint8_t* data, size_t size) : Imf::IStream("memory"), data_(data), size_(size) {}

    bool read(char c[], int n) override {
        if (n < 0 || uint64_t(n) > size_ - std::min<uint64_t>(pos_, size_))
            throw std::runtime_error("unexpected end of file");
        std::memcpy(c, data_ + pos_, size_t(n));
        pos_ += uint64_t(n);
        return pos_ < size_;
    }
    uint64_t tellg() override { return pos_; }
    void seekg(uint64_t pos) override { pos_ = pos; }
    int64_t size() override { return int64_t(size_); }
    bool isStatelessRead() const override { return true; }
    int64_t read(void* buf, uint64_t sz, uint64_t offset) override {
        if (offset >= size_) return 0;
        uint64_t n = std::min<uint64_t>(sz, size_ - offset);
        std::memcpy(buf, data_ + offset, size_t(n));
        return int64_t(n);
    }

private:
    const uint8_t* data_;
    uint64_t size_;
    uint64_t pos_ = 0;
};

// ---- Colour -----------------------------------------------------------------------------------

struct Mat3 {
    float m[3][3];
};

Mat3 mul(const Mat3& a, const Mat3& b) {
    Mat3 r = {};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) r.m[i][j] += a.m[i][k] * b.m[k][j];
    return r;
}

bool invert(const Mat3& a, Mat3& out) {
    const float(*m)[3] = a.m;
    double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                 m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    if (std::fabs(det) < 1e-12) return false;
    double inv = 1.0 / det;
    out.m[0][0] = float((m[1][1] * m[2][2] - m[1][2] * m[2][1]) * inv);
    out.m[0][1] = float((m[0][2] * m[2][1] - m[0][1] * m[2][2]) * inv);
    out.m[0][2] = float((m[0][1] * m[1][2] - m[0][2] * m[1][1]) * inv);
    out.m[1][0] = float((m[1][2] * m[2][0] - m[1][0] * m[2][2]) * inv);
    out.m[1][1] = float((m[0][0] * m[2][2] - m[0][2] * m[2][0]) * inv);
    out.m[1][2] = float((m[0][2] * m[1][0] - m[0][0] * m[1][2]) * inv);
    out.m[2][0] = float((m[1][0] * m[2][1] - m[1][1] * m[2][0]) * inv);
    out.m[2][1] = float((m[0][1] * m[2][0] - m[0][0] * m[2][1]) * inv);
    out.m[2][2] = float((m[0][0] * m[1][1] - m[0][1] * m[1][0]) * inv);
    return true;
}

// RGB -> XYZ matrix of a set of primaries and white point (column vectors: xyz = M * rgb).
// OpenEXR's own computation also accepts primaries with y = 0 (XYZ files) or y < 0 (ACES AP0).
bool rgbToXyz(const Imf::Chromaticities& c, Mat3& out) {
    try {
        const Imath::M44f m = Imf::RGBtoXYZ(c, 1.0f);  // row vectors: xyz = rgb * m
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) out.m[i][j] = m[j][i];
        return true;
    } catch (...) {
        return false;  // degenerate chromaticities
    }
}

// Bradford chromatic adaptation between two white points.
Mat3 bradford(const Imath::V2f& from, const Imath::V2f& to) {
    static const Mat3 kB = {{{0.8951f, 0.2664f, -0.1614f}, {-0.7502f, 1.7135f, 0.0367f}, {0.0389f, -0.0685f, 1.0296f}}};
    Mat3 bi;
    invert(kB, bi);
    auto cone = [](const Imath::V2f& p, float v[3]) {
        float x = p.x / p.y, z = (1.0f - p.x - p.y) / p.y;
        for (int i = 0; i < 3; ++i) v[i] = kB.m[i][0] * x + kB.m[i][1] + kB.m[i][2] * z;
    };
    float s[3], d[3];
    cone(from, s);
    cone(to, d);
    Mat3 scale = {{{d[0] / s[0], 0, 0}, {0, d[1] / s[1], 0}, {0, 0, d[2] / s[2]}}};
    return mul(bi, mul(scale, kB));
}

bool samePrimaries(const Imf::Chromaticities& a, const Imf::Chromaticities& b) {
    auto close = [](const Imath::V2f& p, const Imath::V2f& q) {
        return std::fabs(p.x - q.x) < 1e-3f && std::fabs(p.y - q.y) < 1e-3f;
    };
    return close(a.red, b.red) && close(a.green, b.green) && close(a.blue, b.blue) && close(a.white, b.white);
}

// Linear [0, 1] -> 8-bit sRGB, through a table fine enough for 8-bit output.
struct SrgbTable {
    static const int kSize = 16384;
    uint8_t v[kSize + 1];
    SrgbTable() {
        for (int i = 0; i <= kSize; ++i) {
            double x = double(i) / kSize;
            double s = x <= 0.0031308 ? 12.92 * x : 1.055 * std::pow(x, 1.0 / 2.4) - 0.055;
            v[i] = uint8_t(std::lround(std::clamp(s, 0.0, 1.0) * 255.0));
        }
    }
    uint8_t operator()(float x) const { return v[int(std::clamp(x, 0.0f, 1.0f) * kSize + 0.5f)]; }
};

float finite(float v) {
    if (std::isnan(v)) return 0.0f;
    return std::clamp(v, -65504.0f, 65504.0f);
}

// Luminance histogram on a log2 scale (64 bins per stop, 2^-20 .. 2^20), for percentiles.
class Histogram {
public:
    Histogram() : bins_(size_t(kBins) + 1, 0) {}

    void add(float l) {
        const int bin = l <= std::ldexp(1.0f, kLow) ? 0 : int((std::log2(l) - kLow) * kPerStop) + 1;
        ++bins_[size_t(std::min(bin, kBins))];
        ++count_;
    }
    // Upper edge of the bin holding the requested fraction of the values.
    float percentile(double fraction) const {
        if (count_ == 0) return 0.0f;
        const uint64_t target = uint64_t(std::ceil(double(count_) * fraction));
        uint64_t acc = 0;
        for (int b = 0; b <= kBins; ++b) {
            acc += bins_[size_t(b)];
            if (acc >= target) return b == 0 ? 0.0f : std::exp2(float(kLow) + float(b) / kPerStop);
        }
        return std::exp2(float(kHigh));
    }

private:
    static const int kPerStop = 64, kLow = -20, kHigh = 20, kBins = (kHigh - kLow) * kPerStop;
    std::vector<uint64_t> bins_;
    uint64_t count_ = 0;
};

// ---- Deep images ------------------------------------------------------------------------------

// Reads one part of a deep file and composites the samples of every pixel front to back (sorted by
// Z) into a flat, premultiplied RGBA image. `pixels` already covers `all`, zero-filled.
Imf::RgbaChannels flattenDeep(Imf::MultiPartInputFile& file, int partNumber, const std::string& layer,
                              const Imath::Box2i& all, Imf::Array2D<Imf::Rgba>& pixels) {
    const Imf::Header& header = file.header(partNumber);
    const Imath::Box2i dw = header.dataWindow();
    const int w = dw.max.x - dw.min.x + 1, h = dw.max.y - dw.min.y + 1;
    const std::string prefix = layer.empty() ? "" : layer + ".";
    enum { R, G, B, A, Y, Z, kChannels };
    const char* const names[kChannels] = {"R", "G", "B", "A", "Y", "Z"};
    bool has[kChannels];
    for (int c = 0; c < kChannels; ++c) has[c] = header.channels().findChannel(prefix + names[c]) != nullptr;
    const bool colour = has[R] || has[G] || has[B];

    // Sample counts first, then one float array per channel holding the samples of every pixel.
    Imf::Array2D<unsigned int> counts(h, w);
    std::vector<Imf::Array2D<float*>> pointers(kChannels);
    const ptrdiff_t origin = -ptrdiff_t(dw.min.x) - ptrdiff_t(dw.min.y) * w;
    Imf::DeepFrameBuffer frame;
    frame.insertSampleCountSlice(Imf::Slice(Imf::UINT, reinterpret_cast<char*>(&counts[0][0] + origin),
                                            sizeof(unsigned int), sizeof(unsigned int) * size_t(w)));
    for (int c = 0; c < kChannels; ++c) {
        if (!has[c]) continue;
        pointers[c].resizeErase(h, w);
        frame.insert(prefix + names[c],
                     Imf::DeepSlice(Imf::FLOAT, reinterpret_cast<char*>(&pointers[c][0][0] + origin), sizeof(float*),
                                    sizeof(float*) * size_t(w), sizeof(float)));
    }
    std::vector<float> storage;
    auto allocate = [&]() {
        uint64_t total = 0;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) total += counts[y][x];
        if (total > kMaxDeepSamples) throw std::runtime_error("too many deep samples");
        storage.assign(size_t(total) * kChannels, 0.0f);
        size_t offset = 0;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                for (int c = 0; c < kChannels; ++c)
                    if (has[c]) pointers[c][y][x] = &storage[size_t(c) * size_t(total) + offset];
                offset += counts[y][x];
            }
    };
    if (header.hasType() && header.type() == Imf::DEEPTILE) {
        Imf::DeepTiledInputPart part(file, partNumber);
        part.setFrameBuffer(frame);
        const int nx = part.numXTiles(0), ny = part.numYTiles(0);
        part.readPixelSampleCounts(0, nx - 1, 0, ny - 1, 0, 0);
        allocate();
        part.readTiles(0, nx - 1, 0, ny - 1, 0, 0);
    } else {
        Imf::DeepScanLineInputPart part(file, partNumber);
        part.setFrameBuffer(frame);
        part.readPixelSampleCounts(dw.min.y, dw.max.y);
        allocate();
        part.readPixels(dw.min.y, dw.max.y);
    }

    std::vector<unsigned int> order;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const unsigned int n = counts[y][x];
            if (n == 0) continue;
            auto value = [&](int c, unsigned int k) { return has[c] ? finite(pointers[c][y][x][k]) : 0.0f; };
            order.resize(n);
            std::iota(order.begin(), order.end(), 0u);
            if (has[Z]) {
                auto depth = [&](unsigned int k) {
                    const float z = pointers[Z][y][x][k];
                    return std::isnan(z) ? INFINITY : z;
                };
                std::stable_sort(order.begin(), order.end(),
                                 [&](unsigned int i, unsigned int j) { return depth(i) < depth(j); });
            }
            // Front-to-back "over": colours are premultiplied; without alpha, samples are opaque.
            float r = 0, g = 0, b = 0, a = 0;
            for (unsigned int k : order) {
                const float sa = has[A] ? std::clamp(value(A, k), 0.0f, 1.0f) : 1.0f;
                float sr = value(R, k), sg = value(G, k), sb = value(B, k);
                if (!colour) sr = sg = sb = value(Y, k);
                const float t = 1.0f - a;
                r += t * sr;
                g += t * sg;
                b += t * sb;
                a += t * sa;
                if (a >= 0.9999f) break;
            }
            pixels[dw.min.y + y - all.min.y][dw.min.x + x - all.min.x] = Imf::Rgba(r, g, b, a);
        }

    int present = Imf::WRITE_A;
    if (colour) present |= (has[R] ? Imf::WRITE_R : 0) | (has[G] ? Imf::WRITE_G : 0) | (has[B] ? Imf::WRITE_B : 0);
    else present |= Imf::WRITE_RGB;
    return Imf::RgbaChannels(present);
}

}  // namespace

bool parse(const uint8_t* data, size_t size, Info& info, std::string* error) {
    info = Info();
    if (size < 8 || data[0] != 0x76 || data[1] != 0x2f || data[2] != 0x31 || data[3] != 0x01)
        return fail(error, "not an OpenEXR file");
    const int32_t version = le32(data + 4);
    if ((version & 0xff) != 2) return fail(error, "unsupported OpenEXR version");
    const int32_t flags = version & ~0xff;
    if (flags & ~(0x200 | 0x400 | 0x800 | 0x1000)) return fail(error, "unknown OpenEXR flags");
    info.multipart = (flags & 0x1000) != 0;
    const size_t maxName = (flags & 0x400) ? 255 : 31;

    // Headers: name, type, size, value until an empty name. A multipart file has one header per part,
    // then an empty header. The first part with colour or luminance is shown.
    Reader r(data + 8, size - 8);
    std::string firstError;
    for (int part = 0;; ++part) {
        if (info.multipart && part > 0 && r.atEndMarker()) break;  // end of the header list
        std::vector<std::string> channels;
        int compression = -1;
        bool haveDataWindow = false, haveDisplayWindow = false, haveLineOrder = false, haveAspect = false;
        bool haveScreenCenter = false, haveScreenWidth = false, haveTiles = false;
        bool tiled = (flags & 0x200) != 0, deep = (flags & 0x800) != 0;
        int32_t box[4] = {}, view[4] = {};
        while (!r.atEndMarker()) {
            std::string name, type;
            int32_t length;
            const uint8_t* value;
            if (!r.cstring(name, maxName) || !r.cstring(type, maxName) || !r.i32(length) || length < 0 ||
                !r.bytes(value, size_t(length)))
                return fail(error, "truncated OpenEXR header");
            if (name == "channels" && type == "chlist") {
                if (!readChannels(value, size_t(length), channels, maxName)) return fail(error, "bad channel list");
            } else if (name == "compression" && type == "compression" && length == 1) {
                compression = value[0];
            } else if (name == "dataWindow" && type == "box2i" && length == 16) {
                for (int i = 0; i < 4; ++i) box[i] = le32(value + 4 * i);
                haveDataWindow = true;
            } else if (name == "displayWindow" && type == "box2i" && length == 16) {
                for (int i = 0; i < 4; ++i) view[i] = le32(value + 4 * i);
                haveDisplayWindow = true;
            } else if (name == "lineOrder" && type == "lineOrder" && length == 1) {
                haveLineOrder = value[0] <= 2;
            } else if (name == "pixelAspectRatio" && type == "float" && length == 4) {
                haveAspect = true;
            } else if (name == "screenWindowCenter" && type == "v2f" && length == 8) {
                haveScreenCenter = true;
            } else if (name == "screenWindowWidth" && type == "float" && length == 4) {
                haveScreenWidth = true;
            } else if (name == "tiles" && type == "tiledesc") {
                haveTiles = true;
            } else if (name == "type" && type == "string") {
                std::string kind(reinterpret_cast<const char*>(value), size_t(length));
                if (kind == "tiledimage" || kind == "deeptile") tiled = true;
                else if (kind != "scanlineimage" && kind != "deepscanline") return fail(error, "unknown part type");
                deep = kind == "deepscanline" || kind == "deeptile";
            }
        }
        if (!r.skip(1)) return fail(error, "truncated OpenEXR header");  // end of this header

        auto reject = [&](const char* message) {
            if (firstError.empty()) firstError = message;
        };
        // The image shown is the display window; the data window may be smaller, larger or offset.
        const int64_t w = int64_t(view[2]) - view[0] + 1, h = int64_t(view[3]) - view[1] + 1;
        const int64_t dw = int64_t(box[2]) - box[0] + 1, dh = int64_t(box[3]) - box[1] + 1;
        const int64_t uw = int64_t(std::max(box[2], view[2])) - std::min(box[0], view[0]) + 1;
        const int64_t uh = int64_t(std::max(box[3], view[3])) - std::min(box[1], view[1]) + 1;
        std::string layer, names;
        if (channels.empty() || compression < 0 || !haveDataWindow || !haveDisplayWindow || !haveLineOrder ||
            !haveAspect || !haveScreenCenter || !haveScreenWidth)
            reject("incomplete OpenEXR header");
        else if (size_t(compression) >= kCompressionCount)
            reject("unknown OpenEXR compression");
        else if (tiled && !haveTiles)
            reject("tiled file without tile description");
        else if (w < 1 || h < 1 || dw < 1 || dh < 1 || uint64_t(uw) * uint64_t(uh) > kMaxPixels)
            reject("unsupported image size");
        else if (!pickLayer(channels, layer, names))
            reject("no colour or luminance channel");
        else {
            info.part = part;
            info.tiled = tiled;
            info.deep = deep;
            info.layer = layer;
            info.channels = names;
            info.width = uint32_t(w);
            info.height = uint32_t(h);
            info.compression = kCompressions[compression];
            return true;
        }
        if (!info.multipart || part >= 1023) break;
    }
    return fail(error, firstError.c_str());
}

bool decode(const uint8_t* data, size_t size, const Info& info, Image& out, std::string* error) {
    out = Image();
    Imf::Array2D<Imf::Rgba> pixels;
    Imf::RgbaChannels present;
    Imf::Chromaticities primaries;
    bool havePrimaries = false;
    int w = 0, h = 0;      // display window
    int ox = 0, oy = 0;    // display window origin inside the buffer
    int bw = 0;            // buffer width (union of the data and display windows)
    try {
        MemoryStream stream(data, size);
        std::unique_ptr<Imf::MultiPartInputFile> deep;
        std::unique_ptr<Imf::RgbaInputFile> flat;
        if (info.deep) deep = std::make_unique<Imf::MultiPartInputFile>(stream);
        else flat = std::make_unique<Imf::RgbaInputFile>(info.part, stream, info.layer);
        const Imf::Header& header = deep ? deep->header(info.part) : flat->header();
        const Imath::Box2i dw = header.dataWindow(), view = header.displayWindow();
        w = view.max.x - view.min.x + 1;
        h = view.max.y - view.min.y + 1;
        if (w != int(info.width) || h != int(info.height)) return fail(error, "header mismatch");
        const Imath::Box2i all(Imath::V2i(std::min(dw.min.x, view.min.x), std::min(dw.min.y, view.min.y)),
                               Imath::V2i(std::max(dw.max.x, view.max.x), std::max(dw.max.y, view.max.y)));
        bw = all.max.x - all.min.x + 1;
        const int bh = all.max.y - all.min.y + 1;
        ox = view.min.x - all.min.x;
        oy = view.min.y - all.min.y;
        // Pixels outside the data window are zero, alpha included (transparent).
        pixels.resizeErase(bh, bw);
        const Imf::Rgba zero(0.0f, 0.0f, 0.0f, 0.0f);
        for (int y = 0; y < bh; ++y)
            for (int x = 0; x < bw; ++x) pixels[y][x] = zero;
        if (deep) {
            present = flattenDeep(*deep, info.part, info.layer, all, pixels);
        } else {
            flat->setFrameBuffer(&pixels[0][0] - all.min.x - ptrdiff_t(all.min.y) * bw, 1, size_t(bw));
            flat->readPixels(dw.min.y, dw.max.y);
            present = flat->channels();
        }
        if (Imf::hasChromaticities(header)) {
            primaries = Imf::chromaticities(header);
            havePrimaries = true;
        }
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    } catch (...) {
        return fail(error, "OpenEXR decoding failed");
    }

    // Conversion to linear sRGB / Rec.709 primaries (D65), when the file declares other ones.
    const Imf::Chromaticities rec709;
    bool convert = false;
    Mat3 toSrgb = {};
    if (havePrimaries && !samePrimaries(primaries, rec709)) {
        Mat3 src, dst, dstInv;
        if (rgbToXyz(primaries, src) && rgbToXyz(rec709, dst) && invert(dst, dstInv)) {
            // A real illuminant (D50, ACES D60...) is adapted to D65. An equal-energy white marks absolute
            // CIE XYZ values (the OpenEXR "XYZ" test images): those are converted without adaptation.
            const bool equalEnergy =
                std::fabs(primaries.white.x - 1.0f / 3) < 1e-3f && std::fabs(primaries.white.y - 1.0f / 3) < 1e-3f;
            toSrgb = equalEnergy ? mul(dstInv, src) : mul(dstInv, mul(bradford(primaries.white, rec709.white), src));
            convert = true;
        }
    }

    // A single colour channel (masks, heights) is shown as grey rather than red, green or blue.
    const int colour = present & (Imf::WRITE_RGB | Imf::WRITE_Y);
    const bool single = colour == Imf::WRITE_R || colour == Imf::WRITE_G || colour == Imf::WRITE_B;
    const size_t count = size_t(w) * size_t(h);
    // Pixel i of the display window.
    auto at = [&](size_t i) -> const Imf::Rgba& {
        return pixels[oy + int(i / size_t(w))][ox + int(i % size_t(w))];
    };

    // OpenEXR colours are premultiplied by alpha. Some converters write straight alpha instead: in an
    // image whose values stay within [0, 1], many semi-transparent pixels brighter than their alpha
    // give them away, and dividing by alpha would then create false highlights on the edges.
    bool premultiplied = true;
    {
        uint64_t semi = 0, brighter = 0;
        float peak = 0.0f;
        for (size_t i = 0; i < count; ++i) {
            const Imf::Rgba& p = at(i);
            const float a = finite(float(p.a));
            const float m = std::max({finite(float(p.r)), finite(float(p.g)), finite(float(p.b))});
            peak = std::max(peak, m);
            if (a > 0.0f && a < 1.0f) {
                ++semi;
                brighter += m > a * 1.01f + 1e-3f;
            }
        }
        premultiplied = !(peak <= 1.0f && semi > 0 && brighter * 100 > semi);
    }
    // Straight-alpha linear sRGB of pixel i. Recomputed in both passes to keep memory low.
    auto load = [&](size_t i, float& r, float& g, float& b, float& a) {
        const Imf::Rgba& p = at(i);
        r = finite(float(p.r));
        g = finite(float(p.g));
        b = finite(float(p.b));
        a = std::clamp(finite(float(p.a)), 0.0f, 1.0f);
        if (single) r = g = b = r + g + b;  // the two missing channels are zero
        if (premultiplied && a > 1e-4f && a < 1.0f) {
            r /= a;
            g /= a;
            b /= a;
        }
        if (convert) {
            const float(*m)[3] = toSrgb.m;
            const float r2 = m[0][0] * r + m[0][1] * g + m[0][2] * b;
            const float g2 = m[1][0] * r + m[1][1] * g + m[1][2] * b;
            const float b2 = m[2][0] * r + m[2][1] * g + m[2][2] * b;
            r = r2;
            g = g2;
            b = b2;
        }
        r = std::max(r, 0.0f);
        g = std::max(g, 0.0f);
        b = std::max(b, 0.0f);
    };

    // Pass 1: histograms of the brightest channel (what a display clips) over all pixels and the visible ones.
    Histogram all, visible;
    bool anyAlpha = false, anyColour = false;
    for (size_t i = 0; i < count; ++i) {
        float r, g, b, a;
        load(i, r, g, b, a);
        const float m = std::max({r, g, b});
        all.add(m);
        if (a > 0) visible.add(m);
        anyAlpha |= a > 0;
        anyColour |= m > 0;
    }
    // All-zero alpha with colour data means the alpha is unused: show the image opaque.
    const bool opaque = !anyAlpha && anyColour;
    // Transparent pixels do not count for the exposure.
    const float white = (opaque ? all : visible).percentile(0.999);

    // HDR: the brightest 0.1 % of the image go beyond display white (small overshoots are clipped).
    out.hdr = white > 1.1f;
    out.white = out.hdr ? white : 1.0f;
    out.converted = convert;
    out.straightAlpha = !premultiplied;
    // Shoulder above kKnee: x = m - knee is compressed with an extended Reinhard curve scaled to the
    // room left above the knee, h(x) = x (1 + x / c) / (1 + x / room), so that h(0) = 0 with slope 1
    // (no visible break) and the white level lands exactly on 1. Below the knee nothing changes.
    const float kKnee = 0.5f, room = 1.0f - kKnee;
    const float excess = out.white - kKnee, c = excess * excess / room;

    // Pass 2: tone mapping and sRGB encoding.
    static const SrgbTable srgb;
    out.width = uint32_t(w);
    out.height = uint32_t(h);
    out.bgra.resize(count * 4);
    for (size_t i = 0; i < count; ++i) {
        float r, g, b, a;
        load(i, r, g, b, a);
        if (opaque) a = 1.0f;
        if (out.hdr) {
            // Applied to the brightest channel, then to the three channels alike: hue and saturation are
            // kept, and no channel can exceed 1 (brighter than white: scaled down to full intensity).
            const float m = std::max({r, g, b});
            if (m > kKnee) {
                const float x = m - kKnee;
                const float md = std::min(kKnee + x * (1.0f + x / c) / (1.0f + x / room), 1.0f);
                const float s = md / m;
                r *= s;
                g *= s;
                b *= s;
            }
        }
        uint8_t* o = &out.bgra[i * 4];
        o[0] = srgb(b);
        o[1] = srgb(g);
        o[2] = srgb(r);
        o[3] = uint8_t(std::lround(a * 255.0f));
    }
    return true;
}

std::string describe(const Info& info) {
    std::string s = info.compression + ", " + info.channels;
    if (!info.layer.empty()) s += ", layer " + info.layer;
    s += info.tiled ? ", tiles" : ", scanlines";
    if (info.deep) s += ", deep (flattened)";
    if (info.multipart) s += ", part " + std::to_string(info.part + 1);
    return s;
}

}  // namespace exr
