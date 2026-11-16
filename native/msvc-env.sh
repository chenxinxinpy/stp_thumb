# MSVC 环境（不用 vcvarsall：它会调用 reg.exe，被安全策略拦截）
# 用法: source msvc-env.sh
VCWIN='C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207'
SDKWIN='C:\Program Files (x86)\Windows Kits\10'
SDKVER='10.0.26100.0'

export INCLUDE="$VCWIN\\include;$SDKWIN\\Include\\$SDKVER\\ucrt;$SDKWIN\\Include\\$SDKVER\\shared;$SDKWIN\\Include\\$SDKVER\\um;$SDKWIN\\Include\\$SDKVER\\winrt"
export LIB="$VCWIN\\lib\\x64;$SDKWIN\\Lib\\$SDKVER\\ucrt\\x64;$SDKWIN\\Lib\\$SDKVER\\um\\x64"

VCPOSIX='/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207'
export PATH="$VCPOSIX/bin/Hostx64/x64:$PATH"
