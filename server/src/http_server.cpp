#include "penhu/server/http_server.hpp"

#include <atomic>
#include <fstream>
#include <iterator>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "api_support.hpp"
#include "penhu/util/fs.hpp"
#include "penhu/util/log.hpp"

namespace penhu::server {
namespace {

using json = nlohmann::json;
using detail::RouteContext;

}  // namespace

// -----------------------------------------------------------------------------
//  Impl
// -----------------------------------------------------------------------------

struct HttpServer::Impl {
    explicit Impl(app::AppConfig cfg, ServerOptions opts)
        : config(std::move(cfg)),
          options(std::move(opts)),
          local_models(config) {}

    app::AppConfig  config;
    ServerOptions   options;

    // 这三个有严格的初始化顺序依赖：reports 持有 ledger 的引用，
    // 所以 ledder 必须先就绪。用智能指针 + 在 create() 里按顺序构造，
    // 而不是靠成员声明顺序去保证——后者改一行声明顺序就会变成 UB，
    // 而那种 bug 在 Windows 上往往表现出来是「偶尔崩在别的地方」。
    std::unique_ptr<app::LedgerService>            ledger_owner;
    app::LedgerService*                            ledger_ptr{nullptr};
    std::unique_ptr<report::ReportService>         reports_owner;
    report::ReportService*                         reports_ptr{nullptr};

    deploy::LocalModelManager                local_models;

    // 路由 lambda 捕获的是它的引用，所以它必须比 srv 活得久 ——
    // 放这里（堆上）而不是 create() 的栈上，详见下面注册处的注释。
    std::unique_ptr<detail::RouteContext>    route_ctx;

    httplib::Server             srv;
    std::thread                 worker;
    std::atomic<bool>           listening{false};
    int                         bound_port{0};
};

// -----------------------------------------------------------------------------
//  构造 / 析构
// -----------------------------------------------------------------------------

HttpServer::HttpServer(app::AppConfig config, ServerOptions options)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(options))) {}

HttpServer::~HttpServer() { stop(); }

Result<std::unique_ptr<HttpServer>> HttpServer::create(app::AppConfig config,
                                                      ServerOptions options) {
    auto server = std::unique_ptr<HttpServer>(new HttpServer(std::move(config), std::move(options)));
    Impl& impl = *server->impl_;

    // ---- 账号库 ----
    PENHU_RETURN_IF_ERROR(impl.config.ensure_directories());

    auto service = app::LedgerService::create(impl.config);
    if (service.is_err()) return service.error();
    impl.ledger_owner = std::move(service).value();
    impl.ledger_ptr = impl.ledger_owner.get();

    // ---- 报告服务：通道配置留空 ----
    // 通道是「按账号 + 运行期」才知道的（云端 Key 加密存在各账号账本里，
    // 本地服务是否在跑也是运行期状态），所以构造时一律给空配置 =
    // 两个通道都不可用，等真正生成报告时再用 set_channels 注入。
    // 这一点很重要：如果这里给一个「看起来可用」的默认配置，
    // 第一条不在预期内的请求就会发到一个没配好的 endpoint 上。
    impl.reports_owner = std::make_unique<report::ReportService>(
        *impl.ledger_ptr, llm::LlmConfig{}, llm::LlmConfig{});
    impl.reports_ptr = impl.reports_owner.get();

    // ---- 路由 ----
    //
    // ⚠ RouteContext 必须存在**堆上**（挂到 Impl 里），不能是 create() 的局部变量。
    //   所有路由 lambda 捕获的是它的引用（[&ctx]），而 lambda 会在这个函数
    //   返回之后才被调用 —— 局部变量一析构，捕获到的就是一块被复用过的栈内存，
    //   于是 handler 里读到的 ledger / reports / local_models 全是垃圾指针。
    //
    //   实测症状（吃了不少时间才定位到）：
    //     · 只捕获 [&ledger] 的路由一切正常（/api/categories、/api/records、/api/me）；
    //     · 捕获 [&ctx] 的路由随机崩（/api/settings、/api/analytics），
    //       而且崩点每次都不一样（一次崩在读设置项、看起来像 SQL 的问题）；
    //     · 单元测试完全测不出来 —— 它不走 register_*_routes 这条路，
    //       也不会有「构造函数已返回、handler 还在用捕获的引用」这种时序。
    //   典型的「能编过、单测全绿、一上 HTTP 就随机崩」，只有端到端跑真实进程才能抓。
    impl.route_ctx = std::make_unique<detail::RouteContext>(
        detail::RouteContext{*impl.ledger_ptr, *impl.reports_ptr, impl.local_models});
    detail::register_auth_routes(impl.srv, *impl.route_ctx);
    detail::register_ledger_routes(impl.srv, *impl.route_ctx);
    detail::register_insight_routes(impl.srv, *impl.route_ctx);
    detail::register_local_model_routes(impl.srv, *impl.route_ctx);

    // ---- 不再提供静态资源 ----
    // 这个服务现在只提供 /api/ 下的 JSON 接口。
    //
    // 原来这里挂着一条把 web/ 目录内嵌进来的静态资源路由，给 WebView2 壳子看。
    // 那个方案已经废弃：界面改成原生自绘（native/，Win32 + Direct2D），
    // 不再有 HTML/CSS/JS，也就不需要这个服务去发网页了。
    //
    // 服务本身留着，是因为它是 CLI 自测与端到端验证的载体：
    // `_e2e_api.sh` 打的就是这里的真实进程 + 真实 TCP。
    // 未匹配的路径由下面统一返回 404 JSON，而不是一个空白页。

    impl.srv.Get(".*", [&impl](const httplib::Request& req, httplib::Response& res) {
        if (req.path.rfind("/api/", 0) == 0) {
            res.status = 404;
            res.set_content(detail::make_error_json(ErrorCode::NotFound,
                                                    "没有这个接口: " + req.path).dump(),
                            "application/json; charset=utf-8");
            return;
        }
        // 非 /api/ 路径：明确告诉调用方这里不是网页服务，
        // 免得有人按 WebView 时代的习惯来访问 / 却发现没有页面。
        detail::send_error(res, ErrorCode::NotFound,
                           "这个服务只提供 /api/ 接口。界面是原生程序（penhu-native.exe），"
                           "没有网页版本。");
    });

    // ---- 统一异常出口 ----
    // 任何 handler 里漏出来的异常都在这里变成 500，而不是让连接被直接掐断。
    // 计账应用里「请求突然断了」比「明确报错」难排查得多。
    impl.srv.set_exception_handler([](const httplib::Request&, httplib::Response& res,
                                      std::exception_ptr ep) {
        std::string what = "未知异常";
        try {
            if (ep) std::rethrow_exception(ep);
        } catch (const std::exception& e) {
            what = e.what();
        } catch (...) {
        }
        Logger::default_logger().error("HTTP handler 抛出异常: " + what);
        res.status = 500;
        res.set_content(detail::make_error_json(ErrorCode::Internal, what).dump(),
                        "application/json; charset=utf-8");
    });

    impl.srv.set_error_handler([](const httplib::Request&, httplib::Response& res) {
        // 走到这里说明路由没匹配上（或 handler 自己设了 4xx/5xx 状态但没写 body）。
        // 只补 body，不覆盖已有状态码。
        if (!res.body.empty()) return;
        if (res.status == 404) {
            res.set_content(detail::make_error_json(ErrorCode::NotFound, "没有这个接口").dump(),
                            "application/json; charset=utf-8");
        }
    });

    if (impl.options.log_requests) {
        impl.srv.set_logger([](const httplib::Request& req, const httplib::Response& res) {
            Logger::default_logger().info(req.method + " " + req.path + " -> " +
                                          std::to_string(res.status));
        });
    }

    // 不在 Ctrl+C 之外的场景打印 httplib 自己的日志
    impl.srv.set_payload_max_length(8 * 1024 * 1024);

    return Result<std::unique_ptr<HttpServer>>(std::move(server));
}

