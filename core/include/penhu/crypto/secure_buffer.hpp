#pragma once
// =============================================================================
//  penhu/crypto/secure_buffer.hpp
//  定长密钥缓冲区：不可拷贝、移动即转移、析构即清零。
//
//  为什么需要它：
//    用户的密码派生出的 SQLCipher 密钥会驻留在内存里直到登出。
//    如果它躺在 std::string 或 std::vector 里，一次 realloc、一次日志打印、
//    一次 core dump，密钥就跟着出去了。这个类型的作用是把「密钥」变成一个
//    有明确生命周期和明确边界的东西：只能显式移动，只能显式读出，必然被擦除。
//
//  为何模板实现全部内联而把 libsodium 挡在 .cpp 后面：
//    公共头文件里出现 <sodium.h> 会让所有编译单元都被它影响（宏污染、编译变慢），
//    所以只暴露三个极小的自由函数（secure_wipe / fill_random / ct_equal）。
// =============================================================================

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include "penhu/util/result.hpp"

namespace penhu::crypto {

/// 覆写内存。内部用 libsodium 的 sodium_memzero，
/// 其 volatile 机制保证不会被优化器当成 dead store 删掉。
void secure_wipe(void* data, std::size_t size) noexcept;

namespace detail {
/// 密码学安全随机填充
void fill_random(std::uint8_t* out, std::size_t n) noexcept;
/// 恒定时间比较
bool ct_equal(const void* a, const void* b, std::size_t n) noexcept;
}  // namespace detail

/// 定长安全缓冲区。N 是字节数。
template <std::size_t N>
class SecureBuffer {
    static_assert(N > 0, "SecureBuffer 长度必须大于 0");

public:
    SecureBuffer() noexcept { bytes_.fill(0); }
    ~SecureBuffer() noexcept { wipe(); }

    SecureBuffer(const SecureBuffer&) = delete;
    SecureBuffer& operator=(const SecureBuffer&) = delete;

    SecureBuffer(SecureBuffer&& other) noexcept : bytes_(other.bytes_) { other.wipe(); }

    SecureBuffer& operator=(SecureBuffer&& other) noexcept {
        if (this != &other) {
            wipe();
            bytes_ = other.bytes_;
            other.wipe();
        }
        return *this;
    }

    static SecureBuffer random() noexcept {
        SecureBuffer out;
        detail::fill_random(out.bytes_.data(), N);
        return out;
    }

    static SecureBuffer from_bytes(const std::uint8_t* data, std::size_t len) noexcept {
        SecureBuffer out;
        const std::size_t n = std::min(len, N);
        if (data != nullptr && n > 0) {
            std::copy_n(data, n, out.bytes_.begin());
        }
        return out;
    }

    /// 从十六进制还原。长度必须正好 N*2，否则报错（不做静默补零）
    static Result<SecureBuffer> from_hex(std::string_view hex) {
        if (hex.size() != N * 2) {
            return Result<SecureBuffer>::fail(
                ErrorCode::InvalidArgument,
                "十六进制长度不符：期望 " + std::to_string(N * 2) +
                    " 个字符，实际 " + std::to_string(hex.size()),
                "SecureBuffer::from_hex");
        }
        SecureBuffer out;
        for (std::size_t i = 0; i < N; ++i) {
            const int hi = hex_value(hex[i * 2]);
            const int lo = hex_value(hex[i * 2 + 1]);
            if (hi < 0 || lo < 0) {
                return Result<SecureBuffer>::fail(
                    ErrorCode::InvalidArgument, "含非法十六进制字符", "SecureBuffer::from_hex");
            }
            out.bytes_[i] = static_cast<std::uint8_t>((hi << 4) | lo);
        }
        return Result<SecureBuffer>(std::move(out));
    }

    std::string to_hex() const {
        static constexpr char kDigits[] = "0123456789abcdef";
        std::string out;
        out.resize(N * 2);
        for (std::size_t i = 0; i < N; ++i) {
            out[i * 2]     = kDigits[bytes_[i] >> 4];
            out[i * 2 + 1] = kDigits[bytes_[i] & 0x0F];
        }
        return out;
    }

    std::vector<std::uint8_t> to_vector() const {
        return std::vector<std::uint8_t>(bytes_.begin(), bytes_.end());
    }

    std::uint8_t*       data() noexcept       { return bytes_.data(); }
    const std::uint8_t* data() const noexcept { return bytes_.data(); }

    static constexpr std::size_t size() noexcept { return N; }

    /// 恒定时间比较：不会因为首个不同字节就提前返回
    bool equals(const SecureBuffer& other) const noexcept {
        return detail::ct_equal(bytes_.data(), other.bytes_.data(), N);
    }

    bool is_all_zero() const noexcept {
        for (std::uint8_t b : bytes_) {
            if (b != 0) return false;
        }
        return true;
    }

    /// 立即清零。析构会自动调用；登出时也可手动提前调。
    void wipe() noexcept {
        secure_wipe(bytes_.data(), bytes_.size());
        bytes_.fill(0);
    }

private:
    static int hex_value(char c) noexcept {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    std::array<std::uint8_t, N> bytes_{};
};

/// 常用的密钥长度别名
using Key256 = SecureBuffer<32>;   // AES-256 / XChaCha20 / SQLCipher raw key

/// 加密子系统初始化（封装 sodium_init）。必须在其他 crypto 调用之前至少调一次。
/// 重复调用安全。失败必须让程序拒绝启动，而不是降级运行。
Status initialize_crypto();

/// 自检：跑已知向量，确认库与 CPU 能力（AES-NI 等）符合预期。
bool crypto_self_test();

}  // namespace penhu::crypto
