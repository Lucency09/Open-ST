// 实现可取消的单任务与先持有结果后通知，后台从不借用 Settings 对象。
#include "log_maintenance_task.h"
#include <limits>
#include <utility>

namespace open_st
{
// 保存清理实现，线程在 Start 时才创建。
// 入参：cleanup 为可注入同步清理函数。
// 返回：无。
LogMaintenanceTask::LogMaintenanceTask(std::function<LogCleanupResult(std::stop_token)> cleanup)
    : cleanup_(std::move(cleanup))
{
}

// 确保工作线程先于成员数据销毁。
// 入参：无。
// 返回：无。
LogMaintenanceTask::~LogMaintenanceTask()
{
    this->Stop();
}

// 在 UI 线程发起清理，重复请求不会同时启动两个任务。
// 入参：notify 为只接收编号的可选完成通知。
// 返回：成功启动为 true；失败保留可查询的非运行状态。
bool LogMaintenanceTask::Start(std::function<void(std::uint64_t)> notify) noexcept
{
    try
    {
        {
            const std::scoped_lock<std::mutex> lock(this->mutex_);
            if (this->state_.running || !this->cleanup_ ||
                this->state_.operation == (std::numeric_limits<std::uint64_t>::max)())
                return false;
        }
        if (this->worker_.joinable())
            this->worker_.join();
        std::uint64_t operation{};
        {
            const std::scoped_lock<std::mutex> lock(this->mutex_);
            operation = ++this->state_.operation;
            this->state_.running = true;
            this->state_.result.reset();
            this->completionPending_ = false;
        }
        // 在 worker 内完成 IO，发布结果后调用轻量通知；异常收敛为不可用结果。
        // 入参：stop 为本次线程的取消令牌，捕获操作编号和通知函数。
        // 返回：无。
        this->worker_ = std::jthread(
            [this, operation, notify = std::move(notify)](std::stop_token stop)
            {
                LogCleanupResult result;
                try
                {
                    result = this->cleanup_(stop);
                }
                catch (...)
                {
                    result.status = LogCleanupStatus::Unavailable;
                }
                {
                    const std::scoped_lock<std::mutex> lock(this->mutex_);
                    this->state_.result = result;
                    this->state_.running = false;
                    this->completionPending_ = true;
                }
                try
                {
                    if (notify)
                        notify(operation);
                }
                catch (...)
                {
                    // 通知不是结果的唯一保存位置，主循环或下次打开设置仍能取得结果。
                }
            });
        return true;
    }
    catch (...)
    {
        const std::scoped_lock<std::mutex> lock(this->mutex_);
        this->state_.running = false;
        this->state_.result = LogCleanupResult{};
        this->completionPending_ = true;
        return false;
    }
}

// 仅由 UI 所属线程停止任务，保持通知目标活到线程真正结束。
// 入参：无。
// 返回：无。
void LogMaintenanceTask::Stop() noexcept
{
    if (this->worker_.joinable())
    {
        this->worker_.request_stop();
        this->worker_.join();
    }
}

// 读取任务状态副本，允许工作线程已结束但消息尚未送达。
// 入参：无。
// 返回：当前状态。
LogMaintenanceSnapshot LogMaintenanceTask::Snapshot() const
{
    const std::scoped_lock<std::mutex> lock(this->mutex_);
    return this->state_;
}

// 拒绝过期通知，消费标志不清除已完成结果。
// 入参：operation 为消息或查询看到的编号；snapshot 输出完成状态。
// 返回：有相同编号的待收取结果为 true。
bool LogMaintenanceTask::TakeCompletion(std::uint64_t operation, LogMaintenanceSnapshot& snapshot)
{
    const std::scoped_lock<std::mutex> lock(this->mutex_);
    if (this->state_.running || !this->completionPending_ || this->state_.operation != operation)
        return false;
    snapshot = this->state_;
    this->completionPending_ = false;
    return true;
}
} // namespace open_st
