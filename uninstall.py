# uninstall.py
# 【新增】卸载三维模型查看器：删除右键菜单注册表项 + 删除安装目录。
# 用法：python uninstall.py    （先关闭查看器窗口）
import os
import shutil
import winreg

EXTS = (".stp", ".step", ".stl", ".stla")
INSTALL = os.path.join(os.environ["LOCALAPPDATA"], "Programs", "STPViewer")

for ext in EXTS:
    base = rf"Software\Classes\SystemFileAssociations\{ext}\shell\STPViewer"
    for sub in (base + r"\command", base):
        try:
            winreg.DeleteKey(winreg.HKEY_CURRENT_USER, sub)
            print("已删除", sub)
        except FileNotFoundError:
            pass

if os.path.isdir(INSTALL):
    try:
        shutil.rmtree(INSTALL)
        print("已删除安装目录:", INSTALL)
    except PermissionError:
        print("删除失败，请先关闭查看器:", INSTALL)
else:
    print("安装目录不存在，跳过")
