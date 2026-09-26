#pragma once
// =============================================================================
//  penhu/app/ledger_service.hpp
//  应用服务层：把「领域模型 + 存储 + 加密」拼成一条条业务用例。
//
//  这是整个 core 对外唯一的门面。UI（native 自绘）、CLI、HTTP 接口全部只跟它说话，
//  不允许绕过它直接碰 storage / crypto。这样做的实际好处是：
//  凡是「必须同时做两件事」的操作（建号 + 建库、改密码 + 重加密 + 撤销会话），
//  都只有一个实现，不会有哪个入口漏掉一步。
//
//  认证模型：
//    每个写操作都要带 token。token 换出 SessionContext，里面有该用户的
//    SQLCipher 密钥。密钥只在内存里，所以进程重启后必须重新登录——这是刻意的。
// =============================================================================

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "penhu/app/app_config.hpp"
#include "penhu/crypto/session.hpp"
#include "penhu/domain/category.hpp"
#include "penhu/domain/record.hpp"
#include "penhu/domain/user.hpp"
#include "penhu/storage/database.hpp"
#include "penhu/storage/report_repository.hpp"
#include "penhu/util/result.hpp"

namespace penhu::app {

/// 登录成功后的返回
struct AuthResult {
    std::string  token;
    User::Public user;
    int64_t      expires_at{0};
};

/// 改密码的结果：说明需要重新登录，以及备份文件在哪
struct PasswordChangeResult {
    bool        requires_relogin{true};
    std::string backup_path;   // 可能为空（备份失败但重加密成功的情况会被拒绝，所以一般有值）
};

class LedgerService {
public:
    static Result<std::unique_ptr<LedgerService>> create(AppConfig config);
    ~LedgerService();

    LedgerService(const LedgerService&) = delete;
    LedgerService& operator=(const LedgerService&) = delete;

    // -------------------------------------------------------------------------
    //  账号
    // -------------------------------------------------------------------------

    /// 注册。内部会同时建 users.db 记录 + 创建并初始化该用户的加密库。
    /// 任一步失败都会回滚，不留下「有账号但没有账本文件」的中间态。
    Status register_user(const std::string& username, const std::string& password);

    Result<AuthResult> login(const std::string& username, const std::string& password);
    Status logout(const std::string& token);

    Result<User::Public> current_user(const std::string& token);
    Result<std::vector<User::Public>> list_users();

    /// 当前账号的账本文件名（相对 data/ 目录，形如 "<uuid>.db"）。
    /// 界面的「安全信息」要显示它：用户备份或核对时靠这个文件名
    /// 才能把磁盘上的加密文件对应到具体账号。
    Result<std::string> ledger_filename(const std::string& token);

    Result<PasswordChangeResult> change_password(const std::string& token,
                                                 const std::string& old_password,
                                                 const std::string& new_password);

    /// 永久删除账号。需要密码确认。
    /// 若 config().keep_deleted_backup 为真，加密账本会先移到 backups/ 再解除关联。
    Status delete_account(const std::string& token, const std::string& password);

    // -------------------------------------------------------------------------
    //  分类
    // -------------------------------------------------------------------------

    Result<std::vector<Category>> list_categories(const std::string& token,
                                                 std::optional<Direction> direction = std::nullopt,
                                                 bool include_inactive = false);

    Result<Category> create_category(const std::string& token,
                                     const std::string& name,
                                     const std::string& icon,
                                     const std::string& color_hex,
                                     Direction direction,
                                     int sort_order);

    Status update_category(const std::string& token, const Category& category);
    Status set_category_active(const std::string& token, const std::string& category_id, bool active);
    Status delete_category(const std::string& token, const std::string& category_id);

    // -------------------------------------------------------------------------
    //  记录
    // -------------------------------------------------------------------------

    Result<Record> add_record(const std::string& token, const RecordInput& input);
    Result<Record> update_record(const std::string& token,
                                const std::string& record_id,
                                const RecordInput& input);
    Status delete_record(const std::string& token, const std::string& record_id);
    Result<Record> get_record(const std::string& token, const std::string& record_id);
    Result<PagedRecords> list_records(const std::string& token, const RecordQuery& query);

    // -------------------------------------------------------------------------
    //  分析取数
    // -------------------------------------------------------------------------

    /// 一次性取全量记录 + 全部分类。
    ///
    /// 为什么不提供「按区间取」：个人账本几年也就几千条，全量读进内存再算，
    /// 比按区间多次查简单得多，也让趋势/异常算「上一期」时不用二次取数。
    /// 代价明确写在这里——记录数到十万级时，这是第一个要改成区间下推的地方。
    struct LedgerSnapshot {
        std::vector<Record>   records;
        std::vector<Category> categories;
    };
    Result<LedgerSnapshot> snapshot(const std::string& token);

    // -------------------------------------------------------------------------
    //  报告存档
    // -------------------------------------------------------------------------

    Result<std::optional<storage::StoredReport>> find_report(const std::string& token,
                                                            const Date& anchor,
                                                            const std::string& kind);

    Result<std::vector<storage::StoredReport>> list_reports(const std::string& token,
                                                           const std::string& kind,
                                                           int limit);

    Result<storage::StoredReport> save_report(const std::string& token,
                                             const storage::StoredReport& report);

    Status delete_report(const std::string& token, const std::string& report_id);

    // -------------------------------------------------------------------------
    //  用户设置
    //
    //  分两类，接口上就分开，避免「随手把 API Key 存进明文表」这种事故：
    //    secret —— 用会话里的账本密钥再做一层 AEAD，密文落盘（云端 API Key）
    //    plain  —— 明文，只放主题偏好这类不怕被看的东西
    // -------------------------------------------------------------------------

    Status set_secret(const std::string& token, const std::string& key,
                      const std::string& plaintext);
    Result<std::optional<std::string>> get_secret(const std::string& token,
                                                 const std::string& key);
    Status remove_secret(const std::string& token, const std::string& key);

    Status set_setting(const std::string& token, const std::string& key,
                       const std::string& value);
    Status remove_setting(const std::string& token, const std::string& key);
    Result<std::optional<std::string>> get_setting(const std::string& token,
                                                  const std::string& key);

    // -------------------------------------------------------------------------
    //  其它
    // -------------------------------------------------------------------------

    const AppConfig& config() const noexcept { return config_; }
    crypto::SessionStore& sessions() noexcept { return sessions_; }

    /// 会话数 / 活跃账本数，用于诊断输出
    struct Stats {
        int64_t users{0};
        size_t  active_sessions{0};
        size_t  open_ledgers{0};
    };
    Stats runtime_stats();

private:
    explicit LedgerService(AppConfig config);

    /// token -> 会话上下文。同时校验账号仍存在且启用。
    Result<crypto::SessionContext> authenticate(const std::string& token);

    /// 打开（或复用已打开的）该用户的加密库
    Result<std::shared_ptr<storage::Database>> acquire_ledger(const crypto::SessionContext& ctx);

    /// 首次使用某个账号的账本：建表、校验归属、种预置分类
    Result<std::shared_ptr<storage::Database>> open_or_create_ledger(
        const User& user, const crypto::Key256& key);

    /// 关闭并移出池（登出 / 删账号 / 改密码后）
    void close_ledger(const std::string& user_id, bool checkpoint = true);

    AppConfig config_;

    std::unique_ptr<storage::Database> users_db_;
    crypto::SessionStore               sessions_;

    std::mutex ledger_mutex_;
    std::unordered_map<std::string, std::shared_ptr<storage::Database>> ledgers_;
};

}  // namespace penhu::app
