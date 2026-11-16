// step_occt.cpp - OpenCASCADE 三角化后端
//
// 与我们自研的 STEP 解析器相比，OCCT 走的是真正的 B-rep 离散：
//   STEPControl_Reader → TopoDS_Shape → BRepMesh_IncrementalMesh
// 每个面得到合法的三角网（弦高偏差 + 角度偏差双控制），曲面片是真正的
// 网格而不是"边界环投影耳切"，因此圆角、圆柱、B 样条曲面都会平滑。
//
// 着色用的顶点法线直接对解析曲面求 D1 后做叉积，比三角形法线平均更准，
// 平面面片自动退化为硬边（相邻面之间保留折边）。

#include "step_occt.h"

#include <STEPControl_Reader.hxx>
#include <DESTEP_Parameters.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <BRepBndLib.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepTools.hxx>
#include <Bnd_Box.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopLoc_Location.hxx>
#include <Poly_Triangulation.hxx>
#include <Standard_Version.hxx>
#include <gp_Pnt.hxx>
#include <gp_Pnt2d.hxx>
#include <gp_Vec.hxx>
#include <gp_Trsf.hxx>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <sstream>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace step {

namespace {

inline Vec3 toVec(const gp_Pnt& p) { return Vec3(p.X(), p.Y(), p.Z()); }

inline Vec3 sub3(const Vec3& a, const Vec3& b) { return Vec3(a.x - b.x, a.y - b.y, a.z - b.z); }
inline Vec3 crs(const Vec3& a, const Vec3& b) {
    return Vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
inline double len3(const Vec3& a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
inline Vec3 nrm3(const Vec3& a) {
    double l = len3(a);
    return (l > 1e-12) ? Vec3(a.x / l, a.y / l, a.z / l) : Vec3(0, 0, 0);
}

// 统计三角数（用于偏差自适应迭代）
size_t countTris(const TopoDS_Shape& shape) {
    size_t n = 0;
    for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next()) {
        TopLoc_Location loc;
        Handle(Poly_Triangulation) tr = BRep_Tool::Triangulation(TopoDS::Face(ex.Current()), loc);
        if (!tr.IsNull()) n += (size_t)tr->NbTriangles();
    }
    return n;
}

} // namespace

bool occtVersion(std::string& out) {
    out = OCC_VERSION_COMPLETE;
    return true;
}

bool buildMeshOCCT(const std::string& text, const Options& opt, Mesh& out) {
    if (text.size() < 64) return false;

    STEPControl_Reader reader;
    std::istringstream in(text);
    DESTEP_Parameters prm;
    prm.ReadColor = false; prm.ReadName = false; prm.ReadLayer = false;
    prm.ReadProps = false; prm.ReadMetadata = false; prm.ReadProductMetadata = false;
    // 说明：surfacecurve / bspline.continuity 两个参数实测对本机语料没有加速效果，
    // 保留默认以免影响几何精度。读入耗时主要来自 OCCT 构建 B-rep 本身（约 4ms/面）。
    if (reader.ReadStream("memory.step", prm, in) != IFSelect_RetDone) return false;
    if (reader.TransferRoots() <= 0) return false;
    const TopoDS_Shape shape = reader.OneShape();
    if (shape.IsNull()) return false;

    Bnd_Box box;
    BRepBndLib::Add(shape, box);
    if (box.IsVoid()) return false;
    double x1, y1, z1, x2, y2, z2;
    box.Get(x1, y1, z1, x2, y2, z2);
    const double dx = x2 - x1, dy = y2 - y1, dz = z2 - z1;
    const double diag = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (!(diag > 0.0) || !std::isfinite(diag)) return false;

    // 弦高偏差：先从"偏粗"起步，按实测三角数反推目标偏差再细化一次。
    // 若一上来就给很细（如 diag*0.0012），复杂件会先产出几十万三角，
    // 分网耗时反而飙升到十几秒——这是最初 1075 面件 22 秒的主因。
    const int triCap = (opt.maxTris > 5000) ? opt.maxTris : 5000;
    double defl = diag * 0.0035;
    if (defl < 1e-4) defl = 1e-4;

    const double ang = (opt.angle > 0.05 && opt.angle < 1.2) ? opt.angle : 0.30;
    const bool trace = (std::getenv("STEPTHUMB_TRACE") != nullptr);
    // BRepMesh_IncrementalMesh 只会"加密"不会"放松"：偏差调大并不会让已分好的面重分，
    // 所以超额时先用 BRepTools::Clean 清空网格再按估算偏差重分一次（实测这一步很关键，
    // 否则循环三次全在重复劳动，1075 面件会从 7 秒涨到 25 秒）。
    auto tA = std::chrono::steady_clock::now();
    // 两遍策略：先按包围盒给个粗偏差快速分一遍，得到实际三角数后按
    // tris ∝ 1/defl² 反推目标偏差，再清空重分一次，让结果落在目标三角数附近。
    // 注意 BRepMesh_IncrementalMesh 只会加密不会放松，所以重分前必须 BRepTools::Clean。
    auto doMesh = [&](double d) -> size_t {
        BRepTools::Clean(shape);
        BRepMesh_IncrementalMesh m(shape, d, Standard_False, ang, Standard_True);
        return countTris(shape);
    };
    size_t ntri = doMesh(defl);
    if (ntri == 0) return false;
    if (trace) { std::fprintf(stderr, "  [occt] pass1 defl=%.4f ang=%.2f tris=%zu\n", defl, ang, ntri); std::fflush(stderr); }
    const double want = (double)triCap;
    if ((double)ntri < want * 0.6 || (double)ntri > want * 1.05) {
        double d2 = defl * std::sqrt((double)ntri / want);
        if (d2 < defl * 0.15) d2 = defl * 0.15;     // 防极端细化
        ntri = doMesh(d2);
        defl = d2;
        if (trace) { std::fprintf(stderr, "  [occt] pass2 defl=%.4f tris=%zu\n", defl, ntri); std::fflush(stderr); }
    }
    if (ntri == 0) return false;
    auto tB = std::chrono::steady_clock::now();
    if (trace) { std::fprintf(stderr, "  [occt] mesh=%lldms\n",
        (long long)std::chrono::duration_cast<std::chrono::milliseconds>(tB-tA).count()); std::fflush(stderr); }

    out.tris.clear();
    out.lines.clear();
    out.tris.reserve(std::min<size_t>(ntri, (size_t)triCap));

    size_t faces = 0, loops = 0;
    for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next()) {
        ++faces;
        const TopoDS_Face face = TopoDS::Face(ex.Current());
        TopLoc_Location loc;
        Handle(Poly_Triangulation) tr = BRep_Tool::Triangulation(face, loc);
        if (tr.IsNull() || tr->NbTriangles() < 1) continue;

        const gp_Trsf& tf = loc.Transformation();
        const bool hasTf = !loc.IsIdentity();
        const bool rev = (face.Orientation() == TopAbs_REVERSED);

        // 顶点坐标（已应用装配变换）
        const int nb = tr->NbNodes();
        std::vector<Vec3> pts((size_t)nb + 1);
        for (int i = 1; i <= nb; ++i) {
            gp_Pnt p = tr->Node(i);
            if (hasTf) p.Transform(tf);
            pts[(size_t)i] = toVec(p);
        }

        // 解析曲面法线：D1 的两个切向叉积；平面片跳过（走硬边）
        BRepAdaptor_Surface ad(face);
        const bool planar = (ad.GetType() == GeomAbs_Plane);
        std::vector<Vec3> nrm((size_t)nb + 1);
        std::vector<char> hasN((size_t)nb + 1, 0);
        if (!planar && tr->HasUVNodes()) {
            for (int i = 1; i <= nb; ++i) {
                gp_Pnt2d uv = tr->UVNode(i);
                gp_Pnt P;
                gp_Vec du, dv;
                bool ok = false;
                try {
                    ad.D1(uv.X(), uv.Y(), P, du, dv);
                    ok = true;
                } catch (...) { ok = false; }
                if (!ok) continue;
                Vec3 v = nrm3(crs(Vec3(du.X(), du.Y(), du.Z()), Vec3(dv.X(), dv.Y(), dv.Z())));
                if (len3(v) < 0.5) continue;
                if (hasTf) {
                    gp_Dir d(v.x, v.y, v.z);
                    d.Transform(tf);
                    v = Vec3(d.X(), d.Y(), d.Z());
                }
                if (rev) v = Vec3(-v.x, -v.y, -v.z);
                nrm[(size_t)i] = v;
                hasN[(size_t)i] = 1;
            }
        }

        const Handle(Poly_Triangulation)& t = tr;
        for (int k = 1; k <= t->NbTriangles(); ++k) {
            int a, b, c;
            t->Triangle(k).Get(a, b, c);
            if (rev) std::swap(b, c);
            Tri tri;
            tri.a = pts[(size_t)a];
            tri.b = pts[(size_t)b];
            tri.c = pts[(size_t)c];

            const bool smooth = (!planar) && hasN[(size_t)a] && hasN[(size_t)b] && hasN[(size_t)c];
            if (smooth) {
                tri.na = nrm[(size_t)a];
                tri.nb = nrm[(size_t)b];
                tri.nc = nrm[(size_t)c];
                tri.smooth = true;
            } else {
                Vec3 g = nrm3(crs(sub3(tri.b, tri.a), sub3(tri.c, tri.a)));
                tri.na = tri.nb = tri.nc = g;
                tri.smooth = false;
            }
            out.tris.push_back(tri);
            if (out.tris.size() >= (size_t)triCap) break;
        }
        if (out.tris.size() >= (size_t)triCap) break;
    }

    if (trace) { std::fprintf(stderr, "  [occt] extract=%lldms tris=%zu\n",
        (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-tB).count(),
        out.tris.size()); std::fflush(stderr); }
    if (out.tris.empty()) return false;
    out.stats.entities = 0;
    out.stats.faces = faces;
    out.stats.loops = loops;
    out.stats.sx = dx;
    out.stats.sy = dy;
    out.stats.sz = dz;
    return true;
}

bool buildMeshOCCTFile(const std::wstring& path, const Options& opt, Mesh& out) {
    std::string text;
    if (!readFile(path, text)) return false;
    return buildMeshOCCT(text, opt, out);
}

} // namespace step
