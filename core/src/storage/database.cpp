#include "penhu/storage/database.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cstring>
#include <sstream>

namespace penhu::storage {

// -----------------------------------------------------------------------------
//  错误映射
// -----------------------------------------------------------------------------

Error map_sqlite_error(int rc, std::string message, std::string context) {
    ErrorCode code = ErrorCode::StorageFailure;

    switch (rc & 0xFF) {   // 抹掉扩展码，只看主码
        case SQLITE_CONSTRAINT:
            // 唯一索引冲突（用户名重复）走这里
            code = ErrorCode::AlreadyExists;
            break;
        case SQLITE_NOTADB:
        case SQLITE_CORRUPT:
            // SQLCipher 用错密钥时的典型返回。必须翻译成人话，
            // 否则用户看到 "file is not a database" 会以为文件坏了。
            code = ErrorCode::CryptoFailure;
            if (message.empty()) {
                message = "数据库无法解密（密码错误，或文件已损坏）";
            }
            break;
        case SQLITE_CANTOPEN:
        case SQLITE_IOERR:
        case SQLITE_READONLY:
        case SQLITE_FULL:
            code = ErrorCode::StorageFailure;
            break;
        case SQLITE_MISUSE:
        case SQLITE_RANGE:
        case SQLITE_MISMATCH:
        case SQLITE_FORMAT:
            code = ErrorCode::Internal;
            break;
        case SQLITE_BUSY:
        case SQLITE_LOCKED:
            code = ErrorCode::StorageFailure;
            if (message.empty()) message = "数据库被占用，请稍后重试";
            break;
        default:
            code = ErrorCode::StorageFailure;
            break;
    }

    if (message.empty()) {
        message = sqlite3_errstr(rc);
    }
    return Error{code, std::move(message), std::move(context)};
}

namespace {

/// 用 sqlite3_errmsg 拿到更具体的信息
std::string db_errmsg(sqlite3* db) {
    if (db == nullptr) return {};
    const char* msg = sqlite3_errmsg(db);
    return msg != nullptr ? std::string(msg) : std::string{};
}

/// 执行一条不接受外部输入的 PRAGMA / DDL
Status exec_simple(sqlite3* db, const std::string& sql, const char* what) {
    char* err = nullptr;
    const int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string msg = err != nullptr ? std::string(err) : db_errmsg(db);
        if (err != nullptr) sqlite3_free(err);
        return status_err(map_sqlite_error(rc, std::move(msg), what).code,
                          map_sqlite_error(rc, msg, what).message,
                          std::string(what) + " :: " + sql);
    }
    if (err != nullptr) sqlite3_free(err);
    return status_ok();
}

}  // namespace

// -----------------------------------------------------------------------------
//  Statement
// -----------------------------------------------------------------------------

Statement::~Statement() { destroy(); }

Statement::Statement(Statement&& other) noexcept
    : stmt_(other.stmt_), sql_(std::move(other.sql_)) {
    other.stmt_ = nullptr;
}

Statement& Statement::operator=(Statement&& other) noexcept {
    if (this != &other) {
        destroy();
        stmt_ = other.stmt_;
        sql_ = std::move(other.sql_);
        other.stmt_ = nullptr;
    }
    return *this;
}

void Statement::destroy() noexcept {
    if (stmt_ != nullptr) {
        sqlite3_finalize(stmt_);
        stmt_ = nullptr;
    }
}

Result<Statement> Statement::create(sqlite3* db, const std::string& sql) {
    if (db == nullptr) {
        return Result<Statement>::fail(ErrorCode::Internal,
                                      "在未打开的数据库上准备语句", "Statement::create");
    }
    sqlite3_stmt* stmt = nullptr;
    const int rc = sqlite3_prepare_v2(db, sql.c_str(), static_cast<int>(sql.size()) + 1,
                                      &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return Result<Statement>::fail(
            map_sqlite_error(rc, db_errmsg(db), "Statement::create").code,
            db_errmsg(db),
            std::string("SQL: ") + sql);
    }
    Statement out;
    out.stmt_ = stmt;
    out.sql_ = sql;
    return Result<Statement>(std::move(out));
}

Status Statement::bind(int index, std::nullptr_t) {
    const int rc = sqlite3_bind_null(stmt_, index);
    if (rc != SQLITE_OK) {
        return status_err(ErrorCode::Internal,
                          "绑定 NULL 到参数 " + std::to_string(index) + " 失败",
                          std::string("SQL: ") + sql_);
    }
    return status_ok();
}

Status Statement::bind(int index, int value) {
    const int rc = sqlite3_bind_int(stmt_, index, value);
    if (rc != SQLITE_OK) {
        return status_err(ErrorCode::Internal,
                          "绑定整数到参数 " + std::to_string(index) + " 失败",
                          std::string("SQL: ") + sql_);
    }
    return status_ok();
}

