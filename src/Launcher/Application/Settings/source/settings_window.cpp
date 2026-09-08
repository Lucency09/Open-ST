#include "settings_edit.h"
#include "settings_internal.h"
#include "settings_window_test_access.h"
#include <algorithm>
#include <log.h>
#include <optional>
#include <settings_window.h>
#include <stdexcept>
#include <utility>
#include <window_renderer.h>

namespace open_st
{
class SettingsWindow::Impl final
{
  public:
    // 解析布局、绑定草稿和动作；已有窗口只激活。
    bool Show(HINSTANCE instance, SettingsWindowCallbacks callbacks)
    {
        if (this->IsOpen())
        {
            ShowWindow(this->renderer_->NativeHandle(), SW_RESTORE);
            SetForegroundWindow(this->renderer_->NativeHandle());
            return true;
        }
        if (instance == nullptr || !callbacks.text || !callbacks.currentLanguage || !callbacks.availableLanguages ||
            !callbacks.languageApplied || !callbacks.startupApplied)
        {
            return false;
        }
        this->Close();
        this->callbacks_ = std::move(callbacks);
        nlohmann::json layout;
        if (!ReadSettingsLayout(layout))
        {
            return false;
        }
        this->renderer_ = std::make_unique<WindowRenderer>();
        this->Require(this->renderer_->LoadLayout(layout));
        this->ready_ = this->editSession_.Open({"ui.language"}, this->callbacks_.startupApplied
                                                                    ? std::vector<std::string>{"startup.enabled"}
                                                                    : std::vector<std::string>{});
        this->Require(this->renderer_->SetErrorHandler(
            [this](const RendererResult& result)
            {
                if (result.code == "value_unavailable" && !this->ready_)
                    return;
                const std::wstring message = this->Text(
                    result.code == "value_unavailable" ? "settings.language.unavailable" : "settings.operation_failed");
                if (result.id == "languageSelector")
                {
                    this->languageErrorKey_ = result.code == "value_unavailable" ? "settings.language.unavailable"
                                                                                 : "settings.operation_failed";
                    this->Require(this->renderer_->SetFieldError(result.id, message));
                }
                else
                    this->SetStatus(result.code == "value_unavailable" ? "settings.language.unavailable"
                                    : this->pendingStartup_            ? "settings.startup.apply_failed"
                                                                       : "settings.operation_failed");
            }));
        this->Require(this->renderer_->SetTextResolver([this](std::string_view key) { return this->Text(key); }));
        this->Require(this->renderer_->BindString(
            "languageSelector", [this]()
            { return RendererStringResult{true, this->editSession_.ReadString("ui.language").value_or(""), {}}; },
            [this](std::string_view value)
            {
                if (!this->ready_ || this->busy_ || !this->editSession_.ChangeString("ui.language", value))
                {
                    return RendererChangeResult{false, this->Text("settings.operation_failed")};
                }
                this->languageErrorKey_.clear();
                this->SetStatus({});
                this->UpdateButtons();
                return RendererChangeResult{};
            }));
        if (this->callbacks_.startupApplied)
        {
            this->Require(this->renderer_->BindBool(
                "startupEnabled", [this]()
                { return RendererBoolResult{true, this->editSession_.ReadBool("startup.enabled").value_or(true), {}}; },
                [this](bool value)
                {
                    if (!this->ready_ || this->busy_ || !this->editSession_.ChangeBool("startup.enabled", value))
                        return RendererChangeResult{false, this->Text("settings.operation_failed")};
                    this->SetStatus({});
                    this->UpdateButtons();
                    return RendererChangeResult{};
                }));
        }
        this->Require(this->renderer_->BindOptions("languageSelector", [this]() { return this->QueryLanguages(); }));
        this->Require(this->renderer_->BindAction(
            "startupRepairButton",
            [this]()
            {
                if (this->busy_ || !this->ready_)
                    return;
                this->RunBusy(
                    [this]()
                    {
                        const std::optional<bool> target = this->editSession_.ReadBool("startup.enabled");
                        if (!target || this->editSession_.IsDirty("startup.enabled"))
                        {
                            this->SetStatus("settings.startup.save_first");
                            return;
                        }
                        const SettingsCommitResult verified =
                            this->editSession_.VerifySavedBool("startup.enabled", *target);
                        if (verified != SettingsCommitResult::Unchanged)
                        {
                            this->ReportCommitFailure(verified);
                            return;
                        }
                        this->pendingStartup_ = target;
                        const bool applied = this->callbacks_.startupApplied(*target);
                        if (applied)
                            this->pendingStartup_.reset();
                        this->RefreshTexts();
                        this->SetStatus(applied ? "settings.saved" : "settings.startup.apply_failed");
                    });
            }));
        this->Require(this->renderer_->BindAction("applyButton", [this]() { this->Apply(false); }));
        this->Require(this->renderer_->BindAction("acceptButton", [this]() { this->Apply(true); }));
        this->Require(this->renderer_->BindAction("cancelButton", [this]() { this->Cancel(); }));
        this->Require(this->renderer_->BindAction("restoreDefaultsButton", [this]() { this->RestoreDefaults(); }));
        this->Require(this->renderer_->BindAction("reloadButton", [this]() { this->Reload(); }));
        this->Require(this->renderer_->SetCloseHandler([this]() { this->Cancel(); }));
        this->Require(this->renderer_->SetDefaultAction("acceptButton"));
        this->Require(this->renderer_->ValidateBindings());
        RendererWindowOptions options;
        options.icon = this->callbacks_.largeIcon;
        this->Require(this->renderer_->Show(options));
        if (this->callbacks_.smallIcon != nullptr)
        {
            SendMessageW(this->renderer_->NativeHandle(), WM_SETICON, ICON_SMALL,
                         reinterpret_cast<LPARAM>(this->callbacks_.smallIcon));
        }
        this->SetStatus(this->ready_ ? "" : "settings.edit_load_failed");
        this->UpdateButtons();
        return true;
    }

