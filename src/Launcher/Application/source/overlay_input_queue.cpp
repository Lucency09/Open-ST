// 文件职责：封装遮罩采样排队、取消屏障、容量和重放生命周期。
#include "overlay_input_queue.h"
#include <algorithm>

namespace open_st
{
// 保持完整采样顺序，内存或容量不足时由宿主结束会话。
// 入参：sample 为需延后处理的事件。
// 返回：入队结果；首次入队需要宿主唤醒消息循环。
OverlayInputEnqueueResult OverlayInputQueue::Enqueue(OverlayPointerSample sample) noexcept
try
{
    if (sample.message == WM_LBUTTONDOWN)
        this->ObservePointerDown();
    else if (this->ignoreUntilDown_ && (sample.message == WM_MOUSEMOVE || sample.message == WM_LBUTTONUP))
        return OverlayInputEnqueueResult::Ignored;
    if (this->samples_.size() >= CAPACITY)
        return OverlayInputEnqueueResult::Failed;
    const bool wake = this->samples_.empty();
    this->samples_.push_back(sample);
    return wake ? OverlayInputEnqueueResult::Wake : OverlayInputEnqueueResult::Queued;
}
catch (...)
{
    return OverlayInputEnqueueResult::Failed;
}

// 原始取消截断旧采样；重放取消不能误删排在其后的新手势。
// 入参：replay、deferred、gesture 分别描述消息来源、图形忙状态和手势存在性。
// 返回：无。
void OverlayInputQueue::BeginCancellation(bool replay, bool deferred, bool gesture) noexcept
{
    if (!replay)
        this->samples_.clear();
    this->ignoreUntilDown_ = true;
    if (deferred)
        this->cancellationPending_ = gesture;
}

// 新按下开启独立手势，解除旧手势的尾部输入屏障。
// 入参：无。
// 返回：无。
void OverlayInputQueue::ObservePointerDown() noexcept
{
    this->ignoreUntilDown_ = false;
}

// 只有最外层调用重放；会话重置使旧调用立即停止，避免消费新会话事件。
// 入参：replay 为同步宿主分派函数，失败时丢弃本代剩余输入。
// 返回：本代顺利完成或嵌套调用被忽略时 true。
bool OverlayInputQueue::Drain(const std::function<bool(const OverlayPointerSample&)>& replay) noexcept
{
    if (this->draining_)
        return true;
    this->draining_ = true;
    const std::uint64_t generation = this->generation_;
    bool success = true;
    try
    {
        while (generation == this->generation_ && !this->samples_.empty())
        {
            const OverlayPointerSample sample = this->samples_.front();
            this->samples_.pop_front();
            success = replay(sample);
            if (generation != this->generation_)
                break;
            if (!success)
            {
                this->samples_.clear();
                break;
            }
            if (sample.message == WM_CANCELMODE || sample.message == WM_KEYDOWN)
                this->cancellationPending_ = false;
        }
    }
    catch (...)
    {
        if (generation == this->generation_)
            this->samples_.clear();
        success = false;
    }
    this->draining_ = false;
    return success;
}

// 会话边界只清理排队状态，不提前释放外层调用仍持有的重放保护。
// 入参：无。
// 返回：无。
void OverlayInputQueue::Reset() noexcept
{
    this->samples_.clear();
    ++this->generation_;
    this->cancellationPending_ = false;
    this->ignoreUntilDown_ = false;
}

// 待处理按下也属于需要被取消的手势。
// 入参：无。
// 返回：队列存在左键按下时 true。
bool OverlayInputQueue::HasPendingDown() const noexcept
{
    return std::any_of(this->samples_.begin(), this->samples_.end(),
                       // 只判断消息类型，不读取外部窗口状态。
                       // 入参：sample 为队列事件。
                       // 返回：左键按下时 true。
                       [](const OverlayPointerSample& sample) { return sample.message == WM_LBUTTONDOWN; });
}

// 返回取消处理的只读状态。
// 入参：无。
// 返回：等待取消重放时 true。
bool OverlayInputQueue::CancellationPending() const noexcept
{
    return this->cancellationPending_;
}

// 返回当前重放栈状态。
// 入参：无。
// 返回：外层重放尚未返回时 true。
bool OverlayInputQueue::Draining() const noexcept
{
    return this->draining_;
}

} // namespace open_st
