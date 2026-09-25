// 实现单任务后台识别、取消发布屏障和非阻塞关闭查询，不持有窗口或借用图像。
#include "ocr_engine.h"
#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <ocr_client.h>
#include <optional>
#include <thread>
#include <utility>

namespace open_st
{
// 列出模型档位，作为设置候选与输入验证的唯一领域定义。
// 入参：无。
// 返回：静态只读列表。
std::span<const std::string_view> OcrModelChoices() noexcept
{
    static constexpr std::array<std::string_view, 2> CHOICES{"fast", "best"};
    return CHOICES;
}
// 列出固定语言组合，混合作为首项默认值。
// 入参：无。
// 返回：静态只读列表。
std::span<const std::string_view> OcrLanguageChoices() noexcept
{
    static constexpr std::array<std::string_view, 4> CHOICES{"chi_sim+eng+jpn", "chi_sim", "eng", "jpn"};
    return CHOICES;
}
// 将设置字符串限制在固定模型与语言集合，不向引擎透传任意参数。
// 入参：options 为提交选择。
// 返回：合法为 true。
bool AreOcrOptionsValid(const OcrOptions& options) noexcept
{
    const std::span<const std::string_view> models = OcrModelChoices();
    const std::span<const std::string_view> languages = OcrLanguageChoices();
    return std::find(models.begin(), models.end(), options.model) != models.end() &&
           std::find(languages.begin(), languages.end(), options.language) != languages.end();
}
namespace ocr_detail
{
// 先验证预算和乘法，再确认最后一行可访问，不要求最后一行拥有尾部填充。
// 入参：image 为只读物理像素视图。
// 返回：尺寸、跨度和缓冲区完整时 true。
bool ValidImage(OcrImageView image) noexcept
{
    if (image.width == 0 || image.height == 0 || image.width > MAX_DIMENSION || image.height > MAX_DIMENSION ||
        static_cast<std::uint64_t>(image.width) * image.height > MAX_PIXELS)
        return false;
    const std::size_t rowBytes = static_cast<std::size_t>(image.width) * 4;
    if (image.stride < rowBytes || image.pixels.data() == nullptr || image.pixels.size() < rowBytes)
        return false;
    return image.height == 1 ||
           image.stride <= (image.pixels.size() - rowBytes) / (static_cast<std::size_t>(image.height) - 1);
}
} // namespace ocr_detail

struct OcrClient::Impl
{
    struct Work
    {
        std::uint64_t id{};
        ocr_detail::Image image;
        OcrOptions options;
    };
    std::filesystem::path root;
    std::function<void(std::uint64_t)> notify;
    std::unique_ptr<ocr_detail::Engine> engine;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::thread worker;
    std::optional<Work> pending;
    OcrSnapshot snapshot;
    std::atomic_bool cancel{false};
    std::atomic_bool stopped{true};
    bool shutdown{};

