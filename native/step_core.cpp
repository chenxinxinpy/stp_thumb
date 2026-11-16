// step_core.cpp - STEP(AP203/214/242) B-rep -> 32bpp BGRA 位图
// 纯 C++17 标准库。算法与 step_thumb.py 逐行对应，便于回归对照。
#include "step_core.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <stdio.h>
#include <wchar.h>
#include <unordered_map>

namespace step {

// ------------------------------------------------------------------ 向量工具
static inline Vec3 add(Vec3 a, Vec3 b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
static inline Vec3 sub(Vec3 a, Vec3 b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
static inline Vec3 mul(Vec3 a, double k) { return {a.x*k, a.y*k, a.z*k}; }
static inline double dot(Vec3 a, Vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static inline Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x};
}
static inline Vec3 norm(Vec3 a) {
    double d = std::sqrt(dot(a, a));
    return d > 1e-12 ? mul(a, 1.0/d) : Vec3(0.0, 0.0, 1.0);
}
static inline double dist(Vec3 a, Vec3 b) { return std::sqrt(std::max(dot(sub(a,b), sub(a,b)), 0.0)); }
static inline bool close(Vec3 a, Vec3 b, double tol) { return dot(sub(a,b), sub(a,b)) < tol*tol; }
static inline bool validTri(const Tri& t) {
    Vec3 n = cross(sub(t.b,t.a), sub(t.c,t.a));
    return dot(n,n) > 1e-14;
}
static inline bool isSpace(char c) { return c==' '||c=='\t'||c=='\r'||c=='\n'; }

// ------------------------------------------------------------------ STEP 词法层
enum class VT { Null, Ref, Num, Str, List, Enum };

struct Value {
    VT t = VT::Null;
    int ref = 0;
    double num = 0.0;
    std::string str;
    std::vector<Value> list;
};

struct Entity {
    std::string kind;
    std::vector<Value> args;
};

using Entities = std::unordered_map<int, Entity>;

static inline bool isRef(const Value& v) { return v.t == VT::Ref; }
static inline bool isList(const Value& v) { return v.t == VT::List; }
static inline int refId(const Value& v) { return isRef(v) ? v.ref : -1; }

static double asNum(const Value& v, double def = 0.0) {
    if (v.t == VT::Num) return v.num;
    if (v.t == VT::Str || v.t == VT::Enum) {
        char* end = nullptr;
        double d = std::strtod(v.str.c_str(), &end);
        if (end && *end == '\0') return d;
    }
    return def;
}

static std::string upper(std::string s) {
    for (char& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

static bool truthy(const Value& v) {
    std::string s;
    if (v.t == VT::Str || v.t == VT::Enum) s = upper(v.str);
    else if (v.t == VT::Num) s = std::to_string(v.num);
    else return false;
    return s == ".T." || s == "T" || s == "TRUE" || s == "1";
}

// 从字符串 s[a,b) 中按顶层逗号切分，返回各段 [start,end)
static std::vector<std::pair<size_t,size_t>> splitTop(const std::string& s, size_t a, size_t b) {
    std::vector<std::pair<size_t,size_t>> out;
    int depth = 0;
    bool instr = false;
    size_t start = a;
    for (size_t i = a; i < b; ++i) {
        char c = s[i];
        if (instr) {
            if (c == '\'') {
                if (i+1 < b && s[i+1] == '\'') { ++i; continue; }
                instr = false;
            }
        } else {
            if (c == '\'') instr = true;
            else if (c == '(') ++depth;
            else if (c == ')') --depth;
            else if (c == ',' && depth == 0) { out.emplace_back(start, i); start = i+1; }
        }
    }
    out.emplace_back(start, b);
    return out;
}

// 从 '(' 位置 start 扫描到配对的 ')'（跳过字符串内的括号）
static size_t scanClose(const std::string& s, size_t start) {
    int depth = 0;
    bool instr = false;
    for (size_t j = start; j < s.size(); ++j) {
        char c = s[j];
        if (instr) {
            if (c == '\'') {
                if (j+1 < s.size() && s[j+1] == '\'') { ++j; continue; }
                instr = false;
            }
        } else {
            if (c == '\'') instr = true;
            else if (c == '(') ++depth;
            else if (c == ')') { --depth; if (depth == 0) return j; }
        }
    }
    return s.size() - 1;
}

static Value parseValue(const std::string& s, size_t a, size_t b);
static std::vector<Value> parseArgs(const std::string& s, size_t a, size_t b);

static std::vector<Value> parseArgs(const std::string& s, size_t a, size_t b) {
    std::vector<Value> out;
    for (auto seg : splitTop(s, a, b)) out.push_back(parseValue(s, seg.first, seg.second));
    return out;
}

static bool parseDouble(const std::string& p, double& out) {
    if (p.empty()) return false;
    char* end = nullptr;
    double d = std::strtod(p.c_str(), &end);
    if (!end || end != p.c_str() + p.size()) return false;
    out = d;
    return true;
}

static Value parseValue(const std::string& s, size_t a, size_t b) {
    while (a < b && isSpace(s[a])) ++a;
    while (b > a && isSpace(s[b-1])) --b;
    Value v;
    if (a >= b) return v;
    std::string p = s.substr(a, b-a);
    if (p == "*" || p == "$") return v;                       // Null
    if (p.front() == '(' && p.back() == ')') {
        v.t = VT::List;
        v.list = parseArgs(p, 1, p.size()-1);
        return v;
    }
    if (p.front() == '\'' && p.back() == '\'' && p.size() >= 2) {
        v.t = VT::Str;
        std::string q = p.substr(1, p.size()-2);
        std::string r;
        for (size_t i = 0; i < q.size(); ++i) {
            r += q[i];
            if (q[i] == '\'' && i+1 < q.size() && q[i+1] == '\'') ++i;
        }
        v.str = r;
        return v;
    }
    if (p.front() == '#') {
        char* end = nullptr;
        long id = std::strtol(p.c_str()+1, &end, 10);
        if (end && *end == '\0') { v.t = VT::Ref; v.ref = (int)id; return v; }
        v.t = VT::Str; v.str = p; return v;
    }
    double d;
    if (parseDouble(p, d)) { v.t = VT::Num; v.num = d; return v; }
    if (p.front() == '.') v.t = VT::Enum;
    else v.t = VT::Str;
    v.str = (p.front() == '.') ? upper(p) : p;
    return v;
}

// 解析 '#id = ...;'，兼容简单实体与 ISO-10303-21 复合实体
//   #42040 =( BOUNDED_SURFACE() B_SPLINE_SURFACE(...) );
static Entities parseEntities(const std::string& text) {
    Entities ents;
    const size_t n = text.size();
    size_t i = 0;
    while (i < n) {
        if (text[i] != '#' || i+1 >= n || !std::isdigit((unsigned char)text[i+1])) { ++i; continue; }
        size_t j = i+1;
        long id = 0;
        while (j < n && std::isdigit((unsigned char)text[j])) { id = id*10 + (text[j]-'0'); ++j; }
        size_t k = j;
        while (k < n && isSpace(text[k])) ++k;
        if (k >= n || text[k] != '=') { i = j; continue; }

        // 扫到 depth==0 的分号
        size_t p = k+1, q = p;
        int depth = 0;
        bool instr = false;
        while (q < n) {
            char c = text[q];
            if (instr) {
                if (c == '\'') {
                    if (q+1 < n && text[q+1] == '\'') { q += 2; continue; }
                    instr = false;
                }
            } else {
                if (c == '\'') instr = true;
                else if (c == '(') ++depth;
                else if (c == ')') --depth;
                else if (c == ';' && depth == 0) break;
            }
            ++q;
        }
        size_t bs = p, be = q;
        while (bs < be && isSpace(text[bs])) ++bs;
        while (be > bs && isSpace(text[be-1])) --be;
        i = (q < n) ? q+1 : n;

        if (bs >= be) continue;
        bool compound = (text[bs] == '(');
        // 找 NAME( 的位置；复合实体取最后一个，简单实体取第一个
        size_t nameStart = std::string::npos, nameEnd = 0, paren = std::string::npos;
        for (size_t m = bs; m < be; ++m) {
            char c = text[m];
            if (!(std::isalpha((unsigned char)c) || c == '_')) continue;
            size_t e2 = m;
            while (e2 < be && (std::isalnum((unsigned char)text[e2]) || text[e2] == '_')) ++e2;
            size_t e3 = e2;
            while (e3 < be && isSpace(text[e3])) ++e3;
            if (e3 < be && text[e3] == '(') {
                if (!compound) { nameStart = m; nameEnd = e2; paren = e3; break; }
                nameStart = m; nameEnd = e2; paren = e3;   // 复合：持续更新到最后一个
            }
            m = e2 > m ? e2-1 : m;
        }
        if (nameStart == std::string::npos) continue;
        size_t en = scanClose(std::string(text, bs, be-bs), paren-bs) + bs;
        if (en <= paren) continue;
        Entity ent;
        ent.kind = upper(std::string(text, nameStart, nameEnd-nameStart));
        ent.args = parseArgs(text, paren+1, en);
        ents[(int)id] = std::move(ent);
    }
    return ents;
}

// ------------------------------------------------------------------ 几何实体
static std::optional<Vec3> point(const Entities& e, int r) {
    if (r < 0) return std::nullopt;
    auto it = e.find(r);
    if (it == e.end()) return std::nullopt;
    const Entity& ent = it->second;
    const auto& a = ent.args;
    if (ent.kind == "CARTESIAN_POINT" && a.size() > 1 && isList(a[1])) {
        const auto& L = a[1].list;
        if (L.size() >= 3) return Vec3(asNum(L[0]), asNum(L[1]), asNum(L[2]));
    }
    if (ent.kind == "VERTEX_POINT" && a.size() > 1) return point(e, refId(a[1]));
    return std::nullopt;
}

static std::optional<Vec3> direction(const Entities& e, int r) {
    if (r < 0) return std::nullopt;
    auto it = e.find(r);
    if (it == e.end()) return std::nullopt;
    const auto& a = it->second.args;
    if (a.size() > 1 && isList(a[1])) {
        const auto& L = a[1].list;
        if (L.size() >= 3) return norm(Vec3(asNum(L[0]), asNum(L[1]), asNum(L[2])));
    }
    return std::nullopt;
}

struct Placement { Vec3 c, ax, u, v; };

static std::optional<Placement> placement(const Entities& e, int r) {
    if (r < 0) return std::nullopt;
    auto it = e.find(r);
    if (it == e.end()) return std::nullopt;
    const auto& a = it->second.args;
    if (a.size() < 4) return std::nullopt;
    Vec3 c  = point(e, refId(a[1])).value_or(Vec3(0.0,0.0,0.0));
    Vec3 ax = direction(e, refId(a[2])).value_or(Vec3(0.0,0.0,1.0));
    Vec3 rx = direction(e, refId(a[3])).value_or(Vec3(1.0,0.0,0.0));
    Vec3 u = sub(rx, mul(ax, dot(rx, ax)));
    if (dot(u,u) < 1e-12) u = (std::fabs(ax.z) > 0.9) ? Vec3(1,0,0) : Vec3(0,0,1);
    u = norm(u);
    return Placement{c, ax, u, norm(cross(ax,u))};
}

static const double PI = 3.14159265358979323846;
static double pyMod(double x, double m) { double r = std::fmod(x,m); if (r < 0) r += m; return r; }

// 按 EDGE_CURVE.same_sense 选圆弧方向，而不是一律取短弧
static std::vector<Vec3> arc(const Entities& e, int cid, Vec3 p1, Vec3 p2,
                             int n, double r2opt, bool hasR2, bool sense) {
    auto it = e.find(cid);
    if (it == e.end()) return {p1,p2};
    const auto& a = it->second.args;
    if (a.size() < 3) return {p1,p2};
    auto pl = placement(e, refId(a[1]));
    if (!pl) return {p1,p2};
    double r1 = asNum(a[2], 0.0);
    double r2 = hasR2 ? r2opt : r1;
    if (r1 <= 0 || r2 <= 0) return {p1,p2};
    auto ang = [&](Vec3 p) {
        Vec3 q = sub(p, pl->c);
        return std::atan2(dot(q, pl->v)/std::max(r2,1e-9), dot(q, pl->u)/std::max(r1,1e-9));
    };
    double a1 = ang(p1), a2 = ang(p2);
    double d = a2 - a1;
    if (std::fabs(d) < 1e-7) d = 2*PI;
    else if (sense) { d = pyMod(d, 2*PI); if (d < 1e-7) d = 2*PI; }
    else { d = -pyMod(-d, 2*PI); if (d > -1e-7) d = -2*PI; }
    std::vector<Vec3> pts;
    pts.reserve(n+1);
    for (int i = 0; i <= n; ++i) {
        double t = a1 + d*i/n;
        pts.push_back(add(pl->c, add(mul(pl->u, r1*std::cos(t)), mul(pl->v, r2*std::sin(t)))));
    }
    pts.front() = p1; pts.back() = p2;
    return pts;
}

static std::vector<Vec3> bspline(const Entities& e, int cid, Vec3 p1, Vec3 p2, int n) {
    auto it = e.find(cid);
    if (it == e.end()) return {p1,p2};
    std::vector<Vec3> ctrl;
    for (const auto& group : it->second.args) {
        if (!isList(group)) continue;
        for (const auto& x : group.list) {
            if (isRef(x)) { auto p = point(e, x.ref); if (p) ctrl.push_back(*p); }
        }
    }
    if (ctrl.size() < 2) return {p1,p2};
    for (int pass = 0; pass < 2; ++pass) {      // Chaikin 平滑
        std::vector<Vec3> q;
        q.push_back(ctrl.front());
        for (size_t i = 0; i+1 < ctrl.size(); ++i) {
            Vec3 A = ctrl[i], B = ctrl[i+1];
            q.push_back(Vec3(0.75*A.x+0.25*B.x, 0.75*A.y+0.25*B.y, 0.75*A.z+0.25*B.z));
            q.push_back(Vec3(0.25*A.x+0.75*B.x, 0.25*A.y+0.75*B.y, 0.25*A.z+0.75*B.z));
        }
        q.push_back(ctrl.back());
        ctrl.swap(q);
    }
    std::vector<Vec3> pts;
    pts.reserve(n+1);
    for (int i = 0; i <= n; ++i) {
        size_t k = (size_t)std::lround(i*(double)(ctrl.size()-1)/n);
        pts.push_back(ctrl[std::min(ctrl.size()-1, k)]);
    }
    pts.front() = p1; pts.back() = p2;
    return pts;
}

static std::vector<int> refsOf(const std::vector<Value>& a) {
    std::vector<int> rs;
    for (const auto& x : a) if (isRef(x)) rs.push_back(x.ref);
    return rs;
}

static std::vector<Vec3> edgePoints(const Entities& e, int eid, int arc_n) {
    auto it = e.find(eid);
    if (it == e.end() || it->second.kind != "EDGE_CURVE") return {};
    const auto& a = it->second.args;
    std::vector<int> rs = refsOf(a);
    if (rs.size() < 3) return {};
    auto p1 = point(e, rs[0]), p2 = point(e, rs[1]);
    if (!p1 || !p2) return {};
    int cid = rs[2];
    bool sense = truthy(a.back());
    auto cit = e.find(cid);
    std::string kind = (cit == e.end()) ? std::string() : cit->second.kind;
    const std::vector<Value> ca = (cit == e.end()) ? std::vector<Value>() : cit->second.args;
    if (kind.find("CIRCLE") != std::string::npos)
        return arc(e, cid, *p1, *p2, arc_n, 0, false, sense);
    if (kind.find("ELLIPSE") != std::string::npos && ca.size() > 3)
        return arc(e, cid, *p1, *p2, arc_n, asNum(ca[3],0), true, sense);
    if (kind.find("B_SPLINE_CURVE") != std::string::npos)
        return bspline(e, cid, *p1, *p2, std::min(arc_n,10));
    if (kind == "TRIMMED_CURVE") {
        for (const auto& x : ca) {
            if (isRef(x)) {
                auto t = e.find(x.ref);
                if (t != e.end() && t->second.kind.find("B_SPLINE") != std::string::npos)
                    return bspline(e, x.ref, *p1, *p2, std::min(arc_n,10));
            }
        }
    }
    return {*p1, *p2};
}

static std::vector<Vec3> orientedPoints(const Entities& e, int oid, int arc_n) {
    auto it = e.find(oid);
    if (it == e.end()) return {};
    const auto& a = it->second.args;
    std::vector<int> rs = refsOf(a);
    if (rs.empty()) return {};
    std::vector<Vec3> seg = edgePoints(e, rs.back(), arc_n);
    if (!truthy(a.back())) std::reverse(seg.begin(), seg.end());
    return seg;
}

// 按端点重新串联 EDGE_LOOP 内的有向边（STEP 文件里的排列顺序并不总是首尾相接）
static bool loopData(const Entities& e, int lid, int arc_n,
                     std::vector<Vec3>& chain, std::vector<std::vector<Vec3>>& ordered) {
    auto it = e.find(lid);
    if (it == e.end()) return false;
    const auto& a = it->second.args;
    if (a.size() < 2 || !isList(a[1])) return false;
    std::vector<std::vector<Vec3>> segs;
    for (const auto& x : a[1].list) {
        if (isRef(x)) {
            auto s = orientedPoints(e, x.ref, arc_n);
            if (s.size() >= 2) segs.push_back(std::move(s));
        }
    }
    if (segs.empty()) return false;
    double mn[3] = {1e300,1e300,1e300}, mx[3] = {-1e300,-1e300,-1e300};
    for (const auto& s : segs) for (const auto& p : s) {
        double v[3] = {p.x,p.y,p.z};
        for (int i=0;i<3;++i) { mn[i]=std::min(mn[i],v[i]); mx[i]=std::max(mx[i],v[i]); }
    }
    double span = std::max(mx[0]-mn[0], std::max(mx[1]-mn[1], mx[2]-mn[2]));
    double tol = std::max(0.02, span*2e-5);
    std::vector<bool> used(segs.size(), false);
    chain = segs[0];
    ordered.push_back(segs[0]);
    used[0] = true;
    Vec3 cur = chain.back();
    for (size_t step = 0; step < segs.size(); ++step) {
        size_t pick = segs.size();
        bool rev = false;
        for (size_t k = 0; k < segs.size(); ++k) {
            if (used[k]) continue;
            if (close(segs[k].front(), cur, tol)) { pick=k; rev=false; break; }
            if (close(segs[k].back(), cur, tol))  { pick=k; rev=true;  break; }
        }
        if (pick == segs.size()) break;
        std::vector<Vec3> seg = segs[pick];
        if (rev) std::reverse(seg.begin(), seg.end());
        chain.insert(chain.end(), seg.begin()+1, seg.end());
        ordered.push_back(seg);
        used[pick] = true;
        cur = chain.back();
    }
    std::vector<Vec3> clean;
    clean.reserve(chain.size());
    for (const auto& p : chain) if (clean.empty() || !close(p, clean.back(), tol)) clean.push_back(p);
    if (clean.size() > 2 && close(clean.front(), clean.back(), tol)) clean.pop_back();
    chain.swap(clean);
    return chain.size() >= 3;
}

// ------------------------------------------------------------------ 三角化
static std::vector<Tri> fanTris(const std::vector<Vec3>& pts) {
    std::vector<Tri> out;
    if (pts.size() < 3) return out;
    Vec3 c(0,0,0);
    for (const auto& p : pts) c = add(c, p);
    c = mul(c, 1.0/pts.size());
    for (size_t i=0;i<pts.size();++i) {
        Tri t{c, pts[i], pts[(i+1)%pts.size()]};
        if (validTri(t)) out.push_back(t);
    }
    return out;
}

static std::vector<Vec3> resample(const std::vector<Vec3>& pts, int m) {
    if ((int)pts.size() == m) return pts;
    std::vector<double> cum(pts.size(), 0.0);
    for (size_t i=1;i<pts.size();++i) cum[i] = cum[i-1] + dist(pts[i-1], pts[i]);
    double total = cum.back();
    if (total <= 1e-12 || m < 2) return std::vector<Vec3>(m, pts.front());
    std::vector<Vec3> out;
    out.reserve(m);
    size_t j = 0;
    for (int k=0;k<m;++k) {
        double target = total*k/(m-1);
        while (j < cum.size()-2 && cum[j+1] < target) ++j;
        double t = (target-cum[j])/std::max(cum[j+1]-cum[j], 1e-12);
        Vec3 A=pts[j], B=pts[j+1];
        out.push_back(Vec3(A.x+(B.x-A.x)*t, A.y+(B.y-A.y)*t, A.z+(B.z-A.z)*t));
    }
    return out;
}

// 同一片面内的顶点法线平均：曲面片得到平滑过渡，平面片法线完全一致，
// 相当于自动区分"该平滑的地方"和"该出硬边的地方"。
static void smoothNormals(std::vector<Tri>& tris) {
    if (tris.size() < 2) return;
    struct VN { Vec3 p, n; };
    std::vector<VN> vs;
    vs.reserve(tris.size()*3);
    auto slot = [&](Vec3 p) -> size_t {
        for (size_t i=0;i<vs.size();++i) if (close(p, vs[i].p, 1e-9)) return i;
        vs.push_back({p, Vec3(0,0,0)});
        return vs.size()-1;
    };
    std::vector<size_t> idx(tris.size()*3);
    for (size_t i=0;i<tris.size();++i) {
        const Tri& t = tris[i];
        Vec3 fn = cross(sub(t.b,t.a), sub(t.c,t.a));   // 长度 = 2*面积，天然按面积加权
        size_t i0=slot(t.a), i1=slot(t.b), i2=slot(t.c);
        idx[i*3+0]=i0; idx[i*3+1]=i1; idx[i*3+2]=i2;
        vs[i0].n = add(vs[i0].n, fn);
        vs[i1].n = add(vs[i1].n, fn);
        vs[i2].n = add(vs[i2].n, fn);
    }
    for (auto& v : vs) v.n = norm(v.n);
    for (size_t i=0;i<tris.size();++i) {
        Tri& t = tris[i];
        t.na = vs[idx[i*3+0]].n;
        t.nb = vs[idx[i*3+1]].n;
        t.nc = vs[idx[i*3+2]].n;
        Vec3 fn = norm(cross(sub(t.b,t.a), sub(t.c,t.a)));
        t.smooth = (dot(t.na,fn) < 0.99999 || dot(t.nb,fn) < 0.99999 || dot(t.nc,fn) < 0.99999);
    }
}

// 平面多边形：拟合平面 + 耳切（凹多边形也正确），大环先按弧长抽稀
static std::vector<Tri> polygonTris(std::vector<Vec3> pts, int maxPts = 72) {
    if (pts.size() < 3) return {};
    if ((int)pts.size() > maxPts) {
        std::vector<Vec3> loop = pts;
        loop.push_back(pts.front());
        pts = resample(loop, maxPts);
        if (!pts.empty()) pts.pop_back();
    }
    const int n = (int)pts.size();
    if (n < 3) return {};
    Vec3 N(0,0,0);                                  // Newell 法
    for (int i=0;i<n;++i) {
        Vec3 p = pts[i], q = pts[(i+1)%n];
        N = Vec3(N.x+(p.y-q.y)*(p.z+q.z), N.y+(p.z-q.z)*(p.x+q.x), N.z+(p.x-q.x)*(p.y+q.y));
    }
    if (dot(N,N) < 1e-12) return fanTris(pts);
    N = norm(N);
    Vec3 ref = (std::fabs(N.x) < 0.9) ? Vec3(1,0,0) : Vec3(0,1,0);
    Vec3 u = norm(sub(ref, mul(N, dot(ref,N))));
    Vec3 v = cross(N, u);
    Vec3 o = pts[0];
    std::vector<std::pair<double,double>> uv(n);
    for (int i=0;i<n;++i) { Vec3 q = sub(pts[i],o); uv[i] = {dot(q,u), dot(q,v)}; }
    double area = 0;
    for (int i=0;i<n;++i) area += uv[i].first*uv[(i+1)%n].second - uv[(i+1)%n].first*uv[i].second;
    if (area < 0) { std::reverse(uv.begin(),uv.end()); std::reverse(pts.begin(),pts.end()); }
    auto cr = [](std::pair<double,double> a, std::pair<double,double> b, std::pair<double,double> c) {
        return (b.first-a.first)*(c.second-a.second) - (b.second-a.second)*(c.first-a.first);
    };
    auto inside = [&](std::pair<double,double> p, std::pair<double,double> a,
                      std::pair<double,double> b, std::pair<double,double> c) {
        return cr(a,b,p) >= -1e-9 && cr(b,c,p) >= -1e-9 && cr(c,a,p) >= -1e-9;
    };
    std::vector<int> idx(n);
    for (int i=0;i<n;++i) idx[i]=i;
    std::vector<Tri> out;
    int guard = 0;
    while (idx.size() > 3 && guard < 4*n) {
        ++guard;
        int L = (int)idx.size();
        bool ear = false;
        for (int j=0;j<L;++j) {
            int i0=idx[(j-1+L)%L], i1=idx[j], i2=idx[(j+1)%L];
            auto a=uv[i0], b=uv[i1], c=uv[i2];
            if (cr(a,b,c) <= 1e-12) continue;
            bool bad = false;
            for (int k : idx) {
                if (k==i0||k==i1||k==i2) continue;
                if (inside(uv[k],a,b,c)) { bad=true; break; }
            }
            if (bad) continue;
            Tri t{pts[i0],pts[i1],pts[i2]};
            if (validTri(t)) out.push_back(t);
            idx.erase(idx.begin()+j);
            ear = true;
            break;
        }
        if (!ear) break;
    }
    if (idx.size() == 3) {
        Tri t{pts[idx[0]],pts[idx[1]],pts[idx[2]]};
        if (validTri(t)) out.push_back(t);
    }
    return out.empty() ? fanTris(pts) : out;
}

static std::string surfaceKind(const Entities& e, const std::vector<Value>& a) {
    if (a.size() > 2 && isRef(a[2])) {
        auto it = e.find(a[2].ref);
        if (it != e.end()) return it->second.kind;
    }
    return "UNKNOWN";
}

static std::vector<Line> thinLines(std::vector<Line> lines, int limit) {
    if ((int)lines.size() <= limit) return lines;
    std::vector<Line> out;
    out.reserve(limit);
    double stepv = (double)lines.size()/limit;
    for (int i=0;i<limit;++i) out.push_back(lines[std::min((int)lines.size()-1, (int)(i*stepv))]);
    return out;
}

// ------------------------------------------------------------------ 构建网格
Mesh buildMesh(const std::string& text, const Options& opt) {
    Mesh mesh;
    Entities e = parseEntities(text);
    mesh.stats.entities = e.size();
    std::vector<int> faces;
    faces.reserve(1024);
    for (const auto& kv : e) if (kv.second.kind == "ADVANCED_FACE") faces.push_back(kv.first);
    // 与 Python 版 dict 的插入顺序（STEP 实体 ID 递增）保持一致，
    // 否则边界线抽稀会挑中不同的线，两个版本出图不一致。
    std::sort(faces.begin(), faces.end());
    mesh.stats.faces = faces.size();

    for (int fid : faces) {
        const auto& a = e[fid].args;
        std::string surf = surfaceKind(e, a);
        std::vector<int> bounds;
        if (a.size() > 1 && isList(a[1])) for (const auto& b : a[1].list) if (isRef(b)) bounds.push_back(b.ref);
        for (int bid : bounds) {
            auto bit = e.find(bid);
            if (bit == e.end() || bit->second.kind != "FACE_OUTER_BOUND") continue;   // 内孔暂不挖空
            std::vector<int> lids;
            for (const auto& x : bit->second.args) if (isRef(x)) lids.push_back(x.ref);
            if (lids.empty()) continue;
            std::vector<Vec3> pts;
            std::vector<std::vector<Vec3>> segs;
            if (!loopData(e, lids[0], opt.arc, pts, segs)) continue;
            ++mesh.stats.loops;
            Vec3 ctr(0,0,0);
            for (const auto& p : pts) ctr = add(ctr,p);
            ctr = mul(ctr, 1.0/pts.size());
            for (const auto& s : segs)
                for (size_t i=0;i+1<s.size();++i) mesh.lines.push_back({s[i], s[i+1], ctr});
            if (opt.solid) {
                // 缩略图是固定视角的一次成像：把每个面的边界环在最佳拟合平面上
                // 做耳切填满，顶点保留真实 3D 坐标，由 z-buffer 插值出曲率。
                // 这比"沿两条对边拉链"稳健得多——曲面片不会碎成大三角或留空洞。
                std::vector<Tri> t = polygonTris(pts, 48);
                smoothNormals(t);
                mesh.tris.insert(mesh.tris.end(), t.begin(), t.end());
            } else if (surf == "PLANE" && pts.size() <= 80) {
                std::vector<Tri> t = polygonTris(pts);
                mesh.tris.insert(mesh.tris.end(), t.begin(), t.end());
            }
        }
        if ((int)mesh.tris.size() >= opt.maxTris) break;
    }
    if ((int)mesh.tris.size() > opt.maxTris) mesh.tris.resize(opt.maxTris);
    mesh.lines = thinLines(std::move(mesh.lines), opt.maxLines);
    return mesh;
}

bool Mesh::normalize() {
    std::vector<Vec3> ps;
    ps.reserve(tris.size()*3 + lines.size()*3);
    for (const auto& t : tris) { ps.push_back(t.a); ps.push_back(t.b); ps.push_back(t.c); }
    for (const auto& l : lines) { ps.push_back(l.a); ps.push_back(l.b); ps.push_back(l.ctr); }
    if (ps.empty()) return false;
    double lo[3]={1e300,1e300,1e300}, hi[3]={-1e300,-1e300,-1e300};
    for (const auto& p : ps) {
        double v[3]={p.x,p.y,p.z};
        for (int i=0;i<3;++i) { lo[i]=std::min(lo[i],v[i]); hi[i]=std::max(hi[i],v[i]); }
    }
    stats.sx=hi[0]-lo[0]; stats.sy=hi[1]-lo[1]; stats.sz=hi[2]-lo[2];
    double ctr[3]={(lo[0]+hi[0])/2,(lo[1]+hi[1])/2,(lo[2]+hi[2])/2};
    double scale = std::max(stats.sx, std::max(stats.sy, stats.sz))/2.0;
    if (scale <= 0) scale = 1.0;
    auto n = [&](Vec3 p) { return Vec3((p.x-ctr[0])/scale,(p.y-ctr[1])/scale,(p.z-ctr[2])/scale); };
    for (auto& t : tris) { t.a=n(t.a); t.b=n(t.b); t.c=n(t.c); }
    for (auto& l : lines) { l.a=n(l.a); l.b=n(l.b); l.ctr=n(l.ctr); }
    return true;
}

// ------------------------------------------------------------------ 光栅化

// 实体面着色色带：深蓝 -> 中蓝 -> 浅蓝 -> 冷白（R,G,B）
static inline void shadeRamp(double s, uint8_t out[3]) {
    static const double pos[4]  = {0.00, 0.45, 0.75, 1.00};
    static const double stop[4][3] = {
        { 16,  54, 108},
        { 56, 130, 205},
        {148, 205, 246},
        {238, 249, 255}
    };
    if (s <= pos[0]) { out[0]=(uint8_t)stop[0][0]; out[1]=(uint8_t)stop[0][1]; out[2]=(uint8_t)stop[0][2]; return; }
    if (s >= pos[3]) { out[0]=(uint8_t)stop[3][0]; out[1]=(uint8_t)stop[3][1]; out[2]=(uint8_t)stop[3][2]; return; }
    for (int k=0;k<3;++k) {
        if (s <= pos[k+1]) {
            double t = (s-pos[k])/(pos[k+1]-pos[k]);
            for (int i=0;i<3;++i) out[i] = (uint8_t)(stop[k][i] + (stop[k+1][i]-stop[k][i])*t + 0.5);
            return;
        }
    }
}

Image render(const Mesh& mesh, int size, uint32_t bg, bool edges, bool cull) {
    Image img;
    if (size < 16) size = 16;
    if (size > 2048) size = 2048;

    // 超采样倍率：小图多超采样，大图省时间
    const int ss = (size <= 300) ? 3 : (size <= 1200 ? 2 : 1);
    const int W = size*ss, H = size*ss;

    std::vector<float> zbuf((size_t)W*H, 1e30f);
    std::vector<uint8_t> rgb((size_t)W*H*3);
    const uint8_t bgB=(uint8_t)(bg & 0xFF), bgG=(uint8_t)((bg>>8)&0xFF), bgR=(uint8_t)((bg>>16)&0xFF);
    for (size_t i=0;i<(size_t)W*H;++i) { rgb[i*3]=bgR; rgb[i*3+1]=bgG; rgb[i*3+2]=bgB; }

    const double cy=std::cos(0.62), sy=std::sin(0.62), cp=std::cos(0.48), sp=std::sin(0.48);
    // 主光：左上前方；补光：右前方。只有主光时，正对镜头的大平面会落在
    // 明暗交界处变成一片死黑；补光负责把侧面亮度拉回可见范围。
    const Vec3 keyLight  = norm(Vec3(-0.38, 0.78, -0.50));
    const Vec3 fillLight = norm(Vec3( 0.62,-0.10, -0.78));
    const double scale = W*0.42;
    auto rot = [&](Vec3 p) {
        double x1=p.x*cy+p.z*sy, z1=-p.x*sy+p.z*cy;
        return Vec3(x1, p.y*cp-z1*sp, p.y*sp+z1*cp);
    };
    auto scr = [&](Vec3 p) { Vec3 q=rot(p); return Vec3(W/2.0+q.x*scale, H/2.0-q.y*scale, q.z); };

    // 相机在 -z 方向：可见面的法线应指向 -z。若朝 +z 说明看到的是内壁/背面，
    // 用翻转法线着色但整体压暗，给壳体类零件一点厚度感。
    auto shade = [&](Vec3 n, uint8_t out[3]) {
        bool inner = (n.z > 0);
        if (inner) n = mul(n, -1.0);
        double dk = std::max(0.0, dot(n, keyLight));
        double df = std::max(0.0, dot(n, fillLight));
        double sh = 0.12 + 0.76*dk + 0.20*df;
        if (inner) sh *= 0.74;
        if (sh > 1.0) sh = 1.0;
        shadeRamp(sh, out);
    };

    // 背面剔除前的合理性检查：闭合实体应有一半左右正面朝相机。
    if (cull) {
        size_t front = 0;
        for (const auto& t : mesh.tris) {
            Vec3 a=rot(t.a), b=rot(t.b), c3=rot(t.c);
            if (cross(sub(b,a), sub(c3,a)).z < 0.0) ++front;
        }
        if (mesh.tris.empty() || front * 100 < mesh.tris.size() * 42) cull = false;
    }

    for (const auto& t : mesh.tris) {
        Vec3 q[3] = {rot(t.a), rot(t.b), rot(t.c)};
        Vec3 gq = cross(sub(q[1],q[0]), sub(q[2],q[0]));
        if (cull && gq.z >= 0.0) continue;      // 朝向远方 = 背面，跳过
        uint8_t colFlat[3] = {0,0,0};
        Vec3 nq[3];
        if (t.smooth) {
            nq[0] = rot(t.na); nq[1] = rot(t.nb); nq[2] = rot(t.nc);
        } else {
            shade(norm(gq), colFlat);
        }
        double xs[3], ys[3], zs[3];
        for (int k=0;k<3;++k) { xs[k]=W/2.0+q[k].x*scale; ys[k]=H/2.0-q[k].y*scale; zs[k]=q[k].z; }
        double area = (xs[1]-xs[0])*(ys[2]-ys[0]) - (xs[2]-xs[0])*(ys[1]-ys[0]);
        if (std::fabs(area) < 1e-9) continue;
        int xa = std::max(0, (int)std::floor(std::min({xs[0],xs[1],xs[2]})));
        int xb = std::min(W-1, (int)std::ceil(std::max({xs[0],xs[1],xs[2]})));
        int ya = std::max(0, (int)std::floor(std::min({ys[0],ys[1],ys[2]})));
        int yb = std::min(H-1, (int)std::ceil(std::max({ys[0],ys[1],ys[2]})));
        for (int py=ya; py<=yb; ++py) {
            double fy=py+0.5;
            for (int px=xa; px<=xb; ++px) {
                double fx=px+0.5;
                double w0=((xs[1]-fx)*(ys[2]-fy)-(xs[2]-fx)*(ys[1]-fy))/area;
                double w1=((xs[2]-fx)*(ys[0]-fy)-(xs[0]-fx)*(ys[2]-fy))/area;
                double w2=1.0-w0-w1;
                if (w0<-1e-6||w1<-1e-6||w2<-1e-6) continue;
                float z=(float)(w0*zs[0]+w1*zs[1]+w2*zs[2]);
                size_t idx=(size_t)py*W+px;
                if (z < zbuf[idx]) {
                    zbuf[idx]=z;
                    if (t.smooth) {                       // Gouraud：法线按重心坐标插值
                        Vec3 nn = Vec3(nq[0].x*w0+nq[1].x*w1+nq[2].x*w2,
                                       nq[0].y*w0+nq[1].y*w1+nq[2].y*w2,
                                       nq[0].z*w0+nq[1].z*w1+nq[2].z*w2);
                        uint8_t col[3];
                        shade(norm(nn), col);
                        rgb[idx*3]=col[0]; rgb[idx*3+1]=col[1]; rgb[idx*3+2]=col[2];
                    } else {
                        rgb[idx*3]=colFlat[0]; rgb[idx*3+1]=colFlat[1]; rgb[idx*3+2]=colFlat[2];
                    }
                }
            }
        }
    }

    if (edges && !mesh.lines.empty()) {
        const uint8_t linecol[3] = {148,74,28};   // B,G,R of (28,74,148)
        for (const auto& l : mesh.lines) {
            if (rot(l.ctr).z > 0.12) continue;     // 背面线剔除
            Vec3 s0=scr(l.a), s1=scr(l.b);
            int steps = std::max(1, (int)std::max(std::fabs(s1.x-s0.x), std::fabs(s1.y-s0.y)));
            if (steps > 4096) continue;
            for (int k=0;k<=steps;++k) {
                double t=(double)k/steps;
                double x=s0.x+(s1.x-s0.x)*t, y=s0.y+(s1.y-s0.y)*t, z=s0.z+(s1.z-s0.z)*t;
                int px=(int)x, py=(int)y;
                if (px<0||px>=W||py<0||py>=H) continue;
                size_t idx=(size_t)py*W+px;
                if (z <= (double)zbuf[idx]+0.02) {
                    zbuf[idx]=std::min(zbuf[idx],(float)(z-0.001));
                    rgb[idx*3]=linecol[2]; rgb[idx*3+1]=linecol[1]; rgb[idx*3+2]=linecol[0];
                }
            }
        }
    }

    // 盒式降采样到目标尺寸（超采样即抗锯齿）
    img.w=size; img.h=size;
    img.bgra.assign((size_t)size*size*4, 255);
    const int n = ss*ss;
    for (int y=0;y<size;++y) {
        for (int x=0;x<size;++x) {
            int r=0,g=0,b=0;
            for (int j=0;j<ss;++j) {
                const uint8_t* row = &rgb[(size_t)(y*ss+j)*W*3 + (size_t)x*ss*3];
                for (int i=0;i<ss;++i) { r+=row[i*3]; g+=row[i*3+1]; b+=row[i*3+2]; }
            }
            size_t o=((size_t)y*size+x)*4;
            img.bgra[o+0]=(uint8_t)(b/n);
            img.bgra[o+1]=(uint8_t)(g/n);
            img.bgra[o+2]=(uint8_t)(r/n);
            img.bgra[o+3]=255;
        }
    }
    return img;
}

bool readFile(const std::wstring& path, std::string& out) {
    // _wfopen 在 MSVC 与 MinGW 均可用，避免 std::ifstream(wstring) 的编译器差异
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long len = ftell(f);
    if (len < 0 || len > 512L*1024L*1024L) { fclose(f); return false; }
    fseek(f, 0, SEEK_SET);
    out.resize((size_t)len);
    size_t got = len ? fread(&out[0], 1, (size_t)len, f) : 0;
    fclose(f);
    out.resize(got);
    return true;
}

} // namespace step
