// =============================================================================
//  native/app/shell_diag.cpp
//  诊断模式：无人值守的「自动登录 → 注入真实交互 → 断言 → 出图」状态机。
//
//  从 shell.cpp 的 WM_TIMER 里原样搬出来（不改逻辑，唯一变化：case 体里的
//  return 0 变成函数的 return）。这 1100 多行只在 --diag-* 参数下运行，
//  与正常路径零共用 —— 留在 handle() 里会把消息分发淹掉。
//
//  入口两个：configure_diag() 置起状态，tick_diag() 由 WM_TIMER 逐帧推进。
//  Windows 平台实现（交互注入走 PostMessageW 真实消息链路）；
//  Linux 侧目前只有参数登记，见 shell_linux.cpp 的 configure_diag。
// =============================================================================

#include "app/shell.hpp"

#include <dwmapi.h>
#include <imm.h>
#include <windowsx.h>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <typeinfo>

#include "penhu/util/log.hpp"
#include "app/shell_common.hpp"
#include "ui/convert.hpp"
#include "ui/platform_input.hpp"

namespace penhu::native {

void Shell::configure_diag(const std::string& user, const std::string& pass,
                           const std::string& shot_path, const std::string& scan_dir) {
    diag_user_ = user;
    diag_pass_ = pass;
    diag_shot_ = shot_path;
    if (!scan_dir.empty()) diag_scan_dir_ = ui::to_wide(scan_dir);
    diag_stage_ = 0;
    diag_ms_ = 0.0f;
    g_diag_mode = true;
}


void Shell::tick_diag(HWND hwnd, float dt_ms) {
    if (diag_stage_ < 0) return;   // 没开诊断：正常运行永远在这里返回
    App& app = App::get();
    diag_ms_ += dt_ms;
    if (diag_stage_ == 0 && diag_ms_ > 800.0f) {
        Logger::default_logger().info("diag: 发起登录 " + diag_user_);
        app.login_async(diag_user_, diag_pass_, false);
        diag_stage_ = 1;
    } else if (diag_stage_ == 1 && !app.login_error.empty()) {
        // 登录失败必须立刻收工：无人值守的诊断不许在这里挂死等 token ——
        // 账号被删、账本被篡改（密钥校验失败）都会走到这里。
        Logger::default_logger().info("diag: 登录失败：" + app.login_error);
        Logger::default_logger().info("diag: 全部交互断言 有失败项");
        if (!diag_shot_.empty()) renderer_.save_png(diag_shot_);
        PostQuitMessage(15);
        return;
    } else if (diag_stage_ == 1 && !app.token.empty()) {
        Logger::default_logger().info("diag: token 就绪，等一帧渲染后注入交互");
        diag_stage_ = 2;
    } else if (diag_stage_ == 2) {
        InvalidateRect(hwnd, nullptr, FALSE);
        UpdateWindow(hwnd);   // 先强制画完一帧

        // 注入真实鼠标消息（而不是直接调组件方法），这样走的是完整链路：
        // 命中测试 → 组件回调 → 请求重建 → 下一轮消息里换掉整棵树 →
        // 再移动鼠标去通知「新的」悬停组件。
        // 「点一下就闪退」就发生在这条链路的最后一步。
        // 找一个真正会响应点击的组件再点 —— 而不是按固定坐标碰运气。
        // 上一版就是按坐标点，结果落在 Label 上（Label 不响应点击），
        // 根本没走到「点击 → 请求重建 → 换树」这条路，「没崩」是假象。
        ui::Widget* target = pick_diag_target(&tree_.root);
        if (target == nullptr) {
            Logger::default_logger().warn("diag: 没找到可点组件，跳过交互注入");
        } else {
            const ui::Rect box = target->bounds();
            const float scale = dpi_scale();
            const int cx = static_cast<int>(box.center_x() * scale);
            const int cy = static_cast<int>(box.center_y() * scale);
            diag_before_id_ = app.entry_category_id;
            Logger::default_logger().info(
                std::string("diag: 目标组件 ") + typeid(*target).name() + "  @" +
                std::to_string(cx) + "," + std::to_string(cy) +
                "  点击前 category_id=" + app.entry_category_id);
            PostMessageW(hwnd_, WM_MOUSEMOVE, 0, MAKELPARAM(cx, cy));
            PostMessageW(hwnd_, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(cx, cy));
            PostMessageW(hwnd_, WM_LBUTTONUP, 0, MAKELPARAM(cx, cy));
            // 点完立刻再动一下鼠标：重建之后若 hovered_ 还指着旧组件，
            // 崩的就是这一下
            PostMessageW(hwnd_, WM_MOUSEMOVE, 0, MAKELPARAM(cx + 40, cy + 30));
        }
        diag_stage_ = 3;
    } else if (diag_stage_ == 3) {
        InvalidateRect(hwnd, nullptr, FALSE);
        UpdateWindow(hwnd);
        if (!diag_shot_.empty()) renderer_.save_png(diag_shot_);
        Logger::default_logger().info(
            "diag: 交互后画面已保存 " + diag_shot_ +
            "  点击后 category_id=" + app.entry_category_id +
            (app.entry_category_id != diag_before_id_ ? "  （点击生效）"
                                                      : "  （点击没生效！）"));
        // 不退出 —— 接着验粘贴（用户报「API Key 粘不进去」的真实路径：
        // 设置页 → 密码框 → Ctrl+V）
        app.screen = Screen::Settings;
        app.rebuild_ui();
        diag_stage_ = 4;
        diag_ms_ = 0.0f;
    } else if (diag_stage_ == 4 && diag_ms_ > 400.0f) {
        // 设置页重建完成后，聚焦 API Key 框（密码框），注入 Ctrl+V
        ui::TextField* tf = pick_diag_text_field(&tree_.root, true);
        if (tf == nullptr) {
            Logger::default_logger().warn("diag: 设置页没找到密码输入框");
            PostQuitMessage(1);
            return;
        }
        set_focus(tf);
        diag_paste_before_ = app.set_api_key;
        const std::wstring probe = L"sk-diag-paste-9f8e7d6c";
        if (!ui::clipboard_write_text(probe)) {
            Logger::default_logger().warn("diag: 写剪贴板失败");
            PostQuitMessage(1);
            return;
        }
        Logger::default_logger().info("diag: 注入 Ctrl+V（目标：API Key 密码框）");
        PostMessageW(hwnd_, WM_KEYDOWN, VK_CONTROL, 0);
        PostMessageW(hwnd_, WM_KEYDOWN, 'V', 0);
        PostMessageW(hwnd_, WM_KEYUP, 'V', 0);
        PostMessageW(hwnd_, WM_KEYUP, VK_CONTROL, 0);
        diag_stage_ = 5;
        diag_ms_ = 0.0f;
    } else if (diag_stage_ == 5 && diag_ms_ > 400.0f) {
        // 断言「以测试串开头」而不是完全相等：PostMessage 注入的
        // 键盘消息不更新线程键盘状态表，TranslateMessage 因此会把
        // 'V' 翻译成字面 'v' 的 WM_CHAR 混进来 —— 那是注入方式的
        // 伪影，不是粘贴功能的问题（真实键盘按下时产生的是 0x16，
        // 已被拦截）。要验证的是「剪贴板内容完整进入了密码框」。
        const std::wstring probe = L"sk-diag-paste-9f8e7d6c";
        const bool ok = app.set_api_key.size() >= probe.size() &&
                        app.set_api_key.compare(0, probe.size(), probe) == 0;
        Logger::default_logger().info(
            std::string("diag: 粘贴验证 ") + (ok ? "通过" : "失败") +
            "  set_api_key 长度 " + std::to_string(diag_paste_before_.size()) +
            " -> " + std::to_string(app.set_api_key.size()) +
            (ok ? "" : "  实际内容=" + ui::to_utf8(app.set_api_key)));
        InvalidateRect(hwnd, nullptr, FALSE);
        UpdateWindow(hwnd);
        if (!diag_shot_.empty()) renderer_.save_png(diag_shot_);
        if (!ok) {
            PostQuitMessage(2);
            return;
        }
        // 不退出 —— 接着验滚动（用户报「界面无法滚动也无法拖动」）。
        // 滚轮不需要重新聚焦，直接回记账页（那页内容最长）。
        app.screen = Screen::Entry;
        app.rebuild_ui();
        diag_stage_ = 6;
        diag_ms_ = 0.0f;
    } else if (diag_stage_ == 6 && diag_ms_ > 400.0f) {
        ui::ScrollView* sv = find_scrollview(&tree_.root);
        if (sv == nullptr) {
            Logger::default_logger().warn("diag: 当前页面没有滚动容器");
            PostQuitMessage(3);
            return;
        }
        diag_offset_before_ = sv->offset;
        Logger::default_logger().info(
            "diag: 滚动容器 max_offset=" + std::to_string(sv->max_offset()) +
            "  当前 offset=" + std::to_string(sv->offset));
        if (sv->max_offset() <= 0.0f) {
            Logger::default_logger().warn(
                "diag: 内容没超出视口 —— 滚不动是「没得滚」，不是滚轮坏了");
            PostQuitMessage(4);
            return;
        }
        // 注入滚轮：坐标必须转成屏幕坐标（WM_MOUSEWHEEL 用的是屏幕坐标，
        // WM_MOUSEMOVE 用的才是客户区坐标 —— 这里最容易写错）。
        const ui::Size sz = renderer_.logical_size();
        const float scale = dpi_scale();
        POINT sp{static_cast<int>(sz.w * 0.4f * scale),
                 static_cast<int>(sz.h * 0.5f * scale)};
        ClientToScreen(hwnd_, &sp);
        Logger::default_logger().info("diag: 注入滚轮（向下）");
        PostMessageW(hwnd_, WM_MOUSEWHEEL,
                     MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)),
                     MAKELPARAM(sp.x, sp.y));
        diag_stage_ = 7;
        diag_ms_ = 0.0f;
    } else if (diag_stage_ == 7 && diag_ms_ > 400.0f) {
        ui::ScrollView* sv = find_scrollview(&tree_.root);
        const bool wheel_ok = sv != nullptr && sv->offset > diag_offset_before_ + 0.5f;
        Logger::default_logger().info(
            std::string("diag: 滚轮验证 ") + (wheel_ok ? "通过" : "失败") +
            "  offset " + std::to_string(diag_offset_before_) + " -> " +
            (sv != nullptr ? std::to_string(sv->offset) : std::string("null")));
        if (!wheel_ok) {
            PostQuitMessage(5);
            return;
        }

        // 接着验拖动滚动条。
        //
        // 拖动序列是刻意设计的：**唯一产生位移的那一步，鼠标在内容区
        // 而不是滚动条上**（x 一路移到左边）。因为真正要验证的是
        // 「按下时抓住滑块，之后移动都归它」这套捕获机制 ——
        // 如果只在滚动条上原地上下移动，即使没有捕获也能拖得动，
        // 测出来的「通过」等于没测。
        const ui::Rect thumb = sv->thumb_rect();
        if (thumb.w <= 0.0f) {
            Logger::default_logger().warn("diag: 拿不到滑块矩形");
            PostQuitMessage(6);
            return;
        }
        const float scale = dpi_scale();

        // 先把滚动位置放到中间再拖。滑块贴着顶端或底端时拖动没有余量 ——
        // 上一轮滚轮测试已经把它滚到底了，再往下拖只能动零点几个像素，
        // 断言会因为「变化量小于阈值」误报失败。那是测试没跟上代码，
        // 不是功能坏了（窗口尺寸改成按 DPI 换算之后，这一页从要滚 1000+
        // 变成只要滚 100 出头，就撞上了这个边界）。
        sv->scroll_to(sv->max_offset() * 0.35f, true);
        InvalidateRect(hwnd, nullptr, FALSE);
        UpdateWindow(hwnd);

        diag_offset_before_ = sv->offset;
        const ui::Rect thumb2 = sv->thumb_rect();
        const int cx = static_cast<int>((thumb2.x + thumb2.w * 0.5f) * scale);
        const int cy = static_cast<int>((thumb2.y + thumb2.h * 0.5f) * scale);
        const int far_x = cx - static_cast<int>(400.0f * scale);   // 内容区中间
        const int cy2 = cy + static_cast<int>(160.0f * scale);     // 往下拖
        Logger::default_logger().info(
            "diag: 注入拖动滑块 起点(" + std::to_string(cx) + "," + std::to_string(cy) +
            ") 终点(" + std::to_string(far_x) + "," + std::to_string(cy2) + "，已移出滚动条)");
        PostMessageW(hwnd_, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(cx, cy));
        for (int k = 1; k <= 5; ++k) {
            const int mx = cx + (far_x - cx) * k / 5;
            const int my = cy + (cy2 - cy) * k / 5;
            PostMessageW(hwnd_, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(mx, my));
        }
        PostMessageW(hwnd_, WM_LBUTTONUP, 0, MAKELPARAM(far_x, cy2));
        diag_stage_ = 8;
        diag_ms_ = 0.0f;
    } else if (diag_stage_ == 8 && diag_ms_ > 400.0f) {
        ui::ScrollView* sv = find_scrollview(&tree_.root);
        const bool drag_ok = sv != nullptr && sv->offset > diag_offset_before_ + 0.5f;
        Logger::default_logger().info(
            std::string("diag: 拖动验证（鼠标已移出滚动条）") +
            (drag_ok ? "通过" : "失败") +
            "  offset " + std::to_string(diag_offset_before_) + " -> " +
            (sv != nullptr ? std::to_string(sv->offset) : std::string("null")));
        if (!drag_ok || sv == nullptr) {
            PostQuitMessage(7);
            return;
        }

        // 再验「点击轨道空白处 → 跳到那里」这个滚动条的标准行为
        diag_offset_before_ = sv->offset;
        const ui::Rect thumb2 = sv->thumb_rect();
        const float s2 = dpi_scale();
        const int tx = static_cast<int>((sv->bounds().right() - 9.0f) * s2);
        const int ty = static_cast<int>((sv->bounds().bottom() - 6.0f) * s2);
        Logger::default_logger().info(
            "diag: 注入轨道点击 (" + std::to_string(tx) + "," + std::to_string(ty) + ") 滑块高=" +
            std::to_string(thumb2.h));
        PostMessageW(hwnd_, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(tx, ty));
        PostMessageW(hwnd_, WM_LBUTTONUP, 0, MAKELPARAM(tx, ty));
        diag_stage_ = 9;
        diag_ms_ = 0.0f;
    } else if (diag_stage_ == 9 && diag_ms_ > 400.0f) {
        ui::ScrollView* sv = find_scrollview(&tree_.root);
        // 点最底部：滑块中心落在那里，offset 应该接近 max_offset
        const bool track_ok =
            sv != nullptr && sv->offset > sv->max_offset() * 0.85f;
        Logger::default_logger().info(
            std::string("diag: 轨道点击验证 ") + (track_ok ? "通过" : "失败") +
            "  offset " + std::to_string(diag_offset_before_) + " -> " +
            (sv != nullptr ? std::to_string(sv->offset) : std::string("null")) +
            " / max " + (sv != nullptr ? std::to_string(sv->max_offset()) : "-"));
        InvalidateRect(hwnd, nullptr, FALSE);
        UpdateWindow(hwnd);
        if (!diag_shot_.empty()) renderer_.save_png(diag_shot_);
        if (!track_ok) {
            PostQuitMessage(8);
            return;
        }
        // 最后验「提示条不阻塞操作」（喷壶的原始抱怨）：
        // 弹一条提示，然后看提示条覆盖的位置能不能穿透、期间还能不能滚。
        app.show_toast(L"诊断：这条提示不该挡住任何操作", false);
        diag_stage_ = 10;
        diag_ms_ = 0.0f;
    } else if (diag_stage_ == 10 && diag_ms_ > 400.0f) {
        // ① 命中提示条覆盖的位置，不应该拿到提示条本身
        bool visible = false;
        ui::Widget* hit = nullptr;
        if (tree_.snack != nullptr && tree_.snack->visible) {
            visible = true;
            const ui::Rect b = tree_.snack->bounds();
            hit = tree_.root.hit_test({b.x + b.w * 0.5f, b.y + b.h * 0.5f});
        }
        const bool pass_through = visible && hit != nullptr &&
                                  dynamic_cast<ui::Snackbar*>(hit) == nullptr;
        Logger::default_logger().info(
            std::string("diag: 提示条穿透 ") +
            (pass_through ? "通过" : (visible ? "失败" : "失败（提示条没显示）")) +
            "  命中组件=" +
            (hit != nullptr ? typeid(*hit).name() : "null"));

        // ② 提示条显示期间，滚轮必须照样能滚 ——
        // 这就是「弹个提示整个界面点不动」那个 bug 的直接回归测试
        ui::ScrollView* sv = find_scrollview(&tree_.root);
        diag_offset_before_ = sv != nullptr ? sv->offset : 0.0f;
        Logger::default_logger().info(
            "diag: 提示条显示中，当前 offset=" + std::to_string(diag_offset_before_) +
            "（重建后应保持在上一次的位置，不是 0）");
        // 往上滚一格：如果位置被重建重置到 0，这一下就会「滚不动」，
        // 断言因此能同时抓住「提示条阻塞」和「重建丢位置」两种问题。
        const ui::Size sz = renderer_.logical_size();
        const float scale = dpi_scale();
        POINT sp{static_cast<int>(sz.w * 0.4f * scale),
                 static_cast<int>(sz.h * 0.5f * scale)};
        ClientToScreen(hwnd_, &sp);
        PostMessageW(hwnd_, WM_MOUSEWHEEL,
                     MAKEWPARAM(0, static_cast<WORD>(WHEEL_DELTA)),
                     MAKELPARAM(sp.x, sp.y));
        diag_snack_ok_ = pass_through;
        diag_stage_ = 11;
        diag_ms_ = 0.0f;
    } else if (diag_stage_ == 11 && diag_ms_ > 400.0f) {
        ui::ScrollView* sv = find_scrollview(&tree_.root);
        const bool scroll_ok =
            sv != nullptr &&
            std::fabs(sv->offset - diag_offset_before_) > 0.5f;
        const bool ok = diag_snack_ok_ && scroll_ok;
        Logger::default_logger().info(
            std::string("diag: 提示条期间滚动 ") + (scroll_ok ? "通过" : "失败") +
            "  offset " + std::to_string(diag_offset_before_) + " -> " +
            (sv != nullptr ? std::to_string(sv->offset) : std::string("null")));
        if (!ok) {
            Logger::default_logger().info("diag: 全部交互断言 有失败项");
            InvalidateRect(hwnd, nullptr, FALSE);
            UpdateWindow(hwnd);
            if (!diag_shot_.empty()) renderer_.save_png(diag_shot_);
            PostQuitMessage(9);
            return;
        }

        // ---- 多图识别：入库 + 批量失败路径 ----
        // 造两张：一张已识别（可入库），一张待识别（没有模型通道时会失败）。
        app.go(Screen::Scan);
        app.scan_items.clear();
        {
            ScanItem done;
            done.label = L"diag_done.png";
            done.dims = L"1080×2400";
            done.state = ScanItem::State::Done;
            done.draft.ready = true;
            done.draft.amount_minor = 4321;
            done.draft.direction = 0;
            done.draft.category_name = "餐饮";
            done.draft.category_id = app.category_id_of_name("餐饮");
            done.draft.date_iso = Date::today().to_string();
            done.draft.confidence = 0.9;
            app.scan_items.push_back(std::move(done));

            ScanItem todo;
            todo.label = L"diag_todo.png";
            todo.dims = L"1080×2400";
            app.scan_items.push_back(std::move(todo));
        }
        app.scan_selected = 0;
        app.rebuild_ui();
        diag_stage_ = 12;
        diag_ms_ = 0.0f;
    } else if (diag_stage_ == 12 && diag_ms_ > 400.0f) {
        // 断言入库：记录数 +1，且这一张被标记成已入账
        diag_records_before_ = app.records.size();
        const bool had_ready =
            !app.scan_items.empty() && app.scan_items[0].draft.ready;
        Logger::default_logger().info(
            "diag: 多图入库 注入前记录数=" + std::to_string(diag_records_before_) +
            "  第1张已识别=" + (had_ready ? "1" : "0"));
        app.confirm_scan();
        diag_stage_ = 13;
        diag_ms_ = 0.0f;
    } else if (diag_stage_ == 13 && diag_ms_ > 400.0f) {
        const bool applied =
            !app.scan_items.empty() && app.scan_items[0].applied;
        const bool grew = app.records.size() == diag_records_before_ + 1;
        const bool ok = applied && grew;
        Logger::default_logger().info(
            std::string("diag: 多图入库验证 ") + (ok ? "通过" : "失败") +
            "  记录数 " + std::to_string(diag_records_before_) + " -> " +
            std::to_string(app.records.size()) +
            "  第1张 applied=" + (applied ? "1" : "0"));
        if (!ok) {
            PostQuitMessage(10);
            return;
        }
        // 再走一遍批量识别：没有配置模型通道时，每一张都应该被明确
        // 标成失败并说清原因 —— 不许静默什么都不做。
        app.scan_async();
        app.rebuild_ui();
        diag_stage_ = 14;
        diag_ms_ = 0.0f;
    } else if (diag_stage_ == 14 && diag_ms_ > 800.0f) {
        // 第 2 张（未识别的那张）应当已经带上了可读的失败原因
        const bool marked = app.scan_items.size() >= 2 &&
                            app.scan_items[1].state == ScanItem::State::Failed &&
                            !app.scan_items[1].fail_reason.empty();
        Logger::default_logger().info(
            std::string("diag: 批量失败标记 ") + (marked ? "通过" : "失败") +
            "  第2张 state=" +
            (app.scan_items.size() >= 2
                 ? std::to_string(static_cast<int>(app.scan_items[1].state))
                 : std::string("?")) +
            "  原因=" +
            (app.scan_items.size() >= 2 ? app.scan_items[1].fail_reason
                                        : std::string("(缺项)")));
        if (!marked) {
            Logger::default_logger().info("diag: 全部交互断言 有失败项");
            PostQuitMessage(11);
            return;
        }

        // ---- 截图目录扫描（含去重）----
        // 目录由外部脚本准备好：3 张真 PNG + 1 个 txt（必须被忽略）
        app.clear_scans();
        // 默认用内置的测试目录（脚本从截图产物复制出来的副本）。
        // 传了 --diag-scan-dir 就用它 —— 拿真实数据核对扫描逻辑时用。
        // 注意：无论哪种情况，诊断都**不**打开自动清理去动真实目录。
        app.scan_dir = diag_scan_dir_.empty()
                           ? std::wstring(L"_scan_test")   // 当前目录下的测试夹具
                           : diag_scan_dir_;
        const int first = app.scan_directory();
        diag_scan_first_ = first;
        Logger::default_logger().info(
            "diag: 目录扫描第一遍 新增=" + std::to_string(first) +
            "（目录里 3 张图 + 1 个 txt）");
        // 同一批再扫一遍：应该一张都不加（列表内判重）
        const int second = app.scan_directory();
        Logger::default_logger().info(
            "diag: 目录扫描第二遍 新增=" + std::to_string(second) +
            "（应为 0，否则会重复记账）");
        if (app.scan_items.size() >= 1) {
            // 造一张「已识别」的，走确认入账 —— 它应该被记进已处理清单
            //
            // ⚠️ 覆盖缺口（2026-09-23 补注）：这里**直接伪造识别结果**，
            // 绕过了真正的识别链路（recognize_one → make_image_data_url
            // → WIC 解码）。代价是这条链路上「只在工作线程才暴露」的
            // 问题一条都测不出来 —— 比如 COM 套间是按线程算的，
            // 而识别跑在 std::thread 里。那个 bug 让「扫描识别」从来没成功过，
            // 而 4226 条断言全绿、10 张截图全对。
            // 要覆盖它得单独测（见 docs/WIC_THREAD_FIX.md 里的复现脚本）。
            app.scan_items[0].draft.ready = true;
            app.scan_items[0].draft.amount_minor = 1234;
            app.scan_items[0].draft.category_name = "餐饮";
            app.scan_items[0].draft.category_id =
                app.category_id_of_name("餐饮");
            app.scan_items[0].draft.date_iso = Date::today().to_string();
        }
        app.scan_selected = 0;
        diag_records_before_ = app.records.size();
        app.confirm_scan();
        diag_stage_ = 15;
        diag_ms_ = 0.0f;
    } else if (diag_stage_ == 15 && diag_ms_ > 400.0f) {
        // 入库后清空列表再扫：那一张应该因为「已处理」被跳过
        const bool applied_ok = app.records.size() == diag_records_before_ + 1;
        app.clear_scans();
        const int third = app.scan_directory();
        // 断言写成相对的：首扫数量取决于目录里实际有几张新图
        // （测试目录 3 张、真实目录可能 2 张），写死数字就会在
        // 换个目录时误报失败 —— 这种「测试自己出错」最浪费排查时间。
        //
        // 还要容忍「首扫就是 0」：演示账号的指纹表是持久化的，
        // 同一批测试文件跑第二遍时全都已见过，首扫自然是 0。
        // 这不算失败 —— 首扫为 0 恰恰说明「以前记过的没有再出现」，
        // 去重机制在正常工作。真正的失败形态是 first>0 且
        // third != first-1（记过还出现 = 去重坏了）。
        const bool dedup_ok =
            (diag_scan_first_ == 0 && third == 0) ||
            ((diag_scan_first_ > 0) && (third == diag_scan_first_ - 1));
        const bool ok = applied_ok && dedup_ok;
        Logger::default_logger().info(
            std::string("diag: 目录去重验证 ") + (ok ? "通过" : "失败") +
            "  首扫=" + std::to_string(diag_scan_first_) +
            "  入库后重扫=" + std::to_string(third) +
            "（首扫 0 = 全都以前见过；否则应为首扫减 1）");

        // 顺带验一下开关的持久化
        app.scan_auto_apply = true;
        app.save_scan_settings();
        app.load_scan_settings();
        const bool persist_ok = app.scan_auto_apply;
        Logger::default_logger().info(
            std::string("diag: 设置持久化 ") + (persist_ok ? "通过" : "失败") +
            "  auto_apply 读回=" + (app.scan_auto_apply ? "1" : "0"));
        // 恢复默认，别把测试状态留进账号设置
        app.scan_auto_apply = false;
        app.save_scan_settings();

        if (!ok || !persist_ok) {
            Logger::default_logger().info("diag: 全部交互断言 有失败项");
            PostQuitMessage(12);
            return;
        }

        // ---- 入账后把原图移到回收站 ----
        //
        // ⚠️ 只在**内置测试目录**上做这件事。传了 --diag-scan-dir
        // 说明目标是真实目录，那里面的图是用户的，自动清理的验证
        // 绝不能碰 —— 自动化里跑删除操作，护栏必须写在代码里，
        // 不能靠「我记得不要传那个参数」。
        if (!diag_scan_dir_.empty()) {
            Logger::default_logger().info(
                "diag: 跳过自动清理验证（扫描目录是用户指定的真实目录，不动里面的文件）");
            InvalidateRect(hwnd, nullptr, FALSE);
            UpdateWindow(hwnd);
            if (!diag_shot_.empty()) renderer_.save_png(diag_shot_);
            PostQuitMessage(0);
            return;
        }

        app.scan_auto_clean = true;
        // 取一张还没入账的，造出识别结果后确认入账 —— 它应该被移走
        const std::wstring victim =
            app.scan_items.empty() ? std::wstring() : app.scan_items[0].path;
        const std::wstring keeper =
            app.scan_items.size() > 1 ? app.scan_items[1].path : std::wstring();
        scan_pre_clean_exists_ =
            !victim.empty() && std::filesystem::exists(victim);
        if (!app.scan_items.empty()) {
            app.scan_items[0].draft.ready = true;
            app.scan_items[0].draft.amount_minor = 777;
            app.scan_items[0].draft.category_name = "餐饮";
            app.scan_items[0].draft.category_id =
                app.category_id_of_name("餐饮");
            app.scan_items[0].draft.date_iso = Date::today().to_string();
            app.scan_selected = 0;
            app.confirm_scan();
        }
        diag_stage_ = 16;
        diag_ms_ = 0.0f;
    } else if (diag_stage_ == 16 && diag_ms_ > 600.0f) {
        const std::wstring victim =
            app.scan_items.empty() ? std::wstring() : app.scan_items[0].path;
        const std::wstring keeper =
            app.scan_items.size() > 1 ? app.scan_items[1].path : std::wstring();

        const bool victim_gone =
            !victim.empty() && !std::filesystem::exists(victim);
        // 未入账的那张必须原样留着 —— 这是这个功能最重要的边界
        const bool keeper_kept =
            keeper.empty() || std::filesystem::exists(keeper);
        const bool ok = scan_pre_clean_exists_ && victim_gone && keeper_kept;

        Logger::default_logger().info(
            std::string("diag: 入账后清理原图 ") + (ok ? "通过" : "失败") +
            "  入账前文件在=" + (scan_pre_clean_exists_ ? "1" : "0") +
            "  入账后已移走=" + (victim_gone ? "1" : "0") +
            "  未入账的还在=" + (keeper_kept ? "1" : "0"));

        app.scan_auto_clean = false;
        app.save_scan_settings();
        if (!ok) {
            Logger::default_logger().info("diag: 全部交互断言 有失败项");
            PostQuitMessage(13);
            return;
        }

        // ---- 侧边栏：折叠 / 展开 + 切页 ----
        // 导航是每次操作都要经过的东西，它坏了整个应用就没法用，
        // 所以「点折叠按钮」和「点某一项」都要有断言。
        diag_rail_before_ = app.nav_expanded;
        const ui::Rect tr = tree_.rail->toggle_rect();
        const float s = dpi_scale();
        const int tx = static_cast<int>((tr.x + tr.w * 0.5f) * s);
        const int ty = static_cast<int>((tr.y + tr.h * 0.5f) * s);
        Logger::default_logger().info(
            "diag: 注入点击折叠按钮 (" + std::to_string(tx) + "," +
            std::to_string(ty) + ")  点击前 expanded=" +
            (app.nav_expanded ? "1" : "0"));
        PostMessageW(hwnd_, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(tx, ty));
        PostMessageW(hwnd_, WM_LBUTTONUP, 0, MAKELPARAM(tx, ty));
        diag_stage_ = 17;
        diag_ms_ = 0.0f;
    } else if (diag_stage_ == 17 && diag_ms_ > 400.0f) {
        const bool toggled = (app.nav_expanded != diag_rail_before_);
        Logger::default_logger().info(
            std::string("diag: 侧边栏折叠 ") + (toggled ? "通过" : "失败") +
            "  expanded " + (diag_rail_before_ ? "1" : "0") + " -> " +
            (app.nav_expanded ? "1" : "0"));
        if (!toggled) {
            PostQuitMessage(14);
            return;
        }

        // 点「统计」那一项，应当切页。
        // index 是 2 不是 1：导航里插了「明细」之后统计挪了一格。
        const ui::Rect ir = tree_.rail->item_rect(2);
        const float s = dpi_scale();
        const int ix = static_cast<int>((ir.x + ir.w * 0.5f) * s);
        const int iy = static_cast<int>((ir.y + ir.h * 0.5f) * s);
        diag_screen_before_ = static_cast<int>(app.screen);
        Logger::default_logger().info(
            "diag: 注入点击导航项「统计」(" + std::to_string(ix) + "," +
            std::to_string(iy) + ")");
        PostMessageW(hwnd_, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(ix, iy));
        PostMessageW(hwnd_, WM_LBUTTONUP, 0, MAKELPARAM(ix, iy));
        diag_stage_ = 18;
        diag_ms_ = 0.0f;
    } else if (diag_stage_ == 18 && diag_ms_ > 400.0f) {
        const bool switched =
            (static_cast<int>(app.screen) != diag_screen_before_) &&
            (app.screen == Screen::Stats);
        Logger::default_logger().info(
            std::string("diag: 侧边栏切页 ") + (switched ? "通过" : "失败") +
            "  screen " + std::to_string(diag_screen_before_) + " -> " +
            std::to_string(static_cast<int>(app.screen)) + "（期望 2=Stats）");
        // 顺手把展开状态恢复，别让测试留下折叠态
        app.nav_expanded = true;
        diag_rail_ok_ = switched;

        // ---- 识别结果合并（曾经静默崩溃的那段）----
        // pump() 里把工作线程的结果并进列表时，曾经嵌套锁住同一个
        // std::mutex → 抛 system_error → 没人接 → std::terminate。
        // 症状是「一点识别程序就消失」，而且不留任何日志。
        // 真实识别要配模型 + 联网，自动跑不了，所以直接注入一条结果；
        // 之后走 rebuild_ui() 触发 kMsgRefresh → pump()，
        // 和「工作线程完成一张后 notify 界面」是同一条路径。
        app.go(Screen::Scan);
        app.scan_items.clear();
        {
            ScanItem it;
            it.label = L"diag_merge.png";
            it.dims = L"1080×2400";
            app.scan_items.push_back(std::move(it));
        }
        app.scan_selected = 0;
        {
            ScanDraft d;
            d.ready = true;
            d.amount_minor = 1234;
            d.direction = 0;
            d.category_name = "餐饮";
            d.date_iso = Date::today().to_string();
            d.confidence = 0.88;
            Logger::default_logger().info(
                "diag: 注入一条识别结果，触发 pump 合并");
            app.inject_scan_result_for_diag(0, d);
        }
        app.rebuild_ui();
        diag_stage_ = 19;
        diag_ms_ = 0.0f;
    } else if (diag_stage_ == 19 && diag_ms_ > 600.0f) {
        // 结果必须真的被并进列表；崩掉的话根本走不到这里
        const bool merged =
            !app.scan_items.empty() && app.scan_items[0].draft.ready &&
            app.scan_items[0].state == ScanItem::State::Done &&
            app.scan_items[0].draft.amount_minor == 1234;
        const bool ok = diag_rail_ok_ && merged;
        Logger::default_logger().info(
            std::string("diag: 识别结果合并 ") + (merged ? "通过" : "失败") +
            "  ready=" +
            (!app.scan_items.empty() && app.scan_items[0].draft.ready ? "1" : "0") +
            "  state=" +
            std::to_string(!app.scan_items.empty()
                               ? static_cast<int>(app.scan_items[0].state)
                               : -1) +
            "  amount=" +
            std::to_string(!app.scan_items.empty()
                               ? app.scan_items[0].draft.amount_minor
                               : -1));
        diag_frame_ok_ = ok;

        // ---- 无边框窗口：命中测试的那个分歧点 ----
        // 顶栏空白处必须返回 HTCAPTION（交给系统拖窗口/双击最大化/
        // 拖到边缘吸附），窗口控制按钮上必须返回 HTCLIENT（归我们处理）。
        // 弄反的后果很具体：点关闭变成拖窗口（关不掉），
        // 或者整条顶栏拖不动（用户以为窗口卡死）。
        // 这里用 SendMessage 直接问窗口过程 —— 真实点击时，
        // 系统正是先做这一步再决定把消息发给谁。
        {
            const float s = dpi_scale();
            const ui::Rect tb = tree_.title->bounds();

            const int dx = static_cast<int>((tb.x + tb.w * 0.45f) * s);
            const int dy = static_cast<int>((tb.y + tb.h * 0.5f) * s);
            POINT dp{dx, dy};
            ClientToScreen(hwnd_, &dp);
            const LRESULT hit_drag =
                SendMessageW(hwnd_, WM_NCHITTEST, 0, MAKELPARAM(dp.x, dp.y));
            diag_title_drag_ok_ = (hit_drag == HTCAPTION);

            const ui::Rect cb = tree_.title->control_rect(0);   // 关闭
            const int bx = static_cast<int>((cb.x + cb.w * 0.5f) * s);
            const int by = static_cast<int>((cb.y + cb.h * 0.5f) * s);
            POINT bp{bx, by};
            ClientToScreen(hwnd_, &bp);
            const LRESULT hit_btn =
                SendMessageW(hwnd_, WM_NCHITTEST, 0, MAKELPARAM(bp.x, bp.y));
            diag_title_btn_ok_ = (hit_btn == HTCLIENT);

            Logger::default_logger().info(
                std::string("diag: 顶栏拖拽区 ") +
                (diag_title_drag_ok_ ? "通过" : "失败") +
                "  NCHITTEST=" + std::to_string(hit_drag) +
                "（期望 2=HTCAPTION）");
            Logger::default_logger().info(
                std::string("diag: 控制按钮区 ") +
                (diag_title_btn_ok_ ? "通过" : "失败") +
                "  NCHITTEST=" + std::to_string(hit_btn) +
                "（期望 1=HTCLIENT）");

            // 非客户区必须为 0。这条断言专门盯「系统那条标题栏还在不在」——
            // 只验 NCHITTEST 是不够的：那只证明「我们想让系统接管拖动」，
            // 证明不了「系统标题栏真的消失了」。
            //
            // （这个 bug 真发生过：窗口顶部同时出现系统标题栏和自绘顶栏。
            //   根因是只做了 WM_NCCALCSIZE 返回 0，忘了从窗口样式里去掉
            //   WS_CAPTION —— 而当时那批断言全绿。差别在于：
            //   它们验的是「我加了什么」，没验「该去掉的东西去掉了没有」。）
            RECT wr{}, cr{};
            GetWindowRect(hwnd_, &wr);
            GetClientRect(hwnd_, &cr);
            const int nc_w = (wr.right - wr.left) - (cr.right - cr.left);
            const int nc_h = (wr.bottom - wr.top) - (cr.bottom - cr.top);
            diag_no_frame_ok_ = (nc_w == 0 && nc_h == 0);
            // 双缓冲机制是否真的挂上了。闪烁是视觉现象、
            // 自动断言看不到，但「COMPOSITED 有没有生效」是可测的
            // —— 它没生效的话，拖拽缩放和窗口动画时的撕裂一定会回来。
            {
                const LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
                const bool comp = (ex & WS_EX_COMPOSITED) != 0;
                Logger::default_logger().info(
                    std::string("diag: 窗口双缓冲 ") +
                    (comp ? "通过  WS_EX_COMPOSITED 已挂" : "失败  缺 WS_EX_COMPOSITED"));
                diag_no_frame_ok_ = diag_no_frame_ok_ && comp;
            }
            Logger::default_logger().info(
                std::string("diag: 无系统标题栏 ") +
                (diag_no_frame_ok_ ? "通过" : "失败") +
                "  非客户区 " + std::to_string(nc_w) + "x" +
                std::to_string(nc_h) + "（期望 0x0）");
        }

        // 先记下「正常矩形」：等会儿还原完，窗口要一分不差地回到这里。
        // 这个测量是必须的 —— 只断言「IsZoomed 变回 false」证明不了
        // 尺寸和位置没漂，而最大化/还原最容易出的问题恰恰是漂。
        {
            WINDOWPLACEMENT wp{};
            wp.length = sizeof(wp);
            if (GetWindowPlacement(hwnd_, &wp)) {
                diag_restore_rect_ = wp.rcNormalPosition;
                Logger::default_logger().info(
                    "diag: 最大化前 rcNormalPosition (" +
                    std::to_string(wp.rcNormalPosition.left) + "," +
                    std::to_string(wp.rcNormalPosition.top) + ") " +
                    std::to_string(wp.rcNormalPosition.right -
                                   wp.rcNormalPosition.left) + "x" +
                    std::to_string(wp.rcNormalPosition.bottom -
                                   wp.rcNormalPosition.top));
            }
        }

        // 注入点击「最大化」按钮，走完整链路：命中测试 → 组件回调 → ShowWindow
        {
            const float s = dpi_scale();
            const ui::Rect mb = tree_.title->control_rect(1);
            const int mx = static_cast<int>((mb.x + mb.w * 0.5f) * s);
            const int my = static_cast<int>((mb.y + mb.h * 0.5f) * s);
            Logger::default_logger().info(
                "diag: 注入点击最大化按钮 (" + std::to_string(mx) + "," +
                std::to_string(my) + ")");
            PostMessageW(hwnd_, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(mx, my));
            PostMessageW(hwnd_, WM_LBUTTONUP, 0, MAKELPARAM(mx, my));
        }
        diag_stage_ = 20;
        diag_ms_ = 0.0f;
    } else if (diag_stage_ == 20) {
        // 注入最大化之后，先连续 10 帧记录窗口矩形 ——
        // 这一步是判断**系统自带的**最大化动画到底有没有生效：
        //   · 矩形在百来毫秒内逐帧变化 → 系统动画生效，
        //     那就不能再自己做一套（两套动画叠在一起只会打架、闪烁）；
        //   · 第一帧就已是最终矩形 → 没有动画，才需要自己实现。
        // 无边框窗口（WS_POPUP）能不能吃到系统动画是没法靠猜的，必须测。
        if (diag_seq_n_ < 10) {
            RECT wr{};
            GetWindowRect(hwnd_, &wr);
            diag_max_seq_ +=
                " | t=" + std::to_string(static_cast<int>(diag_ms_)) +
                " (" + std::to_string(wr.left) + "," +
                std::to_string(wr.top) + ") " +
                std::to_string(wr.right - wr.left) + "x" +
                std::to_string(wr.bottom - wr.top);
            ++diag_seq_n_;
            if (diag_seq_n_ == 10) {
                Logger::default_logger().info(
                    "diag: 最大化过程矩形序列" + diag_max_seq_);
            }
            return;   // 下一帧继续收集
        }
        if (diag_ms_ < 600.0f) return;   // 等它彻底定下来再断言
        // 最大化后客户区必须正好等于显示器工作区：
        // 少了底部会被任务栏压住，多了会在屏幕外留一条缝。
        RECT cr{};
        GetClientRect(hwnd_, &cr);
        POINT tl{0, 0};
        ClientToScreen(hwnd_, &tl);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
        const int work_w = mi.rcWork.right - mi.rcWork.left;
        const int work_h = mi.rcWork.bottom - mi.rcWork.top;
        // 容 2px 取整误差：圆角边框在不同 DPI 下会差一点
        // 手动最大化下 IsZoomed 恒为 0（不进 zoomed 态），
        // 断言改盯 manual_max_ 标志 + 客户区几何本身。
        diag_max_ok_ = manual_max_ &&
                       std::abs((cr.right - cr.left) - work_w) <= 2 &&
                       std::abs((cr.bottom - cr.top) - work_h) <= 2 &&
                       tl.x == mi.rcWork.left && tl.y == mi.rcWork.top;
        Logger::default_logger().info(
            std::string("diag: 最大化填满工作区 ") +
            (diag_max_ok_ ? "通过" : "失败") +
            "  manual=" + (manual_max_ ? "1" : "0") +
            " zoomed=" + (IsZoomed(hwnd_) ? "1" : "0") +
            "  客户区 " + std::to_string(cr.right - cr.left) + "x" +
            std::to_string(cr.bottom - cr.top) +
            "  工作区 " + std::to_string(work_w) + "x" +
            std::to_string(work_h) +
            "  原点(" + std::to_string(tl.x) + "," + std::to_string(tl.y) + ")");
        // ★ 任务栏可见性断言 —— 「最大化后任务栏被涂黑」bug 的直接回归。
        // 根因链：无边框窗口最大化时矩形外扩盖满显示器 → Windows
        // 判定为全屏应用、自动隐藏任务栏 → 外扩环没人画 → 黑条。
        // 修复后任务栏必须还在：点它中心命中的必须是任务栏本体。
        {
            HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
            RECT tr{};
            bool tray_ok = false;
            const wchar_t* hit_name = L"(null)";
            wchar_t cls[64] = {};
            if (tray != nullptr && GetWindowRect(tray, &tr) &&
                IsWindowVisible(tray)) {
                POINT c{(tr.left + tr.right) / 2, (tr.top + tr.bottom) / 2};
                HWND hitw = WindowFromPoint(c);
                if (hitw != nullptr) {
                    GetClassNameW(hitw, cls, 64);
                    hit_name = cls;
                }
                tray_ok = (hitw == tray);
            }
            Logger::default_logger().info(
                std::string("diag: 任务栏可见 ") + (tray_ok ? "通过" : "失败") +
                "  任务栏中心命中: " +
                std::string(tray_ok ? "Shell_TrayWnd" : "（其他）") +
                (tray_ok ? "" : std::string(" -> ") +
                     std::string(hit_name, hit_name + wcslen(hit_name))));
            diag_max_ok_ = diag_max_ok_ && tray_ok;
        }
        // 抓一张**整块屏幕**（含任务栏）存档：离屏截图只含客户区，
        // 黑条那种「窗口之外」的问题只有屏幕截图能亲眼确认。
        if (!diag_shot_.empty()) {
            std::wstring sp = ui::to_wide(diag_shot_);
            const size_t dot = sp.find_last_of(L'.');
            if (dot != std::wstring::npos) {
                sp = sp.substr(0, dot) + L"_max_screen.png";
                if (!renderer_.save_screen_png(std::string(sp.begin(),
                    sp.end()))) {
                    Logger::default_logger().warn("diag: 抓屏保存失败");
                }
            }
        }
        // 顺带记录**窗口矩形**（不是客户区）：最大化时系统会按「有边框」
        // 把窗口矩形外扩，知道这一点才能把还原动画的目标算对。
        {
            RECT wr{};
            GetWindowRect(hwnd_, &wr);
            WINDOWPLACEMENT wp{};
            wp.length = sizeof(wp);
            if (GetWindowPlacement(hwnd_, &wp)) {
                Logger::default_logger().info(
                    "diag: 最大化后 窗口矩形 (" + std::to_string(wr.left) + "," +
                    std::to_string(wr.top) + ") " +
                    std::to_string(wr.right - wr.left) + "x" +
                    std::to_string(wr.bottom - wr.top) +
                    "  rcNormalPosition (" +
                    std::to_string(wp.rcNormalPosition.left) + "," +
                    std::to_string(wp.rcNormalPosition.top) + ") " +
                    std::to_string(wp.rcNormalPosition.right -
                                   wp.rcNormalPosition.left) + "x" +
                    std::to_string(wp.rcNormalPosition.bottom -
                                   wp.rcNormalPosition.top));
            }
        }
        // 还原走**真实路径**：注入点击顶栏的那个按钮。
        // 直接 ShowWindow(SW_RESTORE) 会绕开我们自己的动画 ——
        // 那样这一段就等于没测（动画里的 bug 一个都暴露不出来）。
        {
            const float s = dpi_scale();
            const ui::Rect mb = tree_.title->control_rect(1);
            const int mx = static_cast<int>((mb.x + mb.w * 0.5f) * s);
            const int my = static_cast<int>((mb.y + mb.h * 0.5f) * s);
            Logger::default_logger().info(
                "diag: 注入点击还原按钮 (" + std::to_string(mx) + "," +
                std::to_string(my) + ")");
            PostMessageW(hwnd_, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(mx, my));
            PostMessageW(hwnd_, WM_LBUTTONUP, 0, MAKELPARAM(mx, my));
        }
        diag_stage_ = 21;
        diag_ms_ = 0.0f;
    } else if (diag_stage_ == 21 && diag_ms_ > 600.0f) {
        const bool restored = !is_maximized();
        RECT cr{};
        GetClientRect(hwnd_, &cr);
        Logger::default_logger().info(
            std::string("diag: 还原窗口 ") + (restored ? "通过" : "失败") +
            "  客户区 " + std::to_string(cr.right - cr.left) + "x" +
            std::to_string(cr.bottom - cr.top));
        // 还原必须回到最大化之前的**原位置原尺寸**。
        // 只断言 IsZoomed 变 false 是不够的。
        {
            RECT now{};
            GetWindowRect(hwnd_, &now);
            const RECT& want = diag_restore_rect_;
            diag_restore_ok_ =
                (now.left == want.left) && (now.top == want.top) &&
                ((now.right - now.left) == (want.right - want.left)) &&
                ((now.bottom - now.top) == (want.bottom - want.top));
            Logger::default_logger().info(
                std::string("diag: 还原位置尺寸 ") +
                (diag_restore_ok_ ? "通过" : "失败") +
                "  现在是 (" + std::to_string(now.left) + "," +
                std::to_string(now.top) + ") " +
                std::to_string(now.right - now.left) + "x" +
                std::to_string(now.bottom - now.top) +
                "  期望 (" + std::to_string(want.left) + "," +
                std::to_string(want.top) + ") " +
                std::to_string(want.right - want.left) + "x" +
                std::to_string(want.bottom - want.top));
        }
        const bool ok = diag_frame_ok_ && diag_title_drag_ok_ &&
                        diag_title_btn_ok_ && diag_no_frame_ok_ &&
                        diag_max_ok_ && restored && diag_restore_ok_;
        if (!ok) {
            Logger::default_logger().info("diag: 全部交互断言 有失败项");
            InvalidateRect(hwnd, nullptr, FALSE);
            UpdateWindow(hwnd);
            if (!diag_shot_.empty()) renderer_.save_png(diag_shot_);
            PostQuitMessage(17);
            return;
        }

        // ---- 全屏：它和最大化必须分得清 ----
        // 最大化 = 工作区（不压任务栏）；全屏 = 整个显示器（盖住任务栏）。
        // 断言看的是**客户区**分别等于 rcWork 还是 rcMonitor。
        {
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST),
                                &mi)) {
                diag_mon_ = mi.rcMonitor;
                mi_work_ = mi.rcWork;
            }
            Logger::default_logger().info(
                "diag: 注入 F11 进入全屏  显示器 " +
                std::to_string(diag_mon_.right - diag_mon_.left) + "x" +
                std::to_string(diag_mon_.bottom - diag_mon_.top) +
                "  工作区 " +
                std::to_string(mi_work_.right - mi_work_.left) + "x" +
                std::to_string(mi_work_.bottom - mi_work_.top));
            PostMessageW(hwnd_, WM_KEYDOWN, VK_F11, 0);
            PostMessageW(hwnd_, WM_KEYUP, VK_F11, 0);
        }
        diag_stage_ = 22;
        diag_ms_ = 0.0f;
        return;
    } else if (diag_stage_ == 22 && diag_ms_ > 700.0f) {
        // 全屏态：客户区应等于**整个显示器**（含任务栏那条）
        RECT cr{};
        GetClientRect(hwnd_, &cr);
        const int want_w = diag_mon_.right - diag_mon_.left;
        const int want_h = diag_mon_.bottom - diag_mon_.top;
        diag_full_ok_ = std::abs((cr.right - cr.left) - want_w) <= 2 &&
                        std::abs((cr.bottom - cr.top) - want_h) <= 2;
        Logger::default_logger().info(
            std::string("diag: 全屏占满显示器 ") +
            (diag_full_ok_ ? "通过" : "失败") +
            "  客户区 " + std::to_string(cr.right - cr.left) + "x" +
            std::to_string(cr.bottom - cr.top) +
            "  显示器 " + std::to_string(want_w) + "x" +
            std::to_string(want_h) +
            "  任务栏那条 " +
            std::to_string((diag_mon_.bottom - diag_mon_.top) -
                           (mi_work_.bottom - mi_work_.top)) +
            "px 应该被盖住");
        PostMessageW(hwnd_, WM_KEYDOWN, VK_F11, 0);   // 退出全屏
        PostMessageW(hwnd_, WM_KEYUP, VK_F11, 0);
        diag_stage_ = 23;
        diag_ms_ = 0.0f;
        return;
    } else if (diag_stage_ == 23 && diag_ms_ > 700.0f) {
        // 退出全屏必须回到进入之前那个矩形
        RECT now{};
        GetWindowRect(hwnd_, &now);
        const RECT& want = diag_restore_rect_;
        diag_exit_full_ok_ =
            (now.left == want.left) && (now.top == want.top) &&
            ((now.right - now.left) == (want.right - want.left)) &&
            ((now.bottom - now.top) == (want.bottom - want.top));
        Logger::default_logger().info(
            std::string("diag: 退出全屏回到原位 ") +
            (diag_exit_full_ok_ ? "通过" : "失败") +
            "  现在是 (" + std::to_string(now.left) + "," +
            std::to_string(now.top) + ") " +
            std::to_string(now.right - now.left) + "x" +
            std::to_string(now.bottom - now.top) +
            "  期望 (" + std::to_string(want.left) + "," +
            std::to_string(want.top) + ") " +
            std::to_string(want.right - want.left) + "x" +
            std::to_string(want.bottom - want.top));
        diag_chain_ok_ = diag_full_ok_ && diag_exit_full_ok_;

        // ---- 连续缩放：模拟真人拖窗口边缘来回拉 ----
        diag_stage_ = 24;
        diag_ms_ = 0.0f;
        return;
    } else if (diag_stage_ == 24) {
        // 「闪不闪」自动化测不了（那要看屏幕），但能测三件实事：
        // ① 连续缩放到最后崩溃没有；② 尺寸和渲染目标是否一致；
        // ③ 每一步的重绘成本 —— 成本高到跟不上鼠标，就是闪的根源。
        RECT base{};
        GetWindowRect(hwnd_, &base);
        const int bw = base.right - base.left;
        const int bh = base.bottom - base.top;
        const uint64_t t0 = GetTickCount64();
        const int paints_before = paint_count_;
        int steps = 0;
        // 先缩后放，中间跨过 1180 那个栏位档（单栏 ↔ 多栏），
        // 顺带把「跨档重建页面」这条路也压一遍。
        // 15 步而不是 16：原来 i<8?i:(15-i) 会让 k=7 连着出现两次，
        // 尺寸没变就不发 WM_SIZE，于是少一次重绘 —— 断言会误报失败。
        // 测试自己出错最浪费排查时间，序列要保证**相邻两步一定不同**。
        for (int i = 0; i < 15; ++i) {
            const int k = (i < 8) ? i : (14 - i);
            const int w = bw + (k - 4) * 90;
            const int h = bh + (k - 4) * 60;
            // NOCOPYBITS 与动画路径同口径：这一步量的是我们
            // 自己的重绘成本，不该掺进系统旧像素拷贝的开销。
            SetWindowPos(hwnd_, nullptr, base.left, base.top, w, h,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER |
                         SWP_NOCOPYBITS);
            ++steps;
        }
        SetWindowPos(hwnd_, nullptr, base.left, base.top, bw, bh,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER |
                     SWP_NOCOPYBITS);
        ++steps;
        diag_resize_ms_ = static_cast<int>(GetTickCount64() - t0);

        // 无边框窗口的客户区 == 窗口矩形，所以可以直接比
        RECT cr{};
        GetClientRect(hwnd_, &cr);
        const bool size_ok = std::abs((cr.right - cr.left) - bw) <= 2 &&
                             std::abs((cr.bottom - cr.top) - bh) <= 2;
        // ★ 这一步是「缩放不闪」的关键断言：
        // 每一步尺寸变化都必须当场画完，不能拖到后面的 WM_PAINT。
        // 差一步就说明那一帧窗口是「已经变大但还没画」，
        // 露出来的就是背景色 —— 也就是肉眼看到的白闪。
        const int painted = paint_count_ - paints_before;
        diag_resize_ok_ = size_ok && renderer_.has_target() && painted >= steps;
        Logger::default_logger().info(
            std::string("diag: 连续缩放 ") + (diag_resize_ok_ ? "通过" : "失败") +
            "  " + std::to_string(steps) + " 步 / " +
            std::to_string(diag_resize_ms_) + "ms  " +
            std::to_string(diag_resize_ms_ / steps) + "ms/步  " +
            "  当场重绘 " + std::to_string(painted) + "/" +
            std::to_string(steps) + " 步" +
            "  最终客户区 " + std::to_string(cr.right - cr.left) + "x" +
            std::to_string(cr.bottom - cr.top) + "  期望 " +
            std::to_string(bw) + "x" + std::to_string(bh));
        diag_chain_ok_ = diag_chain_ok_ && diag_resize_ok_;

        // 接着走「最大化 → 全屏 → 最大化 → 还原」这条链。
        // 单看每一步都对，不等于连起来也对 —— 状态叠加最容易漏。
        {
            const float s = dpi_scale();
            const ui::Rect mb = tree_.title->control_rect(1);
            const int mx = static_cast<int>((mb.x + mb.w * 0.5f) * s);
            const int my = static_cast<int>((mb.y + mb.h * 0.5f) * s);
            PostMessageW(hwnd_, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(mx, my));
            PostMessageW(hwnd_, WM_LBUTTONUP, 0, MAKELPARAM(mx, my));
        }
        diag_stage_ = 25;
        diag_ms_ = 0.0f;
        return;
    } else if (diag_stage_ == 25 && diag_ms_ > 700.0f) {
        // 链①：点按钮最大化 → 客户区 == 工作区，任务栏必须还在
        RECT cr{};
        GetClientRect(hwnd_, &cr);
        const bool m = std::abs((cr.right - cr.left) -
                                (mi_work_.right - mi_work_.left)) <= 2 &&
                       std::abs((cr.bottom - cr.top) -
                                (mi_work_.bottom - mi_work_.top)) <= 2 &&
                       is_maximized() && !fullscreen_;
        diag_chain_ok_ = diag_chain_ok_ && m;
        Logger::default_logger().info(
            std::string("diag: 链①按钮→最大化 ") + (m ? "通过" : "失败") +
            "  客户区 " + std::to_string(cr.right - cr.left) + "x" +
            std::to_string(cr.bottom - cr.top) + "  工作区 " +
            std::to_string(mi_work_.right - mi_work_.left) + "x" +
            std::to_string(mi_work_.bottom - mi_work_.top) +
            "  maximized=" + std::to_string(is_maximized() ? 1 : 0) +
            " 全屏=" + std::to_string(fullscreen_ ? 1 : 0));
        PostMessageW(hwnd_, WM_KEYDOWN, VK_F11, 0);
        PostMessageW(hwnd_, WM_KEYUP, VK_F11, 0);
        diag_stage_ = 26;
        diag_ms_ = 0.0f;
        return;
    } else if (diag_stage_ == 26 && diag_ms_ > 700.0f) {
        // 链②：最大化 → 全屏，客户区应等于**整个显示器**
        RECT cr{};
        GetClientRect(hwnd_, &cr);
        const bool f = std::abs((cr.right - cr.left) -
                                (diag_mon_.right - diag_mon_.left)) <= 2 &&
                       std::abs((cr.bottom - cr.top) -
                                (diag_mon_.bottom - diag_mon_.top)) <= 2 &&
                       fullscreen_;
        // 全屏的语义就是盖住任务栏：点任务栏中心命中的必须是本窗口
        {
            HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
            bool covered = false;
            if (tray != nullptr) {
                RECT tr{};
                if (GetWindowRect(tray, &tr)) {
                    POINT c{(tr.left + tr.right) / 2,
                            (tr.top + tr.bottom) / 2};
                    covered = (WindowFromPoint(c) == hwnd_);
                }
            }
            diag_chain_ok_ = diag_chain_ok_ && f && covered;
            Logger::default_logger().info(
                std::string("diag: 全屏盖住任务栏 ") +
                (covered ? "通过" : "失败"));
        }
        Logger::default_logger().info(
            std::string("diag: 链②最大化→全屏 ") + (f ? "通过" : "失败") +
            "  客户区 " + std::to_string(cr.right - cr.left) + "x" +
            std::to_string(cr.bottom - cr.top) + "  显示器 " +
            std::to_string(diag_mon_.right - diag_mon_.left) + "x" +
            std::to_string(diag_mon_.bottom - diag_mon_.top));
        PostMessageW(hwnd_, WM_KEYDOWN, VK_F11, 0);   // 退出全屏
        PostMessageW(hwnd_, WM_KEYUP, VK_F11, 0);
        diag_stage_ = 27;
        diag_ms_ = 0.0f;
        return;
    } else if (diag_stage_ == 27 && diag_ms_ > 700.0f) {
        // 链③：退出全屏应**回到最大化**，不是回到普通窗口。
        // 这一条专门盯 rcNormalPosition 被污染的问题：
        // 全屏期间系统把还原目标同步成了全屏尺寸，
        // 不自己存一份的话，后面再点还原就会漂。
        RECT cr{};
        GetClientRect(hwnd_, &cr);
        const bool back = is_maximized() && !fullscreen_ &&
                          std::abs((cr.bottom - cr.top) -
                                   (mi_work_.bottom - mi_work_.top)) <= 2;
        diag_chain_ok_ = diag_chain_ok_ && back;
        Logger::default_logger().info(
            std::string("diag: 链③全屏→回到最大化 ") + (back ? "通过" : "失败") +
            "  maximized=" + std::to_string(is_maximized() ? 1 : 0) +
            " 全屏=" + std::to_string(fullscreen_ ? 1 : 0) + "  客户区 " +
            std::to_string(cr.right - cr.left) + "x" +
            std::to_string(cr.bottom - cr.top));
        {
            const float s = dpi_scale();
            const ui::Rect mb = tree_.title->control_rect(1);
            const int mx = static_cast<int>((mb.x + mb.w * 0.5f) * s);
            const int my = static_cast<int>((mb.y + mb.h * 0.5f) * s);
            PostMessageW(hwnd_, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(mx, my));
            PostMessageW(hwnd_, WM_LBUTTONUP, 0, MAKELPARAM(mx, my));
        }
        diag_stage_ = 28;
        diag_ms_ = 0.0f;
        return;
    } else if (diag_stage_ == 28 && diag_ms_ > 700.0f) {
        // 链④：再点一次应还原到**最初的矩形**，一分不差
        RECT now{};
        GetWindowRect(hwnd_, &now);
        const RECT& want = diag_restore_rect_;
        const bool r = (now.left == want.left) && (now.top == want.top) &&
                       ((now.right - now.left) == (want.right - want.left)) &&
                       ((now.bottom - now.top) == (want.bottom - want.top)) &&
                       !is_maximized() && !fullscreen_;
        diag_chain_ok_ = diag_chain_ok_ && r;
        Logger::default_logger().info(
            std::string("diag: 链④还原到最初矩形 ") + (r ? "通过" : "失败") +
            "  现在是 (" + std::to_string(now.left) + "," +
            std::to_string(now.top) + ") " +
            std::to_string(now.right - now.left) + "x" +
            std::to_string(now.bottom - now.top) + "  期望 (" +
            std::to_string(want.left) + "," + std::to_string(want.top) + ") " +
            std::to_string(want.right - want.left) + "x" +
            std::to_string(want.bottom - want.top));
        Logger::default_logger().info(
            std::string("diag: 全部交互断言 ") +
            (diag_chain_ok_ ? "通过" : "有失败项"));
        InvalidateRect(hwnd, nullptr, FALSE);
        UpdateWindow(hwnd);
        if (!diag_shot_.empty()) renderer_.save_png(diag_shot_);
        PostQuitMessage(diag_chain_ok_ ? 0 : 18);
    }
}

}  // namespace penhu::native