Status Statement::bind(int index, int64_t value) {
    const int rc = sqlite3_bind_int64(stmt_, index, static_cast<sqlite3_int64>(value));
    if (rc != SQLITE_OK) {
        return status_err(ErrorCode::Internal,
                          "绑定 64 位整数到参数 " + std::to_string(index) + " 失败",
                          std::string("SQL: ") + sql_);
    }
    return status_ok();
}

Status Statement::bind(int index, double value) {
    const int rc = sqlite3_bind_double(stmt_, index, value);
    if (rc != SQLITE_OK) {
        return status_err(ErrorCode::Internal,
                          "绑定浮点到参数 " + std::to_string(index) + " 失败",
                          std::string("SQL: ") + sql_);
    }
    return status_ok();
}

Status Statement::bind(int index, bool value) {
    return bind(index, static_cast<int>(value ? 1 : 0));
}

Status Statement::bind(int index, const std::string& value) {
    return bind(index, std::string_view(value));
}

Status Statement::bind(int index, std::string_view value) {
    // SQLITE_TRANSIENT：让 SQLite 自己拷贝一份。
    // 用 SQLITE_STATIC 会要求这块内存在 step() 前一直有效——太容易踩，
    // 而且我们绑定的常常是临时字符串。
    const int rc = sqlite3_bind_text(stmt_, index, value.data(),
                                     static_cast<int>(value.size()),
                                     SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) {
        return status_err(ErrorCode::Internal,
                          "绑定文本到参数 " + std::to_string(index) + " 失败",
                          std::string("SQL: ") + sql_);
    }
    return status_ok();
}

Result<StepResult> Statement::step() {
    const int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW)  return Result<StepResult>(StepResult::Row);
    if (rc == SQLITE_DONE) return Result<StepResult>(StepResult::Done);

    return Result<StepResult>::fail(
        map_sqlite_error(rc, {}, "Statement::step").code,
        sqlite3_errstr(rc),
        std::string("SQL: ") + sql_);
}

Status Statement::reset() {
    const int rc = sqlite3_reset(stmt_);
    if (rc != SQLITE_OK) {
        return status_err(ErrorCode::Internal, "重置语句失败", std::string("SQL: ") + sql_);
    }
    sqlite3_clear_bindings(stmt_);
    return status_ok();
}

int Statement::column_count() const {
    return stmt_ != nullptr ? sqlite3_column_count(stmt_) : 0;
}

bool Statement::column_is_null(int column) const {
    return stmt_ == nullptr || sqlite3_column_type(stmt_, column) == SQLITE_NULL;
}

int64_t Statement::column_int64(int column) const {
    if (stmt_ == nullptr) return 0;
    return static_cast<int64_t>(sqlite3_column_int64(stmt_, column));
}

int Statement::column_int(int column) const {
    return static_cast<int>(column_int64(column));
}

bool Statement::column_bool(int column) const {
    return column_int64(column) != 0;
}

double Statement::column_double(int column) const {
    if (stmt_ == nullptr) return 0.0;
    return sqlite3_column_double(stmt_, column);
}

std::string Statement::column_text(int column) const {
    if (stmt_ == nullptr) return {};
    const unsigned char* text = sqlite3_column_text(stmt_, column);
    if (text == nullptr) return {};
    const int bytes = sqlite3_column_bytes(stmt_, column);
    return std::string(reinterpret_cast<const char*>(text), static_cast<size_t>(bytes));
}

std::string Statement::column_text_or(int column, std::string fallback) const {
    if (column_is_null(column)) return fallback;
    return column_text(column);
}

// -----------------------------------------------------------------------------
//  Database
// -----------------------------------------------------------------------------

Database::~Database() { close(); }

Database::Database(Database&& other) noexcept
    : db_(other.db_), path_(std::move(other.path_)), encrypted_(other.encrypted_) {
    other.db_ = nullptr;
}

Database& Database::operator=(Database&& other) noexcept {
    if (this != &other) {
        close();
        db_ = other.db_;
        path_ = std::move(other.path_);
        encrypted_ = other.encrypted_;
        other.db_ = nullptr;
    }
    return *this;
}

void Database::close() noexcept {
    if (db_ != nullptr) {
        // 关闭前显式清掉页缓存，尽量缩短解密后数据在内存里的停留时间
        if (encrypted_) {
            sqlite3_exec(db_, "PRAGMA cipher_memory_security = ON;", nullptr, nullptr, nullptr);
        }
        sqlite3_close_v2(db_);
        db_ = nullptr;
    }
}

