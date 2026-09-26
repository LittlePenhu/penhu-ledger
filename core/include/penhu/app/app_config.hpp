#pragma once
// =============================================================================
//  penhu/app/app_config.hpp
//  运行配置与数据目录布局。
//
//  目录结构（单一根目录，方便整目录备份或迁移）：
//    <data_dir>/
//      users.db                 明文。账号元数据：用户名、Argon2id 哈希、salt
//      data/<uuid>.db           SQLCipher 加密。每个账号一个，互不可见
//      data/<uuid>.db-wal       同上（WAL 模式产生的，内容同样加密）
//      backups/                 改密码 / 删账号前的自动备份
//      logs/                    penhu-ledger.log
//
//  把「明文库」和「加密库」放进不同子目录不是装饰：
//  将来如果要做「只备份钱的数据」，直接打包 data/ 就行，不会把账号哈希带出去。
// =============================================================================

#include <cstdint>
#include <string>

#include "penhu/crypto/password.hpp"
#include "penhu/util/result.hpp"

namespace penhu::app {

struct AppConfig {
    std::string       data_dir;                       // 根目录（绝对路径）
    std::string       users_db_filename{"users.db"};
    crypto::KdfParams kdf = crypto::KdfParams::moderate();
    int64_t           session_ttl_seconds{12 * 3600};
    std::string       log_level{"info"};

    /// 删账号时是否把加密账本留一份到 backups/。
    /// 默认 true：个人记账应用误删数据的代价远大于磁盘上多一个加密文件。
    /// 想要「彻底抹除」把它设为 false。
    bool keep_deleted_backup{true};

    /// 从指定目录构造（会解析成绝对路径）
    static Result<AppConfig> from_data_dir(const std::string& dir);

    /// 默认位置：%USERPROFILE%/PenHuLedger（Windows）/ $HOME/PenHuLedger
    static Result<AppConfig> default_config();

    std::string users_db_path() const;
    std::string data_subdir() const;
    std::string ledger_path(const std::string& db_filename) const;
    std::string backups_dir() const;
    std::string logs_dir() const;
    std::string log_file_path() const;

    /// 建好所有需要的目录。启动时调一次。
    Status ensure_directories() const;
};

}  // namespace penhu::app
