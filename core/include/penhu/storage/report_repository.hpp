#pragma once
// =============================================================================
//  penhu/storage/report_repository.hpp
//  已生成报告的存档 + 用户级加密设置。
//
//  为什么要存档报告：
//    LLM 调用有成本——本地要占算力（你那台 4060 还在训练），云端要花钱。
//    同一天同一种报告重复生成是纯浪费。所以「生成」的语义是 upsert：
//    已有存档就复用，用户想刷新要显式要求。
//
//  为什么要一个「加密设置」表：
//    云端 LLM 的 API Key 不能明文落盘。这里用会话里的 SQLCipher 密钥
//    再做一层 AEAD（见 crypto/cipher.hpp），把密文 Base64 后存进
//    user_settings.encrypted = 1 的行里。
//    也就是说：整个 data 目录被拷走，AI Key 也拿不到。
// =============================================================================

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "penhu/crypto/cipher.hpp"
#include "penhu/domain/date.hpp"
#include "penhu/storage/database.hpp"

namespace penhu::storage {

/// 报告类型。用字符串存库，便于以后加 "yearly" 而不影响老数据。
namespace report_kind {
constexpr const char* kDaily   = "daily";
constexpr const char* kWeekly  = "weekly";
constexpr const char* kMonthly = "monthly";
bool is_valid(const std::string& kind);
}  // namespace report_kind

struct StoredReport {
    std::string id;
    Date        date;            // 报告锚点日期（每日=当天，每周=周内任意一天，每月=月内任意一天）
    std::string kind;            // daily / weekly / monthly
    std::string provider;        // rule-based / local-llama / cloud-openai
    std::string model;
    std::string content;         // 模型或模板产出的正文
    std::string stats_json;      // 生成时用的结构化数字快照（便于回溯「当时是什么数据」）
    int64_t     created_at{0};
};

class ReportRepository {
public:
    explicit ReportRepository(Database& db) : db_(db) {}

    /// 同日期同类型覆盖写入
    Result<StoredReport> upsert(const StoredReport& report);

    Result<StoredReport> find(const Date& anchor, const std::string& kind);

    /// 最近若干份，按创建时间倒序
    Result<std::vector<StoredReport>> list_recent(const std::string& kind, int limit = 30);

    Status remove(const std::string& id);

    int64_t count();

private:
    static StoredReport row_to_report(Statement& stmt);
    Database& db_;
};

// -----------------------------------------------------------------------------
//  用户级设置（含加密项）
// -----------------------------------------------------------------------------

/// 用户设置访问。加密项用会话密钥做 AEAD，AAD 绑定到 key 名，
/// 这样把「api_key」的密文挪到「其他键」下面也解不开。
class SettingsRepository {
public:
    explicit SettingsRepository(Database& db) : db_(db) {}

    /// 存明文设置（不要用它存秘密）
    Status set_plain(const std::string& key, const std::string& value);
    Result<std::optional<std::string>> get_plain(const std::string& key);

    /// 存加密设置
    Status set_secret(const std::string& key,
                      const std::string& plaintext,
                      const crypto::Key256& db_key);
    /// 读加密设置。若该键存的是明文（encrypted=0），会拒绝并报错，
    /// 避免「以为读到的是解出来的秘密」。
    Result<std::optional<std::string>> get_secret(const std::string& key,
                                                 const crypto::Key256& db_key);

    Status remove(const std::string& key);

    /// 列出所有键名与是否加密（不含值）
    Result<std::vector<std::pair<std::string, bool>>> list_keys();

private:
    Database& db_;
};

}  // namespace penhu::storage
