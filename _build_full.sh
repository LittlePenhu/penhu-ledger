#!/usr/bin/env bash
# =============================================================================
#  penhu-ledger 完整构建（含 SQLCipher / libsodium / cpp-httplib）
#
#  与 _build_portable.sh 的区别只有两点：链入 vcpkg 工具链、打开 native deps。
#  环境准备部分完全一样，原因见 _build_portable.sh 头部注释。
#
#  额外必须注入的环境变量（这是那个折腾了很久的坑，写在这里免得再踩）：
#    ProgramFiles(x86) / ProgramFiles / ProgramData
#  本 shell 环境会把这些变量过滤掉（`env | grep -i program` 为空），
#  而 vcpkg 的 CMake 工具链脚本在枚举 VS 实例时要用它们，
#  读不到就抛 "internal error: ...visualstudio.cpp(90): Value was null"
#  然后整个 configure 失败，错误信息完全看不出真正原因。
# =============================================================================
set -uo pipefail

# ---- 坑 D（与 _vcpkg.sh 里同一颗雷，必须一起防）-----------------------------
# 通过 Git Bash 的 env "PATH=<Windows 格式>" 传值时，MSYS 会对 PATH 做
# 「POSIX <-> Windows 路径列表」转换：它把 `C:\Program Files (x86)\...`
# 里的 `:` 当列表分隔符，首段被切成孤零零一个 `C`，真正的 SDK bin 目录
# 变成 `\Program Files (x86)\Windows Kits\...`（丢了盘符，不可用）。
# 症状是 rc.exe / mt.exe 找不到，而 cl.exe 正常——极容易被误当成「SDK 没装」。
export MSYS2_ENV_CONV_EXCL='*'
export MSYS2_ARG_CONV_EXCL='*'
export MSYS_NO_PATHCONV=1

# 路径不用手填：脚本就放在项目根目录，据此推算。
# vcpkg 和 ninja 默认放在项目根的上一级（vcpkg/、tools/），
# 摆在别处就用环境变量覆盖：VCPKG_WIN / NINJA_WIN。
PROJ_BASH="$(cd "$(dirname "$0")" && pwd)"
PROJ_WIN="$(cd "$PROJ_BASH" && pwd -W)"
ROOT_BASH="$(dirname "$PROJ_BASH")"
ROOT_WIN="$(cd "$ROOT_BASH" && pwd -W)"
NINJA_WIN="${NINJA_WIN:-$ROOT_WIN\\tools\\ninja.exe}"
VCPKG_WIN="${VCPKG_WIN:-$ROOT_WIN\\vcpkg}"
CMAKE="${CMAKE:-/c/Program Files/CMake/bin/cmake.exe}"
ENVFILE="$PROJ_BASH/_vsdev_env.txt"

SDK_VER='10.0.26100.0'
SDK_INC="C:\\Program Files (x86)\\Windows Kits\\10\\Include\\${SDK_VER}"
SDK_LIB="C:\\Program Files (x86)\\Windows Kits\\10\\Lib\\${SDK_VER}"
SDK_BIN="C:\\Program Files (x86)\\Windows Kits\\10\\bin\\${SDK_VER}\\x64"

BUILD_DIR_WIN="${PROJ_WIN}\\build"
BUILD_DIR_BASH="$PROJ_BASH/build"

# 只编这些目标；空 = 全部
TARGETS="${1:-}"

if [ ! -f "$ENVFILE" ]; then
  echo "缺少 $ENVFILE —— 需要先跑 PowerShell 导出 vcvars 环境" >&2
  exit 1
fi

WIN_PATH=""; INCLUDE_V=""; LIB_V=""
while IFS= read -r line; do
  line="${line%$'\r'}"
  [ -z "$line" ] && continue
  name="${line%%=*}"; val="${line#*=}"
  case "$name" in
    PATH)               WIN_PATH="$val" ;;
    INCLUDE)            INCLUDE_V="$val" ;;
    LIB)                LIB_V="$val" ;;
    VCINSTALLDIR|VCToolsInstallDir|VCToolsVersion|VSCMD_ARG_TGT_ARCH) export "$name=$val" ;;
    *) : ;;
  esac
