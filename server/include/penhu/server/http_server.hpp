#pragma once
// =============================================================================
//  penhu/server/http_server.hpp
//  进程内 HTTP 服务。
//
//  它为什么要存在（这份应用是「仅本机」的，不是服务端应用）：
//
//    1) 界面需要一个取数通道。
//       WebView2 里的页面不能直接调 C++，中间要么是自定义 postMessage 桥，
//       要么是 HTTP。HTTP 的好处是前端就是普通的网页——同一份 HTML 在
//       浏览器里也能打开（配合 --static-root 调试），不用为桌面环境特制。
//
//    2) 我需要能验证。
//       有了它我可以用 curl 打真实请求确认业务链路，也可以用浏览器打开
//       同一个地址截图确认 M3 界面渲染。只说「代码写完了」没有意义，
//       要能拿出可复现的证据。
//
//  安全约束（都是硬的，不是建议）：
//    · 只绑 127.0.0.1，不绑 0.0.0.0 —— 局域网里其它机器根本连不上
//    · Host 头必须是回环地址 —— 挡 DNS rebinding（恶意网页把域名解析到
//      127.0.0.1 来打本机 API 是真实攻击手法）
//    · 不加任何 CORS 响应头 —— 默认拒绝跨源读取
//    · 写操作一律要 Bearer token —— 而 token 只存在于内存里
//
//  实现上刻意用 pimpl：httplib 是个很大的头文件，把桌面壳的编译单元也拖进来
//  会让每次改一行桌面代码都要重解析一遍它。
// =============================================================================

#include <memory>
#include <optional>
#include <string>

#include "penhu/app/app_config.hpp"
#include "penhu/app/ledger_service.hpp"
#include "penhu/deploy/local_model.hpp"
#include "penhu/report/report_service.hpp"
#include "penhu/util/result.hpp"

namespace penhu::server {

struct ServerOptions {
    /// 监听端口。0 = 让系统分配一个空闲端口（默认，避免和别的程序撞）。
    int port{0};

    /// 监听地址。**不要改**：绑到 0.0.0.0 会把账本接口暴露到局域网。
    std::string bind_address{"127.0.0.1"};

    /// 非空时从磁盘读取前端资源（调试用：改一行 CSS 刷新就能看到）。
    /// 为空时用编译期内嵌的资源 —— 那是发布形态，exe 自带界面。
    std::string static_root;

    /// 把每个请求打一行日志。排障时很有用，平时比较吵。
    bool log_requests{false};
};

class HttpServer {
public:
    /// 建服务：初始化配置目录、打开账号库、建好各服务对象。
    /// 这时还没有开始监听（start() 才监听）。
    static Result<std::unique_ptr<HttpServer>> create(app::AppConfig config,
                                                     ServerOptions options);

    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    /// 开始监听。非阻塞：内部起一个线程跑 accept 循环，
    /// 返回时端口已经绑定好了（port() 有效）。
    Status start();

    /// 停止监听并回收线程。可以重复调用。
    Status stop();

    bool running() const noexcept;

    /// 实际监听端口。start() 之前对随机端口配置返回 0。
    int port() const noexcept;

    /// "http://127.0.0.1:<port>"，供桌面壳/自测使用
    std::string base_url() const;

    app::LedgerService&        ledger() noexcept;
    report::ReportService&     reports() noexcept;
    deploy::LocalModelManager& local_models() noexcept;

    const app::AppConfig&   config() const noexcept;
    const ServerOptions&    options() const noexcept;

private:
    HttpServer(app::AppConfig config, ServerOptions options);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace penhu::server
