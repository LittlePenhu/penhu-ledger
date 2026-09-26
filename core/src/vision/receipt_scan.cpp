// =============================================================================
//  core/src/vision/receipt_scan.cpp
// =============================================================================

#include "penhu/vision/receipt_scan.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>

#include "penhu/domain/money.hpp"

namespace penhu::vision {

using json = nlohmann::json;

namespace {

/// 提示词。要点：
///  · 明确「只输出 JSON」并给出字段表 —— 不写清楚，模型会回一段 nicely formatted 的说明
///  · 把候选分类原样列出来，并要求照抄 —— 否则同一个商户每次识别可能给出不同的分类名
///  · 显式允许「看不清就留空」 —— 不给这条，模型会倾向于编一个看起来合理的数字
///  · 要求 confidence，让我们能对低把握的结果给出更明显的提示
std::string build_prompt(const ScanRequest& req) {
    std::string cats;
    for (size_t i = 0; i < req.category_candidates.size(); ++i) {
        if (i > 0) cats += "、";
        cats += req.category_candidates[i];
    }
    if (cats.empty()) cats = "（无候选，请自行给出一个简短分类名）";

    std::string p;
    p += "你在帮我记账。下面这张图是一张支付或账单截图。\n";
    p += "只输出一个 JSON 对象，不要任何解释、不要 markdown 代码块、不要前后缀文字。\n\n";
    p += "JSON 字段：\n";
    p += "  amount     字符串。金额数字，不带货币符号，最多两位小数。看不清就填空字符串。\n";
    p += "  currency   字符串，如 \"CNY\"。默认 CNY。\n";
    p += "  direction  \"expense\" 或 \"income\"。付款/消费填 expense，收款/退款填 income。\n";
    p += "  merchant   字符串。商户或收款方名称。看不清填空字符串。\n";
    p += "  datetime   字符串。形如 \"2026-09-20 12:30\"；只有日期就给 \"2026-09-20\"。看不清填空字符串。\n";
    p += "  category   字符串。必须从下面的候选里原样选一个，都不合适就填最后一个。\n";
    p += "  note       字符串。不超过 20 字的简短说明。\n";
    p += "  confidence 数字，0 到 1，表示你对 amount 这一项的把握有多大。\n\n";
    p += "可选的 category 列表（务必原样照抄其中一项）：\n  " + cats + "\n\n";
    p += "硬性规则：\n";
    p += "1. 绝对不要猜金额。看不清就把 amount 留成空字符串，我会自己处理。\n";
    p += "2. 不要输出 JSON 以外的任何内容。\n";
    if (!req.today_iso.empty()) {
        p += "3. 图上没有日期或看不清时，datetime 留空（我会用 " + req.today_iso + "）。\n";
    }
    return p;
}

/// 从回复里抠出第一个完整的 JSON 对象。
/// 模型哪怕被要求「只输出 JSON」，也经常包一层 ```json ... ``` 或加一句「好的，这是结果：」。
/// 所以不直接 parse 整段回复，而是取第一个 '{' 到最后一个 '}'。
std::optional<std::string> extract_json_object(const std::string& text) {
    const auto start = text.find('{');
    const auto end = text.rfind('}');
    if (start == std::string::npos || end == std::string::npos || end <= start) return std::nullopt;
    return text.substr(start, end - start + 1);
}

std::string lower_ascii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

/// 把模型给的分类名对齐到候选表。
/// 先精确匹配，再双向包含匹配（模型可能输出「餐饮美食」或只输出「餐」）。
/// 对不上就退回候选表的最后一项 —— 约定里最后一项是「其他」。
std::string match_category(const std::string& raw, const std::vector<std::string>& cands) {
    if (cands.empty()) return raw;
    if (raw.empty()) return cands.back();

    for (const auto& c : cands) {
        if (c == raw) return c;
    }
    const std::string lraw = lower_ascii(raw);
    for (const auto& c : cands) {
        const std::string lc = lower_ascii(c);
        if (lraw.find(lc) != std::string::npos || lc.find(lraw) != std::string::npos) return c;
    }
    return cands.back();
}

/// "2026-09-20 12:30" -> "2026-09-20"；只保留形如 YYYY-MM-DD 的前缀
std::string normalize_date(const std::string& raw) {
    if (raw.size() < 10) return {};
    const std::string head = raw.substr(0, 10);
    // 校验形状：数字数字数字数字-数字数字-数字数字
    for (size_t i = 0; i < 10; ++i) {
        if (i == 4 || i == 7) {
            if (head[i] != '-' && head[i] != '/') return {};
        } else if (head[i] < '0' || head[i] > '9') {
            return {};
        }
    }
    std::string out = head;
    for (char& c : out) {
        if (c == '/') c = '-';
    }
    return out;
}

std::string get_string(const json& j, const char* key) {
    if (!j.contains(key)) return {};
    const auto& v = j[key];
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number()) return std::to_string(v.get<double>());
    return {};
}

}  // namespace

