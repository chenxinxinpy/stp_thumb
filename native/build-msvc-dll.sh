#!/usr/bin/env bash
# MSVC + OCCT 构建 StepThumb.dll（OCCT 只能用 MSVC 链接，MinGW 无法消费 MSVC 的 C++ DLL）
# 输出到 msvc-occt/
set -e
cd "$(dirname "$0")"
source ./msvc-env.sh

ROOT="C:/Users/CHENXINXIN/WorkBuddy/2026-11-13-16-05-47/step-thumb"
OCCT_WIN="$ROOT/_deps/occtenv/Library"
export INCLUDE="$INCLUDE;$(cygpath -w "$OCCT_WIN/include/opencascade")"
export LIB="$LIB;$(cygpath -w "$OCCT_WIN/lib")"

OUT=msvc-occt
mkdir -p $OUT
cd $OUT

CL="cl.exe //nologo //utf-8 //EHsc //MD //std:c++17 //O2 //DUNICODE //D_UNICODE //D_WIN32_WINNT=0x0A00 //DUSE_OCCT"

OCCT_LIBS="TKDESTEP.lib TKXSBase.lib TKDE.lib TKMesh.lib TKBRep.lib TKTopAlgo.lib TKGeomBase.lib TKG2d.lib TKG3d.lib TKMath.lib TKernel.lib TKShHealing.lib TKXCAF.lib TKVCAF.lib TKV3d.lib TKService.lib TKCAF.lib TKLCAF.lib TKCDF.lib TKBO.lib TKPrim.lib TKGeomAlgo.lib"

echo "[1/2] StepThumb.dll (MSVC + OCCT)"
$CL ../StepThumb.cpp ../step_core.cpp ../step_occt.cpp \
    //LD //Fe:StepThumb.dll \
    //link $OCCT_LIBS ole32.lib user32.lib gdi32.lib shlwapi.lib shell32.lib advapi32.lib propsys.lib //EXPORT:DllGetClassObject //EXPORT:DllCanUnloadNow \
    > dll.log 2>&1 || { tail -25 dll.log; exit 1; }
grep -E "warning C|error" dll.log | head -5 || true

echo "[2/2] step2png.exe"
$CL ../step2png.cpp ../step_core.cpp ../step_occt.cpp \
    //Fe:step2png.exe \
    //link $OCCT_LIBS ole32.lib user32.lib gdi32.lib shell32.lib >> dll.log 2>&1 || { tail -20 dll.log; exit 1; }

# 【新增】回归测试：缩略图串台检查（verify_cache.exe）。
# 必须在 StepThumb.dll 装进 Program Files 之后才有意义，这里只保证它能编译。
echo "[3/3] verify_cache.exe（回归测试用）"
$CL ../verify_cache.cpp \
    //Fe:verify_cache.exe \
    //link ole32.lib user32.lib gdi32.lib shlwapi.lib shell32.lib >> dll.log 2>&1 || { tail -20 dll.log; exit 1; }

ls -l StepThumb.dll step2png.exe verify_cache.exe
echo "[OK] msvc-occt/ 构建完成"
