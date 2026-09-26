#!/usr/bin/env bash
# ---------------------------------------------------------------------------
#  vcpkg 包装脚本
#
#  这台机器上有三个环境坑会让 vcpkg 以完全看不懂的方式失败。三个都在这里补掉，
#  而不是靠记忆——下次换台机器、或者我自己半年后回来，照着注释就能排查。
#
#  ── 坑 1：ProgramFiles(x86) 被 shell 环境过滤掉 ─────────────────────────────
#  实测 `env | grep -i program` 结果为空：ProgramFiles / ProgramFiles(x86)
#  / ProgramData 全都不在，而 SYSTEMROOT、WINDIR 还在。
#
#  vcpkg-tool 的 src/vcpkg/visualstudio.cpp 第 90 行是：
#      const auto& program_files_32_bit =
#          get_program_files_32_bit().value_or_exit(VCPKG_LINE_INFO);
#  get_program_files_32_bit() 就是读环境变量 "ProgramFiles(x86)"。
#  读不到 -> Optional 为空 -> value_or_exit 抛出
#      "internal error: ...visualstudio.cpp(90): Value was null"
#  然后 vcpkg 非零退出，看起来像 VS 装坏了，其实跟 VS 无关。
#
#  ── 坑 2：vcvars64.bat 没有把 Windows SDK 的 include/lib 加进来 ─────────────
#  这台机器上 SDK 在磁盘里存在（10.0.26100.0），但没注册成 VS 组件，
#  vcvars 探测不到它。后果是 vcpkg 编译依赖时：
#      LINK : fatal error LNK1104: 无法打开文件"kernel32.lib"
#      RC   : "rc /fo ..." failed: no such file or directory
#  因为 cl.exe 是靠完整路径调用的（vcpkg 自己算出来），所以「能编译」，
#  但 INCLUDE/LIB 为空，「链接不了」。这一步的报错完全看不出和 SDK 有关。
#  解决：手工把 SDK 的 include/lib/bin 追加进去。
#
#  ---- 坑 3：MSYS 会把 PATH 当 POSIX 冒号分隔列表处理 ----
#  通过 Git Bash 的 `env "PATH=<Windows 格式的值>"` 传环境变量时，
#  MSYS 运行时会对名字叫 PATH 的变量做「POSIX <-> Windows 路径列表」转换。
#  它把 Windows 的 `C:\Program Files (x86)\...` 里的 `:` 当成列表分隔符，
#  于是首段被切成一个孤零零的 `C`，真正的 SDK bin 目录变成
#  `\Program Files (x86)\Windows Kits\...`（丢了盘符，不可用）。
#
#  症状极具误导性：MSVC 的 cl.exe 还能用（vcpkg 自己算绝对路径调用它），
#  INCLUDE/LIB 也正常（它们不是路径列表，不被转换），
#  但 rc.exe / mt.exe 是靠 PATH 找的，于是编译期报：
#      RC Pass 1: command "rc /fo ..." failed ... no such file or directory
#  看起来像「SDK 没装」，其实路径就在传参过程中被切坏了。
#
#  解决：禁止 MSYS 做这个转换。
export MSYS2_ENV_CONV_EXCL='*'
export MSYS2_ARG_CONV_EXCL='*'
export MSYS_NO_PATHCONV=1

