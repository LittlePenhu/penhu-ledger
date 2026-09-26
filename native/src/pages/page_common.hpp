#pragma once
// =============================================================================
//  native/pages/page_common.hpp
//  各页面共用的几个小组件。
//
//  为什么要抽出来：页头、错误条、说明条在每个页面都要用，各自写一份的结果是
//  改一处边距要改五个文件，而且很快就会不一致（登录页 16、统计页 20 这种）。
//  放一起之后，整个应用的「页头高度」「错误条圆角」只有一个定义。
// =============================================================================

#include <functional>
#include <string>

#include "app/app.hpp"
#include "ui/controls.hpp"
// 页面里到处要用 Renderer& 做文本测量，所以在这里一次性带上。
// 只前向声明的话，每个页面的 .cpp 都得自己再 include 一遍，漏一个就是一堆
// 「使用了未定义类型 Renderer」——而且报错位置会指向页面的成员函数，
// 看不出真正缺的是哪一行。
#include "ui/renderer.hpp"

namespace penhu::native {

/// 页面四周留白。宽屏上要给足，否则内容贴着窗口边会显得局促；
/// 窄窗口上则要省着用像素。
inline ui::Insets page_padding(const App& app) {
    return app.wide() ? ui::Insets{32.0f, 20.0f, 32.0f, 32.0f}
                      : ui::Insets{20.0f, 12.0f, 20.0f, 24.0f};
}


/// 页面顶部的一行小字说明，插进内容列的最上面。
///
/// 这里原来是一个 64dp 的 PageHeader（标题 + 副标题 + 右侧账号 / 退出登录）。
/// 它被撤掉了，原因是应用加了自定义顶栏之后，页头只是把同样的信息再写一遍：
/// 顶栏左边已经写着当前页名，右边已经放着用户名和退出登录 ——
/// 再占 64dp 重复一次，是纯浪费。
///
/// 但副标题里的内容不全都是废话：统计口径（「共 100 条记录」）、截图张数、
/// 「正在编辑哪一笔」—— 这些会直接影响用户怎么理解眼前的数据，不能跟着页头
/// 一起丢掉。所以它们下沉成这一行。
ui::Label* info_line(ui::VBox& col, const ui::Theme& th, const std::wstring& text);

/// 错误条：错误容器色底 + 说明文字
class ErrorBanner : public ui::Widget {
public:
    std::wstring text;

    float preferred_height(float width, ui::Renderer& r) override;
    void  layout(const ui::Rect& area, ui::Renderer&) override { bounds_ = area; }
    void  paint(ui::Renderer& r, const ui::Theme& th) override;
};

/// 说明条：中性信息（口径说明、数据不足的提醒）。
/// 和错误条分开是因为「样本不够」不是错误，用红色会把用户引向错误的理解。
class InfoNote : public ui::Widget {
public:
    std::wstring text;
    bool         warn{false};    // true 用 tertiary/warn 色，false 用 surface 容器色

    float preferred_height(float width, ui::Renderer& r) override;
    void  layout(const ui::Rect& area, ui::Renderer&) override { bounds_ = area; }
    void  paint(ui::Renderer& r, const ui::Theme& th) override;
};

/// 小节标题：左侧标题 + 右侧可选说明
class SectionTitle : public ui::Widget {
public:
    std::wstring title;
    std::wstring trailing;

    float preferred_height(float, ui::Renderer&) override { return 28.0f; }
    void  layout(const ui::Rect& area, ui::Renderer&) override { bounds_ = area; }
    void  paint(ui::Renderer& r, const ui::Theme& th) override;
};

/// 把长路径压成一行：保留盘符 + 末尾两段，中间用 … 代替。
/// 完整路径（C:\Users\<你>\Pictures\账本）在窄栏里会折成三行，
/// 而用户认目录靠的就是末尾那两段。
std::wstring short_path(const std::wstring& path);

/// 「标签 + 值」的一格（统计页的总览数字用）
class StatCell : public ui::Widget {
public:
    std::wstring label;
    std::wstring value;
    ui::Color    value_color{0x00000000u};   // 未设置 → on_surface

    float preferred_height(float, ui::Renderer&) override { return 58.0f; }
    void  layout(const ui::Rect& area, ui::Renderer&) override { bounds_ = area; }
    void  paint(ui::Renderer& r, const ui::Theme& th) override;
};

}  // namespace penhu::native
