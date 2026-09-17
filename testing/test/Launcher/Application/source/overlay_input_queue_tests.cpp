// 文件职责：验证延后输入的取消边界、重入和会话寿命，不创建窗口或移动鼠标。
#include "overlay_input_queue.h"
#include <gtest/gtest.h>
#include <stdexcept>
#include <vector>

namespace open_st
{
struct OverlayInputQueueTestAccess final
{
    // 在测试中检查排队数量，生产组件不提供无业务调用的观察接口。
    // 入参：queue 为测试独占的队列。
    // 返回：未消费的采样数。
    static std::size_t Size(const OverlayInputQueue& queue)
    {
        return queue.samples_.size();
    }
};
namespace
{
// 构造带完整坐标的独立采样，避免测试依赖真实光标位置。
// 入参：message 为消息类型；x 为可区分采样的物理横坐标。
// 返回：无外部窗口所有权的值对象。
OverlayPointerSample Sample(UINT message, LONG x = 0)
{
    return {nullptr, message, MK_LBUTTON, x, -70000};
}
} // namespace

// 验证取消截断旧手势，并由新按下恢复采样而不丢失后续坐标。
// 入参：无；使用本地队列和固定物理坐标。
// 返回：无；通过断言报告取消与恢复契约。
TEST(OverlayInputQueueTest, cancellation_discards_old_tail_and_new_down_resumes)
{
    OverlayInputQueue queue;
    ASSERT_EQ(queue.Enqueue(Sample(WM_LBUTTONDOWN, 1)), OverlayInputEnqueueResult::Wake);
    ASSERT_EQ(queue.Enqueue(Sample(WM_MOUSEMOVE, 2)), OverlayInputEnqueueResult::Queued);
    ASSERT_TRUE(queue.HasPendingDown());
    queue.BeginCancellation(false, true, true);
    EXPECT_TRUE(queue.CancellationPending());
    EXPECT_FALSE(queue.HasPendingDown());
    EXPECT_EQ(queue.Enqueue(Sample(WM_MOUSEMOVE, 3)), OverlayInputEnqueueResult::Ignored);
    EXPECT_EQ(queue.Enqueue(Sample(WM_LBUTTONUP, 4)), OverlayInputEnqueueResult::Ignored);
    ASSERT_EQ(queue.Enqueue(Sample(WM_CANCELMODE)), OverlayInputEnqueueResult::Wake);
    ASSERT_EQ(queue.Enqueue(Sample(WM_LBUTTONDOWN, 5)), OverlayInputEnqueueResult::Queued);
    ASSERT_EQ(queue.Enqueue(Sample(WM_MOUSEMOVE, 6)), OverlayInputEnqueueResult::Queued);
    std::vector<LONG> seen;
    ASSERT_TRUE(queue.Drain(
        // 取消消息沿生产路径再次进入屏障，不能删除其后已入队的新手势。
        // 入参：sample 为当前重放事件。
        // 返回：继续重放为 true。
        [&](const OverlayPointerSample& sample)
        {
            if (sample.message == WM_CANCELMODE)
                queue.BeginCancellation(true, false, true);
            if (sample.message == WM_LBUTTONDOWN)
                queue.ObservePointerDown();
            seen.push_back(sample.x);
            EXPECT_EQ(sample.y, -70000);
            return true;
        }));
    EXPECT_EQ(seen, (std::vector<LONG>{0, 5, 6}));
    EXPECT_FALSE(queue.CancellationPending());
    EXPECT_EQ(queue.Enqueue(Sample(WM_MOUSEMOVE, 7)), OverlayInputEnqueueResult::Wake);
}

// 验证窗口分派重入不会夺走外层排队事件。
// 入参：无；在外层分派回调内同步调用重放。
// 返回：无；通过断言报告唯一重放和次序。
TEST(OverlayInputQueueTest, nested_drain_cannot_consume_events_from_outer_drain)
{
    OverlayInputQueue queue;
    ASSERT_EQ(queue.Enqueue(Sample(WM_LBUTTONDOWN, 1)), OverlayInputEnqueueResult::Wake);
    ASSERT_EQ(queue.Enqueue(Sample(WM_MOUSEMOVE, 2)), OverlayInputEnqueueResult::Queued);
    std::vector<LONG> seen;
    unsigned nestedCalls = 0;
    ASSERT_TRUE(queue.Drain(
        // 嵌套分派由外层继续保持顺序和唯一所有权。
        // 入参：sample 为当前事件。
        // 返回：继续重放。
        [&](const OverlayPointerSample& sample)
        {
            EXPECT_TRUE(queue.Draining());
            EXPECT_TRUE(queue.Drain(
                // 嵌套回调不应执行。
                // 入参：未使用采样。
                // 返回：继续值不能绕过外层重入保护。
                [&](const OverlayPointerSample&)
                {
                    ++nestedCalls;
                    return true;
                }));
            seen.push_back(sample.x);
            return true;
        }));
    EXPECT_EQ(nestedCalls, 0U);
    EXPECT_EQ(seen, (std::vector<LONG>{1, 2}));
    EXPECT_FALSE(queue.Draining());
}

// 验证旧会话回调内重置后，新事件留给新会话外层重放。
// 入参：无；同步模拟关闭和重新开始截图。
// 返回：无；通过断言报告代次隔离。
TEST(OverlayInputQueueTest, reset_during_replay_does_not_consume_new_session_events)
{
    OverlayInputQueue queue;
    ASSERT_EQ(queue.Enqueue(Sample(WM_LBUTTONDOWN, 1)), OverlayInputEnqueueResult::Wake);
    ASSERT_EQ(queue.Enqueue(Sample(WM_MOUSEMOVE, 2)), OverlayInputEnqueueResult::Queued);
    unsigned oldCalls = 0;
    EXPECT_TRUE(queue.Drain(
        // 模拟同步窗口消息关闭旧会话并建立新会话。
        // 入参：未使用旧事件。
        // 返回：回调成功，旧重放仍须因代次变化而停止。
        [&](const OverlayPointerSample&)
        {
            ++oldCalls;
            queue.Reset();
            EXPECT_TRUE(queue.Draining());
            EXPECT_EQ(queue.Enqueue(Sample(WM_LBUTTONDOWN, 3)), OverlayInputEnqueueResult::Wake);
            return true;
        }));
    EXPECT_EQ(oldCalls, 1U);
    EXPECT_EQ(OverlayInputQueueTestAccess::Size(queue), 1U);
    EXPECT_TRUE(queue.Drain(
        // 新会话只在新的外层重放中消费。
        // 入参：sample 为新会话采样。
        // 返回：继续。
        [](const OverlayPointerSample& sample)
        {
            EXPECT_EQ(sample.x, 3);
            return true;
        }));
    EXPECT_EQ(OverlayInputQueueTestAccess::Size(queue), 0U);
}

// 验证会话结束清除取消挂起和采样屏障。
// 入参：无；重置包含取消事件的队列。
// 返回：无；通过断言报告新会话输入可用性。
TEST(OverlayInputQueueTest, reset_clears_cancellation_and_allows_new_session_move)
{
    OverlayInputQueue queue;
    queue.BeginCancellation(false, true, true);
    ASSERT_EQ(queue.Enqueue(Sample(WM_CANCELMODE)), OverlayInputEnqueueResult::Wake);
    queue.Reset();
    EXPECT_FALSE(queue.CancellationPending());
    EXPECT_EQ(OverlayInputQueueTestAccess::Size(queue), 0U);
    EXPECT_EQ(queue.Enqueue(Sample(WM_MOUSEMOVE)), OverlayInputEnqueueResult::Wake);
}

// 验证容量耗尽只拒绝新事件，已接受采样仍完整按序重放。
// 入参：无；构造达到独立容量上限的固定序列。
// 返回：无；通过断言报告容量与无合并契约。
TEST(OverlayInputQueueTest, capacity_failure_preserves_all_accepted_samples)
{
    OverlayInputQueue queue;
    for (std::size_t index = 0; index < OverlayInputQueue::CAPACITY; ++index)
    {
        ASSERT_EQ(queue.Enqueue(Sample(WM_MOUSEMOVE, static_cast<LONG>(index))),
                  index == 0 ? OverlayInputEnqueueResult::Wake : OverlayInputEnqueueResult::Queued);
    }
    EXPECT_EQ(queue.Enqueue(Sample(WM_MOUSEMOVE, 9000)), OverlayInputEnqueueResult::Failed);
    LONG expected = 0;
    EXPECT_TRUE(queue.Drain(
        // 满队列也不得合并或丢失已接受的采样。
        // 入参：sample 为当前事件。
        // 返回：继续。
        [&](const OverlayPointerSample& sample)
        {
            EXPECT_EQ(sample.x, expected++);
            return true;
        }));
    EXPECT_EQ(expected, static_cast<LONG>(OverlayInputQueue::CAPACITY));
}

// 验证宿主失效时丢弃本代剩余事件并解除重放保护。
// 入参：无；分派回调返回失败。
// 返回：无；通过断言报告失败清理。
TEST(OverlayInputQueueTest, failed_dispatch_drops_current_session_tail_and_releases_guard)
{
    OverlayInputQueue queue;
    ASSERT_EQ(queue.Enqueue(Sample(WM_LBUTTONDOWN)), OverlayInputEnqueueResult::Wake);
    ASSERT_EQ(queue.Enqueue(Sample(WM_MOUSEMOVE)), OverlayInputEnqueueResult::Queued);
    EXPECT_FALSE(queue.Drain(
        // 模拟宿主窗口已经失效，要求本代剩余事件丢弃。
        // 入参：未使用事件。
        // 返回：false 表示中止重放。
        [](const OverlayPointerSample&) { return false; }));
    EXPECT_EQ(OverlayInputQueueTestAccess::Size(queue), 0U);
    EXPECT_FALSE(queue.Draining());
}

// 验证分派异常清理旧事件并允许后续重新入队。
// 入参：无；分派回调抛出受控异常。
// 返回：无；通过断言报告异常恢复。
TEST(OverlayInputQueueTest, throwing_dispatch_releases_guard_without_leaking_old_events)
{
    OverlayInputQueue queue;
    ASSERT_EQ(queue.Enqueue(Sample(WM_LBUTTONDOWN)), OverlayInputEnqueueResult::Wake);
    EXPECT_FALSE(queue.Drain(
        // 验证窗口分派异常不能令队列永久停在重放状态。
        // 入参：未使用事件。
        // 返回：抛出受控异常。
        [](const OverlayPointerSample&) -> bool { throw std::runtime_error("dispatch failed"); }));
    EXPECT_EQ(OverlayInputQueueTestAccess::Size(queue), 0U);
    EXPECT_FALSE(queue.Draining());
    EXPECT_EQ(queue.Enqueue(Sample(WM_LBUTTONDOWN)), OverlayInputEnqueueResult::Wake);
}
} // namespace open_st
