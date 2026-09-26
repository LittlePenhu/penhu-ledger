#pragma once
// =============================================================================
//  native/app/image_util.hpp
//  本地图片 → 多模态接口可用的 data URL。
//
//  为什么这件事不能省：
//    · 手机支付截图常见 3-5 MB（PNG），base64 之后体积还要涨约 1/3；
//      多数多模态接口对单图有 4 MB 左右的硬限制，原图直发基本就是 400。
//    · 缩放还能显著降低识别延迟和 token 花费，而支付截图的关键信息
//      （金额、商户、时间）在一千多像素宽度下完全清晰。
//
//  放在 native 层是因为要用 WIC（Windows 组件）；core 的 vision 模块
//  只接受成品 data URL，保持零图片依赖、可离线单测。
// =============================================================================

#include <string>

namespace penhu::native {

/// 读图 → 等比缩放到最长边 <= max_edge → 编码 JPEG → 生成 data URL。
/// 失败时 err 里给可读原因（中文，直接可以显示给用户）。
bool make_image_data_url(const std::wstring& path, int max_edge, std::string& out_url,
                         std::wstring* err);

/// 只要尺寸（用于界面显示「已选 1080×2400」这类信息）
bool probe_image_size(const std::wstring& path, int& width, int& height);

}  // namespace penhu::native
