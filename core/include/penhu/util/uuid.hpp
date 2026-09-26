#pragma once
// =============================================================================
//  penhu/util/uuid.hpp
//  UUID v4 生成 + 校验。用于用户 id / 记录 id / 分类 id。
//
//  为什么不用自增整数做主键：
//   1) 服务端多用户模式下，自增 ID 会泄露「系统里一共有多少条记录」；
//   2) 前端要做离线暂存再上行的话，UUID 可以在客户端先定 ID；
//   3) 加密库里 ID 是明文的，UUID 至少不携带顺序信息。
// =============================================================================

#include <string>
#include <string_view>

namespace penhu {

/// 生成 RFC 4122 v4 UUID，形如 "3f2504e0-4f89-41d3-9a0c-0305e82c3301"
std::string new_uuid();

/// 严格校验 36 字符带连字符的 v4 UUID
bool is_valid_uuid(std::string_view text);

/// 去掉连字符（数据库里统一存 32 位小写十六进制，省空间也好比对）
std::string uuid_to_compact(std::string_view uuid);

/// 从 compact 形式还原成带连字符形式
std::string uuid_from_compact(std::string_view compact);

}  // namespace penhu
