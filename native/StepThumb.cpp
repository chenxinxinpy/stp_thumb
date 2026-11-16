// StepThumb.cpp - 原生 64 位 COM 外壳扩展
//   IThumbnailProvider  : 资源管理器 .stp/.step 缩略图
//   IPreviewHandler     : 资源管理器右侧预览窗格
// 所有 STEP 解析都在异常保护内执行；任何失败只返回错误码，不拖累宿主进程。
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX            // 否则 windows.h 的 min/max 宏会破坏 std::min/std::max
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
#include <synchapi.h>

#include <atomic>
#include <string>
#include <vector>
#include <algorithm>
#include <new>

#include "step_core.h"
#ifdef USE_OCCT
#include "step_occt.h"
#endif

#ifndef __GNUC__
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "propsys.lib")
#endif

// {7C3A9E21-4F58-4B0A-9C71-2D6E8B5F1A30} 缩略图 provider
static const CLSID CLSID_StepThumb = { 0x7c3a9e21,0x4f58,0x4b0a,{0x9c,0x71,0x2d,0x6e,0x8b,0x5f,0x1a,0x30} };
// {7C3A9E21-4F58-4B0A-9C71-2D6E8B5F1A31} 预览窗格 handler
static const CLSID CLSID_StepPreview = { 0x7c3a9e21,0x4f58,0x4b0a,{0x9c,0x71,0x2d,0x6e,0x8b,0x5f,0x1a,0x31} };

// 自定义 IID（避免 __uuidof 的编译器差异，MSVC / MinGW 通用）
static const GUID GUID_IUnknown              = {0x00000000,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
static const GUID GUID_IClassFactory         = {0x00000001,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
static const GUID GUID_IThumbnailProvider    = {0xe357fccd,0xa995,0x4576,{0xb0,0x1f,0x23,0x46,0x30,0x15,0x4e,0x96}};
static const GUID GUID_IInitializeWithFile   = {0xb7d14566,0x0509,0x4cce,{0xa7,0x1f,0x0a,0x55,0x42,0x33,0xbd,0x9b}};
static const GUID GUID_IInitializeWithStream = {0xb824b49d,0x22ac,0x4161,{0xac,0x8a,0x99,0x16,0xe8,0xfa,0x3f,0x7f}};
static const GUID GUID_IPreviewHandler       = {0x8895b1c6,0xb41f,0x4c1c,{0xa5,0x62,0x0d,0x56,0x42,0x50,0x83,0x6f}};
static const GUID GUID_IPreviewHandlerVisuals= {0x196bf9a5,0xb346,0x4ef0,{0xaa,0x1e,0x5d,0xcd,0xb7,0x67,0x68,0xb1}};
static const GUID GUID_IObjectWithSite       = {0xfc4801a3,0x2ba9,0x11cf,{0xa2,0x29,0x00,0xaa,0x00,0x3d,0x73,0x52}};
static const GUID GUID_IOleWindow            = {0x00000114,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};

#ifndef WTSAT_ARGB
#define WTSAT_ARGB 2
#endif

static HINSTANCE g_hInst = nullptr;
// 诊断开关：设 STEPTHUMB_ENGINE=native 可强制用自研解析器（排查 OCCT 问题时用）
static bool g_forceNative = false;
static std::atomic<LONG> g_objects(0);
static std::atomic<LONG> g_locks(0);
static SRWLOCK g_cacheLock = SRWLOCK_INIT;

struct CacheEntry {
    std::wstring key;
    FILETIME ft{};
    int size = 0;
    int w = 0, h = 0;
    std::vector<uint8_t> bgra;
};
static std::vector<CacheEntry> g_cache;

extern "C" BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hInst = h;
        DisableThreadLibraryCalls(h);
        wchar_t buf[32]{};
        if (GetEnvironmentVariableW(L"STEPTHUMB_ENGINE", buf, 32) > 0)
            g_forceNative = (_wcsicmp(buf, L"native") == 0);
    }
    return TRUE;
}

// --------------------------------------------------------------- 通用工具
static std::wstring fileKey(const std::wstring& path) {
    FILETIME ft{};
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) ft = fad.ftLastWriteTime;
    return path + L"|" + std::to_wstring(ft.dwHighDateTime) + L":" + std::to_wstring(ft.dwLowDateTime);
}

