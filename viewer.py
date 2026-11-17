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
from vispy.scene.visuals import Mesh, Text
from vispy.scene.cameras import TurntableCamera

from model_loader import load_model, remesh_step, LoadResult, QUALITY_ORDER, DEFAULT_QUALITY
from analysis import bounding_box, estimate_thickness, ThicknessResult

BG = (247/255, 250/255, 252/255, 1.0)
# 【改动】VisPy 默认字体不含中文，指定 Windows 黑体
CN_FONT = "黑体"        # VisPy face 用系统注册的字体家族名（中文名）
COLOR_PRESETS = [
    (0.62, 0.78, 0.98, 1.0),   # 蓝
    (0.95, 0.62, 0.35, 1.0),   # 橙
    (0.45, 0.82, 0.60, 1.0),   # 绿
    (0.85, 0.55, 0.75, 1.0),   # 粉
]


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
        self._result: Optional[LoadResult] = None
        self._quality = DEFAULT_QUALITY
        self._color_index = 0
        self._info_text = None
        self._help_text = None

        self._info_text = Text(
            "尚未加载模型\n按 O 打开文件，或把 .stp / .stl 拖到窗口",
            pos=(14, 12), color=(0.15, 0.18, 0.22), font_size=12,
            face=CN_FONT,
            anchor_x="left", anchor_y="top", parent=self.scene,
        )
        self._help_text = Text(
            "左键旋转 · 右键平移 · 滚轮缩放 · F 适配视图\n"
            "O 打开 · C 换色 · 1/2/3/4 分辨率 · R 重载",
            pos=(14, -12), color=(0.45, 0.48, 0.52), font_size=11,
            face=CN_FONT,
            anchor_x="left", anchor_y="bottom", parent=self.scene,
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
        self._info_text.color = (0.15, 0.18, 0.22)
        self._info_text.text = (
            f"文件：{os.path.basename(r.path)}\n"
            f"格式：{r.format}    三角面：{len(r.faces):,}    分辨率档位：{q}\n"
            f"包围盒：长 {sz[0]:.2f} × 宽 {sz[1]:.2f} × 高 {sz[2]:.2f} mm\n"
            f"{th_line}"
        )

    def _next_color(self):
        self._color_index = (self._color_index + 1) % len(COLOR_PRESETS)
        if self._mesh is not None:
            self._mesh.set_data(color=COLOR_PRESETS[self._color_index])

    # ------------------------------------------------------------------ 加载
    def load_path(self, path: str):
        try:
            r = load_model(path, quality=self._quality)
        except Exception as e:
            self._info_text.color = (0.85, 0.15, 0.15)
            self._info_text.text = f"加载失败：{path}\n{e}"
            return
        self._result = r
        if self._mesh is not None:
            self._mesh.parent = None
        self._mesh = Mesh(
            vertices=r.vertices.astype("f4"),
            faces=r.faces.astype("u4"),
            color=COLOR_PRESETS[self._color_index],
            shading="smooth",           # GPU Blinn-Phong 光照
            parent=self._view.scene,
        )
        self.fit()
        self._update_text()

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
