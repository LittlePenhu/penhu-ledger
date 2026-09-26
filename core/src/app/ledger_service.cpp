#include "penhu/app/ledger_service.hpp"

#include <cstdio>

#include <algorithm>
#include <utility>

#include "penhu/crypto/password.hpp"
#include "penhu/domain/date.hpp"
#include "penhu/storage/category_repository.hpp"
#include "penhu/storage/migrations.hpp"
#include "penhu/storage/record_repository.hpp"
#include "penhu/storage/user_repository.hpp"
#include "penhu/util/fs.hpp"
#include "penhu/util/log.hpp"
#include "penhu/util/uuid.hpp"

namespace penhu::app {
namespace {

constexpr const char* kMetaOwner = "owner_user_id";

/// SQLCipher 的 WAL 会额外产生 -wal / -shm 两个文件。
/// 删库/备份时如果漏掉它们，要么残留数据，要么备份缺最新写入。
std::string wal_path(const std::string& db_path) { return db_path + "-wal"; }
std::string shm_path(const std::string& db_path) { return db_path + "-shm"; }

Status write_meta(storage::Database& db, const std::string& key, const std::string& value) {
    PENHU_ASSIGN_OR_RETURN(stmt, db.prepare(
        "INSERT OR REPLACE INTO meta(key, value) VALUES (?1, ?2);"));
    PENHU_RETURN_IF_ERROR(stmt.bind(1, key));
    PENHU_RETURN_IF_ERROR(stmt.bind(2, value));
    return to_status(stmt.step());
}

Result<std::optional<std::string>> read_meta(storage::Database& db, const std::string& key) {
    PENHU_ASSIGN_OR_RETURN(stmt, db.prepare(
        "SELECT value FROM meta WHERE key = ?1 LIMIT 1;"));
    PENHU_RETURN_IF_ERROR(stmt.bind(1, key));
    auto step = stmt.step();
    if (step.is_err()) return step.error();
    if (step.value() == storage::StepResult::Done) {
        return Result<std::optional<std::string>>(std::optional<std::string>{});
    }
    return Result<std::optional<std::string>>(
        std::optional<std::string>{stmt.column_text(0)});
}

}  // namespace

// -----------------------------------------------------------------------------
//  生命周期
// -----------------------------------------------------------------------------

LedgerService::LedgerService(AppConfig config)
    : config_(std::move(config)),
      sessions_(config_.session_ttl_seconds) {}

LedgerService::~LedgerService() {
    // 关闭所有打开的账本连接，让解密后的页缓存尽早消失
    std::unordered_map<std::string, std::shared_ptr<storage::Database>> copy;
    {
        std::lock_guard<std::mutex> guard(ledger_mutex_);
        copy.swap(ledgers_);
    }
    for (auto& [user_id, db] : copy) {
        if (db && db->is_open()) {
            db->execute("PRAGMA wal_checkpoint(TRUNCATE);");
        }
    }
    copy.clear();
}

Result<std::unique_ptr<LedgerService>> LedgerService::create(AppConfig config) {
    Logger& log = Logger::default_logger();

    // 加密没起来就不要继续：宁可启动失败，也不要跑在一个「加密可能没生效」的状态上
    PENHU_RETURN_IF_ERROR(crypto::initialize_crypto());
    if (!crypto::crypto_self_test()) {
        return Result<std::unique_ptr<LedgerService>>::fail(
            ErrorCode::CryptoFailure,
            "加密子系统自检未通过，拒绝启动（自检失败意味着密钥或随机数不可信）",
            "LedgerService::create");
    }
    log.info("加密子系统自检通过，首选字段加密算法: " +
             std::string(crypto::to_string(crypto::preferred_algorithm())));

    PENHU_RETURN_IF_ERROR(config.ensure_directories());

    auto svc = std::unique_ptr<LedgerService>(new LedgerService(std::move(config)));

    PENHU_ASSIGN_OR_RETURN(db, storage::Database::open_plain(svc->config_.users_db_path()));
    svc->users_db_ = std::make_unique<storage::Database>(std::move(db));
    PENHU_RETURN_IF_ERROR(storage::migrate_users_db(*svc->users_db_));

    log.info("数据目录: " + svc->config_.data_dir);
    log.info("KDF 参数: " + svc->config_.kdf.describe());
    return Result<std::unique_ptr<LedgerService>>(std::move(svc));
}

// -----------------------------------------------------------------------------
//  账本库的获取与释放
// -----------------------------------------------------------------------------

Result<std::shared_ptr<storage::Database>> LedgerService::open_or_create_ledger(
    const User& user, const crypto::Key256& key) {
    const std::string path = config_.ledger_path(user.db_filename);
    const bool existed = fs::exists(path);

    PENHU_ASSIGN_OR_RETURN(raw, storage::Database::open_encrypted(path, key));
    auto db = std::make_shared<storage::Database>(std::move(raw));

    PENHU_RETURN_IF_ERROR(storage::migrate_ledger_db(*db));

    // 归属校验。这一步是为了把「密码错」和「打开了别人的库」分开报，
    // 否则两者都表现成 SQLCipher 的 SQLITE_NOTADB，排查时完全摸不着方向。
    PENHU_ASSIGN_OR_RETURN(owner, read_meta(*db, kMetaOwner));
    if (owner.has_value()) {
        if (owner.value() != user.id) {
            return Result<std::shared_ptr<storage::Database>>::fail(
                ErrorCode::PermissionDenied,
                "账本文件归属不匹配：文件属于 " + owner.value() + "，当前账号是 " + user.id +
                    "。这通常说明数据目录被手工改动过。",
                "open_or_create_ledger: " + path);
        }
    } else {
        PENHU_RETURN_IF_ERROR(write_meta(*db, kMetaOwner, user.id));
        PENHU_RETURN_IF_ERROR(write_meta(*db, "created_at",
                                        std::to_string(timeutil::now_epoch_seconds())));
        PENHU_RETURN_IF_ERROR(write_meta(*db, "schema_owner", "penhu-ledger"));
    }

    // 种预置分类（幂等，每次打开都跑一遍，成本可忽略）
    storage::CategoryRepository categories(*db);
    PENHU_ASSIGN_OR_RETURN(seeded, categories.seed_defaults());
    if (seeded > 0) {
        Logger::default_logger().info("账号 " + user.username + " 种入预置分类 " +
                                      std::to_string(seeded) + " 条");
    }

    Logger::default_logger().debug(
        std::string(existed ? "打开已有账本: " : "新建账本: ") + path +
        " (" + std::to_string(fs::file_size(path)) + " 字节)");

    return db;
}

Result<std::shared_ptr<storage::Database>> LedgerService::acquire_ledger(
    const crypto::SessionContext& ctx) {
    if (!ctx.has_db_key()) {
        return Result<std::shared_ptr<storage::Database>>::fail(
            ErrorCode::Internal, "会话缺少数据库密钥（不应发生，属于程序 bug）",
            "acquire_ledger");
    }

    {
        std::lock_guard<std::mutex> guard(ledger_mutex_);
        auto it = ledgers_.find(ctx.user_id);
        if (it != ledgers_.end()) return it->second;
    }

    // 从 users.db 取最新的用户行：顺便确认账号还在、还没被停用
    PENHU_ASSIGN_OR_RETURN(user, storage::UserRepository(*users_db_).find_by_id(ctx.user_id));
    if (!user.is_active) {
        return Result<std::shared_ptr<storage::Database>>::fail(
            ErrorCode::AuthFailed, "账号已停用", "acquire_ledger");
    }

    PENHU_ASSIGN_OR_RETURN(db, open_or_create_ledger(user, *ctx.db_key));

    std::lock_guard<std::mutex> guard(ledger_mutex_);
    auto [it, inserted] = ledgers_.emplace(ctx.user_id, db);
    // 插入失败说明别的线程刚好放进去了，用已有那个（避免同一用户两份连接）
    return inserted ? db : it->second;
}

void LedgerService::close_ledger(const std::string& user_id, bool checkpoint) {
    std::shared_ptr<storage::Database> db;
    {
        std::lock_guard<std::mutex> guard(ledger_mutex_);
        auto it = ledgers_.find(user_id);
        if (it == ledgers_.end()) return;
        db = it->second;
        ledgers_.erase(it);
    }
    if (db && checkpoint && db->is_open()) {
        // 把 WAL 折回主文件。不这么做的话，主文件会缺最新写入，
        // 而 -wal 文件会在下次打开时被自动重放——如果那个文件被误删，数据就静默丢了。
        db->execute("PRAGMA wal_checkpoint(TRUNCATE);");
    }
    // db 的 shared_ptr 在这里析构，连接随之关闭
}

// -----------------------------------------------------------------------------
//  认证
// -----------------------------------------------------------------------------

Result<crypto::SessionContext> LedgerService::authenticate(const std::string& token) {
    if (token.empty()) {
        return Result<crypto::SessionContext>::fail(ErrorCode::AuthFailed, "缺少访问令牌",
                                                   "authenticate");
    }
    // resolve 会顺带滑动续期
    return sessions_.resolve(token);
}

Status LedgerService::register_user(const std::string& username, const std::string& password) {
    storage::UserRepository users(*users_db_);

    storage::Database::Transaction tx(*users_db_);
    if (!tx.started()) {
        return status_err(ErrorCode::StorageFailure, "无法开启注册事务", "register_user");
    }

    PENHU_ASSIGN_OR_RETURN(user, users.create(username, password, config_.kdf));

    // 注册时就把账本建出来并验证一遍，理由是「有账号但库打不开」这种状态
    // 会在用户第一次登录时才暴露，那时他已经录了一堆账——不如现在就失败。
    PENHU_ASSIGN_OR_RETURN(key, crypto::derive_db_key(password, user.db_salt_hex, config_.kdf));

    auto ledger = open_or_create_ledger(user, key);
    if (ledger.is_err()) {
        const Error err = ledger.error();
        tx.rollback();

        // 清理可能已产生的文件，避免留下一个「有文件但没账号」的孤儿
        const std::string path = config_.ledger_path(user.db_filename);
        fs::remove_file(path);
        fs::remove_file(wal_path(path));
        fs::remove_file(shm_path(path));

        return status_err(err.code,
                          "创建加密账本失败，注册已回滚: " + err.message,
                          "register_user");
    }

    // 注册阶段不需要保持连接，收掉
    if (ledger.value() && ledger.value()->is_open()) {
        ledger.value()->execute("PRAGMA wal_checkpoint(TRUNCATE);");
    }
    ledger.value().reset();

    PENHU_RETURN_IF_ERROR(tx.commit());

    Logger::default_logger().info("注册成功: " + user.username + " (id=" + user.id + ")");
    return status_ok();
}

Result<AuthResult> LedgerService::login(const std::string& username, const std::string& password) {
    using Fail = Result<AuthResult>;
    Logger& log = Logger::default_logger();

    storage::UserRepository users(*users_db_);

    auto found = users.find_for_auth(username);
    if (found.is_err()) {
        // 刻意不区分「用户不存在」和「密码错误」。
        // 虽然是本机应用、泄露后果有限，但注册接口本来就能探测用户名，
        // 这里保持统一措辞能省掉一类「顺手写进日志」的坏习惯。
        log.warn("登录失败（用户名不存在）: " + username);
        return Fail::fail(ErrorCode::AuthFailed, "用户名或密码错误", "login");
    }
    User user = std::move(found).value();

    PENHU_ASSIGN_OR_RETURN(password_ok, crypto::verify_password(password, user.password_hash));
    if (!password_ok) {
        log.warn("登录失败（密码不匹配）: " + username);
        return Fail::fail(ErrorCode::AuthFailed, "用户名或密码错误", "login");
    }
    if (!user.is_active) {
        return Fail::fail(ErrorCode::AuthFailed, "该账号已停用", "login");
    }

    // 顺手升级哈希参数。注意：db_salt 不变，所以账本密钥不受影响，
    // 不会出现「改了哈希参数导致旧库打不开」的联动问题。
    if (crypto::password_needs_rehash(user.password_hash, config_.kdf)) {
        auto rehashed = crypto::hash_password(password, config_.kdf);
        if (rehashed.is_ok()) {
            users.update_password_hash(user.id, rehashed.value());
            log.info("已将 " + user.username + " 的密码哈希升级到当前 KDF 参数");
        } else {
            log.warn("密码哈希升级失败（不影响本次登录）: " + rehashed.error().to_string());
        }
    }

    PENHU_ASSIGN_OR_RETURN(key, crypto::derive_db_key(password, user.db_salt_hex, config_.kdf));
    auto key_ptr = std::make_shared<crypto::Key256>(std::move(key));

    // 先把库打开确认密钥可用，再发 token。
    // 顺序反过来的话，用户会拿到一个「能通过鉴权但所有查询都失败」的会话。
    PENHU_ASSIGN_OR_RETURN(db, open_or_create_ledger(user, *key_ptr));
    {
        std::lock_guard<std::mutex> guard(ledger_mutex_);
        ledgers_[user.id] = db;
    }

    PENHU_ASSIGN_OR_RETURN(token, sessions_.create(user.id, user.username, key_ptr));
    PENHU_ASSIGN_OR_RETURN(ctx, sessions_.resolve(token));

    users.update_last_login(user.id);

    AuthResult out;
    out.token      = token;
    out.user       = user.to_public();
    out.expires_at = ctx.expires_at;

    log.info("登录成功: " + user.username + " (会话数=" + std::to_string(sessions_.size()) + ")");
    return Result<AuthResult>(std::move(out));
}

Status LedgerService::logout(const std::string& token) {
    auto ctx = sessions_.peek(token);
    PENHU_RETURN_IF_ERROR(sessions_.revoke(token));

    if (ctx.is_ok()) {
        const std::string user_id = ctx.value().user_id;
        // 该用户已经没有任何会话了，就把库连接也关掉：
        // 少一个长期持有的解密连接，就少一份内存里的明文页缓存。
        if (sessions_.count_for_user(user_id) == 0) {
            close_ledger(user_id, true);
            Logger::default_logger().info("已关闭账号 " + ctx.value().username + " 的账本连接");
        }
    }
    return status_ok();
}

Result<User::Public> LedgerService::current_user(const std::string& token) {
    PENHU_ASSIGN_OR_RETURN(ctx, authenticate(token));
    PENHU_ASSIGN_OR_RETURN(user, storage::UserRepository(*users_db_).find_by_id(ctx.user_id));
    return Result<User::Public>(user.to_public());
}

Result<std::vector<User::Public>> LedgerService::list_users() {
    PENHU_ASSIGN_OR_RETURN(users, storage::UserRepository(*users_db_).list_all());
    std::vector<User::Public> out;
    out.reserve(users.size());
    for (const auto& u : users) out.push_back(u.to_public());
    return Result<std::vector<User::Public>>(std::move(out));
}

Result<std::string> LedgerService::ledger_filename(const std::string& token) {
    // 这里刻意只做鉴权、不开库：只是要个文件名，没必要为它去解一次
    // SQLCipher（那是一次真实的 Argon2id + 密钥派生，不该为展示而付出）。
    PENHU_ASSIGN_OR_RETURN(ctx, authenticate(token));
    PENHU_ASSIGN_OR_RETURN(user, storage::UserRepository(*users_db_).find_by_id(ctx.user_id));
    return Result<std::string>(user.db_filename);
}

Result<PasswordChangeResult> LedgerService::change_password(const std::string& token,
                                                            const std::string& old_password,
                                                            const std::string& new_password) {
    using Fail = Result<PasswordChangeResult>;
    Logger& log = Logger::default_logger();

    PENHU_ASSIGN_OR_RETURN(ctx, authenticate(token));
    PENHU_RETURN_IF_ERROR(user_policy::validate_password(new_password));

    storage::UserRepository users(*users_db_);
    PENHU_ASSIGN_OR_RETURN(user, users.find_by_id(ctx.user_id));

    PENHU_ASSIGN_OR_RETURN(old_ok, crypto::verify_password(old_password, user.password_hash));
    if (!old_ok) {
        return Fail::fail(ErrorCode::AuthFailed, "原密码不正确", "change_password");
    }

    PENHU_ASSIGN_OR_RETURN(db, acquire_ledger(ctx));
    const std::string path = config_.ledger_path(user.db_filename);

    // 1) WAL 先折回主文件，否则备份缺最新写入
    PENHU_RETURN_IF_ERROR(db->execute("PRAGMA wal_checkpoint(TRUNCATE);"));

    // 2) 先备份。rekey 是全页重写，中途故障会毁库；
    //    没有备份就动它等于赌运气。
    const std::string backup = fs::join(
        config_.backups_dir(),
        user.id + "-before-rekey-" + fs::timestamp_slug() + ".db");
    auto backup_st = fs::copy_file(path, backup, true);
    if (backup_st.is_err()) {
        return Fail::fail(ErrorCode::StorageFailure,
                          "改密码前的备份失败，已中止操作（不对没有备份的账本做重加密）: " +
                              backup_st.error().message,
                          "change_password");
    }
    log.info("改密码前备份: " + backup + " (" + std::to_string(fs::file_size(backup)) + " 字节)");

    // 3) 用同一个 salt 派生新密钥（salt 属于用户，不随密码变化，这样才不用改 users.db 结构）
    PENHU_ASSIGN_OR_RETURN(new_key, crypto::derive_db_key(new_password, user.db_salt_hex, config_.kdf));

    // 4) 重加密
    auto rekey_st = db->rekey(new_key);
    if (rekey_st.is_err()) {
        log.error("重加密失败: " + rekey_st.error().to_string());
        return Fail::fail(rekey_st.error().code,
                          "重加密失败。已保留备份: " + backup + "；原密码仍然有效。",
                          "change_password");
    }

    // 5) 立刻验证新密钥真的能用。rekey 返回成功不等于数据可读，
    //    必须实际读一次才算数。
    {
        auto probe = db->prepare("SELECT count(*) FROM records;");
        bool verified = false;
        if (probe.is_ok()) {
            auto step = probe.value().step();
            verified = step.is_ok() && step.value() == storage::StepResult::Row;
        }
        if (!verified) {
            log.error("重加密后校验失败，从备份恢复");
            close_ledger(user.id, false);
            auto restore = fs::copy_file(backup, path, true);
            if (restore.is_err()) {
                return Fail::fail(
                    ErrorCode::CryptoFailure,
                    "重加密后校验失败，且自动恢复也失败！请手工把备份复制回 " + path +
                        "。备份位置: " + backup,
                    "change_password");
            }
            return Fail::fail(
                ErrorCode::CryptoFailure,
                "重加密后校验失败，已从备份恢复原始账本，原密码依然有效。备份保留在: " + backup,
                "change_password");
        }
    }

    // 6) 更新 users.db 中的密码哈希
    PENHU_ASSIGN_OR_RETURN(new_hash, crypto::hash_password(new_password, config_.kdf));
    PENHU_RETURN_IF_ERROR(users.update_password_hash(user.id, new_hash));

    // 7) 密钥变了，所有既有会话手里的 db_key 全部作废 —— 包括发起这次请求的会话。
    //    直接全撤销，让用户重新登录，比维护「部分会话密钥有效」要安全得多。
    const int revoked = sessions_.revoke_all_for_user(user.id);
    close_ledger(user.id, true);

    log.info("密码已更新: " + user.username + "，撤销会话 " + std::to_string(revoked) + " 个");

    PasswordChangeResult out;
    out.requires_relogin = true;
    out.backup_path = backup;
    return Result<PasswordChangeResult>(std::move(out));
}

Status LedgerService::delete_account(const std::string& token, const std::string& password) {
    Logger& log = Logger::default_logger();

    PENHU_ASSIGN_OR_RETURN(ctx, authenticate(token));

    storage::UserRepository users(*users_db_);
    PENHU_ASSIGN_OR_RETURN(user, users.find_by_id(ctx.user_id));
    PENHU_ASSIGN_OR_RETURN(password_ok, crypto::verify_password(password, user.password_hash));
    if (!password_ok) {
        return status_err(ErrorCode::AuthFailed, "密码不正确，无法删除账号", "delete_account");
    }

    const std::string path = config_.ledger_path(user.db_filename);

    // 先断连接，否则删不掉文件（Windows 会拒绝删除被占用的文件）
    close_ledger(user.id, true);

    if (config_.keep_deleted_backup && fs::exists(path)) {
        const std::string backup = fs::join(
            config_.backups_dir(),
            user.id + "-deleted-" + fs::timestamp_slug() + ".db");
        auto cp = fs::copy_file(path, backup, true);
        if (cp.is_err()) {
            return status_err(ErrorCode::StorageFailure,
                              "删除前的备份失败，已中止: " + cp.error().message,
                              "delete_account");
        }
        log.warn("账号 " + user.username + " 的加密账本已备份到 " + backup +
                 "（仍由原密码加密；若要彻底抹除请手工删除该文件）");
    }

    PENHU_RETURN_IF_ERROR(fs::remove_file(path));
    PENHU_RETURN_IF_ERROR(fs::remove_file(wal_path(path)));
    PENHU_RETURN_IF_ERROR(fs::remove_file(shm_path(path)));

    sessions_.revoke_all_for_user(user.id);
    PENHU_RETURN_IF_ERROR(users.erase(user.id));

    log.warn("账号已删除: " + user.username + " (id=" + user.id + ")");
    return status_ok();
}

// -----------------------------------------------------------------------------
//  分类
// -----------------------------------------------------------------------------

Result<std::vector<Category>> LedgerService::list_categories(const std::string& token,
                                                             std::optional<Direction> direction,
                                                             bool include_inactive) {
    PENHU_ASSIGN_OR_RETURN(ctx, authenticate(token));
    PENHU_ASSIGN_OR_RETURN(db, acquire_ledger(ctx));
    return storage::CategoryRepository(*db).list(direction, include_inactive);
}

Result<Category> LedgerService::create_category(const std::string& token,
                                                const std::string& name,
                                                const std::string& icon,
                                                const std::string& color_hex,
                                                Direction direction,
                                                int sort_order) {
    PENHU_ASSIGN_OR_RETURN(ctx, authenticate(token));
    PENHU_ASSIGN_OR_RETURN(db, acquire_ledger(ctx));
    return storage::CategoryRepository(*db).create(name, icon, color_hex, direction, sort_order);
}

Status LedgerService::update_category(const std::string& token, const Category& category) {
    PENHU_ASSIGN_OR_RETURN(ctx, authenticate(token));
    PENHU_ASSIGN_OR_RETURN(db, acquire_ledger(ctx));
    return storage::CategoryRepository(*db).update(category);
}

Status LedgerService::set_category_active(const std::string& token,
                                          const std::string& category_id,
                                          bool active) {
    PENHU_ASSIGN_OR_RETURN(ctx, authenticate(token));
    PENHU_ASSIGN_OR_RETURN(db, acquire_ledger(ctx));
    return storage::CategoryRepository(*db).set_active(category_id, active);
}

Status LedgerService::delete_category(const std::string& token, const std::string& category_id) {
    PENHU_ASSIGN_OR_RETURN(ctx, authenticate(token));
    PENHU_ASSIGN_OR_RETURN(db, acquire_ledger(ctx));
    return storage::CategoryRepository(*db).remove(category_id);
}

// -----------------------------------------------------------------------------
//  记录
// -----------------------------------------------------------------------------

Result<Record> LedgerService::add_record(const std::string& token, const RecordInput& input) {
    PENHU_RETURN_IF_ERROR(input.validate());

    PENHU_ASSIGN_OR_RETURN(ctx, authenticate(token));
    PENHU_ASSIGN_OR_RETURN(db, acquire_ledger(ctx));

    // 分类必须存在、启用、方向一致。放在服务层校验是因为它要查库。
    PENHU_RETURN_IF_ERROR(
        storage::CategoryRepository(*db).ensure_usable(input.category_id, input.direction));

    Record r;
    r.id          = uuid_to_compact(new_uuid());
    r.user_id     = ctx.user_id;
    r.amount      = input.amount;
    r.direction   = input.direction;
    r.category_id = input.category_id;
    r.date        = input.date;
    r.note        = input.note;
    r.created_at  = timeutil::now_epoch_seconds();
    r.updated_at  = r.created_at;

    return storage::RecordRepository(*db).insert(r);
}

Result<Record> LedgerService::update_record(const std::string& token,
                                            const std::string& record_id,
                                            const RecordInput& input) {
    PENHU_RETURN_IF_ERROR(input.validate());

    PENHU_ASSIGN_OR_RETURN(ctx, authenticate(token));
    PENHU_ASSIGN_OR_RETURN(db, acquire_ledger(ctx));

    storage::RecordRepository records(*db);
    PENHU_ASSIGN_OR_RETURN(existing, records.find_by_id(record_id));

    PENHU_RETURN_IF_ERROR(
        storage::CategoryRepository(*db).ensure_usable(input.category_id, input.direction));

    // 只更新用户能改的字段。created_at 必须保留原值——
    // 否则「按录入时间排序」会在编辑后静默变化，用户会觉得界面在乱跳。
    Record updated = existing;
    updated.amount      = input.amount;
    updated.direction   = input.direction;
    updated.category_id = input.category_id;
    updated.date        = input.date;
    updated.note        = input.note;
    updated.updated_at  = timeutil::now_epoch_seconds();

    PENHU_RETURN_IF_ERROR(records.update(updated));
    return Result<Record>(std::move(updated));
}

Status LedgerService::delete_record(const std::string& token, const std::string& record_id) {
    PENHU_ASSIGN_OR_RETURN(ctx, authenticate(token));
    PENHU_ASSIGN_OR_RETURN(db, acquire_ledger(ctx));
    return storage::RecordRepository(*db).remove(record_id);
}

Result<Record> LedgerService::get_record(const std::string& token, const std::string& record_id) {
    PENHU_ASSIGN_OR_RETURN(ctx, authenticate(token));
    PENHU_ASSIGN_OR_RETURN(db, acquire_ledger(ctx));
    return storage::RecordRepository(*db).find_by_id(record_id);
}

Result<PagedRecords> LedgerService::list_records(const std::string& token,
                                                 const RecordQuery& query) {
    PENHU_ASSIGN_OR_RETURN(ctx, authenticate(token));
    PENHU_ASSIGN_OR_RETURN(db, acquire_ledger(ctx));
    return storage::RecordRepository(*db).query(query);
}

// -----------------------------------------------------------------------------
//  统计与报告的数据访问
// -----------------------------------------------------------------------------

// 注意返回类型要写 LedgerService::LedgerSnapshot：
// 函数定义写在类外时，返回类型出现在 `LedgerService::` 之前，
// 那个位置还在命名空间作用域，看不到类里的嵌套名字。
// 不限定的话报的是「LedgerSnapshot 未声明」+「重载函数与…只是返回类型不同」——
// 后者会让人以为是重复定义，其实只是一次声明没被认出来。
Result<LedgerService::LedgerSnapshot> LedgerService::snapshot(const std::string& token) {
    PENHU_ASSIGN_OR_RETURN(ctx, authenticate(token));
    PENHU_ASSIGN_OR_RETURN(db, acquire_ledger(ctx));

    LedgerSnapshot out;
    // 包含停用的分类：历史记录可能还引用着它们，缺了会导致分类名回填成「已删除」
    //
    // 这里刻意分两步写：PENHU_ASSIGN_OR_RETURN 的第一参数必须是裸标识符，
    // 直接写 out.categories 会被宏的 ## 拼成 `auto _penhu_res_out.categories`，
    // 一次性抛出几十条语法错误（见 result.hpp 里那条警告）。
    PENHU_ASSIGN_OR_RETURN(cats,
                           storage::CategoryRepository(*db).list(std::nullopt, true));
    out.categories = std::move(cats);

    PENHU_ASSIGN_OR_RETURN(recs, storage::RecordRepository(*db).fetch_all());
    out.records = std::move(recs);

    return Result<LedgerSnapshot>(std::move(out));
}

Result<storage::StoredReport> LedgerService::save_report(const std::string& token,
                                                         const storage::StoredReport& report) {
    PENHU_ASSIGN_OR_RETURN(ctx, authenticate(token));
    PENHU_ASSIGN_OR_RETURN(db, acquire_ledger(ctx));
    return storage::ReportRepository(*db).upsert(report);
}

Result<std::optional<storage::StoredReport>> LedgerService::find_report(
    const std::string& token, const Date& anchor, const std::string& kind) {
    PENHU_ASSIGN_OR_RETURN(ctx, authenticate(token));
    PENHU_ASSIGN_OR_RETURN(db, acquire_ledger(ctx));

    auto found = storage::ReportRepository(*db).find(anchor, kind);
    if (found.is_err()) {
        // 「没有存档」不是错误，是正常状态。把它和真正的故障区分开。
        if (found.error().code == ErrorCode::NotFound) {
            return Result<std::optional<storage::StoredReport>>(
                std::optional<storage::StoredReport>{});
        }
        return found.error();
    }
    return Result<std::optional<storage::StoredReport>>(
        std::optional<storage::StoredReport>{std::move(found).value()});
}

Result<std::vector<storage::StoredReport>> LedgerService::list_reports(
    const std::string& token, const std::string& kind, int limit) {
    PENHU_ASSIGN_OR_RETURN(ctx, authenticate(token));
    PENHU_ASSIGN_OR_RETURN(db, acquire_ledger(ctx));
    return storage::ReportRepository(*db).list_recent(kind, limit);
}

Status LedgerService::delete_report(const std::string& token, const std::string& report_id) {
    PENHU_ASSIGN_OR_RETURN(ctx, authenticate(token));
    PENHU_ASSIGN_OR_RETURN(db, acquire_ledger(ctx));
    return storage::ReportRepository(*db).remove(report_id);
}

// -----------------------------------------------------------------------------
//  用户级设置
// -----------------------------------------------------------------------------

Status LedgerService::set_secret(const std::string& token,
                                 const std::string& key,
                                 const std::string& value) {
    PENHU_ASSIGN_OR_RETURN(ctx, authenticate(token));
    if (!ctx.has_db_key()) {
        return status_err(ErrorCode::Internal, "会话缺少数据库密钥", "set_secret");
    }
    PENHU_ASSIGN_OR_RETURN(db, acquire_ledger(ctx));
    return storage::SettingsRepository(*db).set_secret(key, value, *ctx.db_key);
}

Result<std::optional<std::string>> LedgerService::get_secret(const std::string& token,
                                                            const std::string& key) {
    PENHU_ASSIGN_OR_RETURN(ctx, authenticate(token));
    if (!ctx.has_db_key()) {
        return Result<std::optional<std::string>>::fail(ErrorCode::Internal,
                                                       "会话缺少数据库密钥", "get_secret");
    }
    PENHU_ASSIGN_OR_RETURN(db, acquire_ledger(ctx));
    return storage::SettingsRepository(*db).get_secret(key, *ctx.db_key);
}

Status LedgerService::remove_secret(const std::string& token, const std::string& key) {
    PENHU_ASSIGN_OR_RETURN(ctx, authenticate(token));
    PENHU_ASSIGN_OR_RETURN(db, acquire_ledger(ctx));
    return storage::SettingsRepository(*db).remove(key);
}

Status LedgerService::set_setting(const std::string& token,
                                  const std::string& key,
                                  const std::string& value) {
    PENHU_ASSIGN_OR_RETURN(ctx, authenticate(token));
    PENHU_ASSIGN_OR_RETURN(db, acquire_ledger(ctx));
    return storage::SettingsRepository(*db).set_plain(key, value);
}

Status LedgerService::remove_setting(const std::string& token, const std::string& key) {
    PENHU_ASSIGN_OR_RETURN(ctx, authenticate(token));
    PENHU_ASSIGN_OR_RETURN(db, acquire_ledger(ctx));
    return storage::SettingsRepository(*db).remove(key);
}

Result<std::optional<std::string>> LedgerService::get_setting(const std::string& token,
                                                             const std::string& key) {
    PENHU_ASSIGN_OR_RETURN(ctx, authenticate(token));
    PENHU_ASSIGN_OR_RETURN(db, acquire_ledger(ctx));
    auto r = storage::SettingsRepository(*db).get_plain(key);
    return r;
}

// -----------------------------------------------------------------------------
//  诊断
// -----------------------------------------------------------------------------

LedgerService::Stats LedgerService::runtime_stats() {
    Stats s;
    s.users = storage::UserRepository(*users_db_).count();
    s.active_sessions = sessions_.size();
    {
        std::lock_guard<std::mutex> guard(ledger_mutex_);
        s.open_ledgers = ledgers_.size();
    }
    return s;
}

}  // namespace penhu::app
