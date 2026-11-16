// StepThumbSetup.exe - 单文件安装器 / 卸载器（内嵌 DLL 与 CLI）
//   StepThumbSetup.exe            安装（自动 UAC 提权）
//   StepThumbSetup.exe /uninstall 完整卸载
//   StepThumbSetup.exe /silent    静默安装
// 注册表全部写入 HKLM（用户已确认全机范围），卸载按安装清单精确回滚。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>

#include <cstdio>
#include <stdio.h>
#include <wchar.h>
#include <fstream>
#include <string>
#include <algorithm>
#include <vector>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(linker, "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")

static const wchar_t* THUMB_CLSID   = L"{7C3A9E21-4F58-4B0A-9C71-2D6E8B5F1A30}";
static const wchar_t* PREVIEW_CLSID = L"{7C3A9E21-4F58-4B0A-9C71-2D6E8B5F1A31}";
static const wchar_t* IID_THUMB    = L"{e357fccd-a995-4576-b01f-234630154e96}";
static const wchar_t* IID_PREVIEW  = L"{8895b1c6-b41f-4c1c-a562-0d564250836f}";
// 64 位 prevhost.exe surrogate（MSDN: How to Register a Preview Handler）
static const wchar_t* APPID_PREVHOST = L"{6d2b5079-2f0b-48dd-ab7f-97cec514d30b}";
static const wchar_t* APP_SUB = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\StepThumb";

static std::wstring g_dir;
static bool g_silent = false;

static void log(const std::wstring& s) {
    std::wprintf(L"%s\n", s.c_str());
    if (!g_silent) MessageBoxW(nullptr, s.c_str(), L"StepThumb", MB_OK|MB_ICONINFORMATION);
}
static void logErr(const std::wstring& s) {
    std::fwprintf(stderr, L"%s\n", s.c_str());
    if (!g_silent) MessageBoxW(nullptr, s.c_str(), L"StepThumb 安装失败", MB_OK|MB_ICONERROR);
}

static bool isAdmin() {
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    PSID sid = nullptr;
    if (!AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                  DOMAIN_ALIAS_RID_ADMINS, 0,0,0,0,0,0, &sid)) return false;
    BOOL ok = FALSE;
    if (!CheckTokenMembership(nullptr, sid, &ok)) ok = FALSE;
    FreeSid(sid);
    return ok != FALSE;
}

static bool elevateSelf(const std::wstring& params) {
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    SHELLEXECUTEINFOW si{};
    si.cbSize = sizeof(si);
    si.lpVerb = L"runas";
    si.lpFile = self;
    si.lpParameters = params.c_str();
    si.nShow = SW_HIDE;
    si.fMask = SEE_MASK_NOCLOSEPROCESS;
    if (!ShellExecuteExW(&si)) return false;
    if (si.hProcess) { WaitForSingleObject(si.hProcess, INFINITE); CloseHandle(si.hProcess); }
    return true;
}

static std::wstring installDir() {
    wchar_t buf[1024]{};
    DWORD n = GetEnvironmentVariableW(L"ProgramW6432", buf, 1024);
    if (!n || n >= 1024) n = GetEnvironmentVariableW(L"ProgramFiles", buf, 1024);
    std::wstring root = (n && n < 1024) ? buf : L"C:\\Program Files";
    return root + L"\\StepThumb";
}

static bool writeFile(const std::wstring& path, const void* data, size_t len) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        // 旧 DLL 可能被残留的映像句柄占用（explorer/prevhost 刚被杀掉时常见），
        // 此时直接覆盖会 ERROR_SHARING_VIOLATION，但重命名是允许的：先挪走再写。
        // 用带序号的临时名：上一次留下的 .old 可能自己也被句柄占用，不能复用同一个名字
        std::wstring old;
        for (int i = 0; i < 20; ++i) {
            old = path + (i ? L".old" + std::to_wstring(i) : L".old");
            if (DeleteFileW(old.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND) break;
        }
        if (MoveFileExW(path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        }
        if (h == INVALID_HANDLE_VALUE) return false;
    }
    DWORD wrote = 0;
    BOOL ok = WriteFile(h, data, (DWORD)len, &wrote, nullptr);
    CloseHandle(h);
    return ok && wrote == len;
}

