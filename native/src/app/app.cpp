// =============================================================================
//  native/app/app.cpp
// =============================================================================

#include "app/app.hpp"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <thread>
#include <utility>

#include "app/image_util.hpp"
#include "app/platform_fs.hpp"
#include "penhu/app/llm_settings.hpp"
#include "penhu/domain/money.hpp"
#include "penhu/llm/settings.hpp"
#include "penhu/util/log.hpp"
#include "penhu/vision/receipt_scan.hpp"
#include "penhu/ui/prefs.hpp"
#include "penhu/vision/scan_settings.hpp"
#include "ui/convert.hpp"

namespace penhu::native {

namespace {
std::atomic<bool> g_shutting_down{false};

std::string today_iso() { return Date::today().to_string(); }
}  // namespace

App& App::get() {
    static App instance;
    return instance;
}

// -----------------------------------------------------------------------------
//  生命周期
// -----------------------------------------------------------------------------

bool App::init(const std::string& data_dir_override, std::wstring* err) {
    auto cfg = data_dir_override.empty() ? app::AppConfig::default_config()
                                        : app::AppConfig::from_data_dir(data_dir_override);
    if (cfg.is_err()) {
        if (err) *err = L"数据目录不可用：" + ui::to_wide(cfg.error().message);
        return false;
    }

    auto svc = app::LedgerService::create(std::move(cfg).value());
    if (svc.is_err()) {
        if (err) *err = L"账本服务启动失败：" + ui::to_wide(svc.error().message);
        return false;
    }
    ledger_ = std::move(svc).value();
    data_dir = ledger_->config().data_dir;

    if (auto dirs = ledger_->config().ensure_directories(); dirs.is_err()) {
        if (err) *err = L"无法创建数据目录：" + ui::to_wide(dirs.error().message);
        return false;
    }

    Logger::default_logger().set_level(LogLevel::Info);
    Logger::default_logger().set_file(ledger_->config().log_file_path());
    Logger::default_logger().info("原生界面启动，数据目录 " + data_dir);

    // 构造期的通道配置只是兜底默认值；登录后用该账号的设置覆盖（见 apply_channels）。
    // 云端 base_url 传空 = 该通道不可用，避免在用户没配 Key 时误发请求。
    reports_ = std::make_unique<report::ReportService>(
        *ledger_, llm::LlmConfig::local_llama(), llm::LlmConfig::cloud_openai("", "", ""));

    entry_date_iso = today_iso();
    return true;
}

void App::shutdown() {
    g_shutting_down.store(true);
    // 会话只在内存里，进程退出即失效 —— 这里显式登出只是为了让日志与账本收尾
    if (!token.empty() && ledger_ != nullptr) {
        ledger_->logout(token);
        token.clear();
    }
    reports_.reset();
    ledger_.reset();
}

// -----------------------------------------------------------------------------
//  通道配置
// -----------------------------------------------------------------------------

void App::apply_channels() {
    if (token.empty() || ledger_ == nullptr || reports_ == nullptr) return;

    const app::ChannelResolution res = app::resolve_channels(*ledger_, token);

    std::string notes;
    for (const auto& n : res.notes) {
        if (!notes.empty()) notes += "\n";
        notes += n;
    }
    if (notes.empty()) notes = "通道解析正常，没有需要说明的事项。";
    channel_notes = notes;

    std::optional<llm::LlmConfig> cloud = res.cloud;
    std::optional<llm::LlmConfig> local;

    // 本地通道：框架在 core 里是完整的，但这个界面没有接「自动部署 llama.cpp +
    // 起 llama-server」那套流程。这里如实留空，并在 notes 里说明，
    // 而不是给一个「配置里写着本地、实际连不上」的假通道。
    reports_->set_channels(cloud, local);
}

// -----------------------------------------------------------------------------
//  设置读写
// -----------------------------------------------------------------------------

void App::load_llm_settings() {
    if (token.empty()) return;

    auto channel = ledger_->get_setting(token, llm::settings::kKeyChannel);
    std::string mode = channel.is_ok() && channel.value().has_value() ? *channel.value() : "auto";
    set_channel = (mode == "cloud") ? 1 : (mode == "local" ? 2 : 0);

    auto base = ledger_->get_setting(token, llm::settings::kKeyCloudBaseUrl);
    set_base_url = ui::to_wide(base.is_ok() && base.value().has_value()
                                   ? *base.value()
                                   : llm::settings::kDefaultCloudBaseUrl);

    auto model = ledger_->get_setting(token, llm::settings::kKeyCloudModel);
    set_model = ui::to_wide(model.is_ok() && model.value().has_value()
                                ? *model.value()
                                : llm::settings::kDefaultCloudModel);

    auto key = ledger_->get_secret(token, llm::settings::kKeyCloudApiKey);
    api_key_present = key.is_ok() && key.value().has_value() && !key.value()->empty();
    // 已存的 Key 不回填到输入框：界面上只显示「已保存」，要改就重新输一次。
    // 把密钥读回界面控件里，等于让它多一次出现在内存可视区与截图里的机会。
    set_api_key.clear();
}

void App::save_llm_settings() {
    if (token.empty()) return;

    const char* mode = set_channel == 1 ? "cloud" : (set_channel == 2 ? "local" : "auto");
    ledger_->set_setting(token, llm::settings::kKeyChannel, mode);

    const std::string base = ui::to_utf8(set_base_url);
    const std::string model = ui::to_utf8(set_model);
    if (!base.empty()) ledger_->set_setting(token, llm::settings::kKeyCloudBaseUrl, base);
    if (!model.empty()) ledger_->set_setting(token, llm::settings::kKeyCloudModel, model);

    if (!set_api_key.empty()) {
        auto r = ledger_->set_secret(token, llm::settings::kKeyCloudApiKey, ui::to_utf8(set_api_key));
        if (r.is_err()) {
            show_toast(L"API Key 保存失败：" + ui::to_wide(r.error().message), true);
            return;
        }
        set_api_key.clear();
        api_key_present = true;
    }

    apply_channels();
    show_toast(L"设置已保存", false);
}

// -----------------------------------------------------------------------------
//  数据刷新
// -----------------------------------------------------------------------------

std::string App::category_name_of(const std::string& id) {
    for (const auto& c : categories) {
        if (c.id == id) return c.name;
    }
    return {};
}

std::string App::category_id_of_name(const std::string& name) {
    for (const auto& c : categories) {
        if (c.name == name) return c.id;
    }
    return {};
}

std::vector<std::string> App::expense_category_names() {
    std::vector<std::string> out;
    for (const auto& c : categories) {
        if (c.direction == Direction::Expense && c.is_active) out.push_back(c.name);
    }
    // 约定：候选表最后一项是「其他」。
    // vision::parse_scan_reply 在对不上任何候选时会退回最后一项，
    // 所以要保证那确实是「其他」而不是某个具体分类。
    auto it = std::find(out.begin(), out.end(), std::string("其他"));
    if (it != out.end()) {
        const std::string tail = *it;
        out.erase(it);
        out.push_back(tail);
    }
    return out;
}

void App::refresh() {
    if (token.empty() || ledger_ == nullptr) return;

    auto snap = ledger_->snapshot(token);
    if (snap.is_err()) {
        show_toast(L"读取账本失败：" + ui::to_wide(snap.error().message), true);
        return;
    }
    records = std::move(snap.value().records);
    categories = std::move(snap.value().categories);

    const Date today = Date::today();
    const DateRange range = last_n_days(today, range_days);

    if (reports_ != nullptr) {
        // 一次拿全：汇总 + 日序列 + 趋势 + 异常。跑的是本地计算，不调模型。
        auto ana = reports_->analyze_range(token, range, 7, report::ReportKind::Monthly);
        if (ana.is_ok()) {
            dashboard = ana.value().dashboard;
            trend = ana.value().trend;
            anomalies = ana.value().anomalies;
            insights = ana.value().insights;
        } else {
            show_toast(L"统计计算失败：" + ui::to_wide(ana.error().message), true);
        }
    }

    data_note = "统计区间 " + range.from.to_string() + " ~ " + range.to.to_string() +
                "，共 " + std::to_string(records.size()) + " 条记录";

    // 记账页的默认选中分类：当前方向下第一个可用分类
    const Direction want = (entry_direction == 0) ? Direction::Expense : Direction::Income;
    if (entry_category_id.empty() || category_name_of(entry_category_id).empty()) {
        for (const auto& c : categories) {
            if (c.direction == want && c.is_active) {
                entry_category_id = c.id;
                break;
            }
        }
    }
    if (entry_date_iso.empty()) entry_date_iso = today.to_string();
}

// -----------------------------------------------------------------------------
//  导航与提示
// -----------------------------------------------------------------------------

void App::go(Screen s) {
    screen = s;
    if (s == Screen::Settings || s == Screen::Scan) load_llm_settings();

    if (s == Screen::Scan && !token.empty()) {
        load_scan_settings();
        if (scan_auto_scan) {
            // 每次进来都扫一遍：新放进目录的截图应该自己出现。
            // 重复扫描是廉价的（判重靠指纹），而且不扫的话「自动」就没意义了。
            const int added = scan_directory();
            if (added > 0) {
                show_toast(L"发现 " + std::to_wstring(added) + L" 张新截图，开始识别", false);
                scan_async();   // 内部会检查通道；没配模型时会逐张给出明确原因
            }
        }
    }
    if (repaint_) repaint_();
}

void App::show_toast(const std::wstring& text, bool is_error) {
    toast = text;
    toast_error = is_error;
    // 错误显示久一点：一句话没读完就消失，用户只会觉得「点了没反应」
    toast_deadline = monotonic_ms() + (is_error ? 6000 : 3200);
    if (repaint_) repaint_();
}

// -----------------------------------------------------------------------------
//  登录 / 注册（工作线程）
// -----------------------------------------------------------------------------

void App::login_async(std::string user, std::string pass, bool is_register) {
    if (busy || ledger_ == nullptr) return;
    if (user.empty() || pass.empty()) {
        login_error = "用户名和密码都要填";
        if (repaint_) repaint_();
        return;
    }

    busy = true;
    login_error.clear();
    busy_text = is_register ? L"正在创建账号：Argon2id 派生密钥中…"
                            : L"正在登录：Argon2id 派生密钥中…";
    if (repaint_) repaint_();

    auto notify = repaint_;
    std::thread([this, user, pass, is_register, notify]() {
        std::string err;
        app::AuthResult auth;

        if (g_shutting_down.load()) return;

        if (is_register) {
            auto r = ledger_->register_user(user, pass);
            if (r.is_err()) err = r.error().message;
        }
        if (err.empty()) {
            auto r = ledger_->login(user, pass);
            if (r.is_err()) {
                err = r.error().message;
            } else {
                auth = std::move(r).value();
            }
        }

        {
            std::lock_guard<std::mutex> lk(mu_);
            login_slot_.done = true;
            login_slot_.ok = err.empty();
            login_slot_.error = err;
            login_slot_.auth = std::move(auth);
        }
        if (notify) notify();
    }).detach();
}

void App::logout() {
    if (!token.empty() && ledger_ != nullptr) ledger_->logout(token);
    token.clear();
    username.clear();
    ledger_file.clear();
    records.clear();
    categories.clear();
    dashboard = analytics::Dashboard{};
    trend = analytics::TrendResult{};
    anomalies = analytics::AnomalyReport{};
    screen = Screen::Login;
    nav_index = 0;
    if (repaint_) repaint_();
}

// -----------------------------------------------------------------------------
//  记账
// -----------------------------------------------------------------------------

void App::save_entry() {
    if (token.empty()) return;
    entry_error.clear();

    const std::string amount_text = ui::to_utf8(entry_amount);
    if (amount_text.empty()) {
        entry_error = "请先填金额";
        return;
    }
    auto money = Money::parse(amount_text);
    if (money.is_err()) {
        entry_error = "金额格式不对：只接受数字，最多两位小数（例如 12.34）";
        return;
    }
    if (money.value().is_zero()) {
        entry_error = "金额不能是 0";
        return;
    }
    if (entry_category_id.empty()) {
        entry_error = "请选择一个分类";
        return;
    }

    RecordInput in;
    in.amount = money.value().abs();
    in.direction = (entry_direction == 0) ? Direction::Expense : Direction::Income;
    in.category_id = entry_category_id;
    auto d = Date::parse(entry_date_iso);
    in.date = d.is_ok() ? d.value() : Date::today();
    in.note = ui::to_utf8(entry_note);

    const bool editing = !editing_record_id.empty();
    if (editing) {
        auto r = ledger_->update_record(token, editing_record_id, in);
        if (r.is_err()) {
            entry_error = r.error().message;
            return;
        }
    } else {
        auto r = ledger_->add_record(token, in);
        if (r.is_err()) {
            entry_error = r.error().message;
            return;
        }
    }

    const std::wstring shown = ui::to_wide(money.value().abs().to_display_string());
    entry_amount.clear();
    entry_note.clear();
    editing_record_id.clear();
    refresh();
    show_toast(editing ? (L"已更新这笔 " + shown) : (L"已记账 " + shown), false);
}

void App::begin_edit(const std::string& record_id) {
    if (record_id.empty()) return;
    for (const auto& r : records) {
        if (r.id != record_id) continue;
        editing_record_id = r.id;
        entry_amount = ui::to_wide(r.amount.to_plain_string());
        entry_direction = (r.direction == Direction::Income) ? 1 : 0;
        entry_category_id = r.category_id;
        entry_date_iso = r.date.to_string();
        entry_note = ui::to_wide(r.note);
        entry_error.clear();
        screen = Screen::Entry;
        return;
    }
}

void App::cancel_edit() {
    editing_record_id.clear();
    entry_amount.clear();
    entry_note.clear();
    entry_error.clear();
}

void App::toggle_category(const std::string& category_id) {
    if (token.empty() || category_id.empty()) return;
    for (const auto& c : categories) {
        if (c.id != category_id) continue;
        const bool want = !c.is_active;
        auto r = ledger_->set_category_active(token, category_id, want);
        if (r.is_err()) {
            show_toast(L"操作失败：" + ui::to_wide(r.error().message), true);
            return;
        }
        // 正在记账时把当前选中的分类停用掉，要顺手清掉选中项，
        // 否则下一次记账会带着一个已经不在候选里的分类提交。
        if (!want && entry_category_id == category_id) entry_category_id.clear();
        refresh();
        return;
    }
}

void App::delete_record(const std::string& id) {
    if (token.empty() || id.empty()) return;
    auto r = ledger_->delete_record(token, id);
    if (r.is_err()) {
        show_toast(L"删除失败：" + ui::to_wide(r.error().message), true);
        return;
    }
    refresh();
    show_toast(L"已删除这笔记录", false);
}

// -----------------------------------------------------------------------------
//  报告（工作线程）
// -----------------------------------------------------------------------------

void App::generate_report_async(bool force) {
    if (busy || token.empty() || reports_ == nullptr) return;

    report::GenerateOptions opt;
    opt.kind = (report_kind == 0) ? report::ReportKind::Daily
                                  : (report_kind == 1 ? report::ReportKind::Weekly
                                                      : report::ReportKind::Monthly);
    opt.anchor = Date::today();
    opt.use_cache = !force;
    opt.force_regenerate = force;

    busy = true;
    busy_text = force ? L"正在重新生成报告（会调用模型，可能要几十秒）…"
                      : L"正在生成报告…";
    if (repaint_) repaint_();

    auto notify = repaint_;
    std::thread([this, opt, notify]() {
        if (g_shutting_down.load()) return;
        auto r = reports_->generate(token, opt);
        {
            std::lock_guard<std::mutex> lk(mu_);
            report_slot_.done = true;
            report_slot_.ok = r.is_ok();
            if (r.is_ok()) {
                report_slot_.result = std::move(r).value();
            } else {
                report_slot_.error = r.error().message;
            }
        }
        if (notify) notify();
    }).detach();
}

// -----------------------------------------------------------------------------
//  截图识别（工作线程）
// -----------------------------------------------------------------------------

void App::pick_screenshot() {
    std::vector<std::wstring> picked;
    if (!pick_image_files(picked) || picked.empty()) {
        if (!file_dialog_available()) {
            show_toast(L"这个系统上没有可用的文件对话框程序。"
                       L"装一个即可（Arch: sudo pacman -S zenity），"
                       L"或者到「设置」里指定截图目录后用「扫描目录」导入。", true);
        }
        return;   // 用户取消：什么都不用说
    }

    // 追加到现有列表（用户可以分几次选），已存在的路径跳过，避免重复识别同一张
    for (const std::wstring& full : picked) {
        const bool dup = std::any_of(scan_items.begin(), scan_items.end(),
                                     [&](const ScanItem& it) { return it.path == full; });
        if (dup) continue;

        ScanItem item;
        item.path = full;
        const size_t pos = full.find_last_of(L"\\/");
        item.label = (pos == std::wstring::npos) ? full : full.substr(pos + 1);

        int w = 0, h = 0;
        if (probe_image_size(item.path, w, h)) {
            item.dims = std::to_wstring(w) + L"×" + std::to_wstring(h);
        }
        scan_items.push_back(std::move(item));
    }

    // 选中第一张还没识别成功的，省得用户再点一下
    for (size_t i = 0; i < scan_items.size(); ++i) {
        if (!scan_items[i].draft.ready && !scan_items[i].applied) {
            scan_selected = static_cast<int>(i);
            break;
        }
    }
    if (repaint_) repaint_();
}

void App::drop_scan(int index) {
    if (index < 0 || index >= static_cast<int>(scan_items.size())) return;
    scan_items.erase(scan_items.begin() + index);
    if (scan_selected >= static_cast<int>(scan_items.size())) {
        scan_selected = static_cast<int>(scan_items.size()) - 1;
    }
    if (repaint_) repaint_();
}

void App::inject_scan_result_for_diag(int index, ScanDraft draft) {
    // 和 scan_async 的工作线程做的事完全一样：加锁 → 往结果队列放一条 → 唤醒界面。
    // 这里必须真的加锁 —— 它在模拟「另一个线程的写入」，
    // 而正是「pump 拿着锁再去锁同一把锁」把程序搞崩的。
    std::lock_guard<std::mutex> lk(mu_);
    scan_slot_.results.push_back(ScanResult{index, std::move(draft)});
    scan_slot_.current = index + 1;
    scan_slot_.total = index + 1;
    scan_slot_.done = true;
}

void App::clear_scans() {
    scan_items.clear();
    scan_selected = -1;
    if (repaint_) repaint_();
}

ScanDraft& App::cur_draft() {
    if (scan_selected >= 0 && scan_selected < static_cast<int>(scan_items.size())) {
        return scan_items[static_cast<size_t>(scan_selected)].draft;
    }
    // 没有选中时的丢弃槽：写进来的值无人读取，读到的都是空。
    // 这样页面代码可以无条件 app.cur_draft().xxx，不用到处判空。
    static ScanDraft kDiscard;
    kDiscard = ScanDraft{};
    return kDiscard;
}

ScanItem* App::cur_item() {
    if (scan_selected >= 0 && scan_selected < static_cast<int>(scan_items.size())) {
        return &scan_items[static_cast<size_t>(scan_selected)];
    }
    return nullptr;
}

/// 识别一张图。在工作线程里跑，所以**不碰任何 UI 状态**，只返回一份草稿。
static ScanDraft recognize_one(const std::wstring& path, const llm::LlmConfig& cfg,
                               const std::vector<std::string>& candidates,
                               const std::string& today) {
    ScanDraft draft;
    if (g_shutting_down.load()) return draft;

    std::string data_url;
    std::wstring ierr;
    // 缩到最长边 1600：手机截图常见的 1080×2400 缩完约 700KB JPEG，
    // 既远低于接口限制，又足够看清金额和商户名。
    if (!make_image_data_url(path, 1600, data_url, &ierr)) {
        draft.fail_reason = ui::to_utf8(ierr);
        return draft;
    }

    vision::ScanRequest req;
    req.image_data_url = std::move(data_url);
    req.category_candidates = candidates;
    req.today_iso = today;

    auto r = vision::scan_with_vision(req, cfg);
    if (r.is_err()) {
        draft.fail_reason = "调用模型失败：" + r.error().message;
        return draft;
    }

    const vision::ScanOutcome& out = r.value();
    draft.raw_reply = out.raw_reply;
    draft.method = out.provider_label;
    if (out.ok) {
        draft.ready = true;
        draft.amount_minor = out.amount_minor;
        draft.direction = (out.direction == Direction::Income) ? 1 : 0;
        draft.category_name = out.category_name;
        draft.merchant = out.merchant;
        draft.date_iso = out.date_iso;
        draft.note = out.note;
        draft.confidence = out.confidence;
    } else {
        draft.fail_reason = out.fail_reason;
    }
    return draft;
}

void App::scan_async() {
    if (busy || token.empty()) return;
    if (scan_items.empty()) {
        show_toast(L"还没有选图片。先点「选择截图」。", true);
        return;
    }

    const app::ChannelResolution res = app::resolve_channels(*ledger_, token);
    if (!res.cloud.has_value()) {
        // 没有可用模型时不假装识别。把原因如实说清楚，用户才知道该去配什么。
        for (ScanItem& it : scan_items) {
            if (it.applied) continue;
            it.state = ScanItem::State::Failed;
            it.fail_reason =
                "没有可用的模型通道，无法识别截图。\n"
                "到「设置」里填一个支持图片输入的云端模型（例如 gpt-4o / qwen-vl-max /\n"
                "glm-4v 这类的 base_url、模型名和 API Key），保存后再试。";
        }
        if (repaint_) repaint_();
        return;
    }

    // 只处理还没成功识别、也没入库的那些
    std::vector<std::pair<int, std::wstring>> todo;
    for (size_t i = 0; i < scan_items.size(); ++i) {
        if (scan_items[i].applied) continue;
        if (scan_items[i].draft.ready) continue;
        scan_items[i].state = ScanItem::State::Queued;
        scan_items[i].fail_reason.clear();
        todo.emplace_back(static_cast<int>(i), scan_items[i].path);
    }
    if (todo.empty()) {
        show_toast(L"这批图都已经识别过了（可以逐张确认入库）", false);
        return;
    }

    const llm::LlmConfig cfg = *res.cloud;
    const std::vector<std::string> candidates = expense_category_names();
    const std::string today = today_iso();
    const size_t total = todo.size();

    busy = true;
    busy_text = L"正在识别截图 1/" + std::to_wstring(total);
    {
        std::lock_guard<std::mutex> lk(mu_);
        scan_slot_ = ScanSlot{};
        scan_slot_.total = static_cast<int>(total);
    }
    if (repaint_) repaint_();

    auto notify = repaint_;
    // 顺序逐张而不是并发：多模态接口对并发并不友好（很容易撞限流），
    // 而且顺序跑才有稳定的进度可言。
    std::thread([this, cfg, candidates, today, notify, todo = std::move(todo)]() {
        for (size_t k = 0; k < todo.size(); ++k) {
            if (g_shutting_down.load()) return;
            {
                std::lock_guard<std::mutex> lk(mu_);
                scan_slot_.current = static_cast<int>(k);
            }
            if (notify) notify();   // 让「第几张」先显示出来

            // 单张识别出错绝不能连累整个程序：工作线程里逃出去的异常
            // 会一路走到 std::terminate（进程直接消失），而用户只是识别了一张图。
            // 在这一层兜住，把它转成这张的可读失败原因 —— 失败是有价值的输出，
            // 闪退不是。注意区分「识别不出来」和「程序出错」：
            // 前者是模型的判断，后者是我们的 bug，不该让用户去猜。
            ScanDraft draft;
            try {
                draft = recognize_one(todo[k].second, cfg, candidates, today);
            } catch (const std::exception& e) {
                draft.fail_reason = std::string("识别这张时程序内部出错：") + e.what();
            } catch (...) {
                draft.fail_reason = "识别这张时程序内部出错（未知异常）";
            }
            {
                std::lock_guard<std::mutex> lk(mu_);
                scan_slot_.results.push_back(ScanResult{todo[k].first, std::move(draft)});
            }
            if (notify) notify();   // 每完成一张就刷新，用户能看到逐个出结果
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            scan_slot_.done = true;
        }
        if (notify) notify();
    }).detach();
}

// -----------------------------------------------------------------------------
//  截图目录：设置、扫描、自动入账
// -----------------------------------------------------------------------------

namespace {

/// 支持的图片扩展名（小写，含点）
bool is_image_ext(const std::wstring& lower_ext) {
    static const wchar_t* kExts[] = {L".png", L".jpg", L".jpeg", L".bmp",
                                     L".webp", L".tif", L".tiff"};
    for (const wchar_t* e : kExts) {
        if (lower_ext == e) return true;
    }
    return false;
}

std::wstring lower_ext_of(const std::filesystem::path& p) {
    std::wstring e = p.extension().wstring();
    for (wchar_t& c : e) c = static_cast<wchar_t>(towlower(c));
    return e;
}

/// 指纹记录的字段分隔符。用 US（0x1F）而不是逗号或竖线：
/// Windows 文件名禁止控制字符，所以它绝不可能出现在路径里，
/// 也就不需要转义那一套（引 JSON 库进来做这件事不值得）。
constexpr char kSep = '\x1f';

}  // namespace

bool App::is_seen(const std::string& path, long long size, long long mtime) const {
    for (const SeenEntry& e : scan_seen_) {
        // 三个都对上才算「同一张图」：路径相同但大小/时间变了 = 文件被替换过，
        // 那应该重新识别（比如用户重新截了一张同名图）。
        if (e.path == path && e.size == size && e.mtime == mtime) return true;
    }
    return false;
}

bool App::already_listed(const std::string& utf8_path) const {
    for (const ScanItem& it : scan_items) {
        if (ui::to_utf8(it.path) == utf8_path) return true;
    }
    return false;
}

void App::remember_seen(const std::string& path, long long size, long long mtime,
                        bool applied) {
    for (SeenEntry& e : scan_seen_) {
        if (e.path == path && e.size == size && e.mtime == mtime) {
            e.applied = applied;
            save_seen();
            return;
        }
    }
    scan_seen_.push_back(SeenEntry{path, size, mtime, applied});
    // 只留最近的若干条：这是防重复的辅助数据，不是账本，不需要永久保存
    if (static_cast<int>(scan_seen_.size()) > vision::settings::kScanSeenLimit) {
        scan_seen_.erase(scan_seen_.begin(),
                         scan_seen_.begin() +
                             (scan_seen_.size() -
                              static_cast<size_t>(vision::settings::kScanSeenLimit)));
    }
    save_seen();
}

void App::mark_seen_applied(const std::wstring& path) {
    const std::string utf8 = ui::to_utf8(path);
    long long size = 0, mtime = 0;
    if (!file_stamp(path, &size, &mtime)) return;
    remember_seen(utf8, size, mtime, true);
}

void App::load_seen() {
    scan_seen_.clear();
    if (token.empty()) return;

    auto r = ledger_->get_setting(token, vision::settings::kKeyScanSeen);
    if (r.is_err() || !r.value().has_value()) return;

    const std::string& blob = *r.value();
    size_t pos = 0;
    while (pos < blob.size()) {
        size_t nl = blob.find('\n', pos);
        if (nl == std::string::npos) nl = blob.size();
        const std::string line = blob.substr(pos, nl - pos);
        pos = nl + 1;
        if (line.empty()) continue;

        // applied / size / mtime / path（以 kSep 分隔）
        const size_t s1 = line.find(kSep);
        if (s1 == std::string::npos) continue;
        const size_t s2 = line.find(kSep, s1 + 1);
        if (s2 == std::string::npos) continue;
        const size_t s3 = line.find(kSep, s2 + 1);
        if (s3 == std::string::npos) continue;

        SeenEntry e;
        e.applied = (line.substr(0, s1) == "1");
        try {
            e.size = std::stoll(line.substr(s1 + 1, s2 - s1 - 1));
            e.mtime = std::stoll(line.substr(s2 + 1, s3 - s2 - 1));
        } catch (...) {
            continue;   // 坏行直接跳过，不要让一条脏数据毁掉整份记录
        }
        e.path = line.substr(s3 + 1);
        scan_seen_.push_back(std::move(e));
    }
}

void App::save_seen() {
    if (token.empty()) return;
    std::string blob;
    for (const SeenEntry& e : scan_seen_) {
        blob += (e.applied ? "1" : "0");
        blob += kSep;
        blob += std::to_string(e.size);
        blob += kSep;
        blob += std::to_string(e.mtime);
        blob += kSep;
        blob += e.path;
        blob += '\n';
    }
    ledger_->set_setting(token, vision::settings::kKeyScanSeen, blob);
}

void App::load_scan_settings() {
    if (token.empty()) return;

    auto dir = ledger_->get_setting(token, vision::settings::kKeyScanDir);
    if (dir.is_ok() && dir.value().has_value()) {
        scan_dir = ui::to_wide(*dir.value());
    }
    if (scan_dir.empty()) {
        // 默认落在系统「图片」文件夹下的「账本」子目录。
        // 两个平台都不硬编码用户名/路径：Windows 走 KnownFolder，
        // Linux 读 XDG user-dirs（用户可能把「图片」改到别处了）。
        scan_dir = default_screenshot_dir();
    }

    auto autos = ledger_->get_setting(token, vision::settings::kKeyScanAutoScan);
    scan_auto_scan = (autos.is_ok() && autos.value().has_value() && *autos.value() == "1");
    auto autoa = ledger_->get_setting(token, vision::settings::kKeyScanAutoApply);
    scan_auto_apply =
        (autoa.is_ok() && autoa.value().has_value() && *autoa.value() == "1");
    auto autoc = ledger_->get_setting(token, vision::settings::kKeyScanAutoClean);
    scan_auto_clean =
        (autoc.is_ok() && autoc.value().has_value() && *autoc.value() == "1");

    load_seen();
}

void App::load_ui_settings() {
    if (token.empty()) return;
    auto r = ledger_->get_setting(token, penhu::ui::settings::kKeyNavExpanded);
    if (r.is_ok() && r.value().has_value()) {
        nav_expanded = (*r.value() == "1");
    } else {
        // 从没设置过：按窗口宽度给个合理默认 ——
        // 208dp 的侧边栏在 760 宽的窗口里要占掉近三成，那种宽度下默认该收起来。
        // 用户手动改过之后就完全按用户的选择（上面那个分支）。
        nav_expanded = (width_class != WidthClass::Compact);
    }
}

void App::save_ui_settings() {
    if (token.empty()) return;
    ledger_->set_setting(token, penhu::ui::settings::kKeyNavExpanded,
                         nav_expanded ? "1" : "0");
}

void App::save_scan_settings() {
    if (token.empty()) return;
    ledger_->set_setting(token, vision::settings::kKeyScanDir, ui::to_utf8(scan_dir));
    ledger_->set_setting(token, vision::settings::kKeyScanAutoScan,
                         scan_auto_scan ? "1" : "0");
    ledger_->set_setting(token, vision::settings::kKeyScanAutoApply,
                         scan_auto_apply ? "1" : "0");
    ledger_->set_setting(token, vision::settings::kKeyScanAutoClean,
                         scan_auto_clean ? "1" : "0");
}

void App::pick_scan_dir() {
    std::wstring picked;
    if (pick_directory(picked) && !picked.empty()) {
        scan_dir = picked;
        save_scan_settings();
        show_toast(L"截图目录已设置", false);
    } else if (!file_dialog_available()) {
        // 取消和「这台机器上没有可用的对话框程序」是两回事：
        // 前者不用说话，后者必须说清楚缺什么 —— 「点了没反应」最让人上火。
        show_toast(L"这个系统上没有可用的文件对话框程序。装一个即可（Arch: sudo pacman -S zenity）", true);
    }
    if (repaint_) repaint_();
}

int App::scan_directory() {
    scan_new_last = 0;
    if (token.empty()) return 0;
    if (scan_dir.empty()) {
        show_toast(L"还没有设置截图目录", true);
        return 0;
    }

    std::error_code ec;
    const std::filesystem::path dir(scan_dir);
    if (!std::filesystem::is_directory(dir, ec)) {
        show_toast(L"截图目录不存在或打不开：" + scan_dir, true);
        return 0;
    }

    struct Found {
        std::wstring path;
        long long    mtime{0};
    };
    std::vector<Found> fresh;

    for (const auto& entry : std::filesystem::directory_iterator(
             dir, std::filesystem::directory_options::skip_permission_denied, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;

        const std::wstring lower_ext = lower_ext_of(entry.path());
        if (!is_image_ext(lower_ext)) continue;

        const std::wstring full = entry.path().wstring();
        long long size = 0, mtime = 0;
        if (!file_stamp(full, &size, &mtime)) continue;

        const std::string utf8 = ui::to_utf8(full);
        if (is_seen(utf8, size, mtime)) continue;   // 以前处理过
        if (already_listed(utf8)) continue;         // 这一轮已经在列表里了

        fresh.push_back(Found{full, mtime});
    }

    // 按修改时间排序：截图天然是按时间来的，先处理早的更符合「按顺序记账」
    std::sort(fresh.begin(), fresh.end(),
              [](const Found& a, const Found& b) { return a.mtime < b.mtime; });

    for (const Found& f : fresh) {
        ScanItem item;
        item.path = f.path;
        const size_t pos = f.path.find_last_of(L"\\/");
        item.label = (pos == std::wstring::npos) ? f.path : f.path.substr(pos + 1);
        int w = 0, h = 0;
        if (probe_image_size(item.path, w, h)) {
            item.dims = std::to_wstring(w) + L"×" + std::to_wstring(h);
        }
        scan_items.push_back(std::move(item));
    }

    scan_new_last = static_cast<int>(fresh.size());
    if (scan_new_last > 0 && scan_selected < 0) scan_selected = 0;
    if (repaint_) repaint_();
    return scan_new_last;
}

void App::maybe_clean_source(int index) {
    if (!scan_auto_clean) return;
    if (index < 0 || index >= static_cast<int>(scan_items.size())) return;
    ScanItem& it = scan_items[static_cast<size_t>(index)];
    if (!it.applied || it.source_removed) return;   // 没入账 / 已经清过，都不动
    if (it.path.empty()) return;

    if (move_to_trash(it.path)) {
        it.source_removed = true;
        Logger::default_logger().info("已把原图移入回收站：" + ui::to_utf8(it.path));
    } else {
        // 移不动（文件被占用、目录只读等）不该让流程失败 —— 账已经记好了，
        // 这只是清理动作。如实说一句原因即可。
        Logger::default_logger().warn("原图移入回收站失败：" + ui::to_utf8(it.path));
    }
}

bool App::auto_apply(int index) {
    if (index < 0 || index >= static_cast<int>(scan_items.size())) return false;
    ScanItem& it = scan_items[static_cast<size_t>(index)];
    if (it.applied || !it.draft.ready) return false;

    // ---- 安全阀：全自动模式下，这里是唯一能拦住错账的地方 ----
    const ScanDraft& d = it.draft;
    if (d.amount_minor <= 0) {
        it.fail_reason = "自动入账被拦下：识别出的金额不是正数（不拿别的数字顶替）";
        it.state = ScanItem::State::Failed;
        return false;
    }
    if (d.amount_minor > 100000000LL) {   // 100 万元
        it.fail_reason = "自动入账被拦下：金额超过 100 万元，可能是识别错误，请人工核对";
        it.state = ScanItem::State::Failed;
        return false;
    }
    if (d.date_iso.empty()) {
        it.fail_reason = "自动入账被拦下：没能识别出日期";
        it.state = ScanItem::State::Failed;
        return false;
    }

    RecordInput in;
    in.amount = Money::from_minor(d.amount_minor);
    in.direction = (d.direction == 1) ? Direction::Income : Direction::Expense;
    in.category_id = d.category_id;
    if (in.category_id.empty()) in.category_id = category_id_of_name(d.category_name);
    if (in.category_id.empty()) in.category_id = category_id_of_name("其他");
    auto dt = Date::parse(d.date_iso);
    in.date = dt.is_ok() ? dt.value() : Date::today();
    in.note = !d.note.empty() ? d.note : d.merchant;

    auto r = ledger_->add_record(token, in);
    if (r.is_err()) {
        it.fail_reason = "自动入账失败：" + r.error().message;
        it.state = ScanItem::State::Failed;
        return false;
    }

    it.applied = true;
    it.state = ScanItem::State::Done;
    mark_seen_applied(it.path);
    maybe_clean_source(index);
    return true;
}

void App::confirm_scan() {
    ScanItem* item = cur_item();
    if (item == nullptr || token.empty()) return;
    if (item->applied) {
        // 已经入过库就别再来一次 —— 重复记账比识别失败更麻烦，用户得自己去删。
        show_toast(L"这一张已经记过账了", false);
        return;
    }
    ScanDraft& d = item->draft;
    if (!d.ready) {
        show_toast(L"这张还没有识别结果，先点「开始识别」", true);
        return;
    }

    RecordInput in;
    in.amount = Money::from_minor(d.amount_minor);
    in.direction = (d.direction == 1) ? Direction::Income : Direction::Expense;
    in.category_id = d.category_id;
    if (in.category_id.empty()) in.category_id = category_id_of_name(d.category_name);
    auto dt = Date::parse(d.date_iso);
    in.date = dt.is_ok() ? dt.value() : Date::today();

    // 备注优先用模型给的说明，其次用商户名 —— 两者都没有就留空，
    // 不写「截图识别」这种没信息量的占位文本。
    in.note = !d.note.empty() ? d.note : d.merchant;

    if (in.category_id.empty()) {
        show_toast(L"识别出的分类在本账号里不存在，请手选一个再记", true);
        return;
    }

    auto r = ledger_->add_record(token, in);
    if (r.is_err()) {
        show_toast(L"记账失败：" + ui::to_wide(r.error().message), true);
        return;
    }

    const std::wstring shown = ui::to_wide(Money::from_minor(d.amount_minor).abs().to_display_string());
    item->applied = true;
    item->state = ScanItem::State::Done;
    // 记下「这张图已经进过账本」：目录扫描靠它避免重复记账
    mark_seen_applied(item->path);
    // 手动确认入账的同样按设置清理原图（逻辑与自动入账一致）
    for (size_t i = 0; i < scan_items.size(); ++i) {
        if (&scan_items[i] == item) {
            maybe_clean_source(static_cast<int>(i));
            break;
        }
    }

    // 自动跳到下一张还没入库的 —— 批量记账时省掉「回去点列表」这一步，
    // 这是多图相比单图唯一值得多做的自动化。
    for (size_t i = 0; i < scan_items.size(); ++i) {
        if (!scan_items[i].applied && scan_items[i].draft.ready) {
            scan_selected = static_cast<int>(i);
            break;
        }
    }

    refresh();
    show_toast(L"已记入账本 " + shown, false);
}

// -----------------------------------------------------------------------------
//  结果回收（UI 线程）
// -----------------------------------------------------------------------------

void App::pump() {
    bool need_refresh = false;
    bool need_repaint = false;

    {
        std::lock_guard<std::mutex> lk(mu_);

        if (login_slot_.done) {
            // 登录后把界面偏好也读出来（折叠状态等）——
            // 这些设置是每账号的，必须等有了 token 才能读。
            load_ui_settings();
            LoginSlot s = std::move(login_slot_);
            login_slot_ = LoginSlot{};
            busy = false;
            if (s.ok) {
                token = s.auth.token;
                username = ui::to_wide(s.auth.user.username);
                auto lf = ledger_->ledger_filename(token);
                ledger_file = lf.is_ok() ? lf.value() : std::string{};
                login_error.clear();
                screen = Screen::Entry;
                nav_index = 0;
                apply_channels();
                load_llm_settings();
                need_refresh = true;
                need_repaint = true;
            } else {
                login_error = s.error;
                need_repaint = true;
            }
        }

        if (report_slot_.done) {
            ReportSlot s = std::move(report_slot_);
            report_slot_ = ReportSlot{};
            busy = false;
            if (s.ok) {
                report_text = s.result.content;
                report_provider = s.result.provider;
                report_cache = s.result.from_cache;
                report_fallback = s.result.used_fallback || s.result.stale_model_output;
                report_notes = s.result.failover_notes;
                report_meta = s.result.model.empty() ? s.result.provider
                                                     : (s.result.provider + " / " + s.result.model);
            } else {
                report_text.clear();
                report_provider.clear();
                report_notes = {"生成失败：" + s.error};
                need_repaint = true;
            }
            need_repaint = true;
        }

        if (scan_slot_.done || !scan_slot_.results.empty()) {
            std::vector<ScanResult>       fresh;
            int                           current = 0;
            int                           total = 0;
            bool                          all_done = false;
            // 外层那个 lock_guard 已经持有 mu_ 了，这里**绝不能再锁一次**。
            // std::mutex 是非递归的：同一个线程重复 lock 会抛 std::system_error，
            // 而这个异常没人接 → std::terminate → abort()，
            // 表现就是「一点识别，整个程序瞬间消失、连日志都不留一行」。
            // 这个 bug 真发生过（2026-09-21）：识别流程只要一启动就闪退。
            fresh.swap(scan_slot_.results);
            current = scan_slot_.current;
            total = scan_slot_.total;
            all_done = scan_slot_.done;
            if (all_done) {
                scan_slot_ = ScanSlot{};
            }

            // 把刚完成的那几张并进列表
            for (ScanResult& r : fresh) {
                if (r.index < 0 || r.index >= static_cast<int>(scan_items.size())) continue;
                ScanItem& it = scan_items[static_cast<size_t>(r.index)];
                it.draft = std::move(r.draft);
                it.fail_reason = it.draft.fail_reason;
                if (it.draft.ready) {
                    it.state = ScanItem::State::Done;
                    // 分类名对齐到本账号的分类 id —— 模型给的是名字，库里要的是 id
                    if (!it.draft.category_name.empty()) {
                        it.draft.category_id = category_id_of_name(it.draft.category_name);
                    }
                    // 识别成功的立刻记指纹：下次扫描目录时跳过它。
                    // 注意识别**失败**的不记 —— 那样用户改好模型配置后还能重试。
                    long long sz = 0, mt = 0;
                    if (file_stamp(it.path, &sz, &mt)) {
                        remember_seen(ui::to_utf8(it.path), sz, mt, false);
                    }
                } else {
                    it.state = ScanItem::State::Failed;
                }
                // 识别成功但当前没选中任何一张时，自动选中它，省一次点击
                if (it.draft.ready && scan_selected < 0) {
                    scan_selected = r.index;
                }
            }
            // 正在处理的那张标成「识别中」，列表里才看得出进行到哪儿
            if (!all_done) {
                const size_t idx = static_cast<size_t>(current);
                if (idx < scan_items.size() && !scan_items[idx].applied) {
                    scan_items[idx].state = ScanItem::State::Running;
                }
            }

            // 自动入账：每张结果一到就试着写库，不等整批 ——
            // 这样「放进目录 → 一会儿打开记录页，账已经记好了」才成立。
            if (scan_auto_apply && !fresh.empty()) {
                int applied_n = 0;
                for (size_t i = 0; i < scan_items.size(); ++i) {
                    if (scan_items[i].applied || !scan_items[i].draft.ready) continue;
                    if (auto_apply(static_cast<int>(i))) ++applied_n;
                }
                if (applied_n > 0) {
                    need_refresh = true;
                    show_toast(L"已自动记账 " + std::to_wstring(applied_n) + L" 张", false);
                }
            }

            if (all_done) {
                busy = false;
                if (total > 0) {
                    int ok_n = 0, fail_n = 0, applied_n = 0;
                    for (const ScanItem& it : scan_items) {
                        if (it.applied) ++applied_n;
                        if (it.draft.ready) ++ok_n;
                        else if (it.state == ScanItem::State::Failed) ++fail_n;
                    }
                    if (applied_n > 0) {
                        show_toast(L"识别完成：成功 " + std::to_wstring(ok_n) + L" 张，其中 " +
                                       std::to_wstring(applied_n) + L" 张已自动记账",
                                   fail_n > 0);
                    } else {
                        show_toast(L"识别完成：成功 " + std::to_wstring(ok_n) +
                                       L" 张，失败 " + std::to_wstring(fail_n) + L" 张",
                                   ok_n == 0);
                    }
                }
            } else if (total > 0) {
                busy = true;
                busy_text = L"正在识别截图 " + std::to_wstring(current + 1) + L"/" +
                            std::to_wstring(total);
            }
            need_repaint = true;
        }
    }

    if (need_refresh) refresh();
    if (need_repaint && repaint_) repaint_();
}

}  // namespace penhu::native
