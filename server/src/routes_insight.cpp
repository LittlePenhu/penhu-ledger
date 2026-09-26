// =============================================================================
//  server/src/routes_insight.cpp
//  统计分析与报告生成路由。
//
//  这一层只做三件事：解析参数、调服务、序列化。任何计算都不在这里发生——
//  统计口径全在 analytics/ 里，报告编排全在 report_service 里。理由是
//  「界面显示的数字」和「报告里引用的数字」必须来自同一处，
//  一旦 HTTP 层也掺和计算，两个入口迟早会对不上。
//
//  每个需要模型的请求都会先 resolve_channels() 再 set_channels()：
//  云端 API Key 是按账号加密存进该账号账本的，同机多账号各用各的，
//  所以必须每个请求重新解析，不能构造时定死。
// =============================================================================

#include "api_support.hpp"

#include <cstdio>
#include <optional>

#include "penhu/app/llm_settings.hpp"
#include "penhu/llm/settings.hpp"

namespace penhu::server::detail {
namespace {

using app::ChannelMode;
using app::ChannelResolution;

bool guard(const httplib::Request& req, httplib::Response& res, std::string& token,
           bool needs_body) {
    if (!preflight_check(req, res, needs_body)) return false;
    token = bearer_token(req);
    if (token.empty()) {
        send_error(res, ErrorCode::AuthFailed, "缺少 Authorization: Bearer <token> 头");
        return false;
    }
    return true;
}

/// 把该账号的通道配置注入 ReportService，并返回解析结果（界面要显示说明）。
ChannelResolution sync_channels(RouteContext& ctx, const std::string& token) {
    ChannelResolution res = app::resolve_channels(ctx.ledger, token);

    // 本地通道只有在「模式允许」且「llama-server 正在跑」时才注入。
    // build_local_config() 在没有运行中的服务时会直接返回错误——这正是
    // 我们想要的：不给一个必然连不上的配置。
    std::optional<llm::LlmConfig> local;
    if (res.mode != ChannelMode::Cloud) {
        auto built = ctx.local_models.build_local_config();
        if (built.is_ok()) {
            local = built.value();
        } else if (!res.local_model_id.empty()) {
            res.notes.push_back("本地模型已登记但服务未运行：" + built.error().message);
        }
    }

    ctx.reports.set_channels(res.cloud, local);
    return res;
}

json channels_to_json(const ChannelResolution& res, const report::ReportService::ChannelStatus& st) {
    return json{
        {"mode", to_string(res.mode)},
        {"cloudConfigured", res.cloud.has_value()},
        {"cloudReachable", st.cloud_reachable},
        {"cloudLabel", st.cloud_label},
        {"localConfigured", st.local_configured},
        {"localReachable", st.local_reachable},
        {"localLabel", st.local_label},
        {"localModelId", res.local_model_id},
        {"notes", res.notes},
    };
}

json analysis_to_json(const report::Analysis& a) {
    json j;
    j["summary"] = summary_to_json(a.dashboard);
    j["trend"] = trend_to_json(a.trend);
    j["anomalies"] = anomalies_to_json(a.anomalies);
    j["insights"] = insights_to_json(a.insights);
    j["facts"] = report_facts_json(a.facts);
    return j;
}

/// 解析 date 参数；缺省用 fallback
Result<Date> date_param(const httplib::Request& req, const char* name, const Date& fallback) {
    if (!query_has(req, name)) return Result<Date>(fallback);
    auto parsed = Date::parse(query_str(req, name));
    if (parsed.is_err()) {
        return Result<Date>::fail(ErrorCode::InvalidArgument,
                                  std::string("参数 '") + name + "' 不是合法日期（YYYY-MM-DD）",
                                  "date_param");
    }
    return parsed;
}

}  // namespace

void register_insight_routes(httplib::Server& srv, RouteContext& ctx) {
    app::LedgerService& ledger = ctx.ledger;

    // =========================================================================
    //  统计分析
    // =========================================================================

    srv.Get("/api/analytics", [&ctx](const httplib::Request& req, httplib::Response& res) {
        std::string token;
        if (!guard(req, res, token, false)) return;

        const Date today = Date::today();
        auto from = date_param(req, "from", today.add_days(-29));
        if (from.is_err()) { send_error(res, from.error()); return; }
        auto to = date_param(req, "to", today);
        if (to.is_err()) { send_error(res, to.error()); return; }

        if (to.value() < from.value()) {
            send_error(res, ErrorCode::InvalidArgument, "'to' 早于 'from'");
            return;
        }

        const int trend_window = query_int(req, "trendWindow", 7);
        const DateRange range{from.value(), to.value()};

        // 统计不调模型，但仍要同步通道：insights 里会标注「当前模型不可用，
        // 建议仅为算术结论」这类说明，用到的正是通道状态。
        sync_channels(ctx, token);

        auto analysis = ctx.reports.analyze_range(token, range, trend_window);
        if (analysis.is_err()) { send_error(res, analysis.error()); return; }
        send_ok(res, analysis_to_json(analysis.value()));
    });

    // =========================================================================
    //  报告
    // =========================================================================

    srv.Post("/api/reports/generate",
             [&ctx](const httplib::Request& req, httplib::Response& res) {
                 std::string token;
                 if (!guard(req, res, token, true)) return;

                 auto body = parse_json_body(req);
                 if (body.is_err()) { send_error(res, body.error()); return; }

                 const std::string kind_text = opt_str(body.value(), "kind", "daily");
                 auto kind = report::parse_kind(kind_text);
                 if (!kind.has_value()) {
                     send_error(res, ErrorCode::InvalidArgument,
                                "kind 必须是 daily / weekly / monthly");
                     return;
                 }

                 report::GenerateOptions opts;
                 opts.kind = kind.value();
                 const std::string anchor_text = opt_str(body.value(), "anchor");
                 if (!anchor_text.empty()) {
                     auto parsed = Date::parse(anchor_text);
                     if (parsed.is_err()) { send_error(res, parsed.error()); return; }
                     opts.anchor = parsed.value();
                 }
                 opts.force_regenerate = body.value().value("force", false);

                 // 通道说明也一并带上：界面要能解释「为什么这次是模板兜底」
                 ChannelResolution channels = sync_channels(ctx, token);

                 auto result = ctx.reports.generate(token, opts);
                 if (result.is_err()) { send_error(res, result.error()); return; }

                 json j = report_result_to_json(result.value());
                 j["channelMode"] = to_string(channels.mode);
                 j["channelNotes"] = channels.notes;
                 send_ok(res, j);
             });

    // 捕获列表要把 ledger 带上 —— 下面用了它，漏了就是
    // 「函数无法访问 ledger」这种看不出问题的错。
    srv.Get("/api/reports", [&ctx, &ledger](const httplib::Request& req, httplib::Response& res) {
        std::string token;
        if (!guard(req, res, token, false)) return;

        // kind 为空表示「所有类型」——前端的历史列表就是这么调的
        const std::string kind = query_str(req, "kind");
        if (!kind.empty() && !storage::report_kind::is_valid(kind)) {
            send_error(res, ErrorCode::InvalidArgument,
                       "kind 必须是 daily / weekly / monthly，或留空表示全部");
            return;
        }
        const int limit = query_int(req, "limit", 20);

        auto list = ledger.list_reports(token, kind, limit <= 0 ? 20 : limit);
        if (list.is_err()) { send_error(res, list.error()); return; }

        json arr = json::array();
        for (const auto& r : list.value()) arr.push_back(stored_report_to_json(r));
        send_ok(res, arr);
    });

    srv.Get("/api/reports/latest", [&ctx, &ledger](const httplib::Request& req, httplib::Response& res) {
        std::string token;
        if (!guard(req, res, token, false)) return;

        const std::string kind_text = query_str(req, "kind", "daily");
        auto kind = report::parse_kind(kind_text);
        if (!kind.has_value()) {
            send_error(res, ErrorCode::InvalidArgument, "kind 必须是 daily / weekly / monthly");
            return;
        }
        auto anchor = date_param(req, "anchor", Date::today());
        if (anchor.is_err()) { send_error(res, anchor.error()); return; }

        auto found = ledger.find_report(token, anchor.value(), report::storage_id(kind.value()));
        if (found.is_err()) { send_error(res, found.error()); return; }

        // 没有存档不算错误，返回 null 让前端显示「还没有报告」。
        // 这里用 200 + null 而不是 404：404 会被通用错误处理渲染成红字弹条，
        // 而「今天还没生成过报告」是完全正常的状态。
        if (!found.value().has_value()) {
            send_ok(res, json(nullptr));
            return;
        }
        send_ok(res, stored_report_to_json(found.value().value()));
    });

    srv.Delete(R"(/api/reports/([0-9a-fA-F]{32}))",
               [&ledger](const httplib::Request& req, httplib::Response& res) {
                   std::string token;
                   if (!guard(req, res, token, false)) return;

                   auto st = ledger.delete_report(token, req.matches[1].str());
                   if (st.is_err()) { send_error(res, st.error()); return; }
                   send_ok(res, json{{"deleted", true}});
               });

    // =========================================================================
    //  设置
    // =========================================================================

    srv.Get("/api/settings", [&ctx, &ledger](const httplib::Request& req, httplib::Response& res) {
        std::string token;
        if (!guard(req, res, token, false)) return;

        auto user = ctx.ledger.current_user(token);
        if (user.is_err()) { send_error(res, user.error()); return; }

        const app::AppConfig& cfg = ctx.ledger.config();

        // 账本文件名如实取回来展示，方便用户把磁盘上的加密文件对应到具体账号。
        // 只显示文件名不显示完整路径：「数据目录」那一行已经给过路径了。
        auto ledger_file = ctx.ledger.ledger_filename(token);
        if (ledger_file.is_err()) { send_error(res, ledger_file.error()); return; }

        // 这里如实报告真实参数，而不是把配置里的期望值抄一遍：
        // KDF 参数来自 AppConfig，字段加密算法来自 cipher 的实际首选算法——
        // 如果哪天首选算法因为本机不支持而换掉，这个面板会跟着变，
        // 而不是继续显示一个纸面上的算法名。
        json j;
        j["dataDir"] = cfg.data_dir;
        j["usersDbFile"] = cfg.users_db_filename;
        j["ledgerFile"] = ledger_file.value();
        j["ledgerEncrypted"] = true;   // 账本一律 SQLCipher 整库加密，没有「未加密」的代码路径
        j["kdfParams"] = cfg.kdf.describe();
        j["fieldCipher"] = std::string(crypto::to_string(crypto::preferred_algorithm())) +
                           "（字段级 AEAD，AAD 绑定账号与字段名）";
        j["sessionTtlSeconds"] = cfg.session_ttl_seconds;
        j["keepDeletedBackup"] = cfg.keep_deleted_backup;
        j["version"] = PENHU_LEDGER_VERSION;
        j["username"] = user.value().username;
        send_ok(res, j);
    });

    srv.Get("/api/settings/channels",
            [&ctx](const httplib::Request& req, httplib::Response& res) {
                std::string token;
                if (!guard(req, res, token, false)) return;

                ChannelResolution channels = sync_channels(ctx, token);
                // probe_channels 会真的发请求，所以界面是手动点「测试连接」才走这条，
                // 不是每次打开设置抽屉都打一遍网络。
                auto status = ctx.reports.probe_channels();
                send_ok(res, channels_to_json(channels, status));
            });

    srv.Post("/api/settings/llm",
             [&ctx, &ledger](const httplib::Request& req, httplib::Response& res) {
                 std::string token;
                 if (!guard(req, res, token, true)) return;

                 auto body = parse_json_body(req);
                 if (body.is_err()) { send_error(res, body.error()); return; }

                 std::vector<std::string> applied;

                 // 通道模式
                 const std::string mode = opt_str(body.value(), "preferChannel");
                 if (!mode.empty()) {
                     if (mode != "cloud" && mode != "local" && mode != "auto") {
                         send_error(res, ErrorCode::InvalidArgument,
                                    "preferChannel 必须是 cloud / local / auto");
                         return;
                     }
                     auto st = ctx.ledger.set_setting(token, llm::settings::kKeyChannel, mode);
                     if (st.is_err()) { send_error(res, st.error()); return; }
                     applied.push_back("通道模式=" + mode);
                 }

                 const std::string base_url = opt_str(body.value(), "cloudBaseUrl");
                 if (!base_url.empty()) {
                     auto st = ctx.ledger.set_setting(token, llm::settings::kKeyCloudBaseUrl,
                                                      base_url);
                     if (st.is_err()) { send_error(res, st.error()); return; }
                     applied.push_back("云端 base_url");
                 }

                 const std::string model = opt_str(body.value(), "cloudModel");
                 if (!model.empty()) {
                     auto st = ctx.ledger.set_setting(token, llm::settings::kKeyCloudModel, model);
                     if (st.is_err()) { send_error(res, st.error()); return; }
                     applied.push_back("云端模型名");
                 }

                 // API Key 走加密存储。刻意区分「没传」和「传了空串」：
                 // 没传 = 不改；传空串 = 明确要清除。否则用户想删掉已存的 Key
                 // 就没有任何办法（前端清空输入框是常见动作）。
                 if (body.value().contains("cloudApiKey") &&
                     body.value()["cloudApiKey"].is_string()) {
                     const std::string key = body.value()["cloudApiKey"].get<std::string>();
                     Status st = key.empty()
                                     ? ctx.ledger.remove_secret(token,
                                                                llm::settings::kKeyCloudApiKey)
                                     : ctx.ledger.set_secret(token,
                                                             llm::settings::kKeyCloudApiKey, key);
                     if (st.is_err()) { send_error(res, st.error()); return; }
                     applied.push_back(key.empty() ? "清除 API Key" : "API Key（已加密存储）");
                 }

                 // 本地模型 id
                 if (body.value().contains("localModelId") &&
                     body.value()["localModelId"].is_string()) {
                     const std::string id = body.value()["localModelId"].get<std::string>();
                     Status st = id.empty()
                                     ? ctx.ledger.remove_setting(token,
                                                                 llm::settings::kKeyLocalModelId)
                                     : ctx.ledger.set_setting(token,
                                                              llm::settings::kKeyLocalModelId, id);
                     if (st.is_err()) { send_error(res, st.error()); return; }
                     applied.push_back(id.empty() ? "清除本地模型选择" : "本地模型 id=" + id);
                 }

                 send_ok(res, json{{"applied", applied}});
             });

    // ---- 明文设置（主题等；不含任何秘密）----
    srv.Get("/api/settings/prefs",
           [&ledger](const httplib::Request& req, httplib::Response& res) {
               std::string token;
               if (!guard(req, res, token, false)) return;

               json out;
               for (const char* k : {"ui.theme", "ui.last_range", "ui.report_kind"}) {
                   auto v = ledger.get_setting(token, k);
                   if (v.is_ok() && v.value().has_value()) out[k] = *v.value();
               }
               send_ok(res, out);
           });

    srv.Post("/api/settings/prefs",
             [&ledger](const httplib::Request& req, httplib::Response& res) {
                 std::string token;
                 if (!guard(req, res, token, true)) return;

                 auto body = parse_json_body(req);
                 if (body.is_err()) { send_error(res, body.error()); return; }

                 std::vector<std::string> applied;
                 for (const char* k : {"ui.theme", "ui.last_range", "ui.report_kind"}) {
                     if (body.value().contains(k) && body.value()[k].is_string()) {
                         auto st = ledger.set_setting(token, k, body.value()[k].get<std::string>());
                         if (st.is_err()) { send_error(res, st.error()); return; }
                         applied.push_back(k);
                     }
                 }
                 send_ok(res, json{{"applied", applied}});
             });
}

}  // namespace penhu::server::detail
