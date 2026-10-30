// exrtest: test helper for exr-viewer (not packaged). Writes OpenEXR variants with the official library.
//   exrtest merge-deep <out.exr> <in1.exr> <in2.exr> ...   merge deep scanline passes (R, G, B, A, Z) into one
//                                                           deep file: decoding it must match the composite
//   exrtest recompress <in.exr> <out.exr> <COMPRESSION>    rewrite the RGBA image with another compression
//   exrtest depth-first <in.exr> <out.exr>                 two-part file: a depth-only part, then the image
#include <ImfArray.h>
#include <ImfChannelList.h>
#include <ImfCompression.h>
#include <ImfDeepFrameBuffer.h>
#include <ImfDeepScanLineInputPart.h>
#include <ImfDeepScanLineOutputFile.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfMultiPartInputFile.h>
#include <ImfMultiPartOutputFile.h>
#include <ImfOutputPart.h>
#include <ImfPartType.h>
#include <ImfRgbaFile.h>
#include <ImfStandardAttributes.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

const char* const kChannels[] = {"R", "G", "B", "A", "Z"};
const int kCount = 5;

struct Pass {
    Imath::Box2i window;
    Imf::Array2D<unsigned int> counts;
    std::vector<float> samples[kCount];  // per channel, pixels in scanline order
    std::vector<size_t> start;           // first sample of each pixel
};

void readPass(const char* path, Pass& pass) {
    Imf::MultiPartInputFile file(path);
    Imf::DeepScanLineInputPart part(file, 0);
    const Imath::Box2i dw = part.header().dataWindow();
    const int w = dw.max.x - dw.min.x + 1, h = dw.max.y - dw.min.y + 1;
    pass.window = dw;
    pass.counts.resizeErase(h, w);
    Imf::Array2D<float*> pointers[kCount];
    const ptrdiff_t origin = -ptrdiff_t(dw.min.x) - ptrdiff_t(dw.min.y) * w;
    Imf::DeepFrameBuffer frame;
    frame.insertSampleCountSlice(Imf::Slice(Imf::UINT, reinterpret_cast<char*>(&pass.counts[0][0] + origin),
                                            sizeof(unsigned int), sizeof(unsigned int) * size_t(w)));
    for (int c = 0; c < kCount; ++c) {
        pointers[c].resizeErase(h, w);
        frame.insert(kChannels[c], Imf::DeepSlice(Imf::FLOAT, reinterpret_cast<char*>(&pointers[c][0][0] + origin),
                                                  sizeof(float*), sizeof(float*) * size_t(w), sizeof(float)));
    }
    part.setFrameBuffer(frame);
    part.readPixelSampleCounts(dw.min.y, dw.max.y);
    size_t total = 0;
    pass.start.resize(size_t(w) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            pass.start[size_t(y) * w + x] = total;
            total += pass.counts[y][x];
        }
    for (int c = 0; c < kCount; ++c) {
        pass.samples[c].assign(total, 0.0f);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) pointers[c][y][x] = pass.samples[c].data() + pass.start[size_t(y) * w + x];
    }
    part.readPixels(dw.min.y, dw.max.y);
}


int mergeDeep(int argc, char** argv) {
    std::vector<Pass> passes(size_t(argc) - 3);
    Imath::Box2i all;
    for (int i = 3; i < argc; ++i) {
        readPass(argv[i], passes[size_t(i) - 3]);
        all.extendBy(passes[size_t(i) - 3].window);
    }
    Imf::Header header(Imf::MultiPartInputFile(argv[3]).header(0));
    header.dataWindow() = all;
    header.setType(Imf::DEEPSCANLINE);
    header.compression() = Imf::ZIPS_COMPRESSION;
    header.channels() = Imf::ChannelList();
    for (int c = 0; c < kCount; ++c) header.channels().insert(kChannels[c], Imf::Channel(Imf::FLOAT));

    const int w = all.max.x - all.min.x + 1, h = all.max.y - all.min.y + 1;
    Imf::Array2D<unsigned int> counts(h, w);
    std::vector<float> merged[kCount];
    std::vector<size_t> start(size_t(w) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            start[size_t(y) * w + x] = merged[0].size();
            unsigned int n = 0;
            for (const Pass& p : passes) {
                const int px = all.min.x + x - p.window.min.x, py = all.min.y + y - p.window.min.y;
                const int pw = p.window.max.x - p.window.min.x + 1, ph = p.window.max.y - p.window.min.y + 1;
                if (px < 0 || py < 0 || px >= pw || py >= ph) continue;
                const size_t s = p.start[size_t(py) * pw + px];
                for (unsigned int k = 0; k < p.counts[py][px]; ++k)
                    for (int c = 0; c < kCount; ++c) merged[c].push_back(p.samples[c][s + k]);
                n += p.counts[py][px];
            }
            counts[y][x] = n;
        }

    Imf::Array2D<float*> pointers[kCount];
    const ptrdiff_t origin = -ptrdiff_t(all.min.x) - ptrdiff_t(all.min.y) * w;
    Imf::DeepFrameBuffer frame;
    frame.insertSampleCountSlice(Imf::Slice(Imf::UINT, reinterpret_cast<char*>(&counts[0][0] + origin),
                                            sizeof(unsigned int), sizeof(unsigned int) * size_t(w)));
    for (int c = 0; c < kCount; ++c) {
        pointers[c].resizeErase(h, w);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) pointers[c][y][x] = merged[c].data() + start[size_t(y) * w + x];
        frame.insert(kChannels[c], Imf::DeepSlice(Imf::FLOAT, reinterpret_cast<char*>(&pointers[c][0][0] + origin),
                                                  sizeof(float*), sizeof(float*) * size_t(w), sizeof(float)));
    }
    Imf::DeepScanLineOutputFile out(argv[2], header);
    out.setFrameBuffer(frame);
    out.writePixels(h);
    std::printf("%s: %zu samples from %d passes\n", argv[2], merged[0].size(), argc - 3);
    return 0;
}

