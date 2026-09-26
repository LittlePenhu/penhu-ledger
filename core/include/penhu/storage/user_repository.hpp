#pragma once
// =============================================================================
//  penhu/storage/user_repository.hpp
//  users.db 的访问层。这个库是明文的，所以这里每一行代码都必须经得起问：
//  「这一列会不会泄露用户的钱？」
// =============================================================================

#include <string>
#include <vector>
#include "penhu/domain/user.hpp"
#include "penhu/crypto/password.hpp"
#include "penhu/storage/database.hpp"

namespace penhu::storage {

class UserRepository {
public:
    explicit UserRepository(Database& db) : db_(db) {}

    /// 建号。负责：校验用户名 → 生成 Argon2id 哈希 → 生成独立 salt →
    /// 按 uuid 拟定库文件名。**不负责创建那个库文件**（由 LedgerService 做，
    /// 因为「文件建了但账号没建成功」比反过来更难收拾）。
    Result<User> create(const std::string& username,
                        const std::string& password,
                        const crypto::KdfParams& params = crypto::KdfParams::moderate());

    /// 按用户名查（不区分大小写）。查不到返回 NotFound。
    Result<User> find_by_username(const std::string& username);
    Result<User> find_by_id(const std::string& id);

    /// 只取密码哈希与 salt，用于登录校验
    Result<User> find_for_auth(const std::string& username);

    Status update_last_login(const std::string& user_id);
    Status update_password_hash(const std::string& user_id, const std::string& new_hash);
    Status set_active(const std::string& user_id, bool active);

    Result<std::vector<User>> list_all();

    /// 彻底删除账号记录。加密库文件由服务层负责删。
    Status erase(const std::string& user_id);

    bool exists(const std::string& username);

    int64_t count();

private:
    /// 把一行结果集读成 User
    static User row_to_user(Statement& stmt);

    Database& db_;
};

}  // namespace penhu::storage
