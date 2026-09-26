// =============================================================================
//  penhu/deploy/local_model.cpp
//  本地模型部署框架的实现。设计取舍见头文件，这里只记实现层面几个硬决定：
//
//  1) 运行时包（zip）用系统的 tar.exe 解压，不引三方 zip 库。
//     Windows 10 1803+ 自带 bsdtar（C:\Windows\System32\tar.exe），它认 zip。
//     为一个「偶尔用一次」的解压动作去引 miniz/libzip，既增加体积又增加
//     一条要维护的依赖链，不划算。找不到 tar.exe 会明确报错而不是静默失败。
//
//  2) 子进程的 stdout/stderr 重定向到文件（由 process::ChildProcess 负责），
//     不用管道——llama-server 会持续刷刷地打日志，管道缓冲区满了就死锁。
//
//  3) 版本号一律不硬编码。llama.cpp 现在是 nightly 发版，tag 形如 b6xxx，
//     而且**不是每个 release 都带 Windows 二进制**。所以流程是
//     「拉最近 N 个 release → 找第一个真正含目标资产的」，而不是
//     「拼一个 tag 去下载」。后者失效时表现为 404，排查起来还要分辨
//     是网络问题还是版本号过期。
//
//  4) 走代理。这台机器直连 GitHub 不通，必须经系统代理。
//     cpp-httplib 不会自己读环境变量，所以这里显式读 HTTP(S)_PROXY 并
//     set_proxy。这算「顺着环境走」而不是「替用户做决定」。
//
//  5) GGUF 只做「魔数 + 版本 + 大小」的浅检查。深校验（张量表完整性）
//     要读整个文件头结构，收益不大——真正的校验是把它喂给 llama-server，
//     起不来会给出明确报错。所以这里刻意保持浅。
// =============================================================================

#include "penhu/deploy/local_model.hpp"

#include "penhu/util/fs.hpp"
#include "penhu/util/log.hpp"
#include "penhu/util/uuid.hpp"

#if defined(PENHU_HTTPLIB_SSL)
#  define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#include <httplib.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

namespace penhu::deploy {

// -----------------------------------------------------------------------------
//  枚举的字符串表示
// -----------------------------------------------------------------------------

const char* to_string(RuntimeFlavor flavor) noexcept {
    switch (flavor) {
        case RuntimeFlavor::Cpu:    return "cpu";
        case RuntimeFlavor::Vulkan: return "vulkan";
        case RuntimeFlavor::Cuda:   return "cuda";
    }
    return "unknown";
}

const char* asset_marker(RuntimeFlavor flavor) noexcept {
    // 匹配的是 release 资产名里的片段，例如
    //   llama-b6818-bin-win-cpu-x64.zip
    //   llama-b6818-bin-win-vulkan-x64.zip
    //   llama-b6818-bin-win-cuda-12.4-x64.zip
    // CUDA 版的名字里带驱动版本，所以这里只匹配到 "cuda-"，剩下的交给
    // 「结尾必须是 x64.zip」这条规则兜住。
    switch (flavor) {
        case RuntimeFlavor::Cpu:    return "bin-win-cpu-x64";
        case RuntimeFlavor::Vulkan: return "bin-win-vulkan-x64";
        case RuntimeFlavor::Cuda:   return "bin-win-cuda-";
    }
    return "\x01";   // 不可能匹配到的串
}

namespace {

using json = nlohmann::json;

constexpr const char* kGithubReleasesApi =
    "https://api.github.com/repos/ggml-org/llama.cpp/releases?per_page=20";
constexpr const char* kReleaseAssetSuffix = "x64.zip";

int64_t now_seconds() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

std::string http_agent() {
    return std::string("PenHuLedger/") + PENHU_LEDGER_VERSION;
}

// ---- URL 拆解 ---------------------------------------------------------------

struct UrlParts {
    std::string scheme;   // http / https
    std::string host;
    int         port{80};
    std::string path;     // 含开头的 '/'，含 query
};

Result<UrlParts> split_url(const std::string& url) {
    UrlParts u;
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) {
        return Result<UrlParts>::fail(ErrorCode::InvalidArgument,
                                      "URL 缺少协议: " + url, "split_url");
    }
    u.scheme = url.substr(0, scheme_end);
    if (u.scheme != "http" && u.scheme != "https") {
        return Result<UrlParts>::fail(ErrorCode::InvalidArgument,
                                      "不支持的协议 '" + u.scheme + "'", "split_url");
    }

    const std::string rest = url.substr(scheme_end + 3);
    const auto slash = rest.find('/');
    std::string authority = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    u.path = (slash == std::string::npos) ? "/" : rest.substr(slash);

