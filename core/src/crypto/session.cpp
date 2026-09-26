#include "penhu/crypto/session.hpp"
#include "penhu/crypto/password.hpp"
#include "penhu/domain/date.hpp"

#include <algorithm>
#include <vector>

namespace penhu::crypto {

constexpr std::size_t kTokenBytes = 32;   // 256-bit，十六进制后 64 字符

SessionStore::SessionStore(int64_t ttl_seconds)
    : ttl_seconds_(ttl_seconds > 0 ? ttl_seconds : 12 * 3600) {}

SessionStore::~SessionStore() {
    // 显式销毁所有条目：让每个会话的密钥走到 SecureBuffer 的析构，
    // 而不是依赖 unordered_map 的默认析构顺序。
    std::lock_guard<std::mutex> guard(mutex_);
    for (auto& [token, entry] : entries_) {
        if (entry.db_key) {
            entry.db_key->wipe();
            entry.db_key.reset();
        }
        // token 是 unordered_map 的 key，类型是 const std::string，
        // 所以 data() 给的是 const char*。这里要擦掉的是我们自己持有的那份
        // 内存（这个 map 归 SessionStore 所有），用 const_cast 是正当的 ——
        // 目的恰恰是**不留下**这份明文，而不是去改一个逻辑上常量的东西。
        secure_wipe(const_cast<char*>(token.data()), token.size());
    }
    entries_.clear();
}

Result<std::string> SessionStore::create(const std::string& user_id,
                                        const std::string& username,
                                        std::shared_ptr<Key256> db_key) {
    if (user_id.empty() || username.empty()) {
        return Result<std::string>::fail(ErrorCode::InvalidArgument,
                                        "创建会话需要非空的 user_id 与 username",
                                        "SessionStore::create");
    }

    const std::string token = random_hex(kTokenBytes);
    if (token.size() != kTokenBytes * 2) {
        return Result<std::string>::fail(ErrorCode::CryptoFailure,
                                        "会话令牌生成失败（随机数源异常）",
                                        "SessionStore::create");
    }

    const int64_t now = timeutil::now_epoch_seconds();

    std::lock_guard<std::mutex> guard(mutex_);
    Entry entry;
    entry.user_id    = user_id;
    entry.username   = username;
    entry.db_key     = std::move(db_key);
    entry.issued_at  = now;
    entry.expires_at = now + ttl_seconds_;
    entries_.emplace(token, std::move(entry));

    return Result<std::string>(token);
}

Result<SessionContext> SessionStore::resolve(const std::string& token) {
    const int64_t now = timeutil::now_epoch_seconds();

    std::lock_guard<std::mutex> guard(mutex_);
    auto it = entries_.find(token);
    if (it == entries_.end()) {
        return Result<SessionContext>::fail(ErrorCode::AuthFailed,
                                           "会话不存在或已登出（服务可能重启过）",
                                           "SessionStore::resolve");
    }
    if (it->second.expires_at <= now) {
        if (it->second.db_key) it->second.db_key->wipe();
        entries_.erase(it);
        return Result<SessionContext>::fail(ErrorCode::AuthFailed,
                                           "会话已过期，请重新登录",
                                           "SessionStore::resolve");
    }

    // 滑动续期：只要在用，就不该被踢下线
    it->second.expires_at = now + ttl_seconds_;

    SessionContext ctx;
    ctx.token      = token;
    ctx.user_id    = it->second.user_id;
    ctx.username   = it->second.username;
    ctx.db_key     = it->second.db_key;
    ctx.issued_at  = it->second.issued_at;
    ctx.expires_at = it->second.expires_at;
    return Result<SessionContext>(std::move(ctx));
}

Result<SessionContext> SessionStore::peek(const std::string& token) const {
    const int64_t now = timeutil::now_epoch_seconds();

    std::lock_guard<std::mutex> guard(mutex_);
    auto it = entries_.find(token);
    if (it == entries_.end() || it->second.expires_at <= now) {
        return Result<SessionContext>::fail(ErrorCode::AuthFailed,
                                           "会话不存在或已过期",
                                           "SessionStore::peek");
    }
    SessionContext ctx;
    ctx.token      = token;
    ctx.user_id    = it->second.user_id;
    ctx.username   = it->second.username;
    ctx.db_key     = it->second.db_key;
    ctx.issued_at  = it->second.issued_at;
    ctx.expires_at = it->second.expires_at;
    return Result<SessionContext>(std::move(ctx));
}

Status SessionStore::revoke(const std::string& token) {
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = entries_.find(token);
    if (it == entries_.end()) {
        // 重复登出不算错误：用户点了两次登出按钮是很正常的事
        return status_ok();
    }
    if (it->second.db_key) it->second.db_key->wipe();
    entries_.erase(it);
    return status_ok();
}

int SessionStore::revoke_all_for_user(const std::string& user_id) {
    std::lock_guard<std::mutex> guard(mutex_);
    int removed = 0;
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->second.user_id == user_id) {
            if (it->second.db_key) it->second.db_key->wipe();
            it = entries_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

int SessionStore::count_for_user(const std::string& user_id) const {
    const int64_t now = timeutil::now_epoch_seconds();
    std::lock_guard<std::mutex> guard(mutex_);
    int count = 0;
    for (const auto& [token, entry] : entries_) {
        (void)token;
        if (entry.user_id == user_id && entry.expires_at > now) ++count;
    }
    return count;
}

int SessionStore::purge_expired() {
    const int64_t now = timeutil::now_epoch_seconds();
    std::lock_guard<std::mutex> guard(mutex_);
    int removed = 0;
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->second.expires_at <= now) {
            if (it->second.db_key) it->second.db_key->wipe();
            it = entries_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

std::size_t SessionStore::size() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return entries_.size();
}

}  // namespace penhu::crypto
