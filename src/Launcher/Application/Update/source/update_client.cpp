// 编排更新查询和用户确认后的下载；请求、结果与受保护文件只由本模块拥有。
#include "protected_download.h"
#include "update_model.h"
#include "update_transport.h"
#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>
#include <utility>

namespace open_st
{
using namespace update_detail;
namespace
{
// 将网络边界错误转换为业务结果，取消不伪装成联网失败。
// 入参：result 为单跳响应。返回：统一错误码。
UpdateError HttpFailure(const UpdateHttpResult& result) noexcept
{
    if (result.error == UpdateHttpError::Cancelled)
        return UpdateError::Cancelled;
    if (result.error == UpdateHttpError::Timeout)
        return UpdateError::Timeout;
    if (result.error != UpdateHttpError::None)
        return UpdateError::Network;
    if (result.status == 404)
        return UpdateError::NotFound;
    if (result.status == 403 || result.status == 429)
        return UpdateError::RateLimited;
    return result.status == 200 ? UpdateError::None : UpdateError::Network;
}
// 把磁盘和完整性错误分开，让界面不会建议重试损坏包的执行。
// 入参：error 为下载文件结果。返回：更新错误类别。
UpdateError FileFailure(UpdateDownloadError error) noexcept
{
    if (error == UpdateDownloadError::HashMismatch || error == UpdateDownloadError::SizeMismatch)
        return UpdateError::Integrity;
    if (error == UpdateDownloadError::Busy)
        return UpdateError::Busy;
    return UpdateError::Storage;
}
// 每一跳都重新验证域名，保持一次请求的共同截止时间。
// 入参：transport 为边界；request 为首跳；asset 指定资产域；stop 为取消；sink 为响应消费者。
// 返回：最后响应或明确错误，最多跟随五次受信跳转。
UpdateHttpResult Fetch(UpdateTransport& transport, UpdateHttpRequest request, bool asset, std::stop_token stop,
                       const UpdateHttpSink& sink)
{
    for (unsigned hop = 0; hop <= 5; ++hop)
    {
        if (stop.stop_requested())
            return {UpdateHttpError::Cancelled, 0, 0, {}, {}};
        if (!TrustedUrl(request.url, asset))
            return {UpdateHttpError::InvalidRequest, 0, 0, {}, {}};
        if (std::chrono::steady_clock::now() >= request.deadline)
            return {UpdateHttpError::Timeout, 0, 0, {}, {}};
        UpdateHttpResult result = transport.Get(request, stop, sink);
        if (result.error != UpdateHttpError::None)
            return result;
        if (result.status == 301 || result.status == 302 || result.status == 303 || result.status == 307 ||
            result.status == 308)
        {
            if (hop == 5 || result.redirectLocation.empty())
                return {UpdateHttpError::InvalidRequest, 0, 0, {}, {}};
            request.url = std::move(result.redirectLocation);
            continue;
        }
        return result;
    }
    return {UpdateHttpError::InvalidRequest, 0, 0, {}, {}};
}
// 资产地址从固定仓库和数字ID构造，不执行远端字符串。
// 入参：asset 为经过校验的资产。返回：GitHub API 二进制入口。
std::wstring AssetUrl(const UpdateAsset& asset)
{
    return L"https://api.github.com/repos/Lucency09/Open-ST/releases/assets/" + std::to_wstring(asset.id);
}
} // namespace
struct UpdateClient::Impl
{
    const std::thread::id owner = std::this_thread::get_id();
    std::filesystem::path cacheRoot;
    std::function<void()> notify;
    std::unique_ptr<UpdateTransport> transport;
    mutable std::mutex mutex;
    UpdateSnapshot snapshot;
    UpdateDistribution distribution{UpdateDistribution::Portable};
    UpdateRelease release;
    std::shared_ptr<ProtectedDownload> artifact;
    std::jthread worker;

    // 线程创建失败时撤销活动阶段，不能永久停在正在检查或下载。
    // 入参：expected 为刚发布的起始阶段。返回：无，仅修改匹配的阶段。
    void StartFailed(UpdatePhase expected) noexcept
    {
        {
            const std::scoped_lock lock(this->mutex);
            if (this->snapshot.phase != expected)
                return;
            this->snapshot.phase = UpdatePhase::Failed;
            this->snapshot.error = UpdateError::Unavailable;
            ++this->snapshot.revision;
        }
        this->Notify();
    }

