# gl_widget.py
# 【新增】基于 VisPy 的 GPU 渲染组件，替代 VTK。
#
# 为什么换掉 VTK：
#   VTK 的 QVTKRenderWindowInteractor 按 devicePixelRatio 放大渲染缓冲，
#   高 DPI 下与 Qt widget 尺寸错位，模型偏移被裁且无法可靠修复。
#
# 为什么用 VisPy：
#   - 基于 OpenGL 2.0+ 着色器，GPU 渲染，66万面创建仅 0.01s
#   - 原生 PySide6 集成，内置 TurntableCamera（旋转/平移/缩放）
#   - 自动处理 DPI、viewport、VAO/索引缓冲，无底层 GL 状态坑
#   - 内置 Blinn-Phong 光照（shading='smooth'）
#
# 交互：左键拖动旋转 · 右键拖动平移 · 滚轮缩放（内置相机）
import numpy as np
from vispy import scene
from vispy.scene.visuals import Mesh
from vispy.scene.cameras import TurntableCamera


class GLModelWidget(scene.SceneCanvas):
    """GPU 模型渲染组件。set_mesh() 上传网格，set_color() 改色，fit() 适配视图。"""

    def __init__(self, parent=None, size=(900, 760), title="3D 视图"):
        super().__init__(keys=None, parent=parent, size=size, title=title)
        # VisPy 的 SceneCanvas 继承 Frozen 类，禁止直接赋自定义属性，
        # 需先 unfreeze()，赋完再 freeze()。
        self.unfreeze()
        self._view = self.central_widget.add_view()
        self._mesh = None
        self._verts = None
        self._faces = None
        self._color = (0.62, 0.78, 0.98, 1.0)   # 偏亮蓝，shading 后观感适中
        self.freeze()
        self._view.camera = TurntableCamera(
            fov=40.0, elevation=25.0, azimuth=35.0,
            distance=500.0, center=(0, 0, 0),
        )
        # 浅色背景，与软件风格一致
        self.bgcolor = (247/255, 250/255, 252/255, 1.0)

    @property
    def qwidget(self):
        """返回可嵌入 QMainWindow/QSplitter 的原生 Qt widget。"""
        return self.native

    def set_mesh(self, vertices: np.ndarray, faces: np.ndarray):
        """顶点 (N,3) / 面 (M,3)。法线由 VisPy shader 计算。"""
        self._verts = np.asarray(vertices, dtype="f4")
        self._faces = np.asarray(faces, dtype="u4")
        if self._mesh is not None:
            self._mesh.parent = None
            self._mesh = None
        self._mesh = Mesh(
            vertices=self._verts, faces=self._faces,
            color=self._color, shading="smooth",   # GPU Blinn-Phong 平滑光照
            parent=self._view.scene,
        )
        self.fit()
        # 强制首帧渲染：嵌入布局后 Qt 不会立即触发 VisPy 绘制，
        # 不刷新的话窗口会短暂/持续空白。
        self.update()
        self._view.camera._update() if hasattr(self._view.camera, "_update") else None

    def set_color(self, r: float, g: float, b: float):
        self._color = (float(r), float(g), float(b), 1.0)
        if self._mesh is not None:
            self._mesh.set_data(color=self._color)

    def clear_mesh(self):
        if self._mesh is not None:
            self._mesh.parent = None
            self._mesh = None
        self.update()

    def fit(self):
        """按包围盒手动计算相机距离，保证完整显示且留 25% 边距。"""
        if self._verts is None:
            return
        lo = self._verts.min(axis=0)
        hi = self._verts.max(axis=0)
        center = ((lo + hi) / 2.0).tolist()
        extent = hi - lo
        radius = float(np.linalg.norm(extent)) / 2.0 or 1.0
        # 距离 = radius / tan(fov/2) * 边距系数
        import math
        distance = radius / math.tan(math.radians(40.0) / 2.0) * 1.45
        cam = self._view.camera
        cam.center = center
        cam.distance = max(distance, 1e-3)
        cam.elevation = 25.0
        cam.azimuth = 35.0
        self.update()
