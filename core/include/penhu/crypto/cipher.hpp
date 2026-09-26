#pragma once
// =============================================================================
//  penhu/crypto/cipher.hpp
//  字段级 AEAD 加密。用于「不适合整库加密、但也不该明文」的东西：
//  例如用户在设置里填的云端 LLM API Key、以及可选的备注加密。
//
//  为什么同时支持两种算法而不是只挑一个：
//    AES-256-GCM 在支持 AES-NI 的 x86 上是硬件加速的，吞吐最好；
//    但它是 96-bit 随机 nonce，在海量加密下理论上有碰撞风险，
//    而且老 CPU / 某些虚拟化环境下 libsodium 会拒绝提供。
//    XChaCha20-Poly1305 是 192-bit nonce，随机生成绝不会碰撞，纯软件实现也够快。
//  做法：优先 AES-256-GCM，运行时探测不可用就落 XChaCha20；
//  算法标识写进密文头部，所以两种算法的密文可以共存、各自解开。
//
//  密文容器格式（自描述，便于以后换算法）：
//    [0]      version   = 1
//    [1]      algorithm = 1 (AES-256-GCM) | 2 (XChaCha20-Poly1305)
//    [2]      nonce_len
//    [3..]    nonce
//    [..]     ciphertext || tag(16B)
//  整库加密由 SQLCipher 负责（AES-256-CBC + HMAC-SHA512），这里只管字段级。
// =============================================================================

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include "penhu/crypto/secure_buffer.hpp"
#include "penhu/util/result.hpp"

namespace penhu::crypto {

enum class AeadAlgorithm : uint8_t {
    Aes256Gcm           = 1,
    XChaCha20Poly1305   = 2
};

const char* to_string(AeadAlgorithm alg) noexcept;

/// 本机首选的字段加密算法（探测 CPU 能力后决定）
AeadAlgorithm preferred_algorithm() noexcept;

/// 把二进制密文编码成 Base64（存 DB / 塞 JSON 用）
std::string base64_encode(const std::vector<uint8_t>& data);
Result<std::vector<uint8_t>> base64_decode(std::string_view text);

struct SealedBlob {
    std::vector<uint8_t> bytes;

    std::string to_base64() const { return base64_encode(bytes); }
    static Result<SealedBlob> from_base64(std::string_view text);

    bool empty() const noexcept { return bytes.empty(); }
};

/// 加密。aad 是「附加认证数据」——不加密但参与完整性校验，
/// 用来把密文绑到某个上下文（例如 "user:<id>:llm_api_key"），
/// 防止有人把 A 用户的密文搬给 B 用户解。
Result<SealedBlob> seal(const Key256& key,
                        std::string_view plaintext,
                        std::string_view aad = {});

/// 指定算法加密。seal() 其实就是传入 preferred_algorithm() 调它。
/// 之所以单独暴露：自检必须能把两种算法都真跑一遍，
/// 只测「本机首选的那个」等于对另一种算法完全没有覆盖。
Result<SealedBlob> seal_with(AeadAlgorithm alg,
                             const Key256& key,
                             std::string_view plaintext,
                             std::string_view aad = {});

/// 读密文头部里记录的算法（不解密）。失败说明格式非法。
Result<AeadAlgorithm> peek_algorithm(const std::vector<std::uint8_t>& blob);

/// 解密。aad 必须和加密时完全一致，否则认证失败。
Result<std::string> open(const Key256& key,
                         const std::vector<uint8_t>& blob,
                         std::string_view aad = {});

/// 自检：两种算法各跑一遍已知明文的往返 + 篡改检测
bool aead_self_test(std::string* report);

}  // namespace penhu::crypto
