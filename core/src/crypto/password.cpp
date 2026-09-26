#include "penhu/crypto/password.hpp"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <cstring>

namespace penhu::crypto {
namespace {

bool ensure_sodium() noexcept {
    static const int rc = sodium_init();
    return rc >= 0;
}

}  // namespace

// -----------------------------------------------------------------------------
//  KdfParams
// -----------------------------------------------------------------------------

KdfParams KdfParams::interactive() noexcept {
    return KdfParams{static_cast<uint64_t>(crypto_pwhash_OPSLIMIT_INTERACTIVE),
                     static_cast<std::size_t>(crypto_pwhash_MEMLIMIT_INTERACTIVE),
                     16};
}

KdfParams KdfParams::moderate() noexcept {
    return KdfParams{static_cast<uint64_t>(crypto_pwhash_OPSLIMIT_MODERATE),
                     static_cast<std::size_t>(crypto_pwhash_MEMLIMIT_MODERATE),
                     16};
}

KdfParams KdfParams::sensitive() noexcept {
    return KdfParams{static_cast<uint64_t>(crypto_pwhash_OPSLIMIT_SENSITIVE),
                     static_cast<std::size_t>(crypto_pwhash_MEMLIMIT_SENSITIVE),
                     16};
}

std::string KdfParams::describe() const {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "Argon2id(ops=%llu, mem=%.0f MiB, salt=%zu B)",
                  static_cast<unsigned long long>(ops_limit),
                  static_cast<double>(mem_limit_bytes) / (1024.0 * 1024.0),
                  salt_bytes);
    return std::string(buf);
}

// -----------------------------------------------------------------------------
//  随机
// -----------------------------------------------------------------------------

std::string random_hex(std::size_t bytes) {
    if (bytes == 0) return {};
    std::vector<std::uint8_t> buf(bytes);
    detail::fill_random(buf.data(), bytes);

    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.resize(bytes * 2);
    for (std::size_t i = 0; i < bytes; ++i) {
        out[i * 2]     = kDigits[buf[i] >> 4];
        out[i * 2 + 1] = kDigits[buf[i] & 0x0F];
    }
    // 中间态也擦掉：随机数本身可能成为密钥材料
    secure_wipe(buf.data(), buf.size());
    return out;
}

std::string generate_salt_hex(std::size_t bytes) {
    return random_hex(bytes);
}

// -----------------------------------------------------------------------------
//  密码哈希
// -----------------------------------------------------------------------------

Result<std::string> hash_password(std::string_view password, const KdfParams& params) {
    if (!ensure_sodium()) {
        return Result<std::string>::fail(ErrorCode::CryptoFailure,
                                        "libsodium 未初始化", "hash_password");
    }
    if (password.empty()) {
        return Result<std::string>::fail(ErrorCode::InvalidArgument,
                                        "密码不能为空", "hash_password");
    }

    std::array<char, crypto_pwhash_STRBYTES> out{};
    const int rc = crypto_pwhash_str(
        out.data(),
        reinterpret_cast<const char*>(password.data()),
        static_cast<unsigned long long>(password.size()),
        static_cast<unsigned long long>(params.ops_limit),
        params.mem_limit_bytes);

    if (rc != 0) {
        // 唯一会失败的原因是内存分配不出来（Argon2 要一次性申请 MEMLIMIT 那么多）
        return Result<std::string>::fail(
            ErrorCode::CryptoFailure,
            "Argon2id 计算失败，通常是可用内存不足（需要约 " +
                std::to_string(params.mem_limit_bytes / (1024 * 1024)) + " MiB）",
            "hash_password");
    }
    return Result<std::string>(std::string(out.data()));
}

