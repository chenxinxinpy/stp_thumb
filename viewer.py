# viewer.py
# 三维模型查看器 · GPU 渲染版（VisPy + OpenGL）
#
# 为什么是最终形态：
#   实测把 VisPy 的原生 widget 嵌入 PySide6 的 QMainWindow/QSplitter 后，
#   OpenGL 画面不合成（全白）；纯 VisPy 单窗口则渲染完全正常。
#   因此采用 SceneCanvas 作为唯一窗口，信息用 VisPy Text 叠加，
#   文件对话框用 Qt（短暂弹出，不与 canvas 共存竞争）。
#
# 功能：加载 STL/STP/STEP、鼠标旋转/平移/缩放、GPU 光照着色、
#   包围盒尺寸、壁厚估算、分辨率档位、颜色调整、错误反馈
from __future__ import annotations

import math
import os
import sys
from typing import Optional

import numpy as np

# 【关键】必须先 use(app="pyside6")，再 import vispy.scene
import vispy
vispy.use(app="pyside6")

from vispy import app, scene
from vispy.scene.visuals import Mesh, Text, Line
from vispy.scene.cameras import TurntableCamera

from model_loader import load_model, remesh_step, LoadResult, QUALITY_ORDER, DEFAULT_QUALITY
# 【改动】STEP 默认「中」档（线性0.05mm/角0.75°）。
# 绝对精度下「高」0.01mm 会产生数亿面不可行，「低」0.1mm 也要 62 秒。
# 用户可按 1/2/3/4 切换（低0.1mm / 中0.05mm / 高0.01mm / 超高0.005mm）
STEP_LOAD_QUALITY = "中"
from analysis import bounding_box, estimate_thickness, ThicknessResult

BG = (247/255, 250/255, 252/255, 1.0)
# 【改动】VisPy 默认字体不含中文，指定 Windows 黑体
CN_FONT = "黑体"        # VisPy face 用系统注册的字体家族名（中文名）
# 【改动】首个预设改为亮蓝色
COLOR_PRESETS = [
    (0.30, 0.62, 1.00, 1.0),   # 亮蓝（原 0.62,0.78,0.98）
    (0.95, 0.62, 0.35, 1.0),   # 橙
    (0.45, 0.82, 0.60, 1.0),   # 绿
    (0.85, 0.55, 0.75, 1.0),   # 粉
]


# 【改动】档位→绝对精度说明（模块级函数，放在类定义之前）
def _quality_desc(q: str) -> str:
    from model_loader import QUALITY_PRESETS
    lin, ang = QUALITY_PRESETS.get(q, (None, 0.20))
    if lin is None:
        div = {"低": 200, "中": 500}.get(q, 500)
        return f"相对1/{div}/角{ang:g}rad"
    return f"绝对{lin}mm/角{ang:g}°"