#  ── 坑 5：环境块里出现「只有大小写不同」的重复键，会弄死 MSBuild ──────────────
#  MSBuild 的 ToolTask 在建 ProcessStartInfo 时会枚举本进程的环境变量，
#  逐个 Add 进一个**大小写不敏感**的字典。如果环境块里同时存在
#  SystemDrive 和 SYSTEMDRIVE（或 http_proxy 与 HTTP_PROXY），第二次 Add 就抛：
#      System.ArgumentException: 已添加项。
#      字典中的关键字:"SystemDrive"所添加的关键字:"SYSTEMDRIVE"
#  MSBuild 把这个异常包装成
#      error MSB6001: "CL.exe"的命令行开关无效
#  —— 报的是「CL.exe 开关无效」，跟环境变量毫无字面关联，极难猜。
#
#  为什么特别隐蔽：只有**用 MSBuild 构建的 port** 才会踩到（libsodium 就是），
#  用 nmake / jom 构建的（OpenSSL）完全不受影响。所以很容易误判成
#  「这个库本身有 bug」而不是「我的环境有问题」。
#
#  实测父子环境里真实存在的 case 重复：
#      SystemDrive   (本脚本显式设置) vs SYSTEMDRIVE  (父 shell)
#      http_proxy    (父 shell)        vs HTTP_PROXY  (父 shell)
#      https_proxy   (父 shell)        vs HTTPS_PROXY (父 shell)
#
#  修法：exec 之前算出差集，用 env -u 把「不该存在的那个写法」删掉。
#  下面这段会：
#    (a) 我要显式设置的键 —— 父环境里同名但大小写不同的全部删掉；
#    (b) 其余父环境变量 —— 同一小写折叠下只保留第一个，其余删掉。
#  不变量：进入 vcpkg 的环境块里，任何两个变量名都不存在大小写等价的重复。
#
# ---- 坑 4：vcpkg 自带 7-Zip（26.03）要从 GitHub releases 下，本机被代理拦 ────
#  （CONNECT tunnel failed, response 502）。已从 7-zip.org 下好放进
#  vcpkg/downloads/7z2603-x64.7z.exe 缓存，故此处不再需要系统 7z。
#  同类问题也会出现在其它从 GitHub releases 拉的东西上——如果哪天又卡在
#  某个 .part 文件 0 字节，先怀疑这条。
#
#  另外：只跑单个 vcpkg 实例。之前两个实例并发导致互踩锁，报错信息毫无意义。
#
#  ── 怎么确认环境真的传下去了（这个排查手法值得记住）────────────────────────
#  怀疑环境变量没传下去时，不要靠猜，直接问 vcpkg 它给子进程的完整环境：
#
#      # dump_child_env.bat 的内容就是一行： set > %USERPROFILE%\penhu_child_env.txt
#      bash _vcpkg.sh env --triplet x64-windows-static '%USERPROFILE%\dump_child_env.bat'
#      grep -aiE '^(INCLUDE|LIB)=' "$HOME/penhu_child_env.txt"
#
#  实测结论（2026-09-18）：vcpkg **会**把调用方的 INCLUDE / LIB 原样传给子进程，
#  但它自己**不会**生成这两个变量。所以「vcvars 漏了 SDK」这个坑必须由本脚本
#  补掉，指望 vcpkg 兜底是不行的。
#  注意 `vcpkg env` 只能接 0 或 1 个参数，而且那个参数是「整条命令字符串」，
#  用 `--` 分隔或传多个参数都会报 unexpected argument。
#
#  ── 两个环境限制，绕不过去，只能知情 ────────────────────────────────────────
#  1) `reg.exe` 被安全策略的 Program Blacklist 硬拦，提示明确写着
#     「cannot be approved or bypassed from the current command」。
#     所以任何依赖 reg.exe 的路径都走不通（vcpkg 的某些探测会碰到它）。
#  2) `cmd.exe` 也可能被拦。上面那个 `vcpkg env -- xxx.bat` 之所以能跑，
#     是因为 vcpkg 自己起的 cmd，不是我们从 bash 起的。
# ---------------------------------------------------------------------------
set -uo pipefail

# 路径不用手填：脚本就放在项目根目录，据此推算。vcpkg 默认在项目根的上一级。
PROJ_BASH="$(cd "$(dirname "$0")" && pwd)"
ROOT_BASH="$(dirname "$PROJ_BASH")"
VCPKG_EXE="${VCPKG_EXE:-$ROOT_BASH/vcpkg/vcpkg.exe}"
ENVFILE="$PROJ_BASH/_vsdev_env.txt"

SDK_VER='10.0.26100.0'
SDK_INC="C:\\Program Files (x86)\\Windows Kits\\10\\Include\\${SDK_VER}"
SDK_LIB="C:\\Program Files (x86)\\Windows Kits\\10\\Lib\\${SDK_VER}"
SDK_BIN="C:\\Program Files (x86)\\Windows Kits\\10\\bin\\${SDK_VER}\\x64"

if [ ! -f "$ENVFILE" ]; then
  echo "缺少 $ENVFILE —— 需要先跑 PowerShell 导出 vcvars 环境（见 README 构建一节）" >&2
  exit 1
fi

