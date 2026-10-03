// 定义翻译领域配置、统一条目目录和唯一异步任务，不包含 UI 或设置文件操作。
#pragma once
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace open_st
{
enum class TranslationError
{
    None,
    Busy,
    InvalidConfiguration,
    MissingCredentials,
    InvalidInput,
    InputTooLarge,
    SameLanguage,
    Authentication,
    Permission,
    Quota,
    RateLimited,
    Proxy,
    Tls,
    Network,
    Timeout,
    Cancelled,
    Service,
    UnsupportedResponse,
    EmptyResult,
    Truncated,
    ContextLimit,
    Unavailable,
    Redirect,
    NotApplicable,
    OutOfMemory,
    NoInterfaces,
    BudgetExhausted,
    ModelMissing,
    ModelIntegrity
};
// 轻量查询内置本地模型包文件，不创建任务、不读取正文或执行完整性散列。
// 入参：root 为应用绝对资源根，由宿主提供。
// 返回：None 仅表示文件类型/尺寸匹配、尚未加载验证；其他为 Missing/Integrity/Unavailable。
[[nodiscard]] TranslationError QueryLocalTranslationModelFiles(const std::filesystem::path& root) noexcept;
enum class TranslationPhase
{
    Idle,
    Running,
    Cancelling,
    Succeeded,
    Failed,
    Cancelled
};
struct TranslationOptions
{
    std::string sourceLanguage{"auto"}, targetLanguage{"zh-CN"};
};
struct TranslationConfiguration
{
    nlohmann::json interfaces;
    std::string proxyMode{"system"}, proxyAddress;
};
struct TranslationSettings
{
    TranslationOptions options;
    TranslationConfiguration configuration;
};
struct TranslationProfileField
{
    std::string_view key;
    bool secret{}, multiline{};
};
// 创建具有新稳定身份的条目；初始停用，凭据为空。
// 入参：kind 为领域提供方类型。
// 返回：完整 JSON 条目；未知类型抛出参数异常。
nlohmann::json CreateTranslationProfile(std::string_view kind);
// 发布六项默认列表，仅百度默认启用；默认身份固定，复制应创建新身份。
// 入参：无。
// 返回：独立有序 JSON 数组。
nlohmann::json DefaultTranslationInterfaces();
// 发布条目表单字段，秘密值由 secrets 对象存储，其余来自 configuration。
// 入参：kind 为提供方类型。
// 返回：静态字段列表；自定义 HTTP 使用分组表单及 TranslationChoices 专用选项。
std::span<const TranslationProfileField> TranslationProfileFields(std::string_view kind) noexcept;
// 发布唯一顶层设置字段目录，旧单供应商字段不再读取。
// 入参：无。
// 返回：静态字段列表。
std::span<const std::string_view> TranslationSettingKeys() noexcept;
// 发布语言、类型、代理及协议字段选项。
// 入参：fieldKey 为领域字段，例如 translation.kind、deepl.service。
// 返回：静态选项，非枚举字段为空。
std::span<const std::string_view> TranslationChoices(std::string_view fieldKey) noexcept;
// 校验顶层字符串字段，不静默修正非法配置。
// 入参：fieldKey 为字段，value 为 UTF-8 值。
// 返回：合法为 None。
TranslationError ValidateTranslationField(std::string_view fieldKey, std::string_view value) noexcept;
// 校验条目结构、语言能力、模板和静态预算；允许凭据为空。
// 入参：profile 为完整条目。
// 返回：合法为 None，不返回包含凭据的详细文本。
TranslationError ValidateTranslationProfile(const nlohmann::json& profile) noexcept;
// 校验有序列表、唯一 ID 及总预算；空数组可以保存。
// 入参：interfaces 为 JSON 数组。
// 返回：合法为 None。
TranslationError ValidateTranslationInterfaces(const nlohmann::json& interfaces) noexcept;
// 捕获独立设置快照，缺字段采用领域默认，类型错误保留为无效配置。
// 入参：read 读取单个 JSON 设置字段；缺失返回空 optional。
// 返回：独立设置副本；读取和内存异常交给宿主。
TranslationSettings ReadTranslationSettings(const std::function<std::optional<nlohmann::json>(std::string_view)>& read);
// 校验整轮配置与源目标，不把单项缺失凭据判为全局错误。
// 入参：configuration 为列表与代理，options 为源目标。
// 返回：首个全局错误或 None。
TranslationError ValidateTranslationConfiguration(const TranslationConfiguration& configuration,
                                                  const TranslationOptions& options) noexcept;
// 返回固定分类文本键，不包含服务器响应、原文或密钥。
// 入参：error 为领域分类。
// 返回：静态资源键。
std::string_view TranslationErrorTextKey(TranslationError error) noexcept;
// 查询条目是否会发送未加密 HTTP，用于设置与结果窗的持续提示。
// 入参：profile 为待展示条目。
// 返回：明确 HTTP 端点时 true；非法条目返回 false。
bool TranslationProfileUsesInsecureHttp(const nlohmann::json& profile) noexcept;
struct TranslationRequest
{
    std::string text;
    TranslationOptions options;
    TranslationConfiguration configuration;
    std::uint64_t sessionId{}, sourceRevision{}, configurationRevision{};
    bool diagnostic{};
};
struct TranslationResult
{
    std::string text, detectedLanguage, profileId, profileName;
    bool network{}, insecureHttp{};
    TranslationOptions options;
    std::uint64_t requestId{}, sessionId{}, sourceRevision{}, configurationRevision{};
};
// 仅显式连接测试发布的有界诊断；正文仅含安全字段摘要，不含请求凭据。
struct TranslationDiagnostic
{
    unsigned httpStatus{};
    unsigned long systemError{};
    std::int64_t elapsedMilliseconds{};
    std::string providerCode, providerMessage, response;
};
struct TranslationAttempt
{
    std::string profileId, profileName;
    TranslationError error{TranslationError::None};
    bool skipped{}, network{};
    std::optional<TranslationDiagnostic> diagnostic;
};
struct TranslationSnapshot
{
    std::uint64_t requestId{}, revision{};
    TranslationPhase phase{TranslationPhase::Idle};
    TranslationError error{TranslationError::None};
    bool busy{}, insecureHttp{};
    std::string currentProfileId, currentProfileName;
    std::size_t currentIndex{}, totalInterfaces{}, unattempted{};
    std::vector<TranslationAttempt> attempts;
    std::shared_ptr<const TranslationResult> result;
};
namespace translation_detail
{
struct ClientDependencies;
}
struct TranslationClientTestAccess;
class TranslationClient final
{
  public:
    // 创建延迟工作线程客户端，由宿主跨窗口持有唯一实例。
    // 入参：root 为程序绝对目录；notify 为工作线程编号通知，不可重入命令。
    // 返回：尚未加载模型或访问网络的实例。
    TranslationClient(std::filesystem::path root, std::function<void(std::uint64_t)> notify);
    // 取消并回收线程；普通退出须先异步关闭并等到 ShutdownComplete。
    // 入参：无。
    // 返回：无。
    ~TranslationClient();
    // 禁止复制唯一任务与模型所有权。
    // 入参：源对象。
    // 返回：不可调用。
    TranslationClient(const TranslationClient&) = delete;
    // 禁止复制赋值。
    // 入参：源对象。
    // 返回：不可调用。
    TranslationClient& operator=(const TranslationClient&) = delete;
    // 捕获不可变整轮快照，旧任务实际收尾前拒绝新任务。
    // 入参：request 为有界原文配置；requestId 输出单调编号；error 输出拒绝原因。
    // 返回：成功接收为 true，失败不排队。
    bool Submit(const TranslationRequest& request, std::uint64_t& requestId, TranslationError& error) noexcept;
    // 立即撤销结果发布资格，不等待网络或模型加载返回。
    // 入参：无。
    // 返回：无。
    void Cancel() noexcept;
    // 取得可补收只读快照，结果不带凭据。
    // 入参：无。
    // 返回：独立快照。
    TranslationSnapshot Snapshot() const;
    // 永久关闭准入并异步取消，后台释放全部模型后发布完成。
    // 入参：无。
    // 返回：无。
    void RequestShutdown() noexcept;
    // 查询后台是否停止且释放资源。
    // 入参：无。
    // 返回：完成为 true。
    bool ShutdownComplete() const noexcept;

  private:
    friend struct TranslationClientTestAccess;
    // 测试注入提供方与时间预算，不另建测试专用调度器。
    // 入参：notify 为通知；dependencies 为独占提供方工厂及预算。
    // 返回：未开始实例。
    TranslationClient(std::function<void(std::uint64_t)> notify,
                      std::unique_ptr<translation_detail::ClientDependencies> dependencies);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace open_st
