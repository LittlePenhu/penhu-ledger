#include "penhu/app/app_config.hpp"
#include "penhu/util/fs.hpp"

namespace penhu::app {

Result<AppConfig> AppConfig::from_data_dir(const std::string& dir) {
    if (dir.empty()) {
        return Result<AppConfig>::fail(ErrorCode::ConfigError, "数据目录不能为空",
                                       "AppConfig::from_data_dir");
    }
    AppConfig cfg;
    cfg.data_dir = fs::absolute(dir);
    return Result<AppConfig>(std::move(cfg));
}

Result<AppConfig> AppConfig::default_config() {
    const std::string home = fs::home_dir();
    if (home.empty()) {
        return Result<AppConfig>::fail(
            ErrorCode::ConfigError,
            "无法确定用户主目录（USERPROFILE / HOME 都为空），请用 --data-dir 显式指定",
            "AppConfig::default_config");
    }
    return from_data_dir(fs::join(home, "PenHuLedger"));
}

std::string AppConfig::users_db_path() const {
    return fs::join(data_dir, users_db_filename);
}

std::string AppConfig::data_subdir() const {
    return fs::join(data_dir, "data");
}

std::string AppConfig::ledger_path(const std::string& db_filename) const {
    return fs::join(data_subdir(), db_filename);
}

std::string AppConfig::backups_dir() const {
    return fs::join(data_dir, "backups");
}

std::string AppConfig::logs_dir() const {
    return fs::join(data_dir, "logs");
}

std::string AppConfig::log_file_path() const {
    return fs::join(logs_dir(), "penhu-ledger.log");
}

Status AppConfig::ensure_directories() const {
    PENHU_RETURN_IF_ERROR(fs::create_directories(data_dir));
    PENHU_RETURN_IF_ERROR(fs::create_directories(data_subdir()));
    PENHU_RETURN_IF_ERROR(fs::create_directories(backups_dir()));
    PENHU_RETURN_IF_ERROR(fs::create_directories(logs_dir()));
    return status_ok();
}

}  // namespace penhu::app
