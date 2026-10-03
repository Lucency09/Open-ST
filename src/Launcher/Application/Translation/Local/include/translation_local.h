// 声明独立离线翻译引擎；不依赖窗口、网络、设置或父级提供方协议。
#pragma once
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>

namespace open_st::translation_local
{
enum class Error
{
    None,
    NotApplicable,
    InvalidInput,
    InputTooLong,
    ModelMissing,
    ModelIntegrity,
    ModelLoad,
    Unavailable,
    TimedOut,
    Cancelled,
    Truncated,
    OutOfMemory,
    Inference
};
// 轻量检查固定模型包的文件存在、类型与尺寸，不读取正文、散列或加载模型。
// 入参：root 为应用绝对资源根，不查询或改变任何引擎实例。
// 返回：None 仅表示元数据匹配；缺失、类型/尺寸异常和查询失败分别返回对应分类。
[[nodiscard]] Error QueryModelFiles(const std::filesystem::path& root) noexcept;
// 由调用方配置传入的推理参数；本模块不定义档位名、默认档位或档位映射。
struct Options
{
    std::size_t beamSize;
};
struct Result
{
    Error error{Error::None};
    std::string text;
    double loadMilliseconds{}, inferenceMilliseconds{};
    std::string detectedLanguage;
};
// 所有操作由宿主同一工作线程串行调用；实例跨本地条目共享当前方向的缓存。
class Engine final
{
  public:
    // 创建不加载权重的引擎。
    // 入参：root 为程序绝对目录，模型固定在 resources/translation 下。
    // 返回：未加载实例；分配失败抛异常。
    explicit Engine(std::filesystem::path root);
    // 释放当前方向模型与分词器。
    // 入参：无。
    // 返回：无；不得与 Execute 并发。
    ~Engine();
    // 禁止复制模型所有权。
    // 入参：源实例。
    // 返回：不可调用。
    Engine(const Engine&) = delete;
    // 禁止复制赋值模型所有权。
    // 入参：源实例。
    // 返回：不可调用。
    Engine& operator=(const Engine&) = delete;
    // 同步执行有界中英翻译，合作取消后实际停止才返回。
    // 入参：text 为 UTF-8；source 可为 auto；target 为 zh-CN 或 en；options 为显式推理参数；deadline 含加载；stop
    // 为取消。 返回：保留原有硬换行/空白行/缩进的完整译文或分类错误；整文与每行仍受同一模型 token 上限。 auto
    // 先识别全文语言；不支持的语种不加载模型，任一行失败均不截断或发布部分结果。
    [[nodiscard]] Result Execute(std::string_view text, std::string_view source, std::string_view target,
                                 const Options& options, std::chrono::steady_clock::time_point deadline,
                                 std::stop_token stop);
    // 给宿主唯一任务线程提供下次卸载时刻。
    // 入参：无。
    // 返回：末次活动后五分钟，无缓存时为 time_point::max。
    [[nodiscard]] std::chrono::steady_clock::time_point NextMaintenance() const noexcept;
    // 在宿主空闲时释放过期缓存；不建立定时线程。
    // 入参：now 为当前单调时间。
    // 返回：无；不得与 Execute 并发。
    void Maintain(std::chrono::steady_clock::time_point now) noexcept;
    // 主动释放模型；后续仍可按需重新加载。
    // 入参：无。
    // 返回：无；不得与 Execute 并发。
    void Release() noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace open_st::translation_local