    // 通知只发送唤醒，异常不能终止下载工作线程。
    // 入参：无。返回：无。
    void Notify() noexcept
    {
        try
        {
            if (this->notify)
                this->notify();
        }
        catch (...)
        {
        }
    }
    // 原子发布阶段，取消之后旧工作线程不能覆盖用户意图。
    // 入参：phase 为阶段；error 为错误；stop 为本次任务令牌。返回：无。
    void Publish(UpdatePhase phase, UpdateError error, std::stop_token stop)
    {
        {
            const std::scoped_lock lock(this->mutex);
            if (stop.stop_requested() || this->snapshot.phase == UpdatePhase::Cancelled)
                return;
            this->snapshot.phase = phase;
            this->snapshot.error = error;
            ++this->snapshot.revision;
        }
        this->Notify();
    }
    // 请求并解析正式发行，绝不在查询阶段读取资产内容。
    // 入参：current 为编译版本；stop 为取消令牌。返回：无，结果经快照发布。
    void Query(std::string current, std::stop_token stop) noexcept
    try
    {
        if (this->distribution == UpdateDistribution::Installed)
            ProtectedDownload::CleanupOwned(this->cacheRoot);
        const auto version = ParseVersion(current);
        if (!version)
        {
            this->Publish(UpdatePhase::Failed, UpdateError::Configuration, stop);
            return;
        }
        std::string body;
        const UpdateHttpRequest request{
            L"https://api.github.com/repos/Lucency09/Open-ST/releases/latest", L"application/vnd.github+json",
            std::chrono::steady_clock::now() + std::chrono::seconds(10), std::chrono::seconds(10)};
        const UpdateHttpResult response =
            Fetch(*this->transport, request, false, stop,
                  // 限制响应，不让远端对象无限扩张内存。
                  // 入参：bytes 为当前响应块。返回：已接受为 true。
                  [&body](std::span<const std::byte> bytes)
                  {
                      if (bytes.size() > MAX_METADATA_BYTES - body.size())
                          return false;
                      body.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                      return true;
                  });
        UpdateError error = HttpFailure(response);
        if (error != UpdateError::None)
        {
            this->Publish(UpdatePhase::Failed, error, stop);
            return;
        }
        UpdateRelease next;
        if ((response.declaredLength && *response.declaredLength != body.size()) || !ParseRelease(body, next))
        {
            this->Publish(UpdatePhase::Failed, UpdateError::InvalidRelease, stop);
            return;
        }
        UpdatePhase phase = UpdatePhase::Available;
        if (*version == next.version)
            phase = UpdatePhase::Current;
        else if (*version > next.version)
            phase = UpdatePhase::LocalAhead;
        else if ((this->distribution == UpdateDistribution::Installed &&
                  (!next.installer || (next.installer->sha256.empty() && !next.checksums))) ||
                 (this->distribution == UpdateDistribution::Portable && !next.portable))
        {
            phase = UpdatePhase::Failed;
            error = UpdateError::MissingPackage;
        }
        {
            const std::scoped_lock lock(this->mutex);
            if (stop.stop_requested() || this->snapshot.phase == UpdatePhase::Cancelled)
                return;
            this->release = std::move(next);
            this->snapshot.latestVersion = this->release.versionText;
            this->snapshot.releasePage = this->release.page;
        }
        this->Publish(phase, error, stop);
    }
    catch (...)
    {
        this->Publish(UpdatePhase::Failed, UpdateError::InvalidRelease, stop);
    }

