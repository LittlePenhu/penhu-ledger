#include "penhu/crypto/secure_buffer.hpp"
#include "penhu/crypto/cipher.hpp"

#include <sodium.h>

#include <cstring>

namespace penhu::crypto {
namespace {

/// 保证 sodium 已初始化。函数内静态量在 C++11 起是线程安全的一次性初始化。
bool ensure_sodium() noexcept {
    static const int rc = sodium_init();
    return rc >= 0;
}

}  // namespace

void secure_wipe(void* data, std::size_t size) noexcept {
    if (data != nullptr && size > 0) {
        sodium_memzero(data, size);
    }
}

namespace detail {

void fill_random(std::uint8_t* out, std::size_t n) noexcept {
    if (out == nullptr || n == 0) return;
    if (!ensure_sodium()) {
        // sodium 起不来是致命状态：绝不能退化到 rand() 去生成会话 token，
        // 那等于给攻击者一个可预测的 token。按 0 填充让调用方立刻发现异常。
        std::memset(out, 0, n);
        return;
    }
    randombytes_buf(out, n);
}

bool ct_equal(const void* a, const void* b, std::size_t n) noexcept {
    if (a == nullptr || b == nullptr) return false;
    if (!ensure_sodium()) {
        return std::memcmp(a, b, n) == 0;
    }
    return sodium_memcmp(a, b, n) == 0;
}

}  // namespace detail

Status initialize_crypto() {
    if (!ensure_sodium()) {
        return status_err(ErrorCode::CryptoFailure,
                          "libsodium 初始化失败（sodium_init 返回 <0）",
                          "initialize_crypto");
    }
    return status_ok();
}

bool crypto_self_test() {
    if (!ensure_sodium()) return false;

    // 1) 随机数源必须真的在产出不同内容
    unsigned char a[32] = {};
    unsigned char b[32] = {};
    randombytes_buf(a, sizeof(a));
    randombytes_buf(b, sizeof(b));
    if (std::memcmp(a, b, sizeof(a)) == 0) return false;

    // 2) 擦除必须真的擦了
    unsigned char buf[32];
    std::memset(buf, 0xAB, sizeof(buf));
    secure_wipe(buf, sizeof(buf));
    for (unsigned char c : buf) {
        if (c != 0) return false;
    }

    // 3) 恒定时间比较的正确性（内容和长度都一致才算相等）
    const std::uint8_t x[4] = {1, 2, 3, 4};
    const std::uint8_t y[4] = {1, 2, 3, 4};
    const std::uint8_t z[4] = {1, 2, 3, 5};
    if (!detail::ct_equal(x, y, 4)) return false;
    if (detail::ct_equal(x, z, 4)) return false;

    // 4) AEAD 往返与篡改检测
    std::string report;
    if (!aead_self_test(&report)) return false;

    return true;
}

}  // namespace penhu::crypto
