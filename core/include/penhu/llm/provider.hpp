#pragma once
// =============================================================================
//  penhu/llm/provider.hpp
//  大语言模型接入层。
//
//  设计要点（两条，都值得先说清楚）：
//
//  1) 为什么「本地」和「云端」共用一个实现类
//     本地 llama.cpp 的 llama-server 和 DeepSeek/OpenAI 都提供
//     OpenAI 兼容的 POST /v1/chat/completions。写两个几乎一样的 HTTP 客户端
//     只会让「其中一个忘了处理超时」这类 bug 的概率翻倍。
//     所以差异全部收敛到 LlmConfig：base_url / api_key / 超时 / 模型名。
//     「双通道」依然完整，降级链也是真的，只是没有重复代码。
//
//  2) 为什么强制检查「明文 HTTP + 非回环地址」
//     这个应用会把你的消费明细发出去。发到 127.0.0.1 的本地模型没问题，
//     发到公网必须走 TLS。如果有人把 base_url 配成 http://某公网域名/v1，
//     代码会直接拒绝而不是照发——因为那是把账本明文丢到网络上。
//     要覆盖这个行为必须显式设置 allow_plaintext_http。
//
//  另有一个 RuleBased 通道：它不是「模型」，是把统计结果套模板成文字。
//  它不参与 FailoverProvider 的降级链，而是由 ReportService 在
//  「所有真模型都失败」时兜底，保证用户永远能拿到一份报告。
// =============================================================================

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "penhu/crypto/secure_buffer.hpp"
#include "penhu/util/result.hpp"

namespace penhu::llm {

enum class ProviderKind {
    RuleBased   = 0,   // 模板渲染，不调模型
    LocalLlama  = 1,   // 本地 llama-server（OpenAI 兼容）
    CloudOpenAi = 2    // 云端 OpenAI 兼容服务
};

const char* to_string(ProviderKind kind) noexcept;
/// 存档用的稳定标识："rule-based" / "local-llama" / "cloud-openai"
const char* storage_id(ProviderKind kind) noexcept;

struct LlmConfig {
    ProviderKind kind{ProviderKind::LocalLlama};

    /// 不含 /chat/completions。例如 "http://127.0.0.1:8080/v1"
    std::string base_url;
    std::string model;
    /// 云端需要；本地留空。**绝不写入日志**。
    std::string api_key;

    int    timeout_seconds{90};
    double temperature{0.4};
    int    max_tokens{1200};

    /// 是否允许对非回环地址使用明文 http。默认 false，见文件头说明。
    bool allow_plaintext_http{false};

    static LlmConfig local_llama(const std::string& base_url = "http://127.0.0.1:8080/v1",
                                const std::string& model  = "penhu-v0.1.0");
    static LlmConfig cloud_openai(const std::string& base_url,
                                  const std::string& model,
                                  const std::string& api_key);

    /// 校验配置；不合法直接给出可读原因
    Status validate() const;

    /// 存档与日志用的标签："local-llama/penhu-v0.1.0"
    std::string label() const;

    /// 实际请求地址
    std::string endpoint() const;

    /// 是否指向本机
    bool is_loopback() const;
};

struct LlmMessage {
    std::string role;      // "system" | "user" | "assistant"
    std::string content;

    /// 可选：随这条消息一起发的图片，格式是 data URL
    /// （"data:image/jpeg;base64,...."）。非空时 content 会按 OpenAI 的
    /// 多模态数组格式发出（[{type:text}, {type:image_url}]）而不是纯字符串 ——
    /// 多数服务端对「纯字符串里塞 base64」是直接忽略或报 400 的。
    ///
    /// 注意：这里只接受 data URL，不接受本地路径或 http 链接。
    /// 本机文件的读取与缩放（WIC）属于界面层的职责，llm 层保持零文件依赖。
    std::string image_data_url;
};

struct LlmRequest {
    std::vector<LlmMessage> messages;
};

struct LlmResponse {
    std::string text;
    std::string model;
    int         prompt_tokens{0};
    int         completion_tokens{0};
    int64_t     latency_ms{0};
    ProviderKind provider{ProviderKind::RuleBased};
    std::string provider_label;
};

class ILLMProvider {
public:
    virtual ~ILLMProvider() = default;

    virtual ProviderKind kind() const noexcept = 0;
    virtual std::string  label() const = 0;

    /// 快速可用性检查。默认实现返回 ok；HTTP 通道会真的发一个极轻的请求。
    virtual Status probe() = 0;

    virtual Result<LlmResponse> complete(const LlmRequest& request) = 0;
};

/// 建一个 OpenAI 兼容的 provider（本地或云端都走这里）
std::unique_ptr<ILLMProvider> make_openai_compatible(const LlmConfig& config);

// -----------------------------------------------------------------------------
//  降级链
// -----------------------------------------------------------------------------

/// 按顺序尝试多个 provider，谁先成功用谁。
/// 每一步的失败原因都会留在 attempts() 里，界面可以展示「为什么降级了」。
class FailoverProvider : public ILLMProvider {
public:
    explicit FailoverProvider(std::vector<std::unique_ptr<ILLMProvider>> chain);

    ProviderKind kind() const noexcept override { return ProviderKind::CloudOpenAi; }
    std::string  label() const override;
    Status       probe() override;

    Result<LlmResponse> complete(const LlmRequest& request) override;

    /// 上一次 complete 调用里，每个 provider 的尝试结果（含成功的那个）
    struct Attempt {
        std::string label;
        bool        succeeded{false};
        std::string error;
        int64_t     latency_ms{0};
    };
    const std::vector<Attempt>& last_attempts() const noexcept { return last_attempts_; }

    bool all_failed() const noexcept { return all_failed_; }

private:
    std::vector<std::unique_ptr<ILLMProvider>> chain_;
    std::vector<Attempt> last_attempts_;
    bool all_failed_{false};
};

}  // namespace penhu::llm