    // 在用户确认的固定发行范围内核对清单并下载完整安装器。
    // 入参：selected 为确认时的发行副本；stop 为取消。返回：无，只有完整包可发布 Ready。
    void Download(UpdateRelease selected, std::stop_token stop) noexcept
    try
    {
        const UpdateAsset& installer = *selected.installer;
        std::string hash = installer.sha256;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(10);
        if (selected.checksums)
        {
            if (selected.checksums->size > MAX_METADATA_BYTES)
            {
                this->Publish(UpdatePhase::Failed, UpdateError::Integrity, stop);
                return;
            }
            std::string sums;
            const UpdateHttpRequest request{
                AssetUrl(*selected.checksums), L"application/octet-stream",
                std::min(deadline, std::chrono::steady_clock::now() + std::chrono::seconds(10)),
                std::chrono::seconds(10)};
            const UpdateHttpResult response =
                Fetch(*this->transport, request, true, stop,
                      // 校验清单也有独立上限，不接受截断内容。
                      // 入参：bytes 为数据块。返回：在预算内为 true。
                      [&sums](std::span<const std::byte> bytes)
                      {
                          if (bytes.size() > MAX_METADATA_BYTES - sums.size())
                              return false;
                          sums.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                          return true;
                      });
            const UpdateError error = HttpFailure(response);
            if (error != UpdateError::None)
            {
                this->Publish(UpdatePhase::Failed, error, stop);
                return;
            }
            const auto fromList = ChecksumFor(sums, installer.name);
            if (sums.size() != selected.checksums->size ||
                (response.declaredLength && *response.declaredLength != sums.size()) || !fromList ||
                (!hash.empty() && hash != *fromList))
            {
                this->Publish(UpdatePhase::Failed, UpdateError::Integrity, stop);
                return;
            }
            hash = *fromList;
        }
        if (!NormalizeHash(hash))
        {
            this->Publish(UpdatePhase::Failed, UpdateError::Integrity, stop);
            return;
        }
        if (stop.stop_requested())
            return;
        UpdateDownloadError diskError{};
        std::shared_ptr<ProtectedDownload> file =
            ProtectedDownload::Create(this->cacheRoot, installer.size, hash, diskError);
        if (!file)
        {
            this->Publish(UpdatePhase::Failed, FileFailure(diskError), stop);
            return;
        }
        std::uint64_t received{};
        auto lastNotify = std::chrono::steady_clock::now();
        const UpdateHttpRequest request{AssetUrl(installer), L"application/octet-stream", deadline,
                                        std::chrono::seconds(30)};
        const UpdateHttpResult response =
            Fetch(*this->transport, request, true, stop,
                  // 写入和散列同一块字节，UI 只读取进度副本。
                  // 入参：bytes 为本次响应片段。返回：完整接收为 true。
                  [this, &file, &diskError, &received, &lastNotify, stop](std::span<const std::byte> bytes)
                  {
                      if (stop.stop_requested() || !file->Append(bytes, diskError))
                          return false;
                      received += bytes.size();
                      const auto now = std::chrono::steady_clock::now();
                      if (now - lastNotify >= std::chrono::milliseconds(100))
                      {
                          {
                              const std::scoped_lock lock(this->mutex);
                              if (stop.stop_requested())
                                  return false;
                              this->snapshot.received = received;
                              ++this->snapshot.revision;
                          }
                          lastNotify = now;
                          this->Notify();
                      }
                      return true;
                  });
        if (stop.stop_requested())
            return;
        if (diskError != UpdateDownloadError::None)
        {
            this->Publish(UpdatePhase::Failed, FileFailure(diskError), stop);
            return;
        }
        const UpdateError error = HttpFailure(response);
        if (error != UpdateError::None)
        {
            this->Publish(UpdatePhase::Failed, error, stop);
            return;
        }
        if ((response.declaredLength && *response.declaredLength != installer.size) || received != installer.size)
        {
            this->Publish(UpdatePhase::Failed, UpdateError::Integrity, stop);
            return;
        }
        if (!file->Complete(diskError))
        {
            this->Publish(UpdatePhase::Failed, FileFailure(diskError), stop);
            return;
        }
        {
            const std::scoped_lock lock(this->mutex);
            if (stop.stop_requested() || this->snapshot.phase == UpdatePhase::Cancelled)
                return;
            this->artifact = std::move(file);
            this->snapshot.received = received;
        }
        this->Publish(UpdatePhase::Ready, UpdateError::None, stop);
    }
    catch (...)
    {
        this->Publish(UpdatePhase::Failed, UpdateError::Storage, stop);
    }
};
// 生产实例使用 WinHTTP 边界，不在构造时访问网络。
// 入参：cacheRoot 为缓存根，notify 为通知。返回：任务实例。
UpdateClient::UpdateClient(std::filesystem::path cacheRoot, std::function<void()> notify)
    : UpdateClient(std::move(cacheRoot), std::move(notify), MakeWinHttpUpdateTransport())
{
}
// 注入边界供真实任务状态机测试。
// 入参：cacheRoot、notify、transport 为任务资源。返回：未开始实例。
UpdateClient::UpdateClient(std::filesystem::path cacheRoot, std::function<void()> notify,
                           std::unique_ptr<UpdateTransport> transport)
    : impl_(std::make_unique<Impl>())
{
    this->impl_->cacheRoot = std::move(cacheRoot);
    this->impl_->notify = std::move(notify);
    this->impl_->transport = std::move(transport);
}
// 先取消再等待可取消 HTTP 边界退出，成员按寿命顺序回收。
// 入参：无。返回：无。
UpdateClient::~UpdateClient()
{
    this->Cancel();
    if (this->impl_->worker.joinable())
        this->impl_->worker.join();
}
// 开始一项元数据请求，不允许覆盖活动下载。
// 入参：currentVersion、distribution 为本次固定输入。返回：已启动为 true。
bool UpdateClient::Check(std::string currentVersion, UpdateDistribution distribution) noexcept
try
{
    if (std::this_thread::get_id() != this->impl_->owner)
        return false;
    {
        const std::scoped_lock lock(this->impl_->mutex);
        if (this->impl_->snapshot.phase == UpdatePhase::Checking ||
            this->impl_->snapshot.phase == UpdatePhase::Downloading)
            return false;
    }
    if (this->impl_->worker.joinable())
        this->impl_->worker.join();
    {
        const std::scoped_lock lock(this->impl_->mutex);
        this->impl_->artifact.reset();
        this->impl_->release = {};
        this->impl_->snapshot = {
            this->impl_->snapshot.revision + 1, UpdatePhase::Checking, UpdateError::None, currentVersion, {}, {}, 0, 0};
        this->impl_->distribution = distribution;
    }
    this->impl_->worker = std::jthread([impl = this->impl_.get(), current = std::move(currentVersion)](
                                           std::stop_token stop) { impl->Query(current, stop); });
    return true;
}
catch (...)
{
    this->impl_->StartFailed(UpdatePhase::Checking);
    return false;
}
// 只允许安装版的 Available 状态进入下载，同版或取消不产生网络资产请求。
// 入参：无。返回：开始下载时 true。
bool UpdateClient::DownloadConfirmed() noexcept
try
{
    if (std::this_thread::get_id() != this->impl_->owner)
        return false;
    UpdateRelease release;
    {
        const std::scoped_lock lock(this->impl_->mutex);
        if (this->impl_->snapshot.phase != UpdatePhase::Available ||
            this->impl_->distribution != UpdateDistribution::Installed)
            return false;
        release = this->impl_->release;
    }
    if (this->impl_->worker.joinable())
        this->impl_->worker.join();
    {
        const std::scoped_lock lock(this->impl_->mutex);
        this->impl_->snapshot.phase = UpdatePhase::Downloading;
        this->impl_->snapshot.total = release.installer->size;
        ++this->impl_->snapshot.revision;
    }
    this->impl_->worker = std::jthread([impl = this->impl_.get(), release = std::move(release)](std::stop_token stop)
                                       { impl->Download(release, stop); });
    return true;
}
catch (...)
{
    this->impl_->StartFailed(UpdatePhase::Downloading);
    return false;
}
// 取消令牌唤醒 HTTP 等待，发布取消阶段阻止晚到成功结果。
// 入参：无。返回：无。
void UpdateClient::Cancel() noexcept
{
    if (std::this_thread::get_id() != this->impl_->owner)
        return;
    this->impl_->worker.request_stop();
    const std::scoped_lock lock(this->impl_->mutex);
    this->impl_->snapshot.phase = UpdatePhase::Cancelled;
    this->impl_->snapshot.error = UpdateError::Cancelled;
    ++this->impl_->snapshot.revision;
    this->impl_->artifact.reset();
}
// 借助进程内短锁复制状态，不对 UI 暴露下载文件写入权。
// 入参：无。返回：独立快照。
UpdateSnapshot UpdateClient::Snapshot() const
{
    const std::scoped_lock lock(this->impl_->mutex);
    return this->impl_->snapshot;
}
// 文件保护跨越整个系统启动回调，防止取消或文件替换竞争。
// 入参：launch 为宿主同步回调。返回：已交接成功时 true。
bool UpdateClient::LaunchReady(const std::function<bool(const std::filesystem::path&)>& launch) noexcept
try
{
    if (std::this_thread::get_id() != this->impl_->owner)
        return false;
    std::shared_ptr<ProtectedDownload> file;
    {
        const std::scoped_lock lock(this->impl_->mutex);
        if (this->impl_->snapshot.phase != UpdatePhase::Ready || !this->impl_->artifact || !launch)
            return false;
        file = this->impl_->artifact;
        this->impl_->snapshot.phase = UpdatePhase::Idle;
        ++this->impl_->snapshot.revision;
    }
    const bool started = launch(file->Path());
    if (started)
        file->Preserve();
    {
        const std::scoped_lock lock(this->impl_->mutex);
        this->impl_->artifact.reset();
    }
    return started;
}
catch (...)
{
    return false;
}
} // namespace open_st