    const auto colon = authority.rfind(':');
    if (colon != std::string::npos && authority.find(']') == std::string::npos) {
        u.host = authority.substr(0, colon);
        u.port = std::atoi(authority.substr(colon + 1).c_str());
    } else {
        u.host = authority;
    }
    if (u.port == 0) u.port = (u.scheme == "https") ? 443 : 80;
    if (u.host.empty()) {
        return Result<UrlParts>::fail(ErrorCode::InvalidArgument,
                                      "URL 缺少主机名: " + url, "split_url");
    }
    return Result<UrlParts>(std::move(u));
}

// ---- 代理 -------------------------------------------------------------------

struct ProxySetting {
    bool        enabled{false};
    std::string host;
    int         port{0};
};

/// 读环境变量里的代理。大小写两种写法都认（Windows 上两种都常见）。
ProxySetting detect_proxy(const std::string& scheme) {
    ProxySetting p;
    const char* keys[] = {"HTTPS_PROXY", "https_proxy", "HTTP_PROXY", "http_proxy",
                          "ALL_PROXY", "all_proxy"};
    // https 优先看 HTTPS_PROXY，http 优先看 HTTP_PROXY
    const char* ordered[6];
    if (scheme == "https") {
        ordered[0] = keys[0]; ordered[1] = keys[1];
        ordered[2] = keys[2]; ordered[3] = keys[3];
        ordered[4] = keys[4]; ordered[5] = keys[5];
    } else {
        ordered[0] = keys[2]; ordered[1] = keys[3];
        ordered[2] = keys[0]; ordered[3] = keys[1];
        ordered[4] = keys[4]; ordered[5] = keys[5];
    }

    std::string raw;
    for (const char* k : ordered) {
        const char* v = std::getenv(k);
        if (v != nullptr && *v != '\0') {
            raw = v;
            break;
        }
    }
    if (raw.empty()) return p;

    // 形如 http://127.0.0.1:7897
    std::string hostport = raw;
    const auto scheme_end = hostport.find("://");
    if (scheme_end != std::string::npos) hostport = hostport.substr(scheme_end + 3);
    const auto slash = hostport.find('/');
    if (slash != std::string::npos) hostport = hostport.substr(0, slash);
    const auto at = hostport.rfind('@');            // 带用户名密码的情况
    if (at != std::string::npos) hostport = hostport.substr(at + 1);

    const auto colon = hostport.rfind(':');
    if (colon == std::string::npos) {
        p.host = hostport;
        p.port = 8080;
    } else {
        p.host = hostport.substr(0, colon);
        p.port = std::atoi(hostport.substr(colon + 1).c_str());
    }
    p.enabled = !p.host.empty() && p.port > 0;
    return p;
}

// ---- HTTP -------------------------------------------------------------------

// err 标 maybe_unused：编译期开了 TLS 时，下面那段「拒绝 https」的分支
// 整块被编掉，err 就没人用了 —— GCC 会报 -Wunused-parameter。
std::unique_ptr<httplib::Client> make_client(const UrlParts& u, int timeout_seconds,
                                            [[maybe_unused]] std::string* err) {
#if !defined(PENHU_HTTPLIB_SSL)
    if (u.scheme == "https") {
        if (err != nullptr) {
            *err =
                "该构建没有启用 TLS 支持，无法访问 https。请使用带 cpp-httplib[openssl] "
                "的构建（vcpkg.json 里已声明该 feature），或改用本地 http 地址。";
        }
        return nullptr;
    }
#endif

    const std::string base = u.scheme + "://" + u.host + ":" + std::to_string(u.port);
    auto cli = std::make_unique<httplib::Client>(base);
    cli->set_follow_location(true);
    cli->set_connection_timeout(15, 0);
    cli->set_read_timeout(timeout_seconds, 0);
    cli->set_write_timeout(timeout_seconds, 0);
    cli->set_default_headers({{"User-Agent", http_agent()},
                              {"Accept", "application/vnd.github+json"}});

    const ProxySetting proxy = detect_proxy(u.scheme);
    if (proxy.enabled) {
        cli->set_proxy(proxy.host, proxy.port);
    }
    return cli;
}

/// 取一段文本（GitHub API 用）
Result<std::string> http_get_text(const std::string& url, int timeout_seconds = 30) {
    PENHU_ASSIGN_OR_RETURN(u, split_url(url));
    std::string err;
    auto cli = make_client(u, timeout_seconds, &err);
    if (cli == nullptr) {
        return Result<std::string>::fail(ErrorCode::NetworkFailure, err, "http_get_text");
    }

    auto res = cli->Get(u.path.c_str());
    if (!res) {
        return Result<std::string>::fail(
            ErrorCode::NetworkFailure,
            "请求失败: " + httplib::to_string(res.error()) + "  ← " + url,
            "http_get_text");
    }
    if (res->status < 200 || res->status >= 300) {
        std::string hint;
        if (res->status == 403) {
            hint = "（GitHub 未认证接口限流是 60 次/小时，稍后再试即可）";
        } else if (res->status == 404) {
            hint = "（地址不存在）";
        }
        return Result<std::string>::fail(
            ErrorCode::NetworkFailure,
            "HTTP " + std::to_string(res->status) + " " + hint + "  ← " + url,
            "http_get_text");
    }
    return Result<std::string>(res->body);
}

/// 下载到文件，带进度回调。写完会校验大小不为 0。
Status http_download_to_file(const std::string& url, const std::string& dest_path,
                            ProgressFn on_progress, const std::string& stage) {
    PENHU_ASSIGN_OR_RETURN(u, split_url(url));
    std::string err;
    auto cli = make_client(u, 600, &err);
    if (cli == nullptr) {
        return status_err(ErrorCode::NetworkFailure, err, "http_download_to_file");
    }

    std::ofstream ofs(dest_path, std::ios::binary | std::ios::trunc);
    if (!ofs.good()) {
        return status_err(ErrorCode::StorageFailure,
                          "无法写入文件: " + dest_path, "http_download_to_file");
    }

    int64_t written = 0;
    auto res = cli->Get(
        u.path.c_str(),
        [&](const char* data, std::size_t len) {
            ofs.write(data, static_cast<std::streamsize>(len));
            written += static_cast<int64_t>(len);
            return static_cast<bool>(ofs);
        },
        [&](uint64_t current, uint64_t total) {
            if (on_progress) {
                on_progress(stage, static_cast<int64_t>(current),
                            static_cast<int64_t>(total));
            }
            return true;
        });

    ofs.flush();
    const bool write_ok = ofs.good();
    ofs.close();

    if (!res) {
        return status_err(ErrorCode::NetworkFailure,
                          "下载失败: " + httplib::to_string(res.error()) + "  ← " + url,
                          "http_download_to_file");
    }
    if (res->status != 200) {
        return status_err(ErrorCode::NetworkFailure,
                          "下载返回 HTTP " + std::to_string(res->status) + "  ← " + url,
                          "http_download_to_file");
    }
    if (!write_ok) {
        return status_err(ErrorCode::StorageFailure,
                          "写入过程中出错，文件可能不完整: " + dest_path,
                          "http_download_to_file");
    }
    if (written <= 0) {
        return status_err(ErrorCode::NetworkFailure,
                          "下载到 0 字节，视为失败（避免后面解压出一个空目录）",
                          "http_download_to_file");
    }
    return status_ok();
}

// ---- 解压 -------------------------------------------------------------------

std::string find_tar_exe() {
    const std::string sys_tar = "C:\\Windows\\System32\\tar.exe";
    if (fs::exists(sys_tar)) return sys_tar;
    // 退路：PATH 上的 tar（Git for Windows 也带一个）
    return "tar.exe";
}

Status extract_zip(const std::string& zip_path, const std::string& dest_dir,
                   std::string* output) {
    PENHU_RETURN_IF_ERROR(fs::create_directories(dest_dir));

    const std::string tar = find_tar_exe();
    if (!fs::exists(tar) && tar != "tar.exe") {
        return status_err(ErrorCode::ConfigError,
                          "找不到解压工具。Windows 10 1803+ 自带 " + tar +
                              "，如果被删除请手动恢复，或把 tar 放进 PATH。",
                          "extract_zip");
    }

    // bsdtar 解 zip：-x 解压，-f 指定文件，-C 指定目标目录
    auto r = process::run(tar, {"-xf", zip_path, "-C", dest_dir}, {}, 10 * 60 * 1000);
    if (r.is_err()) {
        return status_err(r.error().code,
                          "调用 tar 解压失败: " + r.error().message, "extract_zip");
    }
    if (output != nullptr) *output = r.value().output;
    if (r.value().exit_code != 0) {
        return status_err(ErrorCode::StorageFailure,
                          "tar 解压返回 " + std::to_string(r.value().exit_code) + ": " +
                              r.value().output.substr(0, 800),
                          "extract_zip");
    }
    return status_ok();
}

/// 在目录树里找指定文件名的文件（限制深度，避免在巨大目录里失控）
///
/// 注意这里必须用 list_entries 而不是 list_files：后者只返回普通文件，
/// 子目录一个都拿不到，递归会在第一层就断掉——而 llama.cpp 的 zip
/// 解压后是否带一层顶层目录是会变的，不递归就等于「装完了却找不到 exe」。
Result<std::string> find_file_recursive(const std::string& dir,
                                       const std::string& want_name,
                                       int max_depth = 3) {
    auto entries = fs::list_entries(dir);
    if (entries.is_err()) return entries.error();

    std::vector<std::string> subdirs;
    for (const std::string& name : entries.value()) {
        const std::string full = fs::join(dir, name);
        if (fs::is_regular_file(full)) {
            if (name == want_name) return Result<std::string>(full);
        } else if (fs::is_directory(full)) {
            subdirs.push_back(full);
        }
    }
    if (max_depth <= 0) {
        return Result<std::string>::fail(ErrorCode::NotFound,
                                         "在 " + dir + " 下没找到 " + want_name,
                                         "find_file_recursive");
    }
    for (const std::string& sub : subdirs) {
        auto r = find_file_recursive(sub, want_name, max_depth - 1);
        if (r.is_ok()) return r;
    }
    return Result<std::string>::fail(ErrorCode::NotFound,
                                     "在 " + dir + " 下没找到 " + want_name,
                                     "find_file_recursive");
}

Status write_file_atomic(const std::string& path, const std::string& content) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (!ofs.good()) {
            return status_err(ErrorCode::StorageFailure, "无法写入 " + tmp, "write_file_atomic");
        }
        ofs << content;
        if (!ofs.good()) {
            return status_err(ErrorCode::StorageFailure,
                              "写入 " + tmp + " 过程中出错", "write_file_atomic");
        }
    }
    // 先写临时文件再改名：避免写到一半断电/崩溃留下一个半截的 JSON，
    // 下次启动解析失败会把整个登记表丢掉。
    return fs::rename(tmp, path);
}

