#pragma once
// =============================================================================
//  penhu/domain/category.hpp
//  消费分类。支持系统预置 + 用户自定义。
// =============================================================================

#include <cstdint>
#include <string>
#include <vector>
#include "penhu/domain/record.hpp"
#include "penhu/util/result.hpp"

namespace penhu {

struct Category {
    std::string id;                 // compact uuid
    std::string user_id;            // compact uuid（预置分类也归属到具体用户，便于改名）
    std::string name;               // "餐饮"
    std::string icon;               // Material Symbols 图标名，前端直接用
    std::string color_hex;          // "#FF7043"，同时用于 M3 图表配色
    Direction   direction{Direction::Expense};
    int         sort_order{0};
    bool        is_builtin{false};  // 预置分类不允许删除，只允许改名/停用
    bool        is_active{true};

    bool is_valid() const noexcept;
};

/// 预置分类种子（支出 41 条 + 收入 11 条）。
///
/// 选取标准：覆盖「一个城市年轻人一年里真的会发生的开销」，粒度到
/// 「月底看占比能据此做决定」为止 —— 例如交通拆成 公交/打车/加油停车/火车机票，
/// 因为「交通占 31%」没法指导行动，而「打车占 18%」可以。
///
/// 补齐语义：`CategoryRepository::seed_defaults` 用 INSERT OR IGNORE + (name, direction)
/// 唯一索引，所以这份清单**可以在后续版本里增补**，老账号下次登录会自动补上
/// 缺的那些，既不会重复，也不会把用户改过名字的分类覆盖回去。
///
/// 颜色取自 Material Design 3 的 baseline 色板，保证和界面主题同源。
const std::vector<Category>& builtin_categories();

}  // namespace penhu
