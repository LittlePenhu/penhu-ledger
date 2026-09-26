// =============================================================================
//  native/app/platform_fs_linux.cpp
//  platform_fs 的 Linux 实现。
//
//  和 Windows 那份最大的差别不在 API，而在**有些事桌面环境没给标准答案**：
//  Windows 有 IFileDialog / SHGetKnownFolderPath 这种系统级统一接口，
//  Linux 这边是 freedesktop 规范 + 各家实现，于是分成了三层策略：
//
//    1. 有规范的就按规范做（回收站 = XDG Trash spec，图片目录 = XDG user-dirs）
//    2. 没规范但有事实标准的，调现成工具（选文件/选目录 → zenity / kdialog）
//    3. 都没有时**明确失败并说清缺什么**，不假装成功
//       —— 「点了没反应」比「告诉你没装 zenity」糟糕得多
// =============================================================================

#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <system_error>

#include "app/platform_fs.hpp"
#include "penhu/util/log.hpp"
#include "penhu/util/process.hpp"
#include "ui/convert.hpp"

namespace fs = std::filesystem;

namespace penhu::native {

namespace {

/// 把 UTF-8 路径按 XDG Trash 规范做百分号编码。
/// 保留 '/'（它是路径分隔符）、以及 URL 里的 unreserved 集合；
/// 其余字节（含中文的每一个字节）都转成 %XX。
std::string percent_encode_path(const std::string& utf8) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(utf8.size() * 2);
    for (unsigned char c : utf8) {
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') ||
                                c == '-' || c == '_' || c == '.' || c == '~' ||
                                c == '/';   // 分隔符按规范要保留
        if (unreserved) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

/// 从 `zenity --file-selection` 的输出里取路径。
/// zenity 成功时 stdout 是若干行路径（多选模式一行一个），失败时输出为空。
std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> lines;
    std::string cur;
    for (char c : s) {
        if (c == '\n') {
            // zenity 在 Wayland/某些版本下会把多个路径塞在一行里，用 '|' 分隔
            size_t start = 0;
            while (true) {
                const size_t bar = cur.find('|', start);
                const std::string piece =
                    cur.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
                if (!piece.empty()) lines.push_back(piece);
                if (bar == std::string::npos) break;
                start = bar + 1;
            }
            cur.clear();
        } else if (c != '\r') {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

/// 找一个可用的图形化文件对话框程序。找不到就返回空串。
/// 检查顺序按「桌面环境的默认」排：GNOME 系有 zenity、KDE 系有 kdialog。
std::string find_file_dialog_tool() {
    for (const char* t : {"zenity", "kdialog"}) {
        if (process::command_exists(t)) return t;
    }
    return {};
}

}  // namespace

bool pick_image_files(std::vector<std::wstring>& out) {
    out.clear();
    const std::string tool = find_file_dialog_tool();
    if (tool.empty()) {
        Logger::default_logger().warn(
            "选图对话框不可用：没找到 zenity 或 kdialog。"
            "装一个即可（Arch: sudo pacman -S zenity）。"
            "在此之前可以用「扫描目录」把整个目录导入。");
        return false;
    }

    std::vector<std::string> args;
    if (tool == "zenity") {
        args = {"--file-selection", "--multiple", "--separator=|",
                "--title=选择支付截图（可按 Ctrl / Shift 多选）",
                "--file-filter=图片文件 | *.png *.jpg *.jpeg *.bmp *.gif *.webp *.tif *.tiff",
                "--file-filter=所有文件 | *"};
    } else {
        args = {"--getopenfilename", ".", "图片文件 (*.png *.jpg *.jpeg *.bmp *.webp)"};
    }

    auto r = process::run(tool, args, {}, 0);
    if (r.is_err() || r.value().exit_code != 0) return false;   // 用户取消

    for (const std::string& line : split_lines(r.value().output)) {
        // 多选时 kdialog 也只回一行、可能带引号，统一去掉两端的引号
        std::string p = line;
        while (!p.empty() && (p.front() == '"' || p.front() == '\'')) p.erase(p.begin());
        while (!p.empty() && (p.back() == '"' || p.back() == '\'')) p.pop_back();
        if (!p.empty()) out.push_back(ui::to_wide(p));
    }
    return !out.empty();
}

bool pick_directory(std::wstring& out) {
    const std::string tool = find_file_dialog_tool();
    if (tool.empty()) {
        Logger::default_logger().warn(
            "选目录对话框不可用：没找到 zenity 或 kdialog（Arch: sudo pacman -S zenity）。");
        return false;
    }

    std::vector<std::string> args;
    if (tool == "zenity") {
        args = {"--file-selection", "--directory", "--title=选择截图保存目录"};
    } else {
        args = {"--getexistingdirectory", "."};
    }

    auto r = process::run(tool, args, {}, 0);
    if (r.is_err() || r.value().exit_code != 0) return false;

    const std::vector<std::string> lines = split_lines(r.value().output);
    if (lines.empty()) return false;
    out = ui::to_wide(lines.front());
    return true;
}

bool file_dialog_available() { return !find_file_dialog_tool().empty(); }

bool file_stamp(const std::wstring& path, long long* size, long long* mtime) {
    // 用 ::stat 而不是 std::filesystem：fs 的 last_write_time 要转成 Unix 秒
    // 得靠 C++20 的 clock_cast，而 libstdc++ 目前还没提供。
    // 指纹判重要的就是「同一套数字」，直接 stat 最省事也最准。
    struct stat st{};
    const std::string utf8 = ui::to_utf8(path);
    if (::stat(utf8.c_str(), &st) != 0) return false;
    *size = static_cast<long long>(st.st_size);
    *mtime = static_cast<long long>(st.st_mtime);
    return true;
}

bool move_to_trash(const std::wstring& path) {
    const std::string utf8 = ui::to_utf8(path);

    // 先试 gio（glib 自带，pango 依赖它所以基本一定在）。
    // 它实现了完整的 XDG 规范，包括「文件在别的挂载点上要用那个挂载点的回收站」
    // 这种自己写很容易漏的情况。
    if (process::command_exists("gio")) {
        auto r = process::run("gio", {"trash", "--", utf8}, {}, 15000);
        if (r.is_ok() && r.value().exit_code == 0) return true;
    }

    // 退路：自己实现 XDG 的**主目录回收站**（只覆盖文件在 $HOME 所在文件系统的情况）。
    // 这就是这一层的目的 —— 至少不出「删不掉还当成删掉了」。
    const char* xdg_data = std::getenv("XDG_DATA_HOME");
    const char* home = std::getenv("HOME");
    std::string base = xdg_data != nullptr && *xdg_data != '\0'
                           ? std::string(xdg_data)
                           : (home != nullptr ? std::string(home) + "/.local/share" : std::string());
    if (base.empty()) return false;

    std::error_code ec;
    const fs::path trash = fs::path(base) / "Trash";
    fs::create_directories(trash / "files", ec);
    fs::create_directories(trash / "info", ec);
    if (ec) return false;

    // 同名冲突时按规范追加 .2 / .3 …
    const std::string name = fs::path(utf8).filename().string();
    std::string stem = name;
    std::string ext;
    if (const size_t dot = name.find_last_of('.'); dot != std::string::npos && dot != 0) {
        stem = name.substr(0, dot);
        ext = name.substr(dot);
    }

    std::string final_name = name;
    for (int i = 2; i < 1000; ++i) {
        if (!fs::exists(trash / "files" / final_name, ec) &&
            !fs::exists(trash / "info" / (final_name + ".trashinfo"), ec)) {
            break;
        }
        final_name = stem + "." + std::to_string(i) + ext;
    }

    // 先写 .trashinfo 再改名：反过来的话中途失败会留下一个「回收站里有文件
    // 但没有记录」的状态，用户就没法从文件管理器还原了。
    {
        std::time_t now = std::time(nullptr);
        std::tm tm{};
        localtime_r(&now, &tm);
        char stamp[32]{};
        std::snprintf(stamp, sizeof(stamp), "%04d-%02d-%02dT%02d:%02d:%02d",
                      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                      tm.tm_hour, tm.tm_min, tm.tm_sec);

        const fs::path info = trash / "info" / (final_name + ".trashinfo");
        FILE* f = std::fopen(info.string().c_str(), "wb");
        if (f == nullptr) return false;
        const std::string body = "[Trash Info]\nPath=" + percent_encode_path(utf8) +
                                 "\nDeletionDate=" + stamp + "\n";
        std::fwrite(body.data(), 1, body.size(), f);
        std::fclose(f);
    }

    fs::rename(utf8, trash / "files" / final_name, ec);
    return !ec;
}

uint64_t monotonic_ms() {
    // CLOCK_MONOTONIC：从开机起单调递增，不受 NTP 校准 / 用户改表影响。
    // 不要用 CLOCK_REALTIME —— 那个是「墙上的时间」，会被调整。
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000ull +
           static_cast<uint64_t>(ts.tv_nsec) / 1000000ull;
}

std::wstring default_screenshot_dir() {
    // 优先读 XDG user-dirs 的配置：用户可能把「图片」目录改到别处了，
    // 直接假定 $HOME/Pictures 在那种机器上会给出一个不存在的路径。
    std::string pictures;
    if (const char* home = std::getenv("HOME"); home != nullptr) {
        const fs::path cfg = fs::path(home) / ".config/user-dirs.dirs";
        std::error_code ec;
        if (fs::exists(cfg, ec)) {
            if (FILE* f = std::fopen(cfg.string().c_str(), "rb")) {
                char line[1024];
                while (std::fgets(line, sizeof(line), f) != nullptr) {
                    std::string s = line;
                    if (s.rfind("XDG_PICTURES_DIR", 0) != 0) continue;
                    const size_t eq = s.find('=');
                    if (eq == std::string::npos) continue;
                    std::string v = s.substr(eq + 1);
                    while (!v.empty() && (v.back() == '\n' || v.back() == '\r' ||
                                          v.back() == '"' || v.back() == ' ')) {
                        v.pop_back();
                    }
                    while (!v.empty() && (v.front() == '"' || v.front() == ' ')) v.erase(v.begin());
                    if (v.rfind("$HOME/", 0) == 0) v = std::string(home) + v.substr(5);
                    pictures = v;
                    break;
                }
                std::fclose(f);
            }
        }
        if (pictures.empty()) pictures = std::string(home) + "/Pictures";
    }
    if (pictures.empty()) return {};
    return ui::to_wide(pictures + "/账本");
}

}  // namespace penhu::native
