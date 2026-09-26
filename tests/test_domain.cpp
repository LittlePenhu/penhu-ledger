// =============================================================================
//  tests/test_domain.cpp
//  领域层单测：金额、日期、UUID、账号策略、记录校验、预置分类。
//
//  重点覆盖两类东西：
//   1) 会静默出错的地方（金额的浮点、日期的闰年/月末）——错了不会崩，但账会错；
//   2) 边界（溢出、空输入、非法字符）——这些是「用户手滑」的真实路径。
//
//  写法约定：宏参数里构造日期一律用 tt::d(y, m, d)，不要写 Date{y, m, d}。
//  原因见 tiny_test.hpp 文件头（花括号不保护宏参数里的逗号）。
// =============================================================================

#include "tiny_test.hpp"

#include "penhu/domain/category.hpp"
#include "penhu/domain/date.hpp"
#include "penhu/domain/money.hpp"
#include "penhu/domain/record.hpp"
#include "penhu/domain/user.hpp"
#include "penhu/util/uuid.hpp"

using namespace penhu;

// -----------------------------------------------------------------------------
//  Money
// -----------------------------------------------------------------------------
TT_TEST(Money, 解析常规输入) {
    TT_EQ(Money::parse("12.34").value().minor_units(), 1234);
    TT_EQ(Money::parse("12").value().minor_units(), 1200);
    TT_EQ(Money::parse("0").value().minor_units(), 0);
    TT_EQ(Money::parse("0.1").value().minor_units(), 10);      // 一位小数补零
    TT_EQ(Money::parse(".5").value().minor_units(), 50);       // 省略整数部分
    TT_EQ(Money::parse("-5").value().minor_units(), -500);
    TT_EQ(Money::parse(" 8.80 ").value().minor_units(), 880);  // 前后空格
}

TT_TEST(Money, 解析带货币符号与千分位) {
    TT_EQ(Money::parse("¥12.34").value().minor_units(), 1234);
    TT_EQ(Money::parse("￥12.34").value().minor_units(), 1234);   // 全角￥
    TT_EQ(Money::parse("$99.99").value().minor_units(), 9999);
    TT_EQ(Money::parse("1,234.56").value().minor_units(), 123456);
    TT_EQ(Money::parse("1,234,567").value().minor_units(), 123456700);
}

TT_TEST(Money, 拒绝超过两位小数而不是四舍五入) {
    // 这条是刻意的：静默改掉用户输入的金额，是记账软件最不可接受的 bug
    TT_IS_ERR_CODE(Money::parse("12.345"), ErrorCode::InvalidArgument);
    TT_IS_ERR_CODE(Money::parse("0.001"), ErrorCode::InvalidArgument);
}

TT_TEST(Money, 拒绝垃圾输入) {
    TT_IS_ERR(Money::parse(""));
    TT_IS_ERR(Money::parse("   "));
    TT_IS_ERR(Money::parse("abc"));
    TT_IS_ERR(Money::parse("12.3.4"));
    TT_IS_ERR(Money::parse("1e5"));       // 不支持科学计数法
    TT_IS_ERR(Money::parse("12元"));
    TT_IS_ERR(Money::parse("¥"));
}

TT_TEST(Money, 溢出保护) {
    TT_IS_ERR(Money::parse("99999999999999999999"));       // 20 位整数
    TT_IS_ERR(Money::parse("999999999999999999.99"));
    TT_IS_OK(Money::parse("92233720368547758.07"));        // INT64_MAX 分，边界内应成功
}

TT_TEST(Money, 格式化) {
    TT_EQ(Money::from_minor(1234).to_plain_string(), std::string("12.34"));
    TT_EQ(Money::from_minor(-1234).to_plain_string(), std::string("-12.34"));
    TT_EQ(Money::from_minor(5).to_plain_string(), std::string("0.05"));
    TT_EQ(Money::from_minor(123456).to_grouped_string(), std::string("¥1,234.56"));
    TT_EQ(Money::from_minor(-123456).to_grouped_string(), std::string("¥-1,234.56"));
    TT_EQ(Money::from_minor(100000000).to_grouped_string(), std::string("¥1,000,000.00"));
}

TT_TEST(Money, 序列化往返) {
    const int64_t samples[] = {0, 1, 99, 100, 12345, -12345, 999999999999LL};
    for (int64_t v : samples) {
        Money m = Money::from_minor(v);
        auto back = Money::deserialize(m.serialize());
        TT_CHECK(back.has_value());
        if (back.has_value()) TT_EQ(back->minor_units(), v);
    }
    // INT64_MIN 取负会溢出，单独验一下不崩且符号正确
    TT_EQ(Money::from_minor(INT64_MIN).to_plain_string().substr(0, 1), std::string("-"));
}

