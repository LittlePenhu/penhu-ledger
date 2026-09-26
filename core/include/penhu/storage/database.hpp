#pragma once
// =============================================================================
//  penhu/storage/database.hpp
//  SQLite / SQLCipher 的 RAII 封装。
//
//  为什么自己包一层而不引 SQLiteCpp / sqlite_orm：
//   1) SQLCipher 与原版 SQLite 的 API 完全兼容，我们只需要 libsqlite3 那套，
//      薄封装（约 300 行）比引一个第三方抽象层更好排查；
//   2) 关键需求是「错误码映射」——SQLCipher 用错密钥时返回的是 SQLITE_NOTADB，
//      必须翻译成 CryptoFailure 并给出「密码错误」的人话提示，
//      否则用户会看到「file is not a database」这种完全无法理解的报错；
//   3) 所有 SQL 一律参数绑定，不做字符串拼接，这条要能一眼审出来。
//
//  线程模型：每个 Database 实例对应一个 sqlite3 连接（FULLMUTEX 序列化模式）。
//  本项目里每个登录会话独占自己的加密库连接，跨用户天然隔离。
// =============================================================================

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "penhu/crypto/secure_buffer.hpp"
#include "penhu/util/result.hpp"

struct sqlite3;
struct sqlite3_stmt;

namespace penhu::storage {

/// step() 的结果
enum class StepResult { Row, Done };

/// 把 SQLite 返回码翻译成本项目的错误码。
/// 这是这一层存在的最大理由，见文件头说明。
Error map_sqlite_error(int rc, std::string message, std::string context);

/// 预编译语句，RAII。
class Statement {
public:
    Statement() noexcept = default;
    ~Statement();
    Statement(Statement&& other) noexcept;
    Statement& operator=(Statement&& other) noexcept;
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    static Result<Statement> create(sqlite3* db, const std::string& sql);

    // ---- 参数绑定（下标从 1 开始，和 SQLite 一致）----
    Status bind(int index, std::nullptr_t);
    Status bind(int index, int value);
    Status bind(int index, int64_t value);
    Status bind(int index, double value);
    Status bind(int index, bool value);
    Status bind(int index, const std::string& value);
    Status bind(int index, std::string_view value);

    // ---- 执行 ----
    Result<StepResult> step();
    Status reset();

    /// 遍历整个结果集：fn 对每一行调用一次，返回错误即中止。
    template <class F>
    Status each_row(F&& fn) {
        for (;;) {
            auto r = step();
            if (r.is_err()) return r.error();
            if (r.value() == StepResult::Done) break;
            Status s = fn(*this);
            if (s.is_err()) return s;
        }
        return status_ok();
    }

    // ---- 列读取（下标从 0 开始）----
    int         column_count() const;
    bool        column_is_null(int column) const;
    int64_t     column_int64(int column) const;
    int         column_int(int column) const;
    bool        column_bool(int column) const;
    double      column_double(int column) const;
    std::string column_text(int column) const;
    /// 列为 NULL 时返回 fallback（用于可空字段，省掉调用方到处写判空）
    std::string column_text_or(int column, std::string fallback) const;

    bool valid() const noexcept { return stmt_ != nullptr; }

private:
    void destroy() noexcept;
    sqlite3_stmt* stmt_{nullptr};
    std::string   sql_;   // 出错时把 SQL 一起报出来，不然很难定位
};

/// 数据库连接，RAII。
class Database {
public:
    Database() noexcept = default;
    ~Database();
    Database(Database&& other) noexcept;
    Database& operator=(Database&& other) noexcept;
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    /// 打开明文库。**只用于 users.db**，里面绝不允许出现金融数据。
    static Result<Database> open_plain(const std::string& path);

    /// 打开 SQLCipher 整库加密的库。raw_key 必须是通过 Argon2id 派生的 32 字节。
    /// 注意：SQLCipher 会跳过它自带的 KDF（我们传的是 raw key），
    /// 所以密钥强度完全由 Argon2id 参数决定，见 crypto/password.hpp。
    static Result<Database> open_encrypted(const std::string& path,
                                          const crypto::Key256& raw_key);

    Status execute(const std::string& sql);
    Result<Statement> prepare(const std::string& sql);

    /// 把「原始 SQL 字符串」拿出来给迁移用（内部走 execute，仍然不接受外部输入）
    Status execute_script(const std::string& sql);

    Status begin();
    Status commit();
    Status rollback();

    /// 作用域事务。析构时若既没 commit 也没 rollback，自动 ROLLBACK。
    /// 记账写入涉及「插记录 + 更新报表缓存」多步，没有这个很容易留下半截状态。
    class Transaction {
    public:
        explicit Transaction(Database& db);
        ~Transaction();
        Transaction(const Transaction&) = delete;
        Transaction& operator=(const Transaction&) = delete;
        Transaction(Transaction&& other) noexcept;
        Transaction& operator=(Transaction&& other) noexcept;

        Status commit();
        Status rollback();
        bool   started() const noexcept { return started_; }

    private:
        Database* db_{nullptr};
        bool      started_{false};
        bool      finished_{false};
    };

    /// 改密码：用新密钥重加密整个库
    Status rekey(const crypto::Key256& new_key);

    Status  vacuum();
    int     user_version();
    Status  set_user_version(int version);

    int64_t last_insert_rowid();
    int     changes();

    bool        is_open() const noexcept { return db_ != nullptr; }
    std::string path() const { return path_; }

    bool                     table_exists(const std::string& table_name);
    std::vector<std::string> table_columns(const std::string& table_name);

    sqlite3* raw() noexcept { return db_; }

private:
    void close() noexcept;

    sqlite3*    db_{nullptr};
    std::string path_;
    bool        encrypted_{false};
};

}  // namespace penhu::storage
