// 连接快捷键注册与设置、消息门禁和本地化，保持兄弟模块互不依赖。

#include "annotation_interaction_controller.h"
#include <app.h>
#include <hotkeys.h>
#include <inline_text_editor.h>
#include <log.h>
#include <settings.h>
#include <settings_window.h>
#include <ui_text.h>

namespace open_st
{
// 读取初始配置并尝试注册；业务非法值显式回退默认，不写回用户配置。
// 入参：无。
// 返回：无；失败显示可从托盘继续操作的提示。
void App::InitializeHotkeys()
{
    this->hotkeys_ = std::make_unique<HotkeyManager>(this->messageWindow_);
    bool invalidUser = false;
    std::optional<std::string> value = GetStringSetting("capture.hotkey", invalidUser);
    HotkeyChord chord;
    const bool invalid = invalidUser || !value || !ParseHotkey(*value, chord);
    if (invalid)
        value = GetDefaultStringSetting("capture.hotkey");
    const bool valid = value && ParseHotkey(*value, chord);
    const bool prepared = valid && this->hotkeys_->Prepare(chord);
    if (prepared)
        (void)this->FinishHotkey(true);
    this->RefreshLocalizedUi();
    if (invalid || !prepared)
    {
        OPEN_ST_LOG_WARNING("Initial hotkey unavailable or invalid. win32_error=", this->hotkeys_->LastError());
        const std::wstring target =
            value ? std::wstring(value->begin(), value->end()) : GetUiText("hotkey.unregistered");
        const std::wstring message =
            GetUiText(invalid ? "hotkey.invalid_configuration" : "hotkey.registration_failed", {{L"hotkey", target}});
        (void)MessageBoxW(this->DialogOwner(), message.c_str(), GetUiText("app.title").c_str(), MB_OK | MB_ICONWARNING);
    }
    this->hotkeyBoundary_ = GetTickCount();
}

// 格式化实际活动组合，未注册时明确显示不可用。
// 入参：key 为包含 hotkey 占位符的本地化键。
// 返回：已替换的当前语言文本。
std::wstring App::HotkeyText(std::string_view key) const
{
    std::wstring value = GetUiText("hotkey.unregistered");
    if (this->hotkeys_)
    {
        const std::optional<HotkeyChord> active = this->hotkeys_->Active();
        if (active)
        {
            const std::string token = SerializeHotkey(*active);
            value.assign(token.begin(), token.end());
        }
    }
    return GetUiText(key, {{L"hotkey", value}});
}

// 分别显示活动状态和待清理错误，不把清理失败伪装为未保存。
// 入参：无。
// 返回：当前语言的组合状态。
std::wstring App::HotkeyStatusText() const
{
    const std::optional<std::string> saved = GetStringSetting("capture.hotkey");
    HotkeyChord chord;
    const std::wstring savedText = saved && ParseHotkey(*saved, chord) ? std::wstring(saved->begin(), saved->end())
                                                                       : GetUiText("settings.hotkey.invalid");
    return GetUiText("hotkey.configured", {{L"hotkey", savedText}}) + L"\n" +
           this->HotkeyText(this->hotkeyCleanupPending_ ? "hotkey.cleanup_pending" : "hotkey.active");
}

// 在旧注册仍生效时准备候选，拒绝非法业务配置。
// 入参：value 为规范化或可解析的组合 token。
// 返回：候选可提交为 true；失败保留旧注册。
bool App::PrepareHotkey(std::string_view value) noexcept
{
    HotkeyChord chord;
    if (!this->hotkeys_ || !ParseHotkey(value, chord))
        return false;
    const bool prepared = this->hotkeys_->Prepare(chord);
    if (!prepared)
        OPEN_ST_LOG_WARNING("Hotkey preparation failed. win32_error=", this->hotkeys_->LastError());
    return prepared;
}

// 无异常地切换注册身份并记录清理结果，避免保存成功后再做可失败的准备。
// 入参：commit 为是否已完成设置提交。
// 返回：候选或旧注册清理完成为 true。
bool App::FinishHotkey(bool commit) noexcept
{
    const bool clean = this->hotkeys_ && this->hotkeys_->Finish(commit);
    this->hotkeyCleanupPending_ = !clean;
    this->hotkeyBoundary_ = GetTickCount();
    if (!clean)
        OPEN_ST_LOG_WARNING("Hotkey cleanup remains pending.");
    return clean;
}

// 核验消息身份和时间，录入时转交控件，其余时进入现有截图准入。
// 入参：id、data 为系统组合通知；time 为原消息的 DWORD 时间戳。
// 返回：无；无效、过期或暂停期间的消息丢弃。
void App::DispatchHotkey(WPARAM id, LPARAM data, DWORD time)
{
    if (!this->hotkeys_ || !this->hotkeys_->Matches(id, data) || static_cast<LONG>(time - this->hotkeyBoundary_) <= 0)
        return;
    if (this->hotkeyRecording_)
    {
        if (this->settingsWindow_)
            (void)this->settingsWindow_->ProcessRecordedHotkey(LOWORD(data), HIWORD(data), time);
        return;
    }
    if (this->annotationInteraction_->TextActive())
    {
        // 用户配置的全局组合也可能是编辑键；已核验身份后交由持焦点的原位组件处理。
        // 其他截图组合在正文编辑期间继续暂停，不改变用户注册配置。
        if (!this->overlayInvalidated_ && !this->shuttingDown_ && LOWORD(data) == MOD_CONTROL)
        {
            switch (HIWORD(data))
            {
            case 'A':
                (void)this->annotationInteraction_->InvokeTextCommand(InlineEditCommand::SelectAll);
                break;
            case 'C':
                (void)this->annotationInteraction_->InvokeTextCommand(InlineEditCommand::Copy);
                break;
            case 'V':
                (void)this->annotationInteraction_->InvokeTextCommand(InlineEditCommand::Paste);
                break;
            case 'X':
                (void)this->annotationInteraction_->InvokeTextCommand(InlineEditCommand::Cut);
                break;
            case 'Z':
                (void)this->annotationInteraction_->InvokeTextCommand(InlineEditCommand::Undo);
                break;
            case 'Y':
                (void)this->annotationInteraction_->InvokeTextCommand(InlineEditCommand::Redo);
                break;
            default:
                break;
            }
        }
        return;
    }
    if (this->CompletionBusy() || this->dialogActive_ || this->welcoming_ || this->shuttingDown_ || this->settingsBusy_)
        return;
    this->StartCapture();
}
} // namespace open_st
