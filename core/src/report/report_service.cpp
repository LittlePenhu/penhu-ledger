#include "penhu/report/report_service.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <sstream>

#include "penhu/storage/report_repository.hpp"
#include "penhu/util/log.hpp"

namespace penhu::report {
namespace {

using namespace analytics;

int64_t now_millis() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string yuan(int64_t minor) {
    const bool neg = minor < 0;
    const uint64_t mag = neg ? (~static_cast<uint64_t>(minor) + 1ULL)
                             : static_cast<uint64_t>(minor);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%s¥%llu.%02llu", neg ? "-" : "",
                  static_cast<unsigned long long>(mag / 100ULL),
                  static_cast<unsigned long long>(mag % 100ULL));
    return std::string(buf);
}

std::string percent(double ratio) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%.1f%%", ratio * 100.0);
    return std::string(buf);
}

/// 规则模板会用到当前模型通道的状态，所以这里单独写一个不依赖 LLM 的渲染器。
/// 质量目标是「像人写的日记小结」，不是「像 AI 生成的报告」。
std::string render_rule_based_impl(const ReportFacts& f) {
    std::ostringstream os;

    const char* period = (f.kind == ReportKind::Daily)   ? "这天"
                         : (f.kind == ReportKind::Weekly) ? "这周"
                                                          : "这个月";

    // 1) 总体
    if (f.expense_records == 0 && f.income_records == 0) {
        os << f.range.from.to_string() << " 到 " << f.range.to.to_string()
           << " 没有任何记录。\n\n";
        os << "如果你这段时间确实没有消费，那这份报告是对的；"
              "但更常见的情况是忘了记。记录下来才有的分析。";
        return os.str();
    }

    os << period << "（" << f.range.from.to_string() << " 至 " << f.range.to.to_string()
       << "）一共" << f.expense_records << "笔支出，合计 " << yuan(f.expense_minor) << "。";
    if (f.days_with_records > 0) {
        os << "有记录的 " << f.days_with_records << " 天里，日均 "
           << yuan(f.avg_daily_expense_minor) << "。";
    }
    os << "\n\n";

    // 2) 结构
    if (!f.categories.empty()) {
        const size_t take = std::min<size_t>(3, f.categories.size());
        os << "花钱最多的是";
        for (size_t i = 0; i < take; ++i) {
            if (i > 0) os << (i + 1 == take ? "和" : "、");
            os << "「" << f.categories[i].category_name << "」（"
               << yuan(f.categories[i].total_minor) << "，占 "
               << percent(f.categories[i].share) << "）";
        }
        os << "。";

        if (f.categories.front().share >= 0.5) {
            os << "其中「" << f.categories.front().category_name
               << "」一个人就占了超过一半，你的支出结构比较集中。";
        }
        os << "\n\n";
    }

    // 3) 变化
    if (f.change_ratio.has_value()) {
        const double r = f.change_ratio.value();
        if (f.change_direction == "flat") {
            os << "和上一期相比基本持平。\n\n";
        } else {
            os << "和上一期（" << yuan(f.previous_expense_minor) << "）相比，"
               << (r > 0 ? "多花了 " : "少花了 ")
               << yuan(static_cast<int64_t>(
                      static_cast<double>(f.expense_minor) * std::abs(r)))
               << "，变化幅度 " << percent(std::abs(r)) << "。"
               << (r > 0 ? "注意这只是「比之前多」，不代表花得不合理。\n\n"
                         : "\n\n");
        }
    } else if (f.change_from_zero) {
        os << "上一期没有任何记录，所以算不出变化。这通常说明那段时间漏记了。\n\n";
    }

    // 4) 异常
    if (!f.anomalies.empty()) {
        os << "有 " << f.anomalies.size() << " 笔支出明显偏离你平时的水平，最大的一笔是 ";
        os << f.anomalies.front().date.to_string() << " 的「"
           << (f.anomalies.front().category_name.empty() ? "未分类"
                                                         : f.anomalies.front().category_name)
           << "」" << yuan(f.anomalies.front().amount_minor) << "。";
        os << "判定口径是" << (f.anomalies.front().method == "3sigma"
                                   ? "3σ（基于你自己的历史分布）"
                                   : "IQR（样本偏少时的降级方法）")
           << "，样本 " << f.anomaly_sample_size << " 笔。";
        os << "先确认这些笔有没有记错，再考虑要不要调整。\n\n";
    }

    // 5) 局限——这一节必须保留
    os << "数据局限：";
    if (!f.trend_sufficient) {
        os << "有记录的天数偏少，趋势和均值的代表性有限；";
    }
    os << "本报告只反映已记录的部分，漏记（尤其是小额高频消费）会让结构失真；"
          "同时没有你的收入目标和固定支出信息，所以不判断「花得多不多」。";

    return os.str();
}

}  // namespace

