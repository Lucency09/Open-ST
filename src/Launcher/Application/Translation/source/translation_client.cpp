// 实现统一本地/在线单任务顺序调度、取消发布屏障和工作线程空闲模型维护。
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <text_segmentation.h>
#include <thread>
#include <translation_protocol.h>
#include <translation_provider.h>
#include <vector>

namespace open_st
{
using translation_detail::Clock;
namespace
{
constexpr std::size_t NORMAL_INPUT_CODEPOINTS = 100000;
constexpr std::size_t DIAGNOSTIC_INPUT_CODEPOINTS = 10000;
constexpr std::size_t MAX_ACCUMULATED_TEXT_BYTES = 1024 * 1024;

// 将纯文本子模块错误适配到已有领域分类，不扩展界面错误协议。
// 入参：error 为子模块错误。返回：对应领域错误，不包含原文。
TranslationError SegmentationError(translation_segmentation::Error error) noexcept
{
    switch (error)
    {
    case translation_segmentation::Error::None:
        return TranslationError::None;
    case translation_segmentation::Error::InvalidInput:
        return TranslationError::InvalidInput;
    case translation_segmentation::Error::NoBoundary:
        return TranslationError::InputTooLarge;
    case translation_segmentation::Error::OutputTooLarge:
        return TranslationError::Truncated;
    case translation_segmentation::Error::Cancelled:
        return TranslationError::Cancelled;
    case translation_segmentation::Error::InvalidPlan:
        return TranslationError::Service;
    }
    return TranslationError::Service;
}
// 只有用户取消、全局错误、明确输入错误和资源失败能提前结束整轮。
// 入参：error 为已分类单项错误。
// 返回：必须停止时 true。
bool StopsChain(TranslationError error) noexcept
{
    return error == TranslationError::Cancelled || error == TranslationError::InvalidInput ||
           error == TranslationError::InvalidConfiguration || error == TranslationError::OutOfMemory ||
           error == TranslationError::BudgetExhausted || error == TranslationError::SameLanguage;
}
// 创建生产依赖，不提前加载模型或建立连接。
// 入参：root 为资源目录。
// 返回：独立提供方工厂与固定预算。
std::unique_ptr<translation_detail::ClientDependencies> Dependencies(std::filesystem::path root)
{
    auto dependencies = std::make_unique<translation_detail::ClientDependencies>();
    // 工厂保存资源路径值，不捕获 App 或窗口，模型实例只由工作线程创建。
    // 入参：kind 为提供方类型。
    // 返回：统一提供方。
    dependencies->factory = [root = std::move(root)](std::string_view kind)
    { return translation_detail::CreateProvider(kind, root); };
    return dependencies;
}
} // namespace
struct TranslationClient::Impl
{
    struct Work
    {
        std::uint64_t id{};
        TranslationRequest request;
    };
    std::unique_ptr<translation_detail::ClientDependencies> dependencies;
    std::function<void(std::uint64_t)> notify;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::thread worker;
    std::optional<Work> pending;
    TranslationSnapshot snapshot;
    std::stop_source cancellation;
    std::atomic_bool stopped{true};
    bool shutdown{};
    // 每种提供方只保留一个实例，本地复制条目也共用当前方向模型缓存。
    std::map<std::string, std::unique_ptr<translation_detail::ITranslationProvider>> providers;

