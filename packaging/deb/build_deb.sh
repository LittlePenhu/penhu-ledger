#!/usr/bin/env bash
# =============================================================================
#  build_deb.sh —— 造 .deb（Debian / Ubuntu / 及其派生版）
#
#  用法：bash packaging/deb/build_deb.sh [版本号]
#
#  不依赖 debhelper：直接 cmake --install 到一棵假根，写 DEBIAN/control，
#  再 dpkg-deb 打包。这样在 Arch 上也能给 Debian 造包（只要装了 dpkg）。
#
#  依赖字段：优先用 dpkg-shlibdeps 从 ELF 里自动算（最准）；
#  算不出来时退回到手写清单（下面 DEPENDS_FALLBACK），
#  手写那份是按 Debian 12 / Ubuntu 24.04 写的，别的版本要复核。
# =============================================================================
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
cd "$root"

VERSION="${1:-0.1.0}"
BUILD_DIR="${BUILD_DIR:-build-linux-native}"
ARCH="$(dpkg --print-architecture 2>/dev/null || echo amd64)"
OUT="$(pwd)/dist"
# 暂存目录默认放系统临时目录：Windows 盘（DrvFs）不保留权限位，
# dpkg-deb 会因 control 目录是 777 而拒收。
STAGE="${STAGE:-${TMPDIR:-/tmp}/penhu-deb-stage}"

DEPENDS_FALLBACK="libsqlcipher0 | libsqlcipher1, libsodium23 | libsodium26, libssl3, libwayland-client0, libxkbcommon0, libcairo2, libpango-1.0-0, libfontconfig1, libfreetype6, libharfbuzz0b, libpng16-16, libjpeg62-turbo, xdg-utils"

[[ -d "$BUILD_DIR" ]] || { echo "先构建：bash _build_linux.sh" >&2; exit 1; }

rm -rf "$STAGE"
echo ">> 安装到暂存目录 $STAGE"
DESTDIR="$STAGE" cmake --install "$BUILD_DIR" --prefix /usr

# 文档
install -Dm644 README.md "$STAGE/usr/share/doc/penhu-ledger/README.md"
install -Dm644 ARCHITECTURE.md "$STAGE/usr/share/doc/penhu-ledger/ARCHITECTURE.md"
[[ -f LICENSE ]] && install -Dm644 LICENSE "$STAGE/usr/share/doc/penhu-ledger/copyright"

# ---- 依赖：能自动算就自动算 ----
deps="$DEPENDS_FALLBACK"
if command -v dpkg-shlibdeps >/dev/null 2>&1; then
    cd "$STAGE"
    if dpkg-shlibdeps -O usr/bin/penhu-native >/dev/null 2>&1; then
        deps="$(dpkg-shlibdeps -O usr/bin/penhu-native 2>/dev/null \
                | sed -n 's/^shlibs:Depends=//p')"
        echo ">> 依赖（dpkg-shlibdeps 自动生成）: $deps"
    else
        echo ">> dpkg-shlibdeps 失败，用回退清单"
    fi
    cd "$root"
fi

size_kb="$(du -sk "$STAGE" | cut -f1)"

mkdir -p "$STAGE/DEBIAN"
# 权限必须收一下：Windows 盘挂进 WSL 时全是 777，dpkg-deb 会拒收（0755/0775 才行）
chmod 755 "$STAGE" "$STAGE/DEBIAN" 2>/dev/null || true
cat > "$STAGE/DEBIAN/control" <<EOF
Package: penhu-ledger
Version: $VERSION
Section: utils
Priority: optional
Architecture: $ARCH
Maintainer: PenHu <penhu@localhost>
Installed-Size: $size_kb
Depends: $deps
Recommends: fonts-noto-cjk
Homepage: https://github.com/LittlePenhu/penhu-ledger
Description: Local-first expense tracker with full-database encryption
 PenHu Ledger records daily expenses, produces statistics and (optionally)
 lets a multimodal or text LLM summarise a period. The whole ledger database
 is encrypted with SQLCipher; the per-user key is derived with Argon2id and
 lives only in memory.
 .
 Native Wayland UI (Cairo + Pango) - no Electron, no WebView.
EOF

# 桌面文件与图标的触发器（Debian 用 triggers 通知桌面环境刷新缓存）
mkdir -p "$STAGE/DEBIAN"
cat > "$STAGE/DEBIAN/triggers" <<'EOF'
activate-noawait update-desktop-database
activate-noawait gtk-update-icon-cache
interest-noawait /usr/share/icons/hicolor
EOF

chmod 644 "$STAGE/DEBIAN/control" "$STAGE/DEBIAN/triggers" 2>/dev/null || true

mkdir -p "$OUT"
dpkg-deb --build --root-owner-group "$STAGE" "$OUT/penhu-ledger_${VERSION}_${ARCH}.deb"
echo
echo "==> $OUT/penhu-ledger_${VERSION}_${ARCH}.deb"
dpkg-deb -I "$OUT/penhu-ledger_${VERSION}_${ARCH}.deb" | sed -n '/Package:/,/Description:/p'
