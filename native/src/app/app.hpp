#pragma once
// =============================================================================
//  native/app/app.hpp
//  应用状态 + 业务操作。界面层（pages/）只读它的状态、调它的方法。
//
//  分层用意和原来一致（core 是唯一业务门面），只是换成原生界面后多了一条：
//  **界面不直接调 storage / crypto**，全部经 app::LedgerService —— 这条约束没变。
//
//  异步的三条通道（登录 / 出报告 / 截图识别）都会阻塞秒级到分钟级，
//  必须在工作线程跑，否则窗口会假死。做法统一是：
//    工作线程 → 写「结果槽」（加锁）→ 调 repaint_ 发消息唤醒 UI 线程
//    UI 线程在 pump() 里取走结果并更新状态
//  工作线程一律不碰组件树，避免两个线程同时改同一棵树。
// =============================================================================

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "penhu/analytics/anomaly.hpp"
#include "penhu/analytics/insight.hpp"
#include "penhu/analytics/summary.hpp"
#include "penhu/analytics/trend.hpp"
#include "penhu/app/ledger_service.hpp"
#include "penhu/report/report_service.hpp"

namespace penhu::native {

namespace ui {
class VBox;
}  // namespace ui

// Records 追加在末尾而不是插在 Entry 后面：诊断模式的日志里写过
// 「期望 2=Stats」这种按枚举值核对的断言，插中间会让所有旧值移位。
// 导航栏上的显示顺序与此无关（kMap 显式映射），想把「明细」排在
// 「记账」后面不受这里约束。
enum class Screen { Login, Entry, Stats, Report, Scan, Settings, Records };

/// 窗口宽度档。**多栏布局只在宽屏上做** —— 窄窗口里硬排两栏，
/// 每一栏都挤得放不下东西，还不如老老实实单栏滚动。
enum class WidthClass {
    Compact,    // < 820dp：手机/窄窗
    Medium,     // 820–1180：小笔记本，单栏但可以更宽
    Expanded,   // >= 1180：桌面/平板横屏，可以上多栏
};

/// 截图识别出来的支出草稿。**必须经用户确认才写账**。
struct ScanDraft {
    bool        ready{false};
    int64_t     amount_minor{0};
    int         direction{0};          // 0 支出 / 1 收入
    std::string category_id;
    std::string category_name;
    std::string merchant;
    std::string date_iso;
    std::string note;
    double      confidence{0.0};
    std::string method;                // 用了哪个通道（要给用户看）
    std::string fail_reason;           // 失败原因（可读中文）
    std::string raw_reply;             // 原始回复，排障用
};

/// 一张待识别的截图，连同它的识别结果。
/// 支持一次选多张，所以每张各有各的状态和草稿 —— 一张失败不影响其它张。
struct ScanItem {
    enum class State { Queued, Running, Done, Failed };

    std::wstring path;          // 绝对路径（发给模型前才读盘）
    std::wstring label;         // 显示用文件名
    std::wstring dims;          // "1080×2400"
    State        state{State::Queued};
    ScanDraft    draft;         // 识别出的草稿（用户可改）
    std::string  fail_reason;   // 这张为什么没识别出来
    bool         applied{false};// 是否已记入账本（防重复入库）
    bool         source_removed{false};  // 原图是否已按设置移入回收站

    bool identified() const { return draft.ready; }
};

class App {
public:
    static App& get();

    bool init(const std::string& data_dir, std::wstring* err);
    void shutdown();

    /// shell 注入：把「重绘请求」接到 PostMessage 上
    void set_repaint(std::function<void()> fn) { repaint_ = std::move(fn); }

    /// 请求「重建组件树 + 重绘」。页面里的结构性变化（切 tab、切模式）用它。
    /// 注意：输入框的 on_change **不能**调它 —— 重建会把正在输入的控件换掉，
    /// 光标和输入法未提交的内容都会丢。
    void rebuild_ui() {
        if (repaint_) repaint_();
    }