done < "$ENVFILE"

INCLUDE_V="${INCLUDE_V};${SDK_INC}\\ucrt;${SDK_INC}\\um;${SDK_INC}\\shared;${SDK_INC}\\winrt;${SDK_INC}\\cppwinrt"
LIB_V="${LIB_V};${SDK_LIB}\\ucrt\\x64;${SDK_LIB}\\um\\x64"
WIN_PATH="${SDK_BIN};${WIN_PATH}"

run_cmake() {
  # 刻意不用 `env`：实测本机某些会话下 MSYS 的 `env` 拉起原生 exe 会吞掉
  # 子进程 stdout（退出码仍为 0），表现为「构建日志空白但看起来成功」。
  # 改用 export。注意 export 之后 bash 自己就找不到 Unix 工具了，
  # 所以调用它之后只做 builtin 的事（echo / test / exit）。
  export INCLUDE="$INCLUDE_V"
  export LIB="$LIB_V"
  export PATH="$WIN_PATH"
  "$CMAKE" "$@"
}

echo "--- 依赖是否就位 ---"
LIBDIR="$PROJ_BASH/vcpkg_installed/x64-windows-static/lib"
if [ -d "$LIBDIR" ]; then
  echo "vcpkg lib 数量: $(ls "$LIBDIR"/*.lib 2>/dev/null | wc -l)"
  ls "$LIBDIR"/*.lib 2>/dev/null | xargs -n1 basename 2>/dev/null | head -20
else
  echo "!! $LIBDIR 不存在，先跑 _vcpkg.sh install --triplet x64-windows-static" >&2
  exit 3
fi
echo ""

echo "--- configure ---"
# 显式指定 rc.exe / mt.exe：本机 vcvars 探测不到 Windows SDK，而 CMake
# 默认靠 PATH 找这两个工具。PATH 里其实有（SDK bin 被放在最前），
# 但这里再钉一层绝对路径——万一哪天 vcpkg 的 toolchain 补丁被 git pull 冲掉，
# 也不会退化成「找不到 rc.exe」这种看不出原因的失败。
RC_WIN='C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64/rc.exe'
MT_WIN='C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64/mt.exe'
run_cmake -S "$PROJ_WIN" -B "$BUILD_DIR_WIN" -G Ninja \
  -DCMAKE_MAKE_PROGRAM="$NINJA_WIN" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_RC_COMPILER="$RC_WIN" \
  -DCMAKE_MT="$MT_WIN" \
  -DCMAKE_TOOLCHAIN_FILE="${VCPKG_WIN}\\scripts\\buildsystems\\vcpkg.cmake" \
  -DVCPKG_TARGET_TRIPLET=x64-windows-static \
  -DVCPKG_INSTALLED_DIR="${PROJ_WIN}\\vcpkg_installed" \
  -DVCPKG_MANIFEST_MODE=OFF \
  -DPENHU_WITH_NATIVE_DEPS=ON \
  -DPENHU_BUILD_SERVER=ON \
  -DPENHU_BUILD_NATIVE=ON \
  -DPENHU_BUILD_CLI=ON \
  -DPENHU_BUILD_TESTS=ON 2>&1
CONFIG_RC=$?
echo "CONFIGURE_EXIT=$CONFIG_RC"
[ $CONFIG_RC -ne 0 ] && exit $CONFIG_RC

if [ -n "$TARGETS" ]; then
  echo ""
  echo "--- build: $TARGETS ---"
  # shellcheck disable=SC2086
  run_cmake --build "$BUILD_DIR_WIN" --target $TARGETS 2>&1
else
  echo ""
  echo "--- build: 全部 ---"
  run_cmake --build "$BUILD_DIR_WIN" 2>&1
fi
BUILD_RC=$?
echo "BUILD_EXIT=$BUILD_RC"
exit $BUILD_RC
