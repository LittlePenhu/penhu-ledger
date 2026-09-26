// =============================================================================
//  server/src/routes_local_model.cpp
//  本地模型的部署路由。
//
//  边界要说清楚，因为这块最容易做过头：
//    框架做：装引擎（llama-server）、起/停服务、健康探测、登记你给过的模型
//    框架不做：替你选模型、替你下权重、保证效果
//
//  所以这里没有「推荐模型」这类接口，也没有「一键下载 Qwen」这种东西。
//  模型永远是用户给的本地 GGUF 路径；框架只让「用户给的那个模型」跑起来。
//
//  长耗时操作（装运行时、起服务）会阻塞这个请求。对本机单用户应用是可以接受的：
//  同一时刻只有一个用户在操作。但要说明白，这样做的代价是——
//  期间界面上的其它请求也会排队。想改成异步任务队列是下一步的事，
//  不是现在；先把「能装、能起、能自动接上」这条链跑通。
// =============================================================================

#include "api_support.hpp"

#include "penhu/llm/settings.hpp"

namespace penhu::server::detail {
namespace {

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

deploy::RuntimeFlavor parse_flavor(const std::string& text) {
    if (text == "Vulkan" || text == "vulkan") return deploy::RuntimeFlavor::Vulkan;
    if (text == "Cuda" || text == "CUDA" || text == "cuda") return deploy::RuntimeFlavor::Cuda;
    return deploy::RuntimeFlavor::Cpu;
}

json runtime_to_json(const deploy::RuntimeStatus& r) {
    return json{
        {"installed", r.installed},
        {"serverExe", r.server_exe},
        {"directory", r.directory},
        {"releaseTag", r.release_tag},
        {"flavor", deploy::to_string(r.flavor)},
        {"sizeBytes", r.size_bytes},
        {"error", r.error},
    };
}

json model_to_json(const deploy::LocalModelEntry& m) {
    return json{
        {"id", m.id},
        {"displayName", m.display_name},
        {"ggufPath", m.gguf_path},
        {"note", m.note},
        {"sizeBytes", m.size_bytes},
        {"addedAt", m.added_at},
        {"contextSize", m.context_size},
        {"gpuLayers", m.gpu_layers},
        {"threads", m.threads},
    };
}

json deployment_to_json(const deploy::DeploymentStatus& d) {
    return json{
        {"runtime", runtime_to_json(d.runtime)},
        {"serverRunning", d.server_running},
        {"serverPort", d.server_port},
        {"serverPid", d.server_pid},
        {"activeModelId", d.active_model_id},
        {"activeModelName", d.active_model_name},
        {"baseUrl", d.base_url},
        {"modelField", d.model_field},
        {"logPath", d.log_path},
        {"startedAt", d.started_at},
        {"lastError", d.last_error},
    };
}

}  // namespace

void register_local_model_routes(httplib::Server& srv, RouteContext& ctx) {
    deploy::LocalModelManager& models = ctx.local_models;
    app::LedgerService& ledger = ctx.ledger;

    // ---- 总状态（界面打开设置抽屉时调）----
    srv.Get("/api/local-model/status",
            [&models](const httplib::Request& req, httplib::Response& res) {
                std::string token;
                if (!guard(req, res, token, false)) return;

                deploy::DeploymentStatus st = models.status();
                json j = deployment_to_json(st);

                // 界面直接用 models 数组渲染登记表，所以这里一并带上，
                // 省一次请求（设置抽屉打开时只有这一次往返）。
                auto list = models.list_models();
                j["models"] = json::array();
                if (list.is_ok()) {
                    for (const auto& m : list.value()) j["models"].push_back(model_to_json(m));
                }

                // 前端读的是扁平字段名，这里同时给出，避免前端要在两层结构里挖
                j["runtimeInstalled"] = st.runtime.installed;
                j["runtimeRelease"] = st.runtime.release_tag;
                send_ok(res, j);
            });

    // ---- 装运行时 ----
    srv.Post("/api/local-model/install-runtime",
             [&models](const httplib::Request& req, httplib::Response& res) {
                 std::string token;
                 if (!guard(req, res, token, true)) return;

                 std::string flavor_text = "Cpu";
                 if (!req.body.empty()) {
                     auto body = parse_json_body(req);
                     if (body.is_ok()) flavor_text = opt_str(body.value(), "flavor", "Cpu");
                 }

                 auto installed = models.install_runtime(parse_flavor(flavor_text));
                 if (installed.is_err()) { send_error(res, installed.error()); return; }
                 send_ok(res, runtime_to_json(installed.value()));
             });

    // ---- 指定已有的 llama-server.exe（跳过自动下载）----
    srv.Post("/api/local-model/runtime-path",
             [&models](const httplib::Request& req, httplib::Response& res) {
                 std::string token;
                 if (!guard(req, res, token, true)) return;

                 auto body = parse_json_body(req);
                 if (body.is_err()) { send_error(res, body.error()); return; }
                 auto exe = require_str(body.value(), "serverExe");
                 if (exe.is_err()) { send_error(res, exe.error()); return; }

                 auto st = models.set_runtime_path(exe.value());
                 if (st.is_err()) { send_error(res, st.error()); return; }
                 send_ok(res, runtime_to_json(models.runtime_status()));
             });

    // ---- 模型登记 / 列表 ----
    srv.Get("/api/local-model/models",
            [&models](const httplib::Request& req, httplib::Response& res) {
                std::string token;
                if (!guard(req, res, token, false)) return;

                auto list = models.list_models();
                if (list.is_err()) { send_error(res, list.error()); return; }
                json arr = json::array();
                for (const auto& m : list.value()) arr.push_back(model_to_json(m));
                send_ok(res, arr);
            });

    srv.Post("/api/local-model/models",
             [&models](const httplib::Request& req, httplib::Response& res) {
                 std::string token;
                 if (!guard(req, res, token, true)) return;

                 auto body = parse_json_body(req);
                 if (body.is_err()) { send_error(res, body.error()); return; }

                 auto name = require_str(body.value(), "displayName");
                 if (name.is_err()) { send_error(res, name.error()); return; }
                 auto path = require_str(body.value(), "ggufPath");
                 if (path.is_err()) { send_error(res, path.error()); return; }

                 auto added = models.add_model(name.value(), path.value(),
                                              opt_str(body.value(), "note"));
                 if (added.is_err()) { send_error(res, added.error()); return; }
                 send_ok(res, model_to_json(added.value()), 201);
             });

    // ---- 只做校验，不登记（界面在用户选完文件后先提醒一句）----
    srv.Get("/api/local-model/inspect",
           [](const httplib::Request& req, httplib::Response& res) {
               std::string token;
               if (!guard(req, res, token, false)) return;

               const std::string path = query_str(req, "path");
               if (path.empty()) {
                   send_error(res, ErrorCode::InvalidArgument, "缺少 path 参数");
                   return;
               }
               const deploy::GgufInfo info = deploy::LocalModelManager::inspect_gguf(path);
               send_ok(res, json{
                                {"valid", info.valid},
                                {"sizeBytes", info.size_bytes},
                                {"ggufVersion", info.gguf_version},
                                {"problem", info.problem},
                            });
           });

    srv.Delete(R"(/api/local-model/models/([0-9a-zA-Z_-]+))",
               [&models](const httplib::Request& req, httplib::Response& res) {
                   std::string token;
                   if (!guard(req, res, token, false)) return;

                   auto st = models.remove_model(req.matches[1].str());
                   if (st.is_err()) { send_error(res, st.error()); return; }
                   send_ok(res, json{{"removed", true},
                                     {"note", "只从登记表移除，磁盘上的 GGUF 文件没有被删除。"}});
               });

    srv.Patch(R"(/api/local-model/models/([0-9a-zA-Z_-]+))",
              [&models](const httplib::Request& req, httplib::Response& res) {
                  std::string token;
                  if (!guard(req, res, token, true)) return;

                  auto body = parse_json_body(req);
                  if (body.is_err()) { send_error(res, body.error()); return; }

                  const int ctx_size = query_int(req, "contextSize",
                                                 body.value().value("contextSize", 4096));
                  const int gpu = query_int(req, "gpuLayers",
                                            body.value().value("gpuLayers", 0));
                  const int threads = query_int(req, "threads",
                                                body.value().value("threads", 0));

                  auto st = models.update_model_params(req.matches[1].str(), ctx_size, gpu,
                                                       threads);
                  if (st.is_err()) { send_error(res, st.error()); return; }
                  send_ok(res, json{{"updated", true}});
              });

    // ---- 生命周期 ----
    srv.Post("/api/local-model/start",
             [&models, &ledger](const httplib::Request& req, httplib::Response& res) {
                 std::string token;
                 if (!guard(req, res, token, true)) return;

                 auto body = parse_json_body(req);
                 if (body.is_err()) { send_error(res, body.error()); return; }

                 // 没显式给模型 id 就取账号设置里登记的那个。
                 // 这样界面上「启动本地模型」是一个按钮，不用先在下拉里再选一次。
                 std::string model_id = opt_str(body.value(), "modelId");
                 if (model_id.empty()) {
                     auto saved = ledger.get_setting(token, llm::settings::kKeyLocalModelId);
                     if (saved.is_ok() && saved.value().has_value()) {
                         model_id = *saved.value();
                     }
                 }
                 if (model_id.empty()) {
                     send_error(res, ErrorCode::InvalidArgument,
                                "没有指定模型。先登记一个 GGUF 文件，或在设置里选一个本地模型。");
                     return;
                 }

                 auto started = models.ensure_ready(model_id);
                 if (started.is_err()) { send_error(res, started.error()); return; }

                 // 起成功后把它记成当前账号的本地模型选择，下次不用再传
                 (void)ledger.set_setting(token, llm::settings::kKeyLocalModelId, model_id);

                 json j = deployment_to_json(started.value());
                 j["note"] = "服务已就绪，现在「自动」或「本地」模式下会直接用它生成报告。";
                 send_ok(res, j);
             });

    srv.Post("/api/local-model/stop",
             [&models](const httplib::Request& req, httplib::Response& res) {
                 std::string token;
                 if (!guard(req, res, token, false)) return;

                 auto st = models.stop();
                 if (st.is_err()) { send_error(res, st.error()); return; }
                 send_ok(res, json{{"stopped", true},
                                   {"note", "llama-server 已停止，本地通道暂时不可用。"}});
             });

    srv.Get("/api/local-model/log",
           [&models](const httplib::Request& req, httplib::Response& res) {
               std::string token;
               if (!guard(req, res, token, false)) return;

               const int lines = query_int(req, "lines", 40);
               auto tail = models.tail_log_lines(lines);
               if (tail.is_err()) { send_error(res, tail.error()); return; }
               send_ok(res, json{{"log", tail.value()}});
           });
}

}  // namespace penhu::server::detail