// 【修复】流初始化时拿不到文件路径，缓存键必须退化成「内容哈希」。
// 原来这里用的是常量 L"#stream"，导致同一进程内所有文件共用同一条缓存：
// 第一个文件渲染完写入缓存，之后每个文件都直接命中，缩略图全部变成第一个模型的样子。
// 这正是「同一文件夹下所有模型缩略图都一样」的原因。
static std::wstring contentKey(const std::string& data) {
    uint64_t h = 1469598103934665603ull;           // FNV-1a 64 位
    for (unsigned char c : data) { h ^= c; h *= 1099511628211ull; }
    return L"#c" + std::to_wstring(h) + L"|" + std::to_wstring(data.size());
}

static bool cached(const std::wstring& key, int size, step::Image& img) {
    bool found = false;
    AcquireSRWLockShared(&g_cacheLock);
    for (const auto& c : g_cache) {
        if (c.key == key && c.size == size) {
            img.w = c.w; img.h = c.h; img.bgra = c.bgra; found = true; break;
        }
    }
    ReleaseSRWLockShared(&g_cacheLock);
    return found;
}

static void putCache(const std::wstring& key, int size, const step::Image& img) {
    AcquireSRWLockExclusive(&g_cacheLock);
    if (g_cache.size() >= 8) g_cache.erase(g_cache.begin());
    CacheEntry c;
    c.key = key; c.size = size; c.w = img.w; c.h = img.h; c.bgra = img.bgra;
    g_cache.push_back(std::move(c));
    ReleaseSRWLockExclusive(&g_cacheLock);
}

// 解析 + 渲染，绝不抛异常到 COM 边界
static bool makeImage(const std::string& text, int size, step::Image& img) {
    try {
        if (text.size() > 64u*1024u*1024u) return false;
        step::Options opt;
        // 三角上限/角度偏差按输出尺寸分档：缩略图 256 用 30k 已经肉眼看不出折面，
        // 预览窗格（~900px）给到 15 万才能撑住大图细节。
        opt.maxTris = (size <= 300) ? 30000 : (size <= 700 ? 70000 : 150000);
        opt.angle   = (size <= 300) ? 0.45 : (size <= 700 ? 0.30 : 0.22);
        opt.solid = true;                       // 实体着色（与 stl-thumb 观感一致）
        opt.maxLines = std::max(600, std::min(3000, size*5));

        step::Mesh mesh;
        bool occt = false;
#ifdef USE_OCCT
        if (!g_forceNative) occt = step::buildMeshOCCT(text, opt, mesh);
#endif
        if (!occt) mesh = step::buildMesh(text, opt);
        if (mesh.empty()) return false;
        mesh.normalize();
        // 三角化失败（无有效闭环）时退回线框，避免出现空白图
        img = mesh.tris.empty() ? step::render(mesh, size, 0x00FAFBFD, true)
                                : step::render(mesh, size, 0x00FAFBFD, false, occt);
        return !img.empty();
    } catch (...) { return false; }
}

static bool readStream(IStream* st, std::string& out) {
    if (!st) return false;
    STATSTG stg{};
    if (FAILED(st->Stat(&stg, STATFLAG_NONAME))) return false;
    LARGE_INTEGER zero{};
    if (FAILED(st->Seek(zero, STREAM_SEEK_SET, nullptr))) return false;
    ULONGLONG total = (ULONGLONG)stg.cbSize.QuadPart;
    if (total == 0 || total > 64ull*1024ull*1024ull) return false;
    out.resize((size_t)total);
    ULONG got = 0, done = 0;
    HRESULT hr = st->Read(&out[0], (ULONG)total, &got);
    done = got;
    while (SUCCEEDED(hr) && done < total) {
        got = 0;
        hr = st->Read(&out[done], (ULONG)(total-done), &got);
        if (got == 0) break;
        done += got;
    }
    out.resize(done);
    return done > 0;
}

// 由 32bpp BGRA 生成 HBITMAP（top-down DIB）
static HBITMAP makeBitmap(const step::Image& img) {
    if (img.empty()) return nullptr;
    BITMAPINFOHEADER bh{};
    bh.biSize = sizeof(BITMAPINFOHEADER);
    bh.biWidth = img.w;
    bh.biHeight = -img.h;              // 负 = top-down
    bh.biPlanes = 1;
    bh.biBitCount = 32;
    bh.biCompression = BI_RGB;
    BITMAPINFO bi{};
    bi.bmiHeader = bh;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP hb = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!hb || !bits) return hb;
    memcpy(bits, img.bgra.data(), img.bgra.size());
    return hb;
}