Result<Database> Database::open_plain(const std::string& path) {
    Database out;
    out.path_ = path;
    out.encrypted_ = false;

    const int rc = sqlite3_open_v2(
        path.c_str(), &out.db_,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr);

    if (rc != SQLITE_OK) {
        const std::string msg = db_errmsg(out.db_);
        if (out.db_ != nullptr) { sqlite3_close_v2(out.db_); out.db_ = nullptr; }
        return Result<Database>::fail(map_sqlite_error(rc, msg, "open_plain").code,
                                     msg, "打开明文库失败: " + path);
    }

    auto pragmas = { std::pair<const char*, const char*>{ "PRAGMA foreign_keys = ON;", "fk" },
                     std::pair<const char*, const char*>{ "PRAGMA busy_timeout = 5000;", "timeout" },
                     std::pair<const char*, const char*>{ "PRAGMA journal_mode = WAL;", "wal" } };
    for (const auto& [sql, what] : pragmas) {
        auto st = exec_simple(out.db_, sql, what);
        if (st.is_err()) return st.error();
    }

    return Result<Database>(std::move(out));
}

Result<Database> Database::open_encrypted(const std::string& path,
                                         const crypto::Key256& raw_key) {
    if (raw_key.is_all_zero()) {
        return Result<Database>::fail(ErrorCode::CryptoFailure,
                                     "拒绝用全零密钥打开加密库", "open_encrypted");
    }

    Database out;
    out.path_ = path;
    out.encrypted_ = true;

    const int rc = sqlite3_open_v2(
        path.c_str(), &out.db_,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr);

    if (rc != SQLITE_OK) {
        const std::string msg = db_errmsg(out.db_);
        if (out.db_ != nullptr) { sqlite3_close_v2(out.db_); out.db_ = nullptr; }
        return Result<Database>::fail(map_sqlite_error(rc, msg, "open_encrypted").code,
                                     msg, "打开加密库失败: " + path);
    }

    // 用 raw key 形式：SQLCipher 跳过自带 KDF，直接用这 32 字节做密钥。
    // 这样「密钥强度」这件事完全由我们的 Argon2id 参数掌握，不依赖 SQLCipher 默认的
    // PBKDF2 迭代数，审计起来只有一个地方要看。
    const std::string key_pragma = "PRAGMA key = \"x'" + raw_key.to_hex() + "'\";";
    auto st = exec_simple(out.db_, key_pragma, "set-key");
    if (st.is_err()) return st.error();

    // 立刻做一次真实读取来验证密钥。SQLCipher 是惰性的：
    // 不碰数据页就不会知道密钥对不对，等到第一次 SELECT 才报错，
    // 那样错误会出现在离「登录」很远的地方，非常难排查。
    {
        sqlite3_stmt* probe = nullptr;
        const int prc = sqlite3_prepare_v2(out.db_, "SELECT count(*) FROM sqlite_master;",
                                          -1, &probe, nullptr);
        if (prc != SQLITE_OK) {
            const std::string msg = db_errmsg(out.db_);
            if (probe != nullptr) sqlite3_finalize(probe);
            if (out.db_ != nullptr) { sqlite3_close_v2(out.db_); out.db_ = nullptr; }
            return Result<Database>::fail(map_sqlite_error(prc, msg, "verify-key").code, msg,
                                         "验证加密库密钥时失败（文件可能不是本应用创建）: " + path);
        }
        const int src = sqlite3_step(probe);
        sqlite3_finalize(probe);
        if (src != SQLITE_ROW) {
            const std::string msg = db_errmsg(out.db_);
            if (out.db_ != nullptr) { sqlite3_close_v2(out.db_); out.db_ = nullptr; }
            return Result<Database>::fail(
                ErrorCode::CryptoFailure,
                "密码错误或数据文件已损坏（数据库无法解密）",
                "密钥验证失败: " + path);
        }
    }

    auto pragmas = { std::pair<const char*, const char*>{ "PRAGMA foreign_keys = ON;", "fk" },
                     std::pair<const char*, const char*>{ "PRAGMA busy_timeout = 5000;", "timeout" },
                     std::pair<const char*, const char*>{ "PRAGMA journal_mode = WAL;", "wal" },
                     std::pair<const char*, const char*>{ "PRAGMA cipher_memory_security = ON;", "memsec" } };
    for (const auto& [sql, what] : pragmas) {
        auto s = exec_simple(out.db_, sql, what);
        if (s.is_err()) return s.error();
    }

    return Result<Database>(std::move(out));
}

Status Database::execute(const std::string& sql) {
    if (db_ == nullptr) {
        return status_err(ErrorCode::Internal, "数据库未打开", "Database::execute");
    }
    return exec_simple(db_, sql, "Database::execute");
}

