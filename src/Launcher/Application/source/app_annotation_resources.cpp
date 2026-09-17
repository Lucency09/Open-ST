// 将冻结来源与Graphics缓存绑定到会话，保护准备期间的重入和资源寿命。
#include "annotation_interaction_controller.h"
#include "capture_annotation_state.h"
#include "capture_overlay_session.h"
#include "overlay_input_queue.h"
#include <annotation_mosaic_source.h>
#include <app.h>
#include <stdexcept>

namespace open_st
{
// 建立仅属于本次冻结帧的来源，并注入状态事务的准备边界。
// 入参：无。返回：无，创建失败交由截图入口回收。
void App::InitializeAnnotationResources()
{
    this->annotationSource_ = std::make_unique<AnnotationMosaicSource>(*this->frozenDesktopFrame_);
    if (!this->annotation_->SetPreviewPreparation(
            // 状态层给出真正目标选区，撤销和移动也不能借用旧裁剪。
            // 入参：document和selection为候选。返回：资源完整准备且会话仍有效时true。
            [this](const AnnotationSnapshot& document, RectI selection)
            { return this->PrepareAnnotationPreview(document, selection); }))
        throw std::runtime_error("Cannot configure annotation preparation");
}
// 同步资源准备期间拒绝重入，失效只标记，外层消息结束后回收。
// 入参：document/selection为候选。返回：完整成功时true，失败不改变状态层快照。
bool App::PrepareAnnotationPreview(const AnnotationSnapshot& document, RectI selection) noexcept
{
    if (!this->annotationSource_ || this->overlayInvalidated_ || this->shuttingDown_ || this->annotationPreparing_ ||
        this->overlayRendering_)
        return false;
    this->annotationPreparing_ = true;
    bool ready = false;
    try
    {
        std::wstring error;
        ready = this->annotationSource_->Prepare(document, selection, error);
    }
    catch (...)
    {
    }
    this->annotationPreparing_ = false;
    if (this->overlaySession_)
        this->overlaySession_->Invalidate();
    return ready && !this->overlayInvalidated_ && !this->shuttingDown_ && !this->overlayInput_->CancellationPending();
}

// 将输入交给独立队列，只有从空变非空时唤醒宿主消息窗口。
// 入参：window/message/flags/point 为原始采样。返回：无，入队失败令会话失效。
void App::QueueOverlayPointer(HWND window, UINT message, WPARAM flags, PointI point) noexcept
{
    const OverlayInputEnqueueResult result = this->overlayInput_->Enqueue({window, message, flags, point.x, point.y});
    if (result == OverlayInputEnqueueResult::Failed ||
        (result == OverlayInputEnqueueResult::Wake && !PostMessageW(this->messageWindow_, WM_APP + 10, 0, 0)))
        this->overlayInvalidated_ = true;
}
// 图形调用返回后借用窗口重放事件，队列拥有次序、取消和重入状态。
// 入参：无。返回：无，会话失效时停止重放。
void App::DrainOverlayPointers() noexcept
try
{
    if (this->annotationPreparing_ || this->overlayRendering_ || this->overlayPreparing_ ||
        !this->annotationInteraction_->CanClose())
        return;
    const bool drained = this->overlayInput_->Drain(
        // 队列只交付采样；宿主核对窗口归属并适配 Win32 重放消息。
        // 入参：event 为采样值。返回：允许继续重放时 true。
        [this](const OverlayPointerSample& event)
        {
            if (this->overlayInvalidated_ || this->shuttingDown_ || !this->overlaySession_)
                return false;
            if (!this->overlaySession_->Find(event.window))
                return true;
            const std::uint64_t packed = static_cast<std::uint32_t>(event.x) |
                                         (static_cast<std::uint64_t>(static_cast<std::uint32_t>(event.y)) << 32);
            SendMessageW(
                event.window, WM_APP + 10,
                static_cast<WPARAM>(MAKELONG(static_cast<WORD>(event.message), static_cast<WORD>(event.flags))),
                static_cast<LPARAM>(packed));
            return !this->overlayInvalidated_ && !this->shuttingDown_ && this->overlaySession_;
        });
    if (!drained && this->overlaySession_)
        this->overlayInvalidated_ = true;
}
catch (...)
{
    this->overlayInvalidated_ = true;
}
} // namespace open_st
