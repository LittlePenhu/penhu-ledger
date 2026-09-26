# Linux 移植方案

## 0. 一句话结论

`core` 层（加密、存储、统计、LLM、报告）**基本已经跨平台**；工作量大头在
`native/src` 的界面层：4000 行渲染/窗口代码要从 Direct2D/Win32 换成 Cairo/Wayland。
页面代码（2200 行）与业务逻辑一行都不用动 —— 这是当初分层做对了的回报。

## 1. 现状盘点

| 层 | 行数 | Windows 依赖 | 移植动作 |
|---|---|---|---|
| `core/src` + `core/include` | 12.5k | 只有 `util/fs.cpp`、`util/process.cpp` | **已有 POSIX 分支**（`#else` 里是 `<unistd.h>` / `waitpid` / PATH 冒号分隔）。需要复核 + 补边界情况 |
| `native/src/pages` | 2.2k | 2 个文件（误引 platform 头） | 几乎不用改 |
| `native/src/ui`（Widget/组件/主题） | 2.5k | 0 | **完全不用改** |
| `native/src/ui/renderer` | 0.7k | 全部 | **重写为 Cairo + Pango 后端**（保留接口） |
| `native/src/ui/platform_input` | 0.4k | 全部 | 重写（Wayland seat / IME / 剪贴板） |
| `native/src/app/shell` | 2.5k | 全部 | 重写窗口与事件循环（xdg-shell + CSD） |
| `native/src/app/{app,image_util}` | 2.3k | 部分 | 文件对话框 / 回收站 / 图片解码换 Linux 实现 |
| `native/src/titlebar` | 0.4k | 少量 | 大部分可复用（自绘逻辑与平台无关） |

**已经可以直接用的**：`core/CMakeLists.txt` 的依赖声明不用改 —— Linux 侧会在根
CMakeLists 里伪造同名的 IMPORTED target（`sqlcipher::sqlcipher` /
`unofficial-sodium::sodium` / `httplib::httplib`），两平台共用一份链接声明。

## 2. Linux 后端设计

### 2.1 渲染：Cairo + Pango

`Renderer` 对外只有 27 个方法，全部可以在 Cairo 上实现：

| 现接口 | Cairo 对应 |
|---|---|
| `fill` / `fill_round` | `cairo_rectangle` / `cairo_arc` 组合 + `cairo_fill`（先算圆角路径） |
| `stroke_round` | 同上 + `cairo_stroke`（注意半宽内缩，和 D2D 版语义一致） |
| `fill_circle` | `cairo_arc` + `cairo_fill` |
| `draw_line` | `cairo_move_to` / `line_to` / `set_line_width` / `stroke` |
| `push_clip` / `pop_clip` | `cairo_save` / `cairo_rectangle` / `cairo_clip` / `cairo_restore` |
| `text` / `text_line` / `measure` | **Pango**（`pango_cairo_*`）—— 中文、字体回退、字重都由它管 |
| `draw_image` | `cairo_set_source_surface` + `cairo_paint`（cover/contain 用 `cairo_scale` 算） |
| `load_image` / `load_image_mem` | libpng / libjpeg 解码 → `cairo_image_surface_create_for_data` |
| `save_png` / `encode_png` / `save_screen_png` | `cairo_surface_write_to_png`（离屏截图同样可行） |
| `attach_hwnd` / `present` / `resize` | Wayland：`wl_shm` 缓冲 + `wl_surface_attach/commit`（subsurface 不需要） |
| `begin_frame` / `end_frame` | `cairo_create(surface)` / `cairo_destroy` |
| `scale` | 逻辑 dp → 物理像素，Wayland 的 `wl_surface.set_buffer_scale` |

**接口必须先做一处抽象**：`load_image(..., ComPtr<IWICBitmapSource>& out)` 里
`IWICBitmapSource` 是 Windows 专有类型。改成平台无关的
`struct Image; using ImageRef = std::shared_ptr<Image>;`，
`draw_image(ImageRef, ...)`。改动点只有 `renderer.hpp` 和用它的 `ImagePreview`。

