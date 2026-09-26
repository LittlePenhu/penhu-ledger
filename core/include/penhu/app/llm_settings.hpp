#pragma once
// =============================================================================
//  penhu/app/llm_settings.hpp
//  把「账号里的设置」+「环境变量」+「内置默认」拼成可用的模型通道配置。
//
//  为什么这件事要放在 app 层而不是 llm 层：
//    读取设置需要访问某个账号的加密账本（LedgerService），
//    如果放进 llm 层就成了 llm -> app 的反向依赖，
//    而 llm 层是设计成可以被单独测试、不依赖存储的。
//
//  优先级：环境变量 > 账号设置 > 内置默认。
//  环境变量排第一不是为了方便，是为了「排障时不用改账号数据」，
//  以及让端到端验证可以在零界面交互下跑起来。
// =============================================================================

#include <optional>
#include <string>
#include <vector>

#include "penhu/app/ledger_service.hpp"
#include "penhu/llm/provider.hpp"

namespace penhu::app {

/// 通道选择。默认 Auto（云端优先，失败降级本地）。
enum class ChannelMode {
    Cloud,   ///< 只用云端
    Local,   ///< 只用本地
    Auto,    ///< 云端优先，本地兜底
};

const char* to_string(ChannelMode mode) noexcept;

struct ChannelResolution {
    ChannelMode mode{ChannelMode::Auto};

    /// 可用的云端配置。没配 Key、或 URL/模型非法时为 nullopt。
    std::optional<llm::LlmConfig> cloud;

    /// 本地通道。框架不代用户选模型，所以这里只是「用户登记过的模型 id」，
    /// 具体能否跑起来由 LocalModelManager 决定。
    std::string local_model_id;
    bool        local_auto_start{false};

    /// 给人看的说明：每个值从哪来、缺了什么。
    /// 界面上必须显示——不然「为什么报告是模板生成的」没法解释。
    std::vector<std::string> notes;
};

/// 解析当前账号的通道配置。不会失败：任何缺项都会变成 notes 里的一条说明，
/// 让上层有机会降级到模板报告，而不是直接报错让用户看到空白页。
ChannelResolution resolve_channels(LedgerService& ledger, const std::string& token);

}  // namespace penhu::app
