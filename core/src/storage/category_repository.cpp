#include "penhu/storage/category_repository.hpp"

#include <string>
#include <vector>

#include "penhu/util/log.hpp"
#include "penhu/util/uuid.hpp"

namespace penhu::storage {
namespace {

constexpr const char* kColumns =
    "id, name, icon, color_hex, direction, sort_order, is_builtin, is_active";

}  // namespace

Category CategoryRepository::row_to_category(Statement& s) {
    Category c;
    c.id         = s.column_text(0);
    c.name       = s.column_text(1);
    c.icon       = s.column_text(2);
    c.color_hex  = s.column_text(3);
    c.direction  = s.column_int(4) == 1 ? Direction::Income : Direction::Expense;
    c.sort_order = s.column_int(5);
    c.is_builtin = s.column_bool(6);
    c.is_active  = s.column_bool(7);
    return c;
}

Result<int> CategoryRepository::seed_defaults() {
    int created = 0;

    for (const Category& proto : builtin_categories()) {
        // INSERT OR IGNORE 配合 (name, direction) 唯一索引：
        // 已有同名同方向的分类就跳过，所以这个函数可以随便重复调用，
        // 也允许用户先把「餐饮」改名后再登录而不会被覆盖回去。
        PENHU_ASSIGN_OR_RETURN(stmt, db_.prepare(
            "INSERT OR IGNORE INTO categories "
            "(id, name, icon, color_hex, direction, sort_order, is_builtin, is_active) "
            "VALUES (?1, ?2, ?3, ?4, ?5, ?6, 1, 1);"));

        const std::string id = uuid_to_compact(new_uuid());
        PENHU_RETURN_IF_ERROR(stmt.bind(1, id));
        PENHU_RETURN_IF_ERROR(stmt.bind(2, proto.name));
        PENHU_RETURN_IF_ERROR(stmt.bind(3, proto.icon));
        PENHU_RETURN_IF_ERROR(stmt.bind(4, proto.color_hex));
        PENHU_RETURN_IF_ERROR(stmt.bind(5, static_cast<int>(proto.direction)));
        PENHU_RETURN_IF_ERROR(stmt.bind(6, proto.sort_order));

        auto st = stmt.step();
        if (st.is_err()) return st.error();
        created += db_.changes();
    }

    if (created > 0) {
        Logger::default_logger().info("种入预置分类 " + std::to_string(created) + " 条");
    }
    return Result<int>(created);
}

Result<std::vector<Category>> CategoryRepository::list(std::optional<Direction> direction,
                                                       bool include_inactive) {
    std::string sql = std::string("SELECT ") + kColumns + " FROM categories WHERE 1=1";
    if (direction.has_value())  sql += " AND direction = ?";
    if (!include_inactive)      sql += " AND is_active = 1";
    sql += " ORDER BY direction ASC, sort_order ASC, name ASC;";

    PENHU_ASSIGN_OR_RETURN(stmt, db_.prepare(sql));

    int idx = 1;
    if (direction.has_value()) {
        PENHU_RETURN_IF_ERROR(stmt.bind(idx++, static_cast<int>(direction.value())));
    }

    std::vector<Category> categories;
    auto st = stmt.each_row([&categories](Statement& s) -> Status {
        categories.push_back(row_to_category(s));
        return status_ok();
    });
    if (st.is_err()) return st.error();
    return Result<std::vector<Category>>(std::move(categories));
}

Result<Category> CategoryRepository::find_by_id(const std::string& id) {
    PENHU_ASSIGN_OR_RETURN(stmt, db_.prepare(
        std::string("SELECT ") + kColumns + " FROM categories WHERE id = ?1 LIMIT 1;"));
    PENHU_RETURN_IF_ERROR(stmt.bind(1, id));

    auto step = stmt.step();
    if (step.is_err()) return step.error();
    if (step.value() == StepResult::Done) {
        return Result<Category>::fail(ErrorCode::NotFound, "分类不存在: " + id,
                                      "CategoryRepository::find_by_id");
    }
    return Result<Category>(row_to_category(stmt));
}

Result<Category> CategoryRepository::create(const std::string& name,
                                            const std::string& icon,
                                            const std::string& color_hex,
                                            Direction direction,
                                            int sort_order) {
    Category c;
    c.id         = uuid_to_compact(new_uuid());
    c.name       = name;
    c.icon       = icon.empty() ? "label" : icon;
    c.color_hex  = color_hex;
    c.direction  = direction;
    c.sort_order = sort_order;
    c.is_builtin = false;
    c.is_active  = true;

    if (!c.is_valid()) {
        // 分成具体几条，别再用一句笼统的「参数非法」——
        // 之前那句话只提名称和颜色，而真正的失败原因可能是 id 生成出了问题，
        // 排查的人会照着名字/颜色查半天。
        std::string why = "分类参数非法：";
        if (c.id.empty()) why += "内部生成的 id 为空；";
        if (c.name.empty() || c.name.size() > 32) why += "名称必须是 1-32 字节；";
        if (c.color_hex.size() != 7 || c.color_hex[0] != '#') why += "颜色必须是 #RRGGBB 形式；";
        return Result<Category>::fail(ErrorCode::InvalidArgument, why,
                                      "CategoryRepository::create");
    }

    PENHU_ASSIGN_OR_RETURN(stmt, db_.prepare(
        "INSERT INTO categories (id, name, icon, color_hex, direction, sort_order, is_builtin, is_active) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, 0, 1);"));
    PENHU_RETURN_IF_ERROR(stmt.bind(1, c.id));
    PENHU_RETURN_IF_ERROR(stmt.bind(2, c.name));
    PENHU_RETURN_IF_ERROR(stmt.bind(3, c.icon));
    PENHU_RETURN_IF_ERROR(stmt.bind(4, c.color_hex));
    PENHU_RETURN_IF_ERROR(stmt.bind(5, static_cast<int>(c.direction)));
    PENHU_RETURN_IF_ERROR(stmt.bind(6, c.sort_order));

    auto step = stmt.step();
    if (step.is_err()) {
        // 撞上 (name, direction) 唯一索引
        if (step.error().code == ErrorCode::AlreadyExists) {
            return Result<Category>::fail(
                ErrorCode::AlreadyExists,
                "已存在同名" + std::string(direction_label_zh(direction)) + "分类 '" + name + "'",
                "CategoryRepository::create");
        }
        return step.error();
    }
    return Result<Category>(std::move(c));
}

Status CategoryRepository::update(const Category& category) {
    if (!category.is_valid()) {
        return status_err(ErrorCode::InvalidArgument, "分类参数非法", "update");
    }
    PENHU_ASSIGN_OR_RETURN(stmt, db_.prepare(
        "UPDATE categories SET name = ?1, icon = ?2, color_hex = ?3, "
        "direction = ?4, sort_order = ?5, is_active = ?6 WHERE id = ?7;"));
    PENHU_RETURN_IF_ERROR(stmt.bind(1, category.name));
    PENHU_RETURN_IF_ERROR(stmt.bind(2, category.icon));
    PENHU_RETURN_IF_ERROR(stmt.bind(3, category.color_hex));
    PENHU_RETURN_IF_ERROR(stmt.bind(4, static_cast<int>(category.direction)));
    PENHU_RETURN_IF_ERROR(stmt.bind(5, category.sort_order));
    PENHU_RETURN_IF_ERROR(stmt.bind(6, category.is_active));
    PENHU_RETURN_IF_ERROR(stmt.bind(7, category.id));

    auto st = stmt.step();
    if (st.is_err()) return to_status(st);
    if (db_.changes() == 0) {
        return status_err(ErrorCode::NotFound, "分类不存在或内容未变化", "update");
    }
    return status_ok();
}

Status CategoryRepository::set_active(const std::string& id, bool active) {
    PENHU_ASSIGN_OR_RETURN(stmt, db_.prepare(
        "UPDATE categories SET is_active = ?1 WHERE id = ?2;"));
    PENHU_RETURN_IF_ERROR(stmt.bind(1, active));
    PENHU_RETURN_IF_ERROR(stmt.bind(2, id));
    return to_status(stmt.step());
}

Status CategoryRepository::remove(const std::string& id) {
    PENHU_ASSIGN_OR_RETURN(category, find_by_id(id));
    if (category.is_builtin) {
        return status_err(ErrorCode::PermissionDenied,
                          "预置分类不允许删除，只能停用（历史记录还引用着它）",
                          "CategoryRepository::remove");
    }
    if (category_in_use_by_records(id)) {
        return status_err(ErrorCode::PermissionDenied,
                          "该分类下还有记录，请先把那些记录改到别的分类",
                          "CategoryRepository::remove");
    }

    PENHU_ASSIGN_OR_RETURN(stmt, db_.prepare("DELETE FROM categories WHERE id = ?1;"));
    PENHU_RETURN_IF_ERROR(stmt.bind(1, id));
    return to_status(stmt.step());
}

bool CategoryRepository::category_in_use_by_records(const std::string& category_id) {
    auto stmt = db_.prepare("SELECT count(*) FROM records WHERE category_id = ?1 LIMIT 1;");
    if (stmt.is_err()) return true;   // 查不出来就当作在用，宁可拒绝删除
    if (stmt.value().bind(1, category_id).is_err()) return true;
    auto step = stmt.value().step();
    if (step.is_err() || step.value() != StepResult::Row) return true;
    return stmt.value().column_int64(0) > 0;
}

Result<std::unordered_map<std::string, Category>> CategoryRepository::map_by_id() {
    PENHU_ASSIGN_OR_RETURN(categories, list(std::nullopt, true));
    std::unordered_map<std::string, Category> out;
    out.reserve(categories.size());
    for (auto& c : categories) {
        out.emplace(c.id, std::move(c));
    }
    return Result<std::unordered_map<std::string, Category>>(std::move(out));
}

Status CategoryRepository::ensure_usable(const std::string& id, Direction direction) {
    PENHU_ASSIGN_OR_RETURN(category, find_by_id(id));
    if (!category.is_active) {
        return status_err(ErrorCode::InvalidArgument,
                          "分类 '" + category.name + "' 已停用", "ensure_usable");
    }
    if (category.direction != direction) {
        return status_err(
            ErrorCode::InvalidArgument,
            "分类 '" + category.name + "' 是" + direction_label_zh(category.direction) +
                "分类，不能用于登记" + direction_label_zh(direction),
            "ensure_usable");
    }
    return status_ok();
}

int64_t CategoryRepository::count() {
    auto stmt = db_.prepare("SELECT count(*) FROM categories;");
    if (stmt.is_err()) return 0;
    auto step = stmt.value().step();
    if (step.is_err() || step.value() != StepResult::Row) return 0;
    return stmt.value().column_int64(0);
}

}  // namespace penhu::storage
