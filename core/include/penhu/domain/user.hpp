#pragma once
// =============================================================================
//  penhu/domain/user.hpp
//  用户账号模型。
//
//  安全设计的核心一条：
//    users.db 里 只 存 用户名 + Argon2id 哈希 + 每用户独立的 salt，
//    以及那个用户的加密库文件名。金融数据一个字节都不在这里。
//    真正的账本在 data/<uuid>.db，用「密码派生的密钥」做 SQLCipher 整库加密。
//
//  所以即使整个 data 目录被拷走，没有密码也解不开；
//  即使 users.db 泄露，攻击者拿到的只是哈希和文件名。
// =============================================================================

#include <cstdint>
#include <string>
#include "penhu/domain/date.hpp"
#include "penhu/util/result.hpp"

namespace penhu {

struct User {
    std::string id;             // compact uuid
    std::string username;       // 唯一，大小写敏感存储但登录时按不敏感比对
    std::string password_hash;  // libsodium crypto_pwhash_str 输出，自带参数与 salt
    std::string db_salt_hex;    // 16 字节，用于派生该用户的 SQLCipher 密钥
    std::string db_filename;    // 相对 data_dir，"<uuid>.db"
    int64_t     created_at{0};
    int64_t     last_login_at{0};
    bool        is_active{true};

    /// 对外表示（不含任何秘密），API 返回给前端用
    struct Public {
        std::string id;
        std::string username;
        int64_t     created_at{0};
        int64_t     last_login_at{0};
    };
    Public to_public() const { return Public{id, username, created_at, last_login_at}; }
};

/// 用户名 / 密码策略校验。集中放这里，保证注册、改密、管理端三条路径口径一致。
namespace user_policy {

constexpr size_t kMinUsernameLength = 3;
constexpr size_t kMaxUsernameLength = 32;
constexpr size_t kMinPasswordLength = 8;
constexpr size_t kMaxPasswordLength = 128;   // Argon2 对超长密码没有意义，且能防 DoS

/// 只允许字母、数字、下划线、连字符，且必须以字母或数字开头
Status validate_username(const std::string& username);
/// 长度下限 + 必须包含至少两类字符（字母/数字/符号）
Status validate_password(const std::string& password);

/// 统一小写（仅 ASCII），用于「不区分大小写」的用户名比对
std::string normalize_username(const std::string& username);

}  // namespace user_policy

}  // namespace penhu
