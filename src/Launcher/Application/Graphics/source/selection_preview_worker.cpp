// 实现最新请求优先的截图底图转换；图形与COM对象全部在工作线程创建和释放。
#include <selection_preview_worker.h>
#include <selection_output_renderer.h>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>
#include <windows.h>
#include <objbase.h>

namespace open_st
{
struct SelectionPreviewWorker::Impl final
{
    struct Request final
    {
        std::shared_ptr<const FrozenDesktopFrame> desktop;
        RectI selection;
        unsigned int brightness{};
        HdrToneMappingOptions options{};
        std::uint64_t id{};
        std::uint64_t generation{};
    };
    std::function<void()> notify;
    Converter converter;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::optional<Request> pending;
    std::optional<SelectionPreviewResult> result;
    std::uint64_t generation{};
    bool running{};
    bool stopping{};
    std::thread worker;

    // 先完成共享状态初始化，再启动唯一的转换线程。
    // 入参：completion 为完成通知；conversion 为可空测试替身。返回：无。
    Impl(std::function<void()> completion, Converter conversion)
        : notify(std::move(completion)), converter(std::move(conversion)), worker([this]() { this->Run(); })
    {
    }
    // 停止线程并等待资源在所属线程释放。
    // 入参：无。返回：无。
    ~Impl()
    {
        {
            const std::lock_guard<std::mutex> lock(this->mutex);
            this->stopping = true;
            ++this->generation;
            this->pending.reset();
            this->result.reset();
        }
        this->wake.notify_one();
        if (this->worker.joinable())
            this->worker.join();
    }
    // 串行完成转换，发布前再次检查版本；通知回调不持锁。
    // 入参：无。返回：停止时退出，所有图形对象先于COM解绑释放。
    void Run() noexcept
    {
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        {
            std::unique_ptr<SelectionOutputRenderer> renderer;
            for (;;)
            {
                Request request;
                {
                    std::unique_lock<std::mutex> lock(this->mutex);
                    this->wake.wait(lock, [this]() { return this->stopping || this->pending.has_value(); });
                    if (this->stopping)
                        break;
                    request = std::move(*this->pending);
                    this->pending.reset();
                    this->running = true;
                }
                SelectionPreviewResult completed;
                completed.requestId = request.id;
                try
                {
                    if (FAILED(initialized))
                        completed.error = L"无法初始化选区预览工作线程。";
                    else if (this->converter)
                        completed.success = this->converter(*request.desktop, request.selection, request.brightness,
                                                            request.options, completed.frame, completed.error);
                    else
                    {
                        if (!renderer)
                            renderer = std::make_unique<SelectionOutputRenderer>();
                        completed.success =
                            renderer->SetBrightnessPercent(request.brightness) &&
                            renderer->SetToneMappingOptions(request.options) &&
                            renderer->Render(*request.desktop, request.selection, completed.frame, completed.error);
                    }
                    if (!completed.success)
                        completed.frame = {};
                }
                catch (...)
                {
                    completed.frame = {};
                    completed.success = false;
                    // 保持错误路径无第二次内存分配；宿主负责空诊断的本地化回退。
                }
                request.desktop.reset();
                bool announce{};
                {
                    const std::lock_guard<std::mutex> lock(this->mutex);
                    this->running = false;
                    if (!this->stopping && request.generation == this->generation)
                        this->result = std::move(completed);
                    announce = !this->stopping;
                }
                if (announce && this->notify)
                {
                    try
                    {
                        this->notify();
                    }
                    catch (...)
                    { /* 通知失败不终止线程，宿主仍可通过邮箱补收。 */
                    }
                }
            }
        }
        if (SUCCEEDED(initialized))
            CoUninitialize();
    }
};
// 建立生产转换器，实际设备延迟到线程首次HDR任务创建。
// 入参：notify 为宿主通知。返回：无。
SelectionPreviewWorker::SelectionPreviewWorker(std::function<void()> notify)
    : SelectionPreviewWorker(std::move(notify), {})
{
}
// 建立带测试转换替身的后台协议实例。
// 入参：notify/converter 为通知与测试替身。返回：无。
SelectionPreviewWorker::SelectionPreviewWorker(std::function<void()> notify, Converter converter)
    : impl_(std::make_unique<Impl>(std::move(notify), std::move(converter)))
{
}
// 由实现守卫停止线程并释放资源。
// 入参：无。返回：无。
SelectionPreviewWorker::~SelectionPreviewWorker() = default;
// 提交最新的合法裁切，旧邮箱随版本变化立即失效。
// 入参：desktop/selection 为源与裁切，brightness 为亮度，requestId 为宿主版本。返回：接收成功为true。
bool SelectionPreviewWorker::Submit(std::shared_ptr<const FrozenDesktopFrame> desktop, RectI selection,
                                    unsigned int brightness, std::uint64_t requestId, HdrToneMappingOptions options)
{
    if (!desktop || !desktop->IsValid() || selection.IsEmpty() || !IsHdrBrightnessPercent(brightness) ||
        !IsHdrHighlightCeilingNits(options.highlightCeilingNits))
        return false;
    const RectI bounds = desktop->Bounds();
    if (selection.left < bounds.left || selection.top < bounds.top || selection.right > bounds.right ||
        selection.bottom > bounds.bottom)
        return false;
    {
        const std::lock_guard<std::mutex> lock(this->impl_->mutex);
        if (this->impl_->stopping)
            return false;
        ++this->impl_->generation;
        this->impl_->result.reset();
        this->impl_->pending =
            Impl::Request{std::move(desktop), selection, brightness, options, requestId, this->impl_->generation};
    }
    this->impl_->wake.notify_one();
    return true;
}
// 失效正在执行的结果，同时释放尚未执行请求的共享输入。
// 入参：无。返回：无。
void SelectionPreviewWorker::Cancel() noexcept
{
    const std::lock_guard<std::mutex> lock(this->impl_->mutex);
    ++this->impl_->generation;
    this->impl_->pending.reset();
    this->impl_->result.reset();
}
// 移出当前版本唯一结果。
// 入参：无。返回：可空结果。
std::optional<SelectionPreviewResult> SelectionPreviewWorker::TakeResult()
{
    const std::lock_guard<std::mutex> lock(this->impl_->mutex);
    std::optional<SelectionPreviewResult> result = std::move(this->impl_->result);
    this->impl_->result.reset();
    return result;
}
// 在互斥保护下查询转换与等待状态。
// 入参：无。返回：存在工作时true。
bool SelectionPreviewWorker::HasWork() const noexcept
{
    const std::lock_guard<std::mutex> lock(this->impl_->mutex);
    return this->impl_->running || this->impl_->pending.has_value();
}
} // namespace open_st
