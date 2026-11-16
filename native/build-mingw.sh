#!/usr/bin/env bash
# 便携 MinGW-w64 构建（与 MSVC build.bat 产出等价的原生 x64 组件）
set -euo pipefail
cd "$(dirname "$0")"
TC="$(pwd)/_toolchain/mingw64/bin"
GXX="$TC/g++.exe"
WINDRES="$TC/windres.exe"

FLAGS="-O2 -std=c++17 -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -Wall -Wno-unknown-pragmas"
LINK="-static -static-libgcc -static-libstdc++ -lole32 -luser32 -lgdi32 -lshlwapi -lshell32 -ladvapi32 -lpropsys -luuid"
EXE_LINK="$LINK -municode"

echo "[1/6] step_core.o"
"$GXX" $FLAGS -c step_core.cpp -o step_core.o

echo "[2/6] StepThumb.dll"
"$GXX" $FLAGS -shared StepThumb.cpp step_core.o -o StepThumb.dll \
  -Wl,--enable-stdcall-fixup $LINK

echo "[3/6] step2png.exe"
"$GXX" $FLAGS step2png.cpp step_core.o -o step2png.exe $EXE_LINK

echo "[4/6] verify_com.exe"
"$GXX" $FLAGS verify_com.cpp -o verify_com.exe $EXE_LINK
"$GXX" $FLAGS verify_integration.cpp -o verify_integration.exe $EXE_LINK

echo "[5/6] resources"
"$WINDRES" -i setup.rc -o setup_res.o -O coff
"$WINDRES" -i setup-manifest.rc -o setup_manifest_res.o -O coff

echo "[6/6] StepThumbSetup.exe"
"$GXX" $FLAGS Setup.cpp setup_res.o setup_manifest_res.o -o StepThumbSetup.exe $EXE_LINK

echo
echo "[OK] 构建完成："
ls -l StepThumb.dll step2png.exe StepThumbSetup.exe verify_com.exe verify_integration.exe
