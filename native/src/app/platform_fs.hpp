#pragma once
// =============================================================================
//  native/app/platform_fs.hpp
//  平台相关的「文件系统 + 系统对话框」操作。
//
//  为什么抽出来：这些是 app.cpp（业务逻辑，两个平台共用）里**唯一**的
//  Windows 专有调用。把它们集中到一个接口后面，app.cpp 就变得完全平台无关，
//  Linux 侧只要换一个实现文件。
//
//  抽的时候顺手做了一件事：原来 `pick_screenshot` 把「弹对话框」和
//  「把选中的路径变成 ScanItem 列表」混在一个函数里。现在对话框只负责返回
//  一串路径，拼列表那段留在 app.cpp —— 那部分是平台无关的，没必要写两遍。
//
//  五个函数对应五件事，每件在两个平台上的做法差别都很大：
//    pick_image_files    Windows: GetOpenFileNameW（多选）   Linux: zenity / kdialog
//    pick_directory      Windows: IFileDialog+FOS_PICKFOLDERS Linux: zenity -d
//    file_stamp          Windows: GetFileAttributesExW       Linux: stat()
//    move_to_trash       Windows: SHFileOperationW+UNDO      Linux: XDG 回收站规范
//    default_screenshot_dir
//                        Windows: SHGetKnownFolderPath       Linux: XDG_PICTURES_DIR
// =============================================================================

#include <cstdint>
#include <string>
#include <vector>

namespace penhu::native {

/// 弹系统文件对话框选图片，可多选。
/// 返回 false = 用户取消，**或**这个平台上没有可用的对话框程序。
/// 两者要分开处理，所以再提供 file_dialog_available() 让调用方区分：
/// 前者什么都不用说，后者要告诉用户装什么。
bool pick_image_files(std::vector<std::wstring>& out);

/// 弹系统目录选择对话框。语义同 pick_image_files。
bool pick_directory(std::wstring& out);

/// 这个平台上有没有可用的图形化文件对话框。
/// Windows 恒为 true；Linux 取决于 zenity / kdialog 在不在。
bool file_dialog_available();

/// 取文件的大小（字节）与修改时间（Unix 秒）。
/// 用平台 API 而不是 std::filesystem：C++17 的 last_write_time 没有标准方式
/// 转成 Unix 时间戳（要 C++20 的 clock_cast），而指纹判重要的是同一套数字。
bool file_stamp(const std::wstring& path, long long* size, long long* mtime);

/// 把文件移到**回收站**（可还原）。
///
/// 绝不用「永久删除」实现它：批量自动清理不可逆，万一认错了图、或者用户其实
/// 想留着，回收站是最后一道退路。两个平台的实现都遵守这条。
bool move_to_trash(const std::wstring& path);

/// 默认的截图目录（系统「图片」目录下的「账本」）。
/// 取不到「图片」目录时返回空串，调用方保持原值。
std::wstring default_screenshot_dir();

/// 单调毫秒时钟。
///
/// 必须单调，不能用系统时间：提示条的过期时刻是「现在 + 3.2 秒」，
/// 而系统时间会被 NTP 校准、时区切换、用户手动改表动到 ——
/// 用系统时间的话，校准一次就可能让提示条立刻消失或者永远不消失。
/// Windows 用 GetTickCount64（本身就是单调的），Linux 用 CLOCK_MONOTONIC。
uint64_t monotonic_ms();

}  // namespace penhu::native
