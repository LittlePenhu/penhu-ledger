// =============================================================================
//  tests/test_crypto.cpp
//  加密层单测。
//
//  这里的每个用例都在回答同一个问题：「如果有人拿到了磁盘上的文件，
//  他能算出什么？」。所以断言写得比平时更细——包括「两次相同明文
//  加密出来的密文必须不同」这种容易被忽略但很关键的性质。
// =============================================================================

#include "tiny_test.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "penhu/crypto/cipher.hpp"
#include "penhu/crypto/password.hpp"
#include "penhu/crypto/secure_buffer.hpp"
#include "penhu/crypto/session.hpp"

using namespace penhu;
using namespace penhu::crypto;

// -----------------------------------------------------------------------------
//  SecureBuffer
// -----------------------------------------------------------------------------
TT_TEST(Crypto, 密钥缓冲区基本性质) {
    // 默认构造必须全零（否则未初始化的密钥会被当成「看起来正常的密钥」用下去）
    Key256 fresh;
    TT_CHECK(fresh.is_all_zero());

    // 随机生成的不能全零，且两次不同
    Key256 a = Key256::random();
    Key256 b = Key256::random();
    TT_CHECK(!a.is_all_zero());
    TT_CHECK(!b.is_all_zero());
    TT_CHECK(!a.equals(b));

    // 六边形转换往返
    auto back = Key256::from_hex(a.to_hex());
    TT_IS_OK(back);
    if (back.is_ok()) TT_CHECK(back.value().equals(a));

    // 长度不符必须拒绝（不做静默补零——那会造出一个「看起来对」的错密钥）
    TT_IS_ERR(Key256::from_hex("abcd"));
    TT_IS_ERR(Key256::from_hex(std::string(65, 'a')));
    TT_IS_ERR(Key256::from_hex(std::string(64, 'z')));   // 非十六进制字符

    // 擦除后必须真的全零
    a.wipe();
    TT_CHECK(a.is_all_zero());

    // 移动后源对象必须被清零
    Key256 source = Key256::random();
    Key256 dest = std::move(source);
    TT_CHECK_MSG(source.is_all_zero(), "移动后源密钥没有被清零，等于多留了一份密钥在内存里");
    TT_CHECK(!dest.is_all_zero());
}

