# =============================================================================
#  penhu-ledger/triplets/x64-windows-static.cmake
#  目标 triplet 的覆盖版（我们最终要链接的静态库）。
#
#  用法（_vcpkg.sh 里通过 VCPKG_OVERLAY_TRIPLETS 指定）：
#      vcpkg install --triplet x64-windows-static
#
#  ---- 为什么需要覆盖 -----------------------------------------------------
#
#  这台机器上 vcvars（Enter-VsDevShell）给出的环境里，
#  INCLUDE / LIB / PATH **完全不含任何 Windows SDK 路径**，只有 MSVC 的：
#
#      INCLUDE = ...\VC\Tools\MSVC\14.44.35207\include;...\VC\Auxiliary\VS\include
#      LIB     = ...\VC\Tools\MSVC\14.44.35207\lib\x64
#      PATH    = ...\VC\Tools\MSVC\14.44.35207\bin\HostX64\x64;...
#
#  但 SDK 本身是好的：KitsRoot10 注册表项指向 C:\Program Files (x86)\Windows Kits\10，
#  bin\10.0.26100.0\x64 下有 rc.exe / mt.exe，Include 与 Lib 也齐全。
#  原因是 BuildTools 当初用 winget `--override` 安装，替换了默认组件集，
#  SDK 没注册成该 VS 实例的组件，于是 vsdevcmd 的 winsdk 探测被跳过。
#
#  后果（两个报错都看不出和 SDK 有关）：
#    · INCLUDE/LIB 缺 SDK -> LNK1104: 无法打开文件"kernel32.lib"
#    · PATH 缺 SDK bin   -> RC Pass 1: command "rc /fo ..." failed:
#                            no such file or directory
#
#  ---- 为什么不能靠外面传 PATH 解决 --------------------------------------
#
#  实测（用 `vcpkg env --triplet ... -- <dump env 的 bat>` 导出子进程环境）：
#  vcpkg **会用自己算出来的 PATH 覆盖调用方的 PATH**，而 INCLUDE / LIB 会保留。
#  所以「在 bash 里 export PATH=SDK_BIN;...」对 vcpkg 无效——
#  子进程环境里 SDK bin 一次都不出现。
#
#  ---- 已放弃的做法（留档，免得再试一遍）--------------------------------
#
#  用 Git for Windows 自带的 Perl 去编 OpenSSL：不行。
#  那份是精简发行版，缺 Locale::Maketext::Simple 等核心模块，
#  OpenSSL 的 Configure 直接挂：
#      Can't locate Locale/Maketext/Simple.pm in @INC
#        at .../Params/Check.pm line 6.
#      Compilation failed in require at .../IPC/Cmd.pm line 59.
#  必须用完整的 Strawberry Perl（约 290MB，本机网络下不来时用 _fetch_perl.sh
#  走 ghfast.top 镜像断点续传）。
#
#  注意：改 triplet 会改变 ABI 哈希，所有 port 会重新构建。
#  从零装依赖时这个代价是零。
# =============================================================================

set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_PROVIDED_FORTRAN ON)

include("${CMAKE_CURRENT_LIST_DIR}/penhu-sdk-inject.cmake")