Result<std::string> read_file_text(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.good()) {
        return Result<std::string>::fail(ErrorCode::NotFound, "无法读取 " + path,
                                         "read_file_text");
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return Result<std::string>(oss.str());
}

}  // namespace

// -----------------------------------------------------------------------------
//  登记表（只有路径和参数，没有秘密，所以用明文 JSON）
// -----------------------------------------------------------------------------

struct LocalModelManager::Registry {
    std::string   runtime_server_exe;    // 自动安装后解析出的绝对路径
    std::string   runtime_release_tag;   // 从哪个 release 装的，仅作记录
    int           runtime_flavor{0};
    std::string   custom_server_exe;     // 用户手工指定的（优先）
    std::string   preferred_model_id;    // 上次用的模型，方便「一键启动」
    std::vector<LocalModelEntry> models;
};

// -----------------------------------------------------------------------------
//  构造 / 析构
// -----------------------------------------------------------------------------

LocalModelManager::LocalModelManager(app::AppConfig config)
    : config_(std::move(config)), registry_(std::make_unique<Registry>()) {
    // 目录建不起来也不在这里报错：构造期抛异常会让整个应用起不来，
    // 而「本地模型」只是可选功能，不该拖垮记账本身。
    // 真正的失败会在 install_runtime / start 时以 Result 形式报出来。
    auto st = config_.ensure_directories();
    if (st.is_err()) {
        Logger::default_logger().warn("本地模型管理器：目录创建失败 —— " +
                                      st.error().message);
    }
    auto lr = load_registry();
    if (lr.is_err()) {
        // 登记表读不出来不是致命错误（可能是第一次运行），
        // 但如果是「文件在、但解析失败」就必须留痕，否则用户会觉得
        // 「我登记的模型怎么没了」。
        Logger::default_logger().warn("本地模型登记表加载失败（按空表继续）—— " +
                                      lr.error().message);
    }
}

LocalModelManager::~LocalModelManager() {
    // 进程句柄析构时会连带结束子进程（Job Object + KILL_ON_JOB_CLOSE），
    // 所以这里不用显式 stop()。但要走一遍 stop 让日志留下痕迹。
    if (server_process_ && server_process_->is_running()) {
        Logger::default_logger().info("本地模型管理器析构：停止 llama-server");
        stop();
    }
}

// ---- 路径 -------------------------------------------------------------------

std::string LocalModelManager::runtime_dir() const {
    return fs::join(config_.data_dir, "runtime");
}
std::string LocalModelManager::downloads_dir() const {
    return fs::join(runtime_dir(), "downloads");
}
std::string LocalModelManager::server_log_path() const {
    return fs::join(runtime_dir(), "llama-server.log");
}
std::string LocalModelManager::registry_path() const {
    return fs::join(config_.data_dir, "local_models.json");
}

// ---- 登记表读写 -------------------------------------------------------------