// -----------------------------------------------------------------------------
//  生命周期
// -----------------------------------------------------------------------------

Status HttpServer::start() {
    Impl& impl = *impl_;
    if (impl.listening.load()) return status_ok();

    // 先 bind 再 listen_after_bind：这样在 return 之前端口就已经确定了。
    // 直接用 listen() 是阻塞的、且拿不到「实际分了哪个端口」——
    // 而随机端口对本机应用很重要（避免和别的程序抢占固定端口，
    // 也避免「端口被占 → 换端口 → 用户书签失效」这类麻烦）。
    //
    // ⚠ 两个 API 的返回值语义**完全不同**，这里踩过：
    //     bind_to_port(host, port)  -> bool，只表示成功与否
    //     bind_to_any_port(host)    -> int，才是真正的端口号
    // 把前者的返回值当端口存起来，日志会打出 "http://127.0.0.1:1"
    // （1 就是 true），而服务其实好好地监听着你指定的端口 ——
    // 现象是「curl 能通、但打印出来的地址是错的」，特别容易被当成显示小毛病放过。
    int port = 0;
    if (impl.options.port > 0) {
        if (!impl.srv.bind_to_port(impl.options.bind_address, impl.options.port)) {
            return status_err(ErrorCode::NetworkFailure,
                              "无法绑定 " + impl.options.bind_address + ":" +
                                  std::to_string(impl.options.port) +
                                  "（端口被占用，或被安全软件拦截）",
                              "HttpServer::start");
        }
        port = impl.options.port;
    } else {
        port = impl.srv.bind_to_any_port(impl.options.bind_address);
        if (port <= 0) {
            return status_err(ErrorCode::NetworkFailure,
                              "无法在 " + impl.options.bind_address +
                                  " 上绑定任意空闲端口（可能被安全软件拦截）",
                              "HttpServer::start");
        }
    }
    impl.bound_port = port;

    impl.worker = std::thread([&impl]() {
        Logger::default_logger().info("HTTP 服务开始监听 127.0.0.1:" +
                                      std::to_string(impl.bound_port));
        impl.srv.listen_after_bind();
        Logger::default_logger().info("HTTP 服务已退出监听");
    });
    impl.listening.store(true);
    return status_ok();
}

Status HttpServer::stop() {
    Impl& impl = *impl_;
    if (!impl.listening.load()) return status_ok();

    // 顺序很重要：先让 accept 循环退出，再 join。
    // httplib 的 stop() 是线程安全的，且会让 listen_after_bind 尽快返回。
    impl.srv.stop();
    if (impl.worker.joinable()) impl.worker.join();
    impl.listening.store(false);
    impl.bound_port = 0;
    return status_ok();
}

bool HttpServer::running() const noexcept { return impl_->listening.load(); }

int HttpServer::port() const noexcept { return impl_->bound_port; }

std::string HttpServer::base_url() const {
    return "http://127.0.0.1:" + std::to_string(impl_->bound_port);
}

app::LedgerService&        HttpServer::ledger() noexcept { return *impl_->ledger_ptr; }
report::ReportService&     HttpServer::reports() noexcept { return *impl_->reports_ptr; }
deploy::LocalModelManager& HttpServer::local_models() noexcept { return impl_->local_models; }

const app::AppConfig& HttpServer::config() const noexcept { return impl_->config; }
const ServerOptions&  HttpServer::options() const noexcept { return impl_->options; }

}  // namespace penhu::server
