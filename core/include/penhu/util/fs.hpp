#pragma once
// =============================================================================
//  penhu/util/fs.hpp
//  文件系统小工具。
//
//  存在的理由：账本文件的创建/备份/删除都需要跨平台且要能给出可读的错误。
//  std::filesystem 的异常版本会直接把错误信息变成 what()，而我们需要的是
//  「哪个路径、想干什么、系统为什么拒绝」。所以全部包成 Result。
//
//  路径统一用 UTF-8 的 std::string（Windows 下内部转宽字符），
//  因为项目其余部分（SQLite、JSON、HTTP）都用 UTF-8，边界只有这一处。
// =============================================================================

#include <cstdint>
#include <string>
#include <vector>
#include "penhu/util/result.hpp"

namespace penhu::fs {

bool exists(const std::string& path);
bool is_directory(const std::string& path);
bool is_regular_file(const std::string& path);

/// 递归创建目录（已存在则视为成功）
Status create_directories(const std::string& path);

/// 删除文件。文件不存在视为成功（幂等）。
Status remove_file(const std::string& path);

/// 删除目录及其内容。**调用方必须自己确认目标目录是我们自己的数据目录**，
/// 这个函数不做任何安全判断。
Status remove_all(const std::string& path);

/// 拷贝文件。目标存在会被覆盖。
Status copy_file(const std::string& from, const std::string& to, bool overwrite = true);

/// 改名/移动（同卷内原子）
Status rename(const std::string& from, const std::string& to);

/// 文件字节数；不存在返回 0
int64_t file_size(const std::string& path);

/// 列出目录下的普通文件名（不含路径，不递归，已排序）
Result<std::vector<std::string>> list_files(const std::string& dir);

/// 列出目录下的**全部条目**（文件与子目录，不含 . / ..，已排序）。
///
/// 为什么要单独一个函数而不是放宽 list_files：调用方对两者的期望完全不同。
/// list_files 的语义是「这个目录里有哪些文件」，如果某天它开始返回目录名，
/// 那些「遍历文件逐个处理」的代码就会静默地拿到一个目录名去读，
/// 报出来的是「打开失败」，而不是「你调错了函数」。
/// 需要递归/需要区分类型时，用 list_entries + is_directory。
Result<std::vector<std::string>> list_entries(const std::string& dir);

/// 拼路径，自动处理分隔符
std::string join(const std::string& a, const std::string& b);

/// 取文件名部分（含扩展名）
std::string filename(const std::string& path);

/// 取目录部分
std::string parent(const std::string& path);

/// 把相对路径解析到绝对路径（不做 realpath，只拼接 + 规范化）
std::string absolute(const std::string& path);

/// 当前进程可执行文件所在目录
std::string executable_dir();

/// 用户主目录（Windows: %USERPROFILE%，Unix: $HOME）
std::string home_dir();

/// "20260918-163500"，用于备份文件名
std::string timestamp_slug();

}  // namespace penhu::fs
