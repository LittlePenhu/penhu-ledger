#include "penhu/crypto/cipher.hpp"

#include <sodium.h>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <utility>

namespace penhu::crypto {
namespace {

bool ensure_sodium() noexcept {
    static const int rc = sodium_init();
    return rc >= 0;
}

constexpr std::uint8_t kFormatVersion = 1;
constexpr std::size_t  kHeaderBytes   = 3;    // version + algorithm + nonce_len
constexpr std::size_t  kTagBytes      = 16;

std::size_t nonce_len_for(AeadAlgorithm alg) noexcept {
    switch (alg) {
        case AeadAlgorithm::Aes256Gcm:         return 12;   // GCM 标准 96-bit
        case AeadAlgorithm::XChaCha20Poly1305: return 24;   // XChaCha 的 192-bit
    }
    return 0;
}

std::size_t tag_len_for(AeadAlgorithm alg) noexcept {
    switch (alg) {
        case AeadAlgorithm::Aes256Gcm:         return crypto_aead_aes256gcm_ABYTES;
        case AeadAlgorithm::XChaCha20Poly1305: return crypto_aead_xchacha20poly1305_ietf_ABYTES;
    }
    return 0;
}

/// 统一的错误构造，避免每处手写重复字符串
Result<std::string> cipher_fail(std::string what) {
    return Result<std::string>::fail(ErrorCode::CryptoFailure, std::move(what), "cipher::open");
}

}  // namespace

const char* to_string(AeadAlgorithm alg) noexcept {
    switch (alg) {
        case AeadAlgorithm::Aes256Gcm:         return "AES-256-GCM";
        case AeadAlgorithm::XChaCha20Poly1305: return "XChaCha20-Poly1305";
    }
    return "unknown";
}

AeadAlgorithm preferred_algorithm() noexcept {
    static const AeadAlgorithm chosen = [] {
        if (ensure_sodium() && crypto_aead_aes256gcm_is_available() == 1) {
            return AeadAlgorithm::Aes256Gcm;
        }
        return AeadAlgorithm::XChaCha20Poly1305;
    }();
    return chosen;
}

std::string base64_encode(const std::vector<std::uint8_t>& data) {
    if (data.empty()) return {};
    const std::size_t encoded_len =
        sodium_base64_encoded_len(data.size(), sodium_base64_VARIANT_ORIGINAL);
    std::string out(encoded_len, '\0');
    sodium_bin2base64(out.data(), out.size(), data.data(), data.size(),
                      sodium_base64_VARIANT_ORIGINAL);
    out.resize(std::strlen(out.c_str()));
    return out;
}

Result<std::vector<std::uint8_t>> base64_decode(std::string_view text) {
    if (!ensure_sodium()) {
        return Result<std::vector<std::uint8_t>>::fail(
            ErrorCode::CryptoFailure, "libsodium 未初始化", "base64_decode");
    }
    if (text.empty()) return Result<std::vector<std::uint8_t>>(std::vector<std::uint8_t>{});

    std::vector<std::uint8_t> out(text.size());   // 解码后一定不会比原文长
    std::size_t out_len = 0;
    if (sodium_base642bin(out.data(), out.size(), text.data(), text.size(),
                          nullptr, &out_len, nullptr,
                          sodium_base64_VARIANT_ORIGINAL) != 0) {
        return Result<std::vector<std::uint8_t>>::fail(
            ErrorCode::InvalidArgument, "Base64 解码失败", "base64_decode");
    }
    out.resize(out_len);
    return Result<std::vector<std::uint8_t>>(std::move(out));
}

Result<SealedBlob> SealedBlob::from_base64(std::string_view text) {
    auto bytes = base64_decode(text);
    if (bytes.is_err()) return bytes.error();
    return SealedBlob{std::move(bytes.value())};
}

Result<SealedBlob> seal_with(AeadAlgorithm alg,
                             const Key256& key,
                             std::string_view plaintext,
                             std::string_view aad) {
    if (!ensure_sodium()) {
        return Result<SealedBlob>::fail(ErrorCode::CryptoFailure,
                                       "libsodium 未初始化", "cipher::seal_with");
    }
    if (key.is_all_zero()) {
        return Result<SealedBlob>::fail(ErrorCode::CryptoFailure,
                                       "拒绝使用全零密钥", "cipher::seal_with");
    }

    const std::size_t nonce_len = nonce_len_for(alg);
    const std::size_t tag_len   = tag_len_for(alg);
    // nonce_len_for 对未知算法返回 0 —— 显式拦掉，避免后面按 0 长度算出一个
    // 「看起来正常但解不开」的密文。
    if (nonce_len == 0 || tag_len == 0) {
        return Result<SealedBlob>::fail(
            ErrorCode::CryptoFailure,
            std::string("不支持的 AEAD 算法标识: ") + std::to_string(static_cast<int>(alg)),
            "cipher::seal_with");
    }

    std::vector<std::uint8_t> blob;
    blob.reserve(kHeaderBytes + nonce_len + plaintext.size() + tag_len);
    blob.push_back(kFormatVersion);
    blob.push_back(static_cast<std::uint8_t>(alg));
    blob.push_back(static_cast<std::uint8_t>(nonce_len));
    blob.resize(kHeaderBytes + nonce_len);

    // nonce 每次随机生成。AES-GCM 是 96-bit，同一密钥下加密次数过亿时
    // 理论碰撞概率才到 2^-32 量级；本应用场景（用户自己填的 API Key）远远够用。
    randombytes_buf(blob.data() + kHeaderBytes, nonce_len);

    const std::uint8_t* ad = aad.empty() ? nullptr
                                         : reinterpret_cast<const std::uint8_t*>(aad.data());

    std::vector<std::uint8_t> ct(plaintext.size() + tag_len);
    unsigned long long ct_len = 0;

    int rc = -1;
    if (alg == AeadAlgorithm::Aes256Gcm) {
        rc = crypto_aead_aes256gcm_encrypt(
            ct.data(), &ct_len,
            reinterpret_cast<const std::uint8_t*>(plaintext.data()),
            static_cast<unsigned long long>(plaintext.size()),
            ad, static_cast<unsigned long long>(aad.size()),
            nullptr,                        // nsec：只对带随机数复用防护的 API 有意义
            blob.data() + kHeaderBytes,
            key.data());
    } else {
        rc = crypto_aead_xchacha20poly1305_ietf_encrypt(
            ct.data(), &ct_len,
            reinterpret_cast<const std::uint8_t*>(plaintext.data()),
            static_cast<unsigned long long>(plaintext.size()),
            ad, static_cast<unsigned long long>(aad.size()),
            nullptr,
            blob.data() + kHeaderBytes,
            key.data());
    }

    if (rc != 0) {
        secure_wipe(ct.data(), ct.size());
        return Result<SealedBlob>::fail(ErrorCode::CryptoFailure,
                                       std::string(to_string(alg)) + " 加密失败",
                                       "cipher::seal_with");
    }

    blob.insert(blob.end(), ct.begin(), ct.begin() + static_cast<std::ptrdiff_t>(ct_len));
    secure_wipe(ct.data(), ct.size());

    return Result<SealedBlob>(SealedBlob{std::move(blob)});
}

Result<SealedBlob> seal(const Key256& key,
                        std::string_view plaintext,
                        std::string_view aad) {
    // seal 就是「用本机首选算法调 seal_with」。拆成两个函数是为了让自检
    // 能把两种算法都真跑一遍（本机首选只会用到一个，另一个等于没覆盖）。
    return seal_with(preferred_algorithm(), key, plaintext, aad);
}

Result<AeadAlgorithm> peek_algorithm(const std::vector<std::uint8_t>& blob) {
    // 只解头部，不做任何解密运算 —— 所以这里**不需要密钥**。
    // 校验规则刻意和 open() 保持一致：头部合法但 nonce 长度对不上的密文，
    // 在这里就该被判为格式非法，而不是等到解密时才以「认证失败」的面目出现
    // （后者会让人误以为是密钥错了）。
    if (blob.size() < kHeaderBytes + 1) {
        return Result<AeadAlgorithm>::fail(ErrorCode::CryptoFailure,
                                           "密文过短，连头部都不完整",
                                           "cipher::peek_algorithm");
    }
    if (blob[0] != kFormatVersion) {
        return Result<AeadAlgorithm>::fail(
            ErrorCode::CryptoFailure,
            "密文格式版本不受支持：v" + std::to_string(blob[0]),
            "cipher::peek_algorithm");
    }

    const auto alg = static_cast<AeadAlgorithm>(blob[1]);
    const std::size_t declared_nonce_len = blob[2];
    const std::size_t expect_nonce_len = nonce_len_for(alg);
    if (expect_nonce_len == 0) {
        return Result<AeadAlgorithm>::fail(
            ErrorCode::CryptoFailure,
            "密文头部里的算法标识非法：" + std::to_string(blob[1]),
            "cipher::peek_algorithm");
    }
    if (declared_nonce_len != expect_nonce_len) {
        return Result<AeadAlgorithm>::fail(
            ErrorCode::CryptoFailure,
            "nonce 长度与算法不匹配（头部声明 " + std::to_string(declared_nonce_len) +
                "，该算法应为 " + std::to_string(expect_nonce_len) + "）",
            "cipher::peek_algorithm");
    }
    return Result<AeadAlgorithm>(alg);
}

Result<std::string> open(const Key256& key,
                         const std::vector<std::uint8_t>& blob,
                         std::string_view aad) {
    if (!ensure_sodium()) {
        return cipher_fail("libsodium 未初始化");
    }
    if (key.is_all_zero()) {
        return cipher_fail("拒绝使用全零密钥");
    }
    if (blob.size() < kHeaderBytes + 1) {
        return cipher_fail("密文过短，连头部都不完整");
    }
    if (blob[0] != kFormatVersion) {
        return cipher_fail("密文格式版本不受支持：v" + std::to_string(blob[0]));
    }

    const auto raw_alg = static_cast<AeadAlgorithm>(blob[1]);
    const std::size_t declared_nonce_len = blob[2];
    const std::size_t expect_nonce_len = nonce_len_for(raw_alg);
    if (expect_nonce_len == 0) {
        return cipher_fail("密文头部里的算法标识非法：" + std::to_string(blob[1]));
    }
    if (declared_nonce_len != expect_nonce_len) {
        return cipher_fail("nonce 长度与算法不匹配（头部声明 " +
                           std::to_string(declared_nonce_len) + "，该算法应为 " +
                           std::to_string(expect_nonce_len) + "）");
    }
    if (blob.size() < kHeaderBytes + declared_nonce_len + tag_len_for(raw_alg)) {
        return cipher_fail("密文长度不足，可能被截断");
    }

    const std::uint8_t* nonce = blob.data() + kHeaderBytes;
    const std::uint8_t* ct    = nonce + declared_nonce_len;
    const std::size_t   ct_len = blob.size() - kHeaderBytes - declared_nonce_len;

    const std::uint8_t* ad = aad.empty() ? nullptr
                                         : reinterpret_cast<const std::uint8_t*>(aad.data());

    std::vector<std::uint8_t> pt(ct_len);
    unsigned long long pt_len = 0;

    int rc = -1;
    if (raw_alg == AeadAlgorithm::Aes256Gcm) {
        if (crypto_aead_aes256gcm_is_available() != 1) {
            return cipher_fail("密文是用 AES-256-GCM 加密的，但本机 CPU 不支持该算法");
        }
        rc = crypto_aead_aes256gcm_decrypt(
            pt.data(), &pt_len, nullptr, ct, static_cast<unsigned long long>(ct_len),
            ad, static_cast<unsigned long long>(aad.size()), nonce, key.data());
    } else {
        rc = crypto_aead_xchacha20poly1305_ietf_decrypt(
            pt.data(), &pt_len, nullptr, ct, static_cast<unsigned long long>(ct_len),
            ad, static_cast<unsigned long long>(aad.size()), nonce, key.data());
    }

    if (rc != 0) {
        secure_wipe(pt.data(), pt.size());
        // 不区分「密钥错」和「密文被改」是刻意的：对攻击者没有额外信息
        return cipher_fail("认证失败：密钥错误或密文/附加数据已被篡改");
    }

    std::string out(reinterpret_cast<const char*>(pt.data()),
                    static_cast<std::size_t>(pt_len));
    secure_wipe(pt.data(), pt.size());
    return Result<std::string>(std::move(out));
}

bool aead_self_test(std::string* report) {
    std::string log;
    const char* alg_name = to_string(preferred_algorithm());

    if (!ensure_sodium()) {
        if (report) *report = "libsodium 未初始化";
        return false;
    }

    // 两种算法都要真跑一遍往返 —— 只测 preferred() 等于对另一种零覆盖
    const std::pair<AeadAlgorithm, const char*> cases[] = {
        {AeadAlgorithm::Aes256Gcm,         "AES-256-GCM"},
        {AeadAlgorithm::XChaCha20Poly1305, "XChaCha20-Poly1305"},
    };

    Key256 key = Key256::random();
    const std::string plaintext = "PenHu Ledger 加密自检 · ¥1234.56 · 🖋️";
    const std::string aad = "self-test";

    for (const auto& [alg, name] : cases) {
        if (alg == AeadAlgorithm::Aes256Gcm && crypto_aead_aes256gcm_is_available() != 1) {
            log += std::string(name) + ": 跳过（本机 CPU 不支持 AES-NI）\n";
            continue;
        }

        auto sealed = seal_with(alg, key, plaintext, aad);
        if (sealed.is_err()) {
            log += std::string(name) + ": 加密失败 - " + sealed.error().to_string() + "\n";
            if (report) *report = log;
            return false;
        }

        // 头部必须如实记录算法，否则解密方无从选择实现
        auto recorded = peek_algorithm(sealed.value().bytes);
        if (recorded.is_err() || recorded.value() != alg) {
            log += std::string(name) + ": 密文头部记录的算法不对\n";
            if (report) *report = log;
            return false;
        }

        auto opened = open(key, sealed.value().bytes, aad);
        if (opened.is_err()) {
            log += std::string(name) + ": 解密失败 - " + opened.error().to_string() + "\n";
            if (report) *report = log;
            return false;
        }
        if (opened.value() != plaintext) {
            log += std::string(name) + ": 往返内容不一致\n";
            if (report) *report = log;
            return false;
        }

        // 篡改检测：翻转密文最后一位（落在 tag 上），必须解不开
        auto tampered = sealed.value().bytes;
        tampered.back() ^= 0x01;
        auto bad = open(key, tampered, aad);
        if (bad.is_ok()) {
            log += std::string(name) + ": 篡改未被检测到（严重问题）\n";
            if (report) *report = log;
            return false;
        }

        // AAD 不匹配必须失败
        auto wrong_aad = open(key, sealed.value().bytes, "other-context");
        if (wrong_aad.is_ok()) {
            log += std::string(name) + ": AAD 不匹配却解密成功（严重问题）\n";
            if (report) *report = log;
            return false;
        }

        // 错误密钥必须失败
        Key256 wrong_key = Key256::random();
        auto wrong_key_res = open(wrong_key, sealed.value().bytes, aad);
        if (wrong_key_res.is_ok()) {
            log += std::string(name) + ": 错误密钥却解密成功（严重问题）\n";
            if (report) *report = log;
            return false;
        }

        log += std::string(name) + ": 往返/篡改/AAD/错密钥 全部符合预期\n";
    }

    // Base64 编解码往返
    const std::string sample = "hello-密文-二进制\x01\x02";
    std::vector<std::uint8_t> raw(sample.begin(), sample.end());
    auto b64 = base64_encode(raw);
    auto back = base64_decode(b64);
    if (back.is_err() || back.value() != raw) {
        log += "Base64 往返失败\n";
        if (report) *report = log;
        return false;
    }
    log += "Base64 往返: OK\n";
    log += std::string("首选算法: ") + alg_name + "\n";

    if (report) *report = log;
    return true;
}

}  // namespace penhu::crypto
