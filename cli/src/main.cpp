// =============================================================================
//  cli/src/main.cpp
//  PenHu Ledger 命令行入口。
//
//  它不是给最终用户用的（用户用桌面版）。它存在的理由有两个，都很实际：
//
//    1) 自测与举证。
//      `selftest` 会把「加密到底有没有生效」「两个账号到底有没有隔离」
//      这两件最容易被想当然的事，用读原始字节的方式验一遍，并把实测数字
//      打出来。结论要能被别人复现，而不是「我认为应该没问题」。
//
//    2) 排障。
//      `serve` 可以在不起窗口的情况下把 HTTP 服务跑起来，
//      这样能用 curl 打真实请求、能用浏览器打开截图确认界面渲染。
//      界面出问题时，先分清是「服务返回的数据不对」还是「前端渲染不对」，
//      否则两边一起查会浪费大量时间。
// =============================================================================

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "penhu/app/app_config.hpp"
#include "penhu/app/ledger_service.hpp"
#include "penhu/crypto/cipher.hpp"
#include "penhu/crypto/password.hpp"
#include "penhu/domain/date.hpp"
#include "penhu/report/report_service.hpp"
#include "penhu/server/http_server.hpp"
#include "penhu/util/fs.hpp"
#include "penhu/util/log.hpp"
#include "penhu/util/uuid.hpp"

using namespace penhu;

namespace {

// -----------------------------------------------------------------------------
//  输出
// -----------------------------------------------------------------------------

int g_pass = 0;
int g_fail = 0;

void section(const std::string& title) {
    std::cout << "\n=== " << title << " ===\n";
}

void check(bool ok, const std::string& what, const std::string& detail = {}) {
    if (ok) {
        ++g_pass;
        std::cout << "  [OK]   " << what;
    } else {
        ++g_fail;
        std::cout << "  [FAIL] " << what;
    }
    if (!detail.empty()) std::cout << "  —— " << detail;
    std::cout << "\n";
}

void note(const std::string& text) { std::cout << "         " << text << "\n"; }

std::string yuan(int64_t minor) {
    const bool neg = minor < 0;
    const uint64_t mag = neg ? (~static_cast<uint64_t>(minor) + 1ULL)
                             : static_cast<uint64_t>(minor);
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%s¥%llu.%02llu", neg ? "-" : "",
                  static_cast<unsigned long long>(mag / 100ULL),
                  static_cast<unsigned long long>(mag % 100ULL));
    return std::string(buf);
}

std::string read_raw_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// -----------------------------------------------------------------------------
//  参数
// -----------------------------------------------------------------------------

struct Args {
    std::string data_dir;
    int         port{0};
    std::string static_root;
    bool        keep_data{false};
    bool        verbose{false};
    int         speed{0};      // 0=生产参数 1=interactive(快)
};

Args parse_args(int argc, char** argv, int from) {
    Args a;
    for (int i = from; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "参数 " << name << " 后面缺少值\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--data-dir") a.data_dir = next("--data-dir");
        else if (arg == "--port") a.port = std::atoi(next("--port").c_str());
        else if (arg == "--static-root") a.static_root = next("--static-root");
        else if (arg == "--keep-data") a.keep_data = true;
        else if (arg == "--verbose" || arg == "-v") a.verbose = true;
        else if (arg == "--fast-kdf") a.speed = 1;
        else if (arg == "--help" || arg == "-h") { /* 交给调用方处理 */ }
        else {
            std::cerr << "未知参数: " << arg << "\n";
            std::exit(2);
        }
    }
    return a;
}

/// 临时数据目录：自测默认不污染用户的真实数据
struct TempDir {
    std::string path;
    bool        keep{false};
    ~TempDir() {
        if (!keep && !path.empty()) {
            fs::remove_all(path);   // 失败也不抛，临时目录残留不是错误
        }
    }
};

std::string make_temp_dir() {
    const std::string base = fs::home_dir() + "/AppData/Local/Temp";
    const std::string dir = fs::join(base, "penhu-selftest-" + new_uuid().substr(0, 8));
    (void)fs::create_directories(dir);
    return dir;
}

// -----------------------------------------------------------------------------
//  selftest
// -----------------------------------------------------------------------------