字体链（HarmonyOS Sans SC → Noto Sans SC → …）在 Linux 上由 **fontconfig** 解析，
和 Windows 版问 DirectWrite 字体集合是同一套逻辑，只是换个 API（`FcFontList` / `FcMatch`）。

### 2.2 窗口与输入：Wayland（xdg-shell）

| 能力 | Windows 现方案 | Wayland 方案 |
|---|---|---|
| 窗口创建 | `CreateWindowExW` | `wl_compositor.create_surface` + `xdg_wm_base.get_xdg_surface/get_toplevel` |
| 标题栏 | 自绘顶栏 + `WM_NCCALCSIZE` 吃掉系统栏 | **客户端装饰（CSD）**——本来就是这个形态，直接画 |
| 拖动窗口 | `WM_NCHITTEST` 返回 `HTCAPTION` | `xdg_toplevel.move(seat, serial)`（协议接管，等价于 HTCAPTION） |
| 拖动边缘缩放 | `WM_NCHITTEST` 边框热区 | `xdg_toplevel.resize(seat, serial, edges)`（自己判定边缘区域） |
| 最大化 / 还原 | 手动 `SetWindowPos` 到 rcWork | `xdg_toplevel.set_maximized` / `unset_maximized` + 等 configure |
| 全屏 | TOPMOST + rcMonitor | `xdg_toplevel.set_fullscreen` / `unset_fullscreen` |
| 窗口状态同步 | `IsZoomed()` | configure 事件里的 `states`（maximized / fullscreen / activated） |
| 绘制提交 | `SetDIBitsToDevice` 直写 | `wl_shm` 池 + `wl_buffer` + `attach` + `commit`（**天然无撕裂**，不需要 `WS_EX_COMPOSITED`） |
| 动画节拍 | `WM_TIMER` 16ms | `wl_surface.frame` 回调（跟随合成器刷新率，比 16ms 定时器更准） |
| 键盘 | `WM_KEYDOWN` + `ToUnicode` | `wl_keyboard` + **xkbcommon**（keymap → keysym → 文本） |
| 鼠标 / 滚轮 | `WM_MOUSEMOVE` / `WM_MOUSEWHEEL` | `wl_pointer`（`axis` 事件，含高精度滚动 `axis_value120`） |
| 输入法（中文） | `WM_IME_COMPOSITION` + `Imm*` | `wp_text_input_manager_v3`（fcitx5 在 Wayland 下走这个） |
| 剪贴板 | `OpenClipboard` | `wl_data_device` + `wl_data_offer` |
| 屏幕缩放 | `GetDpiForWindow` | `wl_output.scale`（整数缩放）+ `wp_fractional_scale`（可选，分数缩放） |

**Wayland 与 Windows 的关键差异**（会影响现有逻辑）：

1. **几何由合成器决定**，不能自己算。Windows 版「手动最大化到 rcWork」在 Wayland 下
   要改成「请求 set_maximized，等 configure 给的尺寸」—— `manual_max_` 那套
   自维护状态仍然需要（用来同步图标和按钮语义），但尺寸不再由我们决定。
2. **窗口不能自己移动**。拖动必须走 `xdg_toplevel.move`，且需要 `serial`（最近一次
   输入事件的序号）—— 拖动的起点必须来自真实的 pointer 事件。
3. **没有全局坐标系**，`WindowFromPoint` 这类 API 不存在 —— 诊断模式里
   「任务栏可见性」那条断言在 Wayland 下无法实现（安全模型不允许探测别的窗口）。
   要改用「窗口自身状态 + 截图」来验证。
4. **提交是原子的**（bind buffer 后 commit），`WS_EX_COMPOSITED` 那套问题不存在。

### 2.3 平台特有能力对照