ScanOutcome parse_scan_reply(const std::string& reply,
                             const std::vector<std::string>& category_candidates) {
    ScanOutcome out;
    out.raw_reply = reply;

    const auto body = extract_json_object(reply);
    if (!body.has_value()) {
        out.fail_reason = "模型回复里找不到 JSON 对象（可能被截断或回答了别的内容）";
        return out;
    }

    json j;
    try {
        j = json::parse(*body);
    } catch (const std::exception& e) {
        out.fail_reason = std::string("模型回复的 JSON 解析失败：") + e.what();
        return out;
    }
    if (!j.is_object()) {
        out.fail_reason = "模型回复的 JSON 不是对象";
        return out;
    }

    // ---- 金额（最关键的一项，单独处理失败路径）----
    const std::string amount_text = get_string(j, "amount");
    if (amount_text.empty()) {
        out.fail_reason = "模型没能从图里读出金额（截图可能太模糊，或图里根本没有金额）";
        return out;
    }
    auto money = Money::parse(amount_text);
    if (money.is_err()) {
        out.fail_reason = "模型给出的金额无法解析：" + amount_text;
        return out;
    }
    if (money.value().is_zero()) {
        out.fail_reason = "识别到的金额是 0，这多半是识别错了，请手填";
        return out;
    }
    out.amount_minor = money.value().abs().minor_units();   // 方向单独用 direction 表达

    // ---- 方向 ----
    const std::string dir = lower_ascii(get_string(j, "direction"));
    out.direction = (dir == "income" || dir == "in" || dir == "credit")
                        ? Direction::Income
                        : Direction::Expense;

    // ---- 分类 ----
    out.category_name = match_category(get_string(j, "category"), category_candidates);

    // ---- 其余字段 ----
    out.merchant = get_string(j, "merchant");
    if (out.merchant.size() > 40) out.merchant.resize(40);
    out.note = get_string(j, "note");
    if (out.note.size() > 40) out.note.resize(40);
    out.date_iso = normalize_date(get_string(j, "datetime"));

    if (j.contains("confidence") && j["confidence"].is_number()) {
        out.confidence = std::clamp(j["confidence"].get<double>(), 0.0, 1.0);
    }

    out.ok = true;
    return out;
}

Result<ScanOutcome> scan_with_vision(const ScanRequest& req, const llm::LlmConfig& cfg) {
    using R = Result<ScanOutcome>;

    if (req.image_data_url.empty()) {
        return R::fail(ErrorCode::InvalidArgument, "没有可用的图片数据", "scan_with_vision");
    }
    if (auto v = cfg.validate(); v.is_err()) {
        return R::fail(v.error().code, "模型通道配置不可用：" + v.error().message, "scan_with_vision");
    }

    llm::LlmRequest request;
    llm::LlmMessage msg;
    msg.role = "user";
    msg.content = build_prompt(req);
    msg.image_data_url = req.image_data_url;
    request.messages.push_back(std::move(msg));

    auto provider = llm::make_openai_compatible(cfg);
    if (provider == nullptr) {
        return R::fail(ErrorCode::ConfigError, "无法创建模型客户端", "scan_with_vision");
    }

    auto reply = provider->complete(request);
    if (reply.is_err()) {
        return R::fail(reply.error().code, reply.error().message, "scan_with_vision");
    }

    ScanOutcome out = parse_scan_reply(reply.value().text, req.category_candidates);
    out.provider_label = reply.value().provider_label.empty()
                             ? cfg.label()
                             : reply.value().provider_label;
    if (!out.ok) {
        // 识别失败不是「异常」，而是「这次没读出来」。原因要原样带给界面，
        // 但不要把它变成 Result 的 error —— 调用方需要拿到 raw_reply 才能排障。
        out.notes.push_back("模型原始回复长度 " + std::to_string(reply.value().text.size()) + " 字节");
    }
    return R::ok(std::move(out));
}

}  // namespace penhu::vision
