#include "penhu/storage/user_repository.hpp"

#include <string>
#include <vector>

#include "penhu/domain/date.hpp"
#include "penhu/util/log.hpp"
#include "penhu/util/uuid.hpp"

namespace penhu::storage {
namespace {

constexpr const char* kUserColumns =
    "id, username, password_hash, db_salt_hex, db_filename, created_at, last_login_at, is_active";

}  // namespace

User UserRepository::row_to_user(Statement& s) {
    User u;
    u.id            = s.column_text(0);
    u.username      = s.column_text(1);
    u.password_hash = s.column_text(2);
    u.db_salt_hex   = s.column_text(3);
    u.db_filename   = s.column_text(4);
    u.created_at    = s.column_int64(5);
    u.last_login_at = s.column_int64(6);
    u.is_active     = s.column_bool(7);
    return u;
}

Result<User> UserRepository::create(const std::string& username,
                                    const std::string& password,
                                    const crypto::KdfParams& params) {
    PENHU_RETURN_IF_ERROR(user_policy::validate_username(username));
    PENHU_RETURN_IF_ERROR(user_policy::validate_password(password));

    // 先查一次给出清晰的错误信息；真正的唯一性由 UNIQUE 索引保证
    // （两次检查之间的竞态窗口极小，且插入语句会兜住）
    if (exists(username)) {
        return Result<User>::fail(ErrorCode::AlreadyExists,
                                  "用户名 '" + username + "' 已被占用",
                                  "UserRepository::create");
    }

    PENHU_ASSIGN_OR_RETURN(hash, crypto::hash_password(password, params));

    User u;
    u.id            = uuid_to_compact(new_uuid());
    u.username      = username;
    u.password_hash = std::move(hash);
    // 每个用户独立的 salt：即使两个人用了同一个密码，他们的库密钥也完全不同，
    // 攻击者不能靠「撞出一个人」去开另一个人的库
    u.db_salt_hex   = crypto::generate_salt_hex(16);
    u.db_filename   = u.id + ".db";
    u.created_at    = timeutil::now_epoch_seconds();
    u.last_login_at = 0;
    u.is_active     = true;

    PENHU_ASSIGN_OR_RETURN(stmt, db_.prepare(
        "INSERT INTO users (id, username, username_ci, password_hash, db_salt_hex, "
        "db_filename, created_at, last_login_at, is_active) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9);"));

    PENHU_RETURN_IF_ERROR(stmt.bind(1, u.id));
    PENHU_RETURN_IF_ERROR(stmt.bind(2, u.username));
    PENHU_RETURN_IF_ERROR(stmt.bind(3, user_policy::normalize_username(u.username)));
    PENHU_RETURN_IF_ERROR(stmt.bind(4, u.password_hash));
    PENHU_RETURN_IF_ERROR(stmt.bind(5, u.db_salt_hex));
    PENHU_RETURN_IF_ERROR(stmt.bind(6, u.db_filename));
    PENHU_RETURN_IF_ERROR(stmt.bind(7, u.created_at));
    PENHU_RETURN_IF_ERROR(stmt.bind(8, u.last_login_at));
    PENHU_RETURN_IF_ERROR(stmt.bind(9, u.is_active));

    auto step = stmt.step();
    if (step.is_err()) return step.error();

    // 立刻把密钥派生要用的参数记进日志（不是密码，不是哈希）
    Logger::default_logger().info(
        "创建账号 '" + u.username + "' (id=" + u.id + ") KDF=" + params.describe());

    return Result<User>(std::move(u));
}

Result<User> UserRepository::find_by_username(const std::string& username) {
    PENHU_ASSIGN_OR_RETURN(stmt, db_.prepare(
        std::string("SELECT ") + kUserColumns + " FROM users WHERE username_ci = ?1 LIMIT 1;"));
    PENHU_RETURN_IF_ERROR(stmt.bind(1, user_policy::normalize_username(username)));

    auto step = stmt.step();
    if (step.is_err()) return step.error();
    if (step.value() == StepResult::Done) {
        return Result<User>::fail(ErrorCode::NotFound,
                                  "用户 '" + username + "' 不存在",
                                  "UserRepository::find_by_username");
    }
    return Result<User>(row_to_user(stmt));
}

Result<User> UserRepository::find_for_auth(const std::string& username) {
    // 语义上和 find_by_username 一样，但单独命名是为了让调用方一眼看出
    // 「这里在拿密码哈希，属于敏感路径」，别顺手把结果丢进日志。
    return find_by_username(username);
}

Result<User> UserRepository::find_by_id(const std::string& id) {
    PENHU_ASSIGN_OR_RETURN(stmt, db_.prepare(
        std::string("SELECT ") + kUserColumns + " FROM users WHERE id = ?1 LIMIT 1;"));
    PENHU_RETURN_IF_ERROR(stmt.bind(1, id));

    auto step = stmt.step();
    if (step.is_err()) return step.error();
    if (step.value() == StepResult::Done) {
        return Result<User>::fail(ErrorCode::NotFound, "用户 id 不存在: " + id,
                                  "UserRepository::find_by_id");
    }
    return Result<User>(row_to_user(stmt));
}

Status UserRepository::update_last_login(const std::string& user_id) {
    PENHU_ASSIGN_OR_RETURN(stmt, db_.prepare(
        "UPDATE users SET last_login_at = ?1 WHERE id = ?2;"));
    PENHU_RETURN_IF_ERROR(stmt.bind(1, timeutil::now_epoch_seconds()));
    PENHU_RETURN_IF_ERROR(stmt.bind(2, user_id));
    return to_status(stmt.step());
}

Status UserRepository::update_password_hash(const std::string& user_id,
                                            const std::string& new_hash) {
    PENHU_ASSIGN_OR_RETURN(stmt, db_.prepare(
        "UPDATE users SET password_hash = ?1 WHERE id = ?2;"));
    PENHU_RETURN_IF_ERROR(stmt.bind(1, new_hash));
    PENHU_RETURN_IF_ERROR(stmt.bind(2, user_id));

    auto st = stmt.step();
    if (st.is_err()) return to_status(st);
    if (db_.changes() == 0) {
        return status_err(ErrorCode::NotFound, "用户不存在，密码未更新",
                          "update_password_hash");
    }
    return status_ok();
}

Status UserRepository::set_active(const std::string& user_id, bool active) {
    PENHU_ASSIGN_OR_RETURN(stmt, db_.prepare(
        "UPDATE users SET is_active = ?1 WHERE id = ?2;"));
    PENHU_RETURN_IF_ERROR(stmt.bind(1, active));
    PENHU_RETURN_IF_ERROR(stmt.bind(2, user_id));
    return to_status(stmt.step());
}

Result<std::vector<User>> UserRepository::list_all() {
    PENHU_ASSIGN_OR_RETURN(stmt, db_.prepare(
        std::string("SELECT ") + kUserColumns + " FROM users ORDER BY created_at ASC;"));

    std::vector<User> users;
    auto st = stmt.each_row([&users](Statement& s) -> Status {
        users.push_back(row_to_user(s));
        return status_ok();
    });
    if (st.is_err()) return st.error();
    return Result<std::vector<User>>(std::move(users));
}

Status UserRepository::erase(const std::string& user_id) {
    PENHU_ASSIGN_OR_RETURN(stmt, db_.prepare("DELETE FROM users WHERE id = ?1;"));
    PENHU_RETURN_IF_ERROR(stmt.bind(1, user_id));

    auto st = stmt.step();
    if (st.is_err()) return to_status(st);
    if (db_.changes() == 0) {
        return status_err(ErrorCode::NotFound, "用户不存在，无法删除", "UserRepository::erase");
    }
    return status_ok();
}

bool UserRepository::exists(const std::string& username) {
    auto stmt = db_.prepare("SELECT 1 FROM users WHERE username_ci = ?1 LIMIT 1;");
    if (stmt.is_err()) return false;
    if (stmt.value().bind(1, user_policy::normalize_username(username)).is_err()) return false;
    auto step = stmt.value().step();
    if (step.is_err()) return false;
    return step.value() == StepResult::Row;
}

int64_t UserRepository::count() {
    auto stmt = db_.prepare("SELECT count(*) FROM users;");
    if (stmt.is_err()) return 0;
    auto step = stmt.value().step();
    if (step.is_err() || step.value() != StepResult::Row) return 0;
    return stmt.value().column_int64(0);
}

}  // namespace penhu::storage