# 只跑一个实例
if tasklist 2>/dev/null | grep -qi "vcpkg.exe"; then
  echo "!! 已有 vcpkg.exe 在运行。等它结束或先杀掉，避免两个实例互踩锁。" >&2
  exit 2
fi

# ---- 从 vcvars 导出文件里取 PATH / INCLUDE / LIB（白名单，绝不整体覆盖）----
WIN_PATH=""; INCLUDE_V=""; LIB_V=""
while IFS= read -r line; do
  line="${line%$'\r'}"
  [ -z "$line" ] && continue
  name="${line%%=*}"; val="${line#*=}"
  case "$name" in
    PATH)    WIN_PATH="$val" ;;
    INCLUDE) INCLUDE_V="$val" ;;
    LIB)     LIB_V="$val" ;;
    *) : ;;
  esac
done < "$ENVFILE"

if [ -z "$INCLUDE_V" ] || [ -z "$LIB_V" ]; then
  echo "!! $ENVFILE 里没有 INCLUDE/LIB —— vcvars 导出不完整，重新导出一次" >&2
  exit 1
fi

# ---- 坑 2：补上 vcvars 漏掉的 Windows SDK ----
INCLUDE_V="${INCLUDE_V};${SDK_INC}\\ucrt;${SDK_INC}\\um;${SDK_INC}\\shared;${SDK_INC}\\winrt;${SDK_INC}\\cppwinrt"
LIB_V="${LIB_V};${SDK_LIB}\\ucrt\\x64;${SDK_LIB}\\um\\x64"
WIN_PATH="${SDK_BIN};${WIN_PATH}"

echo "--- vcpkg 环境 ---"
echo "INCLUDE 段数  : $(echo "$INCLUDE_V" | tr ';' '\n' | wc -l)"
echo "LIB 段数      : $(echo "$LIB_V" | tr ';' '\n' | wc -l)"
echo "cl.exe 在 PATH: $(echo "$WIN_PATH" | tr ';' '\n' | grep -ic 'Hostx64' || true)"
echo "rc.exe 在 PATH: $(echo "$WIN_PATH" | tr ';' '\n' | grep -ic 'Windows Kits' || true)"
echo "PATH 首段     : $(echo "$WIN_PATH" | cut -d';' -f1)"
echo ""

# ---- 关键补丁：让 CMake 直接从 SDK 目录找 rc.exe / mt.exe ----
#
#  背景：本机 vcvars 给的 PATH 里没有 Windows SDK 的 bin，
#  而 vcpkg **会用自己算的 PATH 覆盖调用方的 PATH**（已实测确认），
#  所以往 PATH 里加 SDK bin 对 vcpkg 无效。
#  overlay triplet 里的 set(ENV{PATH}) 能救目标 triplet，
#  但 vcpkg 对**宿主 triplet**（x64-windows，pkgconf/meson 这类构建工具用）
#  不做 overlay 解析 —— 于是 pkgconf 仍然失败在
#      RC Pass 1: command "rc /fo ..." failed: no such file or directory
#
#  换成不动 PATH 的思路：CMake 会用同名环境变量初始化 CMAKE_PROGRAM_PATH，
#  而 find_program() 会搜索它。给上 SDK 的 bin 目录，
#  CMake 自己找编译器配套工具时就能找到 rc.exe / mt.exe，
#  跟 PATH 里有没有无关。
#
#  实测：INCLUDE / LIB 能穿过 vcpkg 传到子进程，环境变量通道是通的，
#  所以这条同样应该能到。记得把它加进 VCPKG_KEEP_ENV_VARS。
PROGRAM_PATH_WIN='C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64'

# ---- 用 overlay triplet 钉住 rc.exe / mt.exe 的绝对路径 ----
# 双保险：即使 PATH 在某一层被改坏，CMake 也已经拿到绝对路径，
# 不会再出现「rc.exe 找不到但看不出来为什么」。详见 triplets/ 下的注释。
OVERLAY_TRIPLETS_WIN="$(cd "$PROJ_BASH" && pwd -W)\\triplets"