Status LocalModelManager::load_registry() {
    const std::string path = registry_path();
    if (!fs::exists(path)) {
        // 首次运行：不算错误，写回一份空的让文件落盘（方便用户自己看格式）
        return save_registry();
    }

    auto text = read_file_text(path);
    if (text.is_err()) return text.error();

    json j;
    try {
        j = json::parse(text.value());
    } catch (const std::exception& e) {
        return status_err(ErrorCode::ConfigError,
                          std::string("local_models.json 解析失败: ") + e.what(),
                          "load_registry");
    }

    Registry reg;
    reg.runtime_server_exe = j.value("runtime_server_exe", std::string{});
    reg.runtime_release_tag = j.value("runtime_release_tag", std::string{});
    reg.runtime_flavor = j.value("runtime_flavor", 0);
    reg.custom_server_exe = j.value("custom_server_exe", std::string{});
    reg.preferred_model_id = j.value("preferred_model_id", std::string{});

    if (j.contains("models") && j["models"].is_array()) {
        for (const auto& m : j["models"]) {
            LocalModelEntry e;
            e.id = m.value("id", std::string{});
            e.display_name = m.value("display_name", std::string{});
            e.gguf_path = m.value("gguf_path", std::string{});
            e.note = m.value("note", std::string{});
            e.size_bytes = m.value("size_bytes", int64_t{0});
            e.added_at = m.value("added_at", int64_t{0});
            e.context_size = m.value("context_size", 4096);
            e.gpu_layers = m.value("gpu_layers", 0);
            e.threads = m.value("threads", 0);
            if (!e.id.empty() && !e.gguf_path.empty()) {
                reg.models.push_back(std::move(e));
            }
        }
    }

    *registry_ = std::move(reg);
    return status_ok();
}

Status LocalModelManager::save_registry() const {
    json j;
    j["version"] = 1;
    j["runtime_server_exe"] = registry_->runtime_server_exe;
    j["runtime_release_tag"] = registry_->runtime_release_tag;
    j["runtime_flavor"] = registry_->runtime_flavor;
    j["custom_server_exe"] = registry_->custom_server_exe;
    j["preferred_model_id"] = registry_->preferred_model_id;

    json arr = json::array();
    for (const auto& m : registry_->models) {
        json o;
        o["id"] = m.id;
        o["display_name"] = m.display_name;
        o["gguf_path"] = m.gguf_path;
        o["note"] = m.note;
        o["size_bytes"] = m.size_bytes;
        o["added_at"] = m.added_at;
        o["context_size"] = m.context_size;
        o["gpu_layers"] = m.gpu_layers;
        o["threads"] = m.threads;
        arr.push_back(std::move(o));
    }
    j["models"] = std::move(arr);

    return write_file_atomic(registry_path(), j.dump(2));
}

// ---- 运行时状态 -------------------------------------------------------------

RuntimeStatus LocalModelManager::runtime_status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return runtime_status_unlocked();
}

RuntimeStatus LocalModelManager::runtime_status_unlocked() const {
    RuntimeStatus st;
    st.flavor = static_cast<RuntimeFlavor>(registry_->runtime_flavor);

    // 优先用户手工指定的
    if (!registry_->custom_server_exe.empty()) {
        st.directory = fs::parent(registry_->custom_server_exe);
        st.server_exe = registry_->custom_server_exe;
        st.release_tag = "(用户指定)";
        st.installed = fs::exists(st.server_exe) && fs::is_regular_file(st.server_exe);
        if (!st.installed) {
            st.error = "指定路径上的文件不存在了: " + st.server_exe;
        } else {
            st.size_bytes = fs::file_size(st.server_exe);
        }
        return st;
    }

    // 再看自动安装记录
    if (!registry_->runtime_server_exe.empty()) {
        st.server_exe = registry_->runtime_server_exe;
        st.directory = fs::parent(st.server_exe);
        st.release_tag = registry_->runtime_release_tag;
        st.installed = fs::exists(st.server_exe) && fs::is_regular_file(st.server_exe);
        if (!st.installed) {
            st.error = "运行时文件不存在了（可能被清理）: " + st.server_exe;
        } else {
            st.size_bytes = fs::file_size(st.server_exe);
        }
        return st;
    }

    st.error = "尚未安装运行时。点「获取运行时」会自动从 llama.cpp 官方 release "
               "下载解压，或手工指定一个已有的 llama-server.exe。";
    return st;
}