    // 通知只携带稳定编号，异常不丢失可补收快照。
    // 入参：id 为整轮编号。
    // 返回：无。
    void Notify(std::uint64_t id) noexcept
    {
        try
        {
            if (this->notify)
                this->notify(id);
        }
        catch (...)
        { /* 通知是提示信号，宿主仍可从 Snapshot 补收。 */
        }
    }
    // 发布当前尝试或失败摘要，取消后不再更新有效结果。
    // 入参：work 为整轮，profile 为当前项，index 为启用项序号，attempt 为可选完成摘要。
    // 返回：无。
    void Progress(const Work& work, const nlohmann::json& profile, std::size_t index,
                  const TranslationAttempt* attempt = nullptr)
    {
        {
            const std::scoped_lock lock(this->mutex);
            if (this->cancellation.stop_requested() || this->shutdown)
                return;
            this->snapshot.currentProfileId = profile.at("id").get<std::string>();
            this->snapshot.currentProfileName = profile.at("name").get<std::string>();
            this->snapshot.currentIndex = index;
            this->snapshot.insecureHttp = TranslationProfileUsesInsecureHttp(profile);
            if (attempt)
            {
                this->snapshot.attempts.push_back(*attempt);
                this->snapshot.unattempted = this->snapshot.totalInterfaces - index;
            }
            ++this->snapshot.revision;
        }
        this->Notify(work.id);
    }
    // 同一段的校验和执行共用截止时间，取消或校验超时后不调用提供方。
    // 入参：input/profile为固定片段与配置；provider为借用实现；roundDeadline为整轮截止；stop为取消；
    // executed输出是否真正调用Execute；performed接收本次返回。返回：已有领域错误，致命错误不被超时覆盖。
    TranslationError ExecutePart(const TranslationRequest& input, const nlohmann::json& profile,
                                 translation_detail::ITranslationProvider& provider, Clock::time_point roundDeadline,
                                 std::stop_token stop, bool& executed, translation_detail::ProviderResult& performed)
    {
        executed = false;
        performed = {};
        if (stop.stop_requested())
            return TranslationError::Cancelled;
        if (Clock::now() >= roundDeadline)
            return TranslationError::BudgetExhausted;
        const Clock::time_point deadline = std::min(roundDeadline, Clock::now() + this->dependencies->itemBudget);
        TranslationError error = provider.Validate(input, profile);
        if (stop.stop_requested())
            return TranslationError::Cancelled;
        if (error == TranslationError::None && Clock::now() >= deadline)
            error = TranslationError::Timeout;
        if (error == TranslationError::None)
        {
            executed = true;
            performed = provider.Execute(input, profile, deadline, stop);
            error = performed.error;
            if (error == TranslationError::None && performed.status != translation_detail::ProviderStatus::Success)
                error = TranslationError::Service;
            if (Clock::now() >= deadline && !StopsChain(error))
                error = TranslationError::Timeout;
        }
        if (stop.stop_requested())
            return TranslationError::Cancelled;
        if (Clock::now() >= roundDeadline && !StopsChain(error))
            error = TranslationError::BudgetExhausted;
        if (error == TranslationError::None)
            error = translation_detail::ValidateResultText(performed.text);
        if (stop.stop_requested())
            return TranslationError::Cancelled;
        if (Clock::now() >= roundDeadline && !StopsChain(error))
            error = TranslationError::BudgetExhausted;
        return error;
    }
    // 普通任务只规划一次；逐家按同一计划串行执行，整家成功后才发布完整带戳结果。
    // 入参：work为整篇固定快照；stop为取消；result接收完整译文。返回：整轮最终分类，诊断保持单次原文请求。
    TranslationError Execute(const Work& work, std::stop_token stop, std::shared_ptr<const TranslationResult>& result)
    {
        const Clock::time_point roundDeadline = Clock::now() + this->dependencies->totalBudget;
        if (stop.stop_requested())
            return TranslationError::Cancelled;
        if (Clock::now() >= roundDeadline)
            return TranslationError::BudgetExhausted;
        translation_segmentation::Plan plan;
        if (!work.request.diagnostic)
        {
            const TranslationError planned =
                SegmentationError(translation_segmentation::Split(work.request.text, plan, stop));
            if (planned != TranslationError::None)
                return planned;
            if (plan.segmentCount == 0)
                return TranslationError::InvalidInput;
        }
        TranslationRequest part = work.request;
        if (!work.request.diagnostic)
            part.text.clear();
        TranslationError last = TranslationError::NoInterfaces;
        std::set<std::string> used;
        std::size_t index{};
        for (const nlohmann::json& profile : work.request.configuration.interfaces)
        {
            if (!profile.at("enabled").get<bool>())
                continue;
            if (stop.stop_requested())
                return TranslationError::Cancelled;
            if (Clock::now() >= roundDeadline)
                return TranslationError::BudgetExhausted;
            const std::string id = profile.at("id").get<std::string>();
            if (!used.insert(id).second)
                return TranslationError::InvalidConfiguration;
            ++index;
            this->Progress(work, profile, index);
            const Clock::time_point started = Clock::now();
            const std::string kind = profile.at("kind").get<std::string>();
            std::unique_ptr<translation_detail::ITranslationProvider>& provider = this->providers[kind];
            if (!provider)
                provider = this->dependencies->factory(kind);
            if (!provider)
                return TranslationError::InvalidConfiguration;
            const translation_detail::ProviderCapabilities capabilities = provider->Capabilities(profile);
            translation_detail::ProviderResult performed;
            bool anyExecuted{};
            if (work.request.diagnostic)
                last = this->ExecutePart(work.request, profile, *provider, roundDeadline, stop, anyExecuted, performed);
            else
            {
                std::vector<std::string> translations;
                translations.reserve(plan.segmentCount);
                std::size_t accumulatedBytes{};
                bool allLanguagesKnown = true;
                std::string detectedLanguage;
                last = TranslationError::None;
                for (const translation_segmentation::Piece& piece : plan.pieces)
                {
                    if (piece.literal)
                        continue;
                    part.text.assign(work.request.text, piece.offset, piece.length);
                    bool executed{};
                    last = this->ExecutePart(part, profile, *provider, roundDeadline, stop, executed, performed);
                    anyExecuted = anyExecuted || executed;
                    if (last != TranslationError::None)
                        break;
                    if (performed.text.size() > MAX_ACCUMULATED_TEXT_BYTES - accumulatedBytes)
                    {
                        last = TranslationError::Truncated;
                        break;
                    }
                    accumulatedBytes += performed.text.size();
                    if (performed.detectedLanguage.empty())
                        allLanguagesKnown = false;
                    if (translations.empty())
                        detectedLanguage = performed.detectedLanguage;
                    else if (detectedLanguage != performed.detectedLanguage)
                        allLanguagesKnown = false;
                    translations.push_back(std::move(performed.text));
                }
                if (last == TranslationError::None)
                {
                    const translation_segmentation::JoinStyle style =
                        work.request.options.targetLanguage == "en" ? translation_segmentation::JoinStyle::Spaced
                                                                    : translation_segmentation::JoinStyle::Compact;
                    const translation_segmentation::Error assembled = translation_segmentation::Assemble(
                        work.request.text, plan, translations, style, MAX_ACCUMULATED_TEXT_BYTES, performed.text, stop);
                    // 原文和计划已通过Split校验，此处空白译文失效不能误判为用户输入错误而阻断后备。
                    last = assembled == translation_segmentation::Error::InvalidInput ? TranslationError::EmptyResult
                                                                                      : SegmentationError(assembled);
                    performed.detectedLanguage = allLanguagesKnown ? std::move(detectedLanguage) : std::string{};
                }
            }
            if (stop.stop_requested())
                return TranslationError::Cancelled;
            if (Clock::now() >= roundDeadline && !StopsChain(last))
                last = TranslationError::BudgetExhausted;
            if (last == TranslationError::None)
                last = translation_detail::ValidateResultText(performed.text);
            if (stop.stop_requested())
                return TranslationError::Cancelled;
            if (Clock::now() >= roundDeadline && !StopsChain(last))
                last = TranslationError::BudgetExhausted;
            TranslationAttempt attempt{
                id, profile.at("name").get<std::string>(), last, !anyExecuted, capabilities.network, {}};
            if (work.request.diagnostic)
            {
                attempt.diagnostic = performed.diagnostic.value_or(TranslationDiagnostic{});
                attempt.diagnostic->elapsedMilliseconds =
                    std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
            }
            this->Progress(work, profile, index, &attempt);
            if (last == TranslationError::None)
            {
                auto translated = std::make_shared<TranslationResult>();
                translated->text = std::move(performed.text);
                translated->detectedLanguage = std::move(performed.detectedLanguage);
                translated->profileId = id;
                translated->profileName = attempt.profileName;
                translated->network = capabilities.network;
                translated->insecureHttp = TranslationProfileUsesInsecureHttp(profile);
                translated->options = work.request.options;
                translated->requestId = work.id;
                translated->sessionId = work.request.sessionId;
                translated->sourceRevision = work.request.sourceRevision;
                translated->configurationRevision = work.request.configurationRevision;
                result = std::move(translated);
                return TranslationError::None;
            }
            if (StopsChain(last))
                return last;
        }
        return last;
    }
    // 工作循环共用一个线程执行任务与空闲维护；停止时先释放全部提供方。
    // 入参：无。
    // 返回：无，所有任务异常转为领域错误。
    void Run() noexcept
    {
        for (;;)
        {
            Work work;
            std::stop_token stop;
            {
                std::unique_lock lock(this->mutex);
                while (!this->shutdown && !this->pending)
                {
                    lock.unlock();
                    Clock::time_point maintenance = Clock::time_point::max();
                    for (auto& [kind, provider] : this->providers)
                    {
                        (void)kind;
                        if (provider)
                        {
                            provider->Maintain(Clock::now());
                            maintenance = std::min(maintenance, provider->NextMaintenance());
                        }
                    }
                    lock.lock();
                    if (this->shutdown || this->pending)
                        break;
                    // 空闲等待只被新任务、停止或提供方维护期限唤醒。
                    // 入参：无。
                    // 返回：有工作或关停时 true。
                    this->wake.wait_until(lock, maintenance,
                                          [this] { return this->shutdown || this->pending.has_value(); });
                }
                if (this->shutdown && !this->pending)
                    break;
                work = std::move(*this->pending);
                this->pending.reset();
                stop = this->cancellation.get_token();
            }
            std::shared_ptr<const TranslationResult> result;
            TranslationError error = TranslationError::Unavailable;
            try
            {
                error = this->Execute(work, stop, result);
            }
            catch (const std::bad_alloc&)
            {
                error = TranslationError::OutOfMemory;
            }
            catch (...)
            {
                error = TranslationError::Service;
            }
            // 释放原文和凭据后才开放下一次提交；结果只保留必要文字与身份戳。
            const std::uint64_t id = work.id;
            work = {};
            {
                const std::scoped_lock lock(this->mutex);
                this->snapshot.busy = false;
                if (this->shutdown || this->cancellation.stop_requested())
                {
                    this->snapshot.phase = TranslationPhase::Cancelled;
                    this->snapshot.error = TranslationError::Cancelled;
                    this->snapshot.result.reset();
                }
                else
                {
                    this->snapshot.error = error;
                    this->snapshot.phase =
                        error == TranslationError::None ? TranslationPhase::Succeeded : TranslationPhase::Failed;
                    this->snapshot.result = error == TranslationError::None ? std::move(result) : nullptr;
                }
                ++this->snapshot.revision;
            }
            this->Notify(id);
        }
        this->providers.clear();
        this->stopped.store(true);
    }
};
// 创建延迟实例，复用资源根供所有本地配置使用。
// 入参：root 为程序目录，notify 为后台稳定编号通知。
// 返回：未执行客户端。
TranslationClient::TranslationClient(std::filesystem::path root, std::function<void(std::uint64_t)> notify)
    : TranslationClient(std::move(notify), Dependencies(std::move(root)))
{
}
// 注入统一提供方工厂以测试真实调度和取消，不另造状态机。
// 入参：notify 为通知，dependencies 为提供方和预算。
// 返回：未执行实例。
TranslationClient::TranslationClient(std::function<void(std::uint64_t)> notify,
                                     std::unique_ptr<translation_detail::ClientDependencies> dependencies)
    : impl_(std::make_unique<Impl>())
{
    this->impl_->notify = std::move(notify);
    this->impl_->dependencies = std::move(dependencies);
}
// 正常宿主先异步停止，此处 join 兜底确保不留悬空线程。
// 入参：无。
// 返回：无。
TranslationClient::~TranslationClient()
{
    this->RequestShutdown();
    if (this->impl_->worker.joinable())
        this->impl_->worker.join();
}
// 全局输入和配置在任何网络前验证；只有实际收尾后才允许新请求。
// 入参：request 为快照；requestId 接收编号；error 接收失败原因。
// 返回：实际接收任务为 true。
bool TranslationClient::Submit(const TranslationRequest& request, std::uint64_t& requestId,
                               TranslationError& error) noexcept
{
    requestId = 0;
    error = TranslationError::None;
    try
    {
        const std::scoped_lock lock(this->impl_->mutex);
        if (this->impl_->shutdown || !this->impl_->dependencies || !this->impl_->dependencies->factory ||
            this->impl_->snapshot.requestId == std::numeric_limits<std::uint64_t>::max())
            error = TranslationError::Unavailable;
        else if (this->impl_->snapshot.busy)
            error = TranslationError::Busy;
        if (error != TranslationError::None)
            return false;
        std::size_t count{};
        if (!translation_detail::CountText(request.text, count) || count == 0 ||
            request.text.find_first_not_of(" \t\r\n") == std::string::npos)
            error = TranslationError::InvalidInput;
        else if (count > (request.diagnostic ? DIAGNOSTIC_INPUT_CODEPOINTS : NORMAL_INPUT_CODEPOINTS))
            error = TranslationError::InputTooLarge;
        else if (request.options.sourceLanguage == request.options.targetLanguage)
            error = TranslationError::SameLanguage;
        else
            error = ValidateTranslationConfiguration(request.configuration, request.options);
        if (error != TranslationError::None)
            return false;
        std::size_t enabled{};
        for (const nlohmann::json& profile : request.configuration.interfaces)
            if (profile.at("enabled").get<bool>())
                ++enabled;
        if (enabled == 0)
        {
            error = TranslationError::NoInterfaces;
            return false;
        }
        Impl::Work work{this->impl_->snapshot.requestId + 1, request};
        std::stop_source cancellation;
        TranslationSnapshot snapshot;
        snapshot.requestId = work.id;
        snapshot.revision = this->impl_->snapshot.revision + 1;
        snapshot.phase = TranslationPhase::Running;
        snapshot.busy = true;
        snapshot.totalInterfaces = enabled;
        snapshot.unattempted = enabled;
        snapshot.attempts.reserve(enabled);
        if (!this->impl_->worker.joinable())
        {
            this->impl_->stopped.store(false);
            try
            {
                // 捕获唯一稳定实现地址，释放由析构等待保证。
                // 入参：无。
                // 返回：无。
                this->impl_->worker = std::thread([state = this->impl_.get()] { state->Run(); });
            }
            catch (...)
            {
                this->impl_->stopped.store(true);
                throw;
            }
        }
        this->impl_->cancellation = std::move(cancellation);
        this->impl_->snapshot = std::move(snapshot);
        requestId = work.id;
        this->impl_->pending = std::move(work);
        this->impl_->wake.notify_one();
        return true;
    }
    catch (const std::bad_alloc&)
    {
        error = TranslationError::OutOfMemory;
    }
    catch (...)
    {
        error = TranslationError::Unavailable;
    }
    return false;
}
// 取消即时撤销发布资格，busy 直到底层 Execute 结束才清除。
// 入参：无。
// 返回：无。
void TranslationClient::Cancel() noexcept
{
    const std::scoped_lock lock(this->impl_->mutex);
    this->impl_->cancellation.request_stop();
    this->impl_->snapshot.result.reset();
    this->impl_->snapshot.phase =
        this->impl_->snapshot.busy ? TranslationPhase::Cancelling : TranslationPhase::Cancelled;
    this->impl_->snapshot.error = TranslationError::Cancelled;
    ++this->impl_->snapshot.revision;
}
// 提供独立摘要副本，便于漏掉窗口消息时补收。
// 入参：无。
// 返回：完整有界快照。
TranslationSnapshot TranslationClient::Snapshot() const
{
    const std::scoped_lock lock(this->impl_->mutex);
    return this->impl_->snapshot;
}
// 永久禁止新任务并非阻塞取消，空闲模型也在原工作线程释放。
// 入参：无。
// 返回：无。
void TranslationClient::RequestShutdown() noexcept
{
    const std::scoped_lock lock(this->impl_->mutex);
    this->impl_->shutdown = true;
    this->impl_->cancellation.request_stop();
    this->impl_->snapshot.result.reset();
    this->impl_->snapshot.error = TranslationError::Cancelled;
    this->impl_->snapshot.phase =
        this->impl_->snapshot.busy ? TranslationPhase::Cancelling : TranslationPhase::Cancelled;
    ++this->impl_->snapshot.revision;
    this->impl_->wake.notify_one();
}
// 后台最后释放提供方后才标记完成，不用窗口状态推断。
// 入参：无。
// 返回：线程资源收尾完成时 true。
bool TranslationClient::ShutdownComplete() const noexcept
{
    return this->impl_->stopped.load();
}
} // namespace open_st
