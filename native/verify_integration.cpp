// verify_integration.cpp - 验证真实 Explorer 集成的两条链路：
//  1) IShellItemImageFactory::GetImage 走系统缩略图管线（等同资源管理器大图标）
//  2) IPreviewHandler 在 prevhost surrogate 中被激活并 DoPreview（预览窗格）
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
#include <shlobj.h>
#include <ole2.h>
#include <cstdio>
#include <cwchar>

static const GUID GUID_IShellItem            = {0x43826d1e,0xe718,0x42ee,{0xbc,0x55,0xa1,0xe2,0x61,0xc3,0x7b,0xfe}};
static const GUID GUID_IShellItemImageFactory = {0xbcc18b79,0xba16,0x442f,{0x80,0xc4,0x8a,0x59,0xc3,0x0c,0x46,0x3b}};
static const GUID GUID_IPreviewHandler       = {0x8895b1c6,0xb41f,0x4c1c,{0xa5,0x62,0x0d,0x56,0x42,0x50,0x83,0x6f}};
static const GUID GUID_IInitializeWithFile   = {0xb7d14566,0x0509,0x4cce,{0xa7,0x1f,0x0a,0x55,0x42,0x33,0xbd,0x9b}};
static const GUID GUID_IOleWindow            = {0x00000114,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
static const CLSID CLSID_StepPreview        = {0x7c3a9e21,0x4f58,0x4b0a,{0x9c,0x71,0x2d,0x6e,0x8b,0x5f,0x1a,0x31}};

static const wchar_t* kHost = L"StepThumbVerifyHost";

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { std::printf("verify_integration <file.step>\n"); return 2; }
    // Shell 解析名必须是标准反斜杠绝对路径（Git Bash 可能传入 C:// 形式）
    wchar_t full[MAX_PATH]{};
    GetFullPathNameW(argv[1], MAX_PATH, full, nullptr);
    if (GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES) {
        std::printf("FAIL file not found (normalized path)\n"); return 3;
    }
    HRESULT hr = OleInitialize(nullptr);   // Shell API 需要完整 OLE 初始化
    if (FAILED(hr)) { std::printf("FAIL OleInitialize 0x%08X\n", (unsigned)hr); return 3; }
    int rc = 0;

    // ---- 1) 系统缩略图管线 ----
    std::printf("\n[1] Shell thumbnail pipeline (IShellItemImageFactory)\n");
    IShellItemImageFactory* factory = nullptr;
    hr = SHCreateItemFromParsingName(full, nullptr, GUID_IShellItemImageFactory, (void**)&factory);
    if (FAILED(hr) || !factory) {
        IShellItem* si = nullptr;
        HRESULT h0 = SHCreateItemFromParsingName(full, nullptr, GUID_IShellItem, (void**)&si);
        if (SUCCEEDED(h0) && si) {
            h0 = si->QueryInterface(GUID_IShellItemImageFactory, (void**)&factory);
            si->Release();
        }
    }
    if (FAILED(hr) && !factory) {
        std::printf("    FAIL cannot obtain IShellItemImageFactory hr=0x%08X\n", (unsigned)hr);
        rc = 4;
    } else {
        SIZE sz = {256, 256};
        HBITMAP hb = nullptr;
        // THUMBNAILONLY：只要真实缩略图，拒绝退回文件图标
        hr = factory->GetImage(sz, (SIIGBF)(SIIGBF_THUMBNAILONLY | SIIGBF_RESIZETOFIT), &hb);
        if (FAILED(hr) || !hb) {
            std::printf("    FAIL GetImage(THUMBNAILONLY) hr=0x%08X  <-- provider 未被 Shell 命中\n", (unsigned)hr);
            rc = 5;
        } else {
            BITMAP bm{};
            GetObjectW(hb, sizeof(bm), &bm);
            std::printf("    OK shell thumbnail = %ldx%ld bpp=%d\n", bm.bmWidth, bm.bmHeight, bm.bmBitsPixel);
            DeleteObject(hb);
        }
        factory->Release();
    }

    // ---- 2) 预览窗格 COM ----
    std::printf("\n[2] Preview handler (prevhost surrogate)\n");
    IPreviewHandler* ph = nullptr;
    hr = CoCreateInstance(CLSID_StepPreview, nullptr, CLSCTX_LOCAL_SERVER,
                          GUID_IPreviewHandler, (void**)&ph);
    if (FAILED(hr) || !ph) {
        std::printf("    LOCAL_SERVER hr=0x%08X, retry INPROC_SERVER\n", (unsigned)hr);
        hr = CoCreateInstance(CLSID_StepPreview, nullptr, CLSCTX_INPROC_SERVER,
                              GUID_IPreviewHandler, (void**)&ph);
    }
    if (FAILED(hr) || !ph) {
        std::printf("    FAIL CoCreateInstance preview hr=0x%08X\n", (unsigned)hr);
        if (!rc) rc = 6;
    } else {
        std::printf("    OK CoCreateInstance preview handler\n");
        IInitializeWithFile* init = nullptr;
        hr = ph->QueryInterface(GUID_IInitializeWithFile, (void**)&init);
        if (SUCCEEDED(hr) && init) {
            hr = init->Initialize(full, STGM_READ);
            std::printf("    %s IInitializeWithFile::Initialize hr=0x%08X\n",
                        SUCCEEDED(hr) ? "OK  " : "FAIL", (unsigned)hr);
            init->Release();
        } else std::printf("    FAIL QueryInterface(IInitializeWithFile)\n");

        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kHost;
        RegisterClassW(&wc);
        HWND host = CreateWindowExW(0, kHost, L"", WS_OVERLAPPEDWINDOW,
                                    0,0,640,480, nullptr,nullptr,wc.hInstance,nullptr);
        if (host) {
            RECT r = {0,0,640,480};
            hr = ph->SetWindow(host, &r);
            std::printf("    %s SetWindow hr=0x%08X\n", SUCCEEDED(hr)?"OK  ":"FAIL", (unsigned)hr);
            hr = ph->SetRect(&r);
            hr = ph->DoPreview();
            std::printf("    %s DoPreview hr=0x%08X\n", SUCCEEDED(hr)?"OK  ":"FAIL", (unsigned)hr);
            IOleWindow* ow = nullptr;
            if (SUCCEEDED(ph->QueryInterface(GUID_IOleWindow, (void**)&ow))) {
                HWND child = nullptr;
                ow->GetWindow(&child);
                std::printf("    %s IOleWindow::GetWindow child=%p\n",
                            child?"OK  ":"FAIL", (void*)child);
                ow->Release();
            }
            ph->Unload();
            DestroyWindow(host);
        }
        ph->Release();
    }

    OleUninitialize();
    std::printf("\n[RESULT] %s\n", rc ? "integration incomplete (see above)" : "ALL OK");
    return rc;
}
