// 编排非模态设置窗口的控件绑定、草稿提交、运行时应用和延迟关闭。

#include "settings_edit.h"
#include "settings_internal.h"
#include "settings_messages.h"
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
            this->RefreshTexts();
            ShowWindow(this->renderer_->NativeHandle(), SW_RESTORE);
            SetForegroundWindow(this->renderer_->NativeHandle());
            return true;
        }
        if (instance == nullptr || !callbacks.text || !callbacks.currentLanguage || !callbacks.availableLanguages ||
            !callbacks.languageApplied || !callbacks.startupApplied || !callbacks.hotkeyDecode ||
            !callbacks.hotkeyEncode || !callbacks.hotkeyFormat || !callbacks.hotkeyPrepare || !callbacks.hotkeyFinish ||
            !callbacks.hotkeyStatus || !callbacks.hotkeyRecording || !callbacks.normalizeBorderColor ||
            !callbacks.validImageFormat || !callbacks.validJpegQuality ||
            (callbacks.ocrAvailable && (!callbacks.ocrModels || !callbacks.ocrLanguages || !callbacks.validOcrOptions)))
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
        if (!this->callbacks_.ocrAvailable && layout.contains("pages") && layout["pages"].is_array())
        {
            auto& pages = layout["pages"];
            for (auto page = pages.begin(); page != pages.end();)
            {
                if (page->is_object() && page->value("id", std::string{}) == "ocr")
                    page = pages.erase(page);
                else
                    ++page;
            }
        }
        this->renderer_ = std::make_unique<WindowRenderer>();
        this->Require(this->renderer_->LoadLayout(layout));
        this->ready_ = this->OpenEditSession();
        this->Require(this->renderer_->SetErrorHandler(
            // 将渲染器错误转为本地化字段或状态提示，未加载草稿时忽略选项缺失。
            // 入参：result 为渲染器报告的结构化错误，包含错误码及目标控件标识。
            // 返回：无返回值。
            [this](const RendererResult& result)
            {
                if (result.code == "value_unavailable" && !this->ready_)
                    return;
                if (result.id == "ocrModel" || result.id == "ocrLanguage")
                {
                    this->Require(this->renderer_->SetFieldError(result.id, this->Text("settings.ocr.invalid")));
                    return;
                }
                if (result.id == "defaultSaveFormat" || result.id == "selectionBorderColor" ||
                    result.id == "jpegQuality")
                {
                    const std::string_view key = result.id == "defaultSaveFormat" ? "export.default_format"
                                                 : result.id == "selectionBorderColor"
                                                     ? "capture.selection_border_color"
                                                     : "export.jpeg_quality";
                    this->Require(this->renderer_->SetFieldError(
                        result.id, result.code == "value_unavailable" ? this->StorageFieldError(key)
                                                                      : this->Text("settings.operation_failed")));
                    return;
                }
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
        this->BindStorageControls();
        this->BindOcrControls();
        this->BindMaintenanceControls();
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
        this->RefreshStorageErrors();
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
        this->integerInputInvalid_ = false;
        this->restoredDefaultsPending_ = false;
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
    bool ProcessDialogMessage(MSG& message) noexcept
    {
        try
        {
            if (!this->IsOpen())
                return false;
            this->UpdateMaintenanceButtons();
            return this->renderer_->ProcessDialogMessage(message);
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
            this->RefreshStorageErrors();
            this->UpdateMaintenanceButtons();
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
    // 打开全部已实现字段，整数单独登记以保留严格类型语义。
    // 入参：无。
    // 返回：基线和有效草稿加载成功时为 true。
    bool OpenEditSession()
    {
        std::vector<std::string> strings{"ui.language", "capture.hotkey", "capture.selection_border_color",
                                         "export.default_format"};
        if (this->callbacks_.ocrAvailable)
        {
            strings.emplace_back("ocr.model");
            strings.emplace_back("ocr.language");
        }
        return this->editSession_.Open(strings, {"startup.enabled"}, {"export.jpeg_quality"});
    }

    // 绑定 OCR 下拉草稿，领域选项及校验完全由宿主提供。
    // 入参：无。
    // 返回：无；关闭功能时不创建绑定或草稿。
    void BindOcrControls()
    {
        if (!this->callbacks_.ocrAvailable)
            return;
        for (const bool model : {true, false})
        {
            const std::string id = model ? "ocrModel" : "ocrLanguage";
            const std::string key = model ? "ocr.model" : "ocr.language";
            this->Require(this->renderer_->BindOptions(id,
                                                       // 查询即时领域选项并转换为公共渲染器协议。
                                                       // 入参：无。
                                                       // 返回：拥有选项值的查询结果，异常由渲染器捕获。
                                                       [this, model]()
                                                       {
                                                           RendererOptionsResult result;
                                                           const auto values = model ? this->callbacks_.ocrModels()
                                                                                     : this->callbacks_.ocrLanguages();
                                                           for (const SettingsOption& value : values)
                                                               result.options.push_back({value.value, value.label});
                                                           return result;
                                                       }));
            this->Require(this->renderer_->BindString(
                id,
                // 读取 OCR 草稿，不把无效配置替换成列表首项。
                // 入参：无。
                // 返回：完整配置值与读取状态。
                [this, key]()
                {
                    const auto value = this->editSession_.ReadString(key);
                    return RendererStringResult{value.has_value(), value.value_or(""), {}};
                },
                // 选择只改变设置草稿，下次识别才读取保存的参数。
                // 入参：value 为新配置值。
                // 返回：草稿变更是否被接受，不执行识别或持久化。
                [this, key](std::string_view value)
                {
                    if (!this->ready_ || this->busy_ || !this->editSession_.ChangeString(key, value))
                        return RendererChangeResult{false, this->Text("settings.operation_failed")};
                    this->SetStatus({});
                    this->RefreshOcrErrors();
                    this->UpdateButtons();
                    return RendererChangeResult{
                        true, this->OcrValid(this->editSession_) ? L"" : this->Text("settings.ocr.invalid")};
                }));
        }
    }

    // 按领域规则复核当前或默认候选，不在 Settings 复制允许值列表。
    // 入参：candidate 为待验证设置草稿。
    // 返回：功能关闭或组合有效时为 true。
    bool OcrValid(const SettingsEditSession& candidate) const
    {
        if (!this->callbacks_.ocrAvailable)
            return true;
        const auto model = candidate.ReadString("ocr.model");
        const auto language = candidate.ReadString("ocr.language");
        return model && language && this->callbacks_.validOcrOptions(*model, *language);
    }

    // 标识 OCR 配置原始类型待显式修复的情况。
    // 入参：无。
    // 返回：开放的 OCR 字段存在类型修复需求时为 true。
    bool OcrNeedsRepair() const noexcept
    {
        return this->callbacks_.ocrAvailable &&
               (this->editSession_.RequiresRepair("ocr.model") || this->editSession_.RequiresRepair("ocr.language"));
    }

    // 本地化 OCR 字段错误，不更改参数草稿。
    // 入参：无。
    // 返回：无；关闭功能时不访问不存在的控件。
    void RefreshOcrErrors()
    {
        if (!this->callbacks_.ocrAvailable)
            return;
        const bool valid = this->OcrValid(this->editSession_);
        for (const bool model : {true, false})
        {
            const char* key = model ? "ocr.model" : "ocr.language";
            const std::wstring error = !valid ? this->Text("settings.ocr.invalid")
                                       : this->editSession_.RequiresRepair(key)
                                           ? this->Text("settings.storage.repair_pending")
                                           : L"";
            this->Require(this->renderer_->SetFieldError(model ? "ocrModel" : "ocrLanguage", error));
        }
    }

    // 为维护页绑定独立动作，旧布局不含这些控件时保持兼容。
    // 入参：无。
    // 返回：无；布局含控件但缺少宿主实现时保留禁用状态。
    void BindMaintenanceControls()
    {
        if (!this->renderer_->GetControlPageId("openLogDirectoryButton").empty())
            this->Require(this->renderer_->BindAction("openLogDirectoryButton",
                                                      // 将打开请求纳入短暂忙状态，防止同步重复操作。
                                                      // 入参：无。
                                                      // 返回：无；缺少回调或已经忙碌时不执行。
                                                      [this]()
                                                      {
                                                          if (this->busy_ || !this->callbacks_.openLogDirectory)
                                                              return;
                                                          this->RunBusy(
                                                              // 请求宿主打开实际日志目录，失败显示独立提示。
                                                              // 入参：无。
                                                              // 返回：无，不保存任何设置草稿。
                                                              [this]()
                                                              {
                                                                  if (!this->callbacks_.openLogDirectory())
                                                                      this->SetStatus(
                                                                          "settings.maintenance.open_failed");
                                                              });
                                                      }));
        if (!this->renderer_->GetControlPageId("clearHistoricalLogsButton").empty())
            this->Require(this->renderer_->BindAction(
                "clearHistoricalLogsButton",
                // 拒绝重复清理，再进入确认与任务启动边界。
                // 入参：无。
                // 返回：无；后台运行期间不维持整个窗口忙状态。
                [this]()
                {
                    if (this->busy_ || !this->callbacks_.clearHistoricalLogs || !this->callbacks_.maintenanceStatus ||
                        this->callbacks_.maintenanceStatus().running)
                        return;
                    this->RunBusy(
                        // 用户确认后启动历史清理并刷新任务状态。
                        // 入参：无。
                        // 返回：无，取消确认时不启动任务。
                        [this]()
                        {
                            if (!this->Confirm("settings.maintenance.clear_confirm"))
                                return;
                            if (!this->callbacks_.clearHistoricalLogs())
                                this->SetStatus("settings.maintenance.start_failed");
                            this->RefreshTexts();
                        });
                }));
        if (!this->renderer_->GetControlPageId("restoreAllSettingsButton").empty())
            // 转发已点击的全部恢复动作，由内部流程准备和确认默认候选。
            // 入参：无。
            // 返回：无。
            this->Require(
                this->renderer_->BindAction("restoreAllSettingsButton", [this]() { this->RestoreAllDefaults(); }));
    }

    // 维护动作只依赖自身可用性，非法颜色或数字草稿不阻断日志操作。
    // 入参：无。
    // 返回：无。
    void UpdateMaintenanceButtons()
    {
        this->Require(this->renderer_->SetEnabled("restoreDefaultsButton",
                                                  this->ready_ && this->renderer_->GetActivePageId() != "maintenance"));
        if (!this->renderer_->GetControlPageId("openLogDirectoryButton").empty())
            this->Require(this->renderer_->SetEnabled("openLogDirectoryButton",
                                                      static_cast<bool>(this->callbacks_.openLogDirectory)));
        if (!this->renderer_->GetControlPageId("clearHistoricalLogsButton").empty())
            this->Require(this->renderer_->SetEnabled("clearHistoricalLogsButton",
                                                      this->callbacks_.clearHistoricalLogs &&
                                                          this->callbacks_.maintenanceStatus &&
                                                          !this->callbacks_.maintenanceStatus().running));
        if (!this->renderer_->GetControlPageId("restoreAllSettingsButton").empty())
            this->Require(this->renderer_->SetEnabled("restoreAllSettingsButton", this->ready_));
    }

    // 明确恢复范围，不扫描未知配置或首次欢迎、保存目录等非可设置字段。
    // 入参：无。
    // 返回：当前构建已经开放编辑的字段。
    std::vector<std::string> AllSettingKeys() const
    {
        std::vector<std::string> keys{"ui.language",           "startup.enabled",
                                      "capture.hotkey",        "capture.selection_border_color",
                                      "export.default_format", "export.jpeg_quality"};
        if (this->callbacks_.ocrAvailable)
        {
            keys.emplace_back("ocr.model");
            keys.emplace_back("ocr.language");
        }
        return keys;
    }

    // 校验候选的实际类型与业务语义，完全独立于原始未完成输入缓冲。
    // 入参：candidate 为已经从默认资源准备的候选。
    // 返回：六字段均有效且语言仍可用时为 true。
    bool ValidateDefaults(const SettingsEditSession& candidate) const
    {
        const std::optional<std::string> language = candidate.ReadString("ui.language");
        const std::optional<bool> startup = candidate.ReadBool("startup.enabled");
        const std::optional<std::string> hotkey = candidate.ReadString("capture.hotkey");
        const std::optional<std::string> color = candidate.ReadString("capture.selection_border_color");
        const std::optional<std::string> format = candidate.ReadString("export.default_format");
        const std::optional<std::int64_t> quality = candidate.ReadInteger("export.jpeg_quality");
        const RendererOptionsResult options = this->QueryLanguages();
        return language && startup && hotkey && this->callbacks_.hotkeyDecode(*hotkey) && color &&
               this->callbacks_.normalizeBorderColor(*color) && format && this->callbacks_.validImageFormat(*format) &&
               quality && this->callbacks_.validJpegQuality(*quality) && options.success && this->OcrValid(candidate) &&
               std::any_of(options.options.begin(), options.options.end(),
                           // 在当前资源选项中精确匹配候选语言。
                           // 入参：option 为正在检查的语言选项。
                           // 返回：选项与候选语言代码一致时为 true。
                           [&language](const RendererOption& option) { return option.value == *language; });
    }

    // 使用受控占位符展示固定候选值，确认期间资源变化不会替换已经展示的目标。
    // 入参：candidate 为已经验证的六字段候选。
    // 返回：当前语言的完整确认说明。
    std::wstring DefaultConfirmation(const SettingsEditSession& candidate) const
    {
        const std::string language = *candidate.ReadString("ui.language");
        const std::string color = *candidate.ReadString("capture.selection_border_color");
        const std::string format = *candidate.ReadString("export.default_format");
        const std::optional<SettingsHotkeyChord> hotkey =
            this->callbacks_.hotkeyDecode(*candidate.ReadString("capture.hotkey"));
        const std::vector<std::pair<std::wstring, std::wstring>> replacements{
            {L"{language}", std::wstring(language.begin(), language.end())},
            {L"{startup}", this->Text(*candidate.ReadBool("startup.enabled") ? "settings.maintenance.enabled"
                                                                             : "settings.maintenance.disabled")},
            {L"{hotkey}", this->callbacks_.hotkeyFormat(*hotkey)},
            {L"{color}", std::wstring(color.begin(), color.end())},
            {L"{format}", this->Text(format == "jpeg" ? "settings.storage.jpeg" : "settings.storage.png")},
            {L"{quality}", std::to_wstring(*candidate.ReadInteger("export.jpeg_quality"))}};
        const std::wstring pattern = this->Text("settings.maintenance.restore_confirm");
        std::wstring result;
        for (std::size_t position = 0; position < pattern.size();)
        {
            bool replaced = false;
            for (const auto& [token, value] : replacements)
            {
                if (pattern.compare(position, token.size(), token) == 0)
                {
                    result += value;
                    position += token.size();
                    replaced = true;
                    break;
                }
            }
            if (!replaced)
                result += pattern[position++];
        }
        if (this->callbacks_.ocrAvailable)
        {
            for (const bool model : {true, false})
            {
                const auto values = model ? this->callbacks_.ocrModels() : this->callbacks_.ocrLanguages();
                const auto selected = candidate.ReadString(model ? "ocr.model" : "ocr.language");
                for (const SettingsOption& value : values)
                {
                    if (selected == value.value)
                        result += L"\n" + this->Text(model ? "settings.ocr.model" : "settings.ocr.language") + L": " +
                                  value.label;
                }
            }
        }
        return result;
    }

    // 确认后立即提交全部默认，失败不覆盖当前草稿或未完成数字文本。
    // 入参：无。
    // 返回：无。
    void RestoreAllDefaults()
    {
        if (this->busy_ || !this->ready_)
            return;
        this->RunBusy(
            // 准备固定默认候选，确认后执行与普通应用共用的条件提交。
            // 入参：无。
            // 返回：无；候选或确认失败时保留原始编辑状态。
            [this]()
            {
                SettingsEditSession candidate = this->editSession_;
                if (!candidate.RestoreDefaults(this->AllSettingKeys()) || !this->ValidateDefaults(candidate))
                {
                    this->SetStatus("settings.defaults_failed");
                    return;
                }
                const std::wstring message = this->DefaultConfirmation(candidate);
                const std::wstring title = this->Text("settings.title");
                const bool confirmed =
                    this->confirmation_ ? this->confirmation_()
                                        : MessageBoxW(this->renderer_->NativeHandle(), message.c_str(), title.c_str(),
                                                      MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
                if (confirmed)
                    this->ExecuteApply(std::move(candidate), true, false);
            });
    }

    // 连接颜色原始输入、格式选择和可无效的数字编辑状态。
    // 入参：无。
    // 返回：无；绑定失败由现有窗口创建边界收敛。
    void BindStorageControls()
    {
        this->Require(this->renderer_->BindString(
            "selectionBorderColor",
            // 返回原始文本，保留大小写和未完成颜色。
            // 入参：无。
            // 返回：当前字符串草稿及读取状态。
            [this]()
            {
                const std::optional<std::string> value =
                    this->editSession_.ReadString("capture.selection_border_color");
                return RendererStringResult{value.has_value(), value.value_or(""), {}};
            },
            // 保存原始编辑文本；非法中间内容也留在草稿中阻止整批提交。
            // 入参：value 为输入框当前全部文本。
            // 返回：草稿更新结果和当前字段提示。
            [this](std::string_view value)
            {
                if (!this->ready_ || this->busy_ ||
                    !this->editSession_.ChangeString("capture.selection_border_color", value))
                    return RendererChangeResult{false, this->Text("settings.operation_failed")};
                this->RefreshStorageErrors();
                this->UpdateButtons();
                return RendererChangeResult{true, this->StorageFieldError("capture.selection_border_color")};
            }));
        this->Require(this->renderer_->BindString(
            "defaultSaveFormat",
            // 读取格式原始 token，未知值不选择其他格式。
            // 入参：无。
            // 返回：当前格式草稿。
            [this]()
            {
                const std::optional<std::string> value = this->editSession_.ReadString("export.default_format");
                return RendererStringResult{value.has_value(), value.value_or(""), {}};
            },
            // 将明确选择的格式更新到草稿，不产生导出副作用。
            // 入参：value 为选择的稳定格式 token。
            // 返回：更新结果及字段错误。
            [this](std::string_view value)
            {
                if (!this->ready_ || this->busy_ || !this->editSession_.ChangeString("export.default_format", value))
                    return RendererChangeResult{false, this->Text("settings.operation_failed")};
                this->RefreshStorageErrors();
                this->UpdateButtons();
                return RendererChangeResult{true, this->StorageFieldError("export.default_format")};
            }));
        // 从宿主文本生成固定格式选项，配置 token 与语言分离。
        // 入参：无。
        // 返回：JPEG 和 PNG 两个选项。
        this->Require(
            this->renderer_->BindOptions("defaultSaveFormat",
                                         [this]()
                                         {
                                             RendererOptionsResult result;
                                             result.options.push_back({"jpeg", this->Text("settings.storage.jpeg")});
                                             result.options.push_back({"png", this->Text("settings.storage.png")});
                                             return result;
                                         }));
        this->Require(this->renderer_->BindInteger(
            "jpegQuality",
            // 提供原始整数意图，语义越界不夹取；未完成文本阻止程序刷新覆盖。
            // 入参：无。
            // 返回：当前整数及读取状态。
            [this]()
            {
                const std::optional<std::int64_t> value = this->editSession_.ReadInteger("export.jpeg_quality");
                return RendererIntegerResult{value.has_value() && !this->integerInputInvalid_, value.value_or(0),
                                             this->StorageFieldError("export.jpeg_quality")};
            },
            // 接收合法数字或无效状态，后者不能继续提交上次合法草稿。
            // 入参：value 为完整整数；空值表示无效或未完成输入。
            // 返回：接受编辑通知并给出字段错误；不可编辑时拒绝。
            [this](std::optional<std::int64_t> value)
            {
                if (!this->ready_ || this->busy_)
                    return RendererChangeResult{false, this->Text("settings.operation_failed")};
                this->integerInputInvalid_ = !value.has_value();
                if (value && !this->editSession_.ChangeInteger("export.jpeg_quality", *value))
                    return RendererChangeResult{false, this->Text("settings.operation_failed")};
                this->RefreshStorageErrors();
                this->UpdateButtons();
                return RendererChangeResult{true, this->StorageFieldError("export.jpeg_quality")};
            }));
    }

    // 查询单字段业务错误，类型错误默认回退显示待修复提示。
    // 入参：key 为本页字段的动态设置键。
    // 返回：本地化错误或待修复说明；字段正常为空串。
    std::wstring StorageFieldError(std::string_view key) const
    {
        bool valid = false;
        const char* invalidKey = "settings.storage.quality_invalid";
        if (key == "export.jpeg_quality")
        {
            const std::optional<std::int64_t> value = this->editSession_.ReadInteger(key);
            valid = !this->integerInputInvalid_ && value && this->callbacks_.validJpegQuality(*value);
        }
        else
        {
            const std::optional<std::string> value = this->editSession_.ReadString(key);
            const bool color = key == "capture.selection_border_color";
            invalidKey = color ? "settings.storage.color_invalid" : "settings.storage.format_invalid";
            valid = value && (color ? this->callbacks_.normalizeBorderColor(*value).has_value()
                                    : this->callbacks_.validImageFormat(*value));
        }
        return !valid                                   ? this->Text(invalidKey)
               : this->editSession_.RequiresRepair(key) ? this->Text("settings.storage.repair_pending")
                                                        : L"";
    }

    // 复核全部存储字段，不把未完成数字当作上次合法值。
    // 入参：无。
    // 返回：三个字段均满足业务规则时为 true。
    bool StorageValid() const
    {
        const std::optional<std::string> color = this->editSession_.ReadString("capture.selection_border_color");
        const std::optional<std::string> format = this->editSession_.ReadString("export.default_format");
        const std::optional<std::int64_t> quality = this->editSession_.ReadInteger("export.jpeg_quality");
        return !this->integerInputInvalid_ && color && this->callbacks_.normalizeBorderColor(*color) && format &&
               this->callbacks_.validImageFormat(*format) && quality && this->callbacks_.validJpegQuality(*quality);
    }

    // 查询需通过显式应用修复的原始类型错误，不把缺字段视为变更。
    // 入参：无。
    // 返回：本页任意字段待修复时为 true。
    bool StorageNeedsRepair() const noexcept
    {
        return this->editSession_.RequiresRepair("capture.selection_border_color") ||
               this->editSession_.RequiresRepair("export.default_format") ||
               this->editSession_.RequiresRepair("export.jpeg_quality");
    }

    // 重取三个字段错误，语言刷新不替换原始编辑文本。
    // 入参：无。
    // 返回：无。
    void RefreshStorageErrors()
    {
        this->Require(this->renderer_->SetFieldError("selectionBorderColor",
                                                     this->StorageFieldError("capture.selection_border_color")));
        this->Require(
            this->renderer_->SetFieldError("defaultSaveFormat", this->StorageFieldError("export.default_format")));
        this->Require(this->renderer_->SetFieldError("jpegQuality", this->StorageFieldError("export.jpeg_quality")));
        this->RefreshOcrErrors();
    }
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
        if (key == "settings.maintenance.status")
            return this->callbacks_.maintenanceStatus ? this->callbacks_.maintenanceStatus().text
                                                      : this->callbacks_.text("settings.maintenance.unavailable");
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
            "applyButton",
            this->ready_ && this->StorageValid() && this->OcrValid(this->editSession_) &&
                (this->editSession_.IsDirty() || this->StorageNeedsRepair() || this->OcrNeedsRepair() ||
                 this->pendingLanguage_.has_value() || this->pendingStartup_.has_value() || this->HotkeyNeedsApply())));
        this->Require(this->renderer_->SetEnabled("startupRepairButton", this->ready_));
        this->Require(this->renderer_->SetEnabled("captureHotkey", this->ready_));
        this->Require(this->renderer_->SetEnabled("selectionBorderColor", this->ready_));
        this->Require(this->renderer_->SetEnabled("defaultSaveFormat", this->ready_));
        this->Require(this->renderer_->SetEnabled("jpegQuality", this->ready_));
        if (this->callbacks_.ocrAvailable)
        {
            this->Require(this->renderer_->SetEnabled("ocrModel", this->ready_));
            this->Require(this->renderer_->SetEnabled("ocrLanguage", this->ready_));
        }
        this->Require(this->renderer_->SetEnabled("acceptButton", this->ready_ && this->StorageValid() &&
                                                                      this->OcrValid(this->editSession_)));
        this->UpdateMaintenanceButtons();
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
        if (result == SettingsCommitResult::Busy)
        {
            this->SetStatus("settings.file_busy");
            if (!ShowSettingsBusyMessage(this->renderer_->NativeHandle(), this->callbacks_.smallIcon,
                                         this->callbacks_.text))
                OPEN_ST_LOG_ERROR("Settings file-busy message failed.");
            return;
        }
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
        if (!this->StorageValid() || !this->OcrValid(this->editSession_))
        {
            this->RefreshStorageErrors();
            return;
        }
        if (!this->editSession_.IsDirty() && !this->StorageNeedsRepair() && !this->OcrNeedsRepair() &&
            !this->pendingLanguage_.has_value() && !this->pendingStartup_.has_value() && !this->HotkeyNeedsApply())
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
            [this, closeWhenDone]() { this->ExecuteApply(this->editSession_, false, closeWhenDone); });
    }

    // 共用条件提交和系统应用顺序；恢复候选在保存成功之前不覆盖原始草稿。
    // 入参：commitSession 为独立候选；restoreAll 强制六字段检查和系统应用；closeWhenDone 为成功后关闭。
    // 返回：无；失败保留草稿或已保存的待应用状态。
    void ExecuteApply(SettingsEditSession commitSession, bool restoreAll, bool closeWhenDone)
    {
        const std::optional<std::string> language = commitSession.ReadString("ui.language");
        if (restoreAll && !this->ValidateDefaults(commitSession))
        {
            this->SetStatus("settings.defaults_failed");
            return;
        }
        if (!language.has_value() || (!restoreAll && !this->ValidateLanguage(*language)))
        {
            return;
        }
        const bool languageChanged = restoreAll || commitSession.IsDirty("ui.language");
        const bool startupChanged = restoreAll || commitSession.IsDirty("startup.enabled");
        const bool hotkeyChanged = commitSession.IsDirty("capture.hotkey");
        const std::optional<bool> startup = commitSession.ReadBool("startup.enabled");
        const std::optional<std::string> hotkey = commitSession.ReadString("capture.hotkey");
        if (!startup || !hotkey || (!restoreAll && !this->ValidateHotkey(*hotkey)))
            return;
        if (!restoreAll && (!this->StorageValid() || !this->OcrValid(commitSession)))
            return;
        const bool dirty =
            restoreAll || commitSession.IsDirty() || this->StorageNeedsRepair() || this->OcrNeedsRepair();
        const bool needsHotkey = restoreAll || hotkeyChanged || this->HotkeyNeedsApply();
        if (dirty || needsHotkey)
        {
            std::optional<std::string> pendingLanguage = languageChanged ? language : this->pendingLanguage_;
            std::optional<bool> pendingStartup = startupChanged ? startup : this->pendingStartup_;
            std::vector<std::string> requiredKeys;
            std::vector<std::string> explicitlyEditedKeys =
                restoreAll ? this->AllSettingKeys() : std::vector<std::string>{};
            if (commitSession.IsDirty("capture.selection_border_color"))
                explicitlyEditedKeys.emplace_back("capture.selection_border_color");
            for (const char* key : {"capture.selection_border_color", "export.default_format", "export.jpeg_quality"})
                if (commitSession.RequiresRepair(key))
                    requiredKeys.emplace_back(key);
            if (this->callbacks_.ocrAvailable)
            {
                for (const char* key : {"ocr.model", "ocr.language"})
                    if (commitSession.RequiresRepair(key))
                        requiredKeys.emplace_back(key);
            }
            if (commitSession.IsDirty("capture.selection_border_color") ||
                commitSession.RequiresRepair("capture.selection_border_color"))
            {
                const std::optional<std::string> color =
                    this->callbacks_.normalizeBorderColor(*commitSession.ReadString("capture.selection_border_color"));
                if (!color || !commitSession.ChangeString("capture.selection_border_color", *color))
                    return;
            }
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
                if (!restoreAll && !hotkeyChanged)
                {
                    const SettingsCommitResult verified = commitSession.VerifyCurrentString("capture.hotkey", *hotkey);
                    if (verified != SettingsCommitResult::Unchanged)
                    {
                        this->ReportCommitFailure(verified);
                        return;
                    }
                }
                if (!this->callbacks_.hotkeyPrepare(*hotkey))
                {
                    this->SetStatus(restoreAll ? "settings.maintenance.defaults_hotkey_failed"
                                               : "settings.hotkey.registration_failed");
                    return;
                }
                candidate.prepared = true;
            }
            const SettingsCommitResult committed =
                dirty ? commitSession.Commit(requiredKeys, explicitlyEditedKeys) : SettingsCommitResult::Unchanged;
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
            this->editSession_ = std::move(commitSession);
            if (restoreAll)
            {
                this->integerInputInvalid_ = false;
                this->languageErrorKey_.clear();
                this->hotkeyErrorKey_.clear();
                this->restoredDefaultsPending_ = true;
            }
            this->pendingLanguage_.swap(pendingLanguage);
            this->pendingStartup_.swap(pendingStartup);
            this->Require(this->renderer_->RefreshValues());
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
        if (this->restoredDefaultsPending_)
        {
            const bool complete = languageApplied && startupApplied && !this->hotkeyCleanupPending_ &&
                                  verificationFailure == SettingsCommitResult::Unchanged;
            this->SetStatus(complete ? "settings.maintenance.defaults_saved" : "settings.maintenance.defaults_partial");
            this->restoredDefaultsPending_ = !complete;
        }
        this->closeAfterBusy_ = !this->hotkeyCleanupPending_ &&
                                (this->closeAfterBusy_ || (languageApplied && startupApplied && closeWhenDone));
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
        if (this->busy_ || !this->ready_ || this->renderer_->GetActivePageId() == "maintenance")
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
                if (page == this->renderer_->GetControlPageId("selectionBorderColor"))
                    fields.emplace_back("capture.selection_border_color");
                if (page == this->renderer_->GetControlPageId("defaultSaveFormat"))
                    fields.emplace_back("export.default_format");
                if (page == this->renderer_->GetControlPageId("jpegQuality"))
                    fields.emplace_back("export.jpeg_quality");
                if (this->callbacks_.ocrAvailable && page == this->renderer_->GetControlPageId("ocrModel"))
                {
                    fields.emplace_back("ocr.model");
                    fields.emplace_back("ocr.language");
                }
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
                for (const std::string& key : fields)
                {
                    bool valid = true;
                    if (key == "capture.selection_border_color")
                    {
                        const std::optional<std::string> value = candidate.ReadString(key);
                        valid = value && this->callbacks_.normalizeBorderColor(*value).has_value();
                    }
                    else if (key == "export.default_format")
                    {
                        const std::optional<std::string> value = candidate.ReadString(key);
                        valid = value && this->callbacks_.validImageFormat(*value);
                    }
                    else if (key == "export.jpeg_quality")
                    {
                        const std::optional<std::int64_t> value = candidate.ReadInteger(key);
                        valid = value && this->callbacks_.validJpegQuality(*value);
                    }
                    if (!valid)
                    {
                        this->SetStatus("settings.defaults_failed");
                        return;
                    }
                }
                if (std::find(fields.begin(), fields.end(), "ocr.model") != fields.end() && !this->OcrValid(candidate))
                {
                    this->SetStatus("settings.defaults_failed");
                    return;
                }
                this->editSession_ = std::move(candidate);
                if (std::find(fields.begin(), fields.end(), "export.jpeg_quality") != fields.end())
                    this->integerInputInvalid_ = false;
                this->Require(this->renderer_->RefreshValues());
                this->RefreshStorageErrors();
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
                if ((this->editSession_.IsDirty() || this->integerInputInvalid_) &&
                    !this->Confirm("settings.reload_confirm"))
                {
                    return;
                }
                this->ready_ = this->OpenEditSession();
                if (!this->ready_)
                {
                    this->SetStatus("settings.edit_load_failed");
                    return;
                }
                this->integerInputInvalid_ = false;
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
                this->RefreshStorageErrors();
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
                this->SetStatus(this->restoredDefaultsPending_       ? "settings.maintenance.defaults_partial"
                                : this->pendingLanguage_.has_value() ? "settings.language.apply_failed"
                                : this->pendingStartup_              ? "settings.startup.apply_failed"
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
    bool integerInputInvalid_{};
    bool restoredDefaultsPending_{};
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
