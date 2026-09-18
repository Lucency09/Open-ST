// 将更新任务绑定到公共关于表单；不解析网络响应、不拥有下载文件或配置副本。
#include "about_window.h"
#include <atomic>
#include <message_dialog.h>
#include <ui_text.h>
#include <update_client.h>
#include <window_renderer.h>

namespace open_st
{
namespace
{
constexpr UINT UPDATE_WAKE = WM_APP + 111;
std::atomic<std::uint64_t> nextWindow{1};
// 将不含用户内容的错误类别映射为现有本地化入口。
// 入参：error 为更新结果。返回：产品文字键。
std::string_view ErrorText(UpdateError error) noexcept
{
    switch (error)
    {
    case UpdateError::Configuration:
        return "update.configuration_error";
    case UpdateError::Timeout:
        return "update.timeout";
    case UpdateError::NotFound:
        return "update.not_found";
    case UpdateError::RateLimited:
        return "update.rate_limited";
    case UpdateError::InvalidRelease:
        return "update.invalid_release";
    case UpdateError::MissingPackage:
        return "update.missing_package";
    case UpdateError::Integrity:
        return "update.integrity_error";
    case UpdateError::Storage:
        return "update.storage_error";
    case UpdateError::Busy:
        return "update.file_busy";
    case UpdateError::Unavailable:
        return "update.operation_failed";
    default:
        return "update.network_error";
    }
}
struct AboutDialog
{
    const AboutWindowOptions& options;
    WindowRenderer renderer;
    std::unique_ptr<UpdateClient> client;
    std::uint64_t serial = nextWindow.fetch_add(1), seen{};
    bool closing{}, prompting{};

