#include "penhu/domain/category.hpp"

namespace penhu {

bool Category::is_valid() const noexcept {
    // 同 Record::is_valid：categories 表没有 user_id 列，归属由「每用户一个
    // 独立加密库」保证。要求它非空会让自建分类永远失败。
    if (id.empty()) return false;
    if (name.empty() || name.size() > 32) return false;
    // 颜色必须是 "#RRGGBB"，前端直接塞进 CSS 变量；不做校验就等于开了个 XSS 口子
    if (color_hex.size() != 7 || color_hex[0] != '#') return false;
    for (size_t i = 1; i < 7; ++i) {
        const char c = color_hex[i];
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!hex) return false;
    }
    return true;
}

const std::vector<Category>& builtin_categories() {
    // id / user_id 留空，由 CategoryRepository::seed_defaults 落库时再分配。
    // 颜色取 Material Design 3 baseline 色板里的主色，保证图表和界面主题同源。
    //
    // 这套清单的取向（相对最初的 15 条做了两件事）：
    //   1) 粗粒度分类拆开：原来「交通」把打车、加油、公交全塞在一起，
    //      月底看占比时只能说「交通占 31%」，没法回答「是不是打车花多了」。
    //      现在拆成 8 条，代价是网格长一点，换来的是统计能落地。
    //   2) 补上原来完全没有的常见项：外卖、买菜、房租房贷、宽带、订阅、
    //      宠物、母婴、税费、保险、快递、二手变卖 —— 这些在真实账本里出现的
    //      频率不比「人情」低，缺了就只能往「其他」里塞。
    //
    // sort_order 是「网格里的显示顺序」，按使用频率从高到低排：
    // 记账时最常点的「餐饮」永远在第一个，不用翻。
    // 最后一个（999）必须是「其他」—— 截图识别在对不上任何候选时会退回它。
    static const std::vector<Category> kBuiltins = {
        // ---- 支出：吃喝 ----
        {"", "", "餐饮",     "restaurant",           "#FF7043", Direction::Expense,  10, true, true},
        {"", "", "外卖",     "takeout_dining",       "#FF8A65", Direction::Expense,  20, true, true},
        {"", "", "咖啡饮品", "local_cafe",           "#A1887F", Direction::Expense,  30, true, true},
        {"", "", "买菜",     "local_grocery_store",  "#9CCC65", Direction::Expense,  40, true, true},
        {"", "", "零食",     "icecream",             "#FFB74D", Direction::Expense,  50, true, true},
        // ---- 支出：出行 ----
        {"", "", "交通",     "directions_bus",       "#42A5F5", Direction::Expense,  60, true, true},
        {"", "", "打车",     "local_taxi",           "#29B6F6", Direction::Expense,  70, true, true},
        {"", "", "加油停车", "local_gas_station",    "#546E7A", Direction::Expense,  80, true, true},
        {"", "", "火车机票", "train",                "#5C6BC0", Direction::Expense,  90, true, true},
        // ---- 支出：买东西 ----
        {"", "", "购物",     "shopping_bag",         "#AB47BC", Direction::Expense, 100, true, true},
        {"", "", "服饰",     "checkroom",            "#BA68C8", Direction::Expense, 110, true, true},
        {"", "", "数码",     "devices",              "#7E57C2", Direction::Expense, 120, true, true},
        {"", "", "家居日用", "chair",                "#8D6E63", Direction::Expense, 130, true, true},
        {"", "", "美妆护理", "spa",                  "#F06292", Direction::Expense, 140, true, true},
        // ---- 支出：住 ----
        {"", "", "居住",     "home",                 "#26A69A", Direction::Expense, 150, true, true},
        {"", "", "房租房贷", "house",                "#00897B", Direction::Expense, 160, true, true},
        {"", "", "水电燃气", "bolt",                 "#FFA726", Direction::Expense, 170, true, true},
        {"", "", "物业维修", "build",                "#78909C", Direction::Expense, 180, true, true},
        // ---- 支出：通讯 ----------
        {"", "", "通讯",     "smartphone",           "#29B6F6", Direction::Expense, 190, true, true},
        {"", "", "宽带网络", "wifi",                 "#4FC3F7", Direction::Expense, 200, true, true},
        {"", "", "订阅服务", "subscriptions",        "#9575CD", Direction::Expense, 210, true, true},
        // ---- 支出：玩乐 ----
        {"", "", "娱乐",     "movie",                "#FFA726", Direction::Expense, 220, true, true},
        {"", "", "游戏",     "sports_esports",       "#FF7043", Direction::Expense, 230, true, true},
        {"", "", "旅行",     "flight",               "#26C6DA", Direction::Expense, 240, true, true},
        {"", "", "运动健身", "fitness_center",       "#66BB6A", Direction::Expense, 250, true, true},
        // ---- 支出：健康 ----
        {"", "", "医疗",     "local_hospital",       "#EF5350", Direction::Expense, 260, true, true},
        {"", "", "药品",     "medication",           "#E57373", Direction::Expense, 270, true, true},
        // ---- 支出：成长 ----
        {"", "", "学习",     "school",               "#5C6BC0", Direction::Expense, 280, true, true},
        {"", "", "书籍",     "menu_book",            "#7986CB", Direction::Expense, 290, true, true},
        {"", "", "办公",     "work",                 "#607D8B", Direction::Expense, 300, true, true},
        // ---- 支出：人情与家庭 ----
        {"", "", "人情往来", "card_giftcard",        "#EC407A", Direction::Expense, 310, true, true},
        {"", "", "送礼红包", "redeem",               "#F06292", Direction::Expense, 320, true, true},
        {"", "", "宠物",     "pets",                 "#A1887F", Direction::Expense, 330, true, true},
        {"", "", "母婴",     "child_friendly",       "#FFAB91", Direction::Expense, 340, true, true},
        // ---- 支出：固定与杂项 ----
        {"", "", "保险",     "shield",               "#8D6E63", Direction::Expense, 350, true, true},
        {"", "", "税费",     "account_balance",      "#795548", Direction::Expense, 360, true, true},
        {"", "", "金融手续费", "credit_card",        "#546E7A", Direction::Expense, 370, true, true},
        {"", "", "快递物流", "local_shipping",       "#90A4AE", Direction::Expense, 380, true, true},
        {"", "", "理发美容", "content_cut",          "#F48FB1", Direction::Expense, 390, true, true},
        {"", "", "公益捐赠", "volunteer_activism",   "#81C784", Direction::Expense, 400, true, true},
        {"", "", "其他",     "more_horiz",           "#78909C", Direction::Expense, 999, true, true},

        // ---- 收入 ----
        {"", "", "工资",     "payments",             "#66BB6A", Direction::Income,   10, true, true},
        {"", "", "奖金",     "emoji_events",         "#FFD54F", Direction::Income,   20, true, true},
        {"", "", "兼职",     "work_history",         "#9CCC65", Direction::Income,   30, true, true},
        {"", "", "理财收益", "trending_up",          "#26C6DA", Direction::Income,   40, true, true},
        {"", "", "利息分红", "savings",              "#4DB6AC", Direction::Income,   50, true, true},
        {"", "", "红包收入", "redeem",               "#FFCA28", Direction::Income,   60, true, true},
        {"", "", "报销",     "receipt_long",         "#AED581", Direction::Income,   70, true, true},
        {"", "", "退款",     "undo",                 "#81C784", Direction::Income,   80, true, true},
        {"", "", "二手变卖", "sell",                 "#A5D6A7", Direction::Income,   90, true, true},
        {"", "", "礼金",     "card_giftcard",        "#FFE082", Direction::Income,  100, true, true},
        {"", "", "其他收入", "savings",              "#8D6E63", Direction::Income,  999, true, true},
    };
    return kBuiltins;
}

}  // namespace penhu