| 功能 | Windows | Linux |
|---|---|---|
| 选图片（多选） | `GetOpenFileNameW` + `OFN_ALLOWMULTISELECT` | **自绘选择器**（用现有 Widget 树，零新依赖）；可选回退 `zenity --file-selection` |
| 选目录 | `IFileDialog` + `FOS_PICKFOLDERS` | 同上 |
| 图片默认目录 | `SHGetKnownFolderPath(FOLDERID_Pictures)` | `XDG_PICTURES_DIR`（读 `~/.config/user-dirs.dirs`，回退 `$HOME/Pictures`） |
| 移动到回收站 | `SHFileOperationW` + `FOF_ALLOWUNDO` | **XDG Trash 规范**：移到 `$XDG_DATA_HOME/Trash/files/` + 写 `.trashinfo`（纯文件操作，几十行） |
| 通知 | 无（用应用内提示条） | 同样用应用内提示条（可选 `org.freedesktop.Notifications` D-Bus） |
| 图片解码 | WIC | libpng / libjpeg-turbo（或 vendored stb_image，更省事） |
| 屏幕截图（诊断） | `GetDC(NULL)` + `BitBlt` | 离屏截图照常；**抓全屏需要 portal 或 wlr-screencopy** —— 诊断改用「离屏渲染帧」做视觉核对 |
| 应用图标 | exe 内 ICO 资源 | `.desktop` 的 `Icon=` + hicolor 多尺寸 PNG |

## 3. 分批计划（每批都能独立验证）

| 批次 | 内容 | 验证方式 |
|---|---|---|
| **A** | 构建系统平台化 + 依赖脚本 + 打包全套 | ✅ 已完成（Windows 构建已验证未破坏） |
| **B** | `core` 层在 Linux 编译 + `penhu-cli selftest` 通过 | `_build_linux.sh portable` → `test`（真实进程 + 真实加密库） |
| **C** | Cairo 渲染后端（`renderer` 的 Linux 实现）+ 接口去 Windows 类型 | 离屏渲染出 PNG，和 Windows 版的离屏截图逐张比对 |
| **D** | Wayland 窗口与事件循环（xdg-shell + CSD + 输入） | 窗口能开、能画、能响应鼠标键盘 |
| **E** | 平台特有能力（文件选择器 / 回收站 / 图片解码 / 输入法） | 逐项手测 |
| **F** | 打包实测（makepkg / dpkg-deb / rpmbuild / AppImage） | 装出来的包能跑 |

## 4. 验证环境

**本机（Windows）没有 WSL/Docker，Linux 侧无法编译。** 计划：

1. 启用 WSL2，导入 Arch
2. WSLg 自带 Wayland 合成器（weston）→ **Wayland 应用可以直接跑起来并显示窗口**，
   我能在 Windows 桌面看到界面并用诊断模式截图
3. 若 WSLg 的 weston 缺某些协议（如 text-input-v3），输入法要换回 X11 路径或标记为待测

## 5. 已知风险（不掩饰）

- **WSLg 与真实 Arch 的差异**：WSLg 的 weston 不是完整的桌面环境，
  窗口装饰、输入法、剪贴板行为都可能和 KDE/GNOME 下不同。WSL 里通过 ≠ 你的 Arch 上没问题。
- **缩放行为**：Wayland 的分数缩放（`wp_fractional_scale`）各家合成器支持不一，
  150% 这样的比例在 Wayland 上要么走整数缩放（2x，界面偏大）要么走 fractional（要额外协议）。
- **自绘文件选择器**是额外工作量（但换来零依赖 + 视觉一致）。
- **打包脚本未实测**：`PKGBUILD` / `spec` / `build_deb.sh` 都只做了语法检查，
  真正跑起来多半还要修几处（路径、包名、依赖字段）。

## 6. 打包产物

见 `packaging/README.md`。四种：Arch（PKGBUILD）、Debian（.deb）、
Fedora/openSUSE（.rpm）、通用（AppImage）。
