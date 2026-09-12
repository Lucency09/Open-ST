// 编排非模态设置窗口的控件绑定、草稿提交、运行时应用和延迟关闭。

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
    // 在 UI 线程创建并显示设置窗口；窗口已存在时激活原窗口。
    // 入参：instance 为当前程序模块句柄；callbacks 为移交给窗口的宿主回调集合，捕获对象须活到窗口释放回调。
    // 返回：已有窗口激活或新建显示成功为 true；参数、布局、绑定或创建失败为 false。
    bool Show(HINSTANCE instance, SettingsWindowCallbacks callbacks)
    {
        if (this->IsOpen())
        {
            ShowWindow(this->renderer_->NativeHandle(), SW_RESTORE);
            SetForegroundWindow(this->renderer_->NativeHandle());
            return true;
        }
        if (instance == nullptr || !callbacks.text || !callbacks.currentLanguage || !callbacks.availableLanguages ||
            !callbacks.languageApplied || !callbacks.startupApplied || !callbacks.hotkeyDecode ||
            !callbacks.hotkeyEncode || !callbacks.hotkeyFormat || !callbacks.hotkeyPrepare || !callbacks.hotkeyFinish ||
            !callbacks.hotkeyStatus || !callbacks.hotkeyRecording)
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
        this->ready_ = this->editSession_.Open({"ui.language", "capture.hotkey"}, {"startup.enabled"});
        this->Require(this->renderer_->SetErrorHandler(
            // 将渲染器错误转为本地化字段或状态提示，未加载草稿时忽略选项缺失。
            // 入参：result 为渲染器报告的结构化错误，包含错误码及目标控件标识。
            // 返回：无返回值。
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
        // 按动态文本键查询当前界面语言，并允许启动项状态文字由宿主提供。
        // 入参：key 为宿主提供的动态界面文本键。
        // 返回：宿主当前语言的界面文本；启动项状态键由宿主状态回调提供。
        this->Require(this->renderer_->SetTextResolver([this](std::string_view key) { return this->Text(key); }));
        this->Require(this->renderer_->BindKeyChord(
            "captureHotkey",
            // 从草稿解析组合，不将非法配置改写为默认值。
            // 入参：无。
            // 返回：合法数值组合或本地化字段错误。
            [this]()
            {
                const std::optional<std::string> value = this->ReadHotkey();
                const std::optional<SettingsHotkeyChord> chord =
                    value ? this->callbacks_.hotkeyDecode(*value) : std::nullopt;
                this->hotkeyErrorKey_ = chord ? "" : "settings.hotkey.invalid";
                return chord ? RendererKeyChordResult{true, {chord->modifiers, chord->key}, {}}
                             : RendererKeyChordResult{false, {}, this->Text("settings.hotkey.invalid")};
            },
            // 将通用录入结果转换为业务 token，再更新草稿。
            // 入参：chord 为输入控件的数值组合。
            // 返回：合法且可编辑时接受，否则返回字段错误。
            [this](RendererKeyChord chord)
            {
                const std::optional<std::string> value = this->callbacks_.hotkeyEncode({chord.modifiers, chord.key});
                if (!value || !this->ChangeHotkey(*value))
                {
                    this->hotkeyErrorKey_ = "settings.hotkey.invalid";
                    return RendererChangeResult{false, this->Text(this->hotkeyErrorKey_)};
                }
                return RendererChangeResult{};
            },
            // 按宿主提供的显示规则绘制录入预览。
            // 入参：chord 为完整或未完成的数值组合。
            // 返回：当前语言的组合显示文本。
            [this](RendererKeyChord chord) { return this->callbacks_.hotkeyFormat({chord.modifiers, chord.key}); },
            // 撤销本次录入时恢复原始草稿，包括原先尚未修复的非法 token。
            // 入参：无。
            // 返回：恢复成功的接受结果；失败交由渲染器异常边界报告。
            [this]()
            {
                if (this->recordingOriginal_ &&
                    !this->editSession_.ChangeString("capture.hotkey", *this->recordingOriginal_))
                    throw std::runtime_error("Cannot restore hotkey draft");
                const std::optional<std::string> value = this->ReadHotkey();
                this->hotkeyErrorKey_ = value && this->callbacks_.hotkeyDecode(*value) ? "" : "settings.hotkey.invalid";
                if (this->renderer_)
                {
                    this->UpdateButtons();
                    this->RefreshTexts();
                }
                return RendererChangeResult{};
            }));
        // 通知宿主录入生命周期以暂停全局截图触发。
        // 入参：recording 为当前是否录入。
        // 返回：无。
        this->Require(this->renderer_->SetKeyRecordingHandler(
            [this](bool recording)
            {
                if (recording)
                    this->recordingOriginal_ = this->ReadHotkey();
                this->callbacks_.hotkeyRecording(recording);
            }));
        this->Require(this->renderer_->BindString(
            "languageSelector",
            // 从编辑草稿读取当前语言，缺失时向下拉框提供空选项值。
            // 入参：无显式入参。
            // 返回：成功的字符串读取结果；值为 ui.language 草稿，缺失时为空字符串。
            [this]()
            { return RendererStringResult{true, this->editSession_.ReadString("ui.language").value_or(""), {}}; },
            // 仅在草稿可用且空闲时接受语言选择，随后清除旧错误并刷新提交按钮。
            // 入参：value 为下拉框请求采用的新语言代码。
            // 返回：草稿可用、空闲且修改成功时返回接受结果；否则返回拒绝结果及本地化错误。
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
                "startupEnabled",
                // 读取启动项布尔草稿，缺失时保留默认勾选状态。
                // 入参：无显式入参。
                // 返回：成功的布尔读取结果；值为 startup.enabled 草稿，缺失时为 true。
                [this]()
                { return RendererBoolResult{true, this->editSession_.ReadBool("startup.enabled").value_or(true), {}}; },
                // 更新启动项草稿并刷新状态，不在勾选时修改系统注册表。
                // 入参：value 为复选框请求采用的自启启用状态。
                // 返回：草稿可用、空闲且修改成功时返回接受结果；否则返回拒绝结果及本地化错误。
                [this](bool value)
                {
                    if (!this->ready_ || this->busy_ || !this->editSession_.ChangeBool("startup.enabled", value))
                        return RendererChangeResult{false, this->Text("settings.operation_failed")};
                    this->SetStatus({});
                    this->UpdateButtons();
                    return RendererChangeResult{};
                }));
        }
        // 每次展开语言列表时重新查询可用语言，避免缓存已移除的选项。
        // 入参：无显式入参。
        // 返回：包含当前可用语言代码及显示文字的选项结果；查询失败时带本地化错误。
        this->Require(this->renderer_->BindOptions("languageSelector", [this]() { return this->QueryLanguages(); }));
        this->Require(this->renderer_->BindAction(
            "startupRepairButton",
            // 将启动项修复请求纳入忙状态保护，防止重复提交。
            // 入参：无显式入参。
            // 返回：无返回值。
            [this]()
            {
                if (this->busy_ || !this->ready_)
                    return;
                this->RunBusy(
                    // 复核已保存的启动项值后才请求系统应用，失败保留待重试标记。
                    // 入参：无显式入参。
                    // 返回：无返回值。
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
        // 提交修改并保留设置窗口，供用户继续调整。
        // 入参：无显式入参。
        // 返回：无返回值。
        this->Require(this->renderer_->BindAction("applyButton", [this]() { this->Apply(false); }));
        // 提交修改，成功应用所有副作用后请求关闭。
        // 入参：无显式入参。
        // 返回：无返回值。
        this->Require(this->renderer_->BindAction("acceptButton", [this]() { this->Apply(true); }));
        // 关闭设置窗口并丢弃未提交草稿，保留此前已保存内容。
        // 入参：无显式入参。
        // 返回：无返回值。
        this->Require(this->renderer_->BindAction("cancelButton", [this]() { this->Cancel(); }));
        // 进入默认值恢复流程，恢复只修改草稿。
        // 入参：无显式入参。
        // 返回：无返回值。
        this->Require(this->renderer_->BindAction("restoreDefaultsButton", [this]() { this->RestoreDefaults(); }));
        // 确认后重新加载已保存设置，放弃当前未提交修改。
        // 入参：无显式入参。
        // 返回：无返回值。
        this->Require(this->renderer_->BindAction("reloadButton", [this]() { this->Reload(); }));
        // 将关闭按钮等同于取消操作，忙状态期间不关闭。
        // 入参：无显式入参。
        // 返回：无返回值。
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

    // 空闲时先释放渲染器再清空草稿与回调；忙时仅记录延迟关闭请求。
    // 入参：无显式入参。
    // 返回：无返回值。
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
        this->hotkeyErrorKey_.clear();
        this->hotkeyCleanupPending_ = false;
        this->recordingOriginal_.reset();
        this->ready_ = false;
        this->busy_ = false;
    }

    // 只有实际窗口存在时才报告打开。
    // 入参：无显式入参。
    // 返回：实际设置窗口存在时为 true，否则为 false。
    bool IsOpen() const noexcept
    {
        return this->renderer_ != nullptr && this->renderer_->NativeHandle() != nullptr;
    }

    // 转发非模态键盘处理，不传播异常。
    // 入参：message 为应用消息循环取得的待处理消息。
    // 返回：消息已被窗口键盘导航处理为 true，否则为 false。
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

    // 取得当前设置窗口句柄，供宿主设置模态窗口的 owner。
    // 入参：无显式入参。
    // 返回：借用的当前设置窗口 HWND；未打开时为 nullptr，关闭后失效，不得由调用方销毁。
    HWND NativeHandle() const noexcept
    {
        return this->renderer_ != nullptr ? this->renderer_->NativeHandle() : nullptr;
    }

    // 测试替换确认边界，避免自动测试阻塞。
    // 入参：confirmation 为替换确认对话框的测试回调，返回 true 接受、false 取消；捕获对象须保持有效。
    // 返回：无返回值。
    void SetConfirmation(std::function<bool()> confirmation)
    {
        this->confirmation_ = std::move(confirmation);
    }

    // 本地化切换后重新解析业务状态键，不缓存旧译文。
    // 入参：无显式入参。
    // 返回：无返回值。
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
            this->Require(this->renderer_->SetFieldError(
                "captureHotkey", this->hotkeyErrorKey_.empty() ? L"" : this->Text(this->hotkeyErrorKey_)));
        }
        catch (...)
        {
            OPEN_ST_LOG_ERROR("Settings text refresh failed.");
        }
    }

    // 校验规范组合并更新草稿，不注册或写盘。
    // 入参：value 为宿主生成的组合 token。
    // 返回：验证和草稿更新成功时为 true。
    bool ChangeHotkey(std::string_view value)
    {
        if (!this->ready_ || this->busy_ || !this->callbacks_.hotkeyDecode(value) ||
            !this->editSession_.ChangeString("capture.hotkey", value))
            return false;
        this->hotkeyErrorKey_.clear();
        this->Require(this->renderer_->SetFieldError("captureHotkey", {}));
        this->SetStatus({});
        this->UpdateButtons();
        return true;
    }

    // 返回快捷键草稿副本，供状态与测试检查。
    // 入参：无。
    // 返回：可用的快捷键草稿，否则为空。
    std::optional<std::string> ReadHotkey() const
    {
        return this->editSession_.ReadString("capture.hotkey");
    }

    // 转发宿主匹配过身份和时间的全局组合通知。
    // 入参：modifiers、key 为组合；time 为原消息时间戳。
    // 返回：录入控件接受通知时为 true。
    bool ProcessRecordedHotkey(UINT modifiers, UINT key, DWORD time) noexcept
    {
        try
        {
            return this->renderer_ && this->renderer_->ProcessRecordedHotkey(modifiers, key, time);
        }
        catch (...)
        {
            return false;
        }
    }

  private:
    // 判断当前草稿是否仍需注册或清理，宿主未提供查询时仅消费本地清理状态。
    // 入参：无。
    // 返回：点击应用还需处理快捷键副作用时为 true。
    bool HotkeyNeedsApply() const
    {
        const std::optional<std::string> value = this->ReadHotkey();
        return this->hotkeyCleanupPending_ ||
               (value && this->callbacks_.hotkeyNeedsApply && this->callbacks_.hotkeyNeedsApply(*value));
    }
    // 验证提交目标并保存可重新本地化的字段错误。
    // 入参：value 为待提交的快捷键 token。
    // 返回：组合有效时为 true。
    bool ValidateHotkey(std::string_view value)
    {
        const bool valid = this->callbacks_.hotkeyDecode(value).has_value();
        this->hotkeyErrorKey_ = valid ? "" : "settings.hotkey.invalid";
        this->Require(this->renderer_->SetFieldError("captureHotkey", valid ? L"" : this->Text(this->hotkeyErrorKey_)));
        if (!valid)
            this->SetStatus("settings.hotkey.invalid");
        return valid;
    }

    // 在注册事务完成后同步托盘与设置文字。
    // 入参：无。
    // 返回：无；宿主异常由统一忙状态边界处理。
    void RefreshHotkeyUi()
    {
        if (this->callbacks_.hotkeyRefresh)
            this->callbacks_.hotkeyRefresh();
        this->RefreshTexts();
    }
    // 检查渲染器操作是否成功，将布局或控件错误交给统一异常处理流程。
    // 入参：result 为需要检查的渲染器操作结果。
    // 返回：无返回值；结果失败时记录结构化定位并抛出异常，由外层窗口边界处理。
    void Require(const RendererResult& result) const
    {
        if (!result)
        {
            OPEN_ST_LOG_ERROR("Settings renderer failed. code=", result.code, " path=", result.path, " id=", result.id);
            throw std::runtime_error("Settings renderer failure");
        }
    }

    // 按业务文本键查询宿主当前语言的文案或动态启动项状态。
    // 入参：key 为宿主提供的动态界面文本键。
    // 返回：当前语言的宽字符串；启动项状态键优先从 startupStatus 回调取得。
    std::wstring Text(std::string_view key) const
    {
        if (key == "settings.startup.status" && this->callbacks_.startupStatus)
            return this->callbacks_.startupStatus();
        if (key == "settings.hotkey.status")
            return this->callbacks_.hotkeyStatus();
        return this->callbacks_.text(key);
    }

    // 保存状态文本键并刷新本地化状态栏，供后续语言切换重新查询。
    // 入参：key 为要显示的状态文案键；空键表示清除状态。
    // 返回：无返回值。
    void SetStatus(std::string_view key)
    {
        this->statusKey_ = key;
        this->Require(this->renderer_->SetStatus(key.empty() ? L"" : this->Text(key)));
    }

    // 草稿与待生效状态共同决定应用按钮是否可用。
    // 入参：无显式入参。
    // 返回：无返回值。
    void UpdateButtons()
    {
        this->Require(this->renderer_->SetEnabled("languageSelector", this->ready_));
        if (this->callbacks_.startupApplied)
            this->Require(this->renderer_->SetEnabled("startupEnabled", this->ready_));
        this->Require(this->renderer_->SetEnabled(
            "applyButton", this->ready_ && (this->editSession_.IsDirty() || this->pendingLanguage_.has_value() ||
                                            this->pendingStartup_.has_value() || this->HotkeyNeedsApply())));
        this->Require(this->renderer_->SetEnabled("startupRepairButton", this->ready_));
        this->Require(this->renderer_->SetEnabled("captureHotkey", this->ready_));
        this->Require(this->renderer_->SetEnabled("acceptButton", this->ready_));
        this->Require(this->renderer_->SetEnabled("restoreDefaultsButton", this->ready_));
    }

    // 通过宿主回调查询当前资源声明的语言，构造下拉框可用选项。
    // 入参：无显式入参。
    // 返回：成功时包含当前语言代码及显示标签；查询抛异常时返回失败标志和本地化错误。
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
    // 入参：language 为提交前待验证的语言代码。
    // 返回：语言仍在可用选项中为 true；不可用或查询失败为 false，同时更新字段错误显示。
    bool ValidateLanguage(std::string_view language)
    {
        const RendererOptionsResult options = this->QueryLanguages();
        const bool available =
            options.success &&
            std::any_of(options.options.begin(), options.options.end(),
                        // 以语言代码精确匹配动态选项，拒绝已从资源移除的语言。
                        // 入参：option 为当前检查的语言选项，其 value 为稳定语言代码。
                        // 返回：option.value 与捕获的目标语言一致为 true，否则为 false。
                        [language](const RendererOption& option) { return option.value == language; });
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

    // 将提交冲突、读取失败和写入失败转换为相应状态提示，保留用户草稿。
    // 入参：result 为编辑会话提交或重试核验的失败类别。
    // 返回：无返回值。
    void ReportCommitFailure(SettingsCommitResult result)
    {
        this->SetStatus(result == SettingsCommitResult::Conflict ? "settings.conflict" : "settings.save_failed");
    }

    // 提交设置草稿并应用已保存的语言和自启意图，失败时保留可重试状态。
    // 入参：closeWhenDone 为全部保存和运行期应用成功后是否关闭窗口。
    // 返回：无返回值。
    void Apply(bool closeWhenDone)
    {
        if (this->busy_ || !this->ready_)
        {
            return;
        }
        if (!this->editSession_.IsDirty() && !this->pendingLanguage_.has_value() &&
            !this->pendingStartup_.has_value() && !this->HotkeyNeedsApply())
        {
            if (closeWhenDone)
            {
                this->Require(this->renderer_->RequestClose());
            }
            return;
        }
        this->RunBusy(
            // 提交草稿后分别复核并应用语言和启动项，副作用失败保留重试目标。
            // 入参：无显式入参。
            // 返回：无返回值。
            [this, closeWhenDone]()
            {
                const std::optional<std::string> language = this->editSession_.ReadString("ui.language");
                if (!language.has_value() || !this->ValidateLanguage(*language))
                {
                    return;
                }
                const bool languageChanged = this->editSession_.IsDirty("ui.language");
                const bool startupChanged = this->editSession_.IsDirty("startup.enabled");
                const bool hotkeyChanged = this->editSession_.IsDirty("capture.hotkey");
                const std::optional<bool> startup = this->editSession_.ReadBool("startup.enabled");
                const std::optional<std::string> hotkey = this->ReadHotkey();
                if (!startup || !hotkey || !this->ValidateHotkey(*hotkey))
                    return;
                const bool dirty = this->editSession_.IsDirty();
                const bool needsHotkey = hotkeyChanged || this->HotkeyNeedsApply();
                if (dirty || needsHotkey)
                {
                    std::optional<std::string> pendingLanguage = languageChanged ? language : this->pendingLanguage_;
                    std::optional<bool> pendingStartup = startupChanged ? startup : this->pendingStartup_;
                    struct CandidateGuard final
                    {
                        SettingsWindowCallbacks& callbacks;
                        bool& cleanupPending;
                        bool prepared{};
                        // 在提交失败或异常时撤销候选，保留旧活动注册。
                        // 入参：无。
                        // 返回：无；宿主契约要求 finish 不抛出，防御性捕获仍保留清理故障。
                        ~CandidateGuard() noexcept
                        {
                            if (!this->prepared)
                                return;
                            try
                            {
                                this->cleanupPending = !this->callbacks.hotkeyFinish(false);
                            }
                            catch (...)
                            {
                                this->cleanupPending = true;
                                OPEN_ST_LOG_ERROR("Hotkey candidate rollback failed.");
                            }
                        }
                    } candidate{this->callbacks_, this->hotkeyCleanupPending_};
                    if (needsHotkey)
                    {
                        if (!hotkeyChanged)
                        {
                            const SettingsCommitResult verified =
                                this->editSession_.VerifyCurrentString("capture.hotkey", *hotkey);
                            if (verified != SettingsCommitResult::Unchanged)
                            {
                                this->ReportCommitFailure(verified);
                                return;
                            }
                        }
                        if (!this->callbacks_.hotkeyPrepare(*hotkey))
                        {
                            this->SetStatus("settings.hotkey.registration_failed");
                            return;
                        }
                        candidate.prepared = true;
                    }
                    const SettingsCommitResult committed =
                        dirty ? this->editSession_.Commit() : SettingsCommitResult::Unchanged;
                    if (committed != SettingsCommitResult::Saved && committed != SettingsCommitResult::Unchanged)
                    {
                        if (candidate.prepared)
                        {
                            candidate.prepared = false;
                            this->hotkeyCleanupPending_ = !this->callbacks_.hotkeyFinish(false);
                        }
                        this->RefreshHotkeyUi();
                        this->ReportCommitFailure(committed);
                        return;
                    }
                    if (candidate.prepared)
                    {
                        candidate.prepared = false;
                        this->hotkeyCleanupPending_ = !this->callbacks_.hotkeyFinish(true);
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
                this->RefreshHotkeyUi();
                if (verificationFailure != SettingsCommitResult::Unchanged)
                    this->ReportCommitFailure(verificationFailure);
                else
                    this->SetStatus(this->hotkeyCleanupPending_           ? "settings.hotkey.cleanup_pending"
                                    : !languageApplied && !startupApplied ? "settings.effects_failed"
                                    : !languageApplied                    ? "settings.language.apply_failed"
                                    : !startupApplied                     ? "settings.startup.apply_failed"
                                                                          : "settings.saved");
                this->closeAfterBusy_ = !this->hotkeyCleanupPending_ &&
                                        (this->closeAfterBusy_ || (languageApplied && startupApplied && closeWhenDone));
            });
    }

    // 请求用户确认恢复默认或重新加载操作，测试可替换确认边界。
    // 入参：key 为确认问题的本地化文本键。
    // 返回：用户选择是或确认替身接受时为 true，否则为 false。
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
    // 入参：无显式入参。
    // 返回：无返回值。
    void RestoreDefaults()
    {
        if (this->busy_ || !this->ready_)
        {
            return;
        }
        this->RunBusy(
            // 经确认恢复语言和启动项草稿默认值，重新校验可用语言并刷新控件。
            // 入参：无显式入参。
            // 返回：无返回值。
            [this]()
            {
                if (!this->Confirm("settings.restore_confirm"))
                {
                    return;
                }
                const std::string page = this->renderer_->GetActivePageId();
                std::vector<std::string> fields;
                if (page == this->renderer_->GetControlPageId("languageSelector"))
                    fields.emplace_back("ui.language");
                if (page == this->renderer_->GetControlPageId("startupEnabled"))
                    fields.emplace_back("startup.enabled");
                if (page == this->renderer_->GetControlPageId("captureHotkey"))
                    fields.emplace_back("capture.hotkey");
                SettingsEditSession candidate = this->editSession_;
                if (fields.empty() || !candidate.RestoreDefaults(fields))
                {
                    this->SetStatus("settings.defaults_failed");
                    return;
                }
                if (std::find(fields.begin(), fields.end(), "ui.language") != fields.end())
                {
                    const std::optional<std::string> language = candidate.ReadString("ui.language");
                    const RendererOptionsResult options = this->QueryLanguages();
                    if (!language.has_value() || !options.success ||
                        !std::any_of(options.options.begin(), options.options.end(),
                                     // 检查恢复出的默认语言是否仍在当前资源支持的选项中。
                                     // 入参：option 为当前检查的语言选项，其 value 为稳定语言代码。
                                     // 返回：option.value 与捕获的目标语言一致为 true，否则为 false。
                                     [&language](const RendererOption& option) { return option.value == *language; }))
                    {
                        this->SetStatus("settings.defaults_failed");
                        return;
                    }
                }
                if (std::find(fields.begin(), fields.end(), "capture.hotkey") != fields.end())
                {
                    const std::optional<std::string> value = candidate.ReadString("capture.hotkey");
                    if (!value || !this->callbacks_.hotkeyDecode(*value))
                    {
                        this->SetStatus("settings.defaults_failed");
                        return;
                    }
                    this->hotkeyErrorKey_.clear();
                }
                this->editSession_ = std::move(candidate);
                this->Require(this->renderer_->RefreshValues());
                this->SetStatus({});
            });
    }

    // 提供可操作的读取重试和冲突重载；不撤销已保存内容。
    // 入参：无显式入参。
    // 返回：无返回值。
    void Reload()
    {
        if (this->busy_)
        {
            return;
        }
        this->RunBusy(
            // 有未保存草稿时先确认，再重开编辑会话并复核待重试副作用目标。
            // 入参：无显式入参。
            // 返回：无返回值。
            [this]()
            {
                if (this->editSession_.IsDirty() && !this->Confirm("settings.reload_confirm"))
                {
                    return;
                }
                this->ready_ = this->editSession_.Open({"ui.language", "capture.hotkey"}, {"startup.enabled"});
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
                this->hotkeyErrorKey_.clear();
                this->Require(this->renderer_->SetFieldError("languageSelector", {}));
                this->Require(this->renderer_->SetFieldError("captureHotkey", {}));
                this->SetStatus(this->pendingLanguage_.has_value() ? "settings.language.apply_failed"
                                : this->pendingStartup_            ? "settings.startup.apply_failed"
                                : this->hotkeyCleanupPending_      ? "settings.hotkey.cleanup_pending"
                                                                   : "");
            });
    }

    // 向宿主发送忙状态变化通知，吞掉通知异常以确保窗口仍能恢复交互。
    // 入参：busy 表示操作进入忙状态还是恢复空闲。
    // 返回：无返回值。
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

    // 在确认、提交和运行期应用期间禁用重复操作，异常后恢复交互并处理延迟关闭。
    // 入参：operation 为本次受忙状态保护的同步操作，借用到调用结束。
    // 返回：无返回值。
    void RunBusy(const std::function<void()>& operation)
    {
        bool notified = false;
        this->closeAfterBusy_ = false;
        try
        {
            // 先结束完整录入，再进入业务忙状态，异常也经下方统一恢复。
            this->Require(this->renderer_->SetBusy(true));
            this->busy_ = true;
            this->NotifyBusy(true);
            notified = true;
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
        if (notified)
            this->NotifyBusy(false);
        this->Require(this->renderer_->SetBusy(false));
        this->UpdateButtons();
        if (this->closeAfterBusy_)
        {
            this->Require(this->renderer_->RequestClose());
        }
    }

    // 取消只关闭，不回滚已保存结果。
    // 入参：无显式入参。
    // 返回：无返回值。
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
    std::optional<std::string> recordingOriginal_;
    std::string statusKey_;
    std::string languageErrorKey_;
    std::string hotkeyErrorKey_;
    std::function<bool()> confirmation_;
    bool ready_{};
    bool busy_{};
    bool closeAfterBusy_{};
    bool hotkeyCleanupPending_{};
};

// 创建编排对象，不立即读取布局或创建窗口。
// 入参：无显式入参。
// 返回：无返回值。
SettingsWindow::SettingsWindow() : impl_(std::make_unique<Impl>()) {}

// 上级回调对象销毁前同步释放窗口。
// 入参：无显式入参。
// 返回：无返回值。
SettingsWindow::~SettingsWindow()
{
    this->Close();
}

// 在 UI 线程创建并显示设置窗口；窗口已存在时激活原窗口。
// 入参：instance 为当前程序模块句柄；callbacks 为移交给窗口的宿主回调集合，捕获对象须活到窗口释放回调。
// 返回：已有窗口激活或新建显示成功为 true；参数、布局、绑定或创建失败为 false。
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
// 入参：message 为应用消息循环取得的待处理消息。
// 返回：消息已被窗口键盘导航处理为 true，否则为 false。
bool SettingsWindow::ProcessDialogMessage(MSG& message) const noexcept
{
    return this->impl_->ProcessDialogMessage(message);
}

// 同步关闭供应用退出使用；控件动作通过内部延迟关闭。
// 入参：无显式入参。
// 返回：无返回值。
void SettingsWindow::Close() noexcept
{
    this->impl_->Close();
}

// 查询实际窗口存活状态。
// 入参：无显式入参。
// 返回：实际设置窗口存在时为 true，否则为 false。
bool SettingsWindow::IsOpen() const noexcept
{
    return this->impl_->IsOpen();
}

// 为测试返回借用窗口，避免全局搜索。
// 入参：window 为要检查的设置窗口对象。
// 返回：借用的原生窗口 HWND；未打开时为 nullptr，关闭后失效。
HWND SettingsWindowTestAccess::NativeHandle(const SettingsWindow& window) noexcept
{
    return window.impl_->NativeHandle();
}

// 注入确认边界以免测试阻塞在模态提示。
// 入参：window 为被测设置窗口；confirmation 为确认替身，返回 true 接受、false 取消；捕获对象须保持有效。
// 返回：无返回值。
void SettingsWindowTestAccess::SetConfirmation(SettingsWindow& window, std::function<bool()> confirmation)
{
    window.impl_->SetConfirmation(std::move(confirmation));
}
// 由上级在语言资源变化后刷新已打开的设置窗口。
// 入参：无显式入参。
// 返回：无返回值。
void SettingsWindow::RefreshTexts() noexcept
{
    this->impl_->RefreshTexts();
}
// 取得当前设置窗口句柄，供宿主设置模态窗口的 owner。
// 入参：无显式入参。
// 返回：借用的当前设置窗口 HWND；未打开时为 nullptr，关闭后失效，不得由调用方销毁。
HWND SettingsWindow::NativeHandle() const noexcept
{
    return this->impl_->NativeHandle();
}

// 转交宿主已验证的全局组合到当前录入控件。
// 入参：modifiers、key 为组合；time 为原始消息时间。
// 返回：录入控件已接受时为 true。
bool SettingsWindow::ProcessRecordedHotkey(UINT modifiers, UINT key, DWORD time) noexcept
{
    return this->impl_->ProcessRecordedHotkey(modifiers, key, time);
}

// 通过业务验证入口更新测试草稿，不写文件或操作系统。
// 入参：window 为被测窗口；value 为快捷键 token。
// 返回：草稿接受时为 true。
bool SettingsWindowTestAccess::ChangeHotkey(SettingsWindow& window, std::string_view value)
{
    return window.impl_->ChangeHotkey(value);
}

// 读取测试窗口的快捷键草稿。
// 入参：window 为被测窗口。
// 返回：快捷键草稿副本，编辑会话不可用时为空。
std::optional<std::string> SettingsWindowTestAccess::ReadHotkey(const SettingsWindow& window)
{
    return window.impl_->ReadHotkey();
}

} // namespace open_st
