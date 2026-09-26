// =============================================================================
//  server/src/routes_ledger.cpp
//  分类与记录的 CRUD 路由。
//
//  金额的传输约定：
//    请求可以传 amountMinor（整数分）或 amount（字符串元，如 "12.34"）。
//    响应同时给 amountMinor、amount（"12.34"）和 amountDisplay（"¥12.34"）。
//    为什么以分为准：前端用浮点做金额累加会出现 0.1+0.2 的问题，
//    所有算术必须走整数，展示层才转字符串。
// =============================================================================

#include "api_support.hpp"

#include <optional>

namespace penhu::server::detail {
namespace {

bool guard(const httplib::Request& req, httplib::Response& res, std::string& token,
           bool needs_body) {
    if (!preflight_check(req, res, needs_body)) return false;
    token = bearer_token(req);
    if (token.empty()) {
        send_error(res, ErrorCode::AuthFailed, "缺少 Authorization: Bearer <token> 头");
        return false;
    }
    return true;
}

/// 从请求体解析一个金额：优先用 amountMinor，其次解析 amount 字符串
Result<Money> parse_amount(const json& body) {
    if (body.contains("amountMinor") && body["amountMinor"].is_number_integer()) {
        const int64_t minor = body["amountMinor"].get<int64_t>();
        if (minor <= 0) {
            return Result<Money>::fail(ErrorCode::InvalidArgument, "金额必须大于 0", "parse_amount");
        }
        return Result<Money>(Money::from_minor(minor));
    }
    if (body.contains("amount")) {
        const std::string text = body["amount"].is_string()
                                     ? body["amount"].get<std::string>()
                                     : body["amount"].dump();
        return Money::parse(text);
    }
    return Result<Money>::fail(ErrorCode::InvalidArgument,
                               "缺少金额字段（amountMinor 或 amount）", "parse_amount");
}

/// 把请求体解析成 RecordInput。allow_partial 用于 PATCH（未给的字段沿用 existing）。
Result<RecordInput> parse_record_input(const json& body,
                                      const std::optional<Record>& existing) {
    RecordInput in;

    if (existing.has_value()) {
        in = RecordInput{existing->amount, existing->direction, existing->category_id,
                         existing->date, existing->note};
    }

    if (body.contains("amountMinor") || body.contains("amount")) {
        auto amount = parse_amount(body);
        if (amount.is_err()) return amount.error();
        in.amount = amount.value();
    }

    if (body.contains("direction") && body["direction"].is_string()) {
        auto parsed = parse_direction(body["direction"].get<std::string>());
        if (!parsed.has_value()) {
            return Result<RecordInput>::fail(
                ErrorCode::InvalidArgument,
                "direction 必须是 'expense' 或 'income'（也接受 支出/收入）",
                "parse_record_input");
        }
        in.direction = parsed.value();
    }

    if (body.contains("categoryId") && body["categoryId"].is_string()) {
        in.category_id = body["categoryId"].get<std::string>();
    }

    if (body.contains("date") && body["date"].is_string()) {
        auto date = Date::parse(body["date"].get<std::string>());
        if (date.is_err()) return date.error();
        in.date = date.value();
    }

    if (body.contains("note")) {
        in.note = body["note"].is_string() ? body["note"].get<std::string>()
                                          : body["note"].dump();
    }

    return Result<RecordInput>(std::move(in));
}

}  // namespace

void register_ledger_routes(httplib::Server& srv, RouteContext& ctx) {
    app::LedgerService& ledger = ctx.ledger;

    // =========================================================================
    //  分类
    // =========================================================================

    srv.Get("/api/categories", [&ledger](const httplib::Request& req, httplib::Response& res) {
        if (!preflight_check(req, res, false)) return;
        const std::string token = bearer_token(req);

        std::optional<Direction> direction;
        if (query_has(req, "direction")) {
            auto parsed = parse_direction(query_str(req, "direction"));
            if (parsed.has_value()) direction = parsed.value();
        }
        const bool include_inactive = query_bool(req, "includeInactive", false);

        auto cats = ledger.list_categories(token, direction, include_inactive);
        if (cats.is_err()) { send_error(res, cats.error()); return; }

        json arr = json::array();
        for (const auto& c : cats.value()) arr.push_back(category_to_json(c));
        send_ok(res, arr);
    });

    srv.Post("/api/categories", [&ledger](const httplib::Request& req, httplib::Response& res) {
        std::string token;
        if (!guard(req, res, token, true)) return;

        auto body = parse_json_body(req);
        if (body.is_err()) { send_error(res, body.error()); return; }

        auto name = require_str(body.value(), "name");
        if (name.is_err()) { send_error(res, name.error()); return; }

        Direction direction = Direction::Expense;
        if (body.value().contains("direction") && body.value()["direction"].is_string()) {
            auto parsed = parse_direction(body.value()["direction"].get<std::string>());
            if (parsed.has_value()) direction = parsed.value();
        }

        const std::string icon = opt_str(body.value(), "icon", "label");
        const std::string color = opt_str(body.value(), "color", "#78909C");
        const int sort_order = query_int(req, "sortOrder",
                                        body.value().value("sortOrder", 500));

        auto created = ledger.create_category(token, name.value(), icon, color, direction,
                                             sort_order);
        if (created.is_err()) { send_error(res, created.error()); return; }
        send_ok(res, category_to_json(created.value()), 201);
    });

    // PATCH 单个分类（id 是 32 位十六进制）
    srv.Patch(R"(/api/categories/([0-9a-fA-F]{32}))",
              [&ledger](const httplib::Request& req, httplib::Response& res) {
                  std::string token;
                  if (!guard(req, res, token, true)) return;

                  const std::string id = req.matches[1].str();
                  auto body = parse_json_body(req);
                  if (body.is_err()) { send_error(res, body.error()); return; }

                  // 先取现有值，PATCH 语义是「只改给了的字段」
                  auto cats = ledger.list_categories(token, std::nullopt, true);
                  if (cats.is_err()) { send_error(res, cats.error()); return; }

                  std::optional<Category> existing;
                  for (const auto& c : cats.value()) {
                      if (c.id == id) { existing = c; break; }
                  }
                  if (!existing.has_value()) {
                      send_error(res, ErrorCode::NotFound, "分类不存在: " + id);
                      return;
                  }

                  Category updated = existing.value();
                  if (body.value().contains("name")) {
                      updated.name = opt_str(body.value(), "name", updated.name);
                  }
                  if (body.value().contains("icon")) {
                      updated.icon = opt_str(body.value(), "icon", updated.icon);
                  }
                  if (body.value().contains("color")) {
                      updated.color_hex = opt_str(body.value(), "color", updated.color_hex);
                  }
                  if (body.value().contains("sortOrder") &&
                      body.value()["sortOrder"].is_number_integer()) {
                      updated.sort_order = body.value()["sortOrder"].get<int>();
                  }
                  if (body.value().contains("isActive") &&
                      body.value()["isActive"].is_boolean()) {
                      updated.is_active = body.value()["isActive"].get<bool>();
                  }

                  auto st = ledger.update_category(token, updated);
                  if (st.is_err()) { send_error(res, st.error()); return; }
                  send_ok(res, category_to_json(updated));
              });

    srv.Delete(R"(/api/categories/([0-9a-fA-F]{32}))",
               [&ledger](const httplib::Request& req, httplib::Response& res) {
                   std::string token;
                   if (!guard(req, res, token, false)) return;

                   auto st = ledger.delete_category(token, req.matches[1].str());
                   if (st.is_err()) { send_error(res, st.error()); return; }
                   send_ok(res, json{{"deleted", true}});
               });

    // =========================================================================
    //  记录
    // =========================================================================

    srv.Get("/api/records", [&ledger](const httplib::Request& req, httplib::Response& res) {
        if (!preflight_check(req, res, false)) return;
        const std::string token = bearer_token(req);

        RecordQuery q;
        if (query_has(req, "from") && query_has(req, "to")) {
            auto from = Date::parse(query_str(req, "from"));
            auto to = Date::parse(query_str(req, "to"));
            if (from.is_err()) { send_error(res, from.error()); return; }
            if (to.is_err()) { send_error(res, to.error()); return; }
            q.range = DateRange{from.value(), to.value()};
        }
        if (query_has(req, "direction")) {
            auto parsed = parse_direction(query_str(req, "direction"));
            if (parsed.has_value()) q.direction = parsed.value();
        }
        if (query_has(req, "categoryId")) {
            q.category_id = query_str(req, "categoryId");
        }
        if (query_has(req, "keyword")) {
            q.note_keyword = query_str(req, "keyword");
        }
        if (query_has(req, "minMinor")) q.min_minor = query_int64(req, "minMinor", 0);
        if (query_has(req, "maxMinor")) q.max_minor = query_int64(req, "maxMinor", 0);
        q.limit = query_int(req, "limit", 50);
        q.offset = query_int(req, "offset", 0);
        q.ascending = query_bool(req, "ascending", false);

        auto page = ledger.list_records(token, q);
        if (page.is_err()) { send_error(res, page.error()); return; }

        // 拿分类表来回填名称与颜色
        auto cats = ledger.list_categories(token, std::nullopt, true);
        static const std::vector<Category> kEmpty;
        const auto& categories = cats.is_ok() ? cats.value() : kEmpty;

        send_ok(res, records_page_to_json(page.value(), categories));
    });

    srv.Post("/api/records", [&ledger](const httplib::Request& req, httplib::Response& res) {
        std::string token;
        if (!guard(req, res, token, true)) return;

        auto body = parse_json_body(req);
        if (body.is_err()) { send_error(res, body.error()); return; }

        auto input = parse_record_input(body.value(), std::nullopt);
        if (input.is_err()) { send_error(res, input.error()); return; }

        // 没给日期就默认今天——记账时最常发生的情况就是「现在花的钱」
        if (!input.value().date.is_valid()) {
            input.value().date = Date::today();
        }
        // 没给分类就拒绝，不猜
        if (input.value().category_id.empty()) {
            send_error(res, ErrorCode::InvalidArgument, "必须指定 categoryId");
            return;
        }
        if (input.value().amount.minor_units() == 0) {
            send_error(res, ErrorCode::InvalidArgument, "必须提供金额");
            return;
        }

        auto created = ledger.add_record(token, input.value());
        if (created.is_err()) { send_error(res, created.error()); return; }

        auto cats = ledger.list_categories(token, std::nullopt, true);
        std::string name = "（未知分类）";
        std::string color = "#78909C";
        if (cats.is_ok()) {
            for (const auto& c : cats.value()) {
                if (c.id == created.value().category_id) {
                    name = c.name;
                    color = c.color_hex;
                    break;
                }
            }
        }
        send_ok(res, record_to_json(created.value(), name, color), 201);
    });

    srv.Patch(R"(/api/records/([0-9a-fA-F]{32}))",
              [&ledger](const httplib::Request& req, httplib::Response& res) {
                  std::string token;
                  if (!guard(req, res, token, true)) return;

                  const std::string id = req.matches[1].str();
                  auto body = parse_json_body(req);
                  if (body.is_err()) { send_error(res, body.error()); return; }

                  auto existing = ledger.get_record(token, id);
                  if (existing.is_err()) { send_error(res, existing.error()); return; }

                  auto input = parse_record_input(body.value(), existing.value());
                  if (input.is_err()) { send_error(res, input.error()); return; }

                  auto updated = ledger.update_record(token, id, input.value());
                  if (updated.is_err()) { send_error(res, updated.error()); return; }

                  auto cats = ledger.list_categories(token, std::nullopt, true);
                  std::string name = "（未知分类）";
                  std::string color = "#78909C";
                  if (cats.is_ok()) {
                      for (const auto& c : cats.value()) {
                          if (c.id == updated.value().category_id) {
                              name = c.name;
                              color = c.color_hex;
                              break;
                          }
                      }
                  }
                  send_ok(res, record_to_json(updated.value(), name, color));
              });

    srv.Delete(R"(/api/records/([0-9a-fA-F]{32}))",
               [&ledger](const httplib::Request& req, httplib::Response& res) {
                   std::string token;
                   if (!guard(req, res, token, false)) return;

                   auto st = ledger.delete_record(token, req.matches[1].str());
                   if (st.is_err()) { send_error(res, st.error()); return; }
                   send_ok(res, json{{"deleted", true}});
               });

    srv.Get(R"(/api/records/([0-9a-fA-F]{32}))",
            [&ledger](const httplib::Request& req, httplib::Response& res) {
                if (!preflight_check(req, res, false)) return;
                const std::string token = bearer_token(req);

                auto record = ledger.get_record(token, req.matches[1].str());
                if (record.is_err()) { send_error(res, record.error()); return; }

                auto cats = ledger.list_categories(token, std::nullopt, true);
                std::string name = "（未知分类）";
                std::string color = "#78909C";
                if (cats.is_ok()) {
                    for (const auto& c : cats.value()) {
                        if (c.id == record.value().category_id) {
                            name = c.name;
                            color = c.color_hex;
                            break;
                        }
                    }
                }
                send_ok(res, record_to_json(record.value(), name, color));
            });
}

}  // namespace penhu::server::detail
