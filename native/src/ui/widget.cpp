// =============================================================================
//  native/ui/widget.cpp
// =============================================================================

#include "ui/widget.hpp"

#include <algorithm>
#include <cmath>

#include "ui/renderer.hpp"

namespace penhu::native::ui {

namespace {
// ---- 滚动条尺寸。视觉与命中范围分开：画得细好看，抓得宽好用。 ----
constexpr float kScrollBarWidth    = 5.0f;    // 滑块视觉宽度
constexpr float kScrollBarInset    = 3.0f;    // 距右边缘
constexpr float kScrollBarHitWidth = 18.0f;   // 可抓取宽度（鼠标/手指都够得着）
constexpr float kScrollThumbMin    = 40.0f;   // 滑块最短长度，太短会抓不住
}  // namespace

// -----------------------------------------------------------------------------
//  Widget 基类
// -----------------------------------------------------------------------------

Widget* Widget::add(std::unique_ptr<Widget> child) {
    child->parent_ = this;
    Widget* raw = child.get();
    children_.push_back(std::move(child));
    return raw;
}

void Widget::clear_children() {
    children_.clear();
}

float Widget::preferred_height(float, Renderer&) { return 0.0f; }

bool Widget::tick(float dt_ms) {
    bool dirty = false;
    // 用 |= 而不是短路：所有子节点都要被推进，不能因为前面有变化就跳过后面的
    for (auto& c : children_) dirty = c->tick(dt_ms) || dirty;
    return dirty;
}

void Widget::layout(const Rect& area, Renderer& r) {
    bounds_ = area;
    // 默认行为：子节点都占满同一矩形（层叠语义）。
    // 这样「只画自己、不关心子节点排列」的复合组件不用重写 layout。
    for (auto& c : children_) {
        if (c->visible) c->layout(area, r);
    }
}

bool Widget::on_pointer_move(const Point&, bool) { return false; }
bool Widget::on_pointer_down(const Point&) { return false; }
bool Widget::on_pointer_up(const Point&) { return false; }
bool Widget::on_wheel(const Point&, float) { return false; }
bool Widget::on_key(unsigned, bool) { return false; }
bool Widget::on_text(wchar_t) { return false; }

Widget* Widget::hit_test(const Point& p) {
    // 纯视觉层一进入就不参与命中：点它等于点它下面的东西。
    // 通知就该只是通知 —— 挡住用户正在做的事是本末倒置。
    if (!visible || pointer_transparent) return nullptr;
    if (!bounds_.contains(p)) return nullptr;
    // 逆序：后加的子节点画在上层，命中也要优先
    for (auto it = children_.rbegin(); it != children_.rend(); ++it) {
        if (Widget* hit = (*it)->hit_test(p)) return hit;
    }
    return this;
}

void Widget::collect_focusables(std::vector<Widget*>& out) {
    if (!visible) return;
    if (focusable && enabled) out.push_back(this);
    for (auto& c : children_) c->collect_focusables(out);
}

// -----------------------------------------------------------------------------
//  VBox
// -----------------------------------------------------------------------------

float VBox::preferred_height(float width, Renderer& r) {
    const float inner_w = std::max(0.0f, width - padding.horizontal());
    float total = padding.vertical();
    int count = 0;
    for (auto& c : children_) {
        if (!c->visible) continue;
        if (count > 0) total += gap;
        ++count;
        // flex 子节点的高度由父容器决定，这里计 0 不代表「不要高度」，
        // 而是「不参与固有高度」。调用方（一般是 ScrollView）会另外处理。
        if (c->flex <= 0.0f) total += c->preferred_height(inner_w, r);
    }
    return total;
}

void VBox::layout(const Rect& area, Renderer& r) {
    bounds_ = area;
    const Rect in = area.deflate(padding);
    const float inner_w = std::max(0.0f, in.w);

    std::vector<float> heights(children_.size(), 0.0f);
    float used = 0.0f;
    float flex_sum = 0.0f;
    int   count = 0;

    // 限宽子节点的实际可用宽度：测量与定位必须用同一个值，
    // 否则文字按全宽换行、却被放进窄盒子里，行数会算错。
    auto width_for = [&](const Widget* c) {
        if (c->max_width > 0.0f && inner_w > c->max_width) return c->max_width;
        return inner_w;
    };

    for (size_t i = 0; i < children_.size(); ++i) {
        Widget* c = children_[i].get();
        if (!c->visible) continue;
        ++count;
        if (c->flex > 0.0f) {
            flex_sum += c->flex;
        } else {
            heights[i] = c->preferred_height(width_for(c), r);
            used += heights[i];
        }
    }

    const float gaps = gap * static_cast<float>(std::max(0, count - 1));
    const float remaining = std::max(0.0f, in.h - used - gaps);
    if (flex_sum > 0.0f) {
        for (size_t i = 0; i < children_.size(); ++i) {
            if (children_[i]->visible && children_[i]->flex > 0.0f) {
                heights[i] = remaining * (children_[i]->flex / flex_sum);
            }
        }
    }

    float y = in.y;
    for (size_t i = 0; i < children_.size(); ++i) {
        Widget* c = children_[i].get();
        if (!c->visible) continue;
        const float cw = width_for(c);
        const float cx = in.x + (inner_w - cw) * 0.5f;
        c->layout(Rect{cx, y, cw, heights[i]}, r);
        y += heights[i] + gap;
    }
}

void VBox::paint(Renderer& r, const Theme& th) {
    for (auto& c : children_) {
        if (c->visible) c->paint(r, th);
    }
}

// -----------------------------------------------------------------------------
//  HBox
// -----------------------------------------------------------------------------

float HBox::preferred_height(float width, Renderer& r) {
    const float inner_w = std::max(0.0f, width - padding.horizontal());
    int visible_count = 0;
    for (auto& c : children_) if (c->visible) ++visible_count;
    if (visible_count == 0) return padding.vertical();

    const float gaps = gap * static_cast<float>(visible_count - 1);
    // 布局前只能估一个「平分宽度」来问子节点要高度。真正的宽度分配在 layout 里做。
    const float share = std::max(40.0f, (inner_w - gaps) / static_cast<float>(visible_count));

    float tallest = 0.0f;
    for (auto& c : children_) {
        if (!c->visible) continue;
        tallest = std::max(tallest, c->preferred_height(share, r));
    }
    return padding.vertical() + tallest;
}

void HBox::layout(const Rect& area, Renderer& r) {
    bounds_ = area;
    const Rect in = area.deflate(padding);
    const float inner_w = std::max(0.0f, in.w);
    const float inner_h = std::max(0.0f, in.h);

    int visible_count = 0;
    for (auto& c : children_) if (c->visible) ++visible_count;
    if (visible_count == 0) return;

    const float gaps = gap * static_cast<float>(visible_count - 1);
    const float share = std::max(0.0f, (inner_w - gaps) / static_cast<float>(visible_count));

    std::vector<float> widths(children_.size(), 0.0f);
    float flex_sum = 0.0f;
    float tallest = 0.0f;
    float fixed_total = 0.0f;

    for (size_t i = 0; i < children_.size(); ++i) {
        Widget* c = children_[i].get();
        if (!c->visible) continue;
        if (c->flex > 0.0f) {
            flex_sum += c->flex;
        } else if (c->width_hint > 0.0f && c->width_hint <= inner_w) {
            // 明确指定了宽度（工具条按钮），不参与平分
            widths[i] = c->width_hint;
            fixed_total += c->width_hint;
            tallest = std::max(tallest, c->preferred_height(c->width_hint, r));
        } else {
            widths[i] = share;
            fixed_total += share;
            tallest = std::max(tallest, c->preferred_height(share, r));
        }
    }

    if (flex_sum > 0.0f) {
        const float flex_space = std::max(0.0f, inner_w - gaps - fixed_total);
        for (size_t i = 0; i < children_.size(); ++i) {
            Widget* c = children_[i].get();
            if (c->visible && c->flex > 0.0f) widths[i] = flex_space * (c->flex / flex_sum);
        }
    }

    // Stretch 之外的对齐方式意味着「容器不跟着子节点长高」，按内容高度收窄
    const float child_h = (align == Align::Stretch || tallest <= 0.0f)
                              ? inner_h
                              : std::min(tallest, inner_h);

    float x = in.x;
    for (size_t i = 0; i < children_.size(); ++i) {
        Widget* c = children_[i].get();
        if (!c->visible) continue;
        float y = in.y;
        if (align == Align::Center) y = in.y + (inner_h - child_h) * 0.5f;
        else if (align == Align::End) y = in.bottom() - child_h;
        c->layout(Rect{x, y, widths[i], child_h}, r);
        x += widths[i] + gap;
    }
}

void HBox::paint(Renderer& r, const Theme& th) {
    for (auto& c : children_) {
        if (c->visible) c->paint(r, th);
    }
}

// -----------------------------------------------------------------------------
//  Grid
// -----------------------------------------------------------------------------

int Grid::resolve_columns(float inner_width) const {
    if (columns > 0) return std::max(1, columns);
    const float unit = std::max(1.0f, min_cell_width + gap_x);
    return std::max(1, static_cast<int>((inner_width + gap_x) / unit));
}

float Grid::preferred_height(float width, Renderer& r) {
    const float inner_w = std::max(0.0f, width - padding.horizontal());
    const int cols = resolve_columns(inner_w);
    const float cell_w = std::max(1.0f, (inner_w - gap_x * static_cast<float>(cols - 1)) / static_cast<float>(cols));

    float ch = cell_height;
    if (ch <= 0.0f) {
        int n = 0;
        for (auto& c : children_) {
            if (!c->visible) continue;
            if (n >= cols) break;
            ++n;
            ch = std::max(ch, c->preferred_height(cell_w, r));
        }
    }

    int rows = 0;
    for (auto& c : children_) if (c->visible) ++rows;
    rows = (rows + cols - 1) / cols;
    if (rows == 0) return padding.vertical();
    return padding.vertical() + static_cast<float>(rows) * ch + gap_y * static_cast<float>(rows - 1);
}

void Grid::layout(const Rect& area, Renderer& r) {
    bounds_ = area;
    const Rect in = area.deflate(padding);
    const int cols = resolve_columns(in.w);
    const float cell_w = std::max(1.0f, (in.w - gap_x * static_cast<float>(cols - 1)) / static_cast<float>(cols));

    float ch = cell_height;
    if (ch <= 0.0f) {
        int n = 0;
        for (auto& c : children_) {
            if (!c->visible) continue;
            if (n >= cols) break;
            ++n;
            ch = std::max(ch, c->preferred_height(cell_w, r));
        }
    }

    int index = 0;
    for (auto& c : children_) {
        if (!c->visible) continue;
        const int col = index % cols;
        const int row = index / cols;
        const float x = in.x + static_cast<float>(col) * (cell_w + gap_x);
        const float y = in.y + static_cast<float>(row) * (ch + gap_y);
        c->layout(Rect{x, y, cell_w, ch}, r);
        ++index;
    }
}

void Grid::paint(Renderer& r, const Theme& th) {
    for (auto& c : children_) {
        if (c->visible) c->paint(r, th);
    }
}

// -----------------------------------------------------------------------------
//  Stack
// -----------------------------------------------------------------------------

float Stack::preferred_height(float width, Renderer& r) {
    float tallest = 0.0f;
    for (auto& c : children_) {
        if (!c->visible) continue;
        tallest = std::max(tallest, c->preferred_height(width, r));
    }
    return padding.vertical() + tallest;
}

void Stack::layout(const Rect& area, Renderer& r) {
    bounds_ = area;
    for (auto& c : children_) {
        if (c->visible) c->layout(area, r);
    }
}

void Stack::paint(Renderer& r, const Theme& th) {
    for (auto& c : children_) {
        if (c->visible) c->paint(r, th);
    }
}

Widget* Stack::hit_test(const Point& p) {
    if (!visible || !bounds_.contains(p)) return nullptr;
    // 从上往下逐层问，**上层没命中就继续问下层**。
    //
    // 不要写成「只看最上层，没命中就返回 nullptr」—— 那会让浮层变成整屏的
    // 点击黑洞：提示条（Snackbar）一显示，最上层就是它，而它只占一小块，
    // 于是页面其它地方的点击统统落空，表现为「弹个提示之后整个界面点不动」。
    // 整屏不透明层不受影响：那种层任何坐标都命中，下层自然拿不到事件。
    for (auto it = children_.rbegin(); it != children_.rend(); ++it) {
        if (!(*it)->visible) continue;
        if ((*it)->pointer_transparent) continue;   // 纯视觉层，不参与命中
        if (Widget* hit = (*it)->hit_test(p)) return hit;
    }
    return this;
}

// -----------------------------------------------------------------------------
//  ScrollView
// -----------------------------------------------------------------------------

VBox* ScrollView::make_content() {
    auto c = std::make_unique<VBox>();
    VBox* raw = c.get();
    // 挂进 children_：这样基类的通用遍历（collect_focusables 等）能看见它。
    // 注意这里只是把它「登记」为子节点，实际的布局与绘制仍由下面几个重写方法
    // 显式控制（要处理限宽、偏移、裁剪），不走基类的顺序遍历。
    add(std::move(c));
    return raw;
}

ScrollView::ScrollView() : content(*make_content()) {
    // 拖滚动条不该抢走输入框的焦点（见 preserve_focus_on_press 的说明）
    preserve_focus_on_press = true;
}

float ScrollView::preferred_height(float, Renderer&) {
    // 视口高度由父容器决定（一般是 flex:1 撑满）。内容多高都不影响视口高度。
    return 0.0f;
}

float ScrollView::max_offset() const {
    return std::max(0.0f, content_height_ - bounds_.h);
}

void ScrollView::layout(const Rect& area, Renderer& r) {
    bounds_ = area;

    // 内容限宽并居中：设在 content 上的 max_width 必须在这里生效。
    // VBox::layout 只约束「它的直接子节点」，而 content 是 ScrollView 的直接子节点，
    // 所以如果把限宽交给 VBox，ScrollView 会把 content 铺满整行，限宽形同虚设
    // （第一版就是这样，截图里能看到卡片一直顶到屏幕右边缘）。
    const float limit = content.max_width;
    const float inner_w = (limit > 0.0f && area.w > limit) ? limit : area.w;
    const float inner_x = area.x + (area.w - inner_w) * 0.5f;

    content_height_ = content.preferred_height(inner_w, r);
    // 内容为空的容器给一个视口高度，免得全空时布局塌掉
    if (content_height_ < area.h) content_height_ = area.h;

    // 内容高度可能变了（列表增删、加载完成），两个偏移都要夹回合法区间：
    // 只夹 offset 的话，target 会停在界外，下一帧动画又把它拽出去。
    clamp_both();

    // 关键一步：把滚动偏移直接减进内容区的 y。
    // 子节点的绝对坐标因此天然是「屏幕上的真实位置」，
    // hit_test 不需要任何 transform 逆变换。
    content.layout(Rect{inner_x, area.y - offset, inner_w, content_height_}, r);
}

Rect ScrollView::thumb_rect() const {
    const float mo = max_offset();
    if (mo <= 0.5f || bounds_.h <= 0.0f) return {};
    const float ratio = std::min(1.0f, bounds_.h / std::max(1.0f, content_height_));
    const float thumb_h = std::max(kScrollThumbMin, bounds_.h * ratio);
    const float travel = std::max(0.0f, bounds_.h - thumb_h);
    const float t = offset / mo;
    return Rect{bounds_.right() - kScrollBarInset - kScrollBarWidth,
                bounds_.y + travel * clampf(t, 0.0f, 1.0f),
                kScrollBarWidth, thumb_h};
}

Rect ScrollView::bar_hit_rect() const {
    if (max_offset() <= 0.5f) return {};
    // 热区宽度独立于视觉宽度：视觉上细一点好看，命中范围给足才好抓。
    return Rect{bounds_.right() - kScrollBarHitWidth, bounds_.y,
                kScrollBarHitWidth, bounds_.h};
}

void ScrollView::apply_drag(float y) {
    const Rect thumb = thumb_rect();
    const float travel = std::max(0.0f, bounds_.h - thumb.h);
    if (travel <= 0.0f) return;
    // 光标位置减去抓取偏移 = 滑块顶端应有的位置，再换算成比例
    const float t = clampf((y - drag_grab_dy_ - bounds_.y) / travel, 0.0f, 1.0f);
    // 拖动**必须跟手**：offset 与 target 一起设，不能走动画，
    // 否则鼠标已经停了、内容还在追，感觉像橡皮筋。
    offset = t * max_offset();
    target_offset = offset;
}

bool ScrollView::on_pointer_down(const Point& p) {
    if (!enabled || max_offset() <= 0.5f) return false;
    if (!bar_hit_rect().contains(p)) return false;

    const Rect thumb = thumb_rect();
    dragging_ = true;
    if (thumb.contains(p)) {
        // 抓住滑块本身：记住抓在哪一点上，拖动时滑块跟着走而不跳
        drag_grab_dy_ = p.y - thumb.y;
    } else {
        // 点在轨道空白处：滑块中心跳到这里（这是滚动条的标准行为）
        drag_grab_dy_ = thumb.h * 0.5f;
        apply_drag(p.y);
    }
    return true;
}

bool ScrollView::on_pointer_move(const Point& p, bool) {
    if (!dragging_) return false;
    // 不检查 p 是否还在窗口内：拖动时鼠标移出滚动条甚至移出窗口是常事，
    // apply_drag 自己会把结果 clamp 到合法范围（不然「拖着拖着就断了」）。
    apply_drag(p.y);
    return true;
}

bool ScrollView::on_pointer_up(const Point&) {
    if (!dragging_) return false;
    dragging_ = false;
    return true;
}

void ScrollView::paint(Renderer& r, const Theme& th) {
    r.push_clip(bounds_);
    content.paint(r, th);
    r.pop_clip();

    if (!show_bar) return;
    const Rect thumb = thumb_rect();
    if (thumb.w <= 0.0f) return;
    // 拖动中把滑块画得实一点：这是「我抓住了它」的即时反馈
    r.fill_round(thumb, thumb.w * 0.5f,
                 th.palette.outline.alpha_of(dragging_ ? 0.85f : 0.45f));
}

bool ScrollView::tick(float dt_ms) {
    bool dirty = Widget::tick(dt_ms);   // 子节点可能也在动

    const float diff = target_offset - offset;
    if (std::fabs(diff) < 0.4f) {
        // 收尾：抹掉残差，保证动画真的会停（否则永远差一点点，白重绘）
        if (offset != target_offset) {
            offset = target_offset;
            dirty = true;
        }
        return dirty;
    }

    // 指数逼近，时间常数 70ms：约 150ms 走完 90%，滑动感自然又不停滞。
    // 用 exp 而不是「每帧固定百分比」是为了**与帧率无关** ——
    // 固定百分比在 60fps 和 30fps 下速度会差一倍。
    const float alpha = 1.0f - std::exp(-std::max(0.0f, dt_ms) / 70.0f);
    offset += diff * alpha;
    clamp_both();
    return true;
}

void ScrollView::scroll_to(float value, bool immediate) {
    // 这里**只保证非负，不做上限夹取**。
    // 上限依赖 max_offset()，而它要等 layout 之后才知道（content_height_ 和
    // bounds_ 都还没算）。如果在重建后的立刻调用就把值夹住，得到的上限是 0，
    // 于是「恢复滚动位置」被静默夹成 0 —— 页面照样弹回顶部，而且看不出一丝异常。
    // 真正的上限交给 layout 里的 clamp_both。
    target_offset = std::max(0.0f, value);
    if (immediate) offset = target_offset;
}

void ScrollView::clamp_both() {
    // 还没 layout 过（没有有效视口/内容高度）时不夹：
    // 此时 max_offset() 恒为 0，一夹就把刚设好的目标抹平
    if (bounds_.h <= 0.0f) return;
    const float mo = max_offset();
    target_offset = clampf(target_offset, 0.0f, mo);
    offset = clampf(offset, 0.0f, mo);
}

bool ScrollView::on_wheel(const Point&, float delta) {
    if (max_offset() <= 0.0f) return false;
    // 只改目标位置，实际偏移交给每帧的 tick 去追 —— 这样才是滑动而不是跳格。
    // 滚轮向前（delta>0）对应内容下移，即偏移减小。
    target_offset = clampf(target_offset - delta * wheel_step, 0.0f, max_offset());
    return true;
}

Widget* ScrollView::hit_test(const Point& p) {
    if (!visible || !bounds_.contains(p)) return nullptr;
    if (Widget* hit = content.hit_test(p)) return hit;
    return this;
}

void ScrollView::ensure_visible(const Widget* child) {
    if (child == nullptr) return;
    const float top = child->bounds().y;
    const float bottom = child->bounds().bottom();
    const float view_top = bounds_.y;
    const float view_bottom = bounds_.bottom();
    // 目标位置而不是当前位置：调用方想看到的是「最终滚到那里」，
    // 中途的动画交给 tick（切进编辑态时这会让定位显得自然）
    if (top < view_top) {
        scroll_to(target_offset - (view_top - top) - 8.0f);
    } else if (bottom > view_bottom) {
        scroll_to(target_offset + (bottom - view_bottom) + 8.0f);
    }
}

}  // namespace penhu::native::ui