Status LocalModelManager::set_runtime_path(const std::string& server_exe) {
    if (server_exe.empty()) {
        // 传空 = 取消自定义，回到自动安装的运行时
        std::lock_guard<std::mutex> lock(mutex_);
        registry_->custom_server_exe.clear();
        return save_registry();
    }
    if (!fs::exists(server_exe)) {
        return status_err(ErrorCode::NotFound, "文件不存在: " + server_exe,
                          "set_runtime_path");
    }
    if (!fs::is_regular_file(server_exe)) {
        return status_err(ErrorCode::InvalidArgument, "不是普通文件: " + server_exe,
                          "set_runtime_path");
    }
    if (fs::filename(server_exe) != "llama-server.exe") {
        // 硬性要求文件名：用户很容易指到 llama-cli.exe 或 llama-bench.exe 上，
        // 那两个不会提供 HTTP 接口，报错要到「健康检查超时」才出现，
        // 很难联想到是选错了可执行文件。这里直接拦掉。
        return status_err(ErrorCode::InvalidArgument,
                          "请指向 llama-server.exe（当前选的是 " +
                              fs::filename(server_exe) +
                              "）。llama-cli / llama-bench 不提供 HTTP 接口。",
                          "set_runtime_path");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    registry_->custom_server_exe = fs::absolute(server_exe);
    return save_registry();
}

// ---- 获取运行时 -------------------------------------------------------------

Result<RuntimeStatus> LocalModelManager::install_runtime(RuntimeFlavor flavor,
                                                        ProgressFn on_progress) {
    // 已经有可用的就直接返回，避免重复下载 100+MB
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const RuntimeStatus current = runtime_status_unlocked();
        if (current.installed && current.flavor == flavor) {
            Logger::default_logger().info("运行时已存在，跳过下载: " + current.server_exe);
            return Result<RuntimeStatus>(current);
        }
    }

    if (on_progress) on_progress("正在查询 llama.cpp 最新发布", 0, 0);

    auto releases_text = http_get_text(kGithubReleasesApi, 30);
    if (releases_text.is_err()) {
        return Result<RuntimeStatus>::fail(
            releases_text.error().code,
            "无法获取 llama.cpp 发布列表 —— " + releases_text.error().message +
                "。本机若需要代理，请设置 HTTPS_PROXY 环境变量。",
            "install_runtime");
    }

    json releases;
    try {
        releases = json::parse(releases_text.value());
    } catch (const std::exception& e) {
        return Result<RuntimeStatus>::fail(
            ErrorCode::NetworkFailure,
            std::string("GitHub API 返回的内容不是 JSON（可能被代理/门户页面拦了）: ") +
                e.what(),
            "install_runtime");
    }
    if (!releases.is_array() || releases.empty()) {
        return Result<RuntimeStatus>::fail(ErrorCode::NetworkFailure,
                                          "发布列表为空", "install_runtime");
    }

    // 遍历找第一个真正带目标资产的 release。
    // 这就是「不硬编码版本号」的落点：llama.cpp 是 nightly 发版，
    // 有些 build 只发 tag 不带二进制，硬编码必然迟早 404。
    const std::string marker = asset_marker(flavor);
    std::string asset_url;
    std::string asset_name;
    std::string release_tag;

    for (const auto& rel : releases) {
        const std::string tag = rel.value("tag_name", std::string{});
        if (!rel.contains("assets") || !rel["assets"].is_array()) continue;
        for (const auto& a : rel["assets"]) {
            const std::string name = a.value("name", std::string{});
            if (name.find(marker) == std::string::npos) continue;
            if (!(name.size() >= std::strlen(kReleaseAssetSuffix) &&
                  name.compare(name.size() - std::strlen(kReleaseAssetSuffix),
                               std::strlen(kReleaseAssetSuffix),
                               kReleaseAssetSuffix) == 0)) {
                continue;   // 必须是以 x64.zip 结尾，避免抓到 debug/符号包
            }
            asset_url = a.value("browser_download_url", std::string{});
            asset_name = name;
            release_tag = tag;
            break;
        }
        if (!asset_url.empty()) break;
    }

    if (asset_url.empty()) {
        return Result<RuntimeStatus>::fail(
            ErrorCode::NotFound,
            std::string("最近 20 个 release 里没有 ") + to_string(flavor) +
                " 的 Windows 二进制包。可能是这个后端暂时没在发，"
                "换个类型（CPU 版兼容性最好）或稍后再试。",
            "install_runtime");
    }

    Logger::default_logger().info("安装本地模型运行时: " + asset_name + "（" +
                                  release_tag + "）");

    PENHU_RETURN_IF_ERROR(fs::create_directories(downloads_dir()));
    const std::string zip_path = fs::join(downloads_dir(), asset_name);

    // 已经下过且大小合理就复用（弱校验：只查大小 > 1MB 且不是明显损坏）
    const int64_t existing = fs::file_size(zip_path);
    if (existing > 1024 * 1024) {
        if (on_progress) on_progress("复用已下载的运行时包", existing, existing);
    } else {
        PENHU_RETURN_IF_ERROR(http_download_to_file(
            asset_url, zip_path, on_progress, "下载运行时（" + asset_name + "）"));
    }

    const std::string install_dir = fs::join(runtime_dir(), "llama");
    if (on_progress) on_progress("正在解压运行时", 0, 0);

    std::string tar_output;
    PENHU_RETURN_IF_ERROR(extract_zip(zip_path, install_dir, &tar_output));

    auto exe = find_file_recursive(install_dir, "llama-server.exe", 3);
    if (exe.is_err()) {
        return Result<RuntimeStatus>::fail(
            ErrorCode::NotFound,
            "解压完成但没找到 llama-server.exe。解压目录: " + install_dir +
                "；tar 输出: " + tar_output.substr(0, 400),
            "install_runtime");
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        registry_->runtime_server_exe = exe.value();
        registry_->runtime_release_tag = release_tag;
        registry_->runtime_flavor = static_cast<int>(flavor);
        PENHU_RETURN_IF_ERROR(save_registry());
    }

    if (on_progress) {
        on_progress("运行时已就绪", fs::file_size(exe.value()), fs::file_size(exe.value()));
    }
    return Result<RuntimeStatus>(runtime_status());
}

// ---- GGUF 检查 --------------------------------------------------------------

GgufInfo LocalModelManager::inspect_gguf(const std::string& path) {
    GgufInfo info;

    if (!fs::exists(path)) {
        info.problem = "文件不存在: " + path;
        return info;
    }
    if (!fs::is_regular_file(path)) {
        info.problem = "不是普通文件（是不是指到目录上了？）: " + path;
        return info;
    }
    info.size_bytes = fs::file_size(path);
    if (info.size_bytes < 32) {
        info.problem = "文件太小（" + std::to_string(info.size_bytes) +
                       " 字节），不可能是 GGUF";
        return info;
    }

    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.good()) {
        info.problem = "打不开文件（权限？被占用？）: " + path;
        return info;
    }

    char magic[4] = {0, 0, 0, 0};
    ifs.read(magic, 4);
    if (ifs.gcount() != 4 || std::memcmp(magic, "GGUF", 4) != 0) {
        char shown[5] = {0};
        std::memcpy(shown, magic, 4);
        info.problem = std::string("不是 GGUF 格式（文件头是 \"") + shown +
                       "\"，应为 \"GGUF\"）。注意 .gguf 后缀并不代表内容正确，"
                       "也可能是没下完的文件。";
        return info;
    }

    // 紧接着的 4 字节是 uint32 小端版本号
    unsigned char vbuf[4] = {0, 0, 0, 0};
    ifs.read(reinterpret_cast<char*>(vbuf), 4);
    info.gguf_version = static_cast<uint32_t>(vbuf[0]) |
                        (static_cast<uint32_t>(vbuf[1]) << 8) |
                        (static_cast<uint32_t>(vbuf[2]) << 16) |
                        (static_cast<uint32_t>(vbuf[3]) << 24);

    if (info.gguf_version == 0 || info.gguf_version > 10) {
        // 版本号离谱通常是「文件被截断/损坏」或者「根本不是 llama.cpp 出的 GGUF」。
        // 不当成致命错误：只给提示，让 llama-server 自己去做最终判定。
        info.problem = "GGUF 版本号可疑（" + std::to_string(info.gguf_version) +
                       "）。文件可能损坏或来自非 llama.cpp 的实现，仍可尝试启动。";
    }

    info.valid = true;
    return info;
}