// --------------------------------------------------------------- 缩略图 provider
class ThumbProvider : public IThumbnailProvider,
                      public IInitializeWithFile,
                      public IInitializeWithStream {
    std::atomic<LONG> ref_{1};
    std::wstring path_;
    std::string content_;
public:
    ThumbProvider() { ++g_objects; }
    virtual ~ThumbProvider() { --g_objects; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (IsEqualIID(riid, GUID_IUnknown) || IsEqualIID(riid, GUID_IThumbnailProvider))
            *ppv = static_cast<IThumbnailProvider*>(this);
        else if (IsEqualIID(riid, GUID_IInitializeWithFile))
            *ppv = static_cast<IInitializeWithFile*>(this);
        else if (IsEqualIID(riid, GUID_IInitializeWithStream))
            *ppv = static_cast<IInitializeWithStream*>(this);
        if (!*ppv) return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = --ref_;
        if (!r) delete this;
        return (ULONG)r;
    }

    HRESULT STDMETHODCALLTYPE Initialize(LPCWSTR pszFilePath, DWORD) override {
        if (!pszFilePath) return E_INVALIDARG;
        try { path_ = pszFilePath; content_.clear(); return S_OK; }
        catch (...) { return E_OUTOFMEMORY; }
    }
    HRESULT STDMETHODCALLTYPE Initialize(IStream* pstream, DWORD) override {
        try { return readStream(pstream, content_) ? S_OK : E_FAIL; }
        catch (...) { return E_FAIL; }
    }

    HRESULT STDMETHODCALLTYPE GetThumbnail(UINT cx, HBITMAP* phbmp, WTS_ALPHATYPE* pdwAlpha) override {
        if (!phbmp || !pdwAlpha) return E_POINTER;
        *phbmp = nullptr;
        *pdwAlpha = (WTS_ALPHATYPE)WTSAT_ARGB;
        try {
            int size = (int)cx;
            if (size < 16) size = 16;
            if (size > 1024) size = 1024;
            step::Image img;
            // 【修复】原来：path_.empty() ? L"#stream" : fileKey(path_)
            // 常量键会让流初始化的所有文件串台，改为内容哈希。
            std::wstring key = !path_.empty() ? fileKey(path_) : contentKey(content_);
            if (!cached(key, size, img)) {
                std::string text = content_;
                if (text.empty() && !path_.empty()) step::readFile(path_, text);
                if (text.empty() || !makeImage(text, size, img)) return E_FAIL;
                putCache(key, size, img);
            }
            HBITMAP hb = makeBitmap(img);
            if (!hb) return E_FAIL;
            *phbmp = hb;
            return S_OK;
        } catch (...) { return E_FAIL; }
    }
};

// --------------------------------------------------------------- 预览窗格
static const wchar_t* kWndClass = L"StepThumbPreviewWnd";
static bool g_classReady = false;
class PreviewHandler;
static LRESULT CALLBACK PreviewWndProc(HWND, UINT, WPARAM, LPARAM);

static void ensureClass() {
    if (g_classReady) return;
    WNDCLASSW wc{};
    wc.lpfnWndProc = PreviewWndProc;
    wc.hInstance = g_hInst;
    wc.lpszClassName = kWndClass;
    wc.hbrBackground = nullptr;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    if (RegisterClassW(&wc)) g_classReady = true;
}

