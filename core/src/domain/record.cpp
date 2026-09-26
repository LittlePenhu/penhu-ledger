#include "penhu/domain/record.hpp"

#include <algorithm>

namespace penhu {

const char* to_string(Direction d) noexcept {
    switch (d) {
        case Direction::Expense: return "expense";
        case Direction::Income:  return "income";
    }
    return "expense";
}

std::optional<Direction> parse_direction(std::string_view text) noexcept {
    if (text == "expense" || text == "支出" || text == "0") return Direction::Expense;
    if (text == "income"  || text == "收入" || text == "1") return Direction::Income;
    return std::nullopt;
}

const char* direction_label_zh(Direction d) noexcept {
    return d == Direction::Expense ? "支出" : "收入";
}

bool Record::is_valid() const noexcept {
    // 刻意**不检查** user_id：records 表压根没有这一列。
    // 「一个用户一个独立加密库（data/<uuid>.db）」这个设计本身就把归属钉死了，
    // 再在每一行上存一份 user_id 是冗余的；而且 meta.owner_user_id 在开库时
    // 会校验一次，那才是真正的边界。
    // 之前这里要求 user_id 非空，后果是：从库里读出来的记录 user_id 恒为空，
    // 于是 update() 一律报「记录不完整」，而且报错完全指向错误的方向。
    if (id.empty() || category_id.empty()) return false;
    if (amount.minor_units() <= 0) return false;   // 方向由 direction 表达，金额恒为正
    if (!date.is_valid()) return false;
    return true;
}

Status RecordInput::validate() const {
    if (amount.minor_units() <= 0) {
        return status_err(ErrorCode::InvalidArgument,
                          "金额必须大于 0（支出还是收入由方向字段决定，不要用负号）",
                          "RecordInput::validate");
    }
    // 单笔上限 1 亿：不是业务规则，是为了拦住「把日期当金额输进去」这类错误
    constexpr int64_t kMaxSingleMinor = 100'000'000LL * 100LL;
    if (amount.minor_units() > kMaxSingleMinor) {
        return status_err(ErrorCode::InvalidArgument,
                          "单笔金额超过 1 亿元，请检查是否输错", "RecordInput::validate");
    }
    if (!date.is_valid()) {
        return status_err(ErrorCode::InvalidArgument, "日期无效", "RecordInput::validate");
    }
    if (category_id.empty()) {
        return status_err(ErrorCode::InvalidArgument, "必须指定分类", "RecordInput::validate");
    }
    constexpr size_t kMaxNoteLength = 500;
    if (note.size() > kMaxNoteLength) {
        return status_err(ErrorCode::InvalidArgument,
                          "备注超过 " + std::to_string(kMaxNoteLength) + " 字",
                          "RecordInput::validate");
    }
    return status_ok();
}

}  // namespace penhu
