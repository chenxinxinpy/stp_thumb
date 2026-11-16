#pragma once
// step_core.h - STEP(AP203/214/242) B-rep -> 32bpp BGRA 位图
// 纯 C++17 标准库实现，不依赖 Windows 头、不依赖任何第三方库。
// 与 step_thumb.py 的算法完全一致（含四处修正：复合实体、有向边串联、
// 圆弧 same_sense、背面线剔除）。

#include <cstdint>
#include <string>
#include <vector>

namespace step {

struct Vec3 {
    double x = 0.0, y = 0.0, z = 0.0;
    Vec3() = default;
    Vec3(double a, double b, double c) : x(a), y(b), z(c) {}
};

struct Tri {
    Vec3 a, b, c;
    Vec3 na, nb, nc;        // 顶点法线（仅 smooth=true 时有效）
    bool smooth = false;    // true = Gouraud 插值着色，false = 整片平面着色
};
struct Line { Vec3 a, b, ctr; };

struct Stats {
    size_t entities = 0;
    size_t faces = 0;
    size_t loops = 0;
    double sx = 0.0, sy = 0.0, sz = 0.0;   // 原始包围盒尺寸 (mm)
};

struct Options {
    int arc = 8;            // 每条曲线边的采样段数
    int maxTris = 20000;    // 三角形上限
    int maxLines = 6000;    // 边界线抽稀上限
    bool solid = false;     // false = wireframe（默认），true = 全部近似填充
    double angle = 0.30;    // 仅 OCCT 后端：相邻面片角度偏差(rad)，越大越省

};

struct Mesh {
    std::vector<Tri> tris;
    std::vector<Line> lines;
    Stats stats;
    bool empty() const { return tris.empty() && lines.empty(); }
    // 重心居中 + 最大边归一到 [-1,1]；成功返回 true
    bool normalize();
};

// 32bpp BGRA，自上而下逐行，alpha 恒为 255
struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> bgra;
    bool empty() const { return w <= 0 || h <= 0 || bgra.empty(); }
};

// 解析 STEP 文本并构建近似网格（线程安全，无全局状态）
Mesh buildMesh(const std::string& text, const Options& opt);

// z-buffer 软件光栅化；size 为输出正方形边长；bg 为 0x00RRGGBB
// edges=false 时不画边界线，纯实体着色（缩略图/预览默认用法）；
// 内部按尺寸自动超采样 2~3 倍再做盒式降采样，边缘无锯齿。
// cull=true 时剔除背面三角形（OCCT 网格朝向可靠，可消掉内部面与
// 外表共面时的深度打架网纹）；若剔除比例异常高（法线整体反了）会自动放弃。
Image render(const Mesh& mesh, int size, const uint32_t bg = 0x00FAFBFD,
             bool edges = true, bool cull = false);

// 读取整个文件为字符串（失败返回 false）
bool readFile(const std::wstring& path, std::string& out);

} // namespace step
