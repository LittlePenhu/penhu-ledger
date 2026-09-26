// =============================================================================
//  server/src/routes_auth.cpp
//  账号相关路由。
//
//  会话令牌只在响应体里返回一次，前端存在内存里（不放 localStorage）。
//  这样关掉窗口就等于登出，也避免了「token 留在磁盘上被读走」。
// =============================================================================

#include "api_support.hpp"

namespace penhu::server::detail {
namespace {

using app::LedgerService;

/// 每个需要鉴权的 handler 都先做这个：Host 校验 + token 取出
// 注意参数是 const Request&：httplib 传给 handler 的就是 const 引用，
// 写成非 const 的话 handler 里那句 guard(req, res, token) 直接编译不过
// （报「无法将参数 1 从 const httplib::Request 转换为 httplib::Request &」）。
bool guard(const httplib::Request& req, httplib::Response& res, std::string& token) {
    if (!preflight_check(req, res, false)) return false;
    token = bearer_token(req);
    if (token.empty()) {
        send_error(res, ErrorCode::AuthFailed, "缺少 Authorization: Bearer <token> 头");
        return false;
    }
    return true;
}

}  // namespace

void register_auth_routes(httplib::Server& srv, RouteContext& ctx) {
    LedgerService& ledger = ctx.ledger;

    // ---- 健康检查（不需要鉴权，前端用来确认服务已就绪）----
    srv.Get("/api/health", [&ledger](const httplib::Request& req, httplib::Response& res) {
        if (!preflight_check(req, res, false)) return;
        const auto stats = ledger.runtime_stats();
        send_ok(res, json{
                         {"version", PENHU_LEDGER_VERSION},
                         {"users", stats.users},
                         {"activeSessions", stats.active_sessions},
                         {"openLedgers", stats.open_ledgers},
                         {"dataDir", ledger.config().data_dir},
                     });
    });

    // ---- 注册 ----
    srv.Post("/api/auth/register", [&ledger](const httplib::Request& req, httplib::Response& res) {
        if (!preflight_check(req, res, true)) return;

        auto body = parse_json_body(req);
        if (body.is_err()) { send_error(res, body.error()); return; }

        auto username = require_str(body.value(), "username");
        if (username.is_err()) { send_error(res, username.error()); return; }
        auto password = require_str(body.value(), "password");
        if (password.is_err()) { send_error(res, password.error()); return; }

        auto st = ledger.register_user(username.value(), password.value());
        if (st.is_err()) { send_error(res, st.error()); return; }

        send_ok(res, json{{"username", username.value()}, {"created", true}}, 201);
    });

    // ---- 登录 ----
    srv.Post("/api/auth/login", [&ledger](const httplib::Request& req, httplib::Response& res) {
        if (!preflight_check(req, res, true)) return;

        auto body = parse_json_body(req);
        if (body.is_err()) { send_error(res, body.error()); return; }

        auto username = require_str(body.value(), "username");
        if (username.is_err()) { send_error(res, username.error()); return; }
        auto password = require_str(body.value(), "password");
        if (password.is_err()) { send_error(res, password.error()); return; }

        auto result = ledger.login(username.value(), password.value());
        if (result.is_err()) { send_error(res, result.error()); return; }

        const auto& auth = result.value();
        send_ok(res, json{
                         {"token", auth.token},
                         {"expiresAt", auth.expires_at},
                         {"user", {{"id", auth.user.id},
                                   {"username", auth.user.username},
                                   {"createdAt", auth.user.created_at},
                                   {"lastLoginAt", auth.user.last_login_at}}},
                     });
    });

    // ---- 登出 ----
    srv.Post("/api/auth/logout", [&ledger](const httplib::Request& req, httplib::Response& res) {
        std::string token;
        if (!guard(req, res, token)) return;

        auto st = ledger.logout(token);
        if (st.is_err()) { send_error(res, st.error()); return; }
        send_ok(res, json{{"loggedOut", true}});
    });

    // ---- 当前用户 ----
    srv.Get("/api/me", [&ledger](const httplib::Request& req, httplib::Response& res) {
        std::string token;
        if (!guard(req, res, token)) return;

        auto user = ledger.current_user(token);
        if (user.is_err()) { send_error(res, user.error()); return; }

        send_ok(res, json{{"id", user.value().id},
                          {"username", user.value().username},
                          {"createdAt", user.value().created_at},
                          {"lastLoginAt", user.value().last_login_at}});
    });

    // ---- 账号列表（本机多账号，登录页用它渲染下拉）----
    srv.Get("/api/users", [&ledger](const httplib::Request& req, httplib::Response& res) {
        if (!preflight_check(req, res, false)) return;

        // 刻意不鉴权：登录页需要在登录前就知道有哪些账号可选。
        // 泄露的信息仅限于「这台机器上有这几个用户名」——这在本地应用里
        // 等价于「桌面上有几个用户文件夹」，不算新暴露。
        auto users = ledger.list_users();
        if (users.is_err()) { send_error(res, users.error()); return; }

        json arr = json::array();
        for (const auto& u : users.value()) {
            arr.push_back({{"id", u.id},
                           {"username", u.username},
                           {"createdAt", u.created_at},
                           {"lastLoginAt", u.last_login_at}});
        }
        send_ok(res, arr);
    });

    // ---- 改密码 ----
    srv.Post("/api/auth/change-password",
             [&ledger](const httplib::Request& req, httplib::Response& res) {
                 std::string token;
                 if (!guard(req, res, token)) return;
                 if (!preflight_check(req, res, true)) return;

                 auto body = parse_json_body(req);
                 if (body.is_err()) { send_error(res, body.error()); return; }

                 auto old_pw = require_str(body.value(), "oldPassword");
                 if (old_pw.is_err()) { send_error(res, old_pw.error()); return; }
                 auto new_pw = require_str(body.value(), "newPassword");
                 if (new_pw.is_err()) { send_error(res, new_pw.error()); return; }

                 auto result = ledger.change_password(token, old_pw.value(), new_pw.value());
                 if (result.is_err()) { send_error(res, result.error()); return; }

                 send_ok(res, json{
                                  {"requiresRelogin", result.value().requires_relogin},
                                  {"backupPath", result.value().backup_path},
                              });
             });

    // ---- 删账号 ----
    srv.Post("/api/auth/delete-account",
             [&ledger](const httplib::Request& req, httplib::Response& res) {
                 std::string token;
                 if (!guard(req, res, token)) return;
                 if (!preflight_check(req, res, true)) return;

                 auto body = parse_json_body(req);
                 if (body.is_err()) { send_error(res, body.error()); return; }

                 auto password = require_str(body.value(), "password");
                 if (password.is_err()) { send_error(res, password.error()); return; }

                 auto st = ledger.delete_account(token, password.value());
                 if (st.is_err()) { send_error(res, st.error()); return; }

                 send_ok(res, json{
                                  {"deleted", true},
                                  {"note", "加密账本已移除；若开启了 keep_deleted_backup，"
                                           "备份仍在 data_dir/backups/ 下（仍由原密码加密）。"},
                              });
             });
}

}  // namespace penhu::server::detail
