#include "api_support.hpp"

#include <algorithm>
#include <cstdlib>
#include <unordered_map>

namespace penhu::server::detail {

// -----------------------------------------------------------------------------
//  响应
// -----------------------------------------------------------------------------

int http_status_for(ErrorCode code) {
    switch (code) {
        case ErrorCode::Ok:               return 200;
        case ErrorCode::InvalidArgument:  return 400;
        case ErrorCode::AuthFailed:       return 401;
        case ErrorCode::PermissionDenied: return 403;
        case ErrorCode::NotFound:         return 404;
        case ErrorCode::AlreadyExists:    return 409;
        case ErrorCode::ConfigError:      return 400;
        // 加密/存储/网络类都属于「服务端出了问题」，用 5xx，
        // 前端据此区分「我请求错了」和「它自己坏了」。
        case ErrorCode::CryptoFailure:    return 500;
        case ErrorCode::StorageFailure:   return 500;
        case ErrorCode::NetworkFailure:   return 502;
        case ErrorCode::LlmFailure:       return 502;
        case ErrorCode::Internal:         return 500;
    }
    return 500;
}

json make_error_json(const Error& err) {
    json j;
    j["ok"] = false;
    j["error"] = {
        {"code", to_string(err.code)},
        {"message", err.message},
    };
    // context 只在调试时有意义，但本机应用，带上更方便排查
    if (!err.context.empty()) {
        j["error"]["context"] = err.context;
    }
    return j;
}

json make_error_json(ErrorCode code, const std::string& message) {
    return make_error_json(Error{code, message, {}});
}

void send_ok(httplib::Response& res, const json& data, int status) {
    json body;
    body["ok"] = true;
    body["data"] = data;
    res.status = status;
    res.set_content(body.dump(), "application/json; charset=utf-8");
}

void send_error(httplib::Response& res, const Error& err) {
    res.status = http_status_for(err.code);
    res.set_content(make_error_json(err).dump(), "application/json; charset=utf-8");
}

void send_error(httplib::Response& res, ErrorCode code, const std::string& message,
                const std::string& context) {
    send_error(res, Error{code, message, context});
}

// -----------------------------------------------------------------------------
//  请求
// -----------------------------------------------------------------------------

Result<json> parse_json_body(const httplib::Request& req) {
    if (req.body.empty()) {
        return Result<json>::fail(ErrorCode::InvalidArgument, "请求体为空（需要 JSON）",
                                  "parse_json_body");
    }
    if (req.body.size() > 4 * 1024 * 1024) {
        return Result<json>::fail(ErrorCode::InvalidArgument, "请求体过大（上限 4 MB）",
                                  "parse_json_body");
    }
    try {
        return Result<json>(json::parse(req.body));
    } catch (const json::parse_error& e) {
        return Result<json>::fail(ErrorCode::InvalidArgument,
                                  std::string("请求体不是合法 JSON: ") + e.what(),
                                  "parse_json_body");
    }
}

std::string bearer_token(const httplib::Request& req) {
    const std::string header = req.get_header_value("Authorization");
    const std::string prefix = "Bearer ";
    if (header.size() <= prefix.size()) return {};
    if (header.compare(0, prefix.size(), prefix) != 0) return {};
    return header.substr(prefix.size());
}

int query_int(const httplib::Request& req, const char* name, int fallback) {
    if (!req.has_param(name)) return fallback;
    const std::string v = req.get_param_value(name);
    if (v.empty()) return fallback;
    try {
        return std::stoi(v);
    } catch (...) {
        return fallback;
    }
}

int64_t query_int64(const httplib::Request& req, const char* name, int64_t fallback) {
    if (!req.has_param(name)) return fallback;
    const std::string v = req.get_param_value(name);
    if (v.empty()) return fallback;
    try {
        return std::stoll(v);
    } catch (...) {
        return fallback;
    }
}

std::string query_str(const httplib::Request& req, const char* name,
                      const std::string& fallback) {
    if (!req.has_param(name)) return fallback;
    return req.get_param_value(name);
}

bool query_has(const httplib::Request& req, const char* name) {
    return req.has_param(name);
}

bool query_bool(const httplib::Request& req, const char* name, bool fallback) {
    if (!req.has_param(name)) return fallback;
    const std::string v = req.get_param_value(name);
    if (v == "1" || v == "true" || v == "yes") return true;
    if (v == "0" || v == "false" || v == "no") return false;
    return fallback;
}

Result<std::string> require_str(const json& body, const char* field) {
    if (!body.contains(field) || !body[field].is_string()) {
        return Result<std::string>::fail(ErrorCode::InvalidArgument,
                                        std::string("缺少字符串字段 '") + field + "'",
                                        "require_str");
    }
    return Result<std::string>(body[field].get<std::string>());
}

Result<int64_t> require_int(const json& body, const char* field) {
    if (!body.contains(field)) {
        return Result<int64_t>::fail(ErrorCode::InvalidArgument,
                                     std::string("缺少字段 '") + field + "'", "require_int");
    }
    // 前端可能把金额以字符串形式传来（"12.34"），这里也接受
    if (body[field].is_string()) {
        try {
            return Result<int64_t>(std::stoll(body[field].get<std::string>()));
        } catch (...) {
            return Result<int64_t>::fail(
                ErrorCode::InvalidArgument,
                std::string("字段 '") + field + "' 不是合法的整数", "require_int");
        }
    }
    if (!body[field].is_number_integer()) {
        return Result<int64_t>::fail(ErrorCode::InvalidArgument,
                                     std::string("字段 '") + field + "' 不是整数", "require_int");
    }
    return Result<int64_t>(body[field].get<int64_t>());
}

Result<double> require_number(const json& body, const char* field) {
    if (!body.contains(field)) {
        return Result<double>::fail(ErrorCode::InvalidArgument,
                                    std::string("缺少字段 '") + field + "'", "require_number");
    }
    if (body[field].is_string()) {
        try {
            return Result<double>(std::stod(body[field].get<std::string>()));
        } catch (...) {
            return Result<double>::fail(
                ErrorCode::InvalidArgument,
                std::string("字段 '") + field + "' 不是合法数字", "require_number");
        }
    }
    if (!body[field].is_number()) {
        return Result<double>::fail(ErrorCode::InvalidArgument,
                                    std::string("字段 '") + field + "' 不是数字",
                                    "require_number");
    }
    return Result<double>(body[field].get<double>());
}

std::string opt_str(const json& body, const char* field, const std::string& fallback) {
    if (!body.contains(field) || !body[field].is_string()) return fallback;
    return body[field].get<std::string>();
}

// -----------------------------------------------------------------------------
//  序列化
// -----------------------------------------------------------------------------

json category_to_json(const Category& c) {
    return json{
        {"id", c.id},
        {"name", c.name},
        {"icon", c.icon},
        {"color", c.color_hex},
        {"direction", to_string(c.direction)},
        {"directionLabel", direction_label_zh(c.direction)},
        {"sortOrder", c.sort_order},
        {"isBuiltin", c.is_builtin},
        {"isActive", c.is_active},
    };
}

json record_to_json(const Record& r, const std::string& category_name,
                    const std::string& category_color) {
    return json{
        {"id", r.id},
        {"amountMinor", r.amount.minor_units()},
        {"amount", r.amount.to_plain_string()},
        {"amountDisplay", r.amount.to_display_string()},
        {"direction", to_string(r.direction)},
        {"directionLabel", direction_label_zh(r.direction)},
        {"categoryId", r.category_id},
        {"categoryName", category_name},
        {"categoryColor", category_color},
        {"date", r.date.to_string()},
        {"weekday", r.date.weekday_zh()},
        {"note", r.note},
        {"createdAt", r.created_at},
        {"updatedAt", r.updated_at},
    };
}

json daily_point_to_json(const analytics::DailyPoint& p) {
    return json{
        {"date", p.date.to_string()},
        {"weekday", p.date.weekday_zh()},
        {"expenseMinor", p.expense_minor},
        {"incomeMinor", p.income_minor},
        {"recordCount", p.record_count},
        {"hasRecords", p.has_records},
    };
}

json breakdown_to_json(const CategoryBreakdown& b) {
    return json{
        {"categoryId", b.category_id},
        {"name", b.category_name},
        {"color", b.category_color},
        {"direction", to_string(b.direction)},
        {"totalMinor", b.total_minor},
        {"recordCount", b.record_count},
        {"share", b.share},
    };
}

json summary_to_json(const analytics::Dashboard& d) {
    const Summary& s = d.summary;
    json j;
    j["range"] = {{"from", d.range.from.to_string()}, {"to", d.range.to.to_string()}};
    j["expenseMinor"] = s.expense_minor;
    j["incomeMinor"] = s.income_minor;
    j["netMinor"] = s.net_minor;
    j["expenseRecords"] = s.expense_records;
    j["incomeRecords"] = s.income_records;
    j["activeDays"] = s.active_days;
    j["avgDailyExpenseMinor"] = s.avg_daily_expense_minor.minor_units();
    j["maxExpenseMinor"] = s.max_expense_minor.minor_units();
    j["maxExpenseDate"] = s.max_expense_date;
    j["daysInRange"] = d.days_in_range;
    j["daysWithRecords"] = d.days_with_records;

    j["byCategory"] = json::array();
    for (const auto& b : s.by_category) {
        j["byCategory"].push_back(breakdown_to_json(b));
    }

    j["daily"] = json::array();
    for (const auto& p : d.daily) {
        j["daily"].push_back(daily_point_to_json(p));
    }
    return j;
}

json trend_to_json(const analytics::TrendResult& t) {
    json j;
    j["window"] = t.window;
    j["sufficientData"] = t.sufficient_data;
    j["daysWithRecords"] = t.days_with_records;
    j["note"] = t.note;
    j["comparedToPrevious"] = {
        {"currentMinor", t.compared_to_previous.current_minor},
        {"previousMinor", t.compared_to_previous.previous_minor},
        {"deltaMinor", t.compared_to_previous.delta_minor},
        {"direction", t.compared_to_previous.direction},
        {"fromZero", t.compared_to_previous.from_zero},
    };
    if (t.compared_to_previous.change_ratio.has_value()) {
        j["comparedToPrevious"]["changeRatio"] = t.compared_to_previous.change_ratio.value();
    } else {
        j["comparedToPrevious"]["changeRatio"] = nullptr;
    }

    j["points"] = json::array();
    for (const auto& p : t.points) {
        j["points"].push_back({
            {"date", p.date.to_string()},
            {"valueMinor", p.value_minor},
            {"movingAverageMinor", p.moving_average_minor},
            {"windowUsed", p.window_used},
        });
    }
    return j;
}

json anomaly_to_json(const analytics::Anomaly& a) {
    return json{
        {"kind", analytics::to_string(a.kind)},
        {"date", a.date.to_string()},
        {"categoryId", a.category_id},
        {"categoryName", a.category_name},
        {"recordId", a.record_id},
        {"amountMinor", a.amount_minor},
        {"thresholdMinor", a.threshold_minor},
        {"score", a.score},
        {"method", a.method},
        {"explanation", a.explanation},
    };
}

json anomalies_to_json(const analytics::AnomalyReport& r) {
    json j;
    j["sampleSize"] = r.sample_size;
    j["scannedDays"] = r.scanned_days;
    j["reliable"] = r.reliable;
    j["methodSummary"] = r.method_summary;
    j["limitations"] = r.limitations;
    j["suppressedCount"] = r.suppressed_count;
    j["items"] = json::array();
    for (const auto& a : r.items) {
        j["items"].push_back(anomaly_to_json(a));
    }
    return j;
}

json insight_to_json(const analytics::Insight& i) {
    return json{
        {"ruleId", i.rule_id},
        {"severity", analytics::to_string(i.severity)},
        {"title", i.title},
        {"detail", i.detail},
        {"relatedMinor", i.related_minor},
        {"categoryId", i.category_id},
        {"categoryName", i.category_name},
    };
}

json insights_to_json(const analytics::InsightSet& s) {
    json j;
    j["actionable"] = s.actionable;
    j["dataNote"] = s.data_note;
    j["limitations"] = s.limitations;
    j["items"] = json::array();
    for (const auto& i : s.items) {
        j["items"].push_back(insight_to_json(i));
    }
    return j;
}

json report_facts_json(const report::ReportFacts& f) {
    json j;
    j["kind"] = report::storage_id(f.kind);
    j["range"] = {{"from", f.range.from.to_string()}, {"to", f.range.to.to_string()}};
    j["expenseMinor"] = f.expense_minor;
    j["incomeMinor"] = f.income_minor;
    j["netMinor"] = f.net_minor;
    j["expenseRecords"] = f.expense_records;
    j["daysInRange"] = f.days_in_range;
    j["daysWithRecords"] = f.days_with_records;
    j["avgDailyExpenseMinor"] = f.avg_daily_expense_minor;
    j["changeDirection"] = f.change_direction;
    j["categoryNames"] = json::array();
    for (const auto& c : f.categories) {
        j["categoryNames"].push_back({{"name", c.category_name}, {"totalMinor", c.total_minor}});
    }
    return j;
}

json report_result_to_json(const report::ReportResult& r) {
    json j;
    j["content"] = r.content;
    j["provider"] = r.provider;
    j["model"] = r.model;
    j["fromCache"] = r.from_cache;
    j["usedFallback"] = r.used_fallback;
    j["staleModelOutput"] = r.stale_model_output;
    j["style"] = report::to_string(r.style_used);
    j["latencyMs"] = r.latency_ms;
    j["failoverNotes"] = r.failover_notes;
    j["facts"] = report_facts_json(r.facts);
    return j;
}

json stored_report_to_json(const storage::StoredReport& r) {
    return json{
        {"id", r.id},
        {"date", r.date.to_string()},
        {"kind", r.kind},
        {"provider", r.provider},
        {"model", r.model},
        {"content", r.content},
        {"createdAt", r.created_at},
    };
}

CategoryLookup::CategoryLookup(const std::vector<Category>& categories) {
    index_.reserve(categories.size());
    for (const auto& c : categories) {
        index_.emplace_back(c.id, &c);
    }
    std::sort(index_.begin(), index_.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
}

std::string CategoryLookup::name_of(const std::string& id) const {
    auto it = std::lower_bound(index_.begin(), index_.end(), id,
                               [](const auto& pair, const std::string& key) {
                                   return pair.first < key;
                               });
    if (it != index_.end() && it->first == id) return it->second->name;
    return "（未知分类）";
}

std::string CategoryLookup::color_of(const std::string& id) const {
    auto it = std::lower_bound(index_.begin(), index_.end(), id,
                               [](const auto& pair, const std::string& key) {
                                   return pair.first < key;
                               });
    if (it != index_.end() && it->first == id) return it->second->color_hex;
    return "#78909C";
}

void build_lookup(const std::vector<Category>& categories, std::vector<const Category*>& out) {
    out.clear();
    out.reserve(categories.size());
    for (const auto& c : categories) out.push_back(&c);
}

json records_page_to_json(const PagedRecords& page, const std::vector<Category>& categories) {
    // 先建索引再渲染，避免每条记录线性扫一遍分类表（N+1）
    std::unordered_map<std::string, const Category*> index;
    index.reserve(categories.size());
    for (const auto& c : categories) index.emplace(c.id, &c);

    json items = json::array();
    for (const auto& r : page.items) {
        const auto it = index.find(r.category_id);
        const std::string name = (it != index.end()) ? it->second->name : "（未知分类）";
        const std::string color = (it != index.end()) ? it->second->color_hex : "#78909C";
        items.push_back(record_to_json(r, name, color));
    }

    return json{
        {"total", page.total},
        {"count", page.items.size()},
        {"items", items},
    };
}

// -----------------------------------------------------------------------------
//  守卫
// -----------------------------------------------------------------------------

namespace {

bool is_loopback_hostname(const std::string& host) {
    if (host == "localhost") return true;
    if (host == "127.0.0.1") return true;
    if (host == "[::1]" || host == "::1") return true;
    // 整个 127/8 都是回环
    if (host.rfind("127.", 0) == 0) return true;
    return false;
}

/// 从 "127.0.0.1:8080" / "localhost" / "[::1]:80" 里取出主机名
std::string strip_port(const std::string& host_header) {
    if (host_header.empty()) return {};
    if (host_header.front() == '[') {
        const size_t close = host_header.find(']');
        if (close == std::string::npos) return host_header;
        return host_header.substr(0, close + 1);
    }
    const size_t colon = host_header.rfind(':');
    if (colon == std::string::npos) return host_header;
    return host_header.substr(0, colon);
}

}  // namespace

bool host_header_is_loopback(const httplib::Request& req) {
    const std::string host_header = req.get_header_value("Host");
    if (host_header.empty()) {
        // HTTP/1.0 或某些客户端可能不带 Host。本机应用里这种情况少见，
        // 但不能因此放行——放行等于把 rebinding 的防线拆了。
        return false;
    }
    return is_loopback_hostname(strip_port(host_header));
}

bool preflight_check(const httplib::Request& req, httplib::Response& res, bool needs_body) {
    if (!host_header_is_loopback(req)) {
        send_error(res, ErrorCode::PermissionDenied,
                   "拒绝服务非回环 Host 的请求（防 DNS rebinding）。"
                   "请通过 http://127.0.0.1:<端口> 访问。",
                   "Host: " + req.get_header_value("Host"));
        return false;
    }
    if (needs_body) {
        const std::string ctype = req.get_header_value("Content-Type");
        if (ctype.find("application/json") == std::string::npos) {
            send_error(res, ErrorCode::InvalidArgument,
                       "请求需要 Content-Type: application/json");
            return false;
        }
    }
    return true;
}

}  // namespace penhu::server::detail
