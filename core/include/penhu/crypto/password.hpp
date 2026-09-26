#pragma once
// =============================================================================
//  penhu/crypto/password.hpp
//  密码哈希与密钥派生。
//
//  两个用途，参数不同，刻意分开：
//    1) hash_password / verify_password —— 只用来「验证登录」。
//       输出是 libsodium 的 $argon2id$v=19$m=...$salt$hash 自描述字符串，
//       参数嵌在里面，所以以后调强参数时老用户依然能登录（needs_rehash 会提示）。
//    2) derive_db_key —— 把密码变成 32 字节原始密钥，喂给 SQLCipher。
//       这条路径不做「哈希后存起来」，密钥只在内存里存在。
//
//  算法选 Argon2id 而不是 PBKDF2/bcrypt 的理由：
//    Argon2id 同时抗 GPU 并行和抗侧信道（前半个 pass 走 data-independent），
//    是当前密码哈希的推荐选择。libsodium 的 crypto_pwhash 正是 Argon2id13。
// =============================================================================

#include <cstdint>
#include <string>
#include <string_view>
#include "penhu/crypto/secure_buffer.hpp"
#include "penhu/util/result.hpp"

namespace penhu::crypto {

/// Argon2id 成本参数
struct KdfParams {
    uint64_t ops_limit{0};         // 时间成本：迭代遍数
    std::size_t mem_limit_bytes{0}; // 空间成本：内存用量
    std::size_t salt_bytes{16};

    /// 约 64MB / 2 遍。用于交互式登录的保底档。
    static KdfParams interactive() noexcept;
    /// 约 256MB / 3 遍。本项目默认档：本机应用，登录慢 1 秒换更强的抗爆破。
    static KdfParams moderate() noexcept;
    /// 约 1GB / 4 遍。给「数据特别敏感」的场景，当前 CLI 可通过 --kdf sensitive 启用。
    static KdfParams sensitive() noexcept;

    std::string describe() const;
};

/// 生成 salt 的十六进制表示
std::string generate_salt_hex(std::size_t bytes = 16);

/// 生成随机令牌 / 十六进制串（用于会话 token、salt、id）
std::string random_hex(std::size_t bytes);

/// 计算 Argon2id 哈希，返回自描述的 "$argon2id$..." 字符串
Result<std::string> hash_password(std::string_view password,
                                  const KdfParams& params = KdfParams::moderate());

/// 校验密码。libsodium 内部是恒定时间比较。
Result<bool> verify_password(std::string_view password, std::string_view stored_hash);

/// 判断已存哈希是否需要按新参数重算（登录成功后顺手升级）
bool password_needs_rehash(std::string_view stored_hash, const KdfParams& params);

/// 从密码 + salt 派生 32 字节数据库密钥。
/// salt_hex 必须来自用户的 users.db 记录，且每个用户独立。
Result<Key256> derive_db_key(std::string_view password,
                             std::string_view salt_hex,
                             const KdfParams& params = KdfParams::moderate());

}  // namespace penhu::crypto
