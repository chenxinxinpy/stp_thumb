#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
step_thumb.py - STEP (AP203/AP214/AP242) -> 缩略图 PNG + 单文件交互 3D 预览

设计目标：与 stl-thumb 相同的思路（文件 -> 几何 -> 渲染位图），但前端面向 B-rep：
  1. 纯标准库解析 STEP 的 DATA 段实体；
  2. 从 ADVANCED_FACE / EDGE_LOOP / EDGE_CURVE 还原每个面的边界环；
  3. 对直线/圆/椭圆/B 样条边采样，用面心扇形三角化得到近似网格；
  4. 复用已安装的 stl-thumb 渲染器出 PNG（没有时自动降级为内置软件光栅化器）；
  5. 输出一个零外部依赖的单文件 HTML 交互预览器。

零第三方依赖；不写注册表、不联网、不启动任何 CAD 软件。

限制：不评估 CAD 曲面，圆柱/圆锥/自由曲面按其边界环近似；内孔不挖空。
这是"识别零件用的缩略图"，不是精确几何。
"""

import argparse
import json
import math
import os
import re
import struct
import subprocess
import sys
import tempfile
import zlib

STL_THUMB_CANDIDATES = [
    r"C:\Program Files\stl-thumb\stl-thumb.exe",
    r"C:\Program Files (x86)\stl-thumb\stl-thumb.exe",
]

# ---------------------------------------------------------------- STEP 词法层

class Ref:
    __slots__ = ("id",)

    def __init__(self, i):
        self.id = i

    def __repr__(self):
        return "#%d" % self.id


def split_top(s):
    """按顶层逗号切分 STEP 参数串（忽略括号内部与字符串内部的逗号）。"""
    out, depth, instr, start, i, n = [], 0, False, 0, 0, len(s)
    while i < n:
        c = s[i]
        if instr:
            if c == "'":
                if i + 1 < n and s[i + 1] == "'":
                    i += 2
                    continue
                instr = False
        else:
            if c == "'":
                instr = True
            elif c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
            elif c == "," and depth == 0:
                out.append(s[start:i])
                start = i + 1
        i += 1
    out.append(s[start:])
    return out


def parse_value(p):
    p = p.strip()
    if not p or p in ("*", "$"):
        return None
    if p.startswith("(") and p.endswith(")"):
        return parse_args(p[1:-1])
    if p.startswith("'") and p.endswith("'"):
        return p[1:-1].replace("''", "'")
    if p.startswith("#"):
        try:
            return Ref(int(p[1:]))
        except ValueError:
            return p
    try:
        return float(p)
    except ValueError:
        return p.upper() if p.startswith(".") else p


def parse_args(s):
    return [parse_value(p) for p in split_top(s)]


ASSIGN_RE = re.compile(r"#(\d+)\s*=\s*")
NAME_RE = re.compile(r"([A-Za-z_][A-Za-z_0-9]*)\s*\(")


def _scan_close(s, start):
    """从 '(' 位置扫描到配对的 ')'（跳过字符串中的括号）。"""
    depth, instr, j, n = 0, False, start, len(s)
    while j < n:
        c = s[j]
        if instr:
            if c == "'":
                if j + 1 < n and s[j + 1] == "'":
                    j += 2
                    continue
                instr = False
        else:
            if c == "'":
                instr = True
            elif c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
                if depth == 0:
                    return j
        j += 1
    return n - 1


def parse_entities(text):
    """解析 STEP 实体，兼容简单实体与复合实体 '#id =( A() B(...) );'。"""
    ents = {}
    n = len(text)
    for m in ASSIGN_RE.finditer(text):
        eid, i = int(m.group(1)), m.end()
        depth, instr, j = 0, False, i
        while j < n:
            c = text[j]
            if instr:
                if c == "'":
                    if j + 1 < n and text[j + 1] == "'":
                        j += 2
                        continue
                    instr = False
            else:
                if c == "'":
                    instr = True
                elif c == "(":
                    depth += 1
                elif c == ")":
                    depth -= 1
                elif c == ";" and depth == 0:
                    break
            j += 1
        body = text[i:j].strip()
        if not body:
            continue
        if body.startswith("("):                       # 复合实体
            ms = list(NAME_RE.finditer(body))
            if not ms:
                continue
            last = ms[-1]
            kind = last.group(1).upper()
            st, en = last.end() - 1, _scan_close(body, last.end() - 1)
            args = parse_args(body[st + 1:en])
        else:
            mm = NAME_RE.match(body)
            if not mm:
                continue
            kind = mm.group(1).upper()
            st, en = mm.end() - 1, _scan_close(body, mm.end() - 1)
            args = parse_args(body[st + 1:en])
        ents[eid] = (kind, args)
    return ents


# ---------------------------------------------------------------- 向量工具

def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def add(a, b):
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])


def mul(a, k):
    return (a[0] * k, a[1] * k, a[2] * k)


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])


def norm(a):
    d = math.sqrt(dot(a, a))
    return mul(a, 1.0 / d) if d > 1e-12 else (0.0, 0.0, 1.0)


def rid(x):
    return x.id if isinstance(x, Ref) else None


# ---------------------------------------------------------------- 几何实体

def point(e, r):
    ent = e.get(r)
    if not ent:
        return None
    k, a = ent
    if k == "CARTESIAN_POINT" and len(a) > 1 and isinstance(a[1], list):
        try:
            return tuple(float(x) for x in a[1][:3])
        except (TypeError, ValueError):
            return None
    if k == "VERTEX_POINT" and len(a) > 1:
        return point(e, rid(a[1]))
    return None


def direction(e, r):
    ent = e.get(r)
    if not ent:
        return None
    a = ent[1]
    if len(a) > 1 and isinstance(a[1], list):
        try:
            return norm(tuple(float(x) for x in a[1][:3]))
        except (TypeError, ValueError):
            return None
    return None


def placement(e, r):
    """AXIS2_PLACEMENT_3D -> (圆心/原点, 法向, 局部 X, 局部 Y)。"""
    ent = e.get(r)
    if not ent:
        return None
    a = ent[1]
    if len(a) < 4:
        return None
    c = point(e, rid(a[1])) or (0.0, 0.0, 0.0)
    ax = direction(e, rid(a[2])) or (0.0, 0.0, 1.0)
    rx = direction(e, rid(a[3])) or (1.0, 0.0, 0.0)
    u = sub(rx, mul(ax, dot(rx, ax)))
    if dot(u, u) < 1e-12:
        u = (1.0, 0.0, 0.0) if abs(ax[2]) > 0.9 else (0.0, 0.0, 1.0)
    u = norm(u)
    return c, ax, u, norm(cross(ax, u))


def _arc(e, cid, p1, p2, n, r2=None, sense=True):
    """按 STEP 的 EDGE_CURVE.same_sense 选择圆弧方向，而不是一律取短弧。"""
    ent = e.get(cid)
    if not ent or not p1 or not p2:
        return [p1, p2]
    a = ent[1]
    pl = placement(e, rid(a[1])) if len(a) > 1 else None
    if not pl or len(a) < 3:
        return [p1, p2]
    c, ax, u, v = pl
    try:
        r1 = float(a[2])
    except (TypeError, ValueError):
        return [p1, p2]
    r2 = r1 if r2 is None else r2

    def ang(p):
        q = sub(p, c)
        return math.atan2(dot(q, v) / max(r2, 1e-9), dot(q, u) / max(r1, 1e-9))

    a1, a2 = ang(p1), ang(p2)
    d = a2 - a1
    if abs(d) < 1e-7:                          # 整圆
        d = 2 * math.pi
    elif sense:
        d = d % (2 * math.pi)
        if d < 1e-7:
            d = 2 * math.pi
    else:
        d = -((-d) % (2 * math.pi))
        if d > -1e-7:
            d = -2 * math.pi
    pts = []
    for i in range(n + 1):
        t = a1 + d * i / n
        pts.append(add(c, add(mul(u, r1 * math.cos(t)), mul(v, r2 * math.sin(t)))))
    pts[0], pts[-1] = p1, p2
    return pts


def _bspline(e, cid, p1, p2, n):
    ent = e.get(cid)
    if not ent or not p1 or not p2:
        return [p1, p2]
    a = ent[1]
    ctrl = []
    for group in a:
        if isinstance(group, list):
            for x in group:
                if isinstance(x, Ref):
                    p = point(e, x.id)
                    if p:
                        ctrl.append(p)
    if len(ctrl) < 2:
        return [p1, p2]
    for _ in range(2):                     # Chaikin 平滑，避免折线感
        q = [ctrl[0]]
        for i in range(len(ctrl) - 1):
            A, B = ctrl[i], ctrl[i + 1]
            q.append(tuple(0.75 * A[j] + 0.25 * B[j] for j in range(3)))
            q.append(tuple(0.25 * A[j] + 0.75 * B[j] for j in range(3)))
        q.append(ctrl[-1])
        ctrl = q
    pts = [ctrl[min(len(ctrl) - 1, round(i * (len(ctrl) - 1) / n))] for i in range(n + 1)]
    pts[0], pts[-1] = p1, p2
    return pts


def edge_points(e, eid, arc_n):
    """EDGE_CURVE -> 采样点列（端点 + 曲线中间点）。"""
    ent = e.get(eid)
    if not ent or ent[0] != "EDGE_CURVE":
        return []
    a = ent[1]
    rs = [x.id for x in a if isinstance(x, Ref)]
    if len(rs) < 3:
        return []
    p1, p2, cid = point(e, rs[0]), point(e, rs[1]), rs[2]
    if not p1 or not p2:
        return []
    sense = str(a[-1]).upper() in (".T.", "T", "TRUE", "1")
    cent = e.get(cid)
    kind = cent[0] if cent else ""
    ca = cent[1] if cent else []
    if "CIRCLE" in kind:
        return _arc(e, cid, p1, p2, arc_n, sense=sense)
    if "ELLIPSE" in kind and len(ca) > 3:
        try:
            return _arc(e, cid, p1, p2, arc_n, float(ca[3]), sense)
        except (TypeError, ValueError):
            pass
    if "B_SPLINE_CURVE" in kind:
        return _bspline(e, cid, p1, p2, min(arc_n, 10))
    if kind == "TRIMMED_CURVE":
        for x in ca:
            if isinstance(x, Ref) and "B_SPLINE" in (e.get(x.id, ("", []))[0]):
                return _bspline(e, x.id, p1, p2, min(arc_n, 10))
    return [p1, p2]


def oriented_points(e, oid, arc_n):
    ent = e.get(oid)
    if not ent:
        return []
    a = ent[1]
    rs = [x.id for x in a if isinstance(x, Ref)]
    if not rs:
        return []
    seg = edge_points(e, rs[-1], arc_n)
    orient = str(a[-1]).upper() in (".T.", "T", "TRUE", "1")
    return seg if orient else seg[::-1]


def _close(a, b, tol):
    d = sub(a, b)
    return dot(d, d) < tol * tol


def loop_data(e, lid, arc_n, tol=0.02):
    """按端点重新串联 EDGE_LOOP 内的有向边，返回 (闭合点链, 有序边段列表)。"""
    ent = e.get(lid)
    if not ent:
        return [], []
    a = ent[1]
    group = a[1] if len(a) > 1 and isinstance(a[1], list) else []
    segs = []
    for x in group:
        if isinstance(x, Ref):
            s = oriented_points(e, x.id, arc_n)
            if len(s) >= 2:
                segs.append(s)
    if not segs:
        return [], []
    allp = [p for s in segs for p in s]
    span = max(max(p[i] for p in allp) - min(p[i] for p in allp) for i in range(3))
    tol = max(tol, span * 2e-5)
    used = [False] * len(segs)
    first = list(segs[0])
    chain, ordered, used[0] = list(first), [first], True
    cur = chain[-1]
    for _ in range(len(segs)):
        pick = None
        for i, s in enumerate(segs):
            if used[i]:
                continue
            if _close(s[0], cur, tol):
                pick = (i, s, False)
                break
            if _close(s[-1], cur, tol):
                pick = (i, s, True)
                break
        if pick is None:
            break
        i, s, rev = pick
        seg = s[::-1] if rev else s
        chain.extend(seg[1:])
        ordered.append(seg)
        used[i] = True
        cur = chain[-1]
    clean = []
    for p in chain:
        if not clean or not _close(p, clean[-1], tol):
            clean.append(p)
    if len(clean) > 2 and _close(clean[0], clean[-1], tol):
        clean.pop()
    return clean, ordered


def loop_points(e, lid, arc_n):
    return loop_data(e, lid, arc_n)[0]


def _valid(t):
    n = cross(sub(t[1], t[0]), sub(t[2], t[0]))
    return dot(n, n) > 1e-14


def _dist(a, b):
    return math.sqrt(max(dot(sub(a, b), sub(a, b)), 0.0))


def fan_tris(pts):
    """平面面片：面心扇形三角化。"""
    if len(pts) < 3:
        return []
    c = tuple(sum(p[i] for p in pts) / len(pts) for i in range(3))
    return [t for t in ((c, pts[i], pts[(i + 1) % len(pts)])
                        for i in range(len(pts))) if _valid(t)]


def _resample(pts, m):
    """按弧长把折线重采样为 m 个点，用于两条对应边的参数对齐。"""
    if len(pts) == m:
        return list(pts)
    cum = [0.0]
    for i in range(1, len(pts)):
        cum.append(cum[-1] + _dist(pts[i - 1], pts[i]))
    total = cum[-1]
    if total <= 1e-12:
        return [pts[0]] * m
    out, j = [], 0
    for k in range(m):
        target = total * k / (m - 1)
        while j < len(cum) - 2 and cum[j + 1] < target:
            j += 1
        t = (target - cum[j]) / max(cum[j + 1] - cum[j], 1e-12)
        out.append(tuple(pts[j][i] + (pts[j + 1][i] - pts[j][i]) * t for i in range(3)))
    return out


def surface_tris(segs, chain):
    """曲面：取环上两条最长的边（通常是两条圆弧/母线），按端点就近配对生成条带。"""
    longs = sorted([s for s in segs if len(s) >= 3], key=len, reverse=True)[:2]
    if len(longs) < 2:
        return fan_tris(chain)
    A, B = longs
    m = max(len(A), len(B))
    A, B = _resample(A, m), _resample(B, m)
    d1 = _dist(A[0], B[0]) + _dist(A[-1], B[-1])
    d2 = _dist(A[0], B[-1]) + _dist(A[-1], B[0])
    if d2 < d1:
        B = B[::-1]
    tris = []
    for i in range(m - 1):
        for t in ((A[i], A[i + 1], B[i + 1]), (A[i], B[i + 1], B[i])):
            if _valid(t):
                tris.append(t)
    return tris


def polygon_tris(pts, max_pts=72):
    """平面多边形：投影到拟合平面后做耳切三角化（凹多边形也正确），大环先抽稀。"""
    n = len(pts)
    if n < 3:
        return []
    if n > max_pts:
        pts = _resample(list(pts) + [pts[0]], max_pts)[:-1]
        n = len(pts)
    N = (0.0, 0.0, 0.0)                                  # Newell 法拟合平面
    for i in range(n):
        p, q = pts[i], pts[(i + 1) % n]
        N = (N[0] + (p[1] - q[1]) * (p[2] + q[2]),
             N[1] + (p[2] - q[2]) * (p[0] + q[0]),
             N[2] + (p[0] - q[0]) * (p[1] + q[1]))
    if dot(N, N) < 1e-12:
        return fan_tris(pts)
    N = norm(N)
    ref = (1.0, 0.0, 0.0) if abs(N[0]) < 0.9 else (0.0, 1.0, 0.0)
    u = norm(sub(ref, mul(N, dot(ref, N))))
    v = cross(N, u)
    o = pts[0]
    uv = [(dot(sub(p, o), u), dot(sub(p, o), v)) for p in pts]
    area = sum(uv[i][0] * uv[(i + 1) % n][1] - uv[(i + 1) % n][0] * uv[i][1]
               for i in range(n))
    if area < 0:
        uv, pts = uv[::-1], pts[::-1]

    def cr(a, b, c):
        return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])

    def inside(p, a, b, c):
        return (cr(a, b, p) >= -1e-9 and cr(b, c, p) >= -1e-9 and
                cr(c, a, p) >= -1e-9)

    idx, out, guard = list(range(n)), [], 0
    while len(idx) > 3 and guard < 4 * n:
        guard += 1
        L, ear = len(idx), False
        for j in range(L):
            i0, i1, i2 = idx[j - 1], idx[j], idx[(j + 1) % L]
            a, b, c = uv[i0], uv[i1], uv[i2]
            if cr(a, b, c) <= 1e-12:
                continue
            if any(inside(uv[k], a, b, c) for k in idx if k not in (i0, i1, i2)):
                continue
            t = (pts[i0], pts[i1], pts[i2])
            if _valid(t):
                out.append(t)
            idx.pop(j)
            ear = True
            break
        if not ear:
            break
    if len(idx) == 3:
        t = tuple(pts[k] for k in idx)
        if _valid(t):
            out.append(t)
    return out if out else fan_tris(pts)


def surface_kind(e, face_args):
    if len(face_args) > 2 and isinstance(face_args[2], Ref):
        return e.get(face_args[2].id, ("UNKNOWN", []))[0]
    return "UNKNOWN"


def _thin_lines(lines, limit):
    if len(lines) <= limit:
        return lines
    step = len(lines) / float(limit)
    return [lines[min(len(lines) - 1, int(i * step))] for i in range(limit)]


def build_mesh(text, arc_n=8, max_tris=20000, mode="wireframe", max_lines=6000):
    """
    mode:
      wireframe - 平面端面实心 + 所有面的真实边界线（默认，对复杂自由曲面最稳健）
      solid     - 所有面都近似填充（平面用耳切、曲面用条带），简单零件观感更好
    """
    e = parse_entities(text)
    faces = [eid for eid, v in e.items() if v[0] == "ADVANCED_FACE"]
    tris, lines, loops = [], [], 0
    for fid in faces:
        a = e[fid][1]
        surf = surface_kind(e, a)
        bounds = a[1] if len(a) > 1 and isinstance(a[1], list) else []
        for b in bounds:
            if not isinstance(b, Ref):
                continue
            bent = e.get(b.id)
            if not bent or bent[0] != "FACE_OUTER_BOUND":   # 内孔暂不挖空
                continue
            lids = [x.id for x in bent[1] if isinstance(x, Ref)]
            if not lids:
                continue
            pts, segs = loop_data(e, lids[0], arc_n)
            if len(pts) < 3:
                continue
            loops += 1
            ctr = tuple(sum(p[i] for p in pts) / len(pts) for i in range(3))
            for s in segs:
                for i in range(len(s) - 1):
                    lines.append((s[i], s[i + 1], ctr))
            if mode == "solid":
                tris.extend(polygon_tris(pts) if surf in ("PLANE", "UNKNOWN", "")
                            else surface_tris(segs, pts))
            elif surf == "PLANE" and len(pts) <= 80:
                tris.extend(polygon_tris(pts))
        if len(tris) >= max_tris:
            break
    return (tris[:max_tris], _thin_lines(lines, max_lines),
            len(e), len(faces), loops)


def normalize_geometry(tris, lines):
    """重心居中 + 最大边归一到 [-1,1]，返回 (三角形, 线段, 原始包围盒尺寸 mm)。"""
    ps = [p for t in tris for p in t] + [p for seg in lines for p in seg]
    if not ps:
        return [], [], (0.0, 0.0, 0.0)
    lo = [min(p[i] for p in ps) for i in range(3)]
    hi = [max(p[i] for p in ps) for i in range(3)]
    size = tuple(hi[i] - lo[i] for i in range(3))
    ctr = [(lo[i] + hi[i]) / 2 for i in range(3)]
    scale = max(size) / 2.0 or 1.0

    def n(p):
        return tuple((p[i] - ctr[i]) / scale for i in range(3))

    nt = [tuple(n(p) for p in t) for t in tris]
    nl = [(n(a), n(b), n(c)) for a, b, c in lines]
    return nt, nl, size


# ---------------------------------------------------------------- 输出：STL

def write_stl(path, tris):
    with open(path, "wb") as f:
        f.write(b"step_thumb: STEP B-rep boundary approximation".ljust(80, b" "))
        f.write(struct.pack("<I", len(tris)))
        for t in tris:
            n = norm(cross(sub(t[1], t[0]), sub(t[2], t[0])))
            f.write(struct.pack("<12fH", n[0], n[1], n[2],
                                *t[0], *t[1], *t[2], 0))
    return path


# ---------------------------------------------------------------- 输出：PNG

def write_png(path, w, h, rgb):
    raw = b"".join(b"\x00" + bytes(rgb[y * w * 3:(y + 1) * w * 3]) for y in range(h))

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    png = (b"\x89PNG\r\n\x1a\n" +
           chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)


def render_internal(tris, lines, size=512, bg=(250, 251, 253)):
    """内置软件光栅化器（z-buffer）+ 边界线，零依赖兜底渲染。"""
    w = h = size
    buf = bytearray(bytes(bg) * (w * h))
    zbuf = [float("inf")] * (w * h)
    cy, sy = math.cos(0.62), math.sin(0.62)
    cp, sp = math.cos(0.48), math.sin(0.48)
    light = norm((-0.45, 0.75, 1.0))
    scale = size * 0.40

    def rot(p):
        x, y, z = p
        x1, z1 = x * cy + z * sy, -x * sy + z * cy
        return (x1, y * cp - z1 * sp, y * sp + z1 * cp)

    for t in tris:
        q = [rot(p) for p in t]
        n = norm(cross(sub(q[1], q[0]), sub(q[2], q[0])))
        sh = 0.32 + 0.68 * abs(dot(n, light))
        col = (int(58 * sh + 26), int(126 * sh + 32), int(214 * sh + 38))
        s = [(w / 2 + p[0] * scale, h / 2 - p[1] * scale, p[2]) for p in q]
        (x0, y0, z0), (x1, y1, z1), (x2, y2, z2) = s
        area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
        if abs(area) < 1e-9:
            continue
        xa, xb = max(0, int(min(x0, x1, x2))), min(w - 1, int(max(x0, x1, x2)))
        ya, yb = max(0, int(min(y0, y1, y2))), min(h - 1, int(max(y0, y1, y2)))
        for py in range(ya, yb + 1):
            fy = py + 0.5
            for px in range(xa, xb + 1):
                fx = px + 0.5
                w0 = ((x1 - fx) * (y2 - fy) - (x2 - fx) * (y1 - fy)) / area
                w1 = ((x2 - fx) * (y0 - fy) - (x0 - fx) * (y2 - fy)) / area
                w2 = 1.0 - w0 - w1
                if w0 < -1e-6 or w1 < -1e-6 or w2 < -1e-6:
                    continue
                z = w0 * z0 + w1 * z1 + w2 * z2
                idx = py * w + px
                if z < zbuf[idx]:
                    zbuf[idx] = z
                    buf[idx * 3:idx * 3 + 3] = bytes(col)

    linecol = (28, 74, 148)

    def scr(p):
        q = rot(p)
        return w / 2 + q[0] * scale, h / 2 - q[1] * scale, q[2]

    for a, b, ctr in lines:
        if rot(ctr)[2] > 0.12:              # 背面线剔除：只保留朝向相机的半边
            continue
        x0, y0, z0 = scr(a)
        x1, y1, z1 = scr(b)
        steps = max(1, int(max(abs(x1 - x0), abs(y1 - y0))))
        if steps > 4096:
            continue
        for k in range(steps + 1):
            t = k / steps
            x, y, z = x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, z0 + (z1 - z0) * t
            px, py = int(x), int(y)
            if 0 <= px < w and 0 <= py < h:
                idx = py * w + px
                if z <= zbuf[idx] + 0.02:
                    zbuf[idx] = min(zbuf[idx], z - 0.001)
                    buf[idx * 3:idx * 3 + 3] = bytes(linecol)
    return w, h, buf


def find_renderer():
    for p in STL_THUMB_CANDIDATES:
        if os.path.isfile(p):
            return p
    return None


def render_png(tris, lines, png_path, size, renderer, stl_path=None, mode="wireframe"):
    if mode == "wireframe":                # 线框只能由内置光栅化器绘制
        w, h, buf = render_internal(tris, lines, size)
        write_png(png_path, w, h, buf)
        return "internal (线框)"
    ext = find_renderer()
    use_ext = (renderer == "stl-thumb" or (renderer == "auto" and ext))
    if use_ext and ext:
        tmp = stl_path
        owned = tmp is None
        if owned:
            fd, tmp = tempfile.mkstemp(suffix=".stl", prefix="step_thumb_")
            os.close(fd)
            write_stl(tmp, tris)
        try:
            r = subprocess.run([ext, tmp, png_path, "-s", str(size),
                                "-b", "fafbfdff", "--recalc-normals"],
                               capture_output=True, timeout=120)
            if r.returncode == 0 and os.path.isfile(png_path) and os.path.getsize(png_path) > 100:
                return "stl-thumb"
        except (OSError, subprocess.TimeoutExpired):
            pass
        finally:
            if owned and os.path.exists(tmp):
                os.remove(tmp)
    w, h, buf = render_internal(tris, [], size)
    write_png(png_path, w, h, buf)
    return "internal"


# ---------------------------------------------------------------- 输出：HTML

HTML_TEMPLATE = r"""<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>__TITLE__ · STEP 预览</title>
<style>
  :root{--bg:#f4f6f9;--card:#fff;--line:#dfe4ec;--ink:#1f2937;--muted:#6b7280;--accent:#2f6fdb}
  *{box-sizing:border-box}
  body{margin:0;background:var(--bg);color:var(--ink);font:14px/1.6 -apple-system,"Segoe UI","Microsoft YaHei",sans-serif}
  .wrap{max-width:920px;margin:28px auto;padding:0 18px}
  .card{background:var(--card);border:1px solid var(--line);border-radius:14px;box-shadow:0 6px 22px rgba(30,50,90,.06);overflow:hidden}
  .head{padding:16px 20px;border-bottom:1px solid var(--line);display:flex;align-items:center;gap:14px;flex-wrap:wrap}
  .head h1{font-size:16px;margin:0;font-weight:650;word-break:break-all}
  .tag{font-size:12px;color:var(--muted);background:#f1f4f9;border:1px solid var(--line);border-radius:999px;padding:2px 10px}
  .canvas-box{position:relative;background:linear-gradient(180deg,#fbfcfe,#f3f6fb);display:flex;justify-content:center}
  canvas{display:block;max-width:100%;height:auto;cursor:grab;touch-action:none}
  canvas:active{cursor:grabbing}
  .bar{padding:12px 20px;border-top:1px solid var(--line);display:flex;align-items:center;gap:10px;flex-wrap:wrap}
  button{font:inherit;font-size:13px;padding:6px 14px;border:1px solid var(--line);background:#fff;color:var(--ink);border-radius:8px;cursor:pointer}
  button:hover{border-color:var(--accent);color:var(--accent)}
  button.on{background:var(--accent);border-color:var(--accent);color:#fff}
  .hint{margin-left:auto;color:var(--muted);font-size:12px}
  .meta{padding:10px 20px;color:var(--muted);font-size:12px;border-top:1px dashed var(--line)}
  .warn{margin-top:14px;padding:12px 16px;background:#fff8e8;border:1px solid #f0dfae;border-radius:10px;color:#7a5b12;font-size:12px}
</style>
</head>
<body>
<div class="wrap">
  <div class="card">
    <div class="head"><h1 id="t">__TITLE__</h1><span class="tag">STEP / B-rep 近似预览</span></div>
    <div class="canvas-box"><canvas id="c" width="800" height="600"></canvas></div>
    <div class="bar">
      <button id="reset">重置视角</button>
      <button id="wire">线框</button>
      <button id="spin">自动旋转</button>
      <span class="hint">拖拽旋转 · 滚轮缩放</span>
    </div>
    <div class="meta" id="m"></div>
  </div>
  <div class="warn">说明：由 STEP 的 B-rep 边界环近似三角化生成，曲线与曲面按采样逼近，内孔未挖空；用于快速识别零件，不作为精确几何依据。</div>
</div>
<script>
const D = __DATA__;
const V = D.v, F = D.f, L = D.l, N = F.length / 3;
const cv = document.getElementById('c'), ctx = cv.getContext('2d');
let yaw = 0.62, pitch = 0.48, zoom = 1, wire = false, spin = false;
let drag = null;
document.getElementById('m').textContent =
  '面数 ' + D.faces + ' · 面片 ' + N + ' · 边界线 ' + (L.length / 9) + ' · 包围盒 ' +
  D.size.map(x => x.toFixed(2)).join(' × ') + ' mm · 实体 ' + D.entities;

function project(p){
  const cy=Math.cos(yaw), sy=Math.sin(yaw), cp=Math.cos(pitch), sp=Math.sin(pitch);
  const x1=p[0]*cy+p[2]*sy, z1=-p[0]*sy+p[2]*cy;
  const y2=p[1]*cp-z1*sp, z2=p[1]*sp+z1*cp;
  return [x1,y2,z2];
}
function draw(){
  const W=cv.width,H=cv.height;
  ctx.fillStyle='#fafbfd';ctx.fillRect(0,0,W,H);
  const buf=new Float32Array(V.length);
  const sc=Math.min(W,H)*0.40*zoom;
  for(let i=0;i<V.length;i+=3){
    const q=project([V[i],V[i+1],V[i+2]]);
    buf[i]=W/2+q[0]*sc; buf[i+1]=H/2-q[1]*sc; buf[i+2]=q[2];
  }
  const q3=new Float32Array(V.length);
  for(let i=0;i<V.length;i+=3){
    const q=project([V[i],V[i+1],V[i+2]]);q3[i]=q[0];q3[i+1]=q[1];q3[i+2]=q[2];
  }
  const list=[];
  for(let k=0;k<N;k++){
    const a=F[k*3]*3,b=F[k*3+1]*3,c=F[k*3+2]*3;
    const ax=q3[a],ay=q3[a+1],az=q3[a+2];
    const ux=q3[b]-ax,uy=q3[b+1]-ay,uz=q3[b+2]-az;
    const vx=q3[c]-ax,vy=q3[c+1]-ay,vz=q3[c+2]-az;
    let nx=uy*vz-uz*vy,ny=uz*vx-ux*vz,nz=ux*vy-uy*vx;
    const d=Math.hypot(nx,ny,nz)||1;nx/=d;ny/=d;nz/=d;
    const sh=0.32+0.68*Math.abs(nx*-0.4+ny*0.72+nz*0.57);
    list.push({a,b,c,z:(az+q3[b+2]+q3[c+2])/3,sh});
  }
  list.sort((p,q)=>p.z-q.z);
  for(const t of list){
    ctx.beginPath();
    ctx.moveTo(buf[t.a],buf[t.a+1]);ctx.lineTo(buf[t.b],buf[t.b+1]);ctx.lineTo(buf[t.c],buf[t.c+1]);ctx.closePath();
    if(wire){ctx.strokeStyle='rgba(35,72,140,.55)';ctx.lineWidth=.7;ctx.stroke();}
    else{
      const r=Math.round(26+58*t.sh),g=Math.round(32+126*t.sh),b=Math.round(38+214*t.sh);
      ctx.fillStyle='rgb('+Math.min(r,255)+','+Math.min(g,255)+','+Math.min(b,255)+')';ctx.fill();
      ctx.strokeStyle='rgba(20,50,110,.10)';ctx.lineWidth=.4;ctx.stroke();
    }
  }
  if(L.length){
    ctx.strokeStyle='rgba(24,66,132,.45)';ctx.lineWidth=.8;ctx.beginPath();
    for(let i=0;i<L.length;i+=9){
      const c=project([L[i+6],L[i+7],L[i+8]]);
      if(c[2]>0.12) continue;
      const a=project([L[i],L[i+1],L[i+2]]), b=project([L[i+3],L[i+4],L[i+5]]);
      ctx.moveTo(W/2+a[0]*sc,H/2-a[1]*sc);
      ctx.lineTo(W/2+b[0]*sc,H/2-b[1]*sc);
    }
    ctx.stroke();
  }
}
cv.addEventListener('pointerdown',e=>{drag={x:e.clientX,y:e.clientY,yaw,pitch};cv.setPointerCapture(e.pointerId);});
cv.addEventListener('pointermove',e=>{if(!drag)return;yaw=drag.yaw+(e.clientX-drag.x)*.008;pitch=Math.max(-1.45,Math.min(1.45,drag.pitch-(e.clientY-drag.y)*.008));draw();});
cv.addEventListener('pointerup',()=>drag=null);
cv.addEventListener('pointercancel',()=>drag=null);
cv.addEventListener('wheel',e=>{e.preventDefault();zoom=Math.max(.35,Math.min(3.5,zoom*(e.deltaY>0?.9:1.1)));draw();},{passive:false});
document.getElementById('reset').onclick=()=>{yaw=.62;pitch=.48;zoom=1;draw();};
const wb=document.getElementById('wire');wb.onclick=()=>{wire=!wire;wb.classList.toggle('on',wire);draw();};
const sb=document.getElementById('spin');sb.onclick=()=>{spin=!spin;sb.classList.toggle('on',spin);};
function loop(){if(spin&&!drag){yaw+=.012;draw();}requestAnimationFrame(loop);}
draw();loop();
</script>
</body>
</html>
"""


def mesh_to_arrays(tris):
    verts, idx, lookup = [], [], {}
    for t in tris:
        row = []
        for p in t:
            key = (round(p[0], 5), round(p[1], 5), round(p[2], 5))
            if key not in lookup:
                lookup[key] = len(verts) // 3
                verts.extend((round(key[0], 5), round(key[1], 5), round(key[2], 5)))
            row.append(lookup[key])
        idx.extend(row)
    return verts, idx


def write_html(path, tris, lines, meta):
    verts, idx = mesh_to_arrays(tris)
    flat = []
    for a, b, c in lines:
        flat.extend((round(a[0], 5), round(a[1], 5), round(a[2], 5),
                     round(b[0], 5), round(b[1], 5), round(b[2], 5),
                     round(c[0], 5), round(c[1], 5), round(c[2], 5)))
    data = {
        "v": verts, "f": idx, "l": flat,
        "faces": meta["faces"], "entities": meta["entities"],
        "size": [round(x, 3) for x in meta["size"]],
    }
    html = (HTML_TEMPLATE.replace("__DATA__", json.dumps(data, separators=(",", ":")))
                         .replace("__TITLE__", meta["title"]))
    with open(path, "w", encoding="utf-8") as f:
        f.write(html)
    return path


# ---------------------------------------------------------------- CLI

def main():
    ap = argparse.ArgumentParser(description="STEP -> 缩略图 PNG + 单文件 3D 预览（零第三方依赖）")
    ap.add_argument("step", help="输入 .step/.stp 文件")
    ap.add_argument("--out-dir", default=".", help="输出目录（默认当前目录）")
    ap.add_argument("--size", type=int, default=512, help="缩略图边长，默认 512")
    ap.add_argument("--arc", type=int, default=8, help="每条曲线边的采样段数，默认 8")
    ap.add_argument("--max-tris", type=int, default=20000, help="三角形上限，默认 20000")
    ap.add_argument("--max-lines", type=int, default=6000, help="边界线抽稀上限，默认 6000")
    ap.add_argument("--mode", choices=("wireframe", "solid"), default="wireframe",
                    help="wireframe=平面端面实心+真实边界线（默认，复杂曲面更稳）；solid=所有面近似填充")
    ap.add_argument("--renderer", choices=("auto", "stl-thumb", "internal"), default="auto",
                    help="solid 模式的 PNG 渲染器：auto=优先复用 stl-thumb，internal=纯 Python 兜底")
    ap.add_argument("--stl", action="store_true", help="额外导出近似网格 STL")
    ap.add_argument("--no-html", action="store_true", help="不生成交互预览 HTML")
    ap.add_argument("--no-png", action="store_true", help="只生成 HTML / STL")
    args = ap.parse_args()

    src = os.path.abspath(args.step)
    if not os.path.isfile(src):
        sys.exit("找不到文件：%s" % src)
    out_dir = os.path.abspath(args.out_dir)
    os.makedirs(out_dir, exist_ok=True)
    stem = os.path.splitext(os.path.basename(src))[0]

    import time
    t0 = time.time()
    with open(src, "rb") as f:
        raw = f.read()
    text = raw.decode("utf-8", errors="replace")
    tris, lines, nent, nfaces, nloops = build_mesh(
        text, args.arc, args.max_tris, args.mode, args.max_lines)
    if not tris and not lines:
        sys.exit("未能从该 STEP 提取到任何面边界（可能不是 ADVANCED_BREP 或格式不受支持）")
    tris, lines, size = normalize_geometry(tris, lines)
    meta = {"faces": nfaces, "entities": nent, "size": size, "title": stem}
    print("解析：实体 %d 个 / 面 %d 个 / 边界环 %d 个 -> 面片 %d 个 + 边界线 %d 段，耗时 %.2fs"
          % (nent, nfaces, nloops, len(tris), len(lines), time.time() - t0))
    print("包围盒：%.2f × %.2f × %.2f mm" % size)

    stl_path = os.path.join(out_dir, stem + ".stl") if args.stl else None
    if stl_path:
        write_stl(stl_path, tris)
        print("STL  ：%s" % stl_path)

    if not args.no_png:
        png_path = os.path.join(out_dir, stem + ".png")
        engine = render_png(tris, lines, png_path, args.size, args.renderer,
                            stl_path, args.mode)
        print("缩略图：%s（%s / %s）" % (png_path, args.mode, engine))

    if not args.no_html:
        html_path = os.path.join(out_dir, stem + ".preview.html")
        write_html(html_path, tris, lines, meta)
        print("预览  ：%s" % html_path)


if __name__ == "__main__":
    main()
