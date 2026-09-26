#pragma once
// =============================================================================
//  penhu/vision/receipt_scan.hpp
//  支付截图识别：把一张支付/账单截图交给多模态模型，取回结构化支出草稿。
//
//  三条设计底线，都是钱相关的，值得写在最前面：
//
//  1) **绝不编造金额。**
//     模型如果没看清金额，允许它返回空字符串，这时我们报「未识别」而不是拿
//     别的数字顶上去。一个猜错的金额比「识别失败，请手填」糟糕得多 ——
//     前者会静默写进账本，后者用户立刻知道要自己动手。
//
//  2) **模型给的是「草稿」，不是「记账」。**
//     返回值必须经用户确认才写库。界面层负责这一步。
//
//  3) **不碰文件、不碰图片编解码。**
//     这里只接受已经准备好的 data URL。图片的读取 / 缩放 / 重编码依赖
//     WIC（Windows 组件），属于界面层职责；core 保持跨平台与零图片依赖，
//     这样这个模块可以用纯字符串做单元测试。
// =============================================================================

#include <cstdint>
#include <string>
#include <vector>

#include "penhu/domain/record.hpp"
#include "penhu/llm/provider.hpp"
#include "penhu/util/result.hpp"

namespace penhu::vision {

struct ScanRequest {
    /// "data:image/jpeg;base64,...."
    std::string image_data_url;
    /// 该账号现有的分类名（支出方向）。给模型一份候选表，比让它自由发挥准得多 ——
    /// 「星巴克」自由发挥会得到「咖啡」「饮品」「餐饮」三种答案，写进库就成了三个分类。
    std::vector<std::string> category_candidates;
    /// 模型没识别出日期时用它，格式 "2026-09-20"
    std::string today_iso;
};

struct ScanOutcome {
    bool        ok{false};
    std::string fail_reason;      // ok=false 时给用户看的可读原因

    int64_t     amount_minor{0};
    Direction   direction{Direction::Expense};
    std::string category_name;    // 模型给的分类名（已对齐候选表）
    std::string merchant;
    std::string date_iso;         // "2026-09-20"
    std::string note;
    double      confidence{0.0};  // 模型自报的把握，[0,1]

    std::string provider_label;   // "cloud-openai/gpt-4o" 之类，要显示给用户
    std::string raw_reply;        // 原始回复，排障用
    std::vector<std::string> notes;  // 例如「模型没给日期，用了今天」
};

/// 调多模态模型识别。cfg 必须是可用的配置（base_url / model 非空）。
Result<ScanOutcome> scan_with_vision(const ScanRequest& req, const llm::LlmConfig& cfg);

/// 解析模型回复。单独暴露出来是为了离线可测 ——
/// 真实模型回复里常见的 markdown 代码块、前后缀解释、字段缺失，
/// 光靠「真调一次模型」是测不全的。
ScanOutcome parse_scan_reply(const std::string& reply,
                             const std::vector<std::string>& category_candidates);

}  // namespace penhu::vision