    // 退出时先释放窗口和捕获回调，再释放编辑状态。
    void Close() noexcept
    {
        if (this->busy_)
        {
            this->closeAfterBusy_ = true;
            return;
        }
        this->renderer_.reset();
        this->editSession_ = {};
        this->callbacks_ = {};
        this->pendingLanguage_.reset();
        this->pendingStartup_.reset();
        this->statusKey_.clear();
        this->languageErrorKey_.clear();
        this->ready_ = false;
        this->busy_ = false;
    }

    // 只有实际窗口存在时才报告打开。
    bool IsOpen() const noexcept
    {
        return this->renderer_ != nullptr && this->renderer_->NativeHandle() != nullptr;
    }

    // 转发非模态键盘处理，不传播异常。
    bool ProcessDialogMessage(MSG& message) const noexcept
    {
        try
        {
            return this->renderer_ != nullptr && this->renderer_->ProcessDialogMessage(message);
        }
        catch (...)
        {
            OPEN_ST_LOG_ERROR("Settings keyboard processing failed.");
            return false;
        }
    }

    // 私有测试入口返回借用窗口。
    HWND NativeHandle() const noexcept
    {
        return this->renderer_ != nullptr ? this->renderer_->NativeHandle() : nullptr;
    }

    // 测试替换确认边界，避免自动测试阻塞。
    void SetConfirmation(std::function<bool()> confirmation)
    {
        this->confirmation_ = std::move(confirmation);
    }

    // 本地化切换后重新解析业务状态键，不缓存旧译文。
    void RefreshTexts() noexcept
    {
        try
        {
            if (!this->IsOpen())
                return;
            this->Require(this->renderer_->RefreshTexts());
            this->Require(this->renderer_->SetStatus(this->statusKey_.empty() ? L"" : this->Text(this->statusKey_)));
            this->Require(this->renderer_->SetFieldError(
                "languageSelector", this->languageErrorKey_.empty() ? L"" : this->Text(this->languageErrorKey_)));
        }
        catch (...)
        {
            OPEN_ST_LOG_ERROR("Settings text refresh failed.");
        }
    }

  private:
    // 记录结构化定位，不记录字段值。
    void Require(const RendererResult& result) const
    {
        if (!result)
        {
            OPEN_ST_LOG_ERROR("Settings renderer failed. code=", result.code, " path=", result.path, " id=", result.id);
            throw std::runtime_error("Settings renderer failure");
        }
    }

    // 所有文字通过上级本地化回调取得。
    std::wstring Text(std::string_view key) const
    {
        if (key == "settings.startup.status" && this->callbacks_.startupStatus)
            return this->callbacks_.startupStatus();
        return this->callbacks_.text(key);
    }

