#!/usr/bin/env bash
# =============================================================================
#  make_tarball.sh —— 打源码 tarball（PKGBUILD / rpmbuild 的 Source0）
#
#  用法：bash packaging/make_tarball.sh [版本号]
#  产物：packaging/penhu-ledger-<版本>.tar.gz
#
#  排除掉构建产物与本地数据：build* / dist / *.log / 测试用的临时目录。
#  一旦把 build/ 打进去，包能到几百 MB，而且 makepkg 会把旧的目标文件当源码用。
# =============================================================================
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
VERSION="${1:-0.1.0}"
NAME="penhu-ledger-${VERSION}"

cd "$root"
rm -f "packaging/${NAME}.tar.gz"

tar czf "packaging/${NAME}.tar.gz" \
    --transform "s,^\./,${NAME}/," \
    --exclude-vcs \
    --exclude='./build' --exclude='./build-*' \
    --exclude='./dist' --exclude='./.workbuddy' \
    --exclude='*.log' --exclude='*.o' --exclude='*.obj' \
    --exclude='./packaging/*.tar.gz' \
    --exclude='./_scan_test' --exclude='./_e2e_data*' \
    --exclude='./vcpkg_installed' --exclude='./_vsdev_env.txt' \
    --exclude='./_native_shot_data' \
    .

echo "==> packaging/${NAME}.tar.gz  ($(du -h "packaging/${NAME}.tar.gz" | cut -f1))"
# head 提前退出会给 tar 发 SIGPIPE：脚本开着 pipefail，不加 || true 会把自己弄崩（141）
tar tzf "packaging/${NAME}.tar.gz" | head -6 || true