// Reads the first RGBA image of a file.
void readRgba(const char* path, Imf::Header& header, Imf::Array2D<Imf::Rgba>& pixels) {
    Imf::RgbaInputFile in(path);
    const Imath::Box2i dw = in.dataWindow();
    const int w = dw.max.x - dw.min.x + 1, h = dw.max.y - dw.min.y + 1;
    pixels.resizeErase(h, w);
    in.setFrameBuffer(&pixels[0][0] - dw.min.x - ptrdiff_t(dw.min.y) * w, 1, size_t(w));
    in.readPixels(dw.min.y, dw.max.y);
    header = Imf::Header(in.displayWindow(), dw, in.header().pixelAspectRatio());
    if (Imf::hasChromaticities(in.header())) Imf::addChromaticities(header, Imf::chromaticities(in.header()));
}

int recompress(char** argv) {
    Imf::Header header;
    Imf::Array2D<Imf::Rgba> pixels;
    readRgba(argv[2], header, pixels);
    Imf::Compression id;
    Imf::getCompressionIdFromName(argv[4], id);
    if (id == Imf::NUM_COMPRESSION_METHODS) throw std::runtime_error(std::string("unknown compression ") + argv[4]);
    header.compression() = id;
    const Imath::Box2i dw = header.dataWindow();
    const int w = dw.max.x - dw.min.x + 1, h = dw.max.y - dw.min.y + 1;
    Imf::RgbaOutputFile out(argv[3], header, Imf::WRITE_RGBA);
    out.setFrameBuffer(&pixels[0][0] - dw.min.x - ptrdiff_t(dw.min.y) * w, 1, size_t(w));
    out.writePixels(h);
    std::printf("%s: %s\n", argv[3], argv[4]);
    return 0;
}

int depthFirst(char** argv) {
    Imf::Header image;
    Imf::Array2D<Imf::Rgba> pixels;
    readRgba(argv[2], image, pixels);
    const Imath::Box2i dw = image.dataWindow();
    const int w = dw.max.x - dw.min.x + 1, h = dw.max.y - dw.min.y + 1;
    const ptrdiff_t origin = -ptrdiff_t(dw.min.x) - ptrdiff_t(dw.min.y) * w;

    Imf::Header depth(image);
    depth.setName("depth");
    depth.setType(Imf::SCANLINEIMAGE);
    depth.channels().insert("Z", Imf::Channel(Imf::FLOAT));
    image.setName("rgba");
    image.setType(Imf::SCANLINEIMAGE);
    for (const char* c : {"R", "G", "B", "A"}) image.channels().insert(c, Imf::Channel(Imf::HALF));
    const Imf::Header headers[2] = {depth, image};
    Imf::MultiPartOutputFile file(argv[3], headers, 2);

    Imf::Array2D<float> z(h, w);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) z[y][x] = 1.0f + float(x + y);
    Imf::FrameBuffer zfb;
    zfb.insert("Z", Imf::Slice(Imf::FLOAT, reinterpret_cast<char*>(&z[0][0] + origin), sizeof(float),
                               sizeof(float) * size_t(w)));
    Imf::OutputPart zpart(file, 0);
    zpart.setFrameBuffer(zfb);
    zpart.writePixels(h);

    Imf::FrameBuffer fb;
    char* base = reinterpret_cast<char*>(&pixels[0][0] + origin);
    const size_t xs = sizeof(Imf::Rgba), ys = sizeof(Imf::Rgba) * size_t(w);
    fb.insert("R", Imf::Slice(Imf::HALF, base + offsetof(Imf::Rgba, r), xs, ys));
    fb.insert("G", Imf::Slice(Imf::HALF, base + offsetof(Imf::Rgba, g), xs, ys));
    fb.insert("B", Imf::Slice(Imf::HALF, base + offsetof(Imf::Rgba, b), xs, ys));
    fb.insert("A", Imf::Slice(Imf::HALF, base + offsetof(Imf::Rgba, a), xs, ys));
    Imf::OutputPart ipart(file, 1);
    ipart.setFrameBuffer(fb);
    ipart.writePixels(h);
    std::printf("%s: depth part, then image part\n", argv[3]);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string cmd = argc > 1 ? argv[1] : "";
    try {
        if (cmd == "merge-deep" && argc >= 4) return mergeDeep(argc, argv);
        if (cmd == "recompress" && argc == 5) return recompress(argv);
        if (cmd == "depth-first" && argc == 4) return depthFirst(argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
    std::fprintf(stderr, "usage: exrtest merge-deep <out> <in...> | recompress <in> <out> <COMP> | depth-first <in> <out>\n");
    return 2;
}
