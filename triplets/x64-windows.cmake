# =============================================================================
#  penhu-ledger/triplets/x64-windows.cmake
#  宿主 triplet 的覆盖版（vcpkg 构建期工具用，例如 pkgconf / meson）。
#
#  为什么必须覆盖它：vcpkg 装依赖时会同时用两个 triplet——
#    · 目标 triplet：x64-windows-static（产出我们要链接的静态库）
#    · 宿主 triplet：x64-windows（产出构建期工具）
#  只覆盖 static 那个的话，宿主侧仍然拿不到 Windows SDK 的 bin，
#  pkgconf 就会以
#      RC Pass 1: command "rc /fo ..." failed: no such file or directory
#  失败。这个报错完全看不出是「另一个 triplet 没覆盖」。
#
#  具体修法见 penhu-sdk-inject.cmake。
# =============================================================================

set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)

include("${CMAKE_CURRENT_LIST_DIR}/penhu-sdk-inject.cmake")
