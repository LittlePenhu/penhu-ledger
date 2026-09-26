#include "penhu/app/llm_settings.hpp"

#include <cstdlib>

#include "penhu/llm/settings.hpp"

namespace penhu::app {
namespace {

using namespace penhu::llm::settings;

/// 环境变量取值；空串视为「没设」，避免 PENHU_CLOUD_API_KEY= 这种写法
/// 被当成有效值，最后报出一个看不懂的 401。
std::optional<std::string> env_value(const char* name) {
    const char* raw = std::getenv(name);
    if (raw == nullptr) return std::nullopt;
    std::string value(raw);
    if (value.empty()) return std::nullopt;
    return value;
}

ChannelMode parse_mode(const std::string& text) {
    if (text == "cloud" || text == "云端") return ChannelMode::Cloud;
    if (text == "local" || text == "本地") return ChannelMode::Local;
    return ChannelMode::Auto;
}

}  // namespace

const char* to_string(ChannelMode mode) noexcept {
    switch (mode) {
        case ChannelMode::Cloud: return "cloud";
        case ChannelMode::Local: return "local";
        case ChannelMode::Auto:  return "auto";
    }
    return "auto";
}

ChannelResolution resolve_channels(LedgerService& ledger, const std::string& token) {
    ChannelResolution out;
    auto note = [&out](std::string msg) { out.notes.push_back(std::move(msg)); };

    // -------------------------------------------------------------------------
    //  1) 通道模式：环境变量 > 账号设置 > 默认 Auto
    // -------------------------------------------------------------------------
    if (auto env_mode = env_value(kEnvChannel)) {
        out.mode = parse_mode(*env_mode);
        note("通道模式来自环境变量 " + std::string(kEnvChannel) + "=" + *env_mode);
    } else {
        auto stored = ledger.get_setting(token, kKeyChannel);
        if (stored.is_ok() && stored.value().has_value()) {
            out.mode = parse_mode(stored.value().value());
            note("通道模式来自账号设置：" + std::string(to_string(out.mode)));
        } else {
            out.mode = ChannelMode::Auto;
        }
    }

    // -------------------------------------------------------------------------
    //  2) 云端
    // -------------------------------------------------------------------------
    std::string base_url = kDefaultCloudBaseUrl;
    std::string model    = kDefaultCloudModel;
    std::string api_key;
    std::string url_src = "内置默认", model_src = "内置默认", key_src;

    if (auto env_url = env_value(kEnvCloudBase)) { base_url = *env_url; url_src = "环境变量"; }
    else {
        auto s = ledger.get_setting(token, kKeyCloudBaseUrl);
        if (s.is_ok() && s.value().has_value() && !s.value()->empty()) {
            base_url = *s.value(); url_src = "账号设置";
        }
    }

    if (auto env_model = env_value(kEnvCloudModel)) { model = *env_model; model_src = "环境变量"; }
    else {
        auto s = ledger.get_setting(token, kKeyCloudModel);
        if (s.is_ok() && s.value().has_value() && !s.value()->empty()) {
            model = *s.value(); model_src = "账号设置";
        }
    }

    if (auto env_key = env_value(kEnvCloudApiKey)) { api_key = *env_key; key_src = "环境变量"; }
    else {
        // 这个键必须是「加密存」的。get_secret 遇到明文存的行会直接报错，
        // 就是为了防止「以为读到的是解密出来的秘密」。
        auto s = ledger.get_secret(token, kKeyCloudApiKey);
        if (s.is_ok() && s.value().has_value() && !s.value()->empty()) {
            api_key = *s.value(); key_src = "账号设置（加密）";
        } else if (s.is_err()) {
            note("读取云端 API Key 失败：" + s.error().message);
        }
    }

    if (api_key.empty()) {
        // 没有 Key 不是错误，是「还没配」。明确说出来，好过让用户对着
        // 一个 401 猜半天。
        note("未配置云端 API Key —— 在设置里填，或设环境变量 " +
             std::string(kEnvCloudApiKey));
    } else {
        llm::LlmConfig cfg = llm::LlmConfig::cloud_openai(base_url, model, api_key);
        cfg.timeout_seconds = kDefaultTimeoutSec;
        auto valid = cfg.validate();
        if (valid.is_err()) {
            note("云端配置不合法：" + valid.error().message);
            note("  base_url 来源=" + url_src + "，model 来源=" + model_src);
        } else {
            out.cloud = std::move(cfg);
            note("云端通道就绪：model=" + model + "（来源 " + model_src +
                 "），base_url 来源=" + url_src + "，Key 来源=" + key_src);
        }
    }

    // -------------------------------------------------------------------------
    //  3) 本地
    // -------------------------------------------------------------------------
    if (auto env_local = env_value(kEnvLocalModelId)) {
        out.local_model_id = *env_local;
        note("本地模型 id 来自环境变量");
    } else {
        auto s = ledger.get_setting(token, kKeyLocalModelId);
        // s 是 Result<optional<string>>：s.value() 拿到的是 optional，
        // 一个 * 就够（多写一层 .value() 会变成对 std::string 解引用）。
        if (s.is_ok() && s.value().has_value()) out.local_model_id = *s.value();
    }

    {
        auto s = ledger.get_setting(token, kKeyLocalAutoStart);
        if (s.is_ok() && s.value().has_value()) {
            out.local_auto_start = (*s.value() == "1" || *s.value() == "true");
        }
    }

    if (out.mode != ChannelMode::Cloud && out.local_model_id.empty()) {
        note("本地通道未登记模型 —— 在设置的「本地模型」里指定一个 GGUF 文件路径");
    }

    return out;
}

}  // namespace penhu::app
