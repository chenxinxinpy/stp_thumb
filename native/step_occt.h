#pragma once
// step_occt.h - 用 OpenCASCADE(OCCT) 做 B-rep 三角化（比自研解析细腻得多）
// 仅在 MSVC + OCCT 头文件可用时编译；MinGW 构建会自动跳过本模块。

#include "step_core.h"
#include <string>

namespace step {

// 从 STEP 文本构建网格：OCCT 精确三角化 + 解析曲面法线（Gouraud 平滑）
// 失败返回 false（调用方可回退到自研解析器）
bool buildMeshOCCT(const std::string& text, const Options& opt, Mesh& out);

// 从文件构建（内部读文件后转调上面的重载）
bool buildMeshOCCTFile(const std::wstring& path, const Options& opt, Mesh& out);

// OCCT 是否真的在工作（仅用于日志/自检）
bool occtVersion(std::string& out);

} // namespace step