ReportService::ReportService(app::LedgerService& ledger,
                             llm::LlmConfig local_config,
                             llm::LlmConfig cloud_config)
    : ledger_(ledger),
      local_cfg_(std::move(local_config)),
      cloud_cfg_(std::move(cloud_config)) {
    // 构造时就判断一次可用性，省得每次生成都重复校验。
    // 注意：base_url 为空的配置 validate() 会失败，正好用来表示「这个通道没配」。
    local_enabled_ = local_cfg_.validate().is_ok();
    cloud_enabled_ = cloud_cfg_.validate().is_ok();
}

void ReportService::set_channels(std::optional<llm::LlmConfig> cloud,
                                std::optional<llm::LlmConfig> local) {
    std::lock_guard<std::mutex> guard(config_mutex_);

    if (cloud.has_value()) {
        cloud_cfg_ = std::move(cloud).value();
        cloud_enabled_ = cloud_cfg_.validate().is_ok();
    } else {
        cloud_enabled_ = false;
    }

    if (local.has_value()) {
        local_cfg_ = std::move(local).value();
        local_enabled_ = local_cfg_.validate().is_ok();
    } else {
        local_enabled_ = false;
    }
}

ReportService::ChannelConfigs ReportService::snapshot_channels() const {
    // 拷一份出来再建 provider：build_chain 里可能发网络请求（探测），
    // 握着一把会被其它请求等的锁去做几秒的 IO，是最容易写出的那种卡顿。
    std::lock_guard<std::mutex> guard(config_mutex_);
    ChannelConfigs out;
    if (cloud_enabled_) out.cloud = cloud_cfg_;
    if (local_enabled_) out.local = local_cfg_;
    return out;
}

// -----------------------------------------------------------------------------
//  统计
// -----------------------------------------------------------------------------

Result<Analysis> ReportService::analyze(const std::string& token,
                                       ReportKind kind,
                                       const Date& anchor_in) {
    const Date anchor = anchor_in.is_valid() ? anchor_in : Date::today();
    return analyze_range(token, range_for(kind, anchor), 7, kind);
}

Result<Analysis> ReportService::analyze_range(const std::string& token,
                                             const DateRange& range,
                                             int trend_window,
                                             ReportKind kind_for_facts) {
    // 取全量数据。个人记账的体量（几年也就几千笔）下，
    // 一次读全量比按区间多次查简单得多，也让趋势/异常的「上一期」计算不用二次取数。
    // 代价写在这里：如果哪天记录数到十万级，这个函数是第一个要改成区间下推的地方。
    auto snapshot = ledger_.snapshot(token);
    if (snapshot.is_err()) return snapshot.error();

    // 窗口边界保护：0 或负数会让滑动平均除零，过大则整条曲线被压平。
    const int window = (trend_window >= 2 && trend_window <= 60) ? trend_window : 7;

    Analysis a;

    auto dashboard = build_dashboard(snapshot.value().records, snapshot.value().categories, range);
    if (dashboard.is_err()) return dashboard.error();
    a.dashboard = std::move(dashboard).value();

    auto trend = compute_expense_trend(snapshot.value().records, range, window);
    if (trend.is_err()) return trend.error();
    a.trend = std::move(trend).value();

    auto anomalies = detect_anomalies(snapshot.value().records, snapshot.value().categories, range);
    if (anomalies.is_err()) return anomalies.error();
    a.anomalies = std::move(anomalies).value();

    a.insights = generate_insights(a.dashboard, a.trend, a.anomalies);
    a.facts = build_facts(kind_for_facts, a.dashboard, a.trend, a.anomalies, a.insights);

    return Result<Analysis>(std::move(a));
}

// -----------------------------------------------------------------------------
//  模型通道
// -----------------------------------------------------------------------------

