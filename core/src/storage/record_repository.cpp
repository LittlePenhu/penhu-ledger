#include "penhu/storage/record_repository.hpp"

#include <algorithm>
#include <string>
#include <variant>
#include <vector>

#include "penhu/domain/date.hpp"

namespace penhu::storage {
namespace {

constexpr const char* kColumns =
    "id, amount_minor, direction, category_id, date_key, note, created_at, updated_at";

/// 待绑定参数。用 variant 是为了让「拼接 SQL 片段」和「绑定值」两个动作解耦：
/// SQL 片段永远是代码里的字面量，值永远是参数。
using BoundValue = std::variant<int64_t, std::string>;

/// 把参数按顺序绑定到 ?1 ?2 ...
Status bind_all(Statement& stmt, const std::vector<BoundValue>& params) {
    int index = 1;
    for (const auto& v : params) {
        if (std::holds_alternative<int64_t>(v)) {
            PENHU_RETURN_IF_ERROR(stmt.bind(index, std::get<int64_t>(v)));
        } else {
            PENHU_RETURN_IF_ERROR(stmt.bind(index, std::get<std::string>(v)));
        }
        ++index;
    }
    return status_ok();
}

/// 根据 RecordQuery 构造 WHERE 子句与参数列表。
/// 所有片段都是硬编码的，用户输入只进 params。
struct WhereClause {
    std::string sql;
    std::vector<BoundValue> params;
};

WhereClause build_where(const RecordQuery& q) {
    WhereClause out;
    out.sql = " WHERE 1=1";

    if (q.range.has_value()) {
        out.sql += " AND date_key >= ? AND date_key <= ?";
        out.params.emplace_back(q.range->from.to_epoch_days());
        out.params.emplace_back(q.range->to.to_epoch_days());
    }
    if (q.direction.has_value()) {
        out.sql += " AND direction = ?";
        out.params.emplace_back(static_cast<int64_t>(q.direction.value()));
    }
    if (q.category_id.has_value() && !q.category_id->empty()) {
        out.sql += " AND category_id = ?";
        out.params.emplace_back(q.category_id.value());
    }
    if (q.note_keyword.has_value() && !q.note_keyword->empty()) {
        // LIKE 的通配符要转义，否则用户输入一个 % 就会匹配全部记录。
        // ESCAPE '\' 让转义生效。
        std::string pattern;
        pattern.reserve(q.note_keyword->size() + 4);
        pattern.push_back('%');
        for (char c : q.note_keyword.value()) {
            if (c == '%' || c == '_' || c == '\\') pattern.push_back('\\');
            pattern.push_back(c);
        }
        pattern.push_back('%');
        out.sql += " AND note LIKE ? ESCAPE '\\'";
        out.params.emplace_back(std::move(pattern));
    }
    if (q.min_minor.has_value()) {
        out.sql += " AND amount_minor >= ?";
        out.params.emplace_back(q.min_minor.value());
    }
    if (q.max_minor.has_value()) {
        out.sql += " AND amount_minor <= ?";
        out.params.emplace_back(q.max_minor.value());
    }
    return out;
}

int clamp_limit(int limit) {
    if (limit <= 0) return 50;
    return std::min(limit, 1000);
}
int clamp_offset(int offset) { return std::max(offset, 0); }

}  // namespace

Record RecordRepository::row_to_record(Statement& s) {
    Record r;
    r.id         = s.column_text(0);
    r.amount     = Money::from_minor(s.column_int64(1));
    r.direction  = s.column_int(2) == 1 ? Direction::Income : Direction::Expense;
    r.category_id = s.column_text(3);
    r.date       = Date::from_epoch_days(s.column_int64(4));
    r.note       = s.column_text(5);
    r.created_at = s.column_int64(6);
    r.updated_at = s.column_int64(7);
    return r;
}

Result<Record> RecordRepository::insert(const Record& record) {
    PENHU_RETURN_IF_ERROR(record.is_valid()
                              ? status_ok()
                              : status_err(ErrorCode::InvalidArgument,
                                           "记录不完整（缺少 id/分类/日期，或金额非正）",
                                           "RecordRepository::insert"));

    PENHU_ASSIGN_OR_RETURN(stmt, db_.prepare(
        "INSERT INTO records (id, amount_minor, direction, category_id, date_key, note, "
        "created_at, updated_at) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8);"));

    PENHU_RETURN_IF_ERROR(stmt.bind(1, record.id));
    PENHU_RETURN_IF_ERROR(stmt.bind(2, record.amount.minor_units()));
    PENHU_RETURN_IF_ERROR(stmt.bind(3, static_cast<int>(record.direction)));
    PENHU_RETURN_IF_ERROR(stmt.bind(4, record.category_id));
    PENHU_RETURN_IF_ERROR(stmt.bind(5, record.date.to_epoch_days()));
    PENHU_RETURN_IF_ERROR(stmt.bind(6, record.note));
    PENHU_RETURN_IF_ERROR(stmt.bind(7, record.created_at));
    PENHU_RETURN_IF_ERROR(stmt.bind(8, record.updated_at));

    auto step = stmt.step();
    if (step.is_err()) return step.error();
    return Result<Record>(record);
}

Status RecordRepository::update(const Record& record) {
    if (!record.is_valid()) {
        // 逐项报出到底哪里不对。原来只有一句「记录不完整」，
        // 排查时会把人往「数据坏了」的方向带 —— 而实测那次真正的毛病
        // 是校验器要求了一个表里根本不存在的列。
        std::string why = "记录不完整（";
        if (record.id.empty()) why += "缺少 id; ";
        if (record.category_id.empty()) why += "缺少分类; ";
        if (record.amount.minor_units() <= 0) why += "金额必须大于 0; ";
        if (!record.date.is_valid()) why += "日期无效; ";
        why += "）";
        return status_err(ErrorCode::InvalidArgument, why, "RecordRepository::update");
    }

    PENHU_ASSIGN_OR_RETURN(stmt, db_.prepare(
        "UPDATE records SET amount_minor = ?1, direction = ?2, category_id = ?3, "
        "date_key = ?4, note = ?5, updated_at = ?6 WHERE id = ?7;"));
    PENHU_RETURN_IF_ERROR(stmt.bind(1, record.amount.minor_units()));
    PENHU_RETURN_IF_ERROR(stmt.bind(2, static_cast<int>(record.direction)));
    PENHU_RETURN_IF_ERROR(stmt.bind(3, record.category_id));
    PENHU_RETURN_IF_ERROR(stmt.bind(4, record.date.to_epoch_days()));
    PENHU_RETURN_IF_ERROR(stmt.bind(5, record.note));
    PENHU_RETURN_IF_ERROR(stmt.bind(6, record.updated_at));
    PENHU_RETURN_IF_ERROR(stmt.bind(7, record.id));

    auto st = stmt.step();
    if (st.is_err()) return to_status(st);
    if (db_.changes() == 0) {
        return status_err(ErrorCode::NotFound, "记录不存在: " + record.id,
                          "RecordRepository::update");
    }
    return status_ok();
}

Status RecordRepository::remove(const std::string& record_id) {
    PENHU_ASSIGN_OR_RETURN(stmt, db_.prepare("DELETE FROM records WHERE id = ?1;"));
    PENHU_RETURN_IF_ERROR(stmt.bind(1, record_id));

    auto st = stmt.step();
    if (st.is_err()) return to_status(st);
    if (db_.changes() == 0) {
        return status_err(ErrorCode::NotFound, "记录不存在: " + record_id,
                          "RecordRepository::remove");
    }
    return status_ok();
}

Result<Record> RecordRepository::find_by_id(const std::string& record_id) {
    PENHU_ASSIGN_OR_RETURN(stmt, db_.prepare(
        std::string("SELECT ") + kColumns + " FROM records WHERE id = ?1 LIMIT 1;"));
    PENHU_RETURN_IF_ERROR(stmt.bind(1, record_id));

    auto step = stmt.step();
    if (step.is_err()) return step.error();
    if (step.value() == StepResult::Done) {
        return Result<Record>::fail(ErrorCode::NotFound, "记录不存在: " + record_id,
                                    "RecordRepository::find_by_id");
    }
    return Result<Record>(row_to_record(stmt));
}

Result<PagedRecords> RecordRepository::query(const RecordQuery& q) {
    const WhereClause where = build_where(q);

    // --- 先取总数（分页控件需要它，且不能受 limit 影响）---
    PagedRecords out;
    {
        PENHU_ASSIGN_OR_RETURN(stmt, db_.prepare("SELECT count(*) FROM records" + where.sql + ";"));
        PENHU_RETURN_IF_ERROR(bind_all(stmt, where.params));
        auto step = stmt.step();
        if (step.is_err()) return step.error();
        if (step.value() != StepResult::Row) {
            return Result<PagedRecords>::fail(ErrorCode::StorageFailure, "统计总条数失败",
                                              "RecordRepository::query");
        }
        out.total = stmt.column_int64(0);
    }

    // --- 再取当页数据 ---
    const int limit = clamp_limit(q.limit);
    const int offset = clamp_offset(q.offset);
    const std::string order = q.ascending ? " ORDER BY date_key ASC, created_at ASC"
                                          : " ORDER BY date_key DESC, created_at DESC";
    const std::string sql =
        std::string("SELECT ") + kColumns + " FROM records" + where.sql + order +
        " LIMIT ? OFFSET ?;";

    PENHU_ASSIGN_OR_RETURN(stmt, db_.prepare(sql));
    auto params = where.params;
    params.emplace_back(static_cast<int64_t>(limit));
    params.emplace_back(static_cast<int64_t>(offset));
    PENHU_RETURN_IF_ERROR(bind_all(stmt, params));

    auto st = stmt.each_row([&out](Statement& s) -> Status {
        out.items.push_back(row_to_record(s));
        return status_ok();
    });
    if (st.is_err()) return st.error();

    return Result<PagedRecords>(std::move(out));
}

Result<std::vector<Record>> RecordRepository::fetch_range(const DateRange& range,
                                                          std::optional<Direction> direction) {
    std::string sql = std::string("SELECT ") + kColumns +
                      " FROM records WHERE date_key >= ? AND date_key <= ?";
    if (direction.has_value()) sql += " AND direction = ?";
    sql += " ORDER BY date_key ASC, created_at ASC;";

    PENHU_ASSIGN_OR_RETURN(stmt, db_.prepare(sql));

    int idx = 1;
    PENHU_RETURN_IF_ERROR(stmt.bind(idx++, range.from.to_epoch_days()));
    PENHU_RETURN_IF_ERROR(stmt.bind(idx++, range.to.to_epoch_days()));
    if (direction.has_value()) {
        PENHU_RETURN_IF_ERROR(stmt.bind(idx++, static_cast<int>(direction.value())));
    }

    std::vector<Record> records;
    auto st = stmt.each_row([&records](Statement& s) -> Status {
        records.push_back(row_to_record(s));
        return status_ok();
    });
    if (st.is_err()) return st.error();
    return Result<std::vector<Record>>(std::move(records));
}

Result<std::vector<Record>> RecordRepository::fetch_all(std::optional<Direction> direction) {
    std::string sql = std::string("SELECT ") + kColumns + " FROM records WHERE 1=1";
    if (direction.has_value()) sql += " AND direction = ?";
    sql += " ORDER BY date_key ASC, created_at ASC;";

    PENHU_ASSIGN_OR_RETURN(stmt, db_.prepare(sql));
    if (direction.has_value()) {
        PENHU_RETURN_IF_ERROR(stmt.bind(1, static_cast<int>(direction.value())));
    }

    std::vector<Record> records;
    auto st = stmt.each_row([&records](Statement& s) -> Status {
        records.push_back(row_to_record(s));
        return status_ok();
    });
    if (st.is_err()) return st.error();
    return Result<std::vector<Record>>(std::move(records));
}

Result<std::optional<Date>> RecordRepository::earliest_date() {
    PENHU_ASSIGN_OR_RETURN(stmt, db_.prepare("SELECT min(date_key) FROM records;"));
    auto step = stmt.step();
    if (step.is_err()) return step.error();
    if (step.value() != StepResult::Row || stmt.column_is_null(0)) {
        return Result<std::optional<Date>>(std::optional<Date>{});
    }
    return Result<std::optional<Date>>(std::optional<Date>{Date::from_epoch_days(stmt.column_int64(0))});
}

Result<std::optional<Date>> RecordRepository::latest_date() {
    PENHU_ASSIGN_OR_RETURN(stmt, db_.prepare("SELECT max(date_key) FROM records;"));
    auto step = stmt.step();
    if (step.is_err()) return step.error();
    if (step.value() != StepResult::Row || stmt.column_is_null(0)) {
        return Result<std::optional<Date>>(std::optional<Date>{});
    }
    return Result<std::optional<Date>>(std::optional<Date>{Date::from_epoch_days(stmt.column_int64(0))});
}

int64_t RecordRepository::count() {
    auto stmt = db_.prepare("SELECT count(*) FROM records;");
    if (stmt.is_err()) return 0;
    auto step = stmt.value().step();
    if (step.is_err() || step.value() != StepResult::Row) return 0;
    return stmt.value().column_int64(0);
}

bool RecordRepository::category_in_use(const std::string& category_id) {
    auto stmt = db_.prepare("SELECT count(*) FROM records WHERE category_id = ?1 LIMIT 1;");
    if (stmt.is_err()) return true;
    if (stmt.value().bind(1, category_id).is_err()) return true;
    auto step = stmt.value().step();
    if (step.is_err() || step.value() != StepResult::Row) return true;
    return stmt.value().column_int64(0) > 0;
}

}  // namespace penhu::storage
