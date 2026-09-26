#pragma once
// =============================================================================
//  penhu/llm/settings.hpp
//  模型通道的默认值与设置键名（纯常量，无依赖，header-only）。
//
//  这里只放「名字和默认值」，不放读取逻辑。读取逻辑在 app/llm_settings.hpp ——
//  因为它需要访问某个账号的加密设置表，会把 llm 层反向依赖到 app 层，不划算。
//
//  设置存在哪里：
//    按账号存进该账号的加密账本（SettingsRepository）。
//      - API Key 走 set_secret，用账本密钥再做一层 AEAD，密文落盘；
//      - base_url / model / 通道选择走 set_plain。
//    为什么不做成「一个全局 config.json」：同机多账号应该各用各的 Key 和模型，
//    放全局文件里要么所有账号共用一个 Key，要么得在文件里自己加密，
//    都不如直接用已经验证过的账号级加密存储。
//
//  环境变量可以覆盖（优先级最高）。存在的理由有两个，都是实用的：
//    1) 排障时不用为了换个 endpoint 去改账号设置；
//    2) 让我能在没有任何界面交互的情况下跑通端到端验证。
// =============================================================================

namespace penhu::llm::settings {

// -----------------------------------------------------------------------------
//  默认值
//
//  默认通道刻意选云端：本地通道要跑一个 GGUF，占显存——这台机器只有一张
//  8GB 的 4060，还在跑训练。让「生成一份日报」去抢显存不划算，而本地小模型
//  的归纳质量也确实不如云端。报告是给人读的东西，质量更值钱。
// -----------------------------------------------------------------------------

inline constexpr const char* kDefaultCloudBaseUrl = "https://api.deepseek.com/v1";
inline constexpr const char* kDefaultCloudModel   = "deepseek-chat";
inline constexpr int         kDefaultTimeoutSec   = 90;

/// 本地通道的默认上下文窗口。4096 是保守值：多数消费级 GGUF 量化模型
/// 在这个窗口下还能正常工作，再大就容易在 8GB 显存上 OOM。
inline constexpr int kDefaultLocalContext = 4096;

// -----------------------------------------------------------------------------
//  设置键名
// -----------------------------------------------------------------------------

/// "cloud" / "local" / "auto"
///   cloud —— 只用云端
///   local —— 只用本地
///   auto  —— 云端优先，失败降级到本地（默认）
inline constexpr const char* kKeyChannel         = "llm.channel";
inline constexpr const char* kKeyCloudBaseUrl    = "llm.cloud.base_url";
inline constexpr const char* kKeyCloudModel      = "llm.cloud.model";
inline constexpr const char* kKeyCloudApiKey     = "llm.cloud.api_key";     // secret
inline constexpr const char* kKeyLocalModelId    = "llm.local.model_id";
inline constexpr const char* kKeyLocalAutoStart  = "llm.local.auto_start";

// -----------------------------------------------------------------------------
//  环境变量名
// -----------------------------------------------------------------------------

inline constexpr const char* kEnvChannel     = "PENHU_LLM_CHANNEL";
inline constexpr const char* kEnvCloudBase   = "PENHU_CLOUD_BASE_URL";
inline constexpr const char* kEnvCloudModel  = "PENHU_CLOUD_MODEL";
inline constexpr const char* kEnvCloudApiKey = "PENHU_CLOUD_API_KEY";
inline constexpr const char* kEnvLocalModelId = "PENHU_LOCAL_MODEL_ID";

}  // namespace penhu::llm::settings