std::vector<std::unique_ptr<llm::ILLMProvider>> ReportService::build_chain(
    std::vector<std::string>* skip_notes) {
    std::vector<std::unique_ptr<llm::ILLMProvider>> chain;

    auto note = [skip_notes](const std::string& msg) {
        if (skip_notes != nullptr) skip_notes->push_back(msg);
        Logger::default_logger().info("模型通道: " + msg);
    };

    // 顺序 = 优先级。云端优先，本地兜底。
    //
    // 为什么是云端优先：本地要跑一个 GGUF，占显存——而这台机器只有一张
    // 8GB 的 4060，还在跑训练。让「生成一份日报」去抢显存不划算。
    // 而且本地小模型的归纳质量确实不如云端，报告是给人读的东西，质量更值钱。
    //
    // 反转这条只改链的顺序和 prompt 档位，不动任何接口——这正是
    // FailoverProvider 抽象该有的好处：策略是上层的事。

    const ChannelConfigs cfgs = snapshot_channels();

    // 第一优先：云端
    if (cfgs.cloud.has_value()) {
        auto provider = llm::make_openai_compatible(cfgs.cloud.value());
        if (provider == nullptr) {
            note("云端通道创建失败（base_url=" + cfgs.cloud->base_url + "）");
        } else {
            chain.push_back(std::move(provider));
        }
    } else {
        note("云端通道不可用（未配置 API Key 或配置不合法）");
    }

    // 第二优先：本地（云端失败/不可用时才轮到它）
    if (cfgs.local.has_value()) {
        auto provider = llm::make_openai_compatible(cfgs.local.value());
        if (provider == nullptr) {
            note("本地通道创建失败（base_url=" + cfgs.local->base_url + "）");
        } else {
            chain.push_back(std::move(provider));
        }
    } else {
        note("本地通道不可用（未登记模型，或 llama-server 未启动）");
    }

    return chain;
}

ReportService::ChannelStatus ReportService::probe_channels() {
    ChannelStatus s;
    const ChannelConfigs cfgs = snapshot_channels();

    if (cfgs.local.has_value()) {
        s.local_configured = true;
        s.local_label = cfgs.local->label();
        auto p = llm::make_openai_compatible(cfgs.local.value());
        if (p != nullptr) s.local_reachable = p->probe().is_ok();
    } else {
        // 没配也要给个标签，界面才知道「本地：未配置」而不是空白
        s.local_label = "未配置（未登记模型或服务未启动）";
    }

    if (cfgs.cloud.has_value()) {
        s.cloud_configured = true;
        s.cloud_label = cfgs.cloud->label();
        auto p = llm::make_openai_compatible(cfgs.cloud.value());
        if (p != nullptr) s.cloud_reachable = p->probe().is_ok();
    } else {
        s.cloud_label = "未配置（缺少 API Key）";
    }

    return s;
}

// -----------------------------------------------------------------------------
//  生成
// -----------------------------------------------------------------------------