static bool extract(int id, const std::wstring& path) {
    HMODULE mod = GetModuleHandleW(nullptr);
    HRSRC r = FindResourceW(mod, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!r) return false;
    HGLOBAL g = LoadResource(mod, r);
    if (!g) return false;
    void* p = LockResource(g);
    DWORD sz = SizeofResource(mod, r);
    if (!p || !sz) return false;
    return writeFile(path, p, sz);
}

static bool getStr(HKEY root, const std::wstring& sub, const wchar_t* name, std::wstring& out) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(root, sub.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    wchar_t buf[2048]{};
    DWORD sz = sizeof(buf), type = 0;
    LONG r = RegQueryValueExW(k, name, nullptr, &type, (BYTE*)buf, &sz);
    RegCloseKey(k);
    if (r != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return false;
    out = buf;
    return true;
}

static bool setStr(HKEY root, const std::wstring& sub, const wchar_t* name,
                   const std::wstring& value, std::vector<std::wstring>& manifest) {
    std::wstring old;
    if (getStr(root, sub, name, old) && !old.empty() && old != value) {
        std::wprintf(L"跳过已有注册（不覆盖）：%s [%s] = %s\n", sub.c_str(),
                     name ? name : L"(默认)", old.c_str());
        return false;                              // 绝不覆盖别人注册的 handler
    }
    HKEY k = nullptr;
    DWORD disp = 0;
    if (RegCreateKeyExW(root, sub.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
                        KEY_SET_VALUE|KEY_READ, nullptr, &k, &disp) != ERROR_SUCCESS) return false;
    LONG r = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)value.c_str(),
                            (DWORD)((value.size()+1)*sizeof(wchar_t)));
    RegCloseKey(k);
    if (r != ERROR_SUCCESS) return false;
    if (disp == REG_CREATED_NEW_KEY) manifest.push_back(L"K " + sub);
    else manifest.push_back(L"V " + sub + L"|" + (name ? name : L""));
    return true;
}

static void delTree(HKEY root, const std::wstring& sub) { RegDeleteTreeW(root, sub.c_str()); }

