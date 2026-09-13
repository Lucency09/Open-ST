// 声明 App 拥有的单个历史日志任务，结果独立于设置窗口和完成通知的生命周期。
#pragma once

#include <cstdint>
#include <functional>
#include <log.h>
#include <mutex>
#include <optional>
#include <thread>

namespace open_st
{
struct LogMaintenanceSnapshot final
{
    std::uint64_t operation{};
    bool running{};
    std::optional<LogCleanupResult> result;
};

class LogMaintenanceTask final
{
  public:
    // 固定清理函数，正式业务默认调用 Common；构造不启动线程。
    // 入参：cleanup 为清理边界，测试可注入不触碰磁盘的函数。
    // 返回：无；函数所有权移入任务。
    explicit LogMaintenanceTask(std::function<LogCleanupResult(std::stop_token)> cleanup = ClearHistoricalLogs);
    // 请求取消并等待线程，后台不比任务对象长寿。
    // 入参：无。
    // 返回：无。
    ~LogMaintenanceTask();
    // 禁止复制线程与结果的唯一所有权。
    // 入参：未命名源对象。
    // 返回：操作已删除。
    LogMaintenanceTask(const LogMaintenanceTask&) = delete;
    // 禁止复制赋值。
    // 入参：未命名源对象。
    // 返回：操作已删除。
    LogMaintenanceTask& operator=(const LogMaintenanceTask&) = delete;
    // 在所属 UI 线程启动一次清理，完成状态先保存再发送通知。
    // 入参：notify 仅负责跨线程通知操作编号，不访问设置窗口；允许为空或通知失败。
    // 返回：成功启动为 true；已运行、无清理函数或线程创建失败为 false。
    bool Start(std::function<void(std::uint64_t)> notify) noexcept;
    // 在所属 UI 线程请求取消并等待，返回后可安全关闭 Logger 和消息窗口。
    // 入参：无。
    // 返回：无；当前系统 IO 返回后完成等待，不强杀线程。
    void Stop() noexcept;
    // 从任意线程查询当前操作及持久保留的完成结果。
    // 入参：无。
    // 返回：值副本，不转移后台所有权。
    LogMaintenanceSnapshot Snapshot() const;
    // 在 UI 线程收取一次尚未通知界面的完成状态，旧编号不能消费新结果。
    // 入参：operation 为预期编号；snapshot 成功时接收完成状态。
    // 返回：本次消费成功为 true；无新结果或编号过期为 false。
    bool TakeCompletion(std::uint64_t operation, LogMaintenanceSnapshot& snapshot);

  private:
    std::function<LogCleanupResult(std::stop_token)> cleanup_;
    mutable std::mutex mutex_;
    LogMaintenanceSnapshot state_;
    bool completionPending_{};
    std::jthread worker_;
};
} // namespace open_st
