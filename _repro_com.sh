#!/usr/bin/env bash
# 编译并运行 _repro_com.cpp（复用 _build_full.sh 的 MSVC 环境加载方式）
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

INCLUDE_V="${INCLUDE_V};${SDK_INC}\\ucrt;${SDK_INC}\\um;${SDK_INC}\\shared"
LIB_V="${LIB_V};${SDK_LIB}\\ucrt\\x64;${SDK_LIB}\\um\\x64"
WIN_PATH="${SDK_BIN};${WIN_PATH}"

export INCLUDE="$INCLUDE_V"
export LIB="$LIB_V"
export PATH="$WIN_PATH"

cd "$PROJ_BASH" || exit 1

# ⚠️ 两个坑叠在一起：
#  1. 导出 Windows 风格的 PATH 之后，bash 里只剩 builtin 可用（tail/grep 都找不到），
#     所以下面不接管道。
#  2. bash 不认 `C:\...` 这种带反斜杠的 PATH 项，cl.exe 必须用 **Unix 风格绝对路径** 调
#     （和 _build_full.sh 里调 cmake/rc.exe 是同一个道理）。
CL="/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207/bin/HostX64/x64/cl.exe"

echo "--- 编译 ---"
"$CL" /nologo /utf-8 /std:c++20 /EHsc /O2 /W4 _repro_com.cpp \
    /link ole32.lib windowscodecs.lib uuid.lib /OUT:_repro_com.exe
echo "cl exit=$?"

echo ""
echo "--- 运行 ---"
./_repro_com.exe
echo "run exit=$?"
