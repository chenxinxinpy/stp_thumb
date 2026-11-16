// verify_cache.cpp - 缩略图串台问题的复现 / 回归测试
//
// 两条路径都要测：
//   provider : IInitializeWithStream -> IThumbnailProvider::GetThumbnail
//              （资源管理器最常用的路径，也是本次 bug 的现场）
//   shell    : IShellItemImageFactory::GetImage
//              （完全等同于在资源管理器里真正取一张缩略图）
//
// 用法: verify_cache.exe <file1> <file2> [file3 ...]
// 退出码: 0 = 通过（各文件缩略图互不相同）；1 = 串台或失败
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#ifdef __GNUC__
#include <shobjidl.h>
#include <thumbcache.h>
#include <propsys.h>
#else
#include <shobjidl_core.h>
#include <thumbcache.h>
#include <propsys.h>
#endif
#include <shlwapi.h>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <chrono>

#ifndef __GNUC__
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#endif

static const GUID GUID_IThumbnailProvider    = {0xe357fccd,0xa995,0x4576,{0xb0,0x1f,0x23,0x46,0x30,0x15,0x4e,0x96}};
static const GUID GUID_IInitializeWithStream = {0xb824b49d,0x22ac,0x4161,{0xac,0x8a,0x99,0x16,0xe8,0xfa,0x3f,0x7f}};
static const GUID GUID_IShellItem      = {0x43826d1e,0xe718,0x42ee,{0xbc,0x55,0xa1,0xe2,0x61,0xc3,0x7b,0xfe}};
static const GUID CLSID_LocalThumbCache= {0x50ef4544,0xac9f,0x4a8e,{0xb2,0x1b,0x8a,0x26,0x18,0x0d,0xb1,0x3f}};
static const GUID IID_IThumbnailCacheD = {0xf676c15d,0x596a,0x4ce2,{0x82,0x34,0x33,0x99,0x6f,0x44,0x5d,0xb1}};

struct Result { uint32_t hash = 0; int w = 0, h = 0; double nonbg = 0; long ms = 0; HRESULT hr = S_OK; };

static uint32_t fnv(const std::vector<uint8_t>& v) {
    uint32_t h = 2166136261u;
    for (uint8_t b : v) { h ^= b; h *= 16777619u; }
    return h;
}

// 把 HBITMAP 取成像素并统计非背景占比（背景 #FAFBFD = 250,251,253）
static uint32_t hashBitmap(HBITMAP hb, Result& r) {
    BITMAP bm{};
    GetObjectW(hb, sizeof(bm), &bm);
    BITMAPINFOHEADER bi{};
    bi.biSize = sizeof(bi);
    bi.biWidth = bm.bmWidth;
    bi.biHeight = bm.bmHeight;
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;
    std::vector<uint8_t> px((size_t)bm.bmWidth * bm.bmHeight * 4);
    HDC dc = GetDC(nullptr);
    int rows = GetDIBits(dc, hb, 0, bm.bmHeight, px.data(), (BITMAPINFO*)&bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    if (!rows) return 0;
    int nonbg = 0;
    for (size_t i = 0; i < px.size(); i += 4)
        if (px[i] != 253 || px[i+1] != 251 || px[i+2] != 250) ++nonbg;
    r.w = bm.bmWidth; r.h = bm.bmHeight;
    r.nonbg = 100.0 * nonbg / (bm.bmWidth * bm.bmHeight);
    return fnv(px);
}

static Result thumbViaStream(const wchar_t* file, UINT cx) {
    Result r;
    auto t0 = std::chrono::steady_clock::now();
    CLSID clsid{};
    if (FAILED(CLSIDFromString(L"{7C3A9E21-4F58-4B0A-9C71-2D6E8B5F1A30}", &clsid))) return r;

    IThumbnailProvider* provider = nullptr;
    if (FAILED(CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER,
                                GUID_IThumbnailProvider, (void**)&provider)) || !provider)
        return r;

    IInitializeWithStream* init = nullptr;
    IStream* stm = nullptr;
    if (SUCCEEDED(provider->QueryInterface(GUID_IInitializeWithStream, (void**)&init)) && init) {
        if (SUCCEEDED(SHCreateStreamOnFileEx(file, STGM_READ | STGM_SHARE_DENY_WRITE,
                                             0, FALSE, nullptr, &stm)) && stm) {
            if (SUCCEEDED(init->Initialize(stm, STGM_READ))) {
                HBITMAP hb = nullptr;
                WTS_ALPHATYPE alpha = (WTS_ALPHATYPE)0;
                if (SUCCEEDED(provider->GetThumbnail(cx, &hb, &alpha)) && hb) {
                    r.hash = hashBitmap(hb, r);
                    DeleteObject(hb);
                }
            }
            stm->Release();
        }
        init->Release();
    }
    provider->Release();
    r.ms = (long)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t0).count();
    return r;
}

