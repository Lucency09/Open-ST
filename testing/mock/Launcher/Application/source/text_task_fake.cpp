// 可控后台任务替身，不启动线程、网络或模型。
#include <algorithm>
#include <array>
#include <text_task_fake.h>

namespace open_st::test_text
{
TextTaskState* state{};
} // namespace open_st::test_text

namespace open_st
{
#ifdef OPEN_ST_HAS_OCR
// 设置回调与客户端实现位于同一生产对象；替身提供固定能力，避免拉入真实工作线程对象。
// 入参：无。返回：与正式边界一致的两个安全测试档位。
std::span<const std::string_view> OcrModelChoices() noexcept
{
    static constexpr std::array<std::string_view, 2> CHOICES{"fast", "best"};
    return CHOICES;
}
// 提供设置回调所需的固定语言目录，不查询模型或系统资源。
// 入参：无。返回：测试认可的固定语言组合。
std::span<const std::string_view> OcrLanguageChoices() noexcept
{
    static constexpr std::array<std::string_view, 4> CHOICES{"chi_sim+eng+jpn", "chi_sim", "eng", "jpn"};
    return CHOICES;
}
// 保留设置表单的合法值校验，不把任意输入放行。
// 入参：options 为候选配置。返回：属于测试固定目录时为 true。
bool AreOcrOptionsValid(const OcrOptions& options) noexcept
{
    const std::span<const std::string_view> models = OcrModelChoices();
    const std::span<const std::string_view> languages = OcrLanguageChoices();
    return std::find(models.begin(), models.end(), options.model) != models.end() &&
           std::find(languages.begin(), languages.end(), options.language) != languages.end();
}
// 独立链接替身只拥有空实现，真实后台线程由 OCR 模块专项覆盖。
struct OcrClient::Impl
{
};
// 建立可控任务边界，不加载模型或启动后台线程。
// 入参：root 和 notify 为真实协调器传入的资源路径及通知。
// 返回：未提交状态。
OcrClient::OcrClient(std::filesystem::path root, std::function<void(std::uint64_t)> notify)
    : impl_(std::make_unique<Impl>())
{
    (void)root;
    (void)notify;
    ++test_text::state->creates;
}
// 记录客户端释放，检验关闭窗口不会提前析构工作所有者。
// 入参：无。
// 返回：无。
OcrClient::~OcrClient()
{
    ++test_text::state->destroys;
}
// 模拟单任务准入，故意保持任务运行至测试明确发布结果。
// 入参：image/options 为真实会话参数；requestId/error 为输出。
// 返回：忙或停止时拒绝，其他情况接受。
bool OcrClient::Submit(OcrImageView image, const OcrOptions& options, std::uint64_t& requestId,
                       OcrError& error) noexcept
{
    requestId = 0;
    error = OcrError::None;
    ++test_text::state->submissions;
    if (test_text::state->snapshot.busy || test_text::state->stopping)
    {
        error = test_text::state->stopping ? OcrError::Unavailable : OcrError::Busy;
        return false;
    }
    if (image.width != 2 || image.height != 2 || image.stride != 8 || image.pixels.size() != 16)
    {
        error = OcrError::InvalidInput;
        return false;
    }
    test_text::state->options = options;
    ++test_text::state->snapshot.requestId;
    ++test_text::state->snapshot.revision;
    test_text::state->snapshot.phase = OcrPhase::Preparing;
    test_text::state->snapshot.busy = true;
    test_text::state->snapshot.text.reset();
    requestId = test_text::state->snapshot.requestId;
    return true;
}
// 保持 busy，模拟取消不能立即中断的模型初始化。
// 入参：无。
// 返回：无，只撤销可发布正文。
void OcrClient::Cancel() noexcept
{
    ++test_text::state->cancellations;
    test_text::state->snapshot.phase = OcrPhase::Cancelled;
    test_text::state->snapshot.text.reset();
    ++test_text::state->snapshot.revision;
}
// 请求退出但不虚构后台已完成。
// 入参：无。
// 返回：无。
void OcrClient::RequestShutdown() noexcept
{
    test_text::state->stopping = true;
    this->Cancel();
}
// 返回测试明确控制的释放状态。
// 入参：无。
// 返回：后台释放完成时为 true。
bool OcrClient::ShutdownComplete() const noexcept
{
    return test_text::state->stopped;
}
// 提供不可变正文及阶段快照，供真实会话执行代次检查。
// 入参：无。
// 返回：当前测试快照。
OcrSnapshot OcrClient::Snapshot() const
{
    return test_text::state->snapshot;
}
#endif
struct TranslationClient::Impl
{
};
// 不创建线程或网络，只观测真实窗口提交。
// 入参：程序目录和通知。返回：可控替身。
TranslationClient::TranslationClient(std::filesystem::path, std::function<void(std::uint64_t)>)
    : impl_(std::make_unique<Impl>())
{
    ++test_text::state->translationCreates;
}
// 释放空替身。入参：无。返回：无。
TranslationClient::~TranslationClient()
{
    ++test_text::state->translationDestroys;
}
// 真实唯一任务准入的替身，保存独立请求快照。
// 入参：request 为原文和资格戳，id/error 为输出。返回：空闲时接受。
bool TranslationClient::Submit(const TranslationRequest& request, std::uint64_t& id, TranslationError& error) noexcept
{
    ++test_text::state->translationSubmissions;
    id = 0;
    error = TranslationError::None;
    if (test_text::state->translation.busy)
    {
        error = TranslationError::Busy;
        return false;
    }
    try
    {
        test_text::state->request = request;
        ++test_text::state->translation.requestId;
        ++test_text::state->translation.revision;
        test_text::state->translation.phase = TranslationPhase::Running;
        test_text::state->translation.busy = true;
        test_text::state->translation.result.reset();
        id = test_text::state->translation.requestId;
        return true;
    }
    catch (...)
    {
        error = TranslationError::OutOfMemory;
        return false;
    }
}
// 保持忙槽位，取消不代表底层已经收尾。
// 入参：无。返回：无。
void TranslationClient::Cancel() noexcept
{
    ++test_text::state->translationCancellations;
    test_text::state->translation.result.reset();
    test_text::state->translation.phase = TranslationPhase::Cancelled;
    ++test_text::state->translation.revision;
}
// 提供权威任务快照。入参：无。返回：快照副本。
TranslationSnapshot TranslationClient::Snapshot() const
{
    return test_text::state->translation;
}
// 退出只撤销资格。入参：无。返回：无。
void TranslationClient::RequestShutdown() noexcept
{
    ++test_text::state->translationShutdownRequests;
    this->Cancel();
}
// 等测试显式通知真实收尾。入参：无。返回：释放状态。
bool TranslationClient::ShutdownComplete() const noexcept
{
    return test_text::state->translationStopped;
}
} // namespace open_st