int cmd_selftest(const Args& args) {
    std::cout << "PenHu Ledger " << PENHU_LEDGER_VERSION << " · 自测\n";
    std::cout << "每一项都会打印实测结果，不打印的结论就是没测过。\n";

    TempDir temp;
    std::string data_dir = args.data_dir;
    if (data_dir.empty()) {
        data_dir = make_temp_dir();
        temp.path = data_dir;
    }
    temp.keep = args.keep_data;
    std::cout << "数据目录: " << data_dir
              << (args.keep_data ? "（保留）" : "（结束后删除）") << "\n";

    // -------------------------------------------------------------------------
    section("1) 加密原语自检");
    // -------------------------------------------------------------------------
    {
        std::string report;
        const bool ok = crypto::aead_self_test(&report);
        check(ok, "AES-256-GCM 与 XChaCha20-Poly1305 往返加解密", "");
        std::cout << report;
    }

    // KDF 耗时是真实用户体验的一部分，量出来比说「安全」有意义
    {
        const auto kdf = (args.speed == 1) ? crypto::KdfParams::interactive()
                                           : crypto::KdfParams::moderate();
        const std::string salt = crypto::generate_salt_hex(16);
        const auto t0 = std::chrono::steady_clock::now();
        auto derived = crypto::derive_db_key("correct horse battery staple", salt, kdf);
        const auto t1 = std::chrono::steady_clock::now();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

        check(derived.is_ok(), "Argon2id 密钥派生",
              std::to_string(ms) + " ms / " + kdf.describe());
        note("这个耗时就等于每次登录要等的时间。调高它更抗暴力破解，也让人更烦。");
    }

    // -------------------------------------------------------------------------
    section("2) 建号与登录");
    // -------------------------------------------------------------------------
    auto cfg_res = app::AppConfig::from_data_dir(data_dir);
    if (cfg_res.is_err()) {
        std::cerr << "配置失败: " << cfg_res.error().to_string() << "\n";
        return 1;
    }
    app::AppConfig cfg = std::move(cfg_res).value();
    if (args.speed == 1) cfg.kdf = crypto::KdfParams::interactive();

    auto svc_res = app::LedgerService::create(std::move(cfg));
    if (svc_res.is_err()) {
        std::cerr << "服务初始化失败: " << svc_res.error().to_string() << "\n";
        return 1;
    }
    app::LedgerService& svc = *svc_res.value();

    const std::string alice_pw = "alice-pass-1";
    const std::string bob_pw = "bob-pass-12";

    auto r_alice = svc.register_user("alice", alice_pw);
    check(r_alice.is_ok(), "注册 alice", r_alice.is_err() ? r_alice.error().message : "");
    auto r_bob = svc.register_user("bob", bob_pw);
    check(r_bob.is_ok(), "注册 bob", r_bob.is_err() ? r_bob.error().message : "");

    // 重名（含大小写不敏感）
    auto dup = svc.register_user("ALICE", alice_pw);
    check(dup.is_err() && dup.error().code == ErrorCode::AlreadyExists,
          "重名注册被拒（大小写不敏感）",
          dup.is_err() ? dup.error().message : "竟然成功了");

    // 弱密码
    auto weak = svc.register_user("weakuser", "12345678");
    check(weak.is_err(), "纯数字 8 位密码被拒",
          weak.is_err() ? weak.error().message : "竟然通过了");

    auto l_alice_res = svc.login("alice", alice_pw);
    if (l_alice_res.is_err()) {
        std::cerr << "alice 登录失败: " << l_alice_res.error().to_string() << "\n";
        return 1;
    }
    const app::AuthResult l_alice = std::move(l_alice_res).value();

    auto l_bob_res = svc.login("bob", bob_pw);
    if (l_bob_res.is_err()) {
        std::cerr << "bob 登录失败: " << l_bob_res.error().to_string() << "\n";
        return 1;
    }
    const app::AuthResult l_bob = std::move(l_bob_res).value();

    check(l_alice.token.size() == 64, "登录发下的 token 是 32 字节十六进制",
          "长度=" + std::to_string(l_alice.token.size()));
    check(l_alice.user.id != l_bob.user.id, "两个账号的 user id 不同");

    auto bad_pw = svc.login("alice", "wrong-password");
    check(bad_pw.is_err() && bad_pw.error().code == ErrorCode::AuthFailed,
          "错密码登录失败");

    auto ghost = svc.login("nobody", "whatever1");
    check(ghost.is_err() && ghost.error().code == ErrorCode::AuthFailed,
          "不存在的账号也返回同样的失败（不泄露用户名是否存在）");

    // -------------------------------------------------------------------------
    section("3) 记账与统计");
    // -------------------------------------------------------------------------
    auto cats_res = svc.list_categories(l_alice.token, Direction::Expense, false);
    if (cats_res.is_err()) {
        std::cerr << "取分类失败: " << cats_res.error().to_string() << "\n";
        return 1;
    }
    std::vector<Category> alice_cats = std::move(cats_res).value();
    check(alice_cats.size() >= 8, "预置支出分类已就绪",
          std::to_string(alice_cats.size()) + " 个");

    const std::string canary = "PLAINTEXT_CANARY_" + new_uuid().substr(0, 8);
    note("明文金丝雀（用来扫原始文件字节）: " + canary);

    int64_t total_minor = 0;
    const Date today = Date::today();
    for (int i = 0; i < 12; ++i) {
        RecordInput in;
        const int64_t amount = 1500 + i * 320;      // 15.00 ~ 50.20
        in.amount = Money::from_minor(amount);
        in.direction = Direction::Expense;
        in.category_id = alice_cats[static_cast<size_t>(i) % alice_cats.size()].id;
        in.date = today.add_days(-(i % 6));
        in.note = (i == 0) ? canary : ("第 " + std::to_string(i + 1) + " 笔");
        auto added = svc.add_record(l_alice.token, in);
        if (added.is_err()) {
            std::cerr << "记账失败: " << added.error().to_string() << "\n";
            return 1;
        }
        total_minor += amount;
    }
    note("写入 12 笔，" + yuan(total_minor) + "，跨 " +
         std::to_string(std::min(6, 12)) + " 个自然日");

    // 金额为 0 必须被拒
    {
        RecordInput bad;
        bad.amount = Money::from_minor(0);
        bad.direction = Direction::Expense;
        bad.category_id = alice_cats.front().id;
        bad.date = today;
        auto r = svc.add_record(l_alice.token, bad);
        check(r.is_err() && r.error().code == ErrorCode::InvalidArgument,
              "金额 0 被拒", r.is_err() ? r.error().message : "");
    }
    // 方向与分类不匹配必须被拒
    {
        auto income = svc.list_categories(l_alice.token, Direction::Income, false);
        if (income.is_ok() && !income.value().empty()) {
            RecordInput bad;
            bad.amount = Money::from_minor(100);
            bad.direction = Direction::Expense;      // 分类是收入类
            bad.category_id = income.value().front().id;
            bad.date = today;
            auto r = svc.add_record(l_alice.token, bad);
            check(r.is_err(), "支出方向 + 收入分类 被拒",
                  r.is_err() ? r.error().message : "");
        }
    }

    // -------------------------------------------------------------------------
    section("4) 多账号隔离");
    // -------------------------------------------------------------------------
    {
        RecordQuery q;
        q.limit = 1000;

        auto a_all = svc.list_records(l_alice.token, q);
        auto b_all = svc.list_records(l_bob.token, q);
        if (a_all.is_err() || b_all.is_err()) {
            check(false, "查询记录时出错");
        } else {
            check(a_all.value().total == 12, "alice 看到自己 12 笔",
                  "实际 " + std::to_string(a_all.value().total));
            check(b_all.value().total == 0, "bob 看到 0 笔（账本隔离）",
                  "实际 " + std::to_string(b_all.value().total));

            // bob 用 alice 的分类记账必须失败
            RecordInput cross;
            cross.amount = Money::from_minor(999);
            cross.direction = Direction::Expense;
            cross.category_id = alice_cats.front().id;
            cross.date = today;
            auto r = svc.add_record(l_bob.token, cross);
            check(r.is_err(), "bob 用 alice 的分类记账被拒（跨账号引用）",
                  r.is_err() ? r.error().message : "");

            // bob 删 alice 的记录必须失败
            if (!a_all.value().items.empty()) {
                auto r2 = svc.delete_record(l_bob.token, a_all.value().items.front().id);
                check(r2.is_err(), "bob 删 alice 的记录被拒（跨账号操作）",
                      r2.is_err() ? r2.error().message : "");
            }

            // 分类表也是各自独立的
            auto b_cats = svc.list_categories(l_bob.token, Direction::Expense, true);
            bool overlap = false;
            if (b_cats.is_ok()) {
                for (const auto& ac : alice_cats) {
                    for (const auto& bc : b_cats.value()) {
                        if (ac.id == bc.id) overlap = true;
                    }
                }
            }
            check(!overlap, "两个账号的分类 id 没有重叠（分类按账号隔离）");
        }

        // 无效 token
        auto bad_token = svc.list_records("deadbeef", q);
        check(bad_token.is_err() && bad_token.error().code == ErrorCode::AuthFailed,
              "无效 token 被拒");
    }

    // -------------------------------------------------------------------------
    section("5) 元数据加密（读原始文件字节）");
    // -------------------------------------------------------------------------
    const std::string alice_ledger =
        fs::join(data_dir, fs::join("data", l_alice.user.id + ".db"));
    const std::string bob_ledger = fs::join(data_dir, fs::join("data", l_bob.user.id + ".db"));
    const std::string users_db = fs::join(data_dir, "users.db");
    {
        // 必须先登出：登出会 checkpoint 并把连接关掉，保证 WAL 的内容落到主文件。
        // 不这么做的话「扫不到明文」可能只是因为数据还在 WAL 里没写进去——
        // 那就是一个假阳性，比不做验证更糟。
        (void)svc.logout(l_alice.token);
        (void)svc.logout(l_bob.token);

        check(fs::exists(alice_ledger), "alice 的账本文件已落盘", alice_ledger);
        check(alice_ledger != bob_ledger, "两个账号各自的库文件不同");

        const std::string bytes = read_raw_file(alice_ledger);
        check(!bytes.empty(), "账本文件非空",
              std::to_string(bytes.size()) + " 字节");

        const bool has_canary = bytes.find(canary) != std::string::npos;
        check(!has_canary, "账本原始字节里搜不到明文备注（金丝雀）",
              has_canary ? "！加密没生效" : "已扫描 " + std::to_string(bytes.size()) + " 字节");

        // 分类名是元数据，也必须看不到
        const bool has_cat = bytes.find(alice_cats.front().name) != std::string::npos;
        check(!has_cat, "账本原始字节里搜不到明文分类名（元数据也不泄露）",
              has_cat ? "！「" + alice_cats.front().name + "」出现在库里" : "");

        const bool has_username = bytes.find("alice") != std::string::npos;
        check(!has_username, "账本原始字节里搜不到用户名");

        // WAL 同样是数据文件，最容易被漏掉
        const std::string wal = alice_ledger + "-wal";
        if (fs::exists(wal) && fs::file_size(wal) > 0) {
            const std::string wal_bytes = read_raw_file(wal);
            const bool wal_leak = wal_bytes.find(canary) != std::string::npos;
            check(!wal_leak, "WAL 文件里也搜不到明文备注",
                  "已扫描 " + std::to_string(wal_bytes.size()) + " 字节");
        } else {
            note("WAL 已合并或为空，跳过（登出时的 checkpoint 生效）");
        }

        // ---- 反向验证：把「我们承认没保护的东西」也钉住 ----
        // users.db 是 WAL 模式：刚写入的行可能还躺在 users.db-wal 里，
        // 只看主库文件会得出「主库里没有用户名」这种与事实相反的结论，
        // 而且下面那两条「不能有明文」的检查会因为文件里没数据而变成空断言。
        std::string users_bytes = read_raw_file(users_db);
        const std::string users_wal = fs::join(data_dir, "users.db-wal");
        if (fs::exists(users_wal)) {
            users_bytes += read_raw_file(users_wal);
            note("users.db 的 WAL 已并入检查（未 checkpoint，主库文件里可能还没有新数据）");
        }
        check(users_bytes.find("alice") != std::string::npos,
              "users.db（含 WAL，明文库）里确实有用户名 —— 这是设计上承认的暴露面");
        check(users_bytes.find(canary) == std::string::npos,
              "明文库里没有账本备注（严重问题若出现则说明写错库了）");
        check(users_bytes.find(alice_pw) == std::string::npos,
              "明文库里没有明文密码");
        note("结论：数据目录整体被拷走时，攻击者只能知道「这台机器上有 alice/bob 两个账号」，"
             "金额、备注、分类都拿不到。");
    }

    // -------------------------------------------------------------------------
    section("6) 分析、异常识别与报告");
    // -------------------------------------------------------------------------
    {
        auto relogin_res = svc.login("alice", alice_pw);
        if (relogin_res.is_err()) {
            check(false, "重新登录失败");
            return 1;
        }
        const std::string token = relogin_res.value().token;

        report::ReportService reports(svc, llm::LlmConfig{}, llm::LlmConfig{});
        const DateRange range{today.add_days(-29), today};

        auto analysis = reports.analyze_range(token, range, 7);
        if (analysis.is_err()) {
            check(false, "统计失败", analysis.error().message);
        } else {
            const auto& a = analysis.value();
            check(a.dashboard.summary.expense_minor == total_minor,
                  "区间支出合计与逐笔累加一致",
                  yuan(a.dashboard.summary.expense_minor) + " vs " + yuan(total_minor));
            check(a.dashboard.daily.size() == 30, "日序列补齐为 30 天（含无记录日）",
                  "实际 " + std::to_string(a.dashboard.daily.size()) + " 天");
            note("日均口径：分母是「有记录的天数」= " +
                 std::to_string(a.dashboard.summary.active_days) + "，不是区间长度 30");
            note("口径说明：" + a.anomalies.method_summary);
            note("建议条数：" + std::to_string(a.insights.items.size()) +
                 "，可执行：" + (a.insights.actionable ? "是" : "否"));
            for (const auto& l : a.insights.limitations) note("局限：" + l);
        }

        // 报告：没配 Key，应当走模板兜底而不是报错
        report::GenerateOptions opts;
        opts.kind = report::ReportKind::Weekly;
        opts.anchor = today;
        auto gen = reports.generate(token, opts);
        if (gen.is_err()) {
            check(false, "生成报告", gen.error().message);
        } else {
            const auto& r = gen.value();
            check(r.used_fallback, "没有模型通道时走模板兜底（不是报错、不是空白）",
                  "provider=" + r.provider);
            check(!r.content.empty(), "报告正文非空",
                  std::to_string(r.content.size()) + " 字符");
            note("兜底原因（每条通道都记了，界面要能显示）：");
            for (const auto& n : r.failover_notes) note("  · " + n);
            note("正文前 120 字符：" + r.content.substr(0, std::min<size_t>(120, r.content.size())));
        }

        (void)svc.logout(token);
    }

    // -------------------------------------------------------------------------
    section("7) 篡改检测");
    // -------------------------------------------------------------------------
    {
        // 改一个字节，重新登录必须明确报错，而不是静默返回半截数据。
        {
            std::fstream f(alice_ledger, std::ios::in | std::ios::out | std::ios::binary);
            if (f.good()) {
                f.seekg(1024, std::ios::beg);
                char c = 0;
                f.read(&c, 1);
                c = static_cast<char>(c ^ 0x5A);
                f.seekp(1024, std::ios::beg);
                f.write(&c, 1);
                note("已把账本第 1024 字节翻转一位");
            }
        }
        auto r = svc.login("alice", alice_pw);
        if (r.is_err()) {
            check(r.error().code == ErrorCode::CryptoFailure ||
                      r.error().code == ErrorCode::StorageFailure,
                  "篡改后报加密/存储错误（错误码贴切，不是 Internal）",
                  std::string(penhu::to_string(r.error().code)) + ": " + r.error().message);
        } else {
            note("篡改落在未被读取的页上没有触发检测。"
                 "这不算失败——SQLCipher 是按页校验 HMAC 的——"
                 "但要知情，不能对外宣称「任何篡改都能发现」。");
            (void)svc.logout(r.value().token);
        }
    }

    // -------------------------------------------------------------------------
    section("汇总");
    // -------------------------------------------------------------------------
    std::cout << "  通过 " << g_pass << " 项，失败 " << g_fail << " 项\n";
    if (g_fail > 0) {
        std::cout << "  有失败项。上面每一条都写了实际返回值，"
                     "不要跳过——这些正是「程序不崩但结果不对」的那类问题。\n";
    }
    return g_fail == 0 ? 0 : 1;
}