Result<ReportResult> ReportService::generate(const std::string& token,
                                            const GenerateOptions& options) {
    Logger& log = Logger::default_logger();

    const Date anchor = options.anchor.is_valid() ? options.anchor : Date::today();

    // ---- 1) 存档命中？ ----
    if (options.use_cache && !options.force_regenerate) {
        auto cached = ledger_.find_report(token, anchor, storage_id(options.kind));
        if (cached.is_ok() && cached.value().has_value()) {
            const auto& stored = cached.value().value();
            // 模板生成的报告不值得当成终态缓存：模型一旦可用就该重生成。
            // 只有模型产出的才复用。
            if (stored.provider != llm::storage_id(llm::ProviderKind::RuleBased)) {
                auto analysis = analyze(token, options.kind, anchor);
                if (analysis.is_err()) return analysis.error();

                ReportResult r;
                r.content = stored.content;
                r.provider = stored.provider;
                r.model = stored.model;
                r.from_cache = true;
                r.stale_model_output = true;
                r.facts = std::move(analysis.value().facts);
                log.info("复用已有报告存档: " + anchor.to_string() + " / " +
                         storage_id(options.kind));
                return Result<ReportResult>(std::move(r));
            }
        }
    }

    // ---- 2) 统计 ----
    auto analysis_res = analyze(token, options.kind, anchor);
    if (analysis_res.is_err()) return analysis_res.error();
    Analysis analysis = std::move(analysis_res).value();

    // ---- 3) 建降级链 ----
    std::vector<std::string> chain_notes;
    auto chain = build_chain(&chain_notes);

    ReportResult result;
    result.facts = analysis.facts;
    result.failover_notes = chain_notes;

    // ---- 4) 无可用通道 -> 直接模板 ----
    if (chain.empty()) {
        result.content = render_rule_based(analysis.facts);
        result.provider = llm::storage_id(llm::ProviderKind::RuleBased);
        result.model.clear();
        result.provider_kind = llm::ProviderKind::RuleBased;
        result.used_fallback = true;
        result.failover_notes.push_back(
            "没有可用的模型通道，已用规则模板生成（数字全部来自本地统计，准确；"
            "文字是模板套出来的，不是模型写的）。");

        storage::StoredReport stored;
        stored.date = anchor;
        stored.kind = storage_id(options.kind);
        stored.provider = result.provider;
        stored.model = result.model;
        stored.content = result.content;
        stored.stats_json = facts_to_json(result.facts);
        auto saved = ledger_.save_report(token, stored);
        if (saved.is_err()) {
            log.warn("报告存档失败（不影响本次返回）: " + saved.error().to_string());
        }
        return Result<ReportResult>(std::move(result));
    }

    // ---- 5) 调模型 ----
    // prompt 档位跟着「链头是谁」走，而不是拍一个默认值：
    //   链头是云端 -> Detailed（云端上下文吃得下，信息给全，报告更有料）
    //   链头是本地 -> Compact（本地可能是几百 M 的小模型，喂长 prompt 反而更差）
    // 显式指定 style 时以调用方为准。
    PromptStyle chosen_style = PromptStyle::Detailed;
    if (options.style.has_value()) {
        chosen_style = options.style.value();
    } else if (!chain.empty() &&
               chain.front()->kind() == llm::ProviderKind::LocalLlama) {
        chosen_style = PromptStyle::Compact;
    }

    const BuiltPrompt prompt = build_prompt(analysis.facts, chosen_style);
    result.style_used = chosen_style;
    result.provider_kind = chain.front()->kind();
    result.provider = llm::storage_id(chain.front()->kind());

    log.info("生成报告: " + anchor.to_string() + " / " + storage_id(options.kind) +
             "，prompt 档位=" + to_string(chosen_style) + "（system " +
             std::to_string(prompt.system_chars) + " 字符，user " +
             std::to_string(prompt.user_chars) + " 字符），通道数=" +
             std::to_string(chain.size()));

    llm::FailoverProvider failover(std::move(chain));

    llm::LlmRequest request;
    // 显式把 image_data_url 置空：聚合初始化漏写字段时 GCC 会警告
    // （-Wmissing-field-initializers）。报告这条通道永远不带图，
    // 写出来比让读者去数字段个数可靠。
    request.messages.push_back(llm::LlmMessage{"system", prompt.system, {}});
    request.messages.push_back(llm::LlmMessage{"user", prompt.user, {}});

    const int64_t started = now_millis();
    auto response = failover.complete(request);
    const int64_t elapsed = now_millis() - started;

    // 把每个通道的尝试结果都记下来，便于界面解释「为什么走了云端 / 为什么是模板」
    for (const auto& attempt : failover.last_attempts()) {
        if (attempt.succeeded) {
            result.failover_notes.push_back(
                attempt.label + "：成功（" + std::to_string(attempt.latency_ms) + "ms）");
        } else {
            result.failover_notes.push_back(attempt.label + "：失败 —— " + attempt.error);
        }
    }

    if (response.is_ok()) {
        result.content = response.value().text;
        result.model = response.value().model;
        result.provider_kind = response.value().provider;
        result.provider = llm::storage_id(response.value().provider);
        result.latency_ms = response.value().latency_ms;
        result.used_fallback = false;

        log.info("报告生成成功: provider=" + result.provider + "，耗时 " +
                 std::to_string(result.latency_ms) + "ms，输出 " +
                 std::to_string(result.content.size()) + " 字符");
    } else {
        // ---- 6) 全失败 -> 模板兜底 ----
        result.content = render_rule_based(analysis.facts);
        result.provider = llm::storage_id(llm::ProviderKind::RuleBased);
        result.provider_kind = llm::ProviderKind::RuleBased;
        result.model.clear();
        result.used_fallback = true;
        result.latency_ms = elapsed;
        result.failover_notes.push_back(
            "所有模型通道都失败了，已改用规则模板生成。上面的数字都是本地统计出来的、"
            "准确的；文字是模板套的，不是模型写的。失败原因见上。");
        log.warn("所有模型通道失败，使用模板兜底: " + response.error().to_string());
    }

    // ---- 7) 存档 ----
    {
        storage::StoredReport stored;
        stored.date = anchor;
        stored.kind = storage_id(options.kind);
        stored.provider = result.provider;
        stored.model = result.model;
        stored.content = result.content;
        stored.stats_json = facts_to_json(result.facts);
        auto saved = ledger_.save_report(token, stored);
        if (saved.is_err()) {
            log.warn("报告存档失败（不影响本次返回）: " + saved.error().to_string());
        }
    }

    return Result<ReportResult>(std::move(result));
}

std::string ReportService::render_rule_based(const ReportFacts& facts) {
    return render_rule_based_impl(facts);
}

}  // namespace penhu::report
