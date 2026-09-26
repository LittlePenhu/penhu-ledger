# Linux 构建验证报告

> 时间：2026-09-22 23:0x
> 环境：Windows 11 25H2 (26200.9457) 上的 WSL2 + Arch Linux
> 复现命令全部列在下面，数字都是实际输出。

---

## 1. 环境（实测版本）

| 组件 | 版本 | 来源 |
|---|---|---|
| WSL | 2.7.14.0，内核 6.18.33.2-2，**WSLg 1.0.73.2** | `wsl.2.7.14.0.x64.msi` |
| 发行版 | Arch Linux rolling（kernel `6.18.33.2-microsoft-standard-WSL2`） | Arch 官方 WSL 镜像 |
| 编译器 | GCC 16.2.1 | pacman |
| 构建 | CMake 4.4.3 / Ninja 1.13.2 | pacman |
| 图形 | cairo 1.18.4 / pango 1.58.2 / wayland-client 1.26.0 | pacman |
| 加密存储 | SQLCipher 3.53.4 / libsodium 1.0.22 / OpenSSL 3.6.4 | pacman |
| 资源 | 22 核 / 15.7 GB | — |
| Wayland socket | `/run/user/0/wayland-0` → `/mnt/wslg/runtime-dir/wayland-0` | WSLg |

界面走 `wl_shm` + Cairo 软件绘制，**不依赖 mesa/libGL** —— WSLg 的 GPU 透传时好时坏，绕开更稳。

---

## 2. 构建与测试结果

### 2.1 零依赖部分（几秒）

```bash
bash _build_linux.sh portable
```

```
用例: 50 通过 / 0 失败（共 50）
断言: 1852 条，失败 0 条
```

### 2.2 完整 core + CLI

```bash
bash _build_linux.sh test
```

```
用例: 50 通过 / 0 失败（共 50）       ← portable 测试
断言: 1852 条，失败 0 条

用例: 75 通过 / 0 失败（共 75）       ← 含加密、存储、多账号隔离、篡改检测
断言: 4226 条，失败 0 条

=== 汇总 ===
  通过 33 项，失败 0 项               ← CLI 自检（真实进程 + 真实 SQLCipher）
```

链接出来的目标：`libpenhu-core-portable.a`、`libpenhu-core.a`、`libpenhu-server.a`、
`penhu-tests-portable`、`penhu-tests`、`penhu-cli`。

**零编译警告**（`-Wall -Wextra -Wpedantic`）。

### 2.3 CMake 依赖探测（Linux 分支首次运行）

```
-- Found OpenSSL: /usr/lib/libcrypto.so (found version "3.6.4")
--   SQLCipher   : /usr/lib/libsqlcipher.so
--   Found libsodium, version 1.0.22
--   httplib     : /usr/include
--   TLS         : 已启用（https 可用）
```

Linux 分支**伪造了与 vcpkg 同名的 target**（`sqlcipher::sqlcipher` /
`unofficial-sodium::sodium` / `httplib::httplib`），所以 `core/` 与 `cli/` 的
CMakeLists **一行都没改**，两个平台共用同一份链接声明。

---

## 3. 移植过程中发现的、Windows 上永远看不到的 bug

### 3.1 宏参数名撞上成员名（`tests/tiny_test.hpp`）

```cpp
#define TT_IS_ERR_CODE(expr, code) \
    ... _r.error().code != (code) ...
```

宏体会把参数在**所有**标识符位置替换掉，包括 `.code` 里那个**成员名** ——
展开成 `_r.error().ErrorCode::InvalidArgument`。GCC 直接报
「`ErrorCode::InvalidArgument` 不是 `const penhu::Error` 的成员」。

MSVC 的传统预处理器在 `.` 之后不做替换（不符合标准），所以这个 bug 在 Windows 上一直藏着。

**规则**：宏参数名要避开宏体里出现的任何成员名/变量名。

### 3.2 `char c == 0xB7` 恒为假（`core/src/domain/money.cpp`）

「金额里的中点（U+00B7，输入法手滑打出的 `·`）也认作小数点」这条**从来没生效过**：
`char` 是带符号的（MSVC 和 GCC 都是），`0xB7` = 183 超出正数范围，编译器甚至直接给出
`-Wtype-limits` 警告。

而且就算换成无符号也不够：UTF-8 里 U+00B7 是**两个字节** `C2 B7`，
只匹配 `B7` 会让前一个字节 `C2` 掉进「非法字符」分支。改成下标循环 + 双字节序列判断。

### 3.3 清理的警告（不影响行为）