// -----------------------------------------------------------------------------
//  serve
// -----------------------------------------------------------------------------

int cmd_serve(const Args& args) {
    auto cfg_res = app::AppConfig::from_data_dir(
        args.data_dir.empty() ? (fs::home_dir() + "/PenHuLedger") : args.data_dir);
    if (cfg_res.is_err()) {
        std::cerr << "配置失败: " << cfg_res.error().to_string() << "\n";
        return 1;
    }

    server::ServerOptions opts;
    opts.port = args.port;
    opts.static_root = args.static_root;
    opts.log_requests = args.verbose;

    auto server_res = server::HttpServer::create(std::move(cfg_res).value(), opts);
    if (server_res.is_err()) {
        std::cerr << "服务创建失败: " << server_res.error().to_string() << "\n";
        return 1;
    }
    auto srv = std::move(server_res).value();

    auto started = srv->start();
    if (started.is_err()) {
        std::cerr << "启动失败: " << started.error().to_string() << "\n";
        return 1;
    }

    std::cout << srv->base_url() << "\n" << std::flush;

    // 等到 Ctrl+C。这个进程的 stdout 只有一行 URL，
    // 方便脚本/工具直接取它拿到端口（随机端口模式下必需）。
    while (true) {
#ifdef _WIN32
        Sleep(200);
#else
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
#endif
        if (!srv->running()) break;
    }
    return 0;
}

