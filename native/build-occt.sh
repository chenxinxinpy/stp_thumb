#!/usr/bin/env bash
# 用 MSVC + OCCT 构建验证工具（step2png），输出到 occtbuild/
# 注意：不用 vcvarsall（会调 reg.exe，被安全策略拦），改为显式 INCLUDE/LIB。
set -e
cd "$(dirname "$0")"
source ./msvc-env.sh

ROOT="C:/Users/CHENXINXIN/WorkBuddy/2026-11-13-16-05-47/step-thumb"
OCCT_WIN="$ROOT/_deps/occtenv/Library"
export INCLUDE="$INCLUDE;$(cygpath -w "$OCCT_WIN/include/opencascade")"
export LIB="$LIB;$(cygpath -w "$OCCT_WIN/lib")"

mkdir -p occtbuild
cd occtbuild

OCCT_LIBS="TKDESTEP.lib TKXSBase.lib TKDE.lib TKMesh.lib TKBRep.lib TKTopAlgo.lib TKGeomBase.lib TKG2d.lib TKG3d.lib TKMath.lib TKernel.lib TKShHealing.lib TKXCAF.lib TKVCAF.lib TKV3d.lib TKService.lib TKCAF.lib TKLCAF.lib TKCDF.lib TKBO.lib TKPrim.lib TKGeomAlgo.lib TKTopAlgo.lib TKBool.lib TKOffset.lib TKFillet.lib TKHLR.lib TKFeat.lib"

cl.exe //nologo //utf-8 //EHsc //MD //std:c++17 //O2 //DUSE_OCCT \
    ../step_core.cpp ../step_occt.cpp ../step2png.cpp \
    //Fe:step2png-occt.exe //link $OCCT_LIBS > compile.log 2>&1 || { tail -30 compile.log; exit 1; }
grep -E "error|warning C" compile.log | head -10 || true
echo "[OK] occtbuild/step2png-occt.exe"