| 位置 | 改动 |
|---|---|
| `core/src/domain/date.cpp` | `snprintf` 缓冲区 32 → 80（GCC 按 `tm_year` 理论上界算，报截断） |
| `core/src/util/log.cpp` | 同上，40 → 80 |
| `core/src/domain/user.cpp` | 删死代码 `is_ascii_alpha` |
| `core/src/report/report_service.cpp` | 聚合初始化补全 `image_data_url` |
| `core/src/deploy/local_model.cpp` | `err` 参数标 `[[maybe_unused]]`（TLS 开启时那个分支整块被编掉） |
| `cli/src/main.cpp` | 三个 Windows 控制台 API 包进 `#ifdef _WIN32` |

---

## 4. 回归：两个平台都是绿的

| 平台 | 构建 | 单测 | CLI 自检 |
|---|---|---|---|
| Windows (MSVC) | 0 错误 | 1852 + 4226 断言 0 失败 | 33 项 0 失败 |
| Linux (GCC 16) | 0 错误 0 警告 | 1852 + 4226 断言 0 失败 | 33 项 0 失败 |

---

## 5. 界面层移植（Cairo + Pango + Wayland）

### 5.1 新写的文件

| 文件 | 作用 |
|---|---|
| `native/src/ui/renderer_cairo.cpp` | Renderer 的 27 个方法：Cairo 图元 + Pango 排版 + 自写图片解码 + PNG 导出 |
| `native/src/app/shell_linux.cpp` | Wayland 窗口（xdg-shell）+ wl_shm 上传 + xkbcommon 键盘 + 离屏截图模式 |
| `native/src/app/image_util_linux.cpp` | 替换 WIC 的图片解码/缩放/JPEG 编码（libpng + libjpeg-turbo） |
| `native/src/app/platform_fs_linux.cpp` | XDG 回收站、XDG 图片目录、zenity/kdialog 文件对话框 |
| `native/src/ui/platform_input_linux.cpp` | 剪贴板（wl-clipboard）与修饰键状态 |
| `native/src/main_linux.cpp` | 入口（setlocale、set_terminate、参数解析） |
| `native/src/ui/bitmap.hpp` | 平台无关位图句柄 |
| `native/src/ui/keys.hpp` | 平台无关键码（数值与 Win32 VK 一致 → Windows 侧零转换） |

### 5.2 共享层抽出来的部分（两平台共用一份，不是复制）

- `native/src/app/shell_common.cpp` —— `AppTree`（整棵界面树）、事件冒泡、
  诊断挑选器、演示数据。这些原本和 Win32 消息处理混在 `shell.cpp` 里。
- `native/src/ui/text_util.cpp` —— `sanitize_single_line`（原本误放在 Windows 实现里）。
- `native/src/app/platform_fs_win.cpp` —— 把 `app.cpp` 里 5 处 Win32 调用收拢到这里，
  于是 `app.cpp` 变成完全平台无关。

### 5.3 界面在 Linux 上渲染成功

```
bash _build_linux.sh native         # 产出 build-linux-native/bin/penhu-native（53MB）
./penhu-native --shot ~/shots --width 1600 --height 900
  → 10 张页面截图（登录/记账/统计/报告/截图/设置/折叠态/两张深色）
```

真实窗口（WSLg 的 weston 合成器）也验证通过：

```
界面字体: Noto Sans CJK SC
Wayland 已连接：compositor/shm/xdg-shell 就绪，输入设备可用
收到首个 configure：目标尺寸 1240x860（物理像素）
首帧已提交给合成器：1240x860 像素
（进程持续运行，timeout 8s 后才被外部终止）
```

### 5.4 移植中最难查的一处 bug（值得单独记）

**现象**：所有文字都不显示，而布局、色块、圆角、命中测试、甚至
`pango_layout_get_pixel_extents` 的量测值全部正常。
`cairo_status()` 也是 `no error`。看起来完全像「字体没装」——
但 `fc-list` 有 701 个字体，独立 Pango 测试也能正常出图。

**原因**：Cairo 的两条冷门语义叠加：

1. `cairo_fill()` / `cairo_stroke()` **不会清空当前路径**；
   `cairo_save()` / `cairo_restore()` **也不保存路径**。
   于是前面画过的卡片、圆角的子路径全都留在当前路径里，
   后面 `cairo_clip()` 按填充规则把它们和文本框一起算，
   得到一个空的（或错乱的）裁剪区域。
2. `cairo_new_path()` 除了清路径，**还会把当前点重置为 (0,0)**。
   我第一版修了 (1)（在 `text()` 里加 `cairo_new_path`），
   但加在了 `cairo_move_to(box.x, y)` **之后** —— 落点被清成 (0,0)，
   文字画到画布左上角又被裁剪区裁掉，依然一个字都没有。

**修法**：所有「建路径 → 填充/描边/裁剪」的入口先 `cairo_new_path()`，
而 `text()` 里必须「**先建裁剪路径、再 `cairo_move_to`**」。

