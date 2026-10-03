// 实现统一本地/在线单任务顺序调度、取消发布屏障和工作线程空闲模型维护。
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <translation_protocol.h>
#include <translation_provider.h>

namespace open_st
{
using translation_detail::Clock;
namespace
{
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
    // 执行完整有序链，提供方 Execute 真实返回后才能进入下一条。
    // 入参：work 为固定快照，stop 为取消；result 接收完整带戳译文。
    // 返回：整轮最终错误，成功为 None。
    TranslationError Execute(const Work& work, std::stop_token stop, std::shared_ptr<const TranslationResult>& result)
    {
        const Clock::time_point roundDeadline = Clock::now() + this->dependencies->totalBudget;
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
            const Clock::time_point deadline = std::min(roundDeadline, Clock::now() + this->dependencies->itemBudget);
            const std::string kind = profile.at("kind").get<std::string>();
            std::unique_ptr<translation_detail::ITranslationProvider>& provider = this->providers[kind];
            if (!provider)
                provider = this->dependencies->factory(kind);
            if (!provider)
                return TranslationError::InvalidConfiguration;
            const translation_detail::ProviderCapabilities capabilities = provider->Capabilities(profile);
            last = provider->Validate(work.request, profile);
            if (last == TranslationError::None && Clock::now() >= deadline)
                last = TranslationError::Timeout;
            const bool skipped = last != TranslationError::None;
            translation_detail::ProviderResult performed;
            if (!skipped && !stop.stop_requested())
            {
                performed = provider->Execute(work.request, profile, deadline, stop);
                last = performed.error;
                if (last == TranslationError::None && performed.status != translation_detail::ProviderStatus::Success)
                    last = TranslationError::Service;
                // 迟到结果失去发布资格，但明确输入/资源错误仍终止整轮，不能变成允许外发的超时。
                if (Clock::now() >= deadline && !StopsChain(last))
                    last = TranslationError::Timeout;
            }
            if (stop.stop_requested())
                return TranslationError::Cancelled;
            if (Clock::now() >= roundDeadline && !StopsChain(last))
                last = TranslationError::BudgetExhausted;
            if (last == TranslationError::None)
                last = translation_detail::ValidateResultText(performed.text);
            TranslationAttempt attempt{id, profile.at("name").get<std::string>(), last, skipped, capabilities.network,
                                       {}};
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
        else if (count > 10000)
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
