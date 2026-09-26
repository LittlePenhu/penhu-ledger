#!/usr/bin/env bash
# =============================================================================
#  _build_linux.sh —— Linux 构建入口
#
#  用法：
#    bash _build_linux.sh core        # 【默认】core + server + CLI + 单测（不含界面，最快）
#    bash _build_linux.sh portable    # 只构建零依赖的纯逻辑部分（几秒，零三方依赖）
#    bash _build_linux.sh test        # core 的构建 + 全部测试 + CLI 自检
#    bash _build_linux.sh native      # 连界面一起编（Cairo/Pango/Wayland，产出 penhu-native）
#
#  和 Windows 的 _build_full.sh 是同一套 CMake，只是目标平台不同。
#
#  为什么默认是 core 而不是 native：core 是「改一行逻辑几秒就能验证」的那条线，
#  界面编译要链接 Cairo/Pango/Wayland 一整套，慢一个数量级。
#  日常改业务逻辑用 core，改界面用 native。
#
#  构建目录全部用 build-linux-* 前缀：源码目录同时被 Windows 构建用着，
#  而 build/ 与 build-portable/ 里已经有 Windows 生成的 CMakeCache.txt ——
#  那里面记的是 C:/... 路径，Linux 侧一进就报
#  「cache 的目录与当前目录不同 / 源码路径不匹配」。
# =============================================================================
set -euo pipefail
cd "$(dirname "$0")"

MODE="${1:-core}"
BUILD_DIR="build-linux-core"
PORTABLE_DIR="build-linux-portable"
NATIVE_DIR="build-linux-native"

JOBS="$(nproc 2>/dev/null || echo 4)"

# 参数：是否构建界面
configure_core() {
    local with_ui="$1"      # ON / OFF
    local dir="$2"
    shift 2
    local extra=("$@")
    # CLI 的 serve 命令要起进程内 HTTP 服务，所以 server 目标**必须**构建：
    # cli 链接 penhu::server，目标不存在的话配置阶段就直接失败
    # （报 "links to penhu::server but the target was not found"）。
    cmake -S . -B "$dir" -G Ninja \
        -DCMAKE_BUILD_TYPE="${BUILD_TYPE:-RelWithDebInfo}" \
        -DCMAKE_INSTALL_PREFIX=/usr \
        -DPENHU_BUILD_NATIVE="$with_ui" \
        -DPENHU_BUILD_CLI=ON \
        -DPENHU_BUILD_TESTS=ON \
        -DPENHU_BUILD_SERVER=ON \
        "${extra[@]}"
}

run_portable_tests() { "./$BUILD_DIR/bin/penhu-tests-portable"; }
run_full_tests() {
    "./$BUILD_DIR/bin/penhu-tests"
    echo
    echo ">> CLI 自检（真实进程 + 真实加密库 + 真实 SQLCipher）"
    "./$BUILD_DIR/bin/penhu-cli" selftest --fast-kdf
}

case "$MODE" in
    portable)
        # 零三方依赖：只编纯逻辑库 + portable 测试
        cmake -S . -B "$PORTABLE_DIR" -G Ninja \
            -DCMAKE_BUILD_TYPE=Debug \
            -DPENHU_WITH_NATIVE_DEPS=OFF \
            -DPENHU_BUILD_TESTS=ON
        cmake --build "$PORTABLE_DIR" -j "$JOBS"
        echo
        echo ">> 跑纯逻辑测试"
        "./$PORTABLE_DIR/bin/penhu-tests-portable"
        ;;
    core)
        configure_core OFF "$BUILD_DIR"
        cmake --build "$BUILD_DIR" -j "$JOBS"
        echo
        echo "构建完成：$BUILD_DIR/bin/{penhu-cli, penhu-tests}"
        ;;
    test)
        configure_core OFF "$BUILD_DIR"
        cmake --build "$BUILD_DIR" -j "$JOBS"
        echo
        echo ">> 纯逻辑单测（50 用例 / 1852 断言）"
        run_portable_tests
        echo
        echo ">> 完整单测（含加密、存储、多账号隔离、篡改检测：75 用例 / 4226 断言）"
        run_full_tests
        ;;
    native)
        configure_core ON "$NATIVE_DIR"
        cmake --build "$NATIVE_DIR" -j "$JOBS"
        echo
        echo "构建完成：$NATIVE_DIR/bin/penhu-native"
        ;;
    full)
        # 打包前的全量构建：core（cli + tests）+ native（界面）
        configure_core OFF "$BUILD_DIR"
        cmake --build "$BUILD_DIR" -j "$JOBS"
        configure_core ON "$NATIVE_DIR"
        cmake --build "$NATIVE_DIR" -j "$JOBS"
        echo
        echo "构建完成：$BUILD_DIR/bin/{penhu-cli,penhu-tests} + $NATIVE_DIR/bin/penhu-native"
        ;;
    *)
        echo "未知模式：$MODE（可用：core / portable / test / native / full）" >&2
        exit 2
        ;;
esac