    // -------------------------------------------------------------------------
    //  基础设施
    // -------------------------------------------------------------------------
    app::LedgerService*    ledger() { return ledger_.get(); }
    report::ReportService* reports() { return reports_.get(); }
    bool        ready() const { return ledger_ != nullptr; }
    std::string data_dir;

    // -------------------------------------------------------------------------
    //  会话
    // -------------------------------------------------------------------------
    std::string  token;
    std::wstring username;
    std::string  ledger_file;
    std::string  login_error;      // 登录页显示的错误
    int          login_mode{0};    // 0 登录 / 1 注册
    std::wstring login_username;   // 输入内容存在 App 里，
    std::wstring login_password;   // 这样重建组件树时不会丢（见 shell.hpp 的说明）
    bool         dark{false};

    // -------------------------------------------------------------------------
    //  数据缓存（每次 refresh() 重取）
    // -------------------------------------------------------------------------
    std::vector<Category>          categories;   // 全部（含收入方向）
    std::vector<Record>            records;      // 全量
    analytics::Dashboard           dashboard;
    analytics::TrendResult         trend;
    analytics::AnomalyReport       anomalies;
    analytics::InsightSet          insights;     // 规则给出的建议（带严重度与依据）
    std::string                    data_note;

    // -------------------------------------------------------------------------
    //  界面状态
    // -------------------------------------------------------------------------
    Screen screen{Screen::Login};
    int    nav_index{0};        // 主界面：0 记账 1 统计 2 报告

    // 记账页
    int          entry_direction{0};
    std::string  entry_category_id;
    std::wstring entry_amount;
    std::wstring entry_note;
    std::string  entry_date_iso;
    std::string  entry_error;
    /// 非空 = 正在编辑这一笔（保存按钮变成「更新」，并出现删除按钮）
    std::string  editing_record_id;
    /// 分类网格是否展开了全部（默认只显示前 12 个）：
    /// 预置支出分类有 41 个，全铺开是 7 行，把「记一笔」的主流程推到屏幕外。
    bool         entry_show_all_categories{false};

    // 统计页
    int range_days{30};

    // 全部记录页（筛选 + 翻页看完整账本）
    int          rec_filter_dir{0};    // 0 全部 / 1 只看支出 / 2 只看收入
    std::string  rec_filter_cat;       // 空 = 全部分类
    /// 备注关键词。注意和其它 TextField 一样：on_change 只存值不重建树，
    /// 回车或点「筛选」才生效（重建会把正在输入的框换掉，光标会丢）。
    std::wstring rec_keyword;
    int          rec_page{0};          // 0 基页号
    bool         rec_cats_expanded{false};

    // 报告页
    int                      report_kind{0};      // 0 日报 / 1 周报 / 2 月报
    std::string              report_text;
    std::string              report_provider;
    std::string              report_meta;
    bool                     report_cache{false};
    bool                     report_fallback{false};
    std::vector<std::string> report_notes;

    // 截图识别页。一次可以选多张，逐张识别、逐张确认入库。
    std::vector<ScanItem> scan_items;
    /// 正在查看/编辑第几张（-1 = 没有选中）。
    /// 列表只显示「文件名 + 状态 + 金额」，编辑区只放当前这一张 ——
    /// 每张都铺一整套金额/分类/日期控件的话，五张图就能把页面撑爆。
    int                   scan_selected{-1};
    /// 识别页的分类网格是否展开（40 多个支出分类全铺开会把详情区拉得很长）
    bool                  scan_cats_expanded{false};

    /// 左侧导航栏是否展开（折叠后只剩图标，给内容让宽度）。
    /// 持久化在账号设置里（ui.nav_expanded），下次打开保持上次的状态。
    bool nav_expanded{true};

    /// 当前窗口宽度档。由 Shell 在尺寸变化时更新（不是每帧算）。
    WidthClass width_class{WidthClass::Medium};
    /// 宽屏：可以做多栏。页面代码用它决定「并排还是堆叠」。
    bool wide() const { return width_class == WidthClass::Expanded; }

    /// 页面内容的最大宽度。宽屏放宽，窄屏收窄 ——
    /// 它不是「固定版心」，而是「一行文字别长到看不过来」的上限。
    float content_max_width() const {
        switch (width_class) {
            case WidthClass::Expanded: return 1400.0f;
            case WidthClass::Medium:   return 980.0f;
            default:                   return 800.0f;
        }
    }