TT_TEST(Money, 加减与缩放) {
    Money a = Money::from_minor(1000);
    Money b = Money::from_minor(250);
    TT_EQ((a + b).minor_units(), 1250);
    TT_EQ((a - b).minor_units(), 750);
    TT_EQ((-a).minor_units(), -1000);

    // 缩放四舍五入到分
    TT_EQ(Money::from_minor(1000).scaled(0.333).minor_units(), 333);
    TT_EQ(Money::from_minor(100).scaled(0.005).minor_units(), 1);   // 0.5 分 -> 1 分
    TT_EQ(Money::from_minor(100).scaled(-1.0).minor_units(), -100);
}

// -----------------------------------------------------------------------------
//  Date
// -----------------------------------------------------------------------------
TT_TEST(Date, 闰年与月长) {
    TT_CHECK(Date::create(2024, 2, 29).is_ok());       // 闰年 2/29 存在
    TT_IS_ERR(Date::create(2023, 2, 29));              // 平年没有 2/29
    TT_IS_ERR(Date::create(2100, 2, 29));              // 整百年不被 400 整除 -> 不是闰年
    TT_CHECK(Date::create(2000, 2, 29).is_ok());       // 2000 是闰年

    TT_EQ(tt::d(2024, 2, 1).days_in_month(), 29);
    TT_EQ(tt::d(2023, 2, 1).days_in_month(), 28);
    TT_EQ(tt::d(2026, 9, 1).days_in_month(), 30);
    TT_EQ(tt::d(2026, 1, 1).days_in_month(), 31);
}

TT_TEST(Date, 解析格式) {
    TT_EQ(Date::parse("2026-09-18").value().to_string(), std::string("2026-09-18"));
    TT_EQ(Date::parse("2026/9/8").value().to_string(), std::string("2026-09-08"));
    TT_EQ(Date::parse("20260918").value().to_string(), std::string("2026-09-18"));
    TT_EQ(Date::parse("2026.9.8").value().to_string(), std::string("2026-09-08"));
    TT_IS_ERR(Date::parse(""));
    TT_IS_ERR(Date::parse("2026-13-01"));
    TT_IS_ERR(Date::parse("2026-02-30"));
    TT_IS_ERR(Date::parse("26-09-18"));      // 两位年份不猜
    TT_IS_ERR(Date::parse("2026年9月18日"));
}

TT_TEST(Date, epoch_days_往返) {
    const Date samples[] = {tt::d(1970, 1, 1), tt::d(2026, 9, 18), tt::d(1900, 1, 1),
                            tt::d(2000, 2, 29), tt::d(9999, 12, 31)};
    for (const Date& date : samples) {
        TT_EQ(Date::from_epoch_days(date.to_epoch_days()), date);
    }
    TT_EQ(tt::d(1970, 1, 1).to_epoch_days(), 0);
    TT_EQ(tt::d(1970, 1, 2).to_epoch_days(), 1);
    TT_EQ(tt::d(1969, 12, 31).to_epoch_days(), -1);
}

TT_TEST(Date, 星期计算) {
    TT_EQ(tt::d(1970, 1, 1).day_of_week(), 4);    // 周四
    TT_EQ(tt::d(2000, 1, 1).day_of_week(), 6);    // 周六
    TT_EQ(tt::d(2026, 9, 18).day_of_week(), 5);   // 周五
    TT_EQ(tt::d(2026, 9, 18).weekday_zh(), std::string("周五"));
    TT_CHECK(tt::d(2026, 9, 19).is_weekend());    // 周六
    TT_CHECK(tt::d(2026, 9, 20).is_weekend());    // 周日
    TT_CHECK(!tt::d(2026, 9, 18).is_weekend());
}

TT_TEST(Date, ISO_周编号跨年) {
    // 2026-01-01 是周四 -> 属于 2026 年第 1 周
    TT_EQ(tt::d(2026, 1, 1).iso_week(), 1);
    // 该周的周一落在 2025-12-29，它归属 2026 年第 1 周
    TT_EQ(tt::d(2025, 12, 29).iso_week(), 1);
    TT_EQ(tt::d(2025, 12, 28).iso_week(), 52);   // 前一天属于 2025 第 52 周
    TT_EQ(tt::d(2026, 9, 18).iso_week(), 38);
}

