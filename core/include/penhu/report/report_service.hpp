#pragma once
// =============================================================================
//  penhu/report/report_service.hpp
//  报告的编排层：取数 -> 分析 -> 组 prompt -> 调模型 -> 存档。
//
//  三条硬性保证：
//
//   1) 数字只有一个来源。
//      界面显示的数字和喂给模型的数字都来自同一个 ReportFacts。
//      所以我不会出现「报告里说花了 1200，饼图显示 1150」这种自相矛盾。
//
//   2) 永远有产出。
//      所有模型通道都失败时，用规则模板渲染一份报告交出去，并且
//      明确标记 used_fallback 和失败原因。用户不会看到「生成失败」的空白页。
//
//   3) 模型的话不被信任到数字层面。
//      ReportResult 同时带 content（模型文字）和 facts（结构化数字）。
//      界面必须用 facts 渲染所有数字，content 只作为「解读」展示。
//      原因很实际：440M 的本地模型经常把数字抄错，把它输出的数字当真会误导用户。
// =============================================================================

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "penhu/analytics/anomaly.hpp"
#include "penhu/analytics/insight.hpp"
#include "penhu/analytics/summary.hpp"
#include "penhu/analytics/trend.hpp"
#include "penhu/app/ledger_service.hpp"
#include "penhu/llm/provider.hpp"
#include "penhu/report/prompt_builder.hpp"
#include "penhu/util/result.hpp"

namespace penhu::report {

struct GenerateOptions {
    ReportKind kind{ReportKind::Daily};
    Date       anchor;                    // 无效时取今天
    /// 不指定则按 provider 类型自动选档
    std::optional<PromptStyle> style;
    bool use_cache{true};
    bool force_regenerate{false};
};

struct ReportResult {
    std::string content;                  // 模型产出或模板渲染的正文
    std::string provider;                 // "local-llama" / "cloud-openai" / "rule-based"
    std::string model;
    llm::ProviderKind provider_kind{llm::ProviderKind::RuleBased};

    bool    from_cache{false};
    bool    used_fallback{false};         // true 表示模型全部失败，内容是模板生成的
    bool    stale_model_output{false};    // 来自存档，可能不是当前模型生成的

    PromptStyle style_used{PromptStyle::Compact};
    int64_t     latency_ms{0};

    std::vector<std::string> failover_notes;   // 每个通道的失败原因
    ReportFacts              facts;            // 界面渲染数字只准用它
};

/// 结构化分析结果（不调模型）。统计页直接用这个。
struct Analysis {
    analytics::Dashboard        dashboard;
    analytics::TrendResult      trend;
    analytics::AnomalyReport    anomalies;
    analytics::InsightSet       insights;
    ReportFacts                 facts;
};

class ReportService {
public:
    ReportService(app::LedgerService& ledger,
                  llm::LlmConfig local_config,
                  llm::LlmConfig cloud_config);

    ReportService(const ReportService&) = delete;
    ReportService& operator=(const ReportService&) = delete;

    /// 只做统计，不调模型。快，可以随界面刷新随便调。
    Result<Analysis> analyze(const std::string& token,
                             ReportKind kind,
                             const Date& anchor);

    /// 同上，但按任意区间算（界面上的「本周/本月/自定义」用它）。
    ///
    /// 为什么单独一个入口而不是让 analyze 接受可选区间：报告是按
    /// 「日/周/月」这三种对齐好的区间存档的，统计页则是用户随手拉的一段范围，
    /// 两者的取值口径本来就不一样。混成一个函数，将来必然有人传进一个
    /// 和 kind 矛盾的范围，然后数字对不上。
    ///
    /// trend_window 是滑动平均与环比用的窗口天数。
    /// kind_for_facts 只影响 facts 里标注的报告类型标签，不影响任何数字。
    Result<Analysis> analyze_range(const std::string& token,
                                  const DateRange& range,
                                  int trend_window,
                                  ReportKind kind_for_facts = ReportKind::Daily);

    /// 出报告。内部会先 analyze，再走模型。
    Result<ReportResult> generate(const std::string& token,
                                 const GenerateOptions& options);

    /// 纯模板渲染。没有模型时也保证能出东西。
    static std::string render_rule_based(const ReportFacts& facts);

    /// 有哪些模型通道可用（用于界面提示「当前只能用本地模型」之类）
    struct ChannelStatus {
        bool local_configured{false};
        bool local_reachable{false};
        bool cloud_configured{false};
        bool cloud_reachable{false};
        std::string local_label;
        std::string cloud_label;
    };
    ChannelStatus probe_channels();

    /// 运行期覆盖通道配置。构造参数只是兜底默认值。
    ///
    /// 为什么必须有这个：云端 API Key 是按账号加密存在该账号账本里的，
    /// 同机多账号必须各用各的；本地通道能不能用，取决于 llama-server
    /// 此刻有没有在跑。两者都不是构造时能知道的事。
    ///
    /// 传 base_url 为空的配置表示「该通道不可用」。
    void set_channels(std::optional<llm::LlmConfig> cloud,
                      std::optional<llm::LlmConfig> local);

    /// 当前生效的通道配置（诊断用）
    const llm::LlmConfig& local_config() const noexcept { return local_cfg_; }
    const llm::LlmConfig& cloud_config() const noexcept { return cloud_cfg_; }

private:
    /// 建降级链：云端优先，本地兜底（顺序即优先级，见 .cpp 里的说明）
    std::vector<std::unique_ptr<llm::ILLMProvider>> build_chain(
        std::vector<std::string>* skip_notes);

    /// 加锁拷一份当前生效的配置，避免握着配置锁去发网络请求
    struct ChannelConfigs {
        std::optional<llm::LlmConfig> cloud;
        std::optional<llm::LlmConfig> local;
    };
    ChannelConfigs snapshot_channels() const;

    app::LedgerService& ledger_;
    llm::LlmConfig      local_cfg_;
    llm::LlmConfig      cloud_cfg_;

    /// 哪个通道可用。默认由构造时的 validate() 决定，set_channels 可覆盖。
    bool local_enabled_{false};
    bool cloud_enabled_{false};

    mutable std::mutex config_mutex_;
};

}  // namespace penhu::report