static Result thumbViaShell(const wchar_t* file, UINT cx) {
    Result r;
    auto t0 = std::chrono::steady_clock::now();
    // 走资源管理器填充缩略图缓存时用的那条路径：IThumbnailCache::GetThumbnail
    IThumbnailCache* cache = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_LocalThumbCache, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IThumbnailCacheD, (void**)&cache);
    if (FAILED(hr) || !cache) { r.hr = hr; return r; }

    std::wstring path(file);
    for (auto& c : path) if (c == L'/') c = L'\\';     // shell 解析用反斜杠更稳
    IShellItem* item = nullptr;
    hr = SHCreateItemFromParsingName(path.c_str(), nullptr, GUID_IShellItem, (void**)&item);
    if (FAILED(hr) || !item) {
        std::printf("       [debug] SHCreateItemFromParsingName hr=0x%08X\n", (unsigned)hr);
        cache->Release(); r.hr = hr; return r;
    }

    ISharedBitmap* sb = nullptr;
    WTS_CACHEFLAGS flags = WTS_DEFAULT;
    WTS_THUMBNAILID id{};
    hr = cache->GetThumbnail(item, cx, WTS_EXTRACT, &sb, &flags, &id);
    if (FAILED(hr)) std::printf("       [debug] GetThumbnail hr=0x%08X\n", (unsigned)hr);
    r.hr = hr;
    if (SUCCEEDED(hr) && sb) {
        HBITMAP hb = nullptr;                 // GetSharedBitmap 是借用，不能 DeleteObject
        WTS_ALPHATYPE at = WTSAT_UNKNOWN;
        if (SUCCEEDED(sb->GetSharedBitmap(&hb)) && hb) {
            sb->GetFormat(&at);
            r.hash = hashBitmap(hb, r);
        }
        sb->Release();
    }
    item->Release();
    cache->Release();
    r.ms = (long)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t0).count();
    return r;
}

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) {
        std::printf("verify_cache <file1> <file2> [file3 ...]\n");
        return 2;
    }
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        std::printf("FAIL CoInitializeEx\n");
        return 3;
    }

    const UINT cx = 256;
    std::vector<uint32_t> hs, hl;
    std::vector<std::string> names;

    std::printf("=== 路径A: IThumbnailProvider + IInitializeWithStream ===\n");
    for (int i = 1; i < argc; ++i) {
        Result r = thumbViaStream(argv[i], cx);
        char nm[128]{};
        WideCharToMultiByte(CP_UTF8, 0, PathFindFileNameW(argv[i]), -1, nm, sizeof(nm)-1, nullptr, nullptr);
        names.push_back(nm);
        if (!r.hash) std::printf("  [%d] %-32s FAIL 无位图\n", i, nm);
        else std::printf("  [%d] %-32s %dx%d 前景%5.1f%% hash=%08X %ldms\n",
                         i, nm, r.w, r.h, r.nonbg, r.hash, r.ms);
        hs.push_back(r.hash);
    }

    std::printf("=== 路径B: IThumbnailCache（资源管理器填充缩略图缓存所走的路）===\n");
    for (int i = 1; i < argc; ++i) {
        Result r = thumbViaShell(argv[i], cx);
        if (!r.hash) std::printf("  [%d] %-32s FAIL 无缩略图 hr=0x%08X\n", i, names[i-1].c_str(), (unsigned)r.hr);
        else std::printf("  [%d] %-32s %dx%d 前景%5.1f%% hash=%08X %ldms\n",
                         i, names[i-1].c_str(), r.w, r.h, r.nonbg, r.hash, r.ms);
        hl.push_back(r.hash);
    }

    int bad = 0;
    for (size_t a = 0; a < names.size(); ++a) {
        if (!hs[a]) { std::printf("FAIL  %s 走 provider 路径出不了图\n", names[a].c_str()); ++bad; }
        if (!hl[a]) { std::printf("FAIL  %s 走 shell 路径出不了图\n", names[a].c_str()); ++bad; }
        for (size_t b = a + 1; b < names.size(); ++b) {
            if (hs[a] && hs[a] == hs[b]) {
                std::printf("COLLIDE provider: %s == %s (hash=%08X)\n",
                            names[a].c_str(), names[b].c_str(), hs[a]); ++bad;
            }
            if (hl[a] && hl[a] == hl[b]) {
                std::printf("COLLIDE shell: %s == %s (hash=%08X)\n",
                            names[a].c_str(), names[b].c_str(), hl[a]); ++bad;
            }
        }
    }

    // 注意：这里不调用 CoUninitialize()——OCCT 静态析构在部分模型上会死锁，
    // 结论行必须在它之前输出，然后直接 ExitProcess。
    if (bad) {
        std::printf("结果: FAIL（%d 项问题）\n", bad);
        ExitProcess(1);          // OCCT 静态析构会挂住，直接退出
    }
    std::printf("结果: PASS — %d 个文件的缩略图互不相同（provider 与 shell 两条路径均通过）\n",
                (int)names.size());
    ExitProcess(0);
}
