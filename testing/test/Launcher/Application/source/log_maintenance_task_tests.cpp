// 验证历史日志后台任务的单任务约束、完成所有权和取消回收，不访问真实日志。

#include "log_maintenance_task.h"
#include <gtest/gtest.h>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>

namespace open_st
{
namespace
{
class TaskEvent final
{
  public:
    // 发布单次事件，使工作线程和测试线程按明确阶段同步。
    // 入参：无。
    // 返回：无；事件保持已触发状态。
    void Signal()
    {
        const std::scoped_lock<std::mutex> lock(this->mutex_);
        this->signalled_ = true;
        this->condition_.notify_all();
    }

    // 有界等待事件，测试异常时也不无限等待后台线程。
    // 入参：无。
    // 返回：五秒内收到事件为 true，超时为 false。
    bool Wait()
    {
        std::unique_lock<std::mutex> lock(this->mutex_);
        // 检查事件已发布，容许虚假唤醒。
        // 入参：无。
        // 返回：事件状态。
        return this->condition_.wait_for(lock, std::chrono::seconds(5), [this]() { return this->signalled_; });
    }

  private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool signalled_{};
};

// 验证清理尚未结束时拒绝第二次启动，不改变操作编号。
// 入参：无。
// 返回：无；断言报告单任务约束和有界同步结果。
TEST(LogMaintenanceTaskTest, rejects_duplicate_start_while_running)
{
    TaskEvent entered;
    TaskEvent release;
    // 保持任务运行，直到测试允许完成；超时产生可检查的错误状态。
    // 入参：未命名取消令牌，本用例用独立事件释放。
    // 返回：完成或等待超时结果。
    LogMaintenanceTask task([&entered, &release](std::stop_token)
    {
        entered.Signal();
        return LogCleanupResult{release.Wait() ? LogCleanupStatus::Completed : LogCleanupStatus::Unavailable};
    });
    ASSERT_TRUE(task.Start({}));
    ASSERT_TRUE(entered.Wait());
    const LogMaintenanceSnapshot before = task.Snapshot();
    EXPECT_TRUE(before.running);
    EXPECT_FALSE(task.Start({}));
    EXPECT_EQ(task.Snapshot().operation, before.operation);
    release.Signal();
    task.Stop();
    const LogMaintenanceSnapshot after = task.Snapshot();
    ASSERT_TRUE(after.result.has_value());
    EXPECT_EQ(after.result->status, LogCleanupStatus::Completed);
}

// 验证完成回调开始前结果已经发布，并保留完整结构化计数。
// 入参：无。
// 返回：无；断言报告通知时查询到的状态。
TEST(LogMaintenanceTaskTest, publishes_result_before_notification)
{
    TaskEvent notified;
    LogMaintenanceSnapshot observed;
    std::uint64_t notifiedOperation{};
    // 生成有可辨识计数的结果，不访问文件系统。
    // 入参：未命名取消令牌。
    // 返回：部分失败和全部计数。
    LogMaintenanceTask task(
        [](std::stop_token) { return LogCleanupResult{LogCleanupStatus::PartialFailure, 2, 1, 3, 4}; });
    // 在通知边界同步读取已发布状态，再允许主线程检查。
    // 入参：operation 为完成任务编号。
    // 返回：无。
    ASSERT_TRUE(task.Start([&](std::uint64_t operation)
    {
        observed = task.Snapshot();
        notifiedOperation = operation;
        notified.Signal();
    }));
    ASSERT_TRUE(notified.Wait());
    task.Stop();
    EXPECT_FALSE(observed.running);
    EXPECT_EQ(observed.operation, notifiedOperation);
    ASSERT_TRUE(observed.result.has_value());
    EXPECT_EQ(observed.result->status, LogCleanupStatus::PartialFailure);
    EXPECT_EQ(observed.result->deleted, 2U);
    EXPECT_EQ(observed.result->failed, 1U);
    EXPECT_EQ(observed.result->retained, 3U);
    EXPECT_EQ(observed.result->alreadyMissing, 4U);
}
// 验证通知抛异常时后台线程仍可回收，结果仍由 UI 主动取得。
// 入参：无。
// 返回：无；断言报告失败通知后的结果所有权。
TEST(LogMaintenanceTaskTest, failed_notification_keeps_completion_available)
{
    TaskEvent notified;
    // 返回固定成功结果，模拟已经完成的历史删除。
    // 入参：未命名取消令牌。
    // 返回：删除两项的完成状态。
    LogMaintenanceTask task([](std::stop_token) { return LogCleanupResult{LogCleanupStatus::Completed, 2}; });
    // 模拟通知投递边界失败；事件只用于主线程同步。
    // 入参：未命名操作编号。
    // 返回：不正常返回，故意抛出异常。
    ASSERT_TRUE(task.Start([&notified](std::uint64_t)
    {
        notified.Signal();
        throw std::runtime_error("notification failed");
    }));
    ASSERT_TRUE(notified.Wait());
    task.Stop();
    LogMaintenanceSnapshot completion;
    ASSERT_TRUE(task.TakeCompletion(task.Snapshot().operation, completion));
    ASSERT_TRUE(completion.result.has_value());
    EXPECT_EQ(completion.result->status, LogCleanupStatus::Completed);
    EXPECT_EQ(completion.result->deleted, 2U);
}

// 验证旧完成消息不能消费新操作结果，错误编号也不改调用方输出。
// 入参：无。
// 返回：无；断言报告操作编号隔离。
TEST(LogMaintenanceTaskTest, stale_operation_cannot_consume_new_completion)
{
    TaskEvent firstNotified;
    TaskEvent secondNotified;
    // 每次操作独立返回完成，避免混入文件删除行为。
    // 入参：未命名取消令牌。
    // 返回：正常完成状态。
    LogMaintenanceTask task([](std::stop_token) { return LogCleanupResult{LogCleanupStatus::Completed}; });
    // 通知第一轮完成，允许主线程发起下一轮。
    // 入参：未命名操作编号。
    // 返回：无。
    ASSERT_TRUE(task.Start([&firstNotified](std::uint64_t) { firstNotified.Signal(); }));
    ASSERT_TRUE(firstNotified.Wait());
    task.Stop();
    const std::uint64_t firstOperation = task.Snapshot().operation;
    // 通知第二轮完成，供测试注入过期编号。
    // 入参：未命名操作编号。
    // 返回：无。
    ASSERT_TRUE(task.Start([&secondNotified](std::uint64_t) { secondNotified.Signal(); }));
    ASSERT_TRUE(secondNotified.Wait());
    task.Stop();
    const std::uint64_t secondOperation = task.Snapshot().operation;
    ASSERT_NE(firstOperation, secondOperation);
    LogMaintenanceSnapshot completion;
    completion.operation = firstOperation;
    EXPECT_FALSE(task.TakeCompletion(firstOperation, completion));
    EXPECT_EQ(completion.operation, firstOperation);
    ASSERT_TRUE(task.TakeCompletion(secondOperation, completion));
    EXPECT_EQ(completion.operation, secondOperation);
    EXPECT_TRUE(completion.result.has_value());
}

// 验证 Stop 请求取消并等待工作函数及其通知结束，随后可安全销毁外部服务。
// 入参：无。
// 返回：无；断言报告取消是否可见和回收后的最终状态。
TEST(LogMaintenanceTaskTest, stop_requests_cancellation_and_joins_worker)
{
    TaskEvent entered;
    TaskEvent cancelled;
    bool cleanupReturned{};
    bool notificationReturned{};
    // 使用取消回调唤醒有界等待，验证 Stop 的请求与回收顺序。
    // 入参：stop 为任务取消令牌。
    // 返回：实际收到取消时为 Cancelled，超时为 Unavailable。
    LogMaintenanceTask task([&](std::stop_token stop)
    {
        // 将取消请求转成测试事件，不触碰产品线程或窗口。
        // 入参：无。
        // 返回：无。
        std::stop_callback onStop(stop, [&cancelled]() { cancelled.Signal(); });
        entered.Signal();
        const bool received = cancelled.Wait();
        cleanupReturned = true;
        return LogCleanupResult{received && stop.stop_requested() ? LogCleanupStatus::Cancelled
                                                                 : LogCleanupStatus::Unavailable};
    });
    // 记录通知也已执行完毕，Stop 返回后读取由 join 同步。
    // 入参：未命名操作编号。
    // 返回：无。
    ASSERT_TRUE(task.Start([&notificationReturned](std::uint64_t) { notificationReturned = true; }));
    ASSERT_TRUE(entered.Wait());
    task.Stop();
    EXPECT_TRUE(cleanupReturned);
    EXPECT_TRUE(notificationReturned);
    const LogMaintenanceSnapshot snapshot = task.Snapshot();
    EXPECT_FALSE(snapshot.running);
    ASSERT_TRUE(snapshot.result.has_value());
    EXPECT_EQ(snapshot.result->status, LogCleanupStatus::Cancelled);
    task.Stop();
    EXPECT_EQ(task.Snapshot().operation, snapshot.operation);
}

// 验证工作函数抛出的异常转成不可用结果，线程不会异常终止应用。
// 入参：无。
// 返回：无；断言报告异常转换及完成可消费状态。
TEST(LogMaintenanceTaskTest, cleanup_exception_becomes_unavailable_result)
{
    TaskEvent notified;
    // 模拟清理边界意外抛异常，不使用真实磁盘失败。
    // 入参：未命名取消令牌。
    // 返回：不正常返回，抛出异常。
    LogMaintenanceTask task([](std::stop_token) -> LogCleanupResult { throw std::runtime_error("cleanup failed"); });
    // 异常结果仍须发送完成通知。
    // 入参：未命名操作编号。
    // 返回：无。
    ASSERT_TRUE(task.Start([&notified](std::uint64_t) { notified.Signal(); }));
    ASSERT_TRUE(notified.Wait());
    task.Stop();
    LogMaintenanceSnapshot completion;
    ASSERT_TRUE(task.TakeCompletion(task.Snapshot().operation, completion));
    ASSERT_TRUE(completion.result.has_value());
    EXPECT_EQ(completion.result->status, LogCleanupStatus::Unavailable);
    EXPECT_FALSE(completion.running);
}

// 验证完成只消费一次，但快照保留结果供设置窗口关闭后重新打开查询。
// 入参：无。
// 返回：无；断言报告消费与持久展示状态相互独立。
TEST(LogMaintenanceTaskTest, consumes_completion_once_but_preserves_snapshot)
{
    TaskEvent notified;
    // 提供易识别结果，检查消费后没有丢弃展示数据。
    // 入参：未命名取消令牌。
    // 返回：删除七项的完成状态。
    LogMaintenanceTask task([](std::stop_token) { return LogCleanupResult{LogCleanupStatus::Completed, 7}; });
    // 同步完成发布，避免依赖调度速度。
    // 入参：未命名操作编号。
    // 返回：无。
    ASSERT_TRUE(task.Start([&notified](std::uint64_t) { notified.Signal(); }));
    ASSERT_TRUE(notified.Wait());
    task.Stop();
    const std::uint64_t operation = task.Snapshot().operation;
    LogMaintenanceSnapshot completion;
    ASSERT_TRUE(task.TakeCompletion(operation, completion));
    EXPECT_FALSE(task.TakeCompletion(operation, completion));
    const LogMaintenanceSnapshot retained = task.Snapshot();
    EXPECT_EQ(retained.operation, operation);
    EXPECT_FALSE(retained.running);
    ASSERT_TRUE(retained.result.has_value());
    EXPECT_EQ(retained.result->status, LogCleanupStatus::Completed);
    EXPECT_EQ(retained.result->deleted, 7U);
}
} // namespace
} // namespace open_st
