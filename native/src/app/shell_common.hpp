#pragma once
// =============================================================================
//  native/app/shell_common.hpp
//  两个平台的 shell 都要用的那几个辅助函数。
//
//  为什么单独一个头而不是塞回 shell.hpp：
//  shell.hpp 末尾要按平台带出各自的 Shell 类（里面是 Win32 / Wayland 的成员），
//  而下面这些是**纯 UI 树操作**，和平台毫无关系。混在一起会让「哪些是共享的」
//  变得看不出来 —— 这正是移植时最容易踩的坑（共享代码里悄悄用了平台 API）。
// =============================================================================

#include <cstdint>
#include <vector>

#include "app/app.hpp"
#include "ui/controls.hpp"
#include "ui/foundation.hpp"
#include "ui/widget.hpp"

namespace penhu::native {

/// 沿父链冒泡分发滚轮事件。返回是否有人消费。
///
/// **不要退回「只把事件发给最深命中节点」的写法** —— 那样会坏一整类交互：
/// 滚轮落在页面里任何一个 Label / Card 上时命中的是那个叶子节点，
/// 而它的 on_wheel 是默认空实现，事件就此止步，永远到不了外层的滚动容器。
/// 用户看到的现象就是「界面怎么滚都不动」（这就是当初滚动失效的原因）。
bool bubble_wheel(ui::Widget* hit, const ui::Point& p, float delta);

/// 按下事件的冒泡版本，返回**真正消费**它的那个组件（没有则 nullptr）。
/// 记下消费者而不是最深命中者，是为了拖动：滚动条的拖动依赖
/// 「按下时抓住滑块，之后所有移动都归它」，而不是靠鼠标恰好停在滑块上。
ui::Widget* bubble_down(ui::Widget* hit, const ui::Point& p);

/// 深度优先找第一个 ScrollView（滚动相关的验证用）
ui::ScrollView* find_scrollview(ui::Widget* w);

/// 演示数据用的伪随机（固定种子 → 每次截图内容一致）。
/// 内联在这里是因为它只有四行，而且两个平台的截图模式都要用。
inline uint32_t next_pseudo(uint32_t& state) {
    state = state * 1664525u + 1013904223u;
    return state >> 16;
}

// ---- 诊断模式用的挑选器（只有界面诊断会用到）----

/// 收集所有「点了会有反应」的组件（Chip / Button / ListRow）
void collect_clickables(ui::Widget* w, std::vector<ui::Widget*>& out);

/// 挑一个「点下去状态一定会变」的组件用来注入点击。
///
/// 这里踩过两次坑，都是「验证方法本身不成立」而不是代码有问题：
///   · 第一次按固定坐标点 → 落在 Label 上（不响应点击），根本没走到重建路径；
///   · 第二次改成点第一个 Chip → 它是**已经选中**的那个，点完状态当然不变，
///     日志里却显示「点击没生效」，看着像 bug。
/// 所以现在挑「第一个未选中的分类格」。
ui::Widget* pick_diag_target(ui::Widget* root);

/// 挑一个「可以粘文本」的输入框：要非 numeric 的（numeric 框会把
/// "PASTE-CHECK-…" 这种测试串整体拒绝，那样测出来的「没生效」是测试串的问题，
/// 不是粘贴功能的问题 —— 又是一次假的失败信号）。
/// want_password = true 时专门找密码框。
ui::TextField* pick_diag_text_field(ui::Widget* w, bool want_password);

/// 截图模式：准备演示账号并灌一批确定性的数据。
/// 用固定种子，保证每次截图内容一致 —— 否则「这次和上次不一样」
/// 会让人分不清是排版改了还是数据变了。
bool prepare_demo_account(App& app);

}  // namespace penhu::native
