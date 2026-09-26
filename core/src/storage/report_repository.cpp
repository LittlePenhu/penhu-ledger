#include "penhu/storage/report_repository.hpp"

#include <cstdio>
#include "penhu/domain/date.hpp"
#include "penhu/util/uuid.hpp"

namespace penhu::storage {
namespace {

constexpr const char* kReportColumns =
    "id, date_key, kind, provider, model, content, stats_json, created_at";

/// 加密设置的 AAD：绑定到键名。把 api_key 的密文换个键名存进去也解不开。
std::string secret_aad(const std::string& key) {
    return "penhu:user_setting:" + key;
}

}  // namespace

namespace report_kind {
bool is_valid(const std::string& kind) {
    return kind == kDaily || kind == kWeekly || kind == kMonthly;
}
}  // namespace report_kind

StoredReport ReportRepository::row_to_report(Statement& s) {
    StoredReport r;
    r.id         = s.column_text(0);
    r.date       = Date::from_epoch_days(s.column_int64(1));
    r.kind       = s.column_text(2);
    r.provider   = s.column_text(3);
    r.model      = s.column_text_or(4, {});
    r.content    = s.column_text(5);
    r.stats_json = s.column_text_or(6, "{}");
    r.created_at = s.column_int64(7);
    return r;
}

Result<StoredReport> ReportRepository::upsert(const StoredReport& report) {
    if (!report_kind::is_valid(report.kind)) {
        return Result<StoredReport>::fail(
            ErrorCode::InvalidArgument,
            "非法的报告类型 '" + report.kind + "'（应为 daily/weekly/monthly）",
            "ReportRepository::upsert");
    }
    if (!report.date.is_valid()) {
        return Result<StoredReport>::fail(ErrorCode::InvalidArgument, "报告锚点日期无效",
                                         "ReportRepository::upsert");
    }
    if (report.content.empty()) {
        return Result<StoredReport>::fail(ErrorCode::InvalidArgument, "报告内容为空，拒绝存档",
                                         "ReportRepository::upsert");
    }

    StoredReport out = report;
    if (out.id.empty()) out.id = uuid_to_compact(new_uuid());
    if (out.created_at == 0) out.created_at = timeutil::now_epoch_seconds();

    // 依赖 (date_key, kind) 上的唯一索引做 upsert
    PENHU_ASSIGN_OR_RETURN(stmt, db_.prepare(R"SQL(
        INSERT INTO reports (id, date_key, kind, provider, model, content, stats_json, created_at)
        VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)
        ON CONFLICT(date_key, kind) DO UPDATE SET
            provider   = excluded.provider,
            model      = excluded.model,
            content    = excluded.content,
            stats_json = excluded.stats_json,
            created_at = excluded.created_at;
    )SQL"));

    PENHU_RETURN_IF_ERROR(stmt.bind(1, out.id));
    PENHU_RETURN_IF_ERROR(stmt.bind(2, out.date.to_epoch_days()));
    PENHU_RETURN_IF_ERROR(stmt.bind(3, out.kind));
    PENHU_RETURN_IF_ERROR(stmt.bind(4, out.provider));
    PENHU_RETURN_IF_ERROR(stmt.bind(5, out.model));
    PENHU_RETURN_IF_ERROR(stmt.bind(6, out.content));
    PENHU_RETURN_IF_ERROR(stmt.bind(7, out.stats_json));
    PENHU_RETURN_IF_ERROR(stmt.bind(8, out.created_at));

    PENHU_RETURN_IF_ERROR(stmt.step());
    return Result<StoredReport>(std::move(out));
}

Result<StoredReport> ReportRepository::find(const Date& anchor, const std::string& kind) {
    PENHU_ASSIGN_OR_RETURN(stmt, db_.prepare(
        std::string("SELECT ") + kReportColumns +
        " FROM reports WHERE date_key = ?1 AND kind = ?2 LIMIT 1;"));
    PENHU_RETURN_IF_ERROR(stmt.bind(1, anchor.to_epoch_days()));
    PENHU_RETURN_IF_ERROR(stmt.bind(2, kind));

    auto step = stmt.step();
    if (step.is_err()) return step.error();
    if (step.value() == StepResult::Done) {
        return Result<StoredReport>::fail(ErrorCode::NotFound,
                                         "尚未生成 " + anchor.to_string() + " 的 " + kind +
                                             " 报告",
                                         "ReportRepository::find");
    }
    return Result<StoredReport>(row_to_report(stmt));
}

Result<std::vector<StoredReport>> ReportRepository::list_recent(const std::string& kind,
                                                               int limit) {
    if (limit <= 0) limit = 30;
    limit = std::min(limit, 365);

    std::string sql = std::string("SELECT ") + kReportColumns +
                      " FROM reports WHERE kind = ?1 ORDER BY created_at DESC LIMIT ?2;";
    PENHU_ASSIGN_OR_RETURN(stmt, db_.prepare(sql));
    PENHU_RETURN_IF_ERROR(stmt.bind(1, kind));
    PENHU_RETURN_IF_ERROR(stmt.bind(2, limit));

    std::vector<StoredReport> out;
    auto st = stmt.each_row([&out](Statement& s) -> Status {
        out.push_back(row_to_report(s));
        return status_ok();
    });
    if (st.is_err()) return st.error();
    return Result<std::vector<StoredReport>>(std::move(out));
}

Status ReportRepository::remove(const std::string& id) {
    PENHU_ASSIGN_OR_RETURN(stmt, db_.prepare("DELETE FROM reports WHERE id = ?1;"));
    PENHU_RETURN_IF_ERROR(stmt.bind(1, id));
    auto st = stmt.step();
    if (st.is_err()) return to_status(st);
    if (db_.changes() == 0) {
        return status_err(ErrorCode::NotFound, "报告不存在: " + id, "ReportRepository::remove");
    }
    return status_ok();
}

int64_t ReportRepository::count() {
    auto stmt = db_.prepare("SELECT count(*) FROM reports;");
    if (stmt.is_err()) return 0;
    auto step = stmt.value().step();
    if (step.is_err() || step.value() != StepResult::Row) return 0;
    return stmt.value().column_int64(0);
}

// -----------------------------------------------------------------------------
//  SettingsRepository
// -----------------------------------------------------------------------------

Status SettingsRepository::set_plain(const std::string& key, const std::string& value) {
    if (key.empty()) {
        return status_err(ErrorCode::InvalidArgument, "设置键名不能为空", "set_plain");
    }
    PENHU_ASSIGN_OR_RETURN(stmt, db_.prepare(R"SQL(
        INSERT INTO user_settings (key, value, encrypted) VALUES (?1, ?2, 0)
        ON CONFLICT(key) DO UPDATE SET value = excluded.value, encrypted = 0;
    )SQL"));
    PENHU_RETURN_IF_ERROR(stmt.bind(1, key));
    PENHU_RETURN_IF_ERROR(stmt.bind(2, value));
    return to_status(stmt.step());
}

Result<std::optional<std::string>> SettingsRepository::get_plain(const std::string& key) {
    PENHU_ASSIGN_OR_RETURN(stmt, db_.prepare(
        "SELECT value, encrypted FROM user_settings WHERE key = ?1 LIMIT 1;"));
    PENHU_RETURN_IF_ERROR(stmt.bind(1, key));

    auto step = stmt.step();
    if (step.is_err()) return step.error();
    if (step.value() == StepResult::Done) {
        return Result<std::optional<std::string>>(std::optional<std::string>{});
    }
    if (stmt.column_bool(1)) {
        // 存的是密文，用 get_plain 读等于把一个 Base64 串当明文交出去
        return Result<std::optional<std::string>>::fail(
            ErrorCode::InvalidArgument,
            "设置项 '" + key + "' 是加密存储的，请用 get_secret 读取", "get_plain");
    }
    return Result<std::optional<std::string>>(
        std::optional<std::string>{stmt.column_text(0)});
}

Status SettingsRepository::set_secret(const std::string& key,
                                      const std::string& plaintext,
                                      const crypto::Key256& db_key) {
    if (key.empty()) {
        return status_err(ErrorCode::InvalidArgument, "设置键名不能为空", "set_secret");
    }
    if (db_key.is_all_zero()) {
        return status_err(ErrorCode::CryptoFailure,
                          "会话密钥为空，拒绝以「加密」名义存明文", "set_secret");
    }

    auto sealed = crypto::seal(db_key, plaintext, secret_aad(key));
    if (sealed.is_err()) return sealed.error();

    const std::string encoded = sealed.value().to_base64();

    PENHU_ASSIGN_OR_RETURN(stmt, db_.prepare(R"SQL(
        INSERT INTO user_settings (key, value, encrypted) VALUES (?1, ?2, 1)
        ON CONFLICT(key) DO UPDATE SET value = excluded.value, encrypted = 1;
    )SQL"));
    PENHU_RETURN_IF_ERROR(stmt.bind(1, key));
    PENHU_RETURN_IF_ERROR(stmt.bind(2, encoded));
    return to_status(stmt.step());
}

Result<std::optional<std::string>> SettingsRepository::get_secret(const std::string& key,
                                                                 const crypto::Key256& db_key) {
    if (db_key.is_all_zero()) {
        return Result<std::optional<std::string>>::fail(
            ErrorCode::CryptoFailure, "会话密钥为空", "get_secret");
    }

    PENHU_ASSIGN_OR_RETURN(stmt, db_.prepare(
        "SELECT value, encrypted FROM user_settings WHERE key = ?1 LIMIT 1;"));
    PENHU_RETURN_IF_ERROR(stmt.bind(1, key));

    auto step = stmt.step();
    if (step.is_err()) return step.error();
    if (step.value() == StepResult::Done) {
        return Result<std::optional<std::string>>(std::optional<std::string>{});
    }

    if (!stmt.column_bool(1)) {
        return Result<std::optional<std::string>>::fail(
            ErrorCode::InvalidArgument,
            "设置项 '" + key + "' 是明文存储的，用 get_secret 读会被当成密文解失败",
            "get_secret");
    }

    const std::string encoded = stmt.column_text(0);
    auto blob = crypto::base64_decode(encoded);
    if (blob.is_err()) return blob.error();

    auto opened = crypto::open(db_key, blob.value(), secret_aad(key));
    if (opened.is_err()) {
        return Result<std::optional<std::string>>::fail(
            ErrorCode::CryptoFailure,
            "解密设置项 '" + key + "' 失败：会话密钥不匹配或密文已损坏",
            "get_secret");
    }
    return Result<std::optional<std::string>>(std::optional<std::string>{std::move(opened).value()});
}

Status SettingsRepository::remove(const std::string& key) {
    PENHU_ASSIGN_OR_RETURN(stmt, db_.prepare("DELETE FROM user_settings WHERE key = ?1;"));
    PENHU_RETURN_IF_ERROR(stmt.bind(1, key));
    return to_status(stmt.step());
}

Result<std::vector<std::pair<std::string, bool>>> SettingsRepository::list_keys() {
    PENHU_ASSIGN_OR_RETURN(stmt, db_.prepare(
        "SELECT key, encrypted FROM user_settings ORDER BY key ASC;"));

    std::vector<std::pair<std::string, bool>> out;
    auto st = stmt.each_row([&out](Statement& s) -> Status {
        out.emplace_back(s.column_text(0), s.column_bool(1));
        return status_ok();
    });
    if (st.is_err()) return st.error();
    return Result<std::vector<std::pair<std::string, bool>>>(std::move(out));
}

}  // namespace penhu::storage
