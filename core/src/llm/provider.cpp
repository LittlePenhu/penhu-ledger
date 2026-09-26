#include "penhu/llm/provider.hpp"
#include "penhu/domain/date.hpp"
#include "penhu/util/log.hpp"

// 启用 TLS 支持（由 CMake 根据 vcpkg 的 cpp-httplib[openssl] feature 决定）。
// 没启用时 https 会被明确拒绝，而不是静默退化成明文——见 validate()。
#if defined(PENHU_HTTPLIB_SSL)
#  define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#include <httplib.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace penhu::llm {
namespace {

using json = nlohmann::json;

bool ensure_http_ready() {
#if defined(PENHU_HTTPLIB_SSL)
    return true;
#else
    return true;   // 明文通道始终可用；https 会在 validate 阶段被拦
#endif
}

struct UrlParts {
    std::string scheme;      // "http" / "https"
    std::string host;
    int         port{80};
    std::string path_prefix; // 不含尾部斜杠，例如 "/v1"
};

Result<UrlParts> parse_url(const std::string& url) {
    UrlParts out;

    const size_t scheme_end = url.find("://");
    if (scheme_end == std::string::npos) {
        return Result<UrlParts>::fail(ErrorCode::ConfigError,
                                     "URL 缺少协议（应为 http:// 或 https://）: " + url,
                                     "parse_url");
    }
    out.scheme = url.substr(0, scheme_end);
    std::transform(out.scheme.begin(), out.scheme.end(), out.scheme.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (out.scheme != "http" && out.scheme != "https") {
        return Result<UrlParts>::fail(ErrorCode::ConfigError,
                                     "不支持的协议 '" + out.scheme + "'（只支持 http/https）",
                                     "parse_url");
    }

    std::string rest = url.substr(scheme_end + 3);
    const size_t path_start = rest.find('/');
    std::string authority = (path_start == std::string::npos) ? rest : rest.substr(0, path_start);
    out.path_prefix = (path_start == std::string::npos) ? "" : rest.substr(path_start);

    // 去掉尾部的 '/'，保证后面拼 endpoint 时不会出现双斜杠
    while (!out.path_prefix.empty() && out.path_prefix.back() == '/') {
        out.path_prefix.pop_back();
    }

    if (authority.empty()) {
        return Result<UrlParts>::fail(ErrorCode::ConfigError, "URL 缺少主机名: " + url,
                                     "parse_url");
    }

    // IPv6 字面量：[::1]:8080
    if (authority.front() == '[') {
        const size_t close = authority.find(']');
        if (close == std::string::npos) {
            return Result<UrlParts>::fail(ErrorCode::ConfigError,
                                         "IPv6 地址格式错误（缺少 ]）: " + url, "parse_url");
        }
        out.host = authority.substr(1, close - 1);
        if (close + 1 < authority.size() && authority[close + 1] == ':') {
            out.port = std::atoi(authority.substr(close + 2).c_str());
        }
    } else {
        const size_t colon = authority.rfind(':');
        if (colon == std::string::npos) {
            out.host = authority;
        } else {
            out.host = authority.substr(0, colon);
            out.port = std::atoi(authority.substr(colon + 1).c_str());
        }
    }

    if (out.host.empty()) {
        return Result<UrlParts>::fail(ErrorCode::ConfigError, "URL 主机名为空: " + url,
                                     "parse_url");
    }
    if (out.port <= 0 || out.port > 65535) {
        out.port = (out.scheme == "https") ? 443 : 80;
    }
    return Result<UrlParts>(std::move(out));
}

bool is_loopback_host(const std::string& host) {
    if (host == "localhost") return true;
    if (host == "::1") return true;
    if (host == "0.0.0.0") return true;      // 本机绑定，仍属本地
    if (host.rfind("127.", 0) == 0) return true;
    return false;
}

/// HTTP 状态码 -> 本项目的错误码
ErrorCode code_for_status(int status) {
    if (status == 401 || status == 403) return ErrorCode::AuthFailed;
    if (status == 404) return ErrorCode::ConfigError;      // 端点路径写错了
    if (status == 429) return ErrorCode::NetworkFailure;   // 限流，可降级
    if (status >= 500) return ErrorCode::NetworkFailure;
    if (status >= 400) return ErrorCode::LlmFailure;
    return ErrorCode::Internal;
}

/// 响应体可能很长，且可能包含用户数据。截断后再进错误信息。
std::string truncate(const std::string& text, size_t max_len = 300) {
    if (text.size() <= max_len) return text;
    return text.substr(0, max_len) + "...(截断，共 " + std::to_string(text.size()) + " 字节)";
}

int64_t now_millis() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace

// -----------------------------------------------------------------------------
//  枚举
// -----------------------------------------------------------------------------

const char* to_string(ProviderKind kind) noexcept {
    switch (kind) {
        case ProviderKind::RuleBased:   return "RuleBased";
        case ProviderKind::LocalLlama:  return "LocalLlama";
        case ProviderKind::CloudOpenAi: return "CloudOpenAi";
    }
    return "Unknown";
}

const char* storage_id(ProviderKind kind) noexcept {
    switch (kind) {
        case ProviderKind::RuleBased:   return "rule-based";
        case ProviderKind::LocalLlama:  return "local-llama";
        case ProviderKind::CloudOpenAi: return "cloud-openai";
    }
    return "unknown";
}

// -----------------------------------------------------------------------------
//  LlmConfig
// -----------------------------------------------------------------------------

LlmConfig LlmConfig::local_llama(const std::string& base_url, const std::string& model) {
    LlmConfig c;
    c.kind = ProviderKind::LocalLlama;
    c.base_url = base_url;
    c.model = model;
    // 本地模型小，生成慢，给足超时；但也不能无限等，否则界面会像卡死
    c.timeout_seconds = 120;
    c.temperature = 0.3;
    // 440M 的模型写长了就开始重复和跑题，压到 600 token 反而质量更稳
    c.max_tokens = 600;
    return c;
}

LlmConfig LlmConfig::cloud_openai(const std::string& base_url,
                                  const std::string& model,
                                  const std::string& api_key) {
    LlmConfig c;
    c.kind = ProviderKind::CloudOpenAi;
    c.base_url = base_url;
    c.model = model;
    c.api_key = api_key;
    c.timeout_seconds = 60;
    c.temperature = 0.4;
    c.max_tokens = 1200;
    return c;
}

Status LlmConfig::validate() const {
    if (kind == ProviderKind::RuleBased) return status_ok();

    if (base_url.empty()) {
        return status_err(ErrorCode::ConfigError, "base_url 为空", "LlmConfig::validate");
    }
    auto parts = parse_url(base_url);
    if (parts.is_err()) return parts.error();
    const UrlParts& p = parts.value();

    if (model.empty()) {
        return status_err(ErrorCode::ConfigError, "model 为空（必须有模型名）",
                          "LlmConfig::validate");
    }

    if (p.scheme == "https") {
#if !defined(PENHU_HTTPLIB_SSL)
        return status_err(
            ErrorCode::ConfigError,
            "该构建没有启用 TLS 支持，无法访问 https 端点。"
            "请用带 cpp-httplib[openssl] feature 的构建，或改用本地 http 端点。",
            "LlmConfig::validate");
#endif
    } else {
        // 明文 http 只允许指向本机
        if (!is_loopback_host(p.host) && !allow_plaintext_http) {
            return status_err(
                ErrorCode::ConfigError,
                "拒绝把消费数据以明文 HTTP 发往非本机地址 '" + p.host +
                    "'。请改用 https，或在配置里显式设置 allow_plaintext_http。",
                "LlmConfig::validate");
        }
    }

    if (timeout_seconds < 1 || timeout_seconds > 600) {
        return status_err(ErrorCode::ConfigError,
                          "超时必须在 1~600 秒之间，当前 " + std::to_string(timeout_seconds),
                          "LlmConfig::validate");
    }
    if (max_tokens < 64 || max_tokens > 32000) {
        return status_err(ErrorCode::ConfigError,
                          "max_tokens 必须在 64~32000 之间，当前 " + std::to_string(max_tokens),
                          "LlmConfig::validate");
    }
    if (temperature < 0.0 || temperature > 2.0) {
        return status_err(ErrorCode::ConfigError, "temperature 必须在 0~2 之间",
                          "LlmConfig::validate");
    }

    // 云端必须有 key。少了 key 会在第一次调用时拿到 401，
    // 那个报错对用户来说很难理解为「我忘了填 key」。
    if (kind == ProviderKind::CloudOpenAi && api_key.empty()) {
        return status_err(ErrorCode::ConfigError,
                          "云端 provider 缺少 api_key", "LlmConfig::validate");
    }

    return status_ok();
}

std::string LlmConfig::label() const {
    return std::string(storage_id(kind)) + "/" + (model.empty() ? "default" : model);
}

std::string LlmConfig::endpoint() const {
    auto parts = parse_url(base_url);
    if (parts.is_err()) return base_url + "/chat/completions";
    return parts.value().path_prefix + "/chat/completions";
}

bool LlmConfig::is_loopback() const {
    auto parts = parse_url(base_url);
    if (parts.is_err()) return false;
    return is_loopback_host(parts.value().host);
}

// -----------------------------------------------------------------------------
//  OpenAI 兼容 provider
// -----------------------------------------------------------------------------
namespace {

class OpenAiCompatibleProvider final : public ILLMProvider {
public:
    explicit OpenAiCompatibleProvider(LlmConfig config) : config_(std::move(config)) {
        auto parts = parse_url(config_.base_url);
        if (parts.is_ok()) {
            parts_ = parts.value();
            // httplib 的字符串构造会把 scheme/host/port 一并解析；
            // 我们自己解析一遍是为了做回环判断和安全检查。
            client_ = std::make_unique<httplib::Client>(config_.base_url);
            client_->set_connection_timeout(5, 0);          // 连不上要快速失败，好降级
            client_->set_read_timeout(config_.timeout_seconds, 0);
            client_->set_write_timeout(10, 0);
            client_->set_follow_location(false);            // 不跟随重定向，避免 key 被转发到别处
            client_->enable_server_certificate_verification(true);
        }
    }

    ProviderKind kind() const noexcept override { return config_.kind; }
    std::string  label() const override { return config_.label(); }

    Status probe() override {
        if (client_ == nullptr) {
            return status_err(ErrorCode::ConfigError, "HTTP 客户端未初始化",
                              "OpenAiCompatible::probe");
        }
        // 本地用 /models 探测（llama-server 支持）；云端也支持，但不强制。
        // 探不通不算失败——有些服务禁了 /models，真正的判据是 chat 调用。
        auto res = client_->Get((parts_.path_prefix + "/models").c_str());
        if (!res) {
            return status_err(ErrorCode::NetworkFailure,
                              std::string("无法连接到 ") + config_.base_url +
                                  "（本地模型服务可能没启动）",
                              "OpenAiCompatible::probe");
        }
        if (res->status >= 400) {
            return status_err(code_for_status(res->status),
                              "探测端点返回 HTTP " + std::to_string(res->status),
                              "OpenAiCompatible::probe");
        }
        return status_ok();
    }

    Result<LlmResponse> complete(const LlmRequest& request) override {
        using Fail = Result<LlmResponse>;

        if (client_ == nullptr) {
            return Fail::fail(ErrorCode::ConfigError,
                              "HTTP 客户端未初始化（base_url 可能非法）: " + config_.base_url,
                              "complete");
        }
        if (request.messages.empty()) {
            return Fail::fail(ErrorCode::InvalidArgument, "请求没有任何 message", "complete");
        }

        // ---- 组请求体 ----
        json body;
        body["model"] = config_.model;
        body["temperature"] = config_.temperature;
        body["max_tokens"] = config_.max_tokens;
        body["stream"] = false;

        json messages = json::array();
        for (const auto& m : request.messages) {
            if (m.image_data_url.empty()) {
                messages.push_back({{"role", m.role}, {"content", m.content}});
            } else {
                // 多模态消息：content 必须是「部件数组」，用 type 区分文本与图片。
                json parts = json::array();
                parts.push_back({{"type", "text"}, {"text", m.content}});
                parts.push_back({{"type", "image_url"},
                                 {"image_url", {{"url", m.image_data_url}}}});
                messages.push_back({{"role", m.role}, {"content", parts}});
            }
        }
        body["messages"] = std::move(messages);

        httplib::Headers headers;
        headers.emplace("Content-Type", "application/json");
        if (!config_.api_key.empty()) {
            headers.emplace("Authorization", "Bearer " + config_.api_key);
        }

        const int64_t started = now_millis();

        auto res = client_->Post(config_.endpoint().c_str(), headers,
                                 body.dump(), "application/json");

        const int64_t elapsed = now_millis() - started;

        if (!res) {
            const auto err = res.error();
            std::string reason;
            switch (err) {
                case httplib::Error::Connection:
                    reason = "连接失败";
                    break;
                case httplib::Error::Timeout:
                    reason = "读取超时（模型可能太慢或没在运行）";
                    break;
                case httplib::Error::Read:
                    reason = "读取响应失败";
                    break;
                case httplib::Error::Write:
                    reason = "发送请求失败";
                    break;
                default:
                    reason = "HTTP 错误";
                    break;
            }
            // 注意：错误信息里绝不包含 prompt 内容或 api_key
            return Fail::fail(ErrorCode::NetworkFailure,
                              "调用 " + config_.label() + " 失败：" + reason +
                                  "（对 " + config_.base_url + "，耗时 " +
                                  std::to_string(elapsed) + "ms）",
                              "complete");
        }

        if (res->status < 200 || res->status >= 300) {
            return Fail::fail(
                code_for_status(res->status),
                "模型服务返回 HTTP " + std::to_string(res->status) + "：" +
                    truncate(res->body),
                "complete");
        }

        // ---- 解析响应 ----
        json parsed;
        try {
            parsed = json::parse(res->body);
        } catch (const std::exception& e) {
            return Fail::fail(ErrorCode::LlmFailure,
                              std::string("响应不是合法 JSON: ") + e.what() +
                                  " | 原文: " + truncate(res->body, 200),
                              "complete");
        }

        // 有些实现把错误放在 body 里但返回 200
        if (parsed.contains("error")) {
            std::string msg = "模型服务返回错误";
            if (parsed["error"].is_object() && parsed["error"].contains("message")) {
                msg += ": " + parsed["error"]["message"].get<std::string>();
            } else {
                msg += ": " + truncate(parsed["error"].dump(), 200);
            }
            return Fail::fail(ErrorCode::LlmFailure, std::move(msg), "complete");
        }

        if (!parsed.contains("choices") || !parsed["choices"].is_array() ||
            parsed["choices"].empty()) {
            return Fail::fail(ErrorCode::LlmFailure,
                              "响应里没有 choices 字段（返回格式不是 OpenAI 兼容？）: " +
                                  truncate(res->body, 200),
                              "complete");
        }

        const auto& choice = parsed["choices"][0];
        if (!choice.contains("message") || !choice["message"].contains("content")) {
            return Fail::fail(ErrorCode::LlmFailure,
                              "choices[0].message.content 缺失: " +
                                  truncate(choice.dump(), 200),
                              "complete");
        }

        std::string text;
        try {
            const auto& content = choice["message"]["content"];
            text = content.is_string() ? content.get<std::string>() : content.dump();
        } catch (const std::exception& e) {
            return Fail::fail(ErrorCode::LlmFailure,
                              std::string("解析 content 失败: ") + e.what(), "complete");
        }

        if (text.empty()) {
            return Fail::fail(ErrorCode::LlmFailure,
                              "模型返回了空内容（可能 max_tokens 设得太小，"
                              "或本地模型已经退化到只会输出空串）",
                              "complete");
        }

        LlmResponse out;
        out.text = std::move(text);
        out.model = parsed.value("model", config_.model);
        out.prompt_tokens = 0;
        out.completion_tokens = 0;
        if (parsed.contains("usage") && parsed["usage"].is_object()) {
            out.prompt_tokens = parsed["usage"].value("prompt_tokens", 0);
            out.completion_tokens = parsed["usage"].value("completion_tokens", 0);
        }
        out.latency_ms = elapsed;
        out.provider = config_.kind;
        out.provider_label = config_.label();
        return Result<LlmResponse>(std::move(out));
    }

private:
    LlmConfig                          config_;
    UrlParts                           parts_{};
    std::unique_ptr<httplib::Client>   client_;
};

}  // namespace

std::unique_ptr<ILLMProvider> make_openai_compatible(const LlmConfig& config) {
    if (config.validate().is_err()) {
        return nullptr;   // 调用方应先 validate；这里再挡一次
    }
    if (!ensure_http_ready()) return nullptr;
    return std::make_unique<OpenAiCompatibleProvider>(config);
}

// -----------------------------------------------------------------------------
//  FailoverProvider
// -----------------------------------------------------------------------------

FailoverProvider::FailoverProvider(std::vector<std::unique_ptr<ILLMProvider>> chain)
    : chain_(std::move(chain)) {}

std::string FailoverProvider::label() const {
    std::string out;
    for (size_t i = 0; i < chain_.size(); ++i) {
        if (i > 0) out += " -> ";
        out += chain_[i]->label();
    }
    return out.empty() ? "empty-chain" : out;
}

Status FailoverProvider::probe() {
    for (auto& p : chain_) {
        auto st = p->probe();
        if (st.is_ok()) return status_ok();
    }
    return status_err(ErrorCode::NetworkFailure, "降级链上所有 provider 都探测失败",
                      "FailoverProvider::probe");
}

Result<LlmResponse> FailoverProvider::complete(const LlmRequest& request) {
    using Fail = Result<LlmResponse>;

    last_attempts_.clear();
    last_attempts_.reserve(chain_.size());
    all_failed_ = true;

    if (chain_.empty()) {
        return Fail::fail(ErrorCode::ConfigError, "降级链为空（没有配置任何模型通道）",
                          "FailoverProvider::complete");
    }

    Logger& log = Logger::default_logger();
    std::string accumulated_errors;

    for (auto& provider : chain_) {
        const int64_t started = now_millis();
        auto result = provider->complete(request);
        const int64_t elapsed = now_millis() - started;

        Attempt attempt;
        attempt.label = provider->label();
        attempt.latency_ms = elapsed;

        if (result.is_ok()) {
            attempt.succeeded = true;
            last_attempts_.push_back(std::move(attempt));
            all_failed_ = false;
            if (last_attempts_.size() > 1) {
                log.warn("已降级到 " + provider->label() + "（前面的通道失败）");
            }
            return result;
        }

        attempt.succeeded = false;
        attempt.error = result.error().to_string();
        last_attempts_.push_back(std::move(attempt));

        if (!accumulated_errors.empty()) accumulated_errors += " | ";
        accumulated_errors += provider->label() + ": " + result.error().message;

        log.warn("provider 失败，尝试下一个: " + result.error().to_string());
    }

    return Fail::fail(ErrorCode::LlmFailure,
                      "所有模型通道都失败了。尝试记录：" + accumulated_errors,
                      "FailoverProvider::complete");
}

}  // namespace penhu::llm
