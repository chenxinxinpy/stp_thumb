@echo off
rem StepThumb native build (MSVC x64 / static CRT / no runtime dependency)
rem Usage: build.bat [outdir]
rem   build.bat         -> output in current dir (overwrites MinGW binaries)
rem   build.bat msvc    -> output in msvc\ (side-by-side comparison, no overwrite)
rem NOTE: keep this file pure ASCII; cmd.exe parses batch files in the OEM code page.
setlocal enabledelayedexpansion
cd /d "%~dp0"

set "OUT=%~1"
if "%OUT%"=="" set "OUT=."
if not exist "%OUT%" mkdir "%OUT%"
for %%D in ("%OUT%") do set "OUTABS=%%~fD"
echo [INFO] Output: !OUTABS!

set "VCVARS="
for %%V in (
  "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
  "C:\Program Files\Microsoft Visual Studio\2022\BuildTools"
  "C:\Program Files (x86)\Microsoft Visual Studio\2022\Community"
  "C:\Program Files\Microsoft Visual Studio\2022\Enterprise"
  "C:\Program Files\Microsoft Visual Studio\2022\Professional"
) do if exist "%%~V\VC\Auxiliary\Build\vcvarsall.bat" set "VCVARS=%%~V\VC\Auxiliary\Build\vcvarsall.bat"

if "%VCVARS%"=="" (
  echo [ERROR] vcvarsall.bat not found. Install VS Build Tools VCTools workload.
  exit /b 1
)
echo [INFO] Using %VCVARS%
rem vcvarsall.bat queries the registry for the Windows SDK path; if reg.exe is
rem blocked by a security policy, INCLUDE will lack the UCRT folder and cl will
rem fail with "crtdbg.h not found". Resolve that policy first, then re-run.
call "%VCVARS%" x64
if errorlevel 1 ( echo [ERROR] vcvarsall init failed & exit /b 1 )
if "%WindowsSdkDir%"=="" echo [WARN] WindowsSdkDir unset - SDK detection may have failed

rem /utf-8: source files contain UTF-8 Chinese comments; without it cl uses CP936
set "CLFLAGS=/nologo /O2 /MT /EHsc /std:c++17 /utf-8 /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 /D_CRT_SECURE_NO_WARNINGS /W3"
set "SYSLIBS=ole32.lib user32.lib gdi32.lib shlwapi.lib shell32.lib advapi32.lib propsys.lib uuid.lib"

echo [1/7] step_core.obj
cl %CLFLAGS% /c step_core.cpp "/Fo!OUTABS!\step_core.obj" || goto :fail

echo [2/7] StepThumb.dll
cl %CLFLAGS% /LD StepThumb.cpp "!OUTABS!\step_core.obj" "/Fe!OUTABS!\StepThumb.dll" /link /MACHINE:X64 || goto :fail

echo [3/7] step2png.exe
cl %CLFLAGS% step2png.cpp "!OUTABS!\step_core.obj" "/Fe!OUTABS!\step2png.exe" /link /MACHINE:X64 || goto :fail

echo [4/7] resources (embed DLL + CLI)
rc /nologo "/I!OUTABS!" "/fo!OUTABS!\setup.res" setup.rc || goto :fail

echo [5/7] StepThumbSetup.exe (UAC via /MANIFESTUAC)
cl %CLFLAGS% Setup.cpp "!OUTABS!\setup.res" "/Fe!OUTABS!\StepThumbSetup.exe" /link /MACHINE:X64 || goto :fail

echo [6/7] verify_com.exe
cl %CLFLAGS% verify_com.cpp "/Fe!OUTABS!\verify_com.exe" /link /MACHINE:X64 || goto :fail

echo [7/7] verify_integration.exe
cl %CLFLAGS% verify_integration.cpp "/Fe!OUTABS!\verify_integration.exe" /link /MACHINE:X64 %SYSLIBS% || goto :fail

echo.
echo [OK] MSVC build finished:
for %%F in (StepThumb.dll step2png.exe StepThumbSetup.exe verify_com.exe verify_integration.exe) do (
  if exist "!OUTABS!\%%F" for %%A in ("!OUTABS!\%%F") do echo       %%~nxA  %%~zA bytes
)
echo.
echo   Verify: "!OUTABS!\step2png.exe" "path\to\part.step" out.png 512
echo   Install: "!OUTABS!\StepThumbSetup.exe" /silent
echo   Uninstall: "!OUTABS!\StepThumbSetup.exe" /uninstall /silent
exit /b 0

:fail
echo [ERROR] Build failed. Check compiler output above.
exit /b 2
