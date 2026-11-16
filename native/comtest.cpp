// comtest.cpp - 逐步骤打印的 COM 缩略图测试（用于定位崩溃点）
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <thumbcache.h>
#include <cstdio>
#include <vector>

static const GUID CLSID_Thumb = { 0x7c3a9e21,0x4f58,0x4b0a,{0x9c,0x71,0x2d,0x6e,0x8b,0x5f,0x1a,0x30} };
static const GUID GUID_InitWithFile = {0xb7d14566,0x0509,0x4cce,{0xa7,0x1f,0x0a,0x55,0x42,0x33,0xbd,0x9b}};

struct IInitFile : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Initialize(LPCWSTR pszFilePath) = 0;
};

#define STEP(msg) do { std::printf("[%d] %s\n", __LINE__, msg); std::fflush(stdout); } while(0)

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { std::printf("comtest <file.step>\n"); return 2; }
    STEP("CoInitializeEx");
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::printf("     hr=0x%08X\n", (unsigned)hr); std::fflush(stdout);

    IUnknown* unk = nullptr;
    if (argc > 3 && !_wcsicmp(argv[3], L"direct")) {   // 绕开 COM 直接加载
        STEP("LoadLibrary(StepThumb.dll)");
        HMODULE hm = LoadLibraryExW(L"C:\\Program Files\\StepThumb\\StepThumb.dll", nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        std::printf("     hm=%p err=%lu\n", (void*)hm, GetLastError()); std::fflush(stdout);
        typedef HRESULT (WINAPI *PFN)(REFCLSID, REFIID, void**);
        PFN pfn = (PFN)GetProcAddress(hm, "DllGetClassObject");
        IClassFactory* cf = nullptr;
        hr = pfn(CLSID_Thumb, IID_IClassFactory, (void**)&cf);
        std::printf("     dgco hr=0x%08X cf=%p\n", (unsigned)hr, (void*)cf); std::fflush(stdout);
        hr = cf->CreateInstance(nullptr, IID_IUnknown, (void**)&unk);
        std::printf("     create hr=0x%08X unk=%p\n", (unsigned)hr, (void*)unk); std::fflush(stdout);
    } else {
    STEP("CoCreateInstance");
    hr = CoCreateInstance(CLSID_Thumb, nullptr, CLSCTX_INPROC_SERVER, IID_IUnknown, (void**)&unk);
    std::printf("     hr=0x%08X ptr=%p\n", (unsigned)hr, (void*)unk); std::fflush(stdout);
    }
    if (FAILED(hr) || !unk) return 3;

    STEP("QueryInterface(IInitializeWithFile)");
    IInitFile* init = nullptr;
    hr = unk->QueryInterface(GUID_InitWithFile, (void**)&init);
    std::printf("     hr=0x%08X\n", (unsigned)hr); std::fflush(stdout);

    STEP("Initialize(path) —— 会触发 STEP 解析");
    hr = init->Initialize(argv[1]);
    std::printf("     hr=0x%08X\n", (unsigned)hr); std::fflush(stdout);

    STEP("GetThumbnail");
    IThumbnailProvider* tp = nullptr;
    hr = unk->QueryInterface(IID_IThumbnailProvider, (void**)&tp);
    if (SUCCEEDED(hr)) {
        HBITMAP hb = nullptr;
        WTS_ALPHATYPE at = WTSAT_UNKNOWN;
        LARGE_INTEGER f0, f1, fr;
        QueryPerformanceFrequency(&fr);
        QueryPerformanceCounter(&f0);
        hr = tp->GetThumbnail(256, &hb, &at);
        QueryPerformanceCounter(&f1);
        std::printf("     hr=0x%08X hbmp=%p time=%.0fms\n", (unsigned)hr, (void*)hb,
                    1000.0 * (double)(f1.QuadPart - f0.QuadPart) / (double)fr.QuadPart);
        std::fflush(stdout);
        if (hb) {
            BITMAP bm{};
            GetObject(hb, sizeof(bm), &bm);
            BITMAPINFOHEADER bi{};
            bi.biSize = sizeof(bi); bi.biWidth = bm.bmWidth; bi.biHeight = -bm.bmHeight;
            bi.biPlanes = 1; bi.biBitCount = 32; bi.biCompression = BI_RGB;
            size_t px = (size_t)bm.bmWidth * bm.bmHeight * 4;
            std::vector<unsigned char> buf(px);
            HDC dc = GetDC(nullptr);
            GetDIBits(dc, hb, 0, bm.bmHeight, buf.data(), (BITMAPINFO*)&bi, DIB_RGB_COLORS);
            ReleaseDC(nullptr, dc);
            BITMAPFILEHEADER fh{};
            fh.bfType = 0x4D42;
            fh.bfOffBits = sizeof(fh) + sizeof(bi);
            fh.bfSize = (DWORD)(fh.bfOffBits + px);
            wchar_t out[MAX_PATH];
            wsprintfW(out, L"com-%s.bmp", (argc > 2) ? argv[2] : L"out");
            FILE* f = _wfopen(out, L"wb");
            if (f) {
                fwrite(&fh, sizeof(fh), 1, f);
                fwrite(&bi, sizeof(bi), 1, f);
                fwrite(buf.data(), 1, px, f);
                fclose(f);
                std::printf("     wrote %ls\n", out); std::fflush(stdout);
            }
            DeleteObject(hb);
        }
        tp->Release();
    }
    STEP("DONE");
    if (init) init->Release();
    unk->Release();
    CoUninitialize();
    return 0;
}
