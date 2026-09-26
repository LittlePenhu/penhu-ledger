#pragma once
// =============================================================================
//  penhu/server/api_support.hpp（内部头，不对外暴露）
//  HTTP 层的公共部分：错误码映射、JSON 序列化、鉴权提取。
//
//  这里有两个刻意的决定：
//
//  1) 一律不加 CORS 响应头。
//     API 只服务本机窗口，不需要跨源访问。不加 Access-Control-Allow-Origin
//     意味着浏览器会拦下一切跨源读取；再配合 Host 头校验挡住 DNS rebinding，
//     一个恶意网页就没法诱导浏览器去读写你的账本。
//
//  2) 统一响应外壳 {ok, data} / {ok, error:{code,message}}。
//     前端只需要判 ok 字段，错误提示直接拿 message；错误码用字符串而不是数字，
//     是因为数字码表在后端一改前端就全错，字符串则能自己说话。
// =============================================================================

#include <string>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "penhu/analytics/anomaly.hpp"
#include "penhu/analytics/insight.hpp"
#include "penhu/analytics/summary.hpp"
#include "penhu/analytics/trend.hpp"
#include "penhu/app/ledger_service.hpp"
#include "penhu/deploy/local_model.hpp"
#include "penhu/domain/category.hpp"
#include "penhu/domain/record.hpp"
#include "penhu/llm/provider.hpp"
#include "penhu/report/prompt_builder.hpp"
#include "penhu/report/report_service.hpp"
#include "penhu/util/result.hpp"

namespace penhu::server::detail {

using json = nlohmann::json;

/// 各路由注册函数的共享上下文
struct RouteContext {
    app::LedgerService&         ledger;
    report::ReportService&      reports;
    deploy::LocalModelManager&  local_models;
};

// -----------------------------------------------------------------------------
//  响应
// -----------------------------------------------------------------------------

/// ErrorCode -> HTTP 状态码
int http_status_for(ErrorCode code);

json make_error_json(const Error& err);
json make_error_json(ErrorCode code, const std::string& message);

/// 统一成功响应
void send_ok(httplib::Response& res, const json& data, int status = 200);
/// 统一错误响应
void send_error(httplib::Response& res, const Error& err);
void send_error(httplib::Response& res, ErrorCode code, const std::string& message,
                const std::string& context = {});

/// 把 Result 直接变成响应；成功时用提供的序列化函数
template <class T, class F>
void send_result(httplib::Response& res, const Result<T>& result, F&& serialize, int status = 200) {
    if (result.is_err()) {
        send_error(res, result.error());
        return;
    }
    send_ok(res, serialize(result.value()), status);
}

// -----------------------------------------------------------------------------
//  请求
// -----------------------------------------------------------------------------

/// 解析 JSON 请求体。失败时返回 InvalidArgument。
Result<json> parse_json_body(const httplib::Request& req);

/// 从 Authorization: Bearer <token> 取 token；没有则返回空串
std::string bearer_token(const httplib::Request& req);

/// 取查询参数（int），缺省时用 fallback
int query_int(const httplib::Request& req, const char* name, int fallback);
int64_t query_int64(const httplib::Request& req, const char* name, int64_t fallback);
std::string query_str(const httplib::Request& req, const char* name, const std::string& fallback = {});
bool query_has(const httplib::Request& req, const char* name);
bool query_bool(const httplib::Request& req, const char* name, bool fallback);

/// 必须存在的字符串字段
Result<std::string> require_str(const json& body, const char* field);
Result<int64_t> require_int(const json& body, const char* field);
Result<double> require_number(const json& body, const char* field);

/// 可选字段
std::string opt_str(const json& body, const char* field, const std::string& fallback = {});

// -----------------------------------------------------------------------------
//  序列化
// -----------------------------------------------------------------------------

json category_to_json(const Category& c);

json record_to_json(const Record& r,
                    const std::string& category_name,
                    const std::string& category_color);

json daily_point_to_json(const analytics::DailyPoint& p);

// CategoryBreakdown 定义在 penhu 顶层（domain/record.hpp），不在 analytics 里
json breakdown_to_json(const CategoryBreakdown& b);

json summary_to_json(const analytics::Dashboard& d);

json trend_to_json(const analytics::TrendResult& t);

json anomaly_to_json(const analytics::Anomaly& a);

json anomalies_to_json(const analytics::AnomalyReport& r);

json insight_to_json(const analytics::Insight& i);

json insights_to_json(const analytics::InsightSet& s);

json report_facts_json(const report::ReportFacts& f);

json report_result_to_json(const report::ReportResult& r);

json stored_report_to_json(const storage::StoredReport& r);

/// 分类 id -> 名称/颜色 的查找，避免列表渲染时 N+1
class CategoryLookup {
public:
    explicit CategoryLookup(const std::vector<Category>& categories);
    std::string name_of(const std::string& id) const;
    std::string color_of(const std::string& id) const;

private:
    std::vector<std::pair<std::string, const Category*>> index_;
};

void build_lookup(const std::vector<Category>& categories, std::vector<const Category*>& out);
json records_page_to_json(const PagedRecords& page,
                          const std::vector<Category>& categories);

// -----------------------------------------------------------------------------
//  路由注册
// -----------------------------------------------------------------------------

void register_auth_routes(httplib::Server& srv, RouteContext& ctx);
void register_ledger_routes(httplib::Server& srv, RouteContext& ctx);
void register_insight_routes(httplib::Server& srv, RouteContext& ctx);
/// 本地模型：运行时安装、模型登记、服务生命周期。模型由用户指定，框架不代下载。
void register_local_model_routes(httplib::Server& srv, RouteContext& ctx);

// -----------------------------------------------------------------------------
//  路由级守卫
// -----------------------------------------------------------------------------

/// Host 头校验：只接受 127.0.0.1 / localhost / [::1]。
/// 目的：挡住 DNS rebinding —— 攻击者把自己的域名解析到 127.0.0.1，
/// 诱导你的浏览器去请求本机服务时，Host 会是那个域名而不是回环地址。
bool host_header_is_loopback(const httplib::Request& req);

/// 统一的请求前置检查（Host 校验 + JSON Content-Type 校验）。
/// 返回 false 表示已经写过响应，调用方应直接返回。
bool preflight_check(const httplib::Request& req, httplib::Response& res, bool needs_body);

}  // namespace penhu::server::detail
