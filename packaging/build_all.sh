#!/usr/bin/env bash
# =============================================================================
#  build_all.sh —— 一次构建 + 出四种包
#
#  用法：bash packaging/build_all.sh [版本号]
#
#  注意：PKGBUILD 需要 makepkg（会拒绝 root 运行，也会在构建前装 makedepends）；
#  .deb 需要 dpkg-deb；.rpm 需要 rpmbuild；AppImage 需要能下载 linuxdeploy。
#  缺哪个工具就跳过哪种包，不会中断其它 —— 这样在 Arch 上也能只出 Arch 包。
# =============================================================================
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
cd "$root"

VERSION="${1:-0.1.0}"

echo "===================================================================="
echo " 1/6  构建"
echo "===================================================================="
bash _build_linux.sh full

echo
echo "===================================================================="
echo " 2/6  源码 tarball"
echo "===================================================================="
bash packaging/make_tarball.sh "$VERSION"

mkdir -p dist

echo
echo "===================================================================="
echo " 3/6  Arch 包（PKGBUILD）"
echo "===================================================================="
if command -v makepkg >/dev/null 2>&1; then
    cp "packaging/penhu-ledger-${VERSION}.tar.gz" packaging/arch/
    ( cd packaging/arch && makepkg -f --nodeps ) || echo ">> Arch 包失败（比如 root 运行），跳过"
    cp packaging/arch/*.pkg.tar.* dist/ 2>/dev/null || true
    rm -f packaging/arch/penhu-ledger-${VERSION}.tar.gz
else
    echo "跳过：没有 makepkg"
fi

echo
echo "===================================================================="
echo " 4/6  Debian 包（.deb）"
echo "===================================================================="
if command -v dpkg-deb >/dev/null 2>&1; then
    bash packaging/deb/build_deb.sh "$VERSION" || echo ">> deb 打包失败，跳过"
else
    echo "跳过：没有 dpkg-deb"
fi

echo
echo "===================================================================="
echo " 5/6  RPM 包"
echo "===================================================================="
if command -v rpmbuild >/dev/null 2>&1; then
    bash packaging/rpm/build_rpm.sh "$VERSION" || echo ">> rpm 打包失败，跳过"
else
    echo "跳过：没有 rpmbuild"
fi

echo
echo "===================================================================="
echo " 6/6  AppImage"
echo "===================================================================="
if command -v curl >/dev/null 2>&1 || command -v wget >/dev/null 2>&1; then
    bash packaging/appimage/build_appimage.sh "$VERSION" || echo ">> AppImage 失败，跳过"
else
    echo "跳过：没有 curl/wget"
fi

echo
echo "===================================================================="
echo " 产物"
echo "===================================================================="
ls -1sh dist/ 2>/dev/null || echo "(dist/ 为空)"
