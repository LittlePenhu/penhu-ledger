#pragma once
// =============================================================================
//  penhu/crypto/session.hpp
//  内存会话。登录成功后拿到 token，之后每个请求带 token 换回上下文。
//
//  数据流：
//    登录 -> derive_db_key(密码, salt) -> 得到一个 32B 密钥
//         -> 拿它打开 data/<uuid>.db (SQLCipher)
//         -> 密钥挂在 Session 上，随 token 存活
//    登出 / 超时 -> 会话销毁 -> SecureBuffer 析构 -> 密钥从内存消失
//
//  刻意的设计后果（必须说清楚，不能藏）：
//    * token 与密钥只存在于内存。进程重启后所有人都要重新登录。
//      这是有意的取舍——把密钥持久化到磁盘就等于把「密码学保护」降级成
//      「一个文件在不在」，那还不如不加密。
//    * token 在 map 里是明文键。所以它只能活在这个进程的内存里；
//      一旦要做持久化或多进程，必须先改成「只存 token 的 SHA-256」。
// =============================================================================

#include <cstdint>
#include <string>
#include <unordered_map>
#include <memory>
#include <mutex>
#include "penhu/crypto/secure_buffer.hpp"
#include "penhu/util/result.hpp"

namespace penhu::crypto {

/// 一次请求的认证上下文。db_key 只在会话有效期内被持有。
struct SessionContext {
    std::string token;
    std::string user_id;
    std::string username;
    std::shared_ptr<Key256> db_key;   // 可能为空（例如只读的元数据接口）
    int64_t     issued_at{0};
    int64_t     expires_at{0};

    bool has_db_key() const noexcept { return db_key != nullptr; }
};

class SessionStore {
public:
    /// ttl_seconds：空闲多久后失效；默认 12 小时。
    explicit SessionStore(int64_t ttl_seconds = 12 * 3600);
    ~SessionStore();

    SessionStore(const SessionStore&) = delete;
    SessionStore& operator=(const SessionStore&) = delete;

    /// 建会话。db_key 用 shared_ptr 传，便于多个会话共享同一把已打开的库密钥。
    Result<std::string> create(const std::string& user_id,
                              const std::string& username,
                              std::shared_ptr<Key256> db_key);

    /// 校验并续期。token 不存在 / 已过期都返回 AuthFailed。
    Result<SessionContext> resolve(const std::string& token);

    /// 不续期的只读查询（例如给日志用）
    Result<SessionContext> peek(const std::string& token) const;

    Status revoke(const std::string& token);

    /// 撤销某用户的全部会话（改密码 / 删账号时用）
    int revoke_all_for_user(const std::string& user_id);

    /// 某用户当前还有几个有效会话。登出时用它判断「能不能把库连接也关掉」。
    int count_for_user(const std::string& user_id) const;

    /// 清理过期项，返回清理条数
    int purge_expired();

    std::size_t size() const;

    int64_t  ttl_seconds() const noexcept { return ttl_seconds_; }
    void     set_ttl_seconds(int64_t ttl) noexcept { ttl_seconds_ = ttl; }

private:
    struct Entry {
        std::string user_id;
        std::string username;
        std::shared_ptr<Key256> db_key;
        int64_t issued_at{0};
        int64_t expires_at{0};
    };

    mutable std::mutex mutex_;
    std::unordered_map<std::string, Entry> entries_;
    int64_t ttl_seconds_;
};

}  // namespace penhu::crypto
