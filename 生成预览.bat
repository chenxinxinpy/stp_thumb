@echo off
setlocal
if "%~1"=="" (
  echo Drag a .step / .stp file onto this .bat to generate a PNG thumbnail and an HTML preview.
  echo Output folder: the "preview" subfolder next to the STEP file.
  pause
  exit /b
)
where python >nul 2>nul
if errorlevel 1 (
  echo Python was not found in PATH. Install Python 3 or edit this script.
  pause
  exit /b
)
python "%~dp0step_thumb.py" "%~1" --out-dir "%~dp1preview" --size 512
pause