// ---- 模型登记 ---------------------------------------------------------------

Result<LocalModelEntry> LocalModelManager::add_model(const std::string& display_name,
                                                    const std::string& gguf_path,
                                                    const std::string& note) {
    if (display_name.empty()) {
        return Result<LocalModelEntry>::fail(ErrorCode::InvalidArgument,
                                             "名称不能为空", "add_model");
    }
    if (gguf_path.empty()) {
        return Result<LocalModelEntry>::fail(
            ErrorCode::InvalidArgument,
            "请给出 GGUF 文件的本地路径。本框架不代下载模型，权重由你自己提供。",
            "add_model");
    }

    const GgufInfo info = inspect_gguf(gguf_path);
    if (!info.valid) {
        return Result<LocalModelEntry>::fail(ErrorCode::InvalidArgument, info.problem,
                                             "add_model");
    }

    // 去重：同一个文件路径不重复登记
    const std::string abs_path = fs::absolute(gguf_path);

    LocalModelEntry e;
    e.id = new_uuid();
    e.display_name = display_name;
    e.gguf_path = abs_path;
    e.note = note;
    e.size_bytes = info.size_bytes;
    e.added_at = now_seconds();
    // 上下文默认取 4096：比 llama.cpp 默认值保守。440M 这类小模型
    // 给太大上下文反而容易跑出重复内容，用户可以在界面上调。
    e.context_size = 4096;
    e.gpu_layers = 0;
    e.threads = 0;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& m : registry_->models) {
            if (m.gguf_path == abs_path) {
                return Result<LocalModelEntry>::fail(
                    ErrorCode::AlreadyExists,
                    "这个文件已经登记过了（id=" + m.id + "，名称=" + m.display_name + "）",
                    "add_model");
            }
        }
        registry_->models.push_back(e);
        PENHU_RETURN_IF_ERROR(save_registry());
    }

    if (!info.problem.empty()) {
        Logger::default_logger().warn("登记的模型有可疑之处（仍已加入）: " + info.problem);
    }
    return Result<LocalModelEntry>(std::move(e));
}

Result<std::vector<LocalModelEntry>> LocalModelManager::list_models() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return Result<std::vector<LocalModelEntry>>(registry_->models);
}

Status LocalModelManager::remove_model(const std::string& id) {
    // 先判断「正在跑的是不是它」，判断完就放锁。
    // stop() 自己会加锁，所以绝不能在持锁状态下调它。
    bool must_stop = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        must_stop = running_.server_running && running_.active_model_id == id;
    }
    if (must_stop) {
        Logger::default_logger().info("被删除的模型正在运行，先停服务");
        PENHU_RETURN_IF_ERROR(stop());
    }

    std::lock_guard<std::mutex> lock(mutex_);

    const auto it =
        std::find_if(registry_->models.begin(), registry_->models.end(),
                     [&](const LocalModelEntry& m) { return m.id == id; });
    if (it == registry_->models.end()) {
        return status_err(ErrorCode::NotFound, "没有这个模型: " + id, "remove_model");
    }
    if (registry_->preferred_model_id == id) {
        registry_->preferred_model_id.clear();
    }

    registry_->models.erase(it);
    // 注意：只删登记，**不动用户的 GGUF 文件**。
    // 那是用户自己的数据，删掉不可恢复，绝不能由「从列表里移除」这个动作代劳。
    return save_registry();
}

Status LocalModelManager::update_model_params(const std::string& id, int context_size,
                                             int gpu_layers, int threads) {
    if (context_size < 128 || context_size > 1024 * 1024) {
        return status_err(ErrorCode::InvalidArgument,
                          "上下文大小不合理: " + std::to_string(context_size) +
                              "（应在 128 ~ 1048576 之间）",
                          "update_model_params");
    }
    if (gpu_layers < 0 || gpu_layers > 1000) {
        return status_err(ErrorCode::InvalidArgument,
                          "GPU 层数不合理: " + std::to_string(gpu_layers),
                          "update_model_params");
    }
    if (threads < 0 || threads > 1024) {
        return status_err(ErrorCode::InvalidArgument,
                          "线程数不合理: " + std::to_string(threads),
                          "update_model_params");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& m : registry_->models) {
        if (m.id != id) continue;
        m.context_size = context_size;
        m.gpu_layers = gpu_layers;
        m.threads = threads;
        return save_registry();
    }
    return status_err(ErrorCode::NotFound, "没有这个模型: " + id, "update_model_params");
}

// ---- 端口与健康检查 ---------------------------------------------------------

int LocalModelManager::find_free_port(int start_from) {
    for (int port = start_from; port < start_from + 200; ++port) {
        httplib::Client probe("http://127.0.0.1:" + std::to_string(port));
        probe.set_connection_timeout(0, 300 * 1000);   // 300ms，µs 为单位
        probe.set_read_timeout(0, 300 * 1000);
        auto res = probe.Get("/health");
        if (!res && res.error() == httplib::Error::Connection) {
            // 连接被拒绝 = 这个端口没人监听
            return port;
        }
        // 其它情况（有响应、超时、其它错误）都当作「可能被占用」，跳过。
        // 宁可多试几个端口，也不要在别人端口上强起一个服务。
    }
    return 0;
}