static bool removeDirTree(const std::wstring& dir) {
    std::vector<std::wstring> stack{dir};
    std::vector<std::wstring> dirs;
    while (!stack.empty()) {
        std::wstring cur = stack.back(); stack.pop_back();
        dirs.push_back(cur);
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((cur + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            std::wstring p = cur + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) stack.push_back(p);
            else { SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL); DeleteFileW(p.c_str()); }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    for (auto it = dirs.rbegin(); it != dirs.rend(); ++it) RemoveDirectoryW(it->c_str());
    return !PathIsDirectoryW(dir.c_str());
}

// 把 setup 同级的 runtime\*.dll（OCCT 及其依赖）复制到安装目录。
// OCCT 的 DLL 无法编译进单个二进制，只能作为随附文件一起落地。
static int copyRuntime(const std::wstring& dst) {
    wchar_t me[MAX_PATH]{};
    GetModuleFileNameW(nullptr, me, MAX_PATH);
    std::wstring src(me);
    size_t p = src.find_last_of(L'\\');
    if (p != std::wstring::npos) src = src.substr(0, p);
    src += L"\\runtime";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((src + L"\\*.dll").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (CopyFileW((src + L"\\" + fd.cFileName).c_str(),
                      (dst + L"\\" + fd.cFileName).c_str(), FALSE)) ++n;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

static void notifyShell() {
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

// ---------------------------------------------------------------- 安装
static int doInstall() {
    if (!isAdmin()) {
        std::wprintf(L"需要管理员权限，正在请求 UAC 提权...\n");
        return elevateSelf(g_silent ? L"/silent" : L"") ? 0 : 5;
    }
    g_dir = installDir();
    if (!CreateDirectoryW(g_dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        logErr(L"无法创建安装目录：" + g_dir); return 6;
    }
    const std::wstring dll = g_dir + L"\\StepThumb.dll";
    const std::wstring cli = g_dir + L"\\step2png.exe";
    const std::wstring self = g_dir + L"\\StepThumbSetup.exe";
    if (!extract(101, dll)) { logErr(L"内嵌 DLL 提取失败"); return 7; }
    if (!extract(102, cli)) { logErr(L"内嵌 CLI 提取失败"); return 8; }
    wchar_t me[MAX_PATH]{};
    GetModuleFileNameW(nullptr, me, MAX_PATH);
    if (_wcsicmp(me, self.c_str()) != 0) CopyFileW(me, self.c_str(), FALSE);
    const int nrt = copyRuntime(g_dir);

    std::vector<std::wstring> manifest;
    HKEY HKLM_ = HKEY_LOCAL_MACHINE;
    const std::wstring cls = L"SOFTWARE\\Classes\\";
    const std::wstring sfa = cls + L"SystemFileAssociations\\";

    // 1) 缩略图 provider：扩展名直挂 + SystemFileAssociations（不动已有 ProgID / 打开方式）
    for (const wchar_t* ext : {L".stp", L".step"}) {
        std::wstring e(ext);
        setStr(HKLM_, cls + e + L"\\shellex\\" + IID_THUMB, nullptr, THUMB_CLSID, manifest);
        setStr(HKLM_, sfa + e + L"\\shellex\\" + IID_THUMB, nullptr, THUMB_CLSID, manifest);
        setStr(HKLM_, cls + e + L"\\shellex\\" + IID_PREVIEW, nullptr, PREVIEW_CLSID, manifest);
        setStr(HKLM_, sfa + e + L"\\shellex\\" + IID_PREVIEW, nullptr, PREVIEW_CLSID, manifest);
    }
    // 2) 独立 ProgID 兜底（不设为 .stp 的默认值，避免改变双击打开方式）
    setStr(HKLM_, cls + L"StepThumb.step\\shellex\\" + IID_THUMB, nullptr, THUMB_CLSID, manifest);
    setStr(HKLM_, cls + L"StepThumb.step\\shellex\\" + IID_PREVIEW, nullptr, PREVIEW_CLSID, manifest);

    // 3) COM 类注册
    setStr(HKLM_, cls + L"CLSID\\" + THUMB_CLSID, nullptr, L"StepThumb STEP Thumbnail Provider", manifest);
    setStr(HKLM_, cls + L"CLSID\\" + THUMB_CLSID + L"\\InprocServer32", nullptr, dll, manifest);
    setStr(HKLM_, cls + L"CLSID\\" + THUMB_CLSID + L"\\InprocServer32", L"ThreadingModel", L"Apartment", manifest);
    setStr(HKLM_, cls + L"CLSID\\" + PREVIEW_CLSID, nullptr, L"StepThumb STEP Preview Handler", manifest);
    setStr(HKLM_, cls + L"CLSID\\" + PREVIEW_CLSID, L"AppID", APPID_PREVHOST, manifest);
    setStr(HKLM_, cls + L"CLSID\\" + PREVIEW_CLSID + L"\\InprocServer32", nullptr, dll, manifest);
    setStr(HKLM_, cls + L"CLSID\\" + PREVIEW_CLSID + L"\\InprocServer32", L"ThreadingModel", L"Apartment", manifest);

    // 4) 预览窗格清单
    const std::wstring ph = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\PreviewHandlers\\" + std::wstring(PREVIEW_CLSID);
    setStr(HKLM_, ph, nullptr, L"StepThumb STEP Preview Handler", manifest);

    // 5) 卸载入口
    const std::wstring uninst = L"\"" + self + L"\" /uninstall /silent";
    setStr(HKLM_, APP_SUB, L"DisplayName", L"StepThumb - STEP 缩略图与预览", manifest);
    setStr(HKLM_, APP_SUB, L"DisplayVersion", L"1.0.0", manifest);
    setStr(HKLM_, APP_SUB, L"Publisher", L"INTAMSYS 增材制造中心", manifest);
    setStr(HKLM_, APP_SUB, L"InstallLocation", g_dir, manifest);
    setStr(HKLM_, APP_SUB, L"UninstallString", uninst, manifest);
    setStr(HKLM_, APP_SUB, L"NoModify", L"1", manifest);
    setStr(HKLM_, APP_SUB, L"NoRepair", L"1", manifest);

    // 6) 安装清单（合并历史记录，重复安装不丢失键信息）
    std::vector<std::wstring> all = manifest;
    const std::wstring mfPath = g_dir + L"\\install-manifest.txt";
    FILE* old = _wfopen(mfPath.c_str(), L"rb");
    if (old) {
        char buf[2048];
        while (fgets(buf, sizeof(buf), old)) {
            std::string l(buf);
            while (!l.empty() && (l.back()=='\n' || l.back()=='\r')) l.pop_back();
            if (l.size() >= 2) all.push_back(std::wstring(l.begin(), l.end()));
        }
        fclose(old);
    }
    std::sort(all.begin(), all.end());
    all.erase(std::unique(all.begin(), all.end()), all.end());
    FILE* mf = _wfopen(mfPath.c_str(), L"wb");
    if (mf) {
        for (const auto& line : all) {
            std::string a(line.begin(), line.end());
            fwrite(a.c_str(), 1, a.size(), mf);
            fputc('\n', mf);
        }
        fclose(mf);
    }
    notifyShell();
    std::wstring msg = L"安装完成。\n\n安装目录：" + g_dir +
        L"\n已注册 .stp / .step 的缩略图与预览窗格（全机 HKLM）。\n" +
        L"若图标未立即变化，请重启资源管理器或注销后重新登录。";
    log(msg);
    std::wprintf(L"随附运行时 %d 个 DLL；注册表条目 %zu 条，清单：%s\\install-manifest.txt\n",
                 nrt, manifest.size(), g_dir.c_str());
    return 0;
}

// ---------------------------------------------------------------- 卸载
static int doUninstall() {
    if (!isAdmin()) {
        std::wprintf(L"需要管理员权限，正在请求 UAC 提权...\n");
        return elevateSelf(L"/uninstall /silent") ? 0 : 5;
    }
    g_dir = installDir();
    // 按安装清单逐项回滚
    std::vector<std::wstring> keys, vals;
    FILE* mf = _wfopen((g_dir + L"\\install-manifest.txt").c_str(), L"rb");
    if (mf) {
        char buf[2048];
        while (fgets(buf, sizeof(buf), mf)) {
            std::string line(buf);
            while (!line.empty() && (line.back()=='\n' || line.back()=='\r')) line.pop_back();
            if (line.size() < 2) continue;
            std::wstring w(line.begin(), line.end());
            if (line[0] == 'K') keys.push_back(w.substr(2));
            else if (line[0] == 'V') vals.push_back(w.substr(2));
        }
        fclose(mf);
    }
    for (const auto& s : vals) {
        size_t bar = s.rfind(L'|');
        if (bar == std::wstring::npos) continue;
        std::wstring sub = s.substr(0, bar), name = s.substr(bar+1);
        HKEY k = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, sub.c_str(), 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
            RegDeleteValueW(k, name.empty() ? nullptr : name.c_str());
            RegCloseKey(k);
        }
    }
    for (auto it = keys.rbegin(); it != keys.rend(); ++it)
        delTree(HKEY_LOCAL_MACHINE, *it);

    // 兜底：无论清单是否丢失，清掉本组件专属的键
    const std::wstring cls = L"SOFTWARE\\Classes\\";
    const std::wstring sfa = cls + L"SystemFileAssociations\\";
    // 清理本组件专属的 shellex 子键；若默认值已被别的程序接管则原样保留
    auto delIfOurs = [&](const std::wstring& sub, const wchar_t* expected) {
        std::wstring cur;
        if (getStr(HKEY_LOCAL_MACHINE, sub, nullptr, cur) && !cur.empty() && cur != expected) {
            std::wprintf(L"保留非本组件注册：%s = %s\n", sub.c_str(), cur.c_str());
            return;
        }
        delTree(HKEY_LOCAL_MACHINE, sub);
    };
    const wchar_t* exts[2] = {L".stp", L".step"};
    for (int i = 0; i < 2; ++i) {
        std::wstring e(exts[i]);
        delIfOurs(cls + e + L"\\shellex\\" + IID_THUMB, THUMB_CLSID);
        delIfOurs(sfa + e + L"\\shellex\\" + IID_THUMB, THUMB_CLSID);
        delIfOurs(cls + e + L"\\shellex\\" + IID_PREVIEW, PREVIEW_CLSID);
        delIfOurs(sfa + e + L"\\shellex\\" + IID_PREVIEW, PREVIEW_CLSID);
    }
    delTree(HKEY_LOCAL_MACHINE, cls + L"CLSID\\" + THUMB_CLSID);
    delTree(HKEY_LOCAL_MACHINE, cls + L"CLSID\\" + PREVIEW_CLSID);
    delTree(HKEY_LOCAL_MACHINE, cls + L"StepThumb.step");
    delTree(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\PreviewHandlers\\" + std::wstring(PREVIEW_CLSID));
    delTree(HKEY_LOCAL_MACHINE, APP_SUB);
    notifyShell();

    // 从 Temp 启动清理副本，删除整个安装目录（含占用中的自身）
    wchar_t tmp[MAX_PATH]{}, me[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring helper = std::wstring(tmp) + L"StepThumbCleanup.exe";
    GetModuleFileNameW(nullptr, me, MAX_PATH);
    CopyFileW(me, helper.c_str(), FALSE);
    std::wstring params = L"/cleanup \"" + g_dir + L"\"";
    SHELLEXECUTEINFOW si{};
    si.cbSize = sizeof(si);
    si.lpFile = helper.c_str();
    si.lpParameters = params.c_str();
    si.nShow = SW_HIDE;
    si.fMask = SEE_MASK_NOCLOSEPROCESS;
    if (ShellExecuteExW(&si)) { if (si.hProcess) CloseHandle(si.hProcess); }
    else removeDirTree(g_dir);
    log(L"卸载完成。注册表项已清除，安装目录正在清理。");
    return 0;
}

static int doCleanup(const std::wstring& dir) {
    for (int i = 0; i < 20 && PathIsDirectoryW(dir.c_str()); ++i) {
        removeDirTree(dir);
        Sleep(100);
    }
    wchar_t me[MAX_PATH]{};
    GetModuleFileNameW(nullptr, me, MAX_PATH);
    MoveFileExW(me, nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    bool uninstall = false, cleanup = false;
    std::wstring cleanupDir;
    for (int i = 1; i < argc; ++i) {
        if (!_wcsicmp(argv[i], L"/silent")) g_silent = true;
        else if (!_wcsicmp(argv[i], L"/uninstall")) uninstall = true;
        else if (!_wcsicmp(argv[i], L"/cleanup")) { cleanup = true; if (i+1 < argc) cleanupDir = argv[i+1]; }
    }
    if (cleanup) return doCleanup(cleanupDir);
    if (uninstall) return doUninstall();
    return doInstall();
}