Status Database::execute_script(const std::string& sql) {
    return execute(sql);
}

Result<Statement> Database::prepare(const std::string& sql) {
    if (db_ == nullptr) {
        return Result<Statement>::fail(ErrorCode::Internal, "数据库未打开", "Database::prepare");
    }
    return Statement::create(db_, sql);
}

Status Database::begin() {
    return execute("BEGIN IMMEDIATE;");
}

Status Database::commit() {
    return execute("COMMIT;");
}

Status Database::rollback() {
    return execute("ROLLBACK;");
}

Database::Transaction::Transaction(Database& db) : db_(&db) {
    started_ = db.begin().is_ok();
}

Database::Transaction::~Transaction() {
    if (started_ && !finished_ && db_ != nullptr) {
        // 没显式收尾就回滚。宁可丢一次写入，也不要留下半个事务。
        db_->rollback();
    }
}

Database::Transaction::Transaction(Transaction&& other) noexcept
    : db_(other.db_), started_(other.started_), finished_(other.finished_) {
    other.db_ = nullptr;
    other.started_ = false;
    other.finished_ = true;
}

Database::Transaction& Database::Transaction::operator=(Transaction&& other) noexcept {
    if (this != &other) {
        if (started_ && !finished_ && db_ != nullptr) db_->rollback();
        db_ = other.db_;
        started_ = other.started_;
        finished_ = other.finished_;
        other.db_ = nullptr;
        other.started_ = false;
        other.finished_ = true;
    }
    return *this;
}

Status Database::Transaction::commit() {
    if (db_ == nullptr || !started_ || finished_) {
        return status_err(ErrorCode::Internal, "事务已结束或未开始", "Transaction::commit");
    }
    auto st = db_->commit();
    finished_ = true;
    return st;
}

Status Database::Transaction::rollback() {
    if (db_ == nullptr || !started_ || finished_) {
        return status_ok();
    }
    auto st = db_->rollback();
    finished_ = true;
    return st;
}

Status Database::rekey(const crypto::Key256& new_key) {
    if (db_ == nullptr) {
        return status_err(ErrorCode::Internal, "数据库未打开", "Database::rekey");
    }
    if (!encrypted_) {
        return status_err(ErrorCode::Internal,
                          "对明文库调用 rekey 没有意义（users.db 不含加密数据）",
                          "Database::rekey");
    }
    if (new_key.is_all_zero()) {
        return status_err(ErrorCode::CryptoFailure, "拒绝用全零密钥重加密", "Database::rekey");
    }
    // SQLCipher 的 rekey 是全页重写：中途断电会毁库，
    // 所以调用方必须先备份文件（LedgerService::change_password 里做了）。
    const std::string sql = "PRAGMA rekey = \"x'" + new_key.to_hex() + "'\";";
    return execute(sql);
}

Status Database::vacuum() {
    return execute("VACUUM;");
}

int Database::user_version() {
    auto stmt = prepare("PRAGMA user_version;");
    if (stmt.is_err()) return 0;
    auto st = stmt.value().step();
    if (st.is_err() || st.value() != StepResult::Row) return 0;
    return stmt.value().column_int(0);
}

Status Database::set_user_version(int version) {
    return execute("PRAGMA user_version = " + std::to_string(version) + ";");
}

int64_t Database::last_insert_rowid() {
    return db_ != nullptr ? static_cast<int64_t>(sqlite3_last_insert_rowid(db_)) : 0;
}

int Database::changes() {
    return db_ != nullptr ? sqlite3_changes(db_) : 0;
}

bool Database::table_exists(const std::string& table_name) {
    auto stmt = prepare("SELECT count(*) FROM sqlite_master WHERE type='table' AND name = ?1;");
    if (stmt.is_err()) return false;
    if (stmt.value().bind(1, table_name).is_err()) return false;
    auto st = stmt.value().step();
    if (st.is_err() || st.value() != StepResult::Row) return false;
    return stmt.value().column_int64(0) > 0;
}

std::vector<std::string> Database::table_columns(const std::string& table_name) {
    std::vector<std::string> columns;
    // PRAGMA 不支持参数占位符，所以这里只能拼接。
    // 防线：表名来自我们自己的迁移代码常量，且下面做了字符白名单校验。
    for (char c : table_name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_';
        if (!ok) return columns;
    }
    auto stmt = prepare("PRAGMA table_info(" + table_name + ");");
    if (stmt.is_err()) return columns;
    auto st = stmt.value().each_row([&columns](Statement& s) -> Status {
        columns.push_back(s.column_text_or(1, {}));
        return status_ok();
    });
    if (st.is_err()) return {};
    return columns;
}

}  // namespace penhu::storage
