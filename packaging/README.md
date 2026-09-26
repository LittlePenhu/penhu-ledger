# packaging —— Linux 打包

四种包一趟出：`bash packaging/build_all.sh`（缺哪个工具就跳过哪种包）。

| 目标 | 脚本 | 产物 | 需要 |
|---|---|---|---|
| Arch | `arch/PKGBUILD` | `penhu-ledger-<版本>-1-x86_64.pkg.tar.zst` | `makepkg`（base-devel） |
| Debian/Ubuntu | `deb/build_deb.sh` | `penhu-ledger_<版本>_amd64.deb` | `dpkg-deb`（dpkg） |
| Fedora/openSUSE | `rpm/build_rpm.sh` | `penhu-ledger-<版本>-1.<dist>.rpm` | `rpmbuild`（rpm-tools） |
| 通用 | `appimage/build_appimage.sh` | `penhu-ledger-<版本>-x86_64.AppImage` | `curl` + FUSE（或能解包运行） |

产物统一落在项目根的 `dist/`。

## 单独构建某个包

```bash
bash _deps_arch.sh                 # Arch：装齐依赖（--check 只检查）
bash _build_linux.sh               # 构建（portable / full / test 三种模式）
bash packaging/make_tarball.sh     # 打源码 tarball（PKGBUILD 与 rpm 都要）
bash packaging/arch/PKGBUILD 的目录里：makepkg -si
```

```bash
bash packaging/deb/build_deb.sh 0.1.0
bash packaging/rpm/build_rpm.sh 0.1.0
bash packaging/appimage/build_appimage.sh 0.1.0
```

## 几个必须知道的点

**依赖字段怎么来的**

- Arch：`PKGBUILD` 的 `depends` 是手写的，和 `_deps_arch.sh` 的 `RUNTIME_DEPS`
  是同一份列表 —— 改一处要改两处。
- RPM：`Requires` **故意留空**，交给 rpmbuild 的 AutoReqProv 扫描 ELF 的
  `NEEDED` 自动生成。手写在发行版升级后极易过期。
- Debian：优先用 `dpkg-shlibdeps` 自动算，算不出来（缺 dpkg-dev 环境）时
  退回脚本里的手写清单 —— 那份是按 Debian 12 / Ubuntu 24.04 写的，换版本要复核。

**许可证还没定**

项目里没有 LICENSE 文件，`PKGBUILD` 里写的是 `license=('custom')`。
定下来之后（MIT / GPL-3.0-or-later / …）需要改三处：
`PKGBUILD` 的 `license=`、`.SRCINFO` 的 `license =`、`rpm/*.spec` 的 `License:`，
并把 LICENSE 文件放进项目根（`package()` 会自动装它）。

**图标在 Linux 上和 Windows 完全不同**

Windows 靠 exe 资源里的 ICO（`wc.hIcon`），运行时就能定。
Linux 没有这个 API：任务栏/启动器图标来自 `.desktop` 的 `Icon=` 字段，
按 hicolor 主题规范在 `/usr/share/icons/hicolor/<尺寸>x<尺寸>/apps/` 下按尺寸找 PNG。
`packaging/icons/` 下那 7 档就是从 `native/resources/app_logo.png` 缩出来的
（`_make_hicolor.py`），少一档，桌面环境在那个尺寸下就显示通用图标。

**AppImage 的库策略**

glibc / libGL / libX11 这类核心库必须排除（要用目标机上的），
cairo / pango / sqlcipher / libsodium 这些要打进去。
linuxdeploy 默认按这套规则收集，所以选它而不是手工 tar。
