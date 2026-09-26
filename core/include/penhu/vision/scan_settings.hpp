// =============================================================================
//  penhu/vision/scan_settings.hpp
//  截图批量导入相关的设置项键名。
//
//  单独放一个头文件而不是塞进 llm/settings.hpp：那一个是「模型通道怎么配」，
//  这里是「截图从哪来、要不要自动记账」，两件事的变更原因完全不同。
// =============================================================================

#pragma once

namespace penhu::vision::settings {

/// 截图保存目录（绝对路径）。空 = 没设置。
inline constexpr const char* kKeyScanDir = "scan.dir";

/// 打开识图页时自动扫描该目录，把没处理过的图加进来并开始识别。
/// "1" / "0"
inline constexpr const char* kKeyScanAutoScan = "scan.auto_scan";

/// 识别成功后直接写入账本，不再逐张确认。
/// **默认关闭**：开了它，模型识别错的金额会静默进账本，用户只能事后去记录
/// 列表里找。这个风险该由用户显式接受，而不是我们替他默认打开。
/// "1" / "0"
inline constexpr const char* kKeyScanAutoApply = "scan.auto_apply";

/// 已处理文件的指纹列表（JSON 数组）。用来回答「这张图是不是已经识别/记账过了」。
/// 存在设置里而不是单独的文件：它是**每账号**的（同一个目录，alice 记过不等于
/// bob 记过），而设置表天然就是每账号一份，还顺带享受整库加密。
inline constexpr const char* kKeyScanSeen = "scan.seen";

/// 入账成功后，把原图移到**回收站**。
/// 默认关闭，而且是刻意的：
///   · 只对「已入账」的图生效 —— 识别失败的绝不动，否则用户连重试的机会都没有；
///   · 走回收站（SHFileOperationW + FOF_ALLOWUNDO）而不是 DeleteFile，
///     误删了还能还原。批量自动删除是不可逆操作，不该留下没有退路的路径。
/// "1" / "0"
inline constexpr const char* kKeyScanAutoClean = "scan.auto_clean";

/// 指纹列表上限。超过就丢最旧的 —— 这是防无限增长，不是功能需要。
inline constexpr int kScanSeenLimit = 500;

}  // namespace penhu::vision::settings