class PreviewHandler : public IPreviewHandler,
                       public IPreviewHandlerVisuals,
                       public IObjectWithSite,
                       public IOleWindow,
                       public IInitializeWithFile,
                       public IInitializeWithStream {
    std::atomic<LONG> ref_{1};
    std::wstring path_;
    std::string content_;
    HWND parent_ = nullptr, hwnd_ = nullptr;
    RECT rect_{};
    IUnknown* site_ = nullptr;
    step::Image img_;
    bool loaded_ = false;
public:
    PreviewHandler() { ++g_objects; }
    virtual ~PreviewHandler() { Unload(); if (site_) { site_->Release(); site_ = nullptr; } --g_objects; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (IsEqualIID(riid, GUID_IUnknown) || IsEqualIID(riid, GUID_IPreviewHandler))
            *ppv = static_cast<IPreviewHandler*>(this);
        else if (IsEqualIID(riid, GUID_IPreviewHandlerVisuals))
            *ppv = static_cast<IPreviewHandlerVisuals*>(this);
        else if (IsEqualIID(riid, GUID_IObjectWithSite))
            *ppv = static_cast<IObjectWithSite*>(this);
        else if (IsEqualIID(riid, GUID_IOleWindow))
            *ppv = static_cast<IOleWindow*>(this);
        else if (IsEqualIID(riid, GUID_IInitializeWithFile))
            *ppv = static_cast<IInitializeWithFile*>(this);
        else if (IsEqualIID(riid, GUID_IInitializeWithStream))
            *ppv = static_cast<IInitializeWithStream*>(this);
        if (!*ppv) return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = --ref_;
        if (!r) delete this;
        return (ULONG)r;
    }

    void Paint(HDC dc) {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        HBRUSH br = CreateSolidBrush(RGB(250,251,253));
        FillRect(dc, &rc, br);
        DeleteObject(br);
        if (img_.empty()) return;
        int cw = rc.right-rc.left, ch = rc.bottom-rc.top;
        if (cw <= 0 || ch <= 0) return;
        int side = (int)(std::min(cw,ch)*0.92);
        if (side < 16) return;
        int x = (cw-side)/2, y = (ch-side)/2;
        BITMAPINFOHEADER bh{};
        bh.biSize = sizeof(BITMAPINFOHEADER);
        bh.biWidth = img_.w;
        bh.biHeight = -img_.h;
        bh.biPlanes = 1;
        bh.biBitCount = 32;
        bh.biCompression = BI_RGB;
        BITMAPINFO bi{}; bi.bmiHeader = bh;
        StretchDIBits(dc, x,y,side,side, 0,0,img_.w,img_.h,
                      img_.bgra.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
    }

    // IInitializeWith*
    HRESULT STDMETHODCALLTYPE Initialize(LPCWSTR pszFilePath, DWORD) override {
        if (!pszFilePath) return E_INVALIDARG;
        try { path_ = pszFilePath; content_.clear(); return S_OK; }
        catch (...) { return E_OUTOFMEMORY; }
    }
    HRESULT STDMETHODCALLTYPE Initialize(IStream* pstream, DWORD) override {
        try { return readStream(pstream, content_) ? S_OK : E_FAIL; }
        catch (...) { return E_FAIL; }
    }

    // IPreviewHandler
    HRESULT STDMETHODCALLTYPE SetWindow(HWND hwnd, const RECT* prc) override {
        parent_ = hwnd;
        if (prc) rect_ = *prc;
        if (hwnd_) {
            SetParent(hwnd_, parent_);
            MoveWindow(hwnd_, rect_.left, rect_.top,
                       rect_.right-rect_.left, rect_.bottom-rect_.top, TRUE);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetRect(const RECT* prc) override {
        if (!prc) return E_INVALIDARG;
        rect_ = *prc;
        if (hwnd_) MoveWindow(hwnd_, rect_.left, rect_.top,
                              rect_.right-rect_.left, rect_.bottom-rect_.top, TRUE);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DoPreview() override {
        try {
            if (!parent_) return E_FAIL;
            ensureClass();
            if (!g_classReady) return E_FAIL;
            if (!hwnd_) {
                hwnd_ = CreateWindowExW(0, kWndClass, L"", WS_CHILD|WS_VISIBLE|WS_CLIPCHILDREN,
                    rect_.left, rect_.top,
                    std::max(1L,rect_.right-rect_.left), std::max(1L,rect_.bottom-rect_.top),
                    parent_, nullptr, g_hInst, nullptr);
                if (!hwnd_) return E_FAIL;
                SetWindowLongPtrW(hwnd_, GWLP_USERDATA, (LONG_PTR)this);
            }
            int size = std::min(rect_.right-rect_.left, rect_.bottom-rect_.top);
            if (size < 64) size = 512;
            if (size > 1024) size = 1024;
            std::string text = content_;
            if (text.empty() && !path_.empty()) step::readFile(path_, text);
            if (text.empty() || !makeImage(text, size, img_)) return E_FAIL;
            loaded_ = true;
            InvalidateRect(hwnd_, nullptr, TRUE);
            return S_OK;
        } catch (...) { return E_FAIL; }
    }
    HRESULT STDMETHODCALLTYPE Unload() override {
        if (hwnd_) { SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0); DestroyWindow(hwnd_); hwnd_ = nullptr; }
        img_ = step::Image();
        content_.clear();
        loaded_ = false;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetFocus() override {
        if (hwnd_) ::SetFocus(hwnd_);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryFocus(HWND* phwnd) override {
        if (!phwnd) return E_POINTER;
        *phwnd = hwnd_ ? hwnd_ : GetFocus();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE TranslateAccelerator(MSG*) override { return S_FALSE; }

    // IPreviewHandlerVisuals
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(COLORREF) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetFont(const LOGFONTW*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetTextColor(COLORREF) override { return S_OK; }

    // IObjectWithSite
    HRESULT STDMETHODCALLTYPE SetSite(IUnknown* pSite) override {
        if (site_) { site_->Release(); site_ = nullptr; }
        site_ = pSite;
        if (site_) site_->AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetSite(REFIID riid, void** ppvSite) override {
        if (!ppvSite) return E_POINTER;
        *ppvSite = nullptr;
        return site_ ? site_->QueryInterface(riid, ppvSite) : E_FAIL;
    }

    // IOleWindow
    HRESULT STDMETHODCALLTYPE GetWindow(HWND* phwnd) override {
        if (!phwnd) return E_POINTER;
        *phwnd = hwnd_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ContextSensitiveHelp(BOOL) override { return E_NOTIMPL; }
};

static LRESULT CALLBACK PreviewWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        auto* self = (PreviewHandler*)GetWindowLongPtrW(h, GWLP_USERDATA);
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (self) self->Paint(dc);
        else FillRect(dc, &ps.rcPaint, (HBRUSH)(COLOR_WINDOW+1));
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_ERASEBKGND) return 1;
    return DefWindowProcW(h, m, w, l);
}

// --------------------------------------------------------------- 类厂
class ClassFactory : public IClassFactory {
    std::atomic<LONG> ref_{1};
    CLSID clsid_;
public:
    explicit ClassFactory(CLSID c) : clsid_(c) { ++g_objects; }
    virtual ~ClassFactory() { --g_objects; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (IsEqualIID(riid, GUID_IUnknown) || IsEqualIID(riid, GUID_IClassFactory))
            *ppv = static_cast<IClassFactory*>(this);
        if (!*ppv) return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = --ref_; if (!r) delete this; return (ULONG)r;
    }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        try {
            if (IsEqualCLSID(clsid_, CLSID_StepThumb)) {
                ThumbProvider* p = new ThumbProvider();
                HRESULT hr = p->QueryInterface(riid, ppv);
                p->Release();
                return hr;
            }
            if (IsEqualCLSID(clsid_, CLSID_StepPreview)) {
                PreviewHandler* p = new PreviewHandler();
                HRESULT hr = p->QueryInterface(riid, ppv);
                p->Release();
                return hr;
            }
        } catch (...) { return E_OUTOFMEMORY; }
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override {
        if (lock) ++g_locks; else --g_locks;
        return S_OK;
    }
};

#ifdef _MSC_VER
#define ST_EXPORT          // MSVC 用链接器 /EXPORT 导出，避免与 combaseapi.h 声明冲突
#else
#define ST_EXPORT __declspec(dllexport)
#endif

extern "C" ST_EXPORT HRESULT STDMETHODCALLTYPE DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (!IsEqualCLSID(rclsid, CLSID_StepThumb) && !IsEqualCLSID(rclsid, CLSID_StepPreview))
        return CLASS_E_CLASSNOTAVAILABLE;
    if (!IsEqualIID(riid, GUID_IClassFactory) && !IsEqualIID(riid, GUID_IUnknown))
        return E_NOINTERFACE;
    try {
        ClassFactory* f = new ClassFactory(rclsid);
        HRESULT hr = f->QueryInterface(riid, ppv);
        f->Release();
        return hr;
    } catch (...) { return E_OUTOFMEMORY; }
}

extern "C" ST_EXPORT HRESULT STDMETHODCALLTYPE DllCanUnloadNow() {
    return (g_objects.load() == 0 && g_locks.load() == 0) ? S_OK : S_FALSE;
}
