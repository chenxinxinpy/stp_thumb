// verify_com.cpp - 安装后验证 COM 链路：
// CoCreateInstance -> IInitializeWithFile -> IThumbnailProvider::GetThumbnail -> BMP
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#ifdef __GNUC__
#include <shobjidl.h>
#include <thumbcache.h>
#include <propsys.h>
#else
#include <shobjidl_core.h>
#include <propsys.h>
#endif
#include <cstdio>
#include <cstdint>
#include <stdio.h>
#include <wchar.h>
#include <vector>

static const GUID GUID_IThumbnailProvider  = {0xe357fccd,0xa995,0x4576,{0xb0,0x1f,0x23,0x46,0x30,0x15,0x4e,0x96}};
static const GUID GUID_IInitializeWithFile = {0xb7d14566,0x0509,0x4cce,{0xa7,0x1f,0x0a,0x55,0x42,0x33,0xbd,0x9b}};
#ifndef __GNUC__
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "gdi32.lib")
#endif

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { std::printf("verify_com <file.step> [out.bmp]\n"); return 2; }
    const wchar_t* out = (argc >= 3) ? argv[2] : L"com-thumb.bmp";
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) { std::printf("FAIL CoInitializeEx hr=0x%08X\n", (unsigned)hr); return 3; }

    CLSID clsid{};
    CLSIDFromString(L"{7C3A9E21-4F58-4B0A-9C71-2D6E8B5F1A30}", &clsid);
    IThumbnailProvider* provider = nullptr;
    hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER,
                          GUID_IThumbnailProvider, (void**)&provider);
    if (FAILED(hr) || !provider) {
        std::printf("FAIL CoCreateInstance hr=0x%08X (DLL 未注册或加载失败)\n", (unsigned)hr);
        CoUninitialize(); return 4;
    }
    IInitializeWithFile* init = nullptr;
    hr = provider->QueryInterface(GUID_IInitializeWithFile, (void**)&init);
    if (FAILED(hr) || !init) {
        std::printf("FAIL QueryInterface(IInitializeWithFile) hr=0x%08X\n", (unsigned)hr);
        provider->Release(); CoUninitialize(); return 5;
    }
    hr = init->Initialize(argv[1], STGM_READ);
    if (FAILED(hr)) { std::printf("FAIL Initialize hr=0x%08X\n", (unsigned)hr); goto done; }

    {
        HBITMAP hb = nullptr;
        WTS_ALPHATYPE alpha = (WTS_ALPHATYPE)0;
        hr = provider->GetThumbnail(256, &hb, &alpha);
        if (FAILED(hr) || !hb) {
            std::printf("FAIL GetThumbnail hr=0x%08X\n", (unsigned)hr);
            goto done;
        }
        BITMAP bm{};
        GetObjectW(hb, sizeof(bm), &bm);
        std::printf("OK bitmap=%ldx%ld bpp=%d alpha=%d\n", bm.bmWidth, bm.bmHeight, bm.bmBitsPixel, (int)alpha);

        BITMAPINFOHEADER bi{};
        bi.biSize = sizeof(BITMAPINFOHEADER);
        bi.biWidth = bm.bmWidth;
        bi.biHeight = bm.bmHeight;
        bi.biPlanes = 1;
        bi.biBitCount = 32;
        bi.biCompression = BI_RGB;
        std::vector<uint8_t> px((size_t)bm.bmWidth*bm.bmHeight*4);
        HDC dc = GetDC(nullptr);
        int rows = GetDIBits(dc, hb, 0, bm.bmHeight, px.data(), (BITMAPINFO*)&bi, DIB_RGB_COLORS);
        ReleaseDC(nullptr, dc);
        if (!rows) { std::printf("FAIL GetDIBits\n"); DeleteObject(hb); goto done; }

        int nonbg = 0;
        for (size_t i = 0; i < px.size(); i += 4) {
            if (px[i] != 253 || px[i+1] != 251 || px[i+2] != 250) ++nonbg;
        }
        std::printf("OK non-background pixels=%d / %zu (%.1f%%)\n",
                    nonbg, px.size()/4, 100.0*nonbg/(px.size()/4));

        BITMAPFILEHEADER fh{};
        fh.bfType = 0x4D42;
        fh.bfOffBits = sizeof(BITMAPFILEHEADER)+sizeof(BITMAPINFOHEADER);
        fh.bfSize = fh.bfOffBits + (DWORD)px.size();
        FILE* f = _wfopen(out, L"wb");
        if (f) {
            fwrite(&fh, sizeof(fh), 1, f);
            fwrite(&bi, sizeof(bi), 1, f);
            fwrite(px.data(), 1, px.size(), f);
            fclose(f);
            wprintf(L"OK wrote %ls\n", out);
        }
        DeleteObject(hb);
    }
done:
    init->Release();
    provider->Release();
    CoUninitialize();
    return FAILED(hr) ? 6 : 0;
}
