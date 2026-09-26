#pragma once
// =============================================================================
//  penhu/deploy/local_model.hpp
//  本地模型部署框架。
//
//  职责边界（刻意的，因为这块最容易做过头）：
//
//    框架负责：                 框架不负责：
//      · 获取 llama.cpp 运行时     · 替你决定用哪个模型
//      · 起 / 停 / 探测服务         · 替你下载特定权重
//      · 把模型跑起来并给出 URL     · 保证模型效果好不好
//      · 记住你登记过哪些模型       · 帮你选量化等级
//
//  也就是说：模型由**用户提供**（本地 GGUF 文件路径），框架只负责
//  「把引擎装好、按你给的权重跑起来、跑起来之后自动接上」。
//  这么做的好处是不必维护一份随时会过期、随时可能 404 的模型清单，
//  也不会替用户做他并不想被做的决定。
//  任何 GGUF 都能用，包括自己在别处训练/转换出来的权重。
//
//  运行时获取为什么会自动：llama-server 是通用引擎（和模型无关），
//  每次让用户手写下载解压太麻烦。所以框架从 llama.cpp 官方 release 拿
//  Windows 预编译包自己解压。但注意：llama.cpp 现在是 nightly 发版，
//  不是每个 release 都带二进制，所以必须遍历 release 列表找第一个
//  真正带该资产的版本 —— 不能硬编码版本号。
//
//  数据落地位置（机器级，不属于任何账号，所以不进加密库）：
//    <data_dir>/runtime/llama/llama-server.exe   运行时
//    <data_dir>/runtime/downloads/               下载缓存
//    <data_dir>/runtime/llama-server.log         服务日志
//    <data_dir>/local_models.json                模型登记表（只有路径，无秘密）
// =============================================================================

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "penhu/app/app_config.hpp"
#include "penhu/llm/provider.hpp"
#include "penhu/util/process.hpp"
#include "penhu/util/result.hpp"

namespace penhu::deploy {

/// 进度回调：stage 是给人看的阶段描述，total <= 0 表示总大小未知
using ProgressFn = std::function<void(const std::string& stage,
                                     int64_t done_bytes,
                                     int64_t total_bytes)>;

/// llama.cpp 预编译包的种类。CPU 版兼容性最好；Vulkan 版能用上显卡
/// 且不需要装 CUDA SDK，是个不错的折中。
enum class RuntimeFlavor {
    Cpu     = 0,
    Vulkan  = 1,
    Cuda    = 2
};

const char* to_string(RuntimeFlavor flavor) noexcept;
/// 对应 release 资产名里的关键片段，用于在 assets 里匹配
const char* asset_marker(RuntimeFlavor flavor) noexcept;

struct RuntimeStatus {
    bool        installed{false};
    std::string server_exe;       // llama-server.exe 绝对路径
    std::string directory;
    std::string release_tag;      // 记下是从哪个 release 装的
    RuntimeFlavor flavor{RuntimeFlavor::Cpu};
    int64_t     size_bytes{0};
    std::string error;            // 不满足可用条件时的原因
};

/// 一个用户登记的本地模型。**模型文件由用户提供，框架不代下载。**
struct LocalModelEntry {
    std::string id;
    std::string display_name;
    std::string gguf_path;        // 用户给的本地路径（绝对路径）
    std::string note;             // 用户自己的备注（量化等级、来源之类）
    int64_t     size_bytes{0};
    int64_t     added_at{0};

    // 启动参数，用户可调
    int context_size{4096};
    int gpu_layers{0};            // 0 = 纯 CPU
    int threads{0};               // 0 = 让 llama.cpp 自己决定
};

/// 部署与运行的当前状态，界面直接渲染这个
struct DeploymentStatus {
    RuntimeStatus runtime;

    bool        server_running{false};
    int         server_port{0};
    int64_t     server_pid{0};
    std::string active_model_id;
    std::string active_model_name;
    std::string base_url;         // "http://127.0.0.1:<port>/v1"，空表示没跑
    std::string model_field;      // 供 OpenAI 兼容接口用的 model 名

