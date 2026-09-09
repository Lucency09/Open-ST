// 创建首次启动欢迎表单，处理确认保存、启动项应用及失败重试。

#include "settings_edit.h"
#include <log.h>
#include <stdexcept>
#include <utility>
#include <welcome_window.h>
#include <window_renderer.h>

namespace open_st
{
class WelcomeWindow::Impl final
{
  public:
    // 创建首次启动欢迎表单并运行模态循环，处理确认保存、自启应用和失败重试。
    // 入参：instance 为程序模块句柄；callbacks 为宿主回调集合；hook 为可选线程消息处理器，返回 true 表示已消费。
    // 返回：欢迎确认已持久化且系统自启应用成功时为 true；用户取消或窗口失败为 false，Failed 可区分窗口故障。
    bool ShowModal(HINSTANCE instance, SettingsWindowCallbacks callbacks, std::function<bool(MSG&)> hook)
    {
        this->failed_ = true;
        if (instance == nullptr || !callbacks.text || !callbacks.startupApplied)
            return false;
        this->callbacks_ = std::move(callbacks);
        this->completed_ = false;
        this->pending_ = false;
        this->statusKey_.clear();
        if (!this->edit_.Open({}, {"startup.enabled", "onboarding.completed"}))
            return false;
        this->renderer_ = std::make_unique<WindowRenderer>();
        const nlohmann::json layout = {
            {"schemaVersion", 1},
            {"window",
             {{"titleKey", "welcome.title"},
              {"initialSize", {640, 480}},
              {"minSize", {520, 400}},
              {"resizable", true}}},
            {"content",
             {{"type", "column"},
              {"id", "welcomeContent"},
              {"padding", 20},
              {"gap", 16},
              {"children",
               nlohmann::json::array(
                   {{{"type", "text"}, {"id", "welcomeIntro"}, {"textKey", "welcome.intro"}},
                    {{"type", "text"}, {"id", "welcomePrivacy"}, {"textKey", "welcome.privacy"}},
                    {{"type", "checkbox"}, {"id", "startupEnabled"}, {"labelKey", "settings.startup.label"}}})}}},
            {"footer",
             {{"trailing",
               nlohmann::json::array({{{"type", "button"}, {"id", "confirmButton"}, {"textKey", "welcome.confirm"}},
                                      {{"type", "button"}, {"id", "exitButton"}, {"textKey", "welcome.exit"}}})}}}};
        this->Require(this->renderer_->LoadLayout(layout));
        this->Require(
            // 将欢迎布局的文本键交给宿主本地化查询。
            // 入参：key 为欢迎布局请求的动态本地化文本键。
            // 返回：宿主当前语言对应的界面宽字符串。
            this->renderer_->SetTextResolver([this](std::string_view key) { return this->callbacks_.text(key); }));
        this->Require(this->renderer_->BindBool(
            "startupEnabled",
            // 读取欢迎表单的启动项草稿，缺失时默认勾选。
            // 入参：无显式入参。
            // 返回：成功的布尔读取结果；值为 startup.enabled 草稿，缺失时默认 true。
            [this]() { return RendererBoolResult{true, this->edit_.ReadBool("startup.enabled").value_or(true), {}}; },
            // 仅在首次保存前接受启动项草稿变更，避免重试时改变已保存目标。
            // 入参：value 为用户在欢迎表单勾选的自启启用状态。
            // 返回：尚未保存且草稿更新成功时接受修改；已进入待重试状态或更新失败时拒绝。
            [this](bool value)
            { return RendererChangeResult{!this->pending_ && this->edit_.ChangeBool("startup.enabled", value), {}}; }));
        // 确认并保存欢迎选择，保存成功后请求宿主应用启动项。
        // 入参：无显式入参。
        // 返回：无返回值。
        this->Require(this->renderer_->BindAction("confirmButton", [this]() { this->Confirm(); }));
        this->Require(
            // 请求延迟关闭欢迎窗口，不提交本次选择。
            // 入参：无显式入参。
            // 返回：无返回值。
            this->renderer_->BindAction("exitButton", [this]() { this->Require(this->renderer_->RequestClose()); }));
        // 将系统关闭动作转为延迟关闭，避免销毁正在执行的回调。
        // 入参：无显式入参。
        // 返回：无返回值。
        this->Require(this->renderer_->SetCloseHandler([this]() { this->Require(this->renderer_->RequestClose()); }));
        this->Require(this->renderer_->SetDefaultAction("confirmButton"));
        RendererWindowOptions options;
        options.icon = this->callbacks_.largeIcon;
        const RendererResult shown = this->renderer_->ShowModal(options, std::move(hook));
        this->failed_ = !shown;
        this->renderer_.reset();
        this->callbacks_ = {};
        return shown && this->completed_;
    }

    // 返回本次启动是否发生非用户取消的窗口故障。
    // 入参：无显式入参。
    // 返回：本次窗口初始化或模态循环失败为 true；正常执行或用户取消为 false。
    bool Failed() const noexcept
    {
        return this->failed_;
    }

    // 异常路径同步关闭本次窗口，避免启动失败后留下孤立窗口。
    // 入参：无显式入参。
    // 返回：无返回值。
    void Reset() noexcept
    {
        this->renderer_.reset();
        this->callbacks_ = {};
    }

