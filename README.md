# stp_thumb

Windows 资源管理器的 STEP/STP 模型缩略图与预览窗格外壳扩展（x64 COM，`IThumbnailProvider` + `IPreviewHandler`）。

把 `.stp` / `.step` 文件用 OCCT 读成 B-rep、三角化后软件光栅化成缩略图，无需安装 CATIA / SolidWorks 即可在文件夹里直接看到模型长什么样。

## 功能

- `.stp` / `.step` 文件夹缩略图（256px，实体着色）
- 资源管理器右侧预览窗格（最大 1024px）
- 两种解析后端：OCCT（默认，精确）与纯 C++ 自研解析器（`STEPTHUMB_ENGINE=native`）
- 配套 `step2png.exe` 命令行工具，便于脱离 Shell 单独验证渲染

## 目录

| 路径 | 说明 |
|---|---|
| `native/StepThumb.cpp` | COM 外壳扩展主体（缩略图 provider + 预览 handler + 类厂） |
| `native/step_core.cpp/.h` | 纯 C++ STEP 文本解析 + z-buffer 软件光栅化 |
| `native/step_occt.cpp/.h` | OCCT B-rep 三角化后端 |
| `native/Setup.cpp` | 安装/卸载程序（写注册表、注册 CLSID） |
| `native/verify_cache.cpp` | **回归测试**：缩略图串台检查（见下文） |
| `native/step2png.cpp` | 命令行 STEP → PNG |
| `native/build-msvc-dll.sh` | MSVC + OCCT 构建脚本（含回归测试的编译） |
| `step_thumb.py` | 早期 Python 原型（保留供参考） |
| `before.log` / `after.log` | 串台 bug 修复前后的验证日志 |

## 构建

依赖 MSVC 2022 BuildTools、Windows SDK 与 OCCT（放在 `_deps/occtenv/Library`）。

> 注意：`vcvarsall.bat` 会调用 `reg.exe`。如果环境不允许，使用仓库自带的 `native/msvc-env.sh` 直接设置 `INCLUDE` / `LIB` / `PATH`。

```bash
cd native
bash build-msvc-dll.sh
```

产物在 `native/msvc-occt/`：`StepThumb.dll`、`step2png.exe`、`verify_cache.exe`。

## 安装

```bat
StepThumbSetup.exe        :: 注册 COM 组件与 .stp/.step 外壳扩展
dist\清空缩略图缓存.bat    :: 换 DLL 后清缓存并重启 explorer.exe
```

安装位置默认为 `C:\Program Files\StepThumb`。替换 DLL 时如果提示占用，先结束 `explorer.exe`，把旧 DLL 改名再拷入新文件。

## 回归测试：缩略图串台

历史 bug：`GetThumbnail` 在流初始化路径下把缓存键写成了常量 `L"#stream"`，导致同一进程内所有 `.stp` 文件共用一条缓存，文件夹里所有缩略图都变成第一个模型的样子。

`verify_cache.exe` 同时对两条路径做检查：

1. `IThumbnailProvider` + `IInitializeWithStream`（bug 现场）
2. `IThumbnailCache::GetThumbnail(..., WTS_EXTRACT)`（资源管理器填充缓存的实际路径）

任意两个不同文件得到相同位图哈希即判 FAIL。

```bat
verify_cache.exe file1.stp file2.stp file3.stp
```

实测结果见 `before.log`（全部 COLLIDE）与 `after.log`（PASS，4 个文件哈希互不相同）。

## 许可

仅在自己的机器上使用，按需修改。