# =============================================================================
#  坑 J：MSBuild 构建的 port 拿不到工具链 include 路径（libsodium 就是这样炸的）
#
#  症状：libsodium 编译报
#      error C1083: 无法打开包括文件: "stddef.h" / "string.h"
#  去看 cl.exe 的命令行，/I 里**只有 libsodium 自己的目录**，
#  MSVC 的 include 和 Windows SDK 的 ucrt/um 一个都没有。
#
#  根因分两层：
#   1) winget 用 --override 装的 BuildTools 实例元数据不完整：
#      有 Msbuild 目录、VC 工具集也在（14.44.35207），但 Windows SDK
#      没注册成该实例的组件。于是 MSBuild 里
#      $(VC_IncludePath) / $(WindowsSDK_IncludePath) 都解析为空。
#      nmake/jom 路线不受影响（OpenSSL 用 INCLUDE 环境变量就能编过），
#      所以这个坑只在 MSBuild 的 port 上暴露 —— 极容易被误判成「库有 bug」。
#   2) CMake/Ninja 那条路我们靠 INCLUDE/LIB 环境变量兜住了；
#      MSBuild 不吃 INCLUDE，它用 $(IncludePath) / $(LibraryPath) 属性。
#
#  修法：直接把工具链/SDK 属性以环境变量形式喂给 MSBuild。
#  MSBuild 启动时会把环境变量导入成同名属性，所以设 IncludePath / LibraryPath /
#  WindowsSdkDir / WindowsSDKVersion 这些名字是有效的，而且不影响编译器选择
#  （所以不会改 ABI、不会触发全量重编）。
#
#  -- 走过的弯路，记下来免得再试 --
#  另一条看起来更"正确"的路是让 vcpkg 改用 Community 实例
#  （C:\Program Files\Microsoft Visual Studio\2022\Community，SDK 注册完整）。
#  结果 vcpkg 直接报：
#      error: in triplet x64-windows-static: Unable to find a valid Visual Studio instance
#        at "C:\Program Files\Microsoft Visual Studio\2022\Community"
#      The following Visual Studio instances were considered:
#        C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools
#  而同一时刻 `vswhere -latest -products * -requires
#  Microsoft.VisualStudio.Component.VC.Tools.x86.x64` 返回的正是 Community。
#  也就是说 **vcpkg 的实例判定规则与 vswhere 的常见用法不一致**，
#  Community 明明有 vcvarsall.bat、vswhere 也认它，vcpkg 就是不认。
#  别再在这条路上花时间：调 VCPKG_VISUAL_STUDIO_PATH 指向 Community 只会
#  让 vcpkg 直接失败。这里显式钉回 BuildTools（vcpkg 自己会选的那个）。
# =============================================================================
VS_PATH_WIN='C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools'
VCTOOLS_VER='14.44.35207'
KITS_WIN='C:\Program Files (x86)\Windows Kits\10'

VCTOOLS_DIR_WIN="${VS_PATH_WIN}\\VC\\Tools\\MSVC\\${VCTOOLS_VER}\\"
VCINSTALL_WIN="${VS_PATH_WIN}\\VC\\"

SDK_INC_PROP="${KITS_WIN}\\Include\\${SDK_VER}\\ucrt;${KITS_WIN}\\Include\\${SDK_VER}\\um;${KITS_WIN}\\Include\\${SDK_VER}\\shared;${KITS_WIN}\\Include\\${SDK_VER}\\winrt;${KITS_WIN}\\Include\\${SDK_VER}\\cppwinrt"
SDK_LIB_PROP="${KITS_WIN}\\Lib\\${SDK_VER}\\ucrt\\x64;${KITS_WIN}\\Lib\\${SDK_VER}\\um\\x64"

# IncludePath / LibraryPath 用完整的 INCLUDE_V / LIB_V（含 MSVC 自己的 include），
# 不能只给 SDK 那部分 —— stddef.h 在 MSVC 的 include 里。
KEEP_ENV_VARS='ProgramFiles(x86);ProgramFiles;ProgramData;INCLUDE;LIB;CMAKE_PROGRAM_PATH;VCPKG_VISUAL_STUDIO_PATH;VCToolsInstallDir;VCInstallDir;VSInstallDir;WindowsSdkDir;WindowsSDKVersion;UniversalCRTSdkDir;UCRTVersion;IncludePath;LibraryPath'