TT_TEST(Date, 周起始与区间) {
    TT_EQ(tt::d(2026, 9, 18).week_start(), tt::d(2026, 9, 14));   // 周五 -> 本周一
    TT_EQ(tt::d(2026, 9, 14).week_start(), tt::d(2026, 9, 14));   // 周一 -> 自己
    TT_EQ(tt::d(2026, 9, 20).week_start(), tt::d(2026, 9, 14));   // 周日 -> 本周一

    auto r = week_range(tt::d(2026, 9, 18));
    TT_EQ(r.from, tt::d(2026, 9, 14));
    TT_EQ(r.to, tt::d(2026, 9, 20));

    auto m = month_range(tt::d(2026, 2, 15));
    TT_EQ(m.from, tt::d(2026, 2, 1));
    TT_EQ(m.to, tt::d(2026, 2, 28));

    auto n = last_n_days(tt::d(2026, 9, 18), 7);
    TT_EQ(n.from, tt::d(2026, 9, 12));
    TT_EQ(n.to, tt::d(2026, 9, 18));
}

TT_TEST(Date, 加月时的月末夹取) {
    // 1/31 + 1 个月：2 月没有 31 号，必须夹到实际最后一天
    TT_EQ(tt::d(2026, 1, 31).add_months(1), tt::d(2026, 2, 28));
    TT_EQ(tt::d(2024, 1, 31).add_months(1), tt::d(2024, 2, 29));   // 闰年
    TT_EQ(tt::d(2026, 3, 31).add_months(1), tt::d(2026, 4, 30));
    TT_EQ(tt::d(2026, 1, 15).add_months(-1), tt::d(2025, 12, 15));
    TT_EQ(tt::d(2026, 1, 15).add_months(12), tt::d(2027, 1, 15));
}

TT_TEST(Date, 区间生成与上限保护) {
    auto days = date_range_inclusive(tt::d(2026, 9, 1), tt::d(2026, 9, 5));
    TT_EQ(days.size(), size_t{5});
    TT_EQ(days.front(), tt::d(2026, 9, 1));
    TT_EQ(days.back(), tt::d(2026, 9, 5));

    TT_EQ(date_range_inclusive(tt::d(2026, 9, 5), tt::d(2026, 9, 1)).size(), size_t{0});
    // 超长区间直接返回空，避免把内存吃光
    TT_EQ(date_range_inclusive(tt::d(1900, 1, 1), tt::d(2100, 1, 1)).size(), size_t{0});
}

TT_TEST(Date, 时间戳转本地日历日) {
    const int64_t now = timeutil::now_epoch_seconds();
    TT_CHECK(now > 1700000000LL);                   // 晚于 2023-11
    const Date today = Date::today();
    TT_CHECK(today.is_valid());
    TT_EQ(timeutil::to_local_date(timeutil::local_date_start_epoch(today)), today);
    // 本机在中国，偏移应当是 8 小时
    TT_EQ(timeutil::local_utc_offset_seconds(), 8 * 3600);
}

// -----------------------------------------------------------------------------
//  UUID
// -----------------------------------------------------------------------------
TT_TEST(Uuid, 生成与校验) {
    const std::string a = new_uuid();
    const std::string b = new_uuid();
    TT_EQ(a.size(), size_t{36});
    TT_CHECK(is_valid_uuid(a));
    TT_CHECK(is_valid_uuid(b));
    TT_CHECK(a != b);                                // 随机性
    TT_CHECK(!is_valid_uuid(""));
    TT_CHECK(!is_valid_uuid("not-a-uuid"));
    TT_CHECK(!is_valid_uuid(a.substr(0, 35)));
    TT_EQ(a[14], '4');                               // 版本位必须是 4
}

TT_TEST(Uuid, 紧凑形式往返) {
    const std::string u = new_uuid();
    const std::string c = uuid_to_compact(u);
    TT_EQ(c.size(), size_t{32});
    TT_EQ(uuid_from_compact(c), u);
    TT_EQ(uuid_to_compact(u), c);
}

// -----------------------------------------------------------------------------
//  账号策略
// -----------------------------------------------------------------------------
TT_TEST(UserPolicy, 用户名规则) {
    TT_IS_OK(user_policy::validate_username("penhu"));
    TT_IS_OK(user_policy::validate_username("Pen_Hu-01"));
    TT_IS_ERR(user_policy::validate_username("ab"));                   // 太短
    TT_IS_ERR(user_policy::validate_username(std::string(33, 'a')));   // 太长
    TT_IS_ERR(user_policy::validate_username("_penhu"));               // 不能以符号开头
    TT_IS_ERR(user_policy::validate_username("喷壶"));                  // 只允许 ASCII
    TT_IS_ERR(user_policy::validate_username("pen hu"));               // 空格
    TT_IS_ERR(user_policy::validate_username("pen@hu"));
}