    // 截图批量导入（对应「截图目录」那一组设置）
    /// 截图保存目录。默认取系统的「图片」文件夹下的 `账本`，
    /// 这样不用硬编码用户名，换台机器也能用。
    std::wstring scan_dir;
    /// 打开识图页时自动扫描目录、把没处理过的图加进来并开始识别
    bool scan_auto_scan{false};
    /// 识别成功后直接写进账本，不再逐张确认。
    /// **默认关闭**：开了它，模型识别错的金额会静默进账本。
    bool scan_auto_apply{false};
    /// 入账成功后把原图移到**回收站**。默认关闭。
    /// 只动「已入账」的图（识别失败的原样留着），且走回收站 —— 还能还原。
    bool scan_auto_clean{false};
    /// 上次扫描新增了几张（界面上给一句反馈）
    int  scan_new_last{0};

    /// 当前选中那张的草稿。没选中时返回一个被丢弃的空槽 ——
    /// 页面代码就不用到处判空了（写进去的值不会被任何人看到）。
    ScanDraft& cur_draft();
    ScanItem*  cur_item();

    // 设置页
    std::wstring set_base_url;
    std::wstring set_model;
    std::wstring set_api_key;
    int          set_channel{0};       // 0 自动 / 1 只用云端 / 2 只用本地
    bool         api_key_present{false};
    std::string  channel_notes;        // 通道解析说明（拼好给界面显示）

    // -------------------------------------------------------------------------
    //  忙碌 / 轻提示
    // -------------------------------------------------------------------------
    bool         busy{false};
    std::wstring busy_text;
    std::wstring toast;
    bool         toast_error{false};
    uint64_t     toast_deadline{0};

    // -------------------------------------------------------------------------
    //  操作
    // -------------------------------------------------------------------------
    /// 重新取全部数据（分类 / 记录 / 统计）。本地 SQLite，快。
    void refresh();
    void go(Screen s);
    void show_toast(const std::wstring& text, bool is_error);
    bool toast_alive(uint64_t now_ms) const { return now_ms < toast_deadline; }

    void login_async(std::string user, std::string pass, bool is_register);
    void logout();
    void save_entry();
    void delete_record(const std::string& id);
    /// 把某笔记录回填到记账表单（切进编辑态）
    void begin_edit(const std::string& record_id);
    void cancel_edit();

    /// 启用 / 停用分类。预置分类不允许删除（历史记录还引用着），只能停用 ——
    /// 这是分类模板从 15 条扩到 52 条之后必须具备的能力：
    /// 一个只吃外卖不做饭的人不需要「买菜」，但也不该因此看到 40 个格子。
    void toggle_category(const std::string& category_id);

    // 分类查询小工具。页面渲染时要按 id 反查名字，放在 public 是因为
    // 这些查询本身没有副作用，也不暴露存储细节。
    std::string category_name_of(const std::string& id);
    std::string category_id_of_name(const std::string& name);
    std::vector<std::string> expense_category_names();
    void generate_report_async(bool force);
    void pick_screenshot();      // 弹原生文件对话框（支持多选）
    /// 选一个目录作为「截图保存位置」
    void pick_scan_dir();
    void scan_async();           // 逐张识别所有待识别的图
    void confirm_scan();         // 把当前选中那张写进账本
    /// 从列表里移除第 index 张（用户主动丢弃）
    void drop_scan(int index);
    /// 清空整批
    void clear_scans();
    /// 诊断专用：模拟「工作线程完成了一张识别」，往结果槽塞一条结果。
    ///
    /// 为什么需要这个入口：pump() 里把结果并进列表的那段代码曾经嵌套锁住
    /// 同一个 std::mutex，异常无人接管 → std::terminate → abort()，
    /// 表现是「一点识别，程序整个消失」。真实路径要配好模型并联网，自动跑不了，
    /// 所以这里直接塞一条 —— 覆盖的仍然是那段出过事的合并代码。
    void inject_scan_result_for_diag(int index, ScanDraft draft);
    /// 扫描截图目录，把「没处理过」的图加进列表。返回新增几张。
    /// 判重靠指纹（路径 + 大小 + 修改时间）：光看文件名不够 ——
    /// 同一张图重新截一次会覆盖，光看路径也不够 —— 文件换了内容要重新识别。
    int  scan_directory();
    /// 重新读 / 保存截图相关设置
    void load_scan_settings();
    void save_scan_settings();
    /// 界面偏好（导航折叠状态等）
    void load_ui_settings();
    void save_ui_settings();
    void save_llm_settings();    // 设置页的「保存」
    void load_llm_settings();    // 打开设置页时把已存的值读进界面