# ---- 坑 5 的实现：算出需要删掉的大小写重复项 ----
#  注意别在这里 fork 子进程（比如用 tr 转小写）：父环境有两百多个变量、
#  CANON 有二十多项，逐对比较会变成上万次 fork，脚本会卡死在中途
#  （第一次写成这样，实测把脚本拖到被强制终止）。
#  用 bash 内建的 ${var,,} 转小写，两趟线性扫描即可。
#
#  CANON 里除了我显式设置的键，还有两批：
#   · VCPKG_VISUAL_STUDIO_PATH / VCToolsInstallDir / WindowsSdkDir / IncludePath …
#     这些是给 MSBuild 补工具链用的，见「坑 J」。
#   · 代理、临时目录之类必须存在的。
CANON=(
  PATH INCLUDE LIB CMAKE_PROGRAM_PATH
  ProgramFiles "ProgramFiles(x86)" ProgramW6432
  CommonProgramFiles "CommonProgramFiles(x86)"
  ProgramData ALLUSERSPROFILE USERPROFILE APPDATA LOCALAPPDATA
  HOMEDRIVE HOMEPATH TEMP TMP SystemDrive SystemRoot WINDIR
  VCPKG_DEFAULT_TRIPLET VCPKG_MAX_CONCURRENCY
  VCPKG_OVERLAY_TRIPLETS VCPKG_KEEP_ENV_VARS
  VCPKG_VISUAL_STUDIO_PATH
  VCToolsInstallDir VCInstallDir VSInstallDir
  WindowsSdkDir WindowsSDKVersion UniversalCRTSdkDir UCRTVersion
  WindowsSDK_IncludePath WindowsSDK_LibraryPath_x64
  IncludePath LibraryPath
)

declare -A _canon_by_lc=()
for _c in "${CANON[@]}"; do
  _canon_by_lc["${_c,,}"]="$_c"
done

#  LC_ALL=C sort 让全大写的写法排在前面（字节序里 'A'..'Z' < 'a'..'z'），
#  这样「同小写只留第一个」的结果是确定的全大写，不会随 env 的输出顺序变来变去。
#  代理变量上这一点很重要：libcurl 对 http:// 查 http_proxy 再查 HTTP_PROXY，
#  留哪个都能用，但不该让它随机。
mapfile -t _parent_names < <(env | sed 's/=.*//' | LC_ALL=C sort)

UNSET_NAMES=()
declare -A _kept_lc=()
for _nm in "${_parent_names[@]}"; do
  [ -z "$_nm" ] && continue
  _lc="${_nm,,}"
  _want="${_canon_by_lc[$_lc]:-}"
  if [ -n "$_want" ]; then
    # 我会显式设置规范写法；父环境里的其它写法（含同名）一律删掉再设
    if [ "$_nm" != "$_want" ]; then
      UNSET_NAMES+=("$_nm")
      echo "  [去重] 删掉父环境的 '$_nm'，改用规范写法 '$_want'"
    fi
  else
    # 不归我管的变量：同一小写折叠下只保留第一个
    if [ -n "${_kept_lc[$_lc]:-}" ]; then
      UNSET_NAMES+=("$_nm")
      echo "  [去重] 删掉父环境的 '$_nm'（与 '${_kept_lc[$_lc]}' 仅大小写不同）"
    else
      _kept_lc["$_lc"]="$_nm"
    fi
  fi
done

# 自检：CANON 自身不允许出现同小写重名（否则 -u 与设置会互相打架）
declare -A _check=()
for _c in "${CANON[@]}"; do
  _lc="${_c,,}"
  if [ -n "${_check[$_lc]:-}" ]; then
    echo "!! 脚本配置错误：CANON 里 '$_c' 与 '${_check[$_lc]}' 大小写折叠后重名" >&2
    exit 1
  fi
  _check["$_lc"]="$_c"
done
echo "  去重项数: ${#UNSET_NAMES[@]}"
echo ""

# ---- 去重：直接从 bash 环境里 unset ----
#  不用 `env -u`，理由见下面「坑 K」。
for _n in ${UNSET_NAMES+"${UNSET_NAMES[@]}"}; do
  unset "$_n" 2>/dev/null || true
done