两版输出**字节完全相同**（md5 一致），所以一开始误以为「改没生效」——
实际上是两个不同的原因导致同一个现象。这是排查中最费时间的地方：
中间量全对、只有「画在哪」被悄悄改掉。

### 5.5 其它踩到的跨平台差异

| 现象 | 原因 |
|---|---|
| 链接期 `undefined reference to xdg_wm_base_interface` | 顶层 `project()` 只启用了 CXX，wayland-scanner 生成的 **.c** 文件没被编译（看着像「协议没生成」，其实是「生成了没编」） |
| `jpeglib.h` 报 `size_t does not name a type` | 它用了 `size_t`/`FILE` 却不自己 include，必须放在 `<cstddef>`/`<cstdio>` **之后** |
| `pango_layout_set_height` 传负数无效 | 该函数**正负号改变单位**：正数是像素高度，负数是「行数」 |
| `key::` 找不到 | 键码表在 `penhu::native::ui::key` 里，而 shell 在 `penhu::native` 命名空间 |
| `windows.h` 漏进共享代码 | `VK_SHIFT` 等 9 个键码、以及 `convert.hpp` 的 `MultiByteToWideChar`；前者换成 `ui/keys.hpp`，后者改成纯 C++ 实现 |

### 5.6 已知的两平台差异（有意保留）

- **图片的圆角裁剪**：Windows 侧的 `draw_image` 只做了矩形裁剪
  （`push_clip(box)`），也就是 `Theme::kCornerSm` 传下去其实没生效 —— 那里是个 bug。
  Linux 侧按参数本意做了圆角，所以两边截图的图片四角会有可见差别。
  要么把 Linux 改成和 Windows 一样（保留 bug），要么修 Windows。**待定**。
- **字体**：Windows 选中的是 Noto Sans SC（本机装了），Linux 是 Noto Sans CJK SC，
  字形不同，所以两平台的截图**不可能逐像素一致**，只能人工核对排版。
- **最小化按钮**：Wayland 的 xdg-shell 没有「客户端请求最小化」，Linux 上该按钮无效
  （保留按钮是为了界面一致）。
- **全屏/最大化**：由合成器决定，状态通过 configure 事件回来；
  Windows 那套「手动最大化 + 动画」的补丁在 Linux 上完全不需要存在。

---

## 6. 还没做的（如实列出）

### 6.1 界面交互没有自动化验证

Windows 侧那套「注入真实消息 + 断言状态变化」的诊断链（20+ 项）**Linux 上还没有对应实现**。
现在的验证是「离屏截图人工核对排版 + 窗口能开起来并提交首帧」，也就是说：

- 鼠标点击、滚轮、拖动滚动条、键盘输入、输入法 —— **没有自动测过**。
  这些代码在 `ui/` 与 `shell_common.cpp` 里和 Windows 共用一份，
  理论上行为一致，但「理论上」不等于验证过。
- 接下来该做的是：把 Wayland 侧的事件处理抽出一个「注入入口」
  （直接调 `handle_pointer_button` / `handle_key` 而不经过协议），
  然后复用 Windows 那套断言。这会比 Windows 侧更容易，因为事件回调本来就是普通成员函数。

### 6.2 输入法完全没做

Wayland 下中文输入要走 `text-input-v3` 协议；`native/CMakeLists.txt` 已经把协议代码
生成好了，但 `shell_linux.cpp` **还没有绑定它**。现状：**Linux 下备注栏打不了中文**
（英文和数字可以）。这是移植里最大的一块缺口。

### 6.3 打包只过了语法检查

`packaging/` 下的 PKGBUILD / deb / rpm / AppImage 脚本从没真跑过。
现在界面能编了，下一步应该至少把 Arch 包真正 `makepkg` 一遍。

### 6.4 其它已知缺口

- `save_screen_png`（抓整屏）在 Linux 上不可用 —— Wayland 协议不允许客户端随便抓屏。
  影响：Windows 那条「最大化时任务栏可见」的断言在 Linux 上没有对应物。
  好在 Linux 也不存在那个问题（窗口尺寸由合成器给，客户端没法把矩形外扩到屏幕外）。
- HiDPI：`wl_surface.set_buffer_scale` / `wp_fractional_scale` 没处理，
  当前按 1.0 走。屏幕上如果有 2x 缩放，界面会偏小。
- 光标形状：没设 `wl_cursor`，用合成器默认箭头。文本输入框上不会变成 I 形光标。
- 惯性滚动、回弹：和 Windows 一样没做。

---

## 7. 一句总结

**Linux 版能跑、能画、能存，但还不能用键盘打中文。**
core 层（加密/存储/统计/LLM/CLI）在两个平台上都经过了完整测试；
界面层在 Linux 上渲染正确、窗口能正常显示，但交互与输入法仍待补齐。