Result<bool> LocalModelManager::wait_for_health(int port, int timeout_ms,
                                               std::string* last_error) const {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    std::string last;

    while (std::chrono::steady_clock::now() < deadline) {
        httplib::Client cli("http://127.0.0.1:" + std::to_string(port));
        cli.set_connection_timeout(0, 500 * 1000);
        cli.set_read_timeout(1, 0);
        auto res = cli.Get("/health");

        if (res && res->status == 200) {
            return Result<bool>(true);
        }
        if (res && res->status == 503) {
            // llama-server 在加载模型时会用 503 表示「还没好」，
            // 这是**正常中间态**，不能算失败。
            last = "服务已起来，正在加载模型（HTTP 503）";
        } else if (res) {
            last = "健康检查返回 HTTP " + std::to_string(res->status);
        } else {
            last = "还没连上: " + httplib::to_string(res.error());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    if (last_error != nullptr) {
        *last_error = last.empty() ? "超时且没有任何响应" : (last + "（直到超时）");
    }
    return Result<bool>(false);
}

// ---- 服务生命周期 -----------------------------------------------------------

Result<DeploymentStatus> LocalModelManager::start(const std::string& model_id,
                                                 ProgressFn on_progress) {
    // ---- 1) 加锁阶段：只做查询与状态快照，**绝不在这里调 stop()** ----
    //
    //  stop() 自己也会锁 mutex_，而 std::mutex 不可重入。
    //  如果这里抱着锁去调 stop()，第一次切换模型就会死锁——而且症状是
    //  「界面卡住没有任何反应」，完全没有报错，极难定位。
    //  所以下面用花括号把锁的作用域收窄，锁外再做耗时/会再次加锁的事。
    LocalModelEntry entry;
    bool            need_stop = false;
    std::string     previous_name;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        const LocalModelEntry* found = nullptr;
        for (const auto& m : registry_->models) {
            if (m.id == model_id) {
                found = &m;
                break;
            }
        }
        if (found == nullptr) {
            return Result<DeploymentStatus>::fail(ErrorCode::NotFound,
                                                  "没有这个模型: " + model_id, "start");
        }
        // 拷贝一份出来：后面会释放锁，不能再持有指向 registry_->models 的指针
        // （remove_model / update_model_params 都可能让迭代器失效）。
        entry = *found;

        if (!fs::exists(entry.gguf_path)) {
            return Result<DeploymentStatus>::fail(
                ErrorCode::NotFound,
                "模型文件不见了: " + entry.gguf_path + "（可能被移动或删除）", "start");
        }

        // 已经在跑同一个模型 → 幂等返回
        if (server_process_ && server_process_->is_running() &&
            running_.active_model_id == model_id) {
            return Result<DeploymentStatus>(running_);
        }

        need_stop = (server_process_ && server_process_->is_running());
        previous_name = running_.active_model_name;
    }

    // ---- 2) 锁外阶段：停旧模型、装运行时、起进程 ----

    // 在跑别的模型 → 先停。同时只跑一个：一张 8GB 显卡上跑两个模型
    // 只会两个都变慢，显存还更容易爆。
    if (need_stop) {
        PENHU_RETURN_IF_ERROR(stop());
        Logger::default_logger().info("已停止上一个模型（" + previous_name + "），切换中");
    }

    // 运行时
    RuntimeStatus rt = runtime_status();
    if (!rt.installed) {
        auto installed = install_runtime(
            static_cast<RuntimeFlavor>(registry_->runtime_flavor), on_progress);
        if (installed.is_err()) return installed.error();
        rt = installed.value();
    }

    // 挑端口
    const int port = find_free_port(18080);
    if (port == 0) {
        return Result<DeploymentStatus>::fail(
            ErrorCode::Internal,
            "从 18080 起连续 200 个端口都不可用，无法启动本地服务", "start");
    }

    if (on_progress) on_progress("正在启动 llama-server", 0, 0);

    std::vector<std::string> args = {
        "-m", entry.gguf_path,
        "--host", "127.0.0.1",
        "--port", std::to_string(port),
        "-c", std::to_string(entry.context_size),
        "-ngl", std::to_string(entry.gpu_layers),
    };
    if (entry.threads > 0) {
        args.push_back("-t");
        args.push_back(std::to_string(entry.threads));
    }

    const std::string log_path = server_log_path();
    auto spawned = process::ChildProcess::spawn(rt.server_exe, args, fs::parent(rt.server_exe),
                                               log_path);
    if (spawned.is_err()) {
        return Result<DeploymentStatus>::fail(
            spawned.error().code,
            "启动 llama-server 失败: " + spawned.error().message +
                "；可执行文件=" + rt.server_exe,
            "start");
    }

    // llama-server 对 model 字段不校验，随便给一个非空串即可。
    // 这里用文件名（去掉扩展名），报错日志里一眼能认出是哪个权重。
    std::string stem = fs::filename(entry.gguf_path);
    const auto dot = stem.rfind('.');
    if (dot != std::string::npos && dot > 0) stem = stem.substr(0, dot);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        server_process_ = std::make_unique<process::ChildProcess>(std::move(spawned).value());

        running_ = DeploymentStatus{};
        running_.runtime = rt;
        running_.server_running = true;
        running_.server_port = port;
        running_.server_pid = server_process_->pid();
        running_.active_model_id = entry.id;
        running_.active_model_name = entry.display_name;
        running_.base_url = "http://127.0.0.1:" + std::to_string(port) + "/v1";
        running_.model_field = stem.empty() ? "local-model" : stem;
        running_.log_path = log_path;
        running_.started_at = now_seconds();
        running_.last_error.clear();

        registry_->preferred_model_id = entry.id;
        (void)save_registry();   // 记不住偏好不影响本次运行，失败不阻断启动
    }

    // 健康检查：给 120 秒。加载一个几 GB 的模型要几十秒，
    // 小模型则几秒。这个上限对小模型偏长，但「等久了」比「误判失败」好——
    // 误判会去杀掉一个其实正在正常加载的进程。
    std::string last_error;
    auto healthy = wait_for_health(port, 120 * 1000, &last_error);

    if (healthy.is_err() || !healthy.value()) {
        // 失败时把日志尾部带出来。这是最重要的诊断信息：
        // 「端口被占」「模型格式不支持」「显存不足」在这里的报错完全不同。
        std::string tail;
        auto t = tail_log_lines(30);
        if (t.is_ok()) tail = t.value();

        std::string failure_reason =
            healthy.is_err() ? healthy.error().message : last_error;
        bool still_running = false;
        int  code = -1;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            still_running = server_process_ && server_process_->is_running();
            code = server_process_ ? server_process_->exit_code() : -1;
            if (server_process_) {
                server_process_->terminate(3000);
                server_process_.reset();
            }
            running_.server_running = false;
            running_.base_url.clear();
            running_.server_port = 0;
            running_.server_pid = 0;
            running_.last_error = failure_reason;
        }

        std::string msg = "llama-server 启动后未能通过健康检查。";
        msg += still_running ? "进程仍在运行（可能加载太慢）。" : "进程已退出";
        if (!still_running) msg += "，退出码 " + std::to_string(code) + "。";
        msg += " 原因: " + failure_reason;
        if (!tail.empty()) msg += "\n--- llama-server 日志尾部 ---\n" + tail;

        return Result<DeploymentStatus>::fail(ErrorCode::Internal, msg, "start");
    }

    if (on_progress) on_progress("本地模型已就绪", 1, 1);

    std::lock_guard<std::mutex> lock(mutex_);
    Logger::default_logger().info("llama-server 就绪: " + running_.base_url + "（模型 " +
                                  running_.active_model_name + "）");
    return Result<DeploymentStatus>(running_);
}