Result<bool> verify_password(std::string_view password, std::string_view stored_hash) {
    if (!ensure_sodium()) {
        return Result<bool>::fail(ErrorCode::CryptoFailure,
                                 "libsodium 未初始化", "verify_password");
    }
    if (stored_hash.empty()) {
        return Result<bool>::fail(ErrorCode::InvalidArgument,
                                 "库里的密码哈希为空", "verify_password");
    }

    // crypto_pwhash_str_verify 要求哈希是 NUL 结尾的 C 字符串，且会自己解析参数
    const std::string hash_copy(stored_hash);
    const int rc = crypto_pwhash_str_verify(
        hash_copy.c_str(),
        reinterpret_cast<const char*>(password.data()),
        static_cast<unsigned long long>(password.size()));

    // 返回 0 = 匹配；-1 = 不匹配或哈希格式非法。两种都按「验证失败」处理，
    // 不区分是为了不给攻击者「这个用户名存在但密码错 vs 哈希坏了」的信号。
    return Result<bool>(rc == 0);
}

bool password_needs_rehash(std::string_view stored_hash, const KdfParams& params) {
    if (!ensure_sodium() || stored_hash.empty()) return false;
    const std::string hash_copy(stored_hash);
    const int rc = crypto_pwhash_str_needs_rehash(
        hash_copy.c_str(),
        static_cast<unsigned long long>(params.ops_limit),
        params.mem_limit_bytes);
    return rc == 1;
}

// -----------------------------------------------------------------------------
//  密钥派生
// -----------------------------------------------------------------------------

Result<Key256> derive_db_key(std::string_view password,
                             std::string_view salt_hex,
                             const KdfParams& params) {
    if (!ensure_sodium()) {
        return Result<Key256>::fail(ErrorCode::CryptoFailure,
                                   "libsodium 未初始化", "derive_db_key");
    }
    if (password.empty()) {
        return Result<Key256>::fail(ErrorCode::InvalidArgument,
                                   "密码不能为空", "derive_db_key");
    }
    if (salt_hex.size() != crypto_pwhash_SALTBYTES * 2) {
        return Result<Key256>::fail(
            ErrorCode::InvalidArgument,
            "salt 长度必须为 " + std::to_string(crypto_pwhash_SALTBYTES) +
                " 字节（十六进制 " + std::to_string(crypto_pwhash_SALTBYTES * 2) + " 字符）",
            "derive_db_key");
    }

    std::array<std::uint8_t, crypto_pwhash_SALTBYTES> salt{};
    for (std::size_t i = 0; i < crypto_pwhash_SALTBYTES; ++i) {
        const auto hex_val = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        const int hi = hex_val(salt_hex[i * 2]);
        const int lo = hex_val(salt_hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return Result<Key256>::fail(ErrorCode::InvalidArgument,
                                       "salt 含非法十六进制字符", "derive_db_key");
        }
        salt[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }

    Key256 key;
    const int rc = crypto_pwhash(
        key.data(),
        key.size(),
        reinterpret_cast<const char*>(password.data()),
        static_cast<unsigned long long>(password.size()),
        salt.data(),
        static_cast<unsigned long long>(params.ops_limit),
        params.mem_limit_bytes,
        crypto_pwhash_ALG_ARGON2ID13);

    // salt 不是秘密，但留在栈上没意义，擦掉
    secure_wipe(salt.data(), salt.size());

    if (rc != 0) {
        // 关键：失败时 key 可能只被部分写入，必须清零后再返回，
        // 否则调用方可能拿到一个「随机但看起来正常」的密钥去开库，
        // 表现成「密码正确但数据读不出来」这种最难排查的故障。
        key.wipe();
        return Result<Key256>::fail(
            ErrorCode::CryptoFailure,
            "Argon2id 密钥派生失败（内存不足或参数非法）", "derive_db_key");
    }

    if (key.is_all_zero()) {
        key.wipe();
        return Result<Key256>::fail(ErrorCode::CryptoFailure,
                                   "派生出的密钥全为零，拒绝使用", "derive_db_key");
    }

    return Result<Key256>(std::move(key));
}

}  // namespace penhu::crypto