    // 状态由宿主提供译文，Renderer 不理解业务。
    void SetStatus(std::string_view key)
    {
        this->statusKey_ = key;
        this->Require(this->renderer_->SetStatus(key.empty() ? L"" : this->Text(key)));
    }

    // 草稿与待生效状态共同决定应用按钮是否可用。
    void UpdateButtons()
    {
        this->Require(this->renderer_->SetEnabled("languageSelector", this->ready_));
        if (this->callbacks_.startupApplied)
            this->Require(this->renderer_->SetEnabled("startupEnabled", this->ready_));
        this->Require(this->renderer_->SetEnabled("applyButton", this->ready_ && (this->editSession_.IsDirty() ||
                                                                                  this->pendingLanguage_.has_value() ||
                                                                                  this->pendingStartup_.has_value())));
        this->Require(this->renderer_->SetEnabled("startupRepairButton", this->ready_));
        this->Require(this->renderer_->SetEnabled("acceptButton", this->ready_));
        this->Require(this->renderer_->SetEnabled("restoreDefaultsButton", this->ready_));
    }

    // 动态查询语言，稳定代码与显示文字分开存储。
    RendererOptionsResult QueryLanguages() const
    {
        try
        {
            RendererOptionsResult result;
            const std::vector<std::string> languages = this->callbacks_.availableLanguages();
            for (const std::string& language : languages)
            {
                result.options.push_back({language, std::wstring(language.begin(), language.end())});
            }
            if (languages.empty())
            {
                result.error = this->Text("settings.language.query_failed");
            }
            return result;
        }
        catch (...)
        {
            return {false, {}, this->Text("settings.language.query_failed")};
        }
    }

    // 提交前复核动态选项，不自动替换无效选择。
    bool ValidateLanguage(std::string_view language)
    {
        const RendererOptionsResult options = this->QueryLanguages();
        const bool available = options.success && std::any_of(options.options.begin(), options.options.end(),
                                                              [language](const RendererOption& option)
                                                              { return option.value == language; });
        this->languageErrorKey_ = available ? "" : "settings.language.unavailable";
        this->Require(this->renderer_->SetFieldError("languageSelector",
                                                     available ? L"" : this->Text("settings.language.unavailable")));
        if (!available)
        {
            this->SetStatus(options.success && !options.options.empty() ? "settings.language.unavailable"
                                                                        : "settings.language.query_failed");
        }
        return available;
    }

    // 冲突和存储失败分别提示，不清除用户草稿。
    void ReportCommitFailure(SettingsCommitResult result)
    {
        this->SetStatus(result == SettingsCommitResult::Conflict ? "settings.conflict" : "settings.save_failed");
    }

    // 确定与应用共用保存入口，保存后才尝试运行时语言切换。
    void Apply(bool closeWhenDone)
    {
        if (this->busy_ || !this->ready_)
        {
            return;
        }
        if (!this->editSession_.IsDirty() && !this->pendingLanguage_.has_value() && !this->pendingStartup_.has_value())
        {
            if (closeWhenDone)
            {
                this->Require(this->renderer_->RequestClose());
            }
            return;
        }
        this->RunBusy(
            [this, closeWhenDone]()
            {
                const std::optional<std::string> language = this->editSession_.ReadString("ui.language");
                if (!language.has_value() || !this->ValidateLanguage(*language))
                {
                    return;
                }
                const bool languageChanged = this->editSession_.IsDirty("ui.language");
                const bool startupChanged = this->editSession_.IsDirty("startup.enabled");
                const std::optional<bool> startup = this->editSession_.ReadBool("startup.enabled");
                if (this->editSession_.IsDirty())
                {
                    std::optional<std::string> pendingLanguage = languageChanged ? language : this->pendingLanguage_;
                    std::optional<bool> pendingStartup = startupChanged ? startup : this->pendingStartup_;
                    const SettingsCommitResult committed = this->editSession_.Commit();
                    if (committed != SettingsCommitResult::Saved && committed != SettingsCommitResult::Unchanged)
                    {
                        this->ReportCommitFailure(committed);
                        return;
                    }
                    this->pendingLanguage_.swap(pendingLanguage);
                    this->pendingStartup_.swap(pendingStartup);
                }
                bool languageApplied = true;
                bool startupApplied = true;
                SettingsCommitResult verificationFailure = SettingsCommitResult::Unchanged;
                if (this->pendingLanguage_)
                {
                    const SettingsCommitResult verified =
                        this->editSession_.VerifySavedString("ui.language", *this->pendingLanguage_);
                    if (verified != SettingsCommitResult::Unchanged)
                    {
                        verificationFailure = verified;
                        languageApplied = false;
                    }
                    else
                    {
                        try
                        {
                            languageApplied = this->callbacks_.languageApplied(*this->pendingLanguage_);
                        }
                        catch (...)
                        {
                            languageApplied = false;
                        }
                    }
                    if (languageApplied)
                        this->pendingLanguage_.reset();
                }
                if (this->pendingStartup_)
                {
                    const SettingsCommitResult verified =
                        this->editSession_.VerifySavedBool("startup.enabled", *this->pendingStartup_);
                    if (verified != SettingsCommitResult::Unchanged)
                    {
                        verificationFailure = verified;
                        startupApplied = false;
                    }
                    else
                    {
                        try
                        {
                            startupApplied = this->callbacks_.startupApplied(*this->pendingStartup_);
                        }
                        catch (...)
                        {
                            startupApplied = false;
                        }
                    }
                    if (startupApplied)
                        this->pendingStartup_.reset();
                }
                this->Require(this->renderer_->RefreshTexts());
                if (verificationFailure != SettingsCommitResult::Unchanged)
                    this->ReportCommitFailure(verificationFailure);
                else
                    this->SetStatus(!languageApplied && !startupApplied ? "settings.effects_failed"
                                    : !languageApplied                  ? "settings.language.apply_failed"
                                    : !startupApplied                   ? "settings.startup.apply_failed"
                                                                        : "settings.saved");
                this->closeAfterBusy_ = this->closeAfterBusy_ || (languageApplied && startupApplied && closeWhenDone);
            });
    }

