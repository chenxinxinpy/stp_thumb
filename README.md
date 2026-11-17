# 三维模型查看器 · GPU 渲染版

支持 STL / STP / STEP 的 GPU 三维模型查看器。

## 运行

```bash
pip install PySide6 numpy trimesh cadquery-ocp vispy
python viewer.py [模型文件]
```

也可把模型文件直接拖到窗口。

## 架构

| 文件 | 职责 |
|---|---|
| `viewer.py` | 主程序：VisPy 单窗口、交互、信息叠加 |
| `model_loader.py` | STL（trimesh）/ STEP（OCCT）读取与三角化 |
| `analysis.py` | 包围盒精确计算；壁厚射线估算 |
| `gl_widget.py` | 可复用 VisPy 渲染组件（供二次开发嵌入） |

**渲染后端**：VisPy + OpenGL（RTX 4060 实测 66 万面创建 0.02 秒，Blinn-Phong 平滑着色，MSAA 抗锯齿）。

**为什么是单窗口**：实测把 VisPy 原生 widget 嵌入 PySide6 的 QMainWindow / QSplitter 后，OpenGL 不合成（全白）。故采用 SceneCanvas 作为唯一顶级窗口。旧 VTK 方案及其 DPI workaround 已完全移除。

## 操作

| 按键 | 功能 |
|---|---|
| O | 打开文件 |
| C | 切换颜色（蓝 / 橙 / 绿 / 粉） |
| 1 / 2 / 3 / 4 | 分辨率档位（低 / 中 / 高 / 超高） |
| F | 适配视图 |
| R | 重新加载 |

鼠标：左键旋转 · 右键平移 · 滚轮缩放。

## 信息面板

左上角叠加显示：文件名、格式、三角面数、分辨率档位、包围盒长宽高、壁厚估算。
左下角常驻操作提示。中文字体使用系统「黑体」。

## 分辨率档位（仅 STEP）

| 档位 | 三角面（duzi-shang.stp） | 耗时 |
|---|---|---|
| 低 | 4.4 万 | 0.9 s |
| 中 | 13.4 万 | 1.6 s |
| **高（默认）** | **66 万** | 6.2 s |
| 超高 | 342 万 | 33.7 s |

STL 面片由文件决定，不能细分。

**壁厚口径**：STEP 用 OCCT 射线测内壁-外壁间距，标注「射线估算」；STL 无实体拓扑，给「网格近似（低置信度）」。
