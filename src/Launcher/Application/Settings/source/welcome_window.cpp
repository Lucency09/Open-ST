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
    // 用内嵌布局创建欢迎窗口，取消路径不提交任何选择。
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
            this->renderer_->SetTextResolver([this](std::string_view key) { return this->callbacks_.text(key); }));
        this->Require(this->renderer_->BindBool(
            "startupEnabled",
            [this]() { return RendererBoolResult{true, this->edit_.ReadBool("startup.enabled").value_or(true), {}}; },
            [this](bool value)
            { return RendererChangeResult{!this->pending_ && this->edit_.ChangeBool("startup.enabled", value), {}}; }));
        this->Require(this->renderer_->BindAction("confirmButton", [this]() { this->Confirm(); }));
        this->Require(
            this->renderer_->BindAction("exitButton", [this]() { this->Require(this->renderer_->RequestClose()); }));
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
    bool Failed() const noexcept
    {
        return this->failed_;
    }

    // 异常路径同步关闭本次窗口，避免启动失败后留下孤立窗口。
    void Reset() noexcept
    {
        this->renderer_.reset();
        this->callbacks_ = {};
    }

    // 前台激活不创建第二个窗口。
    void Activate() noexcept
    {
        if (!this->renderer_ || !this->renderer_->NativeHandle())
            return;
        ShowWindow(this->renderer_->NativeHandle(), SW_RESTORE);
        SetForegroundWindow(this->renderer_->NativeHandle());
    }

    // 状态保存为键，重新本地化避免显示旧语言。
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
    // 统一检查 renderer 结构化结果。
    void Require(const RendererResult& result) const
    {
        if (!result)
            throw std::runtime_error("Welcome renderer operation failed");
    }

    // 保存成功后才操作系统，重试始终复核已保存目标。
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
WelcomeWindow::WelcomeWindow() : impl_(std::make_unique<Impl>()) {}
// 析构释放窗口。
WelcomeWindow::~WelcomeWindow() = default;
// 不让窗口或业务回调异常穿过应用启动边界。
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
void WelcomeWindow::Activate() noexcept
{
    this->impl_->Activate();
}
// 转发语言更新。
void WelcomeWindow::RefreshTexts() noexcept
{
    this->impl_->RefreshTexts();
}
// 查询本次模态执行的窗口故障状态。
bool WelcomeWindow::Failed() const noexcept
{
    return this->impl_->Failed();
}
} // namespace open_st
