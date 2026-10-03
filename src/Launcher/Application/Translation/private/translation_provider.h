// 统一提供方和客户端测试边界；本地子模块不依赖此父层契约。
#pragma once
#include <http_transport.h>
#include <stop_token>
#include <translation_client.h>

namespace open_st::translation_detail
{
using Clock = std::chrono::steady_clock;
struct ProviderCapabilities
{
    bool network{}, supportsAuto{};
    std::size_t maxInputCodepoints{10000};
    std::vector<std::pair<std::string, std::string>> languagePairs;
    std::span<const TranslationProfileField> fields;
};
enum class ProviderStatus
{
    Success,
    NotApplicable,
    Unavailable,
    Failed,
    Cancelled,
    TimedOut
};
struct ProviderResult
{
    ProviderStatus status{ProviderStatus::Failed};
    TranslationError error{TranslationError::Unavailable};
    std::string text, detectedLanguage;
    std::optional<TranslationDiagnostic> diagnostic;
};
class ITranslationProvider
{
  public:
    // 释放提供方，其 Execute 须由宿主先同步结束。
    // 入参：无。
    // 返回：无。
    virtual ~ITranslationProvider() = default;
    // 查询配置能力，不联网、不加载模型。
    // 入参：profile 为已静态验证的单项。
    // 返回：真实语言能力与输入预算。
    virtual ProviderCapabilities Capabilities(const nlohmann::json& profile) const = 0;
    // 检查单项参数与输入，不联网、不加载模型。
    // 入参：input 为整轮快照；profile 为单项。
    // 返回：None 表示可执行，否则跳过或停止原因由统一调度处理。
    virtual TranslationError Validate(const TranslationRequest& input, const nlohmann::json& profile) const = 0;
    // 执行恰好一次，返回前停止访问本次输入，迟到后台计算不得跨下一项。
    // 入参：input 为有界正文/语言；profile 为单项；deadline 为本项截止；stop 为整轮取消。
    // 返回：完整结果或分类失败。
    virtual ProviderResult Execute(const TranslationRequest& input, const nlohmann::json& profile,
                                   Clock::time_point deadline, std::stop_token stop) = 0;
    // 查询空闲资源维护时刻，没有需求时返回 max。
    // 入参：无。
    // 返回：单调时刻。
    virtual Clock::time_point NextMaintenance() const noexcept = 0;
    // 空闲阶段执行释放等维护，和 Execute 在同一线程。
    // 入参：now 为当前时刻。
    // 返回：无。
    virtual void Maintain(Clock::time_point now) noexcept = 0;
};
struct ClientDependencies
{
    std::function<std::unique_ptr<ITranslationProvider>(std::string_view)> factory;
    std::chrono::milliseconds itemBudget{30000}, totalBudget{120000};
};
// 按 kind 建立统一适配器；工厂是唯一需要区分本地和在线的入口。
// 入参：kind 为类型；root 为资源根。
// 返回：提供方实例，未知类型返回空。
std::unique_ptr<ITranslationProvider> CreateProvider(std::string_view kind, const std::filesystem::path& root);
// 在真实在线适配层注入传输替身，用于测试零请求与协议错误映射。
// 入参：kind 为在线类型；transport 为独占单次传输。
// 返回：统一适配器。
std::unique_ptr<ITranslationProvider> CreateOnlineProvider(std::string_view kind,
                                                           std::unique_ptr<http::Transport> transport);
// 映射领域错误到统一结果类型。
// 入参：error 为领域分类。
// 返回：分类状态。
ProviderStatus StatusOf(TranslationError error) noexcept;
} // namespace open_st::translation_detail