Status LocalModelManager::stop() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!server_process_) {
        running_.server_running = false;
        running_.base_url.clear();
        running_.server_port = 0;
        running_.server_pid = 0;
        return status_ok();
    }

    // 温和终止优先：llama-server 收到 Ctrl 类信号会做清理。
    // 但这个进程是我们直接 CreateProcess 起来的、没有控制台，所以
    // terminate 内部大概率走的是强杀路径——这也够用，它没有需要落盘的状态。
    auto st = server_process_->terminate(5000);
    const int code = server_process_->exit_code();
    server_process_.reset();

    running_.server_running = false;
    running_.base_url.clear();
    running_.server_port = 0;
    running_.server_pid = 0;
    running_.active_model_id.clear();
    running_.active_model_name.clear();

    if (st.is_err()) {
        return status_err(st.error().code,
                          "停止 llama-server 时出错（进程已强制结束，退出码 " +
                              std::to_string(code) + "）: " + st.error().message,
                          "stop");
    }
    return status_ok();
}

DeploymentStatus LocalModelManager::status() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (server_process_ && !server_process_->is_running()) {
        // 进程自己死了（崩了 / 被外部杀了）。如实反映，
        // 不要让界面一直显示「运行中」然后所有请求都失败。
        running_.server_running = false;
        running_.base_url.clear();
        const int code = server_process_->exit_code();
        if (running_.last_error.empty()) {
            running_.last_error = "llama-server 已退出，退出码 " + std::to_string(code);
        }
        server_process_.reset();
    }

    running_.runtime = runtime_status_unlocked();
    return running_;
}

Result<DeploymentStatus> LocalModelManager::ensure_ready(const std::string& model_id,
                                                        ProgressFn on_progress) {
    // 已经跑起来了就直接返回
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (server_process_ && server_process_->is_running() &&
            running_.active_model_id == model_id) {
            return Result<DeploymentStatus>(running_);
        }
    }

    // 「一键就绪」= 运行时 → 模型 → 起服务。每步失败都直接报出来，
    // 不做「自动换个模型再试」这种自作主张的兜底。
    RuntimeStatus rt = runtime_status();
    if (!rt.installed) {
        if (on_progress) on_progress("尚未安装运行时，开始获取", 0, 0);
        auto installed = install_runtime(
            static_cast<RuntimeFlavor>(registry_->runtime_flavor), on_progress);
        if (installed.is_err()) return installed.error();
    }

    // start() 内部会自己再检查一遍运行时（幂等）
    return start(model_id, std::move(on_progress));
}

Result<llm::LlmConfig> LocalModelManager::build_local_config() const {
    std::lock_guard<std::mutex> lock(mutex_);

    // 服务没在跑时**明确报错**，而不是给一个「看起来能用」的配置。
    // 给空配置的后果是：报告生成静默降级到模板兜底，用户以为「本地模型不管用」。
    if (!running_.server_running || running_.base_url.empty()) {
        return Result<llm::LlmConfig>::fail(
            ErrorCode::NotFound,
            "本地模型服务当前没有运行，无法生成本地通道配置。"
            "请先在「本地模型」里启动一个模型。",
            "build_local_config");
    }

    llm::LlmConfig cfg = llm::LlmConfig::local_llama(running_.base_url,
                                                     running_.model_field);
    // 本地小模型的归纳能力有限，超时给短一点：与其卡 90 秒，
    // 不如快速失败降级到云端/模板。用户可以自己改。
    cfg.timeout_seconds = 60;
    cfg.max_tokens = 700;
    return Result<llm::LlmConfig>(std::move(cfg));
}

Result<std::string> LocalModelManager::tail_log_lines(int max_lines) const {
    const std::string path = server_log_path();
    if (!fs::exists(path)) {
        return Result<std::string>(std::string("（还没有日志文件，服务可能尚未启动过）"));
    }

    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.good()) {
        return Result<std::string>::fail(ErrorCode::StorageFailure,
                                         "无法读取日志 " + path, "tail_log_lines");
    }

    // 日志可能很大（llama-server 会一直打），不整个读进内存：
    // 从尾部倒着读 256KB 足够了。
    ifs.seekg(0, std::ios::end);
    const std::streamoff size = ifs.tellg();
    const std::streamoff want = 256 * 1024;
    const std::streamoff start = (size > want) ? (size - want) : 0;
    ifs.seekg(start, std::ios::beg);

    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
    if (start > 0) {
        // 从中间截断会得到半行，丢掉它
        const auto nl = content.find('\n');
        if (nl != std::string::npos) content = content.substr(nl + 1);
    }

    std::vector<std::string> lines;
    std::string cur;
    std::istringstream iss(content);
    while (std::getline(iss, cur)) {
        if (!cur.empty() && cur.back() == '\r') cur.pop_back();
        lines.push_back(cur);
    }

    const int n = (max_lines <= 0) ? 40 : max_lines;
    const std::size_t begin =
        (lines.size() > static_cast<std::size_t>(n)) ? lines.size() - n : 0;

    std::ostringstream out;
    for (std::size_t i = begin; i < lines.size(); ++i) {
        out << lines[i] << "\n";
    }
    return Result<std::string>(out.str());
}

}  // namespace penhu::deploy
