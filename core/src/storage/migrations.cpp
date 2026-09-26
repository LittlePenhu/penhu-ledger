#include "penhu/storage/migrations.hpp"
#include "penhu/util/log.hpp"

namespace penhu::storage {
namespace {

// -----------------------------------------------------------------------------
//  users.db（明文）—— 只放账号元数据，绝不放金额
// -----------------------------------------------------------------------------
Status apply_users_v1(Database& db) {
    PENHU_RETURN_IF_ERROR(db.execute(R"SQL(
        CREATE TABLE IF NOT EXISTS users (
            id            TEXT    PRIMARY KEY NOT NULL,
            username      TEXT    NOT NULL,
            username_ci   TEXT    NOT NULL UNIQUE,   -- 小写形式，用于不区分大小写的唯一约束
            password_hash TEXT    NOT NULL,          -- libsodium Argon2id 自描述字符串
            db_salt_hex   TEXT    NOT NULL,          -- 16B，派生该用户 SQLCipher 密钥用
            db_filename   TEXT    NOT NULL,          -- 相对 data_dir
            created_at    INTEGER NOT NULL,
            last_login_at INTEGER NOT NULL DEFAULT 0,
            is_active     INTEGER NOT NULL DEFAULT 1
        );
    )SQL"));

    PENHU_RETURN_IF_ERROR(db.execute(
        "CREATE INDEX IF NOT EXISTS idx_users_username_ci ON users(username_ci);"));

    PENHU_RETURN_IF_ERROR(db.execute(R"SQL(
        CREATE TABLE IF NOT EXISTS app_meta (
            key   TEXT PRIMARY KEY NOT NULL,
            value TEXT NOT NULL
        );
    )SQL"));

    PENHU_RETURN_IF_ERROR(db.execute(R"SQL(
        INSERT OR IGNORE INTO app_meta(key, value) VALUES
            ('schema_owner', 'penhu-ledger'),
            ('data_class',   'account-metadata-only');
    )SQL"));

    return db.set_user_version(1);
}

// -----------------------------------------------------------------------------
//  <uuid>.db（加密）—— 用户的钱都在这里
// -----------------------------------------------------------------------------
Status apply_ledger_v1(Database& db) {
    // meta 表存 owner_user_id。开库时校验一次，用于区分两种完全不同的故障：
    //   「密钥不对」      -> 密码错
    //   「密钥对但换人了」-> 我们的代码打开了错误的库文件
    // 没有这张表的话，后者会伪装成前者，排查成本极高。
    PENHU_RETURN_IF_ERROR(db.execute(R"SQL(
        CREATE TABLE IF NOT EXISTS meta (
            key   TEXT PRIMARY KEY NOT NULL,
            value TEXT NOT NULL
        );
    )SQL"));

    PENHU_RETURN_IF_ERROR(db.execute(R"SQL(
        CREATE TABLE IF NOT EXISTS categories (
            id         TEXT    PRIMARY KEY NOT NULL,
            name       TEXT    NOT NULL,
            icon       TEXT    NOT NULL DEFAULT '',
            color_hex  TEXT    NOT NULL DEFAULT '#78909C',
            direction  INTEGER NOT NULL DEFAULT 0,   -- 0=支出 1=收入
            sort_order INTEGER NOT NULL DEFAULT 0,
            is_builtin INTEGER NOT NULL DEFAULT 0,
            is_active  INTEGER NOT NULL DEFAULT 1
        );
    )SQL"));

    // 同名同方向只允许一个，否则统计图里会出现两个「餐饮」把占比劈成两半
    PENHU_RETURN_IF_ERROR(db.execute(
        "CREATE UNIQUE INDEX IF NOT EXISTS idx_categories_name_dir ON categories(name, direction);"));

    PENHU_RETURN_IF_ERROR(db.execute(R"SQL(
        CREATE TABLE IF NOT EXISTS records (
            id           TEXT    PRIMARY KEY NOT NULL,
            amount_minor INTEGER NOT NULL CHECK (amount_minor > 0),  -- 恒为正，方向看 direction
            direction    INTEGER NOT NULL CHECK (direction IN (0, 1)),
            category_id  TEXT    NOT NULL REFERENCES categories(id) ON DELETE RESTRICT,
            date_key     INTEGER NOT NULL,   -- 1970-01-01 起的天数。区间查询和索引都靠它
            note         TEXT    NOT NULL DEFAULT '',
            created_at   INTEGER NOT NULL,
            updated_at   INTEGER NOT NULL
        );
    )SQL"));

    // 三个索引对应三类高频查询：按日区间、按分类、按方向
    PENHU_RETURN_IF_ERROR(db.execute(
        "CREATE INDEX IF NOT EXISTS idx_records_date ON records(date_key);"));
    PENHU_RETURN_IF_ERROR(db.execute(
        "CREATE INDEX IF NOT EXISTS idx_records_cat_date ON records(category_id, date_key);"));
    PENHU_RETURN_IF_ERROR(db.execute(
        "CREATE INDEX IF NOT EXISTS idx_records_dir_date ON records(direction, date_key);"));

    return db.set_user_version(1);
}

Status apply_ledger_v2(Database& db) {
    // 每日报告的存档。LLM 调用有成本（本地要占用算力，云端要花钱），
    // 生成过就存下来，同一天重复打开界面不该再打一次模型。
    PENHU_RETURN_IF_ERROR(db.execute(R"SQL(
        CREATE TABLE IF NOT EXISTS reports (
            id         TEXT    PRIMARY KEY NOT NULL,
            date_key   INTEGER NOT NULL,
            kind       TEXT    NOT NULL,          -- daily / weekly / monthly
            provider   TEXT    NOT NULL,          -- local-llama / cloud-openai / rule-based
            model      TEXT    NOT NULL DEFAULT '',
            content    TEXT    NOT NULL,
            stats_json TEXT    NOT NULL DEFAULT '{}',
            created_at INTEGER NOT NULL
        );
    )SQL"));
    PENHU_RETURN_IF_ERROR(db.execute(
        "CREATE UNIQUE INDEX IF NOT EXISTS idx_reports_date_kind ON reports(date_key, kind);"));

    // 用户级设置（LLM 配置等）。encrypted=1 表示 value 是 Base64 的 AEAD 密文。
    PENHU_RETURN_IF_ERROR(db.execute(R"SQL(
        CREATE TABLE IF NOT EXISTS user_settings (
            key       TEXT    PRIMARY KEY NOT NULL,
            value     TEXT    NOT NULL,
            encrypted INTEGER NOT NULL DEFAULT 0
        );
    )SQL"));

    return db.set_user_version(2);
}

}  // namespace

Status migrate_users_db(Database& db) {
    const int current = db.user_version();
    if (current > kUsersSchemaVersion) {
        return status_err(ErrorCode::StorageFailure,
                          "users.db 的 schema 版本(" + std::to_string(current) +
                              ")高于本程序支持的版本(" + std::to_string(kUsersSchemaVersion) +
                              ")，请升级程序而不是降级数据",
                          "migrate_users_db");
    }
    if (current == kUsersSchemaVersion) return status_ok();

    Logger& log = Logger::default_logger();
    log.info("users.db schema " + std::to_string(current) + " -> " +
             std::to_string(kUsersSchemaVersion));

    Database::Transaction tx(db);
    if (!tx.started()) {
        return status_err(ErrorCode::StorageFailure, "无法开启迁移事务", "migrate_users_db");
    }
    if (current < 1) {
        PENHU_RETURN_IF_ERROR(apply_users_v1(db));
    }
    return tx.commit();
}

Status migrate_ledger_db(Database& db) {
    const int current = db.user_version();
    if (current > kLedgerSchemaVersion) {
        return status_err(ErrorCode::StorageFailure,
                          "账本库 schema 版本(" + std::to_string(current) +
                              ")高于本程序支持(" + std::to_string(kLedgerSchemaVersion) + ")",
                          "migrate_ledger_db");
    }
    if (current == kLedgerSchemaVersion) return status_ok();

    Logger& log = Logger::default_logger();
    log.info("ledger schema " + std::to_string(current) + " -> " +
             std::to_string(kLedgerSchemaVersion));

    Database::Transaction tx(db);
    if (!tx.started()) {
        return status_err(ErrorCode::StorageFailure, "无法开启迁移事务", "migrate_ledger_db");
    }
    if (current < 1) {
        PENHU_RETURN_IF_ERROR(apply_ledger_v1(db));
    }
    if (current < 2) {
        PENHU_RETURN_IF_ERROR(apply_ledger_v2(db));
    }
    return tx.commit();
}

}  // namespace penhu::storage
