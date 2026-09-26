# =============================================================================
#  penhu-ledger.spec —— Fedora / openSUSE / RHEL 系
#
#  构建：bash packaging/rpm/build_rpm.sh
#
#  Requires 故意留空：rpmbuild 的 AutoReqProv 会扫描 ELF 的 NEEDED 字段
#  自动生成 so 依赖（libsqlcipher.so.0(libsqlcipher.so.0)(64bit) 之类），
#  比手写准确得多 —— 手写的那份在发行版升级后很容易过期。
# =============================================================================
Name:           penhu-ledger
Version:        0.1.0
Release:        1%{?dist}
Summary:        Local-first expense tracker with full-database encryption
Summary(zh_CN): 本地优先的记账与花销统计：整库加密，可选 AI 报告
License:        Proprietary
URL:            https://github.com/USER/penhu-ledger
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  cmake
BuildRequires:  ninja-build
BuildRequires:  gcc-c++
BuildRequires:  pkgconfig
BuildRequires:  wayland-protocols-devel
BuildRequires:  sqlcipher-devel
BuildRequires:  libsodium-devel
BuildRequires:  openssl-devel
BuildRequires:  cpp-httplib-devel
BuildRequires:  wayland-devel
BuildRequires:  libxkbcommon-devel
BuildRequires:  cairo-devel
BuildRequires:  pango-devel
BuildRequires:  fontconfig-devel
BuildRequires:  freetype-devel
BuildRequires:  libpng-devel
BuildRequires:  libjpeg-turbo-devel

Recommends:     google-noto-sans-cjk-fonts

%description
PenHu Ledger records daily expenses, produces statistics and (optionally)
lets a multimodal or text LLM summarise a period. The whole ledger database
is encrypted with SQLCipher; the per-user key is derived with Argon2id and
lives only in memory.

Native Wayland UI (Cairo + Pango) - no Electron, no WebView, no bundled
browser runtime.

%description -l zh_CN
本地优先的记账应用：整库加密（SQLCipher + Argon2id），可选接入大模型生成
阶段性报告。原生 Wayland 界面（Cairo + Pango），不使用 WebView。

%prep
%setup -q

%build
# 用裸 cmake 而不是 %cmake 宏：那套宏是 Fedora 发行版自带的，
# 别的发行版（比如在 Arch 上交叉出包）没有，会展开成怪东西。
cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=%{_prefix} \
    -DPENHU_BUILD_NATIVE=ON \
    -DPENHU_BUILD_CLI=ON \
    -DPENHU_BUILD_TESTS=ON \
    -DPENHU_BUILD_SERVER=ON
cmake --build build -j

%check
# 纯逻辑单测（金额 / 日期 / 统计），零三方依赖
./build/bin/penhu-tests-portable

%install
DESTDIR=%{buildroot} cmake --install build
%{__install} -Dpm 644 README.md %{buildroot}%{_docdir}/%{name}/README.md
%{__install} -Dpm 644 ARCHITECTURE.md %{buildroot}%{_docdir}/%{name}/ARCHITECTURE.md

%files
%{_bindir}/penhu-native
%{_bindir}/penhu-cli
%{_datadir}/applications/penhu-ledger.desktop
%{_datadir}/icons/hicolor/*/apps/penhu-ledger.png
%{_docdir}/%{name}/README.md
%{_docdir}/%{name}/ARCHITECTURE.md

%changelog
* Tue Sep 22 2026 喷壶 <penhu@localhost> - 0.1.0-1
- 首个 Linux 版本：Wayland + Cairo 原生界面