    /// UI 线程每帧调用：取走工作线程写好的结果
    void pump();

private:
    App() = default;

    struct LoginSlot {
        bool            done{false};
        bool            ok{false};
        std::string     error;
        app::AuthResult auth;
    };
    struct ReportSlot {
        bool                     done{false};
        bool                     ok{false};
        std::string              error;
        report::ReportResult     result;
    };
    struct ScanResult {
        int       index{0};
        ScanDraft draft;
    };
    /// 识别结果槽。工作线程每完成一张就 push 一条并唤醒 UI 线程 ——
    /// 攒到最后一起给的话，用户在整个识别过程里只看到一句「识别中」，
    /// 五张图要等一分钟才知道进度、也不知道哪张好了。
    struct ScanSlot {
        bool                     done{false};
        int                      total{0};
        int                      current{0};   // 正在处理第几张（0 基）
        std::vector<ScanResult>  results;      // 已完成的结果，UI 线程取走后清空
    };

    std::mutex mu_;
    LoginSlot  login_slot_;
    ReportSlot report_slot_;
    ScanSlot   scan_slot_;

    // ---- 截图导入的「已处理」记录 ----
    /// 一个文件的指纹。用它回答「这张图是不是已经识别/记账过了」。
    struct SeenEntry {
        std::string path;        // UTF-8 绝对路径
        long long   size{0};     // 字节
        long long   mtime{0};    // Unix 秒
        bool        applied{false};   // 是否已写进账本
    };
    std::vector<SeenEntry> scan_seen_;
    bool is_seen(const std::string& path, long long size, long long mtime) const;
    void remember_seen(const std::string& path, long long size, long long mtime,
                       bool applied);
    /// 把某个已入库的文件在记录里标成 applied（避免下次又被自动入账）
    void mark_seen_applied(const std::wstring& path);
    void load_seen();
    void save_seen();
    /// 入账成功后按设置把原图移进回收站。**只对已入账的图生效** ——
    /// 识别失败的原图必须留着，否则用户连重试和手动记账的依据都没了。
    void maybe_clean_source(int index);
    /// 自动入账一条识别结果。返回 true 表示真的写进去了。
    /// 里面有安全阀：金额必须为正、不超过上限、日期可解析 —— 全自动时
    /// 这几条是唯一能拦住「模型识错一个数就静默进账」的东西。
    bool auto_apply(int index);
    /// 扫描时用的：这张图现在能否被识别（在列表中且不是最新状态）
    bool already_listed(const std::string& utf8_path) const;

    std::unique_ptr<app::LedgerService>    ledger_;
    std::unique_ptr<report::ReportService> reports_;
    std::function<void()>                  repaint_;

    void apply_channels();
};

// -----------------------------------------------------------------------------
//  页面构造。每个函数往 host 里填自己的组件树。
//  分开放是因为它们各自只依赖 App 的公有状态，互不干扰，也方便单独调排版。
// -----------------------------------------------------------------------------
void build_login_page(ui::VBox& host, App& app);
void build_entry_page(ui::VBox& host, App& app);
void build_stats_page(ui::VBox& host, App& app);
void build_report_page(ui::VBox& host, App& app);
void build_scan_page(ui::VBox& host, App& app);
void build_settings_page(ui::VBox& host, App& app);
void build_records_page(ui::VBox& host, App& app);

}  // namespace penhu::native
