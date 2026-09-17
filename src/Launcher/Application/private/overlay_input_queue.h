// 文件职责：拥有遮罩延后输入、取消屏障及重放状态，不调用窗口或图形资源。
#pragma once

#include <Windows.h>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>

namespace open_st
{
struct OverlayPointerSample final
{
    HWND window{};
    UINT message{};
    WPARAM flags{};
    LONG x{}, y{};
};

enum class OverlayInputEnqueueResult
{
    Ignored,
    Queued,
    Wake,
    Failed
};

class OverlayInputQueue final
{
  public:
    static constexpr std::size_t CAPACITY = 8192U;

    // 保存完整指针采样，取消后的移动和抬起不进入队列。
    // 入参：sample 为原始事件及物理坐标。
    // 返回：Wake 要求宿主发送唤醒；Failed 表示容量或分配失败。
    [[nodiscard]] OverlayInputEnqueueResult Enqueue(OverlayPointerSample sample) noexcept;
    // 记录取消屏障；原始取消清除旧采样，重放取消保留其后的新手势。
    // 入参：replay 表示重放事件；deferred 表示尚在图形栈内；gesture 表示存在待取消手势。
    // 返回：无；延后取消的挂起状态由重放取消事件解除。
    void BeginCancellation(bool replay, bool deferred, bool gesture) noexcept;
    // 记录直接处理的新按下，允许随后采样重新进入队列。
    // 入参：无。
    // 返回：无。
    void ObservePointerDown() noexcept;
    // 按序借出采样供宿主重放；嵌套调用不再次分派。
    // 入参：replay 为同步回调，返回 false 时停止并清除当前会话剩余采样。
    // 返回：回调完成或嵌套调用被忽略时 true；回调中止或异常时 false。
    [[nodiscard]] bool Drain(const std::function<bool(const OverlayPointerSample&)>& replay) noexcept;
    // 清理旧会话事件及屏障；正在进行的旧重放不会消费重置后加入的新事件。
    // 入参：无。
    // 返回：无；活动重放保护持续到原调用返回。
    void Reset() noexcept;
    // 查询队列是否含有尚未处理的左键按下。
    // 入参：无。
    // 返回：存在待处理按下为 true。
    [[nodiscard]] bool HasPendingDown() const noexcept;
    // 查询资源准备是否须因延后取消而拒绝发布。
    // 入参：无。
    // 返回：当前手势取消尚未重放时 true。
    [[nodiscard]] bool CancellationPending() const noexcept;
    // 查询是否正在外层重放，供宿主区分正常 ReleaseCapture 重入。
    // 入参：无。
    // 返回：重放栈活动时 true。
    [[nodiscard]] bool Draining() const noexcept;

  private:
    friend struct AppAnnotationTextTestAccess;
    friend struct OverlayInputQueueTestAccess;
    std::deque<OverlayPointerSample> samples_;
    std::uint64_t generation_{};
    bool draining_{};
    bool cancellationPending_{};
    bool ignoreUntilDown_{};
};
} // namespace open_st
