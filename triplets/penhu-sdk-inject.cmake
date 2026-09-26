# =============================================================================
#  penhu-ledger/triplets/penhu-sdk-inject.cmake
#  被 x64-windows-static.cmake 与 x64-windows.cmake 共同 include。
#
#  为什么需要两个 triplet：
#    vcpkg 装依赖时会同时用到「目标 triplet」（x64-windows-static，产出静态库）
#    和「宿主 triplet」（x64-windows，用于 pkgconf / meson 这类构建期工具）。
#    只覆盖 static 那个是不够的——pkgconf 走的正是 x64-windows，
#    于是 rc.exe 找不到，构建失败，而且报错完全看不出是 triplet 没覆盖。
#
#  修的问题（详见 x64-windows-static.cmake 里的长注释）：
#    这台机器的 vcvars 拿不到 Windows SDK 的 include/lib/bin，
#    而 vcpkg 会用自己算出的 PATH 覆盖调用方的 PATH，
#    所以唯一的着力点是 triplet —— 它在 configure 进程里运行，
#    而编译器检测(TryCompile)是 configure 的子进程链，
#    会继承这里 set(ENV{PATH}) 的结果。
# =============================================================================

set(_penhu_sdk_bin "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64")

if(EXISTS "${_penhu_sdk_bin}/rc.exe")
    file(TO_NATIVE_PATH "${_penhu_sdk_bin}" _penhu_sdk_bin_native)
    set(ENV{PATH} "${_penhu_sdk_bin_native};$ENV{PATH}")
    set(CMAKE_RC_COMPILER "${_penhu_sdk_bin}/rc.exe" CACHE FILEPATH "" FORCE)
    message(STATUS "[penhu triplet] SDK bin 已注入 PATH: ${_penhu_sdk_bin}")
else()
    message(WARNING "[penhu triplet] 未找到 ${_penhu_sdk_bin}/rc.exe，"
                    "构建可能因缺少资源编译器而失败")
endif()

if(EXISTS "${_penhu_sdk_bin}/mt.exe")
    set(CMAKE_MT "${_penhu_sdk_bin}/mt.exe" CACHE FILEPATH "" FORCE)
endif()

unset(_penhu_sdk_bin)
unset(_penhu_sdk_bin_native)
