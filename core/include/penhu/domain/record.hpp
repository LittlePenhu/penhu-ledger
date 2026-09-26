#pragma once
// =============================================================================
//  penhu/domain/record.hpp
//  记账记录与查询条件。
// =============================================================================

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include "penhu/domain/money.hpp"
#include "penhu/domain/date.hpp"

namespace penhu {

/// 资金方向。花销统计以支出为主体，但收入要留位，
/// 否则「本月结余」这类指标做不出来。
enum class Direction {
    Expense = 0,   // 支出
    Income  = 1    // 收入
};

const char* to_string(Direction d) noexcept;
std::optional<Direction> parse_direction(std::string_view text) noexcept;
/// "支出" / "收入"
const char* direction_label_zh(Direction d) noexcept;

/// 一笔账
struct Record {
    std::string id;                    // compact uuid
    std::string user_id;               // compact uuid，冗余存一份便于库内自检
    Money       amount;                // 恒为正，方向看 direction
    Direction   direction{Direction::Expense};
    std::string category_id;           // compact uuid
    Date        date;                  // 账发生的本地日历日
    std::string note;                  // 备注，可空
    int64_t     created_at{0};         // UTC epoch 秒
    int64_t     updated_at{0};

    bool is_valid() const noexcept;
};

/// 新增/修改记录的入参（不含 id 与时间戳，由服务层填）
struct RecordInput {
    Money       amount;
    Direction   direction{Direction::Expense};
    std::string category_id;
    Date        date;
    std::string note;

    /// 语义校验：金额必须为正、日期必须有效、分类不能为空。
    /// 这里不校验「分类是否属于该用户」——那需要查库，属于服务层职责。
    Status validate() const;
};

/// 查询条件。全部可选，未设置即不过滤。
struct RecordQuery {
    std::optional<DateRange>  range;        // 按日期区间
    std::optional<Direction>  direction;    // 只看支出 / 只看收入
    std::optional<std::string> category_id; // 限定分类
    std::optional<std::string> note_keyword; // 备注模糊匹配（大小写不敏感）
    std::optional<int64_t>    min_minor;    // 金额下限（分）
    std::optional<int64_t>    max_minor;    // 金额上限（分）

    int  limit{200};    // 默认 200，防前端一次拉爆
    int  offset{0};

    /// 排序：默认按日期倒序 + 创建时间倒序（同日新记的在上）
    bool ascending{false};
};

/// 分页结果
struct PagedRecords {
    std::vector<Record> items;
    int64_t total{0};    // 满足条件的总条数（不受 limit/offset 影响）
};

/// 当日（或任意区间）汇总
struct CategoryBreakdown {
    std::string category_id;
    std::string category_name;      // 由服务层 join 填好，方便直接渲染
    std::string category_color;
    Direction   direction{Direction::Expense};
    int64_t     total_minor{0};
    int64_t     record_count{0};
    double      share{0.0};         // 占同方向总额的比例 [0,1]
};

struct Summary {
    DateRange   range;
    int64_t     expense_minor{0};
    int64_t     income_minor{0};
    int64_t     net_minor{0};                  // income - expense
    int64_t     expense_records{0};
    int64_t     income_records{0};
    int64_t     active_days{0};                // 有记账的天数
    Money       avg_daily_expense_minor{Money::from_minor(0)};  // 见 .cpp 里的取整说明
    Money       max_expense_minor{Money::from_minor(0)};        // 单笔最大支出
    std::string max_expense_date;              // 单笔最大支出的日期
    std::vector<CategoryBreakdown> by_category;
};

}  // namespace penhu