    // 统一窗口关闭语义，取消在请求关闭之前生效。
    // 入参：无。返回：无。
    void Close()
    {
        this->closing = true;
        if (this->client)
            this->client->Cancel();
        (void)this->renderer.RequestClose();
    }
    // 使用公共消息组件提供确认或普通提示，不复制窗口实现。
    // 入参：text 为已本地化内容；confirm 为双按钮模式。返回：明确确认时 true。
    bool Message(std::wstring text, bool confirm = false)
    {
        MessageDialogOptions message;
        message.owner = this->renderer.NativeHandle();
        message.icon = this->options.icon;
        message.confirmation = confirm;
        // 标题由统一本地化提供。
        // 入参：无。返回：窗口标题。
        message.title = []() { return GetUiText("update.title"); };
        // 借用这次消息的固定文本，不接受后台线程修改。
        // 入参：无。返回：正文。
        message.message = [text = std::move(text)]() { return text; };
        // 所有确认都沿用公共按钮文案。
        // 入参：无。返回：确认文字。
        message.acceptText = []() { return GetUiText("dialog.ok"); };
        // 取消同样复用公共文案。
        // 入参：无。返回：取消文字。
        message.cancelText = []() { return GetUiText("dialog.cancel"); };
        WindowRenderer* active = nullptr;
        message.activeRenderer = &active;
        // 子模态中仍传递宿主消息，退出时关闭自己，不消费另一个更新动作。
        // 入参：input 为线程消息。返回：宿主消费时 true。
        message.processThreadMessage = [this, &active](MSG& input)
        {
            if (this->options.stopping && this->options.stopping())
            {
                this->closing = true;
                this->client->Cancel();
                if (active)
                    (void)active->RequestClose();
            }
            if (input.message == UPDATE_WAKE && input.wParam == this->serial)
                return true;
            return this->options.processThreadMessage && this->options.processThreadMessage(input);
        };
        this->prompting = true;
        const MessageDialogResult result = ShowMessageDialog(message);
        this->prompting = false;
        return result == MessageDialogResult::Accepted && !this->closing &&
               !(this->options.stopping && this->options.stopping());
    }
    // 刷新只读任务结果，阶段变化最多产生一个确认或启动意图。
    // 入参：无。返回：无，当前窗口关闭后不执行任何外部动作。
    void Refresh()
    {
        if (this->closing || this->prompting)
            return;
        if (this->options.stopping && this->options.stopping())
        {
            this->Close();
            return;
        }
        const UpdateSnapshot value = this->client->Snapshot();
        if (value.revision == this->seen)
            return;
        this->seen = value.revision;
        const bool busy = value.phase == UpdatePhase::Checking || value.phase == UpdatePhase::Downloading;
        (void)this->renderer.SetEnabled("check", !busy);
        (void)this->renderer.SetEnabled("cancelUpdate", busy);
        if (value.phase == UpdatePhase::Checking)
            (void)this->renderer.SetStatus(GetUiText("update.checking"));
        else if (value.phase == UpdatePhase::Downloading)
        {
            const std::wstring progress = std::to_wstring(value.total ? value.received * 100 / value.total : 0);
            (void)this->renderer.SetStatus(GetUiText("update.downloading", {{L"percent", progress}}));
        }
        else if (value.phase == UpdatePhase::Cancelled)
            (void)this->renderer.SetStatus(GetUiText("update.cancelled"));
        else if (value.phase == UpdatePhase::Available)
        {
            (void)this->renderer.SetStatus(L"");
            const std::wstring current(value.currentVersion.begin(), value.currentVersion.end());
            const std::wstring latest(value.latestVersion.begin(), value.latestVersion.end());
            const bool installed = this->options.distribution == "installed";
            const bool accepted =
                this->Message(GetUiText(installed ? "update.confirm_install" : "update.confirm_portable",
                                        {{L"current", current}, {L"latest", latest}}),
                              true);
            if (!accepted)
            {
                this->client->Cancel();
                this->Refresh();
                return;
            }
            if (installed)
            {
                if (!this->client->DownloadConfirmed() && this->client->Snapshot().phase != UpdatePhase::Failed)
                    (void)this->Message(GetUiText("update.operation_failed"));
                this->Refresh();
            }
            else
            {
                const bool opened = this->options.openPage && this->options.openPage(value.releasePage);
                if (!opened)
                    (void)this->Message(GetUiText("update.open_failed"));
                (void)this->renderer.SetStatus(
                    GetUiText(opened ? "update.portable_instructions" : "update.open_failed"));
            }
        }
        else if (value.phase == UpdatePhase::Ready)
        {
            const bool launched = this->client->LaunchReady(
                // 系统启动仍归宿主，下载保护覆盖这个同步调用。
                // 入参：path 为受保护已校验文件。返回：成功启动时 true。
                [this](const std::filesystem::path& path)
                {
                    return !this->closing && !(this->options.stopping && this->options.stopping()) &&
                           this->options.launchInstaller &&
                           this->options.launchInstaller(path, this->renderer.NativeHandle());
                });
            (void)this->renderer.SetStatus(GetUiText(launched ? "update.installer_opened" : "update.open_failed"));
            if (!launched && !this->closing)
                (void)this->Message(GetUiText("update.open_failed"));
        }
        else if (value.phase == UpdatePhase::Current || value.phase == UpdatePhase::LocalAhead ||
                 value.phase == UpdatePhase::Failed)
        {
            (void)this->renderer.SetStatus(L"");
            (void)this->Message(GetUiText(value.phase == UpdatePhase::Current      ? "update.current"
                                          : value.phase == UpdatePhase::LocalAhead ? "update.local_ahead"
                                                                                   : ErrorText(value.error)));
        }
    }
    // 构造业务表单，原生窗口机制全部由公共 Renderer 执行。
    // 入参：无。返回：正常关闭为 true。
    bool Show()
    {
        const DWORD thread = GetCurrentThreadId();
        const std::uint64_t token = this->serial;
        this->client = std::make_unique<UpdateClient>(
            this->options.cacheRoot,
            // 跨线程仅投递身份，不借用当前对话框对象。
            // 入参：无。返回：无，通知不包含对象地址。
            [thread, token]() { (void)PostThreadMessageW(thread, UPDATE_WAKE, static_cast<WPARAM>(token), 0); });
        if (!this->renderer.LoadLayout(nlohmann::json::parse(R"({
          "schemaVersion":1,"window":{"titleKey":"about.title","initialSize":[560,320],"minSize":[420,280],"resizable":true},
          "content":{"type":"column","id":"body","padding":20,"gap":12,"children":[
            {"type":"text","id":"version","textKey":"about.body"}]},
          "footer":{"leading":[{"type":"button","id":"check","textKey":"update.check"},
            {"type":"button","id":"cancelUpdate","textKey":"update.cancel_download"}],
            "trailing":[{"type":"button","id":"close","textKey":"dialog.ok"}]}})")))
            return false;
        if (!this->renderer.SetTextResolver(
                // 关于正文保留当前编译版本，其余文本直接复用本地化。
                // 入参：key 为布局键。返回：产品文字。
                [this](std::string_view key)
                {
                    const std::wstring version(this->options.version.begin(), this->options.version.end());
                    return GetUiText(key, {{L"version", version}});
                }))
            return false;
        if (!this->renderer.BindAction(
                "check",
                // 窗口回调仅投递意图，确认窗在当前回调返回后的消息边界打开。
                // 入参：无。返回：无。
                [thread, token]() { (void)PostThreadMessageW(thread, UPDATE_WAKE, static_cast<WPARAM>(token), 1); }))
            return false;
        if (!this->renderer.BindAction("cancelUpdate",
                                       // 用户取消使请求立即失效，迟到响应不得重新启动安装器。
                                       // 入参：无。返回：无。
                                       [this]()
                                       {
                                           this->client->Cancel();
                                           this->Refresh();
                                       }) ||
            !this->renderer.BindAction("close",
                                       // 关闭前先撤销任务所有权。
                                       // 入参：无。返回：无。
                                       [this]() { this->Close(); }) ||
            !this->renderer.SetCloseHandler(
                // 系统关闭与按钮使用同一取消路径。
                // 入参：无。返回：无。
                [this]() { this->Close(); }) ||
            !this->renderer.SetDefaultAction("close"))
            return false;
        (void)this->renderer.SetEnabled("cancelUpdate", false);
        if (this->options.activeRenderer)
            *this->options.activeRenderer = &this->renderer;
        RendererWindowOptions window;
        window.owner = this->options.owner;
        window.icon = this->options.icon;
        const RendererResult result = this->renderer.ShowModal(
            window,
            // 消费专属唤醒，其余消息保留宿主的现有模态协调。
            // 入参：input 为当前线程消息。返回：本对话框已消费时 true。
            [this](MSG& input)
            {
                if (input.message == UPDATE_WAKE && input.wParam == this->serial)
                {
                    if (input.lParam == 1 && !this->closing)
                    {
                        if (this->options.distribution != "installed" && this->options.distribution != "portable")
                            (void)this->Message(GetUiText("update.configuration_error"));
                        else if (!this->client->Check(this->options.version, this->options.distribution == "installed"
                                                                                 ? UpdateDistribution::Installed
                                                                                 : UpdateDistribution::Portable))
                        {
                            const UpdatePhase phase = this->client->Snapshot().phase;
                            if (phase != UpdatePhase::Failed && phase != UpdatePhase::Checking &&
                                phase != UpdatePhase::Downloading)
                                (void)this->Message(GetUiText("update.operation_failed"));
                        }
                    }
                    this->Refresh();
                    return true;
                }
                if (this->options.stopping && this->options.stopping())
                    this->Close();
                return this->options.processThreadMessage && this->options.processThreadMessage(input);
            });
        this->client->Cancel();
        if (this->options.activeRenderer)
            *this->options.activeRenderer = nullptr;
        return static_cast<bool>(result);
    }
};
} // namespace
// 把业务异常限制在窗口适配边界，撤销外部 Renderer 借用。
// 入参：options 为宿主借用。返回：正常运行时 true。
bool ShowAboutWindow(const AboutWindowOptions& options) noexcept
try
{
    AboutDialog dialog{options};
    return dialog.Show();
}
catch (...)
{
    if (options.activeRenderer)
        *options.activeRenderer = nullptr;
    return false;
}
} // namespace open_st