TT_TEST(UserPolicy, 密码规则) {
    // 8 位纯小写只有一类字符，按规则应当被拒——它可能就是个常见单词
    TT_IS_ERR(user_policy::validate_password("abcdefgh"));
    TT_IS_ERR(user_policy::validate_password("12345678"));       // 纯数字同样一类

    TT_IS_OK(user_policy::validate_password("abcdefg1"));        // 字母 + 数字
    TT_IS_OK(user_policy::validate_password("Abcdefgh"));        // 小写 + 大写
    TT_IS_OK(user_policy::validate_password("abcd!@#$"));        // 字母 + 符号
    TT_IS_OK(user_policy::validate_password("penhu-2026-kdf"));  // 真实用例

    TT_IS_ERR(user_policy::validate_password("abc1"));                 // 太短
    TT_IS_ERR(user_policy::validate_password(""));                     // 空
    TT_IS_ERR(user_policy::validate_password(std::string(129, 'a')));  // 超长（防 DoS）
}

TT_TEST(UserPolicy, 用户名归一化) {
    TT_EQ(user_policy::normalize_username("PenHu"), std::string("penhu"));
    TT_EQ(user_policy::normalize_username("PEN_HU-01"), std::string("pen_hu-01"));
}

// -----------------------------------------------------------------------------
//  记录校验
// -----------------------------------------------------------------------------
TT_TEST(Record, 入参校验) {
    RecordInput ok;
    ok.amount = Money::from_minor(1000);
    ok.direction = Direction::Expense;
    ok.category_id = "abc";
    ok.date = tt::d(2026, 9, 18);
    TT_IS_OK(ok.validate());

    {
        RecordInput zero = ok;
        zero.amount = Money::from_minor(0);
        TT_IS_ERR(zero.validate());                              // 金额必须为正
    }
    {
        RecordInput negative = ok;
        negative.amount = Money::from_minor(-100);
        TT_IS_ERR(negative.validate());
    }
    {
        RecordInput too_big = ok;
        too_big.amount = Money::from_minor(200000000LL * 100LL); // 2 亿
        TT_IS_ERR(too_big.validate());
    }
    {
        RecordInput no_cat = ok;
        no_cat.category_id.clear();
        TT_IS_ERR(no_cat.validate());
    }
    {
        RecordInput long_note = ok;
        long_note.note = std::string(501, 'x');
        TT_IS_ERR(long_note.validate());
    }
    {
        RecordInput bad_date = ok;
        bad_date.date = tt::d(2026, 13, 1);
        TT_IS_ERR(bad_date.validate());
    }
}

TT_TEST(Record, 方向序列化) {
    TT_EQ(std::string(to_string(Direction::Expense)), std::string("expense"));
    TT_EQ(std::string(to_string(Direction::Income)), std::string("income"));
    TT_CHECK(parse_direction("expense") == Direction::Expense);
    TT_CHECK(parse_direction("收入") == Direction::Income);
    TT_CHECK(!parse_direction("xxx").has_value());
    TT_EQ(std::string(direction_label_zh(Direction::Expense)), std::string("支出"));
}

// -----------------------------------------------------------------------------
//  预置分类
// -----------------------------------------------------------------------------
TT_TEST(Category, 预置分类合法且不重复) {
    const auto& builtins = builtin_categories();
    TT_CHECK(builtins.size() >= 15);

    int expense_count = 0;
    int income_count = 0;

    for (size_t i = 0; i < builtins.size(); ++i) {
        const Category& c = builtins[i];
        TT_EQ(c.color_hex.size(), size_t{7});
        TT_EQ(c.color_hex[0], '#');
        TT_CHECK(!c.name.empty());
        TT_CHECK(!c.icon.empty());
        TT_CHECK(c.is_active);

        (c.direction == Direction::Expense ? expense_count : income_count)++;

        // 同名同方向不允许重复，否则统计图会把「餐饮」劈成两块
        for (size_t j = i + 1; j < builtins.size(); ++j) {
            const bool dup = builtins[j].name == c.name &&
                             builtins[j].direction == c.direction;
            TT_CHECK_MSG(!dup, "重复的预置分类: " + c.name);
        }
    }

    TT_CHECK(expense_count > 0);
    TT_CHECK(income_count > 0);
}
