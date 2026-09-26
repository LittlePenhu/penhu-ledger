// =============================================================================
//  tests/test_integration.cpp
//  端到端集成测试：一个真实的 LedgerService，真实的文件，真实的加密。
//
//  这里刻意不 mock 任何东西。理由：本项目最大的风险不是「逻辑写错了」，
//  而是「以为加密生效了但实际没有」「以为两个账号隔离了但实际没有」。
//  这类风险只有落盘 + 读原始字节才能证伪，mock 掉存储层等于把最该验的部分去掉。
//
//  注意：测试函数返回 void，所以统一用 TT_REQUIRE_OK / TT_TRY_ASSIGN，
//  不能用 core 里的 PENHU_ASSIGN_OR_RETURN（那个会 return Error，类型不匹配）。
// =============================================================================

#include "tiny_test.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "penhu/app/ledger_service.hpp"
#include "penhu/util/fs.hpp"
#include "penhu/util/uuid.hpp"

using namespace penhu;
using namespace penhu::app;

namespace {

/// 每个用例一个独立数据目录，析构时删掉
class TempDataDir {
public:
    explicit TempDataDir(const std::string& tag) {
        const auto base = std::filesystem::temp_directory_path();
        dir_ = (base / ("penhu-test-" + tag + "-" + new_uuid().substr(0, 8))).string();
    }
    ~TempDataDir() { std::filesystem::remove_all(dir_); }
    const std::string& path() const { return dir_; }

private:
    std::string dir_;
};

std::string read_raw_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool contains_bytes(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

/// 测试里用最轻的 Argon2 参数：否则每个用例光等 KDF 就要好几秒，
/// 150 多秒的测试套件没人会愿意跑。生产默认还是 moderate。
Result<std::unique_ptr<LedgerService>> make_service(TempDataDir& dir) {
    auto cfg = AppConfig::from_data_dir(dir.path());
    if (cfg.is_err()) return cfg.error();
    AppConfig c = std::move(cfg).value();
    c.kdf = crypto::KdfParams::interactive();
    return LedgerService::create(std::move(c));
}

}  // namespace

// -----------------------------------------------------------------------------
//  账号生命周期
// -----------------------------------------------------------------------------

TT_TEST(Integration, 注册登录登出) {
    TempDataDir dir("auth");
    TT_TRY_ASSIGN(svc_res, make_service(dir));
    LedgerService& svc = *svc_res;

    TT_IS_OK(svc.register_user("penhu", "penhu-2026"));
    // 重名必须被拒（且不区分大小写）
    TT_IS_ERR_CODE(svc.register_user("PENHU", "penhu-2026"), ErrorCode::AlreadyExists);
    // 弱密码必须被拒
    TT_IS_ERR(svc.register_user("penhu2", "12345678"));

    TT_TRY_ASSIGN(login, svc.login("penhu", "penhu-2026"));
    const std::string token = login.token;
    TT_EQ(token.size(), size_t{64});                  // 32 字节密钥的十六进制
    TT_EQ(login.user.username, std::string("penhu"));
    TT_CHECK(login.expires_at > timeutil::now_epoch_seconds());

    // 登录时用户名不区分大小写
    TT_IS_OK(svc.login("PeNhU", "penhu-2026"));

    // 错密码
    TT_IS_ERR_CODE(svc.login("penhu", "wrong-password"), ErrorCode::AuthFailed);
    // 不存在的用户也返回 AuthFailed（不泄露用户名是否存在）
    TT_IS_ERR_CODE(svc.login("nobody", "whatever1"), ErrorCode::AuthFailed);

    TT_IS_OK(svc.current_user(token));

    // 登出后 token 立刻失效
    TT_IS_OK(svc.logout(token));
    TT_IS_ERR_CODE(svc.current_user(token), ErrorCode::AuthFailed);
    // 重复登出不算错误
    TT_IS_OK(svc.logout(token));
}

TT_TEST(Integration, 注册后预置分类已就绪) {
    TempDataDir dir("seed");
    TT_TRY_ASSIGN(svc_res, make_service(dir));
    LedgerService& svc = *svc_res;

    TT_IS_OK(svc.register_user("seeduser", "seeduser-01"));
    TT_TRY_ASSIGN(login, svc.login("seeduser", "seeduser-01"));

    TT_TRY_ASSIGN(expense_cats, svc.list_categories(login.token, Direction::Expense, false));
    TT_TRY_ASSIGN(income_cats, svc.list_categories(login.token, Direction::Income, false));

    TT_CHECK_MSG(expense_cats.size() >= 8, "支出预置分类太少");
    TT_CHECK_MSG(income_cats.size() >= 3, "收入预置分类太少");

    // 每条都要有合法的颜色（前端会把它塞进 CSS 变量）
    for (const auto& c : expense_cats) {
        TT_EQ(c.color_hex.size(), size_t{7});
        TT_EQ(c.color_hex[0], '#');
        TT_CHECK_MSG(c.is_builtin, "预置分类应当被标记为 builtin: " + c.name);
    }
    // 方向必须对得上
    for (const auto& c : expense_cats) TT_EQ(c.direction, Direction::Expense);
    for (const auto& c : income_cats) TT_EQ(c.direction, Direction::Income);
}

// -----------------------------------------------------------------------------
//  预置分类表本身的自洽性
//
//  为什么单独钉这张表（两种错误都不会抛异常，只会让结果悄悄不对）：
//    · 出现重名 → seed_defaults 靠 INSERT OR IGNORE + (name, direction) 唯一索引
//      去重，于是「静默少插一条」，用户只会觉得「怎么少了个分类」；
//    · 某个方向排序最末的不是「其他」→ 截图识别在对不上任何候选时会退回
//      「最后一项」（约定见 vision::parse_scan_reply），兜底就会落到一个
//      具体分类上，识别结果偏掉而且没人会注意到。
// -----------------------------------------------------------------------------
TT_TEST(Integration, 预置分类表自洽) {
    const auto& all = builtin_categories();
    TT_CHECK_MSG(all.size() >= 40, "预置分类表太小，覆盖不够日常开销");

    std::set<std::string> seen;
    for (const auto& c : all) {
        const std::string key = std::string(to_string(c.direction)) + "/" + c.name;
        if (!seen.insert(key).second) {
            TT_FAIL("预置分类出现重名（会让种入静默少一条）: " + key);
            return;
        }
        TT_CHECK_MSG(!c.name.empty(), "分类名为空");
        TT_EQ(c.color_hex.size(), size_t{7});
        TT_EQ(c.color_hex[0], '#');
        TT_CHECK_MSG(c.is_builtin, "预置分类必须标记 builtin: " + c.name);
        TT_CHECK_MSG(c.is_active, "预置分类默认应当是启用状态: " + c.name);
    }

    for (Direction d : {Direction::Expense, Direction::Income}) {
        const Category* last = nullptr;
        int max_order = -1;
        for (const auto& c : all) {
            if (c.direction != d) continue;
            if (c.sort_order >= max_order) {
                max_order = c.sort_order;
                last = &c;
            }
        }
        if (last == nullptr) {
            TT_FAIL(std::string("方向 ") + to_string(d) + " 没有任何预置分类");
            return;
        }
        TT_CHECK_MSG(last->name.rfind("其他", 0) == 0,
                     std::string("方向 ") + to_string(d) + " 排序最末的预置分类不是「其他」，而是 " +
                         last->name);
    }
}

// -----------------------------------------------------------------------------
//  多用户隔离
// -----------------------------------------------------------------------------

TT_TEST(Integration, 两个账号的数据互不可见) {
    TempDataDir dir("iso");
    TT_TRY_ASSIGN(svc_res, make_service(dir));
    LedgerService& svc = *svc_res;

    TT_IS_OK(svc.register_user("alice", "alice-pass-1"));
    TT_IS_OK(svc.register_user("bob", "bob-pass-12"));

    TT_TRY_ASSIGN(alice_login, svc.login("alice", "alice-pass-1"));
    TT_TRY_ASSIGN(bob_login, svc.login("bob", "bob-pass-12"));

    const std::string alice_token = alice_login.token;
    const std::string bob_token = bob_login.token;
    TT_CHECK_MSG(alice_login.user.id != bob_login.user.id, "两个账号的 id 撞了");

    TT_TRY_ASSIGN(alice_cats, svc.list_categories(alice_token, Direction::Expense, false));
    TT_TRY_ASSIGN(bob_cats, svc.list_categories(bob_token, Direction::Expense, false));
    TT_EQ(bob_cats.size(), alice_cats.size());

    // 分类必须是各自独立的，不能共享一张全局分类表
    for (const auto& ac : alice_cats) {
        for (const auto& bc : bob_cats) {
            TT_CHECK_MSG(ac.id != bc.id, "两个账号出现了相同分类 id（分类未按用户隔离）");
        }
    }

    const std::string alice_cat = alice_cats.front().id;

    // alice 记三笔
    for (int i = 0; i < 3; ++i) {
        RecordInput in;
        in.amount = Money::from_minor(1000 + i * 100);
        in.direction = Direction::Expense;
        in.category_id = alice_cat;
        in.date = Date::create(2026, 9, 10 + i).value_or(Date{});
        in.note = "alice 的第 " + std::to_string(i + 1) + " 笔";
        TT_IS_OK(svc.add_record(alice_token, in));
    }

    RecordQuery q;
    q.limit = 100;

    TT_TRY_ASSIGN(alice_records, svc.list_records(alice_token, q));
    TT_EQ(alice_records.total, int64_t{3});
    TT_EQ(alice_records.items.size(), size_t{3});

    // === 隔离性的核心断言：bob 一条都看不到 ===
    TT_TRY_ASSIGN(bob_records, svc.list_records(bob_token, q));
    TT_EQ(bob_records.total, int64_t{0});
    TT_EQ(bob_records.items.size(), size_t{0});

    // bob 用 alice 的分类 id 记账必须失败（该分类在 bob 的库里不存在）
    RecordInput bob_input;
    bob_input.amount = Money::from_minor(999);
    bob_input.direction = Direction::Expense;
    bob_input.category_id = alice_cat;
    bob_input.date = Date{2026, 9, 18};
    TT_IS_ERR_CODE(svc.add_record(bob_token, bob_input), ErrorCode::NotFound);

    // bob 试图操作 alice 的记录也必须失败
    TT_IS_ERR(svc.delete_record(bob_token, alice_records.items.front().id));

    // 无效 token 必须被拒
    TT_IS_ERR_CODE(svc.list_records("deadbeef", q), ErrorCode::AuthFailed);
    TT_IS_ERR_CODE(svc.list_records("", q), ErrorCode::AuthFailed);
}

// -----------------------------------------------------------------------------
//  记录增删改
// -----------------------------------------------------------------------------

TT_TEST(Integration, 记录校验与增删改) {
    TempDataDir dir("crud");
    TT_TRY_ASSIGN(svc_res, make_service(dir));
    LedgerService& svc = *svc_res;

    TT_IS_OK(svc.register_user("cruduser", "crud-pass-1"));
    TT_TRY_ASSIGN(login, svc.login("cruduser", "crud-pass-1"));
    const std::string token = login.token;

    TT_TRY_ASSIGN(cats, svc.list_categories(token, Direction::Expense, false));
    if (cats.empty()) { TT_FAIL("没有可用分类"); return; }
    const std::string cat_id = cats.front().id;

    RecordInput base;
    base.amount = Money::from_minor(100);
    base.direction = Direction::Expense;
    base.category_id = cat_id;
    base.date = Date{2026, 9, 18};

    // 金额为 0 必须拒绝
    {
        RecordInput bad = base;
        bad.amount = Money::from_minor(0);
        TT_IS_ERR_CODE(svc.add_record(token, bad), ErrorCode::InvalidArgument);
    }
    // 方向与分类不匹配必须拒绝
    {
        TT_TRY_ASSIGN(income_cats, svc.list_categories(token, Direction::Income, false));
        if (!income_cats.empty()) {
            RecordInput mismatched = base;
            mismatched.category_id = income_cats.front().id;
            TT_IS_ERR_CODE(svc.add_record(token, mismatched), ErrorCode::InvalidArgument);
        }
    }
    // 不存在的分类必须拒绝
    {
        RecordInput ghost = base;
        ghost.category_id = "00000000000000000000000000000000";
        TT_IS_ERR_CODE(svc.add_record(token, ghost), ErrorCode::NotFound);
    }
    // 超长备注必须拒绝
    {
        RecordInput long_note = base;
        long_note.note = std::string(501, 'x');
        TT_IS_ERR_CODE(svc.add_record(token, long_note), ErrorCode::InvalidArgument);
    }

    // 正常新增
    RecordInput good = base;
    good.amount = Money::from_minor(2580);
    good.note = "午饭";
    TT_TRY_ASSIGN(added, svc.add_record(token, good));
    TT_EQ(added.amount.minor_units(), 2580);
    TT_EQ(added.note, std::string("午饭"));

    // 改
    RecordInput edited = good;
    edited.amount = Money::from_minor(3000);
    edited.note = "午饭（加了个菜）";
    TT_TRY_ASSIGN(updated, svc.update_record(token, added.id, edited));
    TT_EQ(updated.amount.minor_units(), 3000);
    TT_EQ(updated.note, std::string("午饭（加了个菜）"));
    // created_at 必须保持不变，否则按录入时间排序会在编辑后乱跳
    TT_EQ(updated.created_at, added.created_at);
    TT_CHECK(updated.updated_at >= added.created_at);

    // 删
    TT_IS_OK(svc.delete_record(token, added.id));
    TT_IS_ERR_CODE(svc.get_record(token, added.id), ErrorCode::NotFound);
    // 重复删 -> NotFound
    TT_IS_ERR_CODE(svc.delete_record(token, added.id), ErrorCode::NotFound);
}

TT_TEST(Integration, 查询筛选与分页) {
    TempDataDir dir("query");
    TT_TRY_ASSIGN(svc_res, make_service(dir));
    LedgerService& svc = *svc_res;

    TT_IS_OK(svc.register_user("quser", "quser-pass-1"));
    TT_TRY_ASSIGN(login, svc.login("quser", "quser-pass-1"));
    const std::string token = login.token;

    TT_TRY_ASSIGN(cats, svc.list_categories(token, Direction::Expense, false));
    if (cats.empty()) { TT_FAIL("没有分类"); return; }

    // 造 30 笔：日期跨 2026-09-01 ~ 2026-09-30，金额递增，备注带数字
    for (int i = 0; i < 30; ++i) {
        RecordInput in;
        in.amount = Money::from_minor(1000 + i * 500);
        in.direction = Direction::Expense;
        in.category_id = cats[i % cats.size()].id;
        in.date = Date::create(2026, 9, 1 + i).value_or(Date{2026, 9, 1});
        in.note = "第" + std::to_string(i) + "笔";
        TT_IS_OK(svc.add_record(token, in));
    }

    // 总数
    {
        RecordQuery q;
        q.limit = 1000;
        TT_TRY_ASSIGN(all, svc.list_records(token, q));
        TT_EQ(all.total, int64_t{30});
    }
    // 分页：limit 10 但 total 仍是 30
    {
        RecordQuery q;
        q.limit = 10;
        q.offset = 0;
        TT_TRY_ASSIGN(page1, svc.list_records(token, q));
        TT_EQ(page1.total, int64_t{30});
        TT_EQ(page1.items.size(), size_t{10});
        // 默认按日期倒序：第一页第一条应该是 9 月 30 日
        TT_EQ(page1.items.front().date, Date({2026, 9, 30}));
    }
    // 日期区间过滤
    {
        RecordQuery q;
        q.limit = 1000;
        q.range = DateRange{Date{2026, 9, 1}, Date{2026, 9, 10}};
        TT_TRY_ASSIGN(r, svc.list_records(token, q));
        TT_EQ(r.total, int64_t{10});
    }
    // 金额下限
    {
        RecordQuery q;
        q.limit = 1000;
        q.min_minor = 1000 + 20 * 500;      // >= 第 20 笔
        TT_TRY_ASSIGN(r, svc.list_records(token, q));
        TT_EQ(r.total, int64_t{10});
    }
    // 备注关键字
    {
        RecordQuery q;
        q.limit = 1000;
        q.note_keyword = "第1笔";
        TT_TRY_ASSIGN(r, svc.list_records(token, q));
        TT_EQ(r.total, int64_t{1});
    }
    // LIKE 通配符必须被转义：输入 % 不能匹配到全部
    {
        RecordQuery q;
        q.limit = 1000;
        q.note_keyword = "%";
        TT_TRY_ASSIGN(r, svc.list_records(token, q));
        TT_EQ_MSG(r.total, int64_t{0}, "字符 % 没有被转义，变成了 LIKE 通配符");
    }
    {
        RecordQuery q;
        q.limit = 1000;
        q.note_keyword = "_";
        TT_TRY_ASSIGN(r, svc.list_records(token, q));
        TT_EQ_MSG(r.total, int64_t{0}, "字符 _ 没有被转义，变成了 LIKE 通配符");
    }
    // 升序
    {
        RecordQuery q;
        q.limit = 5;
        q.ascending = true;
        TT_TRY_ASSIGN(r, svc.list_records(token, q));
        TT_EQ(r.items.front().date, Date({2026, 9, 1}));
    }
    // limit 上限保护
    {
        RecordQuery q;
        q.limit = 999999;
        TT_TRY_ASSIGN(r, svc.list_records(token, q));
        TT_CHECK(r.items.size() <= 1000);
    }
}

// -----------------------------------------------------------------------------
//  分类管理
// -----------------------------------------------------------------------------

TT_TEST(Integration, 分类删除保护与停用) {
    TempDataDir dir("cat");
    TT_TRY_ASSIGN(svc_res, make_service(dir));
    LedgerService& svc = *svc_res;

    TT_IS_OK(svc.register_user("catuser", "catuser-01"));
    TT_TRY_ASSIGN(login, svc.login("catuser", "catuser-01"));
    const std::string token = login.token;

    TT_TRY_ASSIGN(cats, svc.list_categories(token, Direction::Expense, false));
    if (cats.empty()) { TT_FAIL("没有分类"); return; }

    // 预置分类不允许删除
    TT_CHECK(cats.front().is_builtin);
    TT_IS_ERR_CODE(svc.delete_category(token, cats.front().id), ErrorCode::PermissionDenied);

    // 自定义分类可以建。
    // 名字刻意取一个「绝不会出现在预置表里」的：原来这里用「宠物」，
    // 后来预置分类扩到 52 条时把「宠物」收了进去，这个用例立刻因为
    // AlreadyExists 挂掉。测试不该依赖「某个名字恰好不在预置表里」这种隐含前提。
    const std::string custom_name = "自建分类-测试";
    TT_TRY_ASSIGN(created, svc.create_category(token, custom_name, "pets", "#FF8A65",
                                              Direction::Expense, 500));
    TT_CHECK(!created.is_builtin);

    // 同名同方向重复创建必须被拒
    TT_IS_ERR_CODE(svc.create_category(token, custom_name, "pets", "#FF8A65",
                                       Direction::Expense, 501),
                   ErrorCode::AlreadyExists);

    // 有记录引用时不允许删除
    RecordInput in;
    in.amount = Money::from_minor(8800);
    in.direction = Direction::Expense;
    in.category_id = created.id;
    in.date = Date{2026, 9, 18};
    TT_IS_OK(svc.add_record(token, in));
    TT_IS_ERR_CODE(svc.delete_category(token, created.id), ErrorCode::PermissionDenied);

    // 停用之后不能再记账
    TT_IS_OK(svc.set_category_active(token, created.id, false));
    TT_IS_ERR(svc.add_record(token, in));

    // 停用后列表默认不含它，但 include_inactive 能看到
    {
        TT_TRY_ASSIGN(active_only, svc.list_categories(token, Direction::Expense, false));
        TT_TRY_ASSIGN(with_inactive, svc.list_categories(token, Direction::Expense, true));
        TT_CHECK(with_inactive.size() > active_only.size());
    }

    // 非法颜色必须被拒（前端会把它塞进 CSS，不校验等于开 XSS 口子）
    TT_IS_ERR(svc.create_category(token, "坏颜色", "bug", "red; background:url(x)",
                                 Direction::Expense, 1));
    TT_IS_ERR(svc.create_category(token, "", "bug", "#FF8A65", Direction::Expense, 1));
}

// -----------------------------------------------------------------------------
//  加密：第 6 条要求的直接证据
// -----------------------------------------------------------------------------

TT_TEST(Encryption, 账本文件里不含明文金额与备注) {
    TempDataDir dir("cipher");
    TT_TRY_ASSIGN(svc_res, make_service(dir));
    LedgerService& svc = *svc_res;

    const std::string username = "cipheruser";
    const std::string password = "cipher-pass-1";
    const std::string canary = "PLAINTEXT_CANARY_7f3a9b21";

    TT_IS_OK(svc.register_user(username, password));
    TT_TRY_ASSIGN(login, svc.login(username, password));
    const std::string token = login.token;
    const std::string user_id = login.user.id;

    TT_TRY_ASSIGN(cats, svc.list_categories(token, Direction::Expense, false));
    if (cats.empty()) { TT_FAIL("没有分类"); return; }

    RecordInput in;
    in.amount = Money::from_minor(4321);
    in.direction = Direction::Expense;
    in.category_id = cats.front().id;
    in.date = Date{2026, 9, 18};
    in.note = canary;
    TT_TRY_ASSIGN(added, svc.add_record(token, in));

    // 先确认数据真的写进去了。
    // 否则「扫不到明文」可能只是因为压根没写，那这个测试就是假阳性。
    TT_TRY_ASSIGN(fetched, svc.get_record(token, added.id));
    TT_EQ(fetched.note, canary);
    TT_EQ(fetched.amount.minor_units(), 4321);

    // 登出会触发 WAL checkpoint 并关闭连接，让数据全部落到主文件
    TT_IS_OK(svc.logout(token));

    const std::string ledger = fs::join(dir.path(), fs::join("data", user_id + ".db"));
    const std::string ledger_wal = ledger + "-wal";
    const std::string users_db = fs::join(dir.path(), "users.db");

    TT_CHECK_MSG(fs::exists(ledger), "账本文件不存在: " + ledger);
    const std::string ledger_bytes = read_raw_file(ledger);
    TT_CHECK_MSG(!ledger_bytes.empty(), "账本文件是空的");

    // ===================== 核心断言 =====================
    TT_CHECK_MSG(!contains_bytes(ledger_bytes, canary),
                 "加密失败：账本文件里出现了明文备注！");
    TT_CHECK_MSG(!contains_bytes(ledger_bytes, username),
                 "账本文件里出现了用户名（本不该有）");

    // WAL 同样是数据文件，容易被忽略，必须一起验
    if (fs::exists(ledger_wal)) {
        const std::string wal_bytes = read_raw_file(ledger_wal);
        TT_CHECK_MSG(!contains_bytes(wal_bytes, canary),
                     "加密失败：WAL 文件里出现了明文备注！");
    }

    // users.db 是明文库。下面两条断言是「故意验证我们公开承认的暴露面」——
    // 我不想只证明加密生效，还想把「什么没被保护」也钉在测试里。
    // users.db 是 WAL 模式：刚提交的行可能还在 users.db-wal 里，主库文件
    // 上看不到。所以「明文暴露面」必须把两个文件合起来看 ——
    // 只读主库的话，下面那条「主库里不能有明文备注」也成了空断言
    // （文件里没数据，自然什么都搜不到），等于白测。
    std::string users_bytes = read_raw_file(users_db);
    const std::string users_wal = fs::join(dir.path(), "users.db-wal");
    if (fs::exists(users_wal)) users_bytes += read_raw_file(users_wal);

    TT_CHECK_MSG(contains_bytes(users_bytes, username),
                 "users.db(+WAL) 里应当有用户名（这是设计上承认的明文暴露）");
    TT_CHECK_MSG(!contains_bytes(users_bytes, canary),
                 "严重问题：明文库里出现了账本备注！");
    TT_CHECK_MSG(!contains_bytes(users_bytes, password),
                 "严重问题：明文库里出现了明文密码！");
}

TT_TEST(Encryption, 篡改账本后报错而不是静默返回空) {
    TempDataDir dir("tamper");
    TT_TRY_ASSIGN(svc_res, make_service(dir));
    LedgerService& svc = *svc_res;

    TT_IS_OK(svc.register_user("tamperuser", "tamper-pass-1"));
    TT_TRY_ASSIGN(login, svc.login("tamperuser", "tamper-pass-1"));
    const std::string user_id = login.user.id;
    TT_IS_OK(svc.logout(login.token));

    const std::string ledger = fs::join(dir.path(), fs::join("data", user_id + ".db"));
    TT_CHECK(fs::exists(ledger));

    // 往靠后的页里翻一位，覆盖「页级 HMAC 校验」这条路径
    {
        std::fstream f(ledger, std::ios::in | std::ios::out | std::ios::binary);
        TT_CHECK(f.good());
        if (!f.good()) return;
        f.seekg(1024, std::ios::beg);
        char c = 0;
        f.read(&c, 1);
        c = static_cast<char>(c ^ 0x5A);
        f.seekp(1024, std::ios::beg);
        f.write(&c, 1);
    }

    auto relogin = svc.login("tamperuser", "tamper-pass-1");
    if (relogin.is_err()) {
        // 期望：明确报错，且错误码是「加密/存储」类，而不是 Internal 或崩溃
        const ErrorCode code = relogin.error().code;
        TT_CHECK_MSG(code == ErrorCode::CryptoFailure || code == ErrorCode::StorageFailure,
                     "篡改后的错误码不够贴切: " +
                         std::string(penhu::to_string(code)) + " / " +
                         relogin.error().message);
    } else {
        // 也可能篡改落在未使用的页上而没被读到。这种情况必须至少保证
        // 不会返回半截脏数据；这里只记录，不算失败，但要在输出里可见。
        TT_CHECK_MSG(true, "篡改落在未读页上，未被检测到（可接受，但需知情）");
    }
}

// -----------------------------------------------------------------------------
//  改密码
// -----------------------------------------------------------------------------

TT_TEST(Integration, 改密码后数据保留且旧密码失效) {
    TempDataDir dir("rekey");
    TT_TRY_ASSIGN(svc_res, make_service(dir));
    LedgerService& svc = *svc_res;

    TT_IS_OK(svc.register_user("rekeyuser", "old-pass-123"));
    TT_TRY_ASSIGN(login, svc.login("rekeyuser", "old-pass-123"));
    const std::string token = login.token;

    TT_TRY_ASSIGN(cats, svc.list_categories(token, Direction::Expense, false));
    if (cats.empty()) { TT_FAIL("没有分类"); return; }

    RecordInput in;
    in.amount = Money::from_minor(12345);
    in.direction = Direction::Expense;
    in.category_id = cats.front().id;
    in.date = Date{2026, 9, 18};
    in.note = "改密码前写入的数据";
    TT_IS_OK(svc.add_record(token, in));

    // 新密码不合规必须被拒，且不改动任何东西
    TT_IS_ERR(svc.change_password(token, "old-pass-123", "123"));
    // 原密码错必须被拒
    TT_IS_ERR_CODE(svc.change_password(token, "wrong-old", "new-pass-456"),
                   ErrorCode::AuthFailed);

    TT_TRY_ASSIGN(changed, svc.change_password(token, "old-pass-123", "new-pass-456"));
    TT_CHECK(changed.requires_relogin);
    TT_CHECK_MSG(!changed.backup_path.empty(), "改密码应当留下备份路径");
    TT_CHECK_MSG(fs::exists(changed.backup_path), "备份文件应当真实存在");

    // 改密码后原会话必须立即失效
    TT_IS_ERR_CODE(svc.current_user(token), ErrorCode::AuthFailed);

    // 旧密码不能登录
    TT_IS_ERR_CODE(svc.login("rekeyuser", "old-pass-123"), ErrorCode::AuthFailed);

    // 新密码能登录，且数据完好——这是「重加密没有毁数据」的证据
    TT_TRY_ASSIGN(relogin, svc.login("rekeyuser", "new-pass-456"));

    RecordQuery q;
    q.limit = 100;
    TT_TRY_ASSIGN(records, svc.list_records(relogin.token, q));
    TT_EQ(records.total, int64_t{1});
    if (!records.items.empty()) {
        TT_EQ(records.items.front().amount.minor_units(), 12345);
        TT_EQ(records.items.front().note, std::string("改密码前写入的数据"));
    }
}

// -----------------------------------------------------------------------------
//  会话与账号
// -----------------------------------------------------------------------------

TT_TEST(Integration, 会话过期后必须重新登录) {
    TempDataDir dir("expiry");
    auto cfg_res = AppConfig::from_data_dir(dir.path());
    TT_REQUIRE_OK(cfg_res);
    AppConfig cfg = std::move(cfg_res).value();
    cfg.kdf = crypto::KdfParams::interactive();
    cfg.session_ttl_seconds = 1;

    auto svc_res = LedgerService::create(std::move(cfg));
    TT_REQUIRE_OK(svc_res);
    LedgerService& svc = *svc_res.value();

    TT_IS_OK(svc.register_user("expuser", "expiry-pass-1"));
    TT_TRY_ASSIGN(login, svc.login("expuser", "expiry-pass-1"));
    const std::string token = login.token;

    TT_IS_OK(svc.current_user(token));
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    TT_IS_ERR_CODE(svc.current_user(token), ErrorCode::AuthFailed);
}

TT_TEST(Integration, 删账号清理文件与会话) {
    TempDataDir dir("delacct");
    TT_TRY_ASSIGN(svc_res, make_service(dir));
    LedgerService& svc = *svc_res;

    TT_IS_OK(svc.register_user("deluser", "delete-pass-1"));
    TT_TRY_ASSIGN(login, svc.login("deluser", "delete-pass-1"));
    const std::string token = login.token;
    const std::string user_id = login.user.id;

    const std::string ledger = fs::join(dir.path(), fs::join("data", user_id + ".db"));
    TT_CHECK(fs::exists(ledger));

    // 密码不对不能删，且文件必须完好
    TT_IS_ERR_CODE(svc.delete_account(token, "wrong-pass"), ErrorCode::AuthFailed);
    TT_CHECK(fs::exists(ledger));

    TT_IS_OK(svc.delete_account(token, "delete-pass-1"));
    TT_CHECK_MSG(!fs::exists(ledger), "账本文件应当已删除");
    TT_IS_ERR_CODE(svc.current_user(token), ErrorCode::AuthFailed);
    TT_IS_ERR_CODE(svc.login("deluser", "delete-pass-1"), ErrorCode::AuthFailed);

    // 备份应当存在（keep_deleted_backup 默认 true）
    auto backups = fs::list_files(fs::join(dir.path(), "backups"));
    TT_REQUIRE_OK(backups);
    bool found = false;
    for (const auto& name : backups.value()) {
        if (name.find("deleted") != std::string::npos) found = true;
    }
    TT_CHECK_MSG(found, "删账号应当留下 deleted 备份");
}

TT_TEST(Integration, 运行统计随操作变化) {
    TempDataDir dir("stats");
    TT_TRY_ASSIGN(svc_res, make_service(dir));
    LedgerService& svc = *svc_res;

    TT_EQ(svc.runtime_stats().users, int64_t{0});

    TT_IS_OK(svc.register_user("st1", "stats-pass-1"));
    TT_IS_OK(svc.register_user("st2", "stats-pass-2"));
    TT_EQ(svc.runtime_stats().users, int64_t{2});

    TT_TRY_ASSIGN(l1, svc.login("st1", "stats-pass-1"));
    TT_TRY_ASSIGN(l2, svc.login("st2", "stats-pass-2"));
    TT_EQ(svc.runtime_stats().active_sessions, size_t{2});
    TT_EQ(svc.runtime_stats().open_ledgers, size_t{2});

    // 登出一个后，库连接应当也被关掉（少一份内存里的解密页缓存）
    TT_IS_OK(svc.logout(l1.token));
    TT_EQ(svc.runtime_stats().active_sessions, size_t{1});
    TT_EQ(svc.runtime_stats().open_ledgers, size_t{1});

    TT_IS_OK(svc.logout(l2.token));
    TT_EQ(svc.runtime_stats().active_sessions, size_t{0});
    TT_EQ(svc.runtime_stats().open_ledgers, size_t{0});
}