    // 前台激活不创建第二个窗口。
    // 入参：无显式入参。
    // 返回：无返回值。
    void Activate() noexcept
    {
        if (!this->renderer_ || !this->renderer_->NativeHandle())
            return;
        ShowWindow(this->renderer_->NativeHandle(), SW_RESTORE);
        SetForegroundWindow(this->renderer_->NativeHandle());
    }

    // 状态保存为键，重新本地化避免显示旧语言。
    // 入参：无显式入参。
    // 返回：无返回值。
    void RefreshTexts() noexcept
    {
        try
        {
            if (!this->renderer_)
                return;
            this->Require(this->renderer_->RefreshTexts());
            this->Require(
                this->renderer_->SetStatus(this->statusKey_.empty() ? L"" : this->callbacks_.text(this->statusKey_)));
        }
        catch (...)
        {
            OPEN_ST_LOG_ERROR("Welcome text refresh failed.");
        }
    }

  private:
    // 检查欢迎表单的布局、绑定及显示操作是否成功。
    // 入参：result 为欢迎窗口要求成功的渲染器操作结果。
    // 返回：无返回值；失败时抛出异常，交由欢迎窗口的外层边界处理。
    void Require(const RendererResult& result) const
    {
        if (!result)
            throw std::runtime_error("Welcome renderer operation failed");
    }

    // 持久化欢迎完成标记和自启意图，再核验磁盘目标并应用系统启动项；失败保留重试状态。
    // 入参：无显式入参。
    // 返回：无返回值。
    void Confirm()
    {
        this->Require(this->renderer_->SetBusy(true));
        try
        {
            const bool target = this->edit_.ReadBool("startup.enabled").value_or(true);
            SettingsCommitResult saved = SettingsCommitResult::Unchanged;
            if (!this->pending_)
            {
                if (!this->edit_.ChangeBool("onboarding.completed", true))
                    throw std::runtime_error("Welcome edit failed");
                saved = this->edit_.Commit({"startup.enabled", "onboarding.completed"});
                if (saved == SettingsCommitResult::Saved || saved == SettingsCommitResult::Unchanged)
                    this->pending_ = true;
            }
            if (saved != SettingsCommitResult::Saved && saved != SettingsCommitResult::Unchanged)
                this->statusKey_ =
                    saved == SettingsCommitResult::Conflict ? "settings.conflict" : "settings.save_failed";
            else if (this->edit_.VerifySavedBool("startup.enabled", target) != SettingsCommitResult::Unchanged ||
                     this->edit_.VerifySavedBool("onboarding.completed", true) != SettingsCommitResult::Unchanged)
                this->statusKey_ = "settings.conflict";
            else if (!this->callbacks_.startupApplied(target))
                this->statusKey_ = "settings.startup.apply_failed";
            else
                this->completed_ = true;
        }
        catch (...)
        {
            this->statusKey_ = this->pending_ ? "settings.startup.apply_failed" : "settings.operation_failed";
        }
        this->Require(this->renderer_->SetBusy(false));
        this->Require(this->renderer_->SetEnabled("startupEnabled", !this->pending_));
        this->RefreshTexts();
        if (this->completed_)
            this->Require(this->renderer_->RequestClose());
    }

    SettingsWindowCallbacks callbacks_;
    SettingsEditSession edit_;
    std::unique_ptr<WindowRenderer> renderer_;
    std::string statusKey_;
    bool failed_{};
    bool completed_{};
    bool pending_{};
};

// 初始化私有编排对象。
// 入参：无显式入参。
// 返回：无返回值。
WelcomeWindow::WelcomeWindow() : impl_(std::make_unique<Impl>()) {}
// 析构释放窗口。
// 入参：无显式入参。
// 返回：无返回值。
WelcomeWindow::~WelcomeWindow() = default;
// 创建首次启动欢迎表单并运行模态循环，处理确认保存、自启应用和失败重试。
// 入参：instance 为程序模块句柄；callbacks 为宿主回调集合；hook 为可选线程消息处理器，返回 true 表示已消费。
// 返回：欢迎确认已持久化且系统自启应用成功时为 true；用户取消或窗口失败为 false，Failed 可区分窗口故障。
bool WelcomeWindow::ShowModal(HINSTANCE instance, SettingsWindowCallbacks callbacks,
                              std::function<bool(MSG&)> hook) noexcept
{
    try
    {
        return this->impl_->ShowModal(instance, std::move(callbacks), std::move(hook));
    }
    catch (...)
    {
        OPEN_ST_LOG_ERROR("Welcome window failed.");
        this->impl_->Reset();
        return false;
    }
}
// 转发激活请求。
// 入参：无显式入参。
// 返回：无返回值。
void WelcomeWindow::Activate() noexcept
{
    this->impl_->Activate();
}
// 转发语言更新。
// 入参：无显式入参。
// 返回：无返回值。
void WelcomeWindow::RefreshTexts() noexcept
{
    this->impl_->RefreshTexts();
}
// 查询本次模态执行的窗口故障状态。
// 入参：无显式入参。
// 返回：本次窗口初始化或模态循环失败为 true；正常执行或用户取消为 false。
bool WelcomeWindow::Failed() const noexcept
{
    return this->impl_->Failed();
}
} // namespace open_st
