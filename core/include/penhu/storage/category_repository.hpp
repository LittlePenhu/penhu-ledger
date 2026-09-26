#pragma once
// =============================================================================
//  penhu/storage/category_repository.hpp
//  分类访问层（每个用户的加密库内）。
// =============================================================================

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "penhu/domain/category.hpp"
#include "penhu/storage/database.hpp"

namespace penhu::storage {

class CategoryRepository {
public:
    explicit CategoryRepository(Database& db) : db_(db) {}

    /// 首次登录时种入预置分类。已存在的（同名同方向）跳过，所以可重复调用。
    /// 返回实际新建的条数。
    Result<int> seed_defaults();

    Result<std::vector<Category>> list(std::optional<Direction> direction = std::nullopt,
                                       bool include_inactive = false);

    Result<Category> find_by_id(const std::string& id);

    Result<Category> create(const std::string& name,
                            const std::string& icon,
                            const std::string& color_hex,
                            Direction direction,
                            int sort_order);

    Status update(const Category& category);

    /// 停用/启用。默认分类不允许删除，只能停用——历史记录还引用着它。
    Status set_active(const std::string& id, bool active);

    /// 删除。仅允许非预置分类，且必须没有任何记录引用它。
    Status remove(const std::string& id);

    /// 一次读全量并建索引，给「列表渲染 + 分类名回填」用，
    /// 避免渲染 200 条记录时查 200 次分类表（N+1）。
    Result<std::unordered_map<std::string, Category>> map_by_id();

    /// 校验某个分类存在、启用中、且方向匹配
    Status ensure_usable(const std::string& id, Direction direction);

    /// 该分类下是否还有记录（删分类前的检查）。
    /// 查不出来时返回 true —— 宁可拒绝删除，也不能留下悬空引用。
    bool category_in_use_by_records(const std::string& category_id);

    int64_t count();

private:
    static Category row_to_category(Statement& stmt);

    Database& db_;
};

}  // namespace penhu::storage