class ViewerCanvas(scene.SceneCanvas):
    """GPU 三维模型查看器（单窗口）。"""

    def __init__(self):
        super().__init__(
            keys="interactive", size=(1200, 820), bgcolor=BG,
            title="三维模型查看器  ·  STL / STP  ·  GPU 渲染",
        )
        self.unfreeze()
        self._view = self.central_widget.add_view()
        self._view.camera = TurntableCamera(
            fov=40.0, elevation=25.0, azimuth=35.0, distance=500.0,
        )
        self._mesh = None
        self._edge_line = None       # 【改动】特征边线（暗面结构勾边）
        self._result: Optional[LoadResult] = None
        self._quality = DEFAULT_QUALITY
        self._color_index = 0
        # 【改动】四行信息拆成 4 个独立 Text，固定 y 间距 28px。
        #   VisPy 单行 Text 的 line_height 对中文不生效，多行必重叠；
        #   独立对象各自定位是唯一可靠的防重叠方案。
        self._info_lines = []
        self._info_text = None
        self._help_text = None
        defaults = ["尚未加载模型", "按 O 打开文件，或把 .stp / .stl 拖到窗口", "", ""]
        for i in range(4):
            t = Text(
                defaults[i], pos=(14, 12 + i * 34),
                color=(0.15, 0.18, 0.22), font_size=14,
                face=CN_FONT, anchor_x="left", anchor_y="top", parent=self.scene,
            )
            self._info_lines.append(t)
        # 首行兼作错误提示入口，兼容旧代码
        self._info_text = self._info_lines[0]
        self._help_text = Text(
            "左键旋转 · 右键平移 · 滚轮缩放 · F 适配视图\n"
            "O 打开 · C 换色 · 1/2/3/4 分辨率 · R 重载 · E 结构边线",
            # 【改动】帮助文字放到第5行（y=150），避开顶部4行信息。
            #   VisPy Text parent=scene 时 pos 是像素、原点在窗口中心上方，
            #   归一化坐标无效，故统一用像素正值与信息行错开。
            pos=(14, 150), color=(0.45, 0.48, 0.52), font_size=11,
            face=CN_FONT,
            anchor_x="left", anchor_y="top", parent=self.scene,
        )
        self.freeze()

    # ------------------------------------------------------------------ 信息
    def _update_text(self):
        r = self._result
        if r is None:
            return
        th: ThicknessResult = estimate_thickness(r)
        sz = bounding_box(r)
        q = r.quality if r.exact_shape is not None else "不适用（STL）"
        if th.min is None:
            th_line = f"壁厚：{th.label} — {th.message}"
        else:
            th_line = (f"壁厚：{th.label}  最小 {th.min:.2f} / 中位 {th.median:.2f} / "
                       f"最大 {th.max:.2f} / 均值 {th.mean:.2f} mm")
        # 【改动】分别设置 4 行，每行独立 Text 对象定位，不再依赖 line_height
        lines = [
            f"文件：{os.path.basename(r.path)}",
            f"格式：{r.format}   三角面：{len(r.faces):,}   档位：{q}（{_quality_desc(q)}）",
            f"包围盒：{sz[0]:.2f} × {sz[1]:.2f} × {sz[2]:.2f} mm（长×宽×高）",
            th_line,
        ]
        for t, line in zip(self._info_lines, lines):
            t.color = (0.15, 0.18, 0.22)
            t.text = line

    def _apply_lighting(self):
        """【改动】提亮背光面：环境光 0.25→0.75，
        漫反射 0.8 / 高光 0.25 保持，保留自然光影层次。"""
        if self._mesh is None or not hasattr(self._mesh, "shading_filter"):
            return
        sf = self._mesh.shading_filter
        if sf is None:
            return
        sf.ambient_light = (1, 1, 1, 0.75)
        sf.diffuse_light = (1, 1, 1, 0.80)
        sf.specular_light = (1, 1, 1, 0.25)

    def _next_color(self):
        self._color_index = (self._color_index + 1) % len(COLOR_PRESETS)
        if self._mesh is not None:
            self._mesh.set_data(color=COLOR_PRESETS[self._color_index])

    # ------------------------------------------------------------------ 加载
    def load_path(self, path: str):
        try:
            # 【改动】STEP 用指定默认档；STL 用当前档位（对 STL 无意义）
            ext = os.path.splitext(path)[1].lower()
            load_q = STEP_LOAD_QUALITY if ext in (".stp", ".step") else self._quality
            r = load_model(path, quality=load_q)
            # 同步内部档位状态，保证后续切换从当前实际档位开始
            if ext in (".stp", ".step"):
                self._quality = load_q
        except Exception as e:
            self._info_text.color = (0.85, 0.15, 0.15)
            self._info_text.text = f"加载失败：{path}\n{e}"
            return
        self._result = r
        # 【改动】清除旧网格与旧边线
        if self._mesh is not None:
            self._mesh.parent = None
        if self._edge_line is not None:
            self._edge_line.parent = None
            self._edge_line = None
        self._mesh = Mesh(
            vertices=r.vertices.astype("f4"),
            faces=r.faces.astype("u4"),
            color=COLOR_PRESETS[self._color_index],
            shading="smooth",           # GPU Blinn-Phong 光照
            parent=self._view.scene,
        )
        self._apply_lighting()
        self._build_feature_edges()     # 【改动】勾出结构边，暗面也能看清
        self.fit()
        self._update_text()

    def _build_feature_edges(self):
        """【改动】提取二面角>25° 的特征边，用深色线勾出结构轮廓。
        暗面即使光照不足，靠边线也能看清孔、槽、台阶等结构。
        面数过多时自动跳过（避免提取卡顿）。"""
        if self._result is None or len(self._result.faces) > 3_000_000:
            return
        try:
            import trimesh
            v, f = self._result.vertices, self._result.faces
            m = trimesh.Trimesh(vertices=v, faces=f, process=False)
            m.merge_vertices()
            edges = m.face_adjacency_edges[m.face_adjacency_angles > np.radians(25)]
            if len(edges) == 0:
                return
            # 每条边两个端点，组成线段数组
            seg = m.vertices[edges].reshape(-1, 3).astype("f4")
            if self._edge_line is not None:
                self._edge_line.parent = None
            self._edge_line = Line(
                pos=seg, color=(0.08, 0.18, 0.35, 0.55), width=1.2,
                parent=self._view.scene,
            )
        except Exception:
            pass    # 边线提取失败不影响主模型显示

    def fit(self):
        if self._result is None:
            return
        v = self._result.vertices
        lo, hi = v.min(axis=0), v.max(axis=0)
        center = ((lo + hi) / 2.0).tolist()
        radius = float((((hi - lo) ** 2).sum()) ** 0.5) / 2.0 or 1.0
        cam = self._view.camera
        cam.center = center
        cam.distance = radius / math.tan(math.radians(40.0) / 2.0) * 1.45
        cam.elevation = 25.0
        cam.azimuth = 35.0

    def set_quality(self, quality: str):
        if quality not in QUALITY_ORDER:
            return
        self._quality = quality
        r = self._result
        if r is None or r.exact_shape is None:
            return
        try:
            new = remesh_step(r, quality)
        except Exception as e:
            self._info_text.text = f"分辨率切换失败：{e}"
            return
        self._result = new
        self._mesh.set_data(
            vertices=new.vertices.astype("f4"),
            faces=new.faces.astype("u4"),
        )
        self._build_feature_edges()     # 【改动】档位变化后重建边线
        self._update_text()

    # ------------------------------------------------------------------ 交互
    def on_key_press(self, event):
        key = event.key.name
        if key == "O":
            self._open_dialog()
        elif key == "C":
            self._next_color()
        elif key == "F":
            self.fit()
        elif key in ("1", "2", "3", "4"):
            self.set_quality(QUALITY_ORDER[int(key) - 1])
        elif key == "R" and self._result is not None:
            self.load_path(self._result.path)
        elif key == "E" and self._edge_line is not None:   # 【改动】E 切换边线
            self._edge_line.visible = not self._edge_line.visible

    def _open_dialog(self):
        from PySide6.QtWidgets import QApplication, QFileDialog
        QApplication.instance() or QApplication(sys.argv)
        path, _ = QFileDialog.getOpenFileName(
            None, "打开模型", "",
            "模型文件 (*.stl *.stla *.stp *.step);;所有文件 (*)",
        )
        if path:
            self.load_path(path)

    def on_drop(self, event):
        for path in event.files:
            self.load_path(path)
            break


def main():
    canvas = ViewerCanvas()
    canvas.show(visible=True)
    if len(sys.argv) > 1:
        canvas.load_path(sys.argv[1])
    app.run()


if __name__ == "__main__":
    main()