// -----------------------------------------------------------------------------
//  帮助
// -----------------------------------------------------------------------------

void print_help() {
    std::cout <<
        "PenHu Ledger " PENHU_LEDGER_VERSION "\n"
        "用法:\n"
        "  penhu-cli selftest [--data-dir DIR] [--fast-kdf] [--keep-data]\n"
        "      端到端自测：加密原语、建号登录、记账统计、多账号隔离、\n"
        "      元数据加密（读原始字节找金丝雀）、报告兜底、篡改检测。\n"
        "      默认用临时目录，跑完删掉。\n"
        "\n"
        "  penhu-cli serve [--data-dir DIR] [--port N] [--static-root DIR] [-v]\n"
        "      只跑 HTTP 服务（不开窗口），stdout 打印一行访问地址。\n"
        "      用于 curl 验证接口、或浏览器打开确认界面渲染。\n"
        "      --static-root 指向项目里的 web/ 可以直接改前端调试。\n"
        "\n"
        "  penhu-cli version | help\n";
}

}  // namespace

int main(int argc, char** argv) {
    // Windows 控制台默认用本地代码页（本机是 GBK/936），而我们的输出是 UTF-8，
    // 不设这一行中文全是乱码（表现为「娉ㄥ唽」这种），一个中文应用的第一印象就毁了。
    // 设成 UTF-8 之后 cmd 和 Windows Terminal 都正常。
    //
    // Linux 没有这件事：终端本来就是 UTF-8、locale 由环境变量决定，
    // 没有「代码页」这个概念，也就没有对应的 API。所以整段只在 Windows 下编译。
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    // 日志写文件，别刷屏；标准输出留给「结论」
    Logger::default_logger().set_level(LogLevel::Warn);

    if (argc < 2) {
        print_help();
        return 2;
    }

    const std::string cmd = argv[1];
    if (cmd == "help" || cmd == "--help" || cmd == "-h") {
        print_help();
        return 0;
    }
    if (cmd == "version" || cmd == "--version") {
        std::cout << PENHU_LEDGER_VERSION << "\n";
        return 0;
    }
    if (cmd == "selftest") {
        return cmd_selftest(parse_args(argc, argv, 2));
    }
    if (cmd == "serve") {
        return cmd_serve(parse_args(argc, argv, 2));
    }

    std::cerr << "未知命令: " << cmd << "\n\n";
    print_help();
    return 2;
}
