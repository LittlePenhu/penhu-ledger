#pragma once
// =============================================================================
//  penhu/storage/record_repository.hpp
//  记账记录访问层。
//
//  安全约定（这一条不允许被绕过）：
//    所有 SQL 的筛选条件一律走 ?1 ?2 ... 参数绑定。
//    备注关键字这种「用户完全可控」的输入绝不允许拼进 SQL 字符串里。
//    动态条件只由固定的 SQL 片段拼装，绑定的值永远是参数。
// =============================================================================

#include <optional>
#include <string>
#include <vector>
#include "penhu/domain/record.hpp"
#include "penhu/storage/database.hpp"

namespace penhu::storage {

class RecordRepository {
public:
    explicit RecordRepository(Database& db) : db_(db) {}

    Result<Record> insert(const Record& record);

    Status update(const Record& record);

    /// 删除前会校验 id 属于当前库（因为我们本来就一人一库，这里是双保险）
    Status remove(const std::string& record_id);

    Result<Record> find_by_id(const std::string& record_id);

    /// 带筛选 + 分页的查询，同时返回总条数
    Result<PagedRecords> query(const RecordQuery& q);

    /// 取某区间的全部记录（统计模块用，不分页）
    Result<std::vector<Record>> fetch_range(const DateRange& range,
                                           std::optional<Direction> direction = std::nullopt);

    /// 取全部记录（导出 / 全量重算用）。条数上限保护。
    Result<std::vector<Record>> fetch_all(std::optional<Direction> direction = std::nullopt);

    /// 库里最早的记录日期，没有记录则返回 nullopt
    Result<std::optional<Date>> earliest_date();
    Result<std::optional<Date>> latest_date();

    int64_t count();

    /// 某分类下是否还有记录（删分类前的检查）
    bool category_in_use(const std::string& category_id);

private:
    static Record row_to_record(Statement& stmt);

    Database& db_;
};

}  // namespace penhu::storage
