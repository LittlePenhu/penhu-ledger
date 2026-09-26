#pragma once
// =============================================================================
//  penhu/storage/migrations.hpp
//  Schema 迁移。
//
//  两个独立的库，各自维护自己的 user_version：
//    users.db   明文，schema 极小，几乎不会变
//    <uuid>.db  每用户一个，SQLCipher 加密，是真正会演进的那个
//
//  迁移策略：顺序执行 + 幂等。每步先判断 user_version，再应用，再写回版本号，
//  全过程在一个事务里。所以任何一步失败都不会留下「一半新一半旧」的 schema——
//  那是最难救的状态。
// =============================================================================

#include "penhu/storage/database.hpp"

namespace penhu::storage {

/// 当前期望的 schema 版本。只增不减；改结构必须 +1 并补一条迁移步骤。
constexpr int kUsersSchemaVersion  = 1;
constexpr int kLedgerSchemaVersion = 2;

/// 把 users.db 升到最新版本
Status migrate_users_db(Database& db);

/// 把某个用户的加密库升到最新版本
Status migrate_ledger_db(Database& db);

}  // namespace penhu::storage