    // 确认使用真实本地化文本，测试可注入无模态替身。
    bool Confirm(std::string_view key) const
    {
        if (this->confirmation_)
        {
            return this->confirmation_();
        }
        const std::wstring message = this->Text(key);
        const std::wstring title = this->Text("settings.title");
        return MessageBoxW(this->renderer_->NativeHandle(), message.c_str(), title.c_str(),
                           MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
    }

    // 恢复当前页的已实现字段，仅修改草稿。
    void RestoreDefaults()
    {
        if (this->busy_ || !this->ready_ ||
            this->renderer_->GetActivePageId() != this->renderer_->GetControlPageId("languageSelector"))
        {
            return;
        }
        this->RunBusy(
            [this]()
            {
                if (!this->Confirm("settings.restore_confirm"))
                {
                    return;
                }
                SettingsEditSession candidate = this->editSession_;
                if (!candidate.RestoreDefaults({"ui.language", "startup.enabled"}))
                {
                    this->SetStatus("settings.defaults_failed");
                    return;
                }
                const std::optional<std::string> language = candidate.ReadString("ui.language");
                const RendererOptionsResult options = this->QueryLanguages();
                if (!language.has_value() || !options.success ||
                    !std::any_of(options.options.begin(), options.options.end(),
                                 [&language](const RendererOption& option) { return option.value == *language; }))
                {
                    this->SetStatus("settings.defaults_failed");
                    return;
                }
                this->editSession_ = std::move(candidate);
                this->Require(this->renderer_->RefreshValues());
                this->SetStatus({});
            });
    }

    // 提供可操作的读取重试和冲突重载；不撤销已保存内容。
    void Reload()
    {
        if (this->busy_)
        {
            return;
        }
        this->RunBusy(
            [this]()
            {
                if (this->editSession_.IsDirty() && !this->Confirm("settings.reload_confirm"))
                {
                    return;
                }
                this->ready_ = this->editSession_.Open({"ui.language"}, {"startup.enabled"});
                if (!this->ready_)
                {
                    this->SetStatus("settings.edit_load_failed");
                    return;
                }
                if (this->pendingLanguage_.has_value() &&
                    this->editSession_.ReadString("ui.language") != this->pendingLanguage_)
                {
                    this->pendingLanguage_.reset();
                }
                if (this->pendingStartup_ && this->editSession_.ReadBool("startup.enabled") != this->pendingStartup_)
                    this->pendingStartup_.reset();
                this->Require(this->renderer_->RefreshValues());
                this->Require(this->renderer_->RefreshTexts());
                this->languageErrorKey_.clear();
                this->Require(this->renderer_->SetFieldError("languageSelector", {}));
                this->SetStatus(this->pendingLanguage_.has_value() ? "settings.language.apply_failed"
                                : this->pendingStartup_            ? "settings.startup.apply_failed"
                                                                   : "");
            });
    }

    // 通知只提供状态，不允许宿主异常打断交互恢复。
    void NotifyBusy(bool busy) noexcept
    {
        try
        {
            if (this->callbacks_.busyChanged)
                this->callbacks_.busyChanged(busy);
        }
        catch (...)
        {
            OPEN_ST_LOG_ERROR("Settings busy notification failed.");
        }
    }

    // 忙状态覆盖确认及应用回调，异常恢复交互，关闭延迟到当前分派结束。
    void RunBusy(const std::function<void()>& operation)
    {
        this->busy_ = true;
        this->closeAfterBusy_ = false;
        this->NotifyBusy(true);
        try
        {
            this->Require(this->renderer_->SetBusy(true));
            operation();
        }
        catch (...)
        {
            OPEN_ST_LOG_ERROR("Settings operation failed.");
            this->closeAfterBusy_ = false;
            try
            {
                this->SetStatus(this->pendingLanguage_.has_value() ? "settings.language.apply_failed"
                                : this->pendingStartup_            ? "settings.startup.apply_failed"
                                                                   : "settings.operation_failed");
            }
            catch (...)
            {
                OPEN_ST_LOG_ERROR("Settings error display failed.");
            }
        }
        this->busy_ = false;
        this->NotifyBusy(false);
        this->Require(this->renderer_->SetBusy(false));
        this->UpdateButtons();
        if (this->closeAfterBusy_)
        {
            this->Require(this->renderer_->RequestClose());
        }
    }

    // 取消只关闭，不回滚已保存结果。
    void Cancel()
    {
        if (!this->busy_)
        {
            this->Require(this->renderer_->RequestClose());
        }
    }

    SettingsEditSession editSession_;
    SettingsWindowCallbacks callbacks_;
    std::unique_ptr<WindowRenderer> renderer_;
    std::optional<std::string> pendingLanguage_;
    std::optional<bool> pendingStartup_;
    std::string statusKey_;
    std::string languageErrorKey_;
    std::function<bool()> confirmation_;
    bool ready_{};
    bool busy_{};
    bool closeAfterBusy_{};
};

// 创建编排对象，不立即读取布局或创建窗口。
SettingsWindow::SettingsWindow() : impl_(std::make_unique<Impl>()) {}

// 上级回调对象销毁前同步释放窗口。
SettingsWindow::~SettingsWindow()
{
    this->Close();
}

// 创建失败回收本次资源，不让异常穿过公开边界。
bool SettingsWindow::Show(HINSTANCE instance, SettingsWindowCallbacks callbacks) noexcept
{
    try
    {
        if (this->impl_->Show(instance, std::move(callbacks)))
        {
            return true;
        }
    }
    catch (...)
    {
        OPEN_ST_LOG_ERROR("Failed to open settings renderer.");
    }
    this->Close();
    return false;
}

// 转发应用的非模态消息导航。
bool SettingsWindow::ProcessDialogMessage(MSG& message) const noexcept
{
    return this->impl_->ProcessDialogMessage(message);
}

// 同步关闭供应用退出使用；控件动作通过内部延迟关闭。
void SettingsWindow::Close() noexcept
{
    this->impl_->Close();
}

// 查询实际窗口存活状态。
bool SettingsWindow::IsOpen() const noexcept
{
    return this->impl_->IsOpen();
}

// 为测试返回借用窗口，避免全局搜索。
HWND SettingsWindowTestAccess::NativeHandle(const SettingsWindow& window) noexcept
{
    return window.impl_->NativeHandle();
}

// 注入确认边界以免测试阻塞在模态提示。
void SettingsWindowTestAccess::SetConfirmation(SettingsWindow& window, std::function<bool()> confirmation)
{
    window.impl_->SetConfirmation(std::move(confirmation));
}
// 由上级在语言资源变化后刷新已打开的设置窗口。
void SettingsWindow::RefreshTexts() noexcept
{
    this->impl_->RefreshTexts();
}
// 宿主借用句柄使系统模态窗口正确禁用其设置 owner。
HWND SettingsWindow::NativeHandle() const noexcept
{
    return this->impl_->NativeHandle();
}
} // namespace open_st
