@echo off
chcp 936 >nul
echo 正在清空 Windows 缩略图缓存（资源管理器会闪一下，属正常）...
taskkill /f /im explorer.exe >nul 2>&1
timeout /t 1 /nobreak >nul
del /f /q "%LOCALAPPDATA%\Microsoft\Windows\Explorer\thumbcache_*.db" >nul 2>&1
del /f /q "%LOCALAPPDATA%\Microsoft\Windows\Explorer\iconcache_*.db" >nul 2>&1
start explorer.exe
echo 完成。打开 STEP 文件所在文件夹按 F5 刷新即可看到新的实体缩略图。
pause
