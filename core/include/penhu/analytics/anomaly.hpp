#pragma once
// =============================================================================
//  penhu/analytics/anomaly.hpp
//  异常支出识别。
//
//  这个模块是整个项目里最容易「看起来聪明、实际在瞎猜」的地方，
//  所以设计上强制了三件事：
//
//   1) 每个结论都带 method 字段，说明它是 3σ 还是 IQR 判出来的；
//   2) 报告里带 sample_size 和 reliable 标志，样本不足时界面必须降级展示；
//   3) limitations 里明写这套方法的已知失败模式，不允许只报「发现 3 笔异常」。
//
//  为什么两种方法都要：
//    3σ 假设数据近似正态。记账金额是典型的右偏分布（大部分小额 + 少量大额），
//    3σ 在样本少时会把正常的大额消费误报成异常，也可能因为样本里已经有异常值
//    而把 σ 撑大、反而漏报。
//    IQR 基于分位数，对离群值不敏感，样本少时更稳，但也更钝。
//  策略：样本足够(>=8)用 3σ；否则降级用 IQR，并在 method 字段里写明。
//
//  为什么做两轮：
//    第一轮阈值被异常值本身抬高。剔除首轮命中项后重算一次，
//    能救回那些「被自己掩盖掉」的异常。只做一轮等于变相漏报。
// =============================================================================

#include <cstdint>
#include <string>
#include <vector>

#include "penhu/domain/category.hpp"
#include "penhu/domain/record.hpp"
#include "penhu/util/result.hpp"

namespace penhu::analytics {

enum class AnomalyKind {
    SingleExpense,       // 单笔金额异常高
    DailyTotal,          // 某一天的支出总额异常高
    CategoryDailyTotal   // 某天某个分类的支出异常高
};

const char* to_string(AnomalyKind kind) noexcept;

struct Anomaly {
    AnomalyKind kind{AnomalyKind::SingleExpense};
    Date        date;

    std::string category_id;
    std::string category_name;
    std::string record_id;          // SingleExpense 才有

    int64_t amount_minor{0};
    double  threshold_minor{0.0};   // 触发阈值
    double  score{0.0};             // amount / threshold，>1 即命中
    std::string method;             // "3sigma" / "iqr"
    std::string explanation;        // 给用户看的一句话
};

struct AnomalyReport {
    std::vector<Anomaly> items;          // 按 amounts 降序
    int64_t              sample_size{0}; // 参与判定的支出笔数
    int64_t              scanned_days{0};
    bool                 reliable{false};// 样本是否够支撑统计判定
    std::string          method_summary; // 「用了什么阈值、基于多少样本」
    std::vector<std::string> limitations;
    /// 因为超过输出上限而没被列出的命中项数（不能悄悄丢掉）
    int64_t suppressed_count{0};
};

/// 检测异常。categories 用于回填分类名。
Result<AnomalyReport> detect_anomalies(const std::vector<Record>& records,
                                       const std::vector<Category>& categories,
                                       const DateRange& range,
                                       int max_items = 20);

}  // namespace penhu::analytics
