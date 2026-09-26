#!/usr/bin/env bash
# =============================================================================
#  penhu-ledger portable 构建（bash 驱动）
#
#  为什么走这条路：
#    这台机器有 MSVC 但没有 MSBuild，CMake 的 "Visual Studio 17 2022" 生成器
#    不可用，只能用 Ninja；Ninja 需要 vcvars 提供的 cl.exe / INCLUDE / LIB。
#    PowerShell 在本环境里对外部进程调用不稳定，所以：
#      PowerShell 只负责把 vcvars 环境导出成文本  ->  bash 加载后调 cmake
#
#  两个必须注意的坑（都是踩过的）：
#   1) vcvars64.bat 没有把 Windows SDK 的 include/lib 加进来。
#      这台机器上 SDK 在磁盘里存在，但没注册成 VS 组件，vcvars 探测不到，
#      于是任何 include <windows.h> 的编译单元都编不过。下面手工补。
#   2) 环境里的 PATH 是 Windows 格式（分号分隔）。绝不能 export 到 bash 里，
#      否则 bash 找不到 tr/grep 这些工具。所以只以白名单方式注入给 cmake 子进程。
#   3) cmake.exe 是原生程序，不认 /c/... 这种 MSYS 路径，必须传 C:\... 格式。
#   4) 通过 `env "PATH=<Windows 路径列表>"` 传值时，MSYS 运行时会把它当成
#      POSIX 冒号分隔列表去转换，把 `C:\Program Files...` 从 `:` 处切开——
#      首段变成一个孤零零的 `C`，SDK bin 目录丢掉盘符。
#      这个脚本之前只是"碰巧还能用"：坏掉的那段成了盘符相对路径
#      `\Program Files (x86)\Windows Kits\...`，在当前盘是 C: 时仍然能找到 rc.exe。
#      一旦换了工作目录所在盘就会突然失败，属于埋着的地雷。所以关掉转换。
# =============================================================================
set -uo pipefail

export MSYS2_ENV_CONV_EXCL='*'
export MSYS2_ARG_CONV_EXCL='*'
export MSYS_NO_PATHCONV=1

# 路径不用手填：脚本就放在项目根目录，据此推算（ninja 默认在上一级的 tools/）
PROJ_BASH="$(cd "$(dirname "$0")" && pwd)"
PROJ_WIN="$(cd "$PROJ_BASH" && pwd -W)"
ROOT_BASH="$(dirname "$PROJ_BASH")"
ROOT_WIN="$(cd "$ROOT_BASH" && pwd -W)"
NINJA_WIN="${NINJA_WIN:-$ROOT_WIN\\tools\\ninja.exe}"
CMAKE='/c/Program Files/CMake/bin/cmake.exe'
ENVFILE="$PROJ_BASH/_vsdev_env.txt"

SDK_VER='10.0.26100.0'
SDK_INC="C:\\Program Files (x86)\\Windows Kits\\10\\Include\\${SDK_VER}"
SDK_LIB="C:\\Program Files (x86)\\Windows Kits\\10\\Lib\\${SDK_VER}"
SDK_BIN="C:\\Program Files (x86)\\Windows Kits\\10\\bin\\${SDK_VER}\\x64"

if [ ! -f "$ENVFILE" ]; then
  echo "缺少 $ENVFILE —— 需要先跑 PowerShell 导出 vcvars 环境" >&2
  exit 1
fi

# ---- 从导出文件里挑出需要的变量（白名单，绝不覆盖 bash 的 PATH）----
WIN_PATH=""
INCLUDE_V=""
LIB_V=""
while IFS= read -r line; do
  line="${line%$'\r'}"
  [ -z "$line" ] && continue
  name="${line%%=*}"
  val="${line#*=}"
  case "$name" in
    PATH)               WIN_PATH="$val" ;;
    INCLUDE)            INCLUDE_V="$val" ;;
    LIB)                LIB_V="$val" ;;
    VCINSTALLDIR|VCToolsInstallDir|VCToolsVersion|VSCMD_ARG_TGT_ARCH) export "$name=$val" ;;
    *) : ;;
  esac
done < "$ENVFILE"

# ---- 补上 vcvars 漏掉的 Windows SDK ----
INCLUDE_V="${INCLUDE_V};${SDK_INC}\\ucrt;${SDK_INC}\\um;${SDK_INC}\\shared;${SDK_INC}\\winrt;${SDK_INC}\\cppwinrt"
LIB_V="${LIB_V};${SDK_LIB}\\ucrt\\x64;${SDK_LIB}\\um\\x64"
WIN_PATH="${SDK_BIN};${WIN_PATH}"

echo "--- 环境检查 ---"
echo "INCLUDE 段数   : $(echo "$INCLUDE_V" | tr ';' '\n' | wc -l)"
echo "LIB 段数       : $(echo "$LIB_V" | tr ';' '\n' | wc -l)"
echo "SDK ucrt 存在  : $([ -d "/c/Program Files (x86)/Windows Kits/10/Include/${SDK_VER}/ucrt" ] && echo yes || echo NO)"
echo "cl.exe 在 PATH : $(echo "$WIN_PATH" | tr ';' '\n' | grep -c 'Hostx64')"
echo "rc.exe 在 PATH : $(echo "$WIN_PATH" | tr ';' '\n' | grep -c 'Windows Kits')"
echo ""

# ---- 用注入了 Windows 环境的方式调用 cmake ----
#
# 这里刻意**不用 `env`**：实测在本机某些会话里，MSYS 的 `env` 拉起原生 exe 时
# 会把子进程的 stdout 整个丢掉（甚至可能根本没起来），而**退出码还是 0**。
# 现象是「构建日志一片空白、看起来成功」，极具欺骗性。
#   实测对比：env FOO=1 cmake.exe --version  -> 0 字节, rc=0
#            cmake.exe --version              -> 正常输出, rc=0
# 改成 bash 的 export —— bash 自己就在关键路径上，这条路可靠。
#
# 代价：export 一个 Windows 格式的 PATH 之后，bash 就找不到 tr/grep 这些
# Unix 工具了。所以这个函数一旦被调用，后面**只能做 builtin 的事**
# （echo / test / exit 都是 builtin，没问题）。
run_cmake() {
  export INCLUDE="$INCLUDE_V"
  export LIB="$LIB_V"
  export PATH="$WIN_PATH"
  "$CMAKE" "$@"
}

BUILD_DIR_WIN="${PROJ_WIN}\\build-portable"

echo "--- configure ---"
run_cmake -S "$PROJ_WIN" -B "$BUILD_DIR_WIN" -G Ninja \
  -DCMAKE_MAKE_PROGRAM="$NINJA_WIN" \
  -DCMAKE_BUILD_TYPE=Release \
  -DPENHU_WITH_NATIVE_DEPS=OFF \
  -DPENHU_BUILD_TESTS=ON 2>&1
CONFIG_RC=$?
echo "CONFIGURE_EXIT=$CONFIG_RC"
[ $CONFIG_RC -ne 0 ] && exit $CONFIG_RC

echo ""
echo "--- build ---"
run_cmake --build "$BUILD_DIR_WIN" --target penhu-tests-portable 2>&1
BUILD_RC=$?
echo "BUILD_EXIT=$BUILD_RC"
exit $BUILD_RC
