// 连接维护页、单个后台日志任务和 UI 线程通知，清理不借用设置窗口。
#include "log_maintenance_task.h"
#include <app.h>
#include <settings_window.h>
#include <shellapi.h>
#include <ui_text.h>

namespace open_st
{
// 使用 Logger 提供的实际目录打开资源管理器，不推测路径、不启动 shell 命令。
// 入参：无。
// 返回：目录存在且系统接受打开请求时为 true，失败不改变日志服务。
bool App::OpenLogDirectory() noexcept
{
    try
    {
        const std::optional<std::filesystem::path> directory = GetLoggingDirectory();
        std::error_code error;
        if (!directory || !std::filesystem::is_directory(*directory, error) || error)
            return false;
        SHELLEXECUTEINFOW request{sizeof(request)};
        request.hwnd = this->DialogOwner();
        request.lpVerb = L"open";
        request.lpFile = directory->c_str();
        request.nShow = SW_SHOWNORMAL;
        request.fMask = SEE_MASK_FLAG_NO_UI;
        return ShellExecuteExW(&request) != FALSE;
    }
    catch (...)
    {
        return false;
    }
}

// 启动专用后台任务，完成通知只携带操作编号。
// 入参：无。
// 返回：已启动为 true，退出或重复请求拒绝。
bool App::StartHistoricalLogCleanup() noexcept
{
    if (this->shuttingDown_ || !IsWindow(this->messageWindow_))
        return false;
    try
    {
        if (!this->logMaintenance_)
            this->logMaintenance_ = std::make_unique<LogMaintenanceTask>();
        // 消息窗口在 StopLogMaintenance 完成后才销毁；后台只投递通知，不调用界面代码。
        // 入参：operation 为 App 任务拥有的单调编号。
        // 返回：无；投递失败时结果仍保存在任务中。
        return this->logMaintenance_->Start(
            [this](std::uint64_t operation)
            {
                if (!PostMessageW(this->messageWindow_, WM_APP + 7, static_cast<WPARAM>(operation), 0))
                    OPEN_ST_LOG_WARNING("Log maintenance notification failed. win32_error=", GetLastError());
            });
    }
    catch (...)
    {
        OPEN_ST_LOG_ERROR("Log maintenance task could not start.");
        return false;
    }
}

// 把后台值类型状态转换成维护页文本，语言变化不重跑任务。
// 入参：无。
// 返回：当前忙状态和已本地化结果。
SettingsMaintenanceStatus App::QueryLogMaintenanceStatus() const
{
    if (!this->logMaintenance_)
        return {false, GetUiText("settings.maintenance.idle")};
    const LogMaintenanceSnapshot snapshot = this->logMaintenance_->Snapshot();
    if (snapshot.running)
        return {true, GetUiText("settings.maintenance.running")};
    if (!snapshot.result)
        return {false, GetUiText("settings.maintenance.idle")};
    const LogCleanupResult& result = *snapshot.result;
    const char* key = "settings.maintenance.unavailable";
    switch (result.status)
    {
    case LogCleanupStatus::Completed:
        key = "settings.maintenance.completed";
        break;
    case LogCleanupStatus::PartialFailure:
        key = "settings.maintenance.partial";
        break;
    case LogCleanupStatus::Cancelled:
        key = "settings.maintenance.cancelled";
        break;
    case LogCleanupStatus::Unavailable:
        break;
    }
    const std::wstring deleted = std::to_wstring(result.deleted);
    const std::wstring failed = std::to_wstring(result.failed);
    return {false, GetUiText(key, {{L"deleted", deleted}, {L"failed", failed}})};
}

// 消费一次完成通知，关闭设置后仍保留 Snapshot 中的结果。
// 入参：无。
// 返回：无；异常不穿过 App 消息边界。
void App::DrainLogMaintenance() noexcept
{
    try
    {
        if (!this->logMaintenance_ || this->settingsBusy_ || this->dialogActive_ || this->completionBusy_ ||
            this->welcoming_ || this->shuttingDown_)
            return;
        const LogMaintenanceSnapshot snapshot = this->logMaintenance_->Snapshot();
        LogMaintenanceSnapshot completed;
        if (!this->logMaintenance_->TakeCompletion(snapshot.operation, completed) || !completed.result)
            return;
        OPEN_ST_LOG_INFO("Historical log cleanup finished. status=", static_cast<int>(completed.result->status),
                         " deleted=", completed.result->deleted, " failed=", completed.result->failed);
        if (this->settingsWindow_ && this->settingsWindow_->IsOpen())
            this->settingsWindow_->RefreshTexts();
    }
    catch (...)
    {
        OPEN_ST_LOG_ERROR("Log maintenance UI refresh failed.");
    }
}

// 退出前先等待 worker，之后消息窗口和日志服务才可释放。
// 入参：无。
// 返回：无。
void App::StopLogMaintenance() noexcept
{
    if (this->logMaintenance_)
        this->logMaintenance_->Stop();
}
} // namespace open_st
