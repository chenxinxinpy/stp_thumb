# model_loader.py
# 加载 STL / STP / STEP 文件为统一三角形网格。
#
# 返回 LoadResult：vertices (N,3) / faces (M,3) / bbox / thickness_mode
#   thickness_mode: "exact"(B-rep,适合OCCT射线) / "surface"(仅三角面,用网格方法)
# STL  -> trimesh 读取(支持 ASCII 与二进制)。
# STEP -> OCCT(cadquery-ocp) 读入 B-rep + BRepMesh 三角化。
from __future__ import annotations

from dataclasses import dataclass
from typing import Optional, Tuple
import os

import numpy as np

STL_EXTS = {".stl", ".stla"}
STEP_EXTS = {".stp", ".step"}
SUPPORTED_EXTS = STL_EXTS | STEP_EXTS

# 【新增】三角化分辨率档位。
#   div  : 线性偏差 = 包围盒对角线 / div，div 越大越精细
#   ang  : 角度偏差(rad)，越小曲面采样越密（决定圆角/圆柱是否会"削成多面体"）
# 实测(duzi-shang.stp, 对角线 326mm)：低 4.4万面0.9s / 中 13.4万面1.6s /
#   高 66万面6.2s / 超高 342万面33.7s。故默认"高"，超高仅在需要时手动选择。
QUALITY_PRESETS = {
    "低":   (200,  0.40),
    "中":   (500,  0.20),
    "高":   (1000, 0.10),
    "超高": (2000, 0.05),
}
DEFAULT_QUALITY = "高"
QUALITY_ORDER = ["低", "中", "高", "超高"]


@dataclass
class LoadResult:
    path: str
    format: str            # "STL" / "STEP"
    vertices: np.ndarray   # (N,3) float64
    faces: np.ndarray      # (M,3) int64
    bbox_min: np.ndarray
    bbox_max: np.ndarray
    exact_shape: Optional[object] = None   # OCCT TopoDS_Shape（STEP 才有）
    exact_solid: bool = False              # B-rep 是否为封闭实体
    quality: str = DEFAULT_QUALITY         # 【新增】三角化分辨率档位（STEP 有效）

    @property
    def size_xyz(self) -> np.ndarray:
        """包围盒长/宽/高（DX, DY, DZ），单位 mm。"""
        return self.bbox_max - self.bbox_min

    @property
    def is_exact(self) -> bool:
        """是否可用 B-rep 做精确壁厚射线。非水密网格不适用。"""
        return self.exact_shape is not None and self.exact_solid

    @property
    def is_mesh_only(self) -> bool:
        return self.exact_shape is None

    @property
    def is_watertight_mesh(self) -> bool:
        """网格是否水密（仅网格路径关心）。"""
        return self._mesh_watertight

    _mesh_watertight: bool = False


def _fmt_error(ext: str, path: str, why: str) -> ValueError:
    return ValueError(f"无法加载 {ext} 文件：{os.path.basename(path)}\n原因：{why}")


# ------------------------------------------------------------------ STL
def _load_stl(path: str) -> LoadResult:
    try:
        import trimesh
    except ImportError as e:
        raise _fmt_error("STL", path, f"缺少依赖 trimesh（{e}）") from e
    try:
        mesh = trimesh.load(path, process=False)
    except Exception as e:
        raise _fmt_error("STL", path, f"解析失败：{e}") from e
    if mesh is None or not isinstance(mesh, trimesh.Trimesh) or len(mesh.vertices) == 0:
        # 有些场景load返回Scene，尝试强制合并
        try:
            merged = mesh.to_mesh() if hasattr(mesh, "to_mesh") else None
            mesh = merged or mesh
        except Exception:
            pass
        if mesh is None or len(getattr(mesh, "vertices", [])) == 0:
            raise _fmt_error("STL", path, "文件中没有三角形，或不是有效的 STL")
    v = np.asarray(mesh.vertices, dtype=np.float64)
    f = np.asarray(mesh.faces, dtype=np.int64).reshape(-1, 3)
    watertight = bool(getattr(mesh, "is_watertight", False))
    return LoadResult(
        path=path, format="STL",
        vertices=v, faces=f,
        bbox_min=v.min(axis=0), bbox_max=v.max(axis=0),
        _mesh_watertight=watertight,
    )


# ------------------------------------------------------------------ STEP
def _mesh_shape(shape: object, lin_defl: float, ang_defl: float) -> Tuple[np.ndarray, np.ndarray]:
    """OCCT TopoDS_Shape -> (vertices, faces)。先对已存在网格重建。"""
    from OCP.BRepMesh import BRepMesh_IncrementalMesh
    from OCP.TopExp import TopExp_Explorer
    from OCP.TopAbs import TopAbs_FACE
    from OCP.TopLoc import TopLoc_Location
    from OCP.BRep import BRep_Tool
    from OCP.BRepTools import BRepTools
    from OCP.TopoDS import TopoDS

    BRepTools.Clean_s(shape)                       # 清掉旧三角网，保证尺寸分档生效
    BRepMesh_IncrementalMesh(shape, lin_defl, False, ang_defl, True)

    verts, faces = [], []
    vmap = {}
    exp = TopExp_Explorer(shape, TopAbs_FACE)
    while exp.More():
        face = TopoDS.Face(exp.Current())
        loc = TopLoc_Location()
        tri = BRep_Tool.Triangulation_s(face, loc)
        if tri is not None:
            trsf = loc.Transformation()
            n = tri.NbNodes()
            idx = [0] * (n + 1)
            for i in range(1, n + 1):
                p = tri.Node(i).Transformed(trsf)
                key = (round(p.X(), 6), round(p.Y(), 6), round(p.Z(), 6))
                gi = vmap.get(key)
                if gi is None:
                    gi = len(verts)
                    vmap[key] = gi
                    verts.append((p.X(), p.Y(), p.Z()))
                idx[i] = gi
            m = tri.NbTriangles()
            for t in range(1, m + 1):
                abcd = tri.Triangle(t).Get()
                a, b, c = abcd[0], abcd[1], abcd[2]
                # 面朝向
                if face.Orientation().name == "TopAbs_REVERSED":
                    faces.append((idx[a], idx[c], idx[b]))
                else:
                    faces.append((idx[a], idx[b], idx[c]))
        exp.Next()
    v = np.array(verts, dtype=np.float64).reshape(-1, 3)
    f = np.array(faces, dtype=np.int64).reshape(-1, 3)
    return v, f