    // 发出可丢失的唤醒通知，异常不逃逸线程且快照仍可轮询补收。
    // 入参：id 为当前单调任务编号。
    // 返回：无。
    void Notify(std::uint64_t id) noexcept
    {
        try
        {
            if (this->notify)
                this->notify(id);
        }
        catch (...)
        {
            // 通知失败不改变可查询的实际完成状态，不重试或调用 UI。
        }
    }
    // 发布阶段，取消在同一把状态锁下撤销资格。
    // 入参：id 为任务编号；phase 为实际阶段。
    // 返回：无。
    void Phase(std::uint64_t id, OcrPhase phase)
    {
        {
            const std::scoped_lock lock(this->mutex);
            if (this->cancel.load() || this->shutdown || id != this->snapshot.requestId)
                return;
            this->snapshot.phase = phase;
            ++this->snapshot.revision;
        }
        this->Notify(id);
    }
    // 执行唯一工作循环，释放每次图像与页面，停止时在后台释放模型。
    // 入参：无。
    // 返回：无；所有识别异常转换为分类失败，不产生脱离宿主的线程。
    void Run() noexcept
    {
        for (;;)
        {
            Work work;
            {
                std::unique_lock lock(this->mutex);
                // 等待唯一待执行任务或关闭；谓词不读取任何窗口状态。
                // 入参：无。
                // 返回：应唤醒工作线程时 true。
                this->wake.wait(lock, [this] { return this->shutdown || this->pending.has_value(); });
                if (this->shutdown && !this->pending)
                    break;
                work = std::move(*this->pending);
                this->pending.reset();
            }
            ocr_detail::Recognition result;
            std::shared_ptr<const std::wstring> text;
            try
            {
                if (!this->cancel.load())
                {
                    // 引擎只发布阶段，所有状态与取消资格仍归客户端。
                    // 入参：phase 为引擎实际进入的阶段。
                    // 返回：无。
                    result = this->engine->Recognize(this->root, work.image, work.options, this->cancel,
                                                     [this, id = work.id](OcrPhase phase) { this->Phase(id, phase); });
                    if (result.error == OcrError::None && !result.text.empty())
                        text = std::make_shared<const std::wstring>(std::move(result.text));
                }
            }
            catch (...)
            {
                result.error = OcrError::Unavailable;
            }
            // 在公开空闲前释放输入，避免下一次提交与上一份大图叠加。
            std::vector<std::byte>().swap(work.image.pixels);
            {
                const std::scoped_lock lock(this->mutex);
                this->snapshot.busy = false;
                this->snapshot.loadMilliseconds = result.loadMilliseconds;
                this->snapshot.preprocessMilliseconds = result.preprocessMilliseconds;
                this->snapshot.recognizeMilliseconds = result.recognizeMilliseconds;
                if (this->cancel.load() || this->shutdown)
                {
                    this->snapshot.phase = OcrPhase::Cancelled;
                    this->snapshot.error = OcrError::Cancelled;
                    this->snapshot.text.reset();
                }
                else
                {
                    this->snapshot.error = result.error;
                    this->snapshot.phase = result.error != OcrError::None
                                               ? OcrPhase::Failed
                                               : (text ? OcrPhase::Succeeded : OcrPhase::Empty);
                    this->snapshot.text = std::move(text);
                }
                ++this->snapshot.revision;
            }
            this->Notify(work.id);
        }
        this->engine.reset();
        this->stopped.store(true);
    }
};

// 构造本地引擎外壳，延迟至第一次提交才创建工作线程。
// 入参：root 为程序根；notify 为工作线程通知。
// 返回：未开始客户端。
OcrClient::OcrClient(std::filesystem::path root, std::function<void(std::uint64_t)> notify)
    : OcrClient(std::move(root), std::move(notify), ocr_detail::CreateEngine())
{
}
// 注入私有引擎，生产与测试共用完整线程状态机。
// 入参：root 为程序根；notify 为通知；engine 为独占引擎。
// 返回：未开始客户端。
OcrClient::OcrClient(std::filesystem::path root, std::function<void(std::uint64_t)> notify,
                     std::unique_ptr<ocr_detail::Engine> engine)
    : impl_(std::make_unique<Impl>())
{
    this->impl_->root = std::move(root);
    this->impl_->notify = std::move(notify);
    this->impl_->engine = std::move(engine);
}
// 宿主已异步收尾时 join 只回收句柄；异常提前析构仍保证无后台悬空访问。
// 入参：无。
// 返回：无。
OcrClient::~OcrClient()
{
    this->RequestShutdown();
    if (this->impl_->worker.joinable())
        this->impl_->worker.join();
}
// 验证输入并紧凑复制行数据，单调编号只有在实际接受时推进。
// 入参：image 为借用图；options 为选择；requestId 输出编号；error 输出错误。
// 返回：提交成功为 true。
bool OcrClient::Submit(OcrImageView image, const OcrOptions& options, std::uint64_t& requestId,
                       OcrError& error) noexcept
{
    requestId = 0;
    error = OcrError::None;
    try
    {
        const std::scoped_lock lock(this->impl_->mutex);
        if (this->impl_->shutdown || !this->impl_->engine ||
            this->impl_->snapshot.requestId == std::numeric_limits<std::uint64_t>::max())
            error = OcrError::Unavailable;
        else if (this->impl_->snapshot.busy)
            error = OcrError::Busy;
        else if (!AreOcrOptionsValid(options))
            error = OcrError::InvalidOptions;
        else if (!ocr_detail::ValidImage(image))
            error = OcrError::InvalidInput;
        if (error != OcrError::None)
            return false;
        Impl::Work work;
        work.options = options;
        work.id = this->impl_->snapshot.requestId + 1;
        work.image.width = image.width;
        work.image.height = image.height;
        const std::size_t rowBytes = static_cast<std::size_t>(image.width) * 4;
        work.image.pixels.resize(rowBytes * image.height);
        for (std::size_t row = 0; row < image.height; ++row)
            std::memcpy(work.image.pixels.data() + row * rowBytes, image.pixels.data() + row * image.stride, rowBytes);
        if (!this->impl_->worker.joinable())
        {
            this->impl_->stopped.store(false);
            try
            {
                // 工作线程只捕获由客户端唯一持有的稳定实现地址。
                // 入参：无。
                // 返回：无，退出标记由 Run 最后发布。
                this->impl_->worker = std::thread([state = this->impl_.get()] { state->Run(); });
            }
            catch (...)
            {
                this->impl_->stopped.store(true);
                throw;
            }
        }
        this->impl_->cancel.store(false);
        this->impl_->snapshot.requestId = work.id;
        ++this->impl_->snapshot.revision;
        this->impl_->snapshot.phase = OcrPhase::Preparing;
        this->impl_->snapshot.error = OcrError::None;
        this->impl_->snapshot.busy = true;
        this->impl_->snapshot.text.reset();
        this->impl_->snapshot.loadMilliseconds = 0;
        this->impl_->snapshot.preprocessMilliseconds = 0;
        this->impl_->snapshot.recognizeMilliseconds = 0;
        requestId = work.id;
        this->impl_->pending = std::move(work);
        this->impl_->wake.notify_one();
        return true;
    }
    catch (...)
    {
        error = OcrError::Unavailable;
        return false;
    }
}
// 取消操作与发布共享锁，保证返回后晚结果不能恢复正文。
// 入参：无。
// 返回：无。
void OcrClient::Cancel() noexcept
{
    const std::scoped_lock lock(this->impl_->mutex);
    this->impl_->cancel.store(true);
    this->impl_->snapshot.phase = OcrPhase::Cancelled;
    this->impl_->snapshot.error = OcrError::Cancelled;
    this->impl_->snapshot.text.reset();
    ++this->impl_->snapshot.revision;
}
// 异步禁止新任务，取消当前发布并唤醒空闲工作线程。
// 入参：无。
// 返回：无。
void OcrClient::RequestShutdown() noexcept
{
    const std::scoped_lock lock(this->impl_->mutex);
    this->impl_->shutdown = true;
    this->impl_->cancel.store(true);
    this->impl_->snapshot.phase = OcrPhase::Cancelled;
    this->impl_->snapshot.error = OcrError::Cancelled;
    this->impl_->snapshot.text.reset();
    ++this->impl_->snapshot.revision;
    this->impl_->wake.notify_one();
}
// 工作线程最后一步才发布 stopped，不依靠窗口存在性推测。
// 入参：无。
// 返回：后台已结束为 true。
bool OcrClient::ShutdownComplete() const noexcept
{
    return this->impl_->stopped.load();
}
// 复制状态并保留不可变正文的共享所有权。
// 入参：无。
// 返回：独立快照。
OcrSnapshot OcrClient::Snapshot() const
{
    const std::scoped_lock lock(this->impl_->mutex);
    return this->impl_->snapshot;
}
} // namespace open_st
