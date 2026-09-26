#!/usr/bin/env bash
# =============================================================================
#  build_rpm.sh —— 造 .rpm（Fedora / openSUSE / RHEL 系）
#
#  用法：bash packaging/rpm/build_rpm.sh [版本号]
#
#  rpmbuild 要求一套固定的目录结构（SOURCES / SPECS / BUILD ...），
#  这里在项目内的 build-rpm/ 下临时搭一套，产物丢到 dist/。
# =============================================================================
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
cd "$root"

VERSION="${1:-0.1.0}"
TOP="$(pwd)/build-rpm"
OUT="$(pwd)/dist"
TARBALL="penhu-ledger-${VERSION}.tar.gz"

command -v rpmbuild >/dev/null 2>&1 || {
    echo "缺 rpmbuild。Arch: sudo pacman -S rpm-tools；Fedora: sudo dnf install rpm-build" >&2
    exit 1
}

# 源码 tarball（rpmbuild 从 SOURCES 里找）
bash packaging/make_tarball.sh "$VERSION" >/dev/null

mkdir -p "$TOP"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}
cp "$(pwd)/packaging/penhu-ledger-${VERSION}.tar.gz" "$TOP/SOURCES/"
cp "$here/penhu-ledger.spec" "$TOP/SPECS/"
sed -i "s/^Version:.*/Version:        ${VERSION}/" "$TOP/SPECS/penhu-ledger.spec"

mkdir -p "$OUT"
rpmbuild -bb \
    --nodeps \
    --define "_topdir $TOP" \
    --define "_sourcedir $TOP/SOURCES" \
    --define "debug_package %{nil}" \
    --define "build_id_links none" \
    --define "_disable_source_footer 1" \
    "$TOP/SPECS/penhu-ledger.spec"

find "$TOP/RPMS" -name '*.rpm' -exec cp {} "$OUT/" \;
echo
ls -1 "$OUT"/*.rpm