TT_TEST(Crypto, 加密子系统自检) {
    TT_IS_OK(initialize_crypto());
    std::string report;
    const bool ok = aead_self_test(&report);
    TT_CHECK_MSG(ok, "AEAD 自检失败:\n" + report);
    if (!report.empty()) {
        std::printf("      自检报告:\n");
        // 逐行缩进打印，方便在测试输出里看
        size_t start = 0;
        while (start < report.size()) {
            const size_t nl = report.find('\n', start);
            const std::string line = report.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
            if (!line.empty()) std::printf("        %s\n", line.c_str());
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
    }
}

// -----------------------------------------------------------------------------
//  密码哈希与密钥派生
// -----------------------------------------------------------------------------
TT_TEST(Crypto, 密码哈希与校验) {
    const crypto::KdfParams fast = crypto::KdfParams::interactive();

    auto hash = hash_password("penhu-2026", fast);
    TT_IS_OK(hash);
    if (hash.is_err()) return;

    // libsodium 的输出是自描述的 $argon2id$... 字符串
    TT_CHECK(hash.value().rfind("$argon2id$", 0) == 0);
    TT_CHECK(hash.value().size() > 60);

    // 同一密码两次哈希必须不同（盐是随机的）
    auto hash2 = hash_password("penhu-2026", fast);
    TT_IS_OK(hash2);
    if (hash2.is_ok()) TT_CHECK(hash.value() != hash2.value());

    // 校验
    TT_CHECK(verify_password("penhu-2026", hash.value()).value_or(false));
    TT_CHECK(!verify_password("penhu-2027", hash.value()).value_or(true));
    TT_CHECK(!verify_password("", hash.value()).value_or(true));
    // 空哈希属于「库里的数据坏了」，这里明确要求它**报错**而不是静默返回 false ——
    // 静默 false 会把「数据损坏」伪装成「密码输错了」，用户永远查不出来。
    TT_IS_ERR(verify_password("penhu-2026", ""));                     // 空哈希
    TT_CHECK(!verify_password("penhu-2026", "garbage").value_or(true)); // 格式错的哈希

    // 哈希字符串里绝不能出现明文密码
    TT_CHECK(hash.value().find("penhu-2026") == std::string::npos);
}

TT_TEST(Crypto, 密钥派生确定性与用户间隔离) {
    const crypto::KdfParams fast = crypto::KdfParams::interactive();
    const std::string salt_a = generate_salt_hex(16);
    const std::string salt_b = generate_salt_hex(16);
    TT_EQ(salt_a.size(), size_t{32});
    TT_CHECK(salt_a != salt_b);

    auto k1 = derive_db_key("same-password", salt_a, fast);
    auto k2 = derive_db_key("same-password", salt_a, fast);
    auto k3 = derive_db_key("same-password", salt_b, fast);
    auto k4 = derive_db_key("other-password", salt_a, fast);

    TT_IS_OK(k1);
    TT_IS_OK(k2);
    TT_IS_OK(k3);
    TT_IS_OK(k4);
    if (k1.is_err() || k2.is_err() || k3.is_err() || k4.is_err()) return;

    // 同密码同盐 -> 同密钥（否则用户重登就开不了自己的库了）
    TT_CHECK_MSG(k1.value().equals(k2.value()), "密钥派生不是确定性的！");
    // 同密码不同盐 -> 不同密钥（这是「两个人用同一个密码，库也互不可开」的基础）
    TT_CHECK_MSG(!k1.value().equals(k3.value()), "不同 salt 派生出了相同密钥！");
    // 同盐不同密码 -> 不同密钥
    TT_CHECK_MSG(!k1.value().equals(k4.value()), "不同密码派生出了相同密钥！");
    // 派生出来的密钥不能全零
    TT_CHECK(!k1.value().is_all_zero());

    // salt 长度必须严格校验
    TT_IS_ERR_CODE(derive_db_key("pw-1234", "abcd", fast), ErrorCode::InvalidArgument);
    TT_IS_ERR_CODE(derive_db_key("", salt_a, fast), ErrorCode::InvalidArgument);
}

TT_TEST(Crypto, 参数升级检测) {
    auto hash = hash_password("penhu-2026-a", crypto::KdfParams::interactive());
    TT_IS_OK(hash);
    if (hash.is_err()) return;

    // 用更弱的参数算出来的哈希，在更强参数下应当被标记为需要重算
    TT_CHECK(!password_needs_rehash(hash.value(), crypto::KdfParams::interactive()));
    TT_CHECK(password_needs_rehash(hash.value(), crypto::KdfParams::moderate()));
}

// -----------------------------------------------------------------------------
//  AEAD
// -----------------------------------------------------------------------------
TT_TEST(Crypto, AEAD往返与密文自描述) {
    Key256 key = Key256::random();
    const std::string plaintext = "sk-abcdef1234567890 这是一段要被加密的 API Key";

    auto sealed = seal(key, plaintext, "user:abc:llm_api_key");
    TT_IS_OK(sealed);
    if (sealed.is_err()) return;

    // 密文里绝不能出现明文片段
    const std::string blob_str(sealed.value().bytes.begin(), sealed.value().bytes.end());
    TT_CHECK(blob_str.find("sk-abcdef") == std::string::npos);
    TT_CHECK(blob_str.find("API Key") == std::string::npos);

    // 头部要如实记录算法，这样以后换算法老密文也解得开
    auto alg = peek_algorithm(sealed.value().bytes);
    TT_IS_OK(alg);
    if (alg.is_ok()) TT_CHECK(alg.value() == preferred_algorithm());

    auto opened = open(key, sealed.value().bytes, "user:abc:llm_api_key");
    TT_IS_OK(opened);
    if (opened.is_ok()) TT_EQ(opened.value(), plaintext);

    // AAD 变了必须解不开（防止把 A 用户的密文搬到 B 用户名下）
    TT_IS_ERR(open(key, sealed.value().bytes, "user:xyz:llm_api_key"));
    // 不给 AAD 也解不开
    TT_IS_ERR(open(key, sealed.value().bytes));
}

TT_TEST(Crypto, 相同明文两次加密得到不同密文) {
    Key256 key = Key256::random();
    const std::string plaintext = "同样的内容";

    auto s1 = seal(key, plaintext);
    auto s2 = seal(key, plaintext);
    TT_IS_OK(s1);
    TT_IS_OK(s2);
    if (s1.is_err() || s2.is_err()) return;

    // 如果两次密文相同，等于泄露了「这两条记录内容一样」这个信息，
    // 而且说明 nonce 复用了——AES-GCM 下 nonce 复用是致命错误。
    TT_CHECK_MSG(s1.value().bytes != s2.value().bytes,
                 "相同明文两次加密得到了相同密文，nonce 可能被复用！");
}

TT_TEST(Crypto, 密文篡改必须被检测) {
    Key256 key = Key256::random();
    auto sealed = seal(key, "敏感内容", "ctx");
    TT_IS_OK(sealed);
    if (sealed.is_err()) return;

    // 逐个位置翻位，每一处都必须导致解密失败
    int detected = 0;
    int total = 0;
    for (size_t i = 0; i < sealed.value().bytes.size(); ++i) {
        auto tampered = sealed.value().bytes;
        tampered[i] = static_cast<uint8_t>(tampered[i] ^ 0x01);
        ++total;
        auto r = open(key, tampered, "ctx");
        if (r.is_err()) ++detected;
    }
    TT_CHECK_MSG(detected == total,
                 "有 " + std::to_string(total - detected) + "/*" + std::to_string(total) +
                     " 处篡改未被检测到");
    TT_CHECK(total > 20);      // 密文至少得有这么长
}

TT_TEST(Crypto, 各种畸形密文都要报错而不是崩溃) {
    Key256 key = Key256::random();
    std::vector<uint8_t> empty;
    TT_IS_ERR(open(key, empty));
    TT_IS_ERR(open(key, std::vector<uint8_t>{1}));
    TT_IS_ERR(open(key, std::vector<uint8_t>{1, 1}));
    // 版本号不对
    TT_IS_ERR(open(key, std::vector<uint8_t>{9, 1, 12, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}));
    // 算法标识非法
    TT_IS_ERR(open(key, std::vector<uint8_t>{1, 99, 12, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}));
    // nonce 长度与算法不符
    TT_IS_ERR(open(key, std::vector<uint8_t>{1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}));
    // 全零密钥必须拒绝
    Key256 zero;
    auto sealed = seal(key, "x");
    if (sealed.is_ok()) TT_IS_ERR(open(zero, sealed.value().bytes));
}

TT_TEST(Crypto, Base64编解码) {
    const std::vector<std::vector<uint8_t>> samples = {
        {},
        {0x00},
        {0xFF},
        {0x01, 0x02, 0x03},
        {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x10, 0x20},
    };
    for (const auto& raw : samples) {
        const std::string encoded = base64_encode(raw);
        auto back = base64_decode(encoded);
        TT_IS_OK(back);
        if (back.is_ok()) TT_CHECK(back.value() == raw);
    }
    // 非法输入
    TT_IS_ERR(base64_decode("!!!!"));
    TT_IS_ERR(base64_decode("a"));

    // SealedBlob 的 Base64 往返
    Key256 key = Key256::random();
    auto sealed = seal(key, "中文内容 with ASCII", "aad");
    TT_IS_OK(sealed);
    if (sealed.is_err()) return;
    const std::string b64 = sealed.value().to_base64();
    auto parsed = SealedBlob::from_base64(b64);
    TT_IS_OK(parsed);
    if (parsed.is_ok()) {
        TT_CHECK(parsed.value().bytes == sealed.value().bytes);
        auto opened = open(key, parsed.value().bytes, "aad");
        TT_IS_OK(opened);
        if (opened.is_ok()) TT_EQ(opened.value(), std::string("中文内容 with ASCII"));
    }
}

// -----------------------------------------------------------------------------
//  会话
// -----------------------------------------------------------------------------
TT_TEST(Crypto, 会话生命周期) {
    SessionStore store(3600);
    TT_EQ(store.size(), size_t{0});

    auto key = std::make_shared<Key256>(Key256::random());
    auto created = store.create("user123", "penhu", key);
    TT_IS_OK(created);
    if (created.is_err()) return;
    const std::string token = created.value();
    TT_EQ(token.size(), size_t{64});
    TT_EQ(store.size(), size_t{1});

    auto resolved = store.resolve(token);
    TT_IS_OK(resolved);
    if (resolved.is_ok()) {
        TT_EQ(resolved.value().user_id, std::string("user123"));
        TT_EQ(resolved.value().username, std::string("penhu"));
        TT_CHECK(resolved.value().has_db_key());
        // 会话里的密钥必须和创建时那把是同一把内容
        TT_CHECK(resolved.value().db_key->equals(*key));
    }

    // 不存在的 token
    TT_IS_ERR_CODE(store.resolve("不存在的token"), ErrorCode::AuthFailed);
    // 空 token
    TT_IS_ERR_CODE(store.resolve(""), ErrorCode::AuthFailed);

    // 撤销
    TT_IS_OK(store.revoke(token));
    TT_IS_ERR_CODE(store.resolve(token), ErrorCode::AuthFailed);
    TT_EQ(store.size(), size_t{0});
    // 重复撤销不算错
    TT_IS_OK(store.revoke(token));
}

TT_TEST(Crypto, 会话过期与批量撤销) {
    SessionStore store(1);   // 1 秒
    auto key = std::make_shared<Key256>(Key256::random());

    auto t1 = store.create("userA", "a", key);
    auto t2 = store.create("userB", "b", key);
    TT_IS_OK(t1);
    TT_IS_OK(t2);
    TT_EQ(store.size(), size_t{2});
    TT_EQ(store.count_for_user("userA"), 1);

    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    // 过期后 resolve 必须失败
    TT_IS_ERR_CODE(store.resolve(t1.value()), ErrorCode::AuthFailed);
    // count_for_user 不应把过期会话算进去
    TT_EQ(store.count_for_user("userB"), 0);
    // purge 会把过期的清掉
    TT_CHECK(store.purge_expired() >= 1);
}
