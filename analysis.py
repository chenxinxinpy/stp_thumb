# analysis.py
# 尺寸与壁厚分析。
#
# bounding_box : 精确长宽高（由网格顶点得到）。
# estimate_thickness :
#   - B-rep(STEP, 封闭实体) -> OCCT 射线，从包围盒中心及 6 个面中心沿各轴双向
#     发射，统计"内壁点到对侧表面的距离"分布，返回 估算/精确 + 置信度。
#   - 仅网格(STL)     -> 网格局部厚度估算：对内点做最近面距离（近似壁厚）。
#     非水密网格只给"表面最近距离"参考值，置信度低。
from __future__ import annotations

from dataclasses import dataclass
from typing import Optional

import numpy as np


@dataclass
class ThicknessResult:
    mode: str            # "exact" / "estimate" / "unavailable"
    label: str           # 中文置信标签
    min: Optional[float] = None
    median: Optional[float] = None
    max: Optional[float] = None
    mean: Optional[float] = None
    samples: int = 0
    message: str = ""


def bounding_box(result) -> np.ndarray:
    """返回 (长,宽,高) = (DX, DY, DZ)，单位 mm。精确。"""
    return result.size_xyz


# ------------------------------------------------------------------ B-rep 射线
def _rays_from_center(cmin: np.ndarray, cmax: np.ndarray):
    """生成探测射线：(起点, 方向)。中心点 + 6 面中心，沿 6 个轴双向。"""
    import numpy as np
    center = (cmin + cmax) / 2.0
    lo, hi = cmin, cmax
    rays = []
    axes = [np.array([1., 0, 0]), np.array([0, 1., 0]), np.array([0, 0, 1.])]
    # 中心点：6 轴双向
    for ax in axes:
        rays.append((center, ax))
        rays.append((center, -ax))
    # 6 个面中心：朝内 3 轴双向
    for dim in range(3):
        for lo_hi in (lo, hi):
            p = center.copy()
            p[dim] = lo_hi[dim]
            for ax in axes:
                rays.append((p, ax))
                rays.append((p, -ax))
    return rays


def _exact_thickness(shape, cmin: np.ndarray, cmax: np.ndarray) -> ThicknessResult:
    # 【改动】用 IntCurvesFace_ShapeIntersector.Perform (非 PerformNearest)：
    #   PerformNearest 每次只返回最近一个交点，无法遍历整条射线的多个穿越，
    #   必须 Perform 之后按 NbPnt 全量遍历并按 WParameter 排序。
    from OCP.IntCurvesFace import IntCurvesFace_ShapeIntersector
    from OCP.gp import gp_Pnt, gp_Dir, gp_Lin

    inter = IntCurvesFace_ShapeIntersector()
    inter.Load(shape, 1e-7)

    size = cmax - cmin
    diag = float(np.sqrt(size[0]**2 + size[1]**2 + size[2]**2)) or 1.0
    max_dist = diag * 2.0          # 射线上限取包围盒对角线的 2 倍，足以穿透

    thickness = []                 # 收集所有 内壁-外壁 距离样本
    for start, direction in _rays_from_center(cmin, cmax):
        lin = gp_Lin(
            gp_Pnt(float(start[0]), float(start[1]), float(start[2])),
            gp_Dir(float(direction[0]), float(direction[1]), float(direction[2])),
        )
        try:
            inter.Perform(lin, 0.0, max_dist)
            if not inter.IsDone():
                continue
            # 射线上所有交点按参数 w 排序，相邻交点间距即局部壁厚
            n = inter.NbPnt()
            pts = []
            for i in range(1, n + 1):
                w = inter.WParameter(i)
                gp = inter.Pnt(i)
                pts.append((w, np.array([gp.X(), gp.Y(), gp.Z()])))
            pts.sort(key=lambda t: t[0])
            for i in range(1, len(pts)):
                dist = float(np.sqrt(((pts[i][1] - pts[i - 1][1])**2).sum()))
                if dist > 1e-4:                     # 忽略数值毛刺产生的近零值
                    thickness.append(dist)
        except Exception:
            continue

    if len(thickness) < 3:
        return ThicknessResult(
            mode="unavailable",
            label="无法判定",
            message="射线探测未获得足够交点，该模型可能为开放面片或射线方向全部脱靶。"
        )

    arr = np.array(thickness)
    # 过滤极端离群：去掉 > 3 倍中位数的值，避免长筋/孔位干扰
    med = float(np.median(arr))
    keep = arr[arr <= med * 3.0] if med > 0 else arr
    if len(keep) == 0:
        keep = arr

    return ThicknessResult(
        mode="estimate",
        label="射线估算",
        min=float(np.min(keep)),
        median=float(np.median(keep)),
        max=float(np.max(keep)),
        mean=float(np.mean(keep)),
        samples=int(len(keep)),
        message=(
            f"沿 {len(keep)} 组射线得到的内壁-外壁间距，已剔除 >3×中位数的离群值。"
            "壁厚分布均匀时接近真实值；存在变壁厚/加强筋时为近似。"
        ),
    )


# ------------------------------------------------------------------ 网格近似
def _mesh_thickness(result) -> ThicknessResult:
    """非水密网格：用内点到表面的最近距离近似最小壁厚，明确标注置信度低。"""
    from scipy.spatial import cKDTree
    v = result.vertices
    tree = cKDTree(v)
    center = (result.bbox_min + result.bbox_max) / 2.0
    # 在包围盒内均匀取内点，算它们到顶点集的最近距离
    lo, hi = result.bbox_min, result.bbox_max
    n = 8
    grids = np.linspace(lo, hi, n)
    inside = []
    for x in grids[:, 0]:
        for y in grids[:, 1]:
            for z in grids[:, 2]:
                pt = np.array([x, y, z])
                # 粗略筛掉明显在外部的点（离 8 角都太近不算）
                d, _ = tree.query(pt)
                inside.append((pt, d))
    dists = np.array([d for _, d in inside])
    if len(dists) == 0:
        return ThicknessResult(mode="unavailable", label="无法判定",
                               message="网格为空，无法估算。")
    # 取最近距离的分布中位数作为"最小壁厚"参考
    return ThicknessResult(
        mode="estimate",
        label="网格近似（低置信度）",
        min=float(np.min(dists)),
        median=float(np.median(dists)),
        max=float(np.max(dists)),
        mean=float(np.mean(dists)),
        samples=int(len(dists)),
        message=(
            "STL 不携带实体拓扑，该值为包围盒内采样点到表面的最近距离，"
            "只能作为最小壁厚的粗略参考，不代表真实壁厚。"
        ),
    )


# ------------------------------------------------------------------ 入口
def estimate_thickness(result) -> ThicknessResult:
    if result.is_exact:
        cmin = result.bbox_min.copy()
        cmax = result.bbox_max.copy()
        # 沿每轴内缩 2%，避免起点正好压在面上导致零长度射线
        pad = (cmax - cmin) * 0.02
        cmin += pad
        cmax -= pad
        return _exact_thickness(result.exact_shape, cmin, cmax)
    return _mesh_thickness(result)
