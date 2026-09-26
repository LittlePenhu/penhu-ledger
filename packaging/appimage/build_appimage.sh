#!/usr/bin/env bash
# =============================================================================
#  build_appimage.sh —— 造 AppImage（单文件、免安装、任何发行版可跑）
#
#  用法：bash packaging/appimage/build_appimage.sh [版本号]
#
#  AppImage 的坑主要在「带哪些库」：glibc / libGL / libX11 这类核心库要排除
#  （必须在目标机上用系统的），其余（cairo / pango / sqlcipher / libsodium …）
#  都要打进包里，否则换台机器就缺库。
#  linuxdeploy 默认按「非核心库」规则自动收集 —— 这也是选它的原因。
#
#  需要下载两个工具（首次运行会自动下）：
#    linuxdeploy-x86_64.AppImage  +  linuxdeploy-plugin-gtk 不需要（我们不用 GTK）
# =============================================================================
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
cd "$root"

VERSION="${1:-0.1.0}"
BUILD_DIR="${BUILD_DIR:-build-linux-native}"
OUT="$(pwd)/dist"
APPDIR="$(pwd)/build-appimage/PenHuLedger.AppImage.d"
TOOLS="$(pwd)/build-appimage/tools"

[[ -d "$BUILD_DIR" ]] || { echo "先构建：bash _build_linux.sh" >&2; exit 1; }

mkdir -p "$TOOLS" "$OUT"

# ---- 取工具（有就跳过）----
fetch() {
    local url="$1" out="$2"
    [[ -x "$out" ]] && return 0
    echo ">> 下载 $(basename "$out")"
    if command -v curl >/dev/null 2>&1; then curl -fL -o "$out" "$url"; else wget -O "$out" "$url"; fi
    chmod +x "$out"
}
LD="$TOOLS/linuxdeploy-x86_64.AppImage"
fetch "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage" "$LD"

# ---- 组装 AppDir ----
rm -rf "$APPDIR"
DESTDIR="$APPDIR" cmake --install "$BUILD_DIR" --prefix /usr
install -Dm644 README.md "$APPDIR/usr/share/doc/penhu-ledger/README.md"

# AppImage 规范要求顶层有 .desktop 和图标
cp "$APPDIR/usr/share/applications/penhu-ledger.desktop" "$APPDIR/penhu-ledger.desktop"
cp "$APPDIR/usr/share/icons/hicolor/256x256/apps/penhu-ledger.png" "$APPDIR/penhu-ledger.png"

# ---- 跑 linuxdeploy ----
# Arch 上如果没有 FUSE，用 --appimage-extract-and-run 解包执行
LD_RUN=("$LD")
if ! "$LD" --version >/dev/null 2>&1; then
    LD_RUN=("$LD" --appimage-extract-and-run)
fi

cd "$(pwd)/build-appimage"
"${LD_RUN[@]}" \
    --appdir "$APPDIR" \
    --desktop-file "$APPDIR/penhu-ledger.desktop" \
    --icon-file "$APPDIR/penhu-ledger.png" \
    --output appimage

cd "$root"
mv build-appimage/*.AppImage "$OUT/penhu-ledger-${VERSION}-x86_64.AppImage" 2>/dev/null || true
echo
ls -1sh "$OUT"/*.AppImage
echo
echo "AppImage 不装进系统，直接 chmod +x 后运行即可。"