    std::string log_path;
    int64_t     started_at{0};
    std::string last_error;
};

/// 模型文件合法性检查结果
struct GgufInfo {
    bool        valid{false};
    int64_t     size_bytes{0};
    uint32_t    gguf_version{0};
    std::string problem;          // 不合法时的原因
};

class LocalModelManager {
public:
    explicit LocalModelManager(app::AppConfig config);
    ~LocalModelManager();

    LocalModelManager(const LocalModelManager&) = delete;
    LocalModelManager& operator=(const LocalModelManager&) = delete;

    // ---- 运行时（引擎）----

    RuntimeStatus runtime_status() const;

    /// 从 llama.cpp 官方 release 下载并解压运行时。
    /// 会遍历最近的 release 找到第一个真正带目标资产的版本。
    Result<RuntimeStatus> install_runtime(RuntimeFlavor flavor = RuntimeFlavor::Cpu,
                                         ProgressFn on_progress = {});

    /// 直接指定一个已有的 llama-server.exe（自带二进制，跳过自动安装）
    Status set_runtime_path(const std::string& server_exe);

    // ---- 模型登记（模型由用户提供）----

    /// 校验一个 GGUF 文件：存在、是普通文件、有 GGUF 魔数、大小合理
    static GgufInfo inspect_gguf(const std::string& path);

    Result<LocalModelEntry> add_model(const std::string& display_name,
                                     const std::string& gguf_path,
                                     const std::string& note = {});

    Result<std::vector<LocalModelEntry>> list_models() const;
    Status remove_model(const std::string& id);
    Status update_model_params(const std::string& id, int context_size, int gpu_layers,
                               int threads);

    // ---- 服务生命周期 ----

    /// 启动指定模型。内部会挑一个空闲端口、拼参数、起进程、轮询健康检查。
    Result<DeploymentStatus> start(const std::string& model_id,
                                  ProgressFn on_progress = {});

    Status stop();
    DeploymentStatus status();

    /// 一步到位：确保运行时可用 → 确保模型可用 → 起服务 → 返回状态
    Result<DeploymentStatus> ensure_ready(const std::string& model_id,
                                         ProgressFn on_progress = {});

    /// 生成可直接给 llm::LlmConfig 的本地配置。
    /// 服务没在跑时返回错误（而不是给一个必然连不上的配置）。
    Result<llm::LlmConfig> build_local_config() const;

    /// 挑一个空闲端口（通过对目标端口发起连接来判断是否被占用）
    static int find_free_port(int start_from = 18080);

    /// 界面用：把最近的日志尾部读出来
    Result<std::string> tail_log_lines(int max_lines = 40) const;

private:
    struct Registry;   // 登记表（JSON 持久化），定义在 .cpp

    /// 不加锁的内部版本。mutex_ 是**非递归**的，所以凡是「已持锁的公开方法
    /// 内部还要再取一次运行状态」的地方，必须走这个函数，否则会死锁。
    RuntimeStatus runtime_status_unlocked() const;

    /// 读写登记表。这两个函数自身**不加锁**——调用方负责持锁，
    /// 因为一次逻辑修改（例如「改参数并落盘」）必须以一个临界区完成，
    /// 在里面各自加锁反而会把原子性拆开。
    Status load_registry();
    Status save_registry() const;

    std::string runtime_dir() const;
    std::string downloads_dir() const;
    std::string server_log_path() const;
    std::string registry_path() const;

    /// 轮询 /health 直到就绪或超时
    Result<bool> wait_for_health(int port, int timeout_ms, std::string* last_error) const;

    app::AppConfig                    config_;
    std::unique_ptr<Registry>         registry_;

    // mutex_ 与 running_ 都是 mutable：status() / build_local_config() / tail_log_lines()
    // 在语义上都是「读」，但需要加锁与回收已退出进程的状态，所以不能用 const 成员。
    mutable std::mutex                mutex_;
    std::unique_ptr<process::ChildProcess> server_process_;
    mutable DeploymentStatus          running_;
};

}  // namespace penhu::deploy