# =============================================================================
#  坑 K：不要用 `env` 来设置环境后调用 vcpkg（本轮踩到，且症状极具欺骗性）
#
#  症状：包装脚本跑完，日志里**只有脚本自己的几行输出**，
#  vcpkg 一行都没有，而**退出码是 0**。看起来像「vcpkg 什么都没干就成功了」，
#  也像「依赖已经装完、没事可做」——两种都是错的。
#
#  实测（三种写法，输出都重定向到文件后比大小）：
#      env FOO=1 vcpkg.exe version        -> 0 字节,   rc=0   ← 输出被吞，甚至可能没起来
#      vcpkg.exe version                  -> 135 字节, rc=0   ← 正常
#      FOO=1 bash -c 'vcpkg.exe version'  -> 135 字节, rc=0   ← 正常
#  也就是说这台机器当前会话下，MSYS 的 `env` 在拉起原生 Windows exe 时
#  会把子进程的 stdout 丢掉。同样的脚本前一天还能正常跑 —— 环境是会变的，
#  所以这里不要依赖 `env`。
#
#  改法：直接用 bash 的 export 把变量放进自己的环境（bash 自己就在关键路径上，
#  这条路已被证明可靠），然后 exec vcpkg。
#  代价：bash 接下来看到的是 Windows 格式的 PATH，脚本里的 tr/grep 会找不到 ——
#  所以这段必须放在脚本**最末尾**，后面只剩一句 exec，不再调用任何 Unix 工具。
# =============================================================================
export PATH="$WIN_PATH"
export INCLUDE="$INCLUDE_V"
export LIB="$LIB_V"
export CMAKE_PROGRAM_PATH="$PROGRAM_PATH_WIN"
# 注意：ProgramFiles(x86) / CommonProgramFiles(x86) 这两类带括号的名字
# **没法用 bash 的 export 设置** —— bash 会校验标识符，直接报
#     export: `ProgramFiles(x86)=...': not a valid identifier
# 早期版本这里还依赖 ProgramFiles(x86)（vcpkg-tool 的 visualstudio.cpp:90
# 读它来定位 32 位程序目录），现在不需要了：因为我们显式给了
# VCPKG_VISUAL_STUDIO_PATH，vcpkg 直接用它，不再走那条会读该变量的
# 实例枚举路径（实测能正常打印 "Detecting compiler hash for triplet ..."）。
# 如果哪天又出现 visualstudio.cpp(90): Value was null，就得想办法注入它 ——
# 那种情况下只能从调用方（外层 shell）传，脚本内部做不到。
export ProgramFiles="C:\\Program Files"
export ProgramW6432="C:\\Program Files"
export CommonProgramFiles="C:\\Program Files\\Common Files"
export ProgramData="C:\\ProgramData"
export ALLUSERSPROFILE="C:\\ProgramData"
HOME_WIN="$(cygpath -w "$HOME")"
export USERPROFILE="$HOME_WIN"
export APPDATA="$HOME_WIN\\AppData\\Roaming"
export LOCALAPPDATA="$HOME_WIN\\AppData\\Local"
export HOMEDRIVE="${HOME_WIN%%:*}:"
export HOMEPATH="${HOME_WIN#*:}"
export TEMP="$(cygpath -w /tmp)"
export TMP="$(cygpath -w /tmp)"
export SystemDrive="C:"
export SystemRoot="C:\\Windows"
export WINDIR="C:\\Windows"
export VCPKG_DEFAULT_TRIPLET="x64-windows-static"
export VCPKG_MAX_CONCURRENCY=3
export VCPKG_OVERLAY_TRIPLETS="$OVERLAY_TRIPLETS_WIN"
export VCPKG_KEEP_ENV_VARS="$KEEP_ENV_VARS"
export VCPKG_VISUAL_STUDIO_PATH="$VS_PATH_WIN"
export VCToolsInstallDir="$VCTOOLS_DIR_WIN"
export VCInstallDir="$VCINSTALL_WIN"
export VSInstallDir="${VS_PATH_WIN}\\"
export WindowsSdkDir="${KITS_WIN}\\"
export WindowsSDKVersion="${SDK_VER}\\"
export UniversalCRTSdkDir="${KITS_WIN}\\"
export UCRTVersion="${SDK_VER}"
export WindowsSDK_IncludePath="$SDK_INC_PROP"
export WindowsSDK_LibraryPath_x64="$SDK_LIB_PROP"
export IncludePath="$INCLUDE_V"
export LibraryPath="$LIB_V"

exec "$VCPKG_EXE" "$@"