def _step_deflections(shape: object, quality: str):
    """按档位计算线性/角度偏差。"""
    from OCP.Bnd import Bnd_Box
    from OCP.BRepBndLib import BRepBndLib
    # 【改动】用 AddOptimal_s(最优包围盒) 替代 Add_s，且对 Get() 的
    #   pybind 返回类型差异做兼容解包，避免 Bnd_Box::Limits 未注册报错。
    box = Bnd_Box()
    BRepBndLib.AddOptimal_s(shape, box)
    xmin, ymin, zmin = box.CornerMin().Coord()
    xmax, ymax, zmax = box.CornerMax().Coord()
    dx, dy, dz = float(xmax - xmin), float(ymax - ymin), float(zmax - zmin)
    diag = (dx * dx + dy * dy + dz * dz) ** 0.5 if dx > 0 else 1.0
    div, ang = QUALITY_PRESETS.get(quality, QUALITY_PRESETS[DEFAULT_QUALITY])
    # 【改动】最小线性偏差下限从 0.05 降到 0.005，保证小模型也能细分
    return max(diag / div, 0.005), ang


def _load_step(path: str, quality: str = DEFAULT_QUALITY) -> LoadResult:
    from OCP.STEPControl import STEPControl_Reader
    from OCP.IFSelect import IFSelect_RetDone
    from OCP.TopExp import TopExp_Explorer
    from OCP.TopAbs import TopAbs_SOLID, TopAbs_SHELL
    from OCP.BRepCheck import BRepCheck_Analyzer

    reader = STEPControl_Reader()
    status = reader.ReadFile(path)
    if status != IFSelect_RetDone:
        raise _fmt_error("STEP", path, f"OCCT 解析失败（状态码 {status}），文件可能损坏或非 STEP 格式")
    nroots = reader.TransferRoots()
    if nroots == 0:
        raise _fmt_error("STEP", path, "OCCT 未能转换任何几何根节点")
    shape = reader.OneShape()

    # 是否封闭实体（决定壁厚射线可靠性）
    solid_exp = TopExp_Explorer(shape, TopAbs_SOLID)
    shell_exp = TopExp_Explorer(shape, TopAbs_SHELL)
    is_solid = solid_exp.More()
    is_shell = shell_exp.More()
    try:
        valid = BRepCheck_Analyzer(shape).IsValid()
    except Exception:
        valid = True
    exact_solid = bool(is_solid and valid)

    # 【改动】三角化精度改为按档位计算（原固定 diag/200 + 0.4rad，曲面明显变形）
    lin_defl, ang_defl = _step_deflections(shape, quality)

    v, f = _mesh_shape(shape, lin_defl, ang_defl)
    if len(v) == 0 or len(f) == 0:
        raise _fmt_error("STEP", path, "几何为空或三角化失败，文件可能不含实体面")

    return LoadResult(
        path=path, format="STEP",
        vertices=v, faces=f,
        bbox_min=v.min(axis=0), bbox_max=v.max(axis=0),
        exact_shape=shape, exact_solid=exact_solid,
        quality=quality,
        _mesh_watertight=(is_shell or is_solid),
    )


def remesh_step(result: LoadResult, quality: str) -> LoadResult:
    """【新增】仅重新三角化，不重读 STEP 文件。
    切换分辨率档位时用：STEP 解析占总耗时大头，重读会白等一次。
    包围盒用 B-rep 的精确包围盒（不受网格精度影响）。"""
    if result.exact_shape is None:
        raise ValueError("仅 STEP 模型支持分辨率切换")
    lin_defl, ang_defl = _step_deflections(result.exact_shape, quality)
    v, f = _mesh_shape(result.exact_shape, lin_defl, ang_defl)
    if len(v) == 0 or len(f) == 0:
        raise ValueError(f"档位「{quality}」三角化失败")
    return LoadResult(
        path=result.path, format=result.format,
        vertices=v, faces=f,
        bbox_min=v.min(axis=0), bbox_max=v.max(axis=0),
        exact_shape=result.exact_shape, exact_solid=result.exact_solid,
        quality=quality,
        _mesh_watertight=result.is_watertight_mesh,
    )


# ------------------------------------------------------------------ 入口
def load_model(path: str, quality: str = DEFAULT_QUALITY) -> LoadResult:
    """统一入口。抛 ValueError 时信息已经面向用户。
    quality 仅对 STEP 生效；STL 的面片数由文件本身决定，不存在细分。"""
    if not os.path.exists(path):
        raise ValueError(f"文件不存在：{path}")
    ext = os.path.splitext(path)[1].lower()
    if ext in STL_EXTS or ext == ".stla":
        return _load_stl(path)
    if ext in STEP_EXTS:
        return _load_step(path, quality)
    raise ValueError(
        f"不支持的格式：{ext or '(无扩展名)'}\n"
        f"当前支持：{' '.join(sorted(SUPPORTED_EXTS))}"
    )
