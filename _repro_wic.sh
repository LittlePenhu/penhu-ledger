#!/usr/bin/env bash
# 编译并运行 _repro_wic.cpp（把应用真正的 image_util.cpp 一起编进来）
set -uo pipefail

export MSYS2_ENV_CONV_EXCL='*'
export MSYS2_ARG_CONV_EXCL='*'
export MSYS_NO_PATHCONV=1

PROJ_BASH="$(cd "$(dirname "$0")" && pwd)"
ENVFILE="$PROJ_BASH/_vsdev_env.txt"

SDK_VER='10.0.26100.0'
SDK_INC="C:\\Program Files (x86)\\Windows Kits\\10\\Include\\${SDK_VER}"
SDK_LIB="C:\\Program Files (x86)\\Windows Kits\\10\\Lib\\${SDK_VER}"
SDK_BIN="C:\\Program Files (x86)\\Windows Kits\\10\\bin\\${SDK_VER}\\x64"

WIN_PATH=""; INCLUDE_V=""; LIB_V=""
while IFS= read -r line; do
  line="${line%$'\r'}"
  [ -z "$line" ] && continue
  name="${line%%=*}"; val="${line#*=}"
  case "$name" in
    PATH)    WIN_PATH="$val" ;;
    INCLUDE) INCLUDE_V="$val" ;;
    LIB)     LIB_V="$val" ;;
    VCINSTALLDIR|VCToolsInstallDir|VCToolsVersion|VSCMD_ARG_TGT_ARCH) export "$name=$val" ;;
    *) : ;;
  esac
done < "$ENVFILE"

INCLUDE_V="${INCLUDE_V};${SDK_INC}\\ucrt;${SDK_INC}\\um;${SDK_INC}\\shared;${SDK_INC}\\winrt;${SDK_INC}\\cppwinrt"
LIB_V="${LIB_V};${SDK_LIB}\\ucrt\\x64;${SDK_LIB}\\um\\x64"
WIN_PATH="${SDK_BIN};${WIN_PATH}"

export INCLUDE="$INCLUDE_V"
export LIB="$LIB_V"
export PATH="$WIN_PATH"

cd "$PROJ_BASH" || exit 1
CL="/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207/bin/HostX64/x64/cl.exe"

echo "--- 编译（_repro_wic.cpp + 应用真实的 image_util.cpp）---"
"$CL" /nologo /utf-8 /std:c++20 /EHsc /O2 /W4 -DNOMINMAX \
    -I native/src \
    _repro_wic.cpp native/src/app/image_util.cpp \
    /link ole32.lib oleaut32.lib windowscodecs.lib uuid.lib /OUT:_repro_wic.exe
echo "cl exit=$?"

echo ""
echo "--- 运行（识别路径的真实行为）---"
./_repro_wic.exe "$REPRO_INPUT"
echo "run exit=$?"
