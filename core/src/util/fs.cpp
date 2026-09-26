#include "penhu/util/fs.hpp"
#include "penhu/domain/date.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <system_error>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace fs_std = std::filesystem;

namespace penhu::fs {
namespace {

/// std::string(UTF-8) -> std::filesystem::path。
/// Windows 下必须走这个转换，否则含中文的路径会乱码。
fs_std::path to_path(const std::string& utf8) {
#if defined(_WIN32)
    if (utf8.empty()) return fs_std::path{};
    const int wlen = ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                          static_cast<int>(utf8.size()), nullptr, 0);
    if (wlen <= 0) return fs_std::path(utf8);   // 退化为本地编码，总比抛异常好
    std::wstring wide(static_cast<size_t>(wlen), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()),
                          wide.data(), wlen);
    return fs_std::path(wide);
#else
    return fs_std::path(utf8);
#endif
}

std::string from_path(const fs_std::path& p) {
#if defined(_WIN32)
    const std::wstring wide = p.wstring();
    if (wide.empty()) return {};
    const int len = ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                                         static_cast<int>(wide.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<size_t>(len), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                          out.data(), len, nullptr, nullptr);
    return out;
#else
    return p.string();
#endif
}

Status from_error(const std::error_code& ec, const std::string& what, const std::string& path) {
    return status_err(ErrorCode::StorageFailure,
                      std::string("文件系统操作失败: ") + what + " -> " + ec.message(),
                      path);
}

}  // namespace

bool exists(const std::string& path) {
    std::error_code ec;
    return fs_std::exists(to_path(path), ec);
}

bool is_directory(const std::string& path) {
    std::error_code ec;
    return fs_std::is_directory(to_path(path), ec);
}

bool is_regular_file(const std::string& path) {
    std::error_code ec;
    return fs_std::is_regular_file(to_path(path), ec);
}

Status create_directories(const std::string& path) {
    std::error_code ec;
    fs_std::create_directories(to_path(path), ec);
    // 已存在不算错误（ec 可能是 "file exists"，也可能为空）
    if (ec && !fs_std::is_directory(to_path(path))) {
        return from_error(ec, "create_directories", path);
    }
    return status_ok();
}

Status remove_file(const std::string& path) {
    if (!exists(path)) return status_ok();   // 幂等
    std::error_code ec;
    fs_std::remove(to_path(path), ec);
    if (ec) return from_error(ec, "remove_file", path);
    return status_ok();
}

Status remove_all(const std::string& path) {
    if (!exists(path)) return status_ok();
    std::error_code ec;
    fs_std::remove_all(to_path(path), ec);
    if (ec) return from_error(ec, "remove_all", path);
    return status_ok();
}

Status copy_file(const std::string& from, const std::string& to, bool overwrite) {
    std::error_code ec;
    fs_std::copy_file(to_path(from), to_path(to),
                      overwrite ? fs_std::copy_options::overwrite_existing
                                : fs_std::copy_options::none,
                      ec);
    if (ec) return from_error(ec, "copy_file", from + " -> " + to);
    return status_ok();
}

Status rename(const std::string& from, const std::string& to) {
    std::error_code ec;
    fs_std::rename(to_path(from), to_path(to), ec);
    if (ec) return from_error(ec, "rename", from + " -> " + to);
    return status_ok();
}

int64_t file_size(const std::string& path) {
    std::error_code ec;
    const auto size = fs_std::file_size(to_path(path), ec);
    if (ec) return 0;
    return static_cast<int64_t>(size);
}

Result<std::vector<std::string>> list_files(const std::string& dir) {
    std::error_code ec;
    if (!fs_std::exists(to_path(dir), ec) || ec) {
        return Result<std::vector<std::string>>::fail(
            ErrorCode::NotFound, "目录不存在: " + dir, "fs::list_files");
    }

    std::vector<std::string> names;
    fs_std::directory_iterator it(to_path(dir), ec);
    if (ec) return from_error(ec, "list_files", dir).error();

    for (const auto& entry : it) {
        std::error_code entry_ec;
        if (entry.is_regular_file(entry_ec) && !entry_ec) {
            names.push_back(from_path(entry.path().filename()));
        }
    }
    std::sort(names.begin(), names.end());
    return Result<std::vector<std::string>>(std::move(names));
}

Result<std::vector<std::string>> list_entries(const std::string& dir) {
    std::error_code ec;
    if (!fs_std::exists(to_path(dir), ec) || ec) {
        return Result<std::vector<std::string>>::fail(
            ErrorCode::NotFound, "目录不存在: " + dir, "fs::list_entries");
    }

    std::vector<std::string> names;
    fs_std::directory_iterator it(to_path(dir), ec);
    if (ec) return from_error(ec, "list_entries", dir).error();

    for (const auto& entry : it) {
        std::error_code entry_ec;
        const bool regular = entry.is_regular_file(entry_ec) && !entry_ec;
        const bool dirp = entry.is_directory(entry_ec) && !entry_ec;
        if (regular || dirp) {
            names.push_back(from_path(entry.path().filename()));
        }
    }
    std::sort(names.begin(), names.end());
    return Result<std::vector<std::string>>(std::move(names));
}

std::string join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    std::string out = a;
    const char last = out.back();
    if (last != '/' && last != '\\') out.push_back('/');
    return out + b;
}

std::string filename(const std::string& path) {
    return from_path(to_path(path).filename());
}

std::string parent(const std::string& path) {
    return from_path(to_path(path).parent_path());
}

std::string absolute(const std::string& path) {
    std::error_code ec;
    auto abs = fs_std::absolute(to_path(path), ec);
    if (ec) return path;
    return from_path(abs.lexically_normal());
}

std::string executable_dir() {
#if defined(_WIN32)
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD len = ::GetModuleFileNameW(nullptr, buf.data(),
                                              static_cast<DWORD>(buf.size()));
        if (len == 0) return {};
        if (len < buf.size()) {
            buf.resize(len);
            break;
        }
        buf.resize(buf.size() * 2);   // 路径被截断，加倍再试
    }
    return from_path(fs_std::path(buf).parent_path());
#else
    std::error_code ec;
    auto p = fs_std::read_symlink("/proc/self/exe", ec);
    if (ec) return {};
    return from_path(p.parent_path());
#endif
}

std::string home_dir() {
#if defined(_WIN32)
    const char* profile = std::getenv("USERPROFILE");
    if (profile != nullptr && *profile != '\0') return std::string(profile);
    const char* drive = std::getenv("HOMEDRIVE");
    const char* path  = std::getenv("HOMEPATH");
    if (drive != nullptr && path != nullptr) return std::string(drive) + std::string(path);
    return {};
#else
    const char* home = std::getenv("HOME");
    return home != nullptr ? std::string(home) : std::string{};
#endif
}

std::string timestamp_slug() {
    const int64_t now = timeutil::now_epoch_seconds();
    const std::string local = timeutil::to_local_string(now);   // "2026-09-18 16:35:00"
    std::string out;
    out.reserve(15);
    for (char c : local) {
        if (c >= '0' && c <= '9') out.push_back(c);
    }
    return out;   // "20260918163500"
}

}  // namespace penhu::fs
