#include "penhu/domain/user.hpp"

#include <algorithm>
#include <cctype>

namespace penhu {
namespace user_policy {

namespace {
bool is_ascii_alnum(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
bool is_ascii_digit(char c) noexcept { return c >= '0' && c <= '9'; }
}  // namespace

Status validate_username(const std::string& username) {
    if (username.size() < kMinUsernameLength) {
        return status_err(ErrorCode::InvalidArgument,
                          "用户名至少 " + std::to_string(kMinUsernameLength) + " 个字符",
                          "validate_username");
    }
    if (username.size() > kMaxUsernameLength) {
        return status_err(ErrorCode::InvalidArgument,
                          "用户名最多 " + std::to_string(kMaxUsernameLength) + " 个字符",
                          "validate_username");
    }
    // 限制成 ASCII：用户名要出现在文件名（<uuid>.db 虽然用 uuid，但用户名会进日志）
    // 和 URL 查询里，放开 Unicode 只会带来编码不一致的坑
    if (!is_ascii_alnum(username.front())) {
        return status_err(ErrorCode::InvalidArgument,
                          "用户名必须以字母或数字开头", "validate_username");
    }
    for (char c : username) {
        if (!is_ascii_alnum(c) && c != '_' && c != '-') {
            return status_err(ErrorCode::InvalidArgument,
                              std::string("用户名含非法字符 '") + c + "'（只允许字母、数字、_ 和 -）",
                              "validate_username");
        }
    }
    return status_ok();
}

Status validate_password(const std::string& password) {
    if (password.size() < kMinPasswordLength) {
        return status_err(ErrorCode::InvalidArgument,
                          "密码至少 " + std::to_string(kMinPasswordLength) + " 个字符",
                          "validate_password");
    }
    if (password.size() > kMaxPasswordLength) {
        return status_err(ErrorCode::InvalidArgument,
                          "密码最多 " + std::to_string(kMaxPasswordLength) + " 个字符",
                          "validate_password");
    }
    // 至少两类字符。刻意不搞「必须大写+数字+符号」那一套——
    // 复杂度规则会把用户逼去用 Passw0rd! 这种，实测强度反而不如长口令。
    int classes = 0;
    bool has_lower = false, has_upper = false, has_digit = false, has_symbol = false;
    for (char c : password) {
        if (is_ascii_digit(c))                     has_digit = true;
        else if (c >= 'a' && c <= 'z')             has_lower = true;
        else if (c >= 'A' && c <= 'Z')             has_upper = true;
        else                                       has_symbol = true;
    }
    classes = (has_lower ? 1 : 0) + (has_upper ? 1 : 0) + (has_digit ? 1 : 0) + (has_symbol ? 1 : 0);
    if (classes < 2) {
        return status_err(ErrorCode::InvalidArgument,
                          "密码需至少包含字母、数字、符号中的两类", "validate_password");
    }
    return status_ok();
}

std::string normalize_username(const std::string& username) {
    std::string out;
    out.reserve(username.size());
    for (char c : username) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

}  // namespace user_policy
}  // namespace penhu
