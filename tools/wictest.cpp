// wictest: decodes an EXR image through WIC and saves it as PNG with WIC's own encoder.
// Usage: wictest <ExrWic.dll|--system> <in.exr> <out.png>
//   ExrWic.dll  loads the codec directly (no registration needed)
//   --system    goes through the registered codecs, like Explorer does, and prints which one
#include <windows.h>
#include <wincodec.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define CHECK(expr)                                                         \
    do {                                                                    \
        HRESULT hr_ = (expr);                                               \
        if (FAILED(hr_)) {                                                  \
            std::fprintf(stderr, "%s failed: 0x%08lX\n", #expr, (unsigned long)hr_); \
            return 1;                                                       \
        }                                                                   \
    } while (0)

static std::wstring widen(const char* s) {
    int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_ACP, 0, s, -1, &w[0], n);
    w.resize(size_t(n - 1));
    return w;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: wictest <ExrWic.dll|--system> <in.exr> <out.png>\n");
        return 2;
    }

    CHECK(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    IWICImagingFactory* factory = nullptr;
    CHECK(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));

    IWICBitmapDecoder* decoder = nullptr;
    const std::wstring in = widen(argv[2]);
    if (!std::strcmp(argv[1], "--system")) {
        CHECK(factory->CreateDecoderFromFilename(in.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                 &decoder));
    } else {
        HMODULE dll = LoadLibraryW(widen(argv[1]).c_str());
        if (!dll) {
            std::fprintf(stderr, "cannot load %s (error %lu)\n", argv[1], GetLastError());
            return 1;
        }
        auto getClass = reinterpret_cast<HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**)>(
            GetProcAddress(dll, "DllGetClassObject"));
        // Same CLSID as CLSID_ExrDecoder in src/wic/exrwic.cpp.
        const CLSID clsid = {0x38384643, 0x1a54, 0x46b6, {0xbf, 0x6b, 0x9a, 0x6e, 0x0a, 0x3a, 0x7b, 0x37}};
        IClassFactory* cf = nullptr;
        CHECK(getClass(clsid, IID_PPV_ARGS(&cf)));
        CHECK(cf->CreateInstance(nullptr, IID_PPV_ARGS(&decoder)));
        IWICStream* stream = nullptr;
        CHECK(factory->CreateStream(&stream));
        CHECK(stream->InitializeFromFilename(in.c_str(), GENERIC_READ));
        DWORD caps = 0;
        CHECK(decoder->QueryCapability(stream, &caps));
        std::printf("QueryCapability: 0x%lX\n", caps);
        CHECK(decoder->Initialize(stream, WICDecodeMetadataCacheOnDemand));
    }

    IWICBitmapDecoderInfo* info = nullptr;
    if (SUCCEEDED(decoder->GetDecoderInfo(&info))) {
        wchar_t name[256] = {};
        UINT len = 0;
        info->GetFriendlyName(256, name, &len);
        std::printf("decoder: %ls\n", name);
        info->Release();
    }
    IWICBitmapFrameDecode* frame = nullptr;
    CHECK(decoder->GetFrame(0, &frame));
    UINT w = 0, h = 0;
    CHECK(frame->GetSize(&w, &h));
    std::printf("frame: %ux%u\n", w, h);

    // Other installed decoders may use another pixel format.
    std::vector<BYTE> pixels(size_t(w) * h * 4);
    IWICFormatConverter* conv = nullptr;
    CHECK(factory->CreateFormatConverter(&conv));
    CHECK(conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                           WICBitmapPaletteTypeCustom));
    CHECK(conv->CopyPixels(nullptr, w * 4, UINT(pixels.size()), pixels.data()));
    conv->Release();

    IWICStream* out = nullptr;
    CHECK(factory->CreateStream(&out));
    CHECK(out->InitializeFromFilename(widen(argv[3]).c_str(), GENERIC_WRITE));
    IWICBitmapEncoder* enc = nullptr;
    CHECK(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc));
    CHECK(enc->Initialize(out, WICBitmapEncoderNoCache));
    IWICBitmapFrameEncode* ef = nullptr;
    CHECK(enc->CreateNewFrame(&ef, nullptr));
    CHECK(ef->Initialize(nullptr));
    CHECK(ef->SetSize(w, h));
    WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
    CHECK(ef->SetPixelFormat(&pf));
    CHECK(ef->WritePixels(h, w * 4, UINT(pixels.size()), pixels.data()));
    CHECK(ef->Commit());
    CHECK(enc->Commit());
    std::printf("wrote %s\n", argv[3]);

    ef->Release();
    enc->Release();
    out->Release();
    frame->Release();
    decoder->Release();
    factory->Release();
    CoUninitialize();
    return 0;
}
