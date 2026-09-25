// 复用公共表单展示OCR草稿，文本复制仍归Export。
#include "ocr_result_window.h"
#include "diagnostic_text.h"
#include <clipboard_writer.h>
#include <limits>
#include <ui_text.h>
#include <window_renderer.h>

namespace open_st
{
// 初始化未显示窗口。
// 入参：无。返回：空草稿实例。
OcrResultWindow::OcrResultWindow() = default;
// 销毁公共窗口及已编辑文本。
// 入参：无。返回：无。
OcrResultWindow::~OcrResultWindow() = default;
// 创建表单，回调仅更新草稿或请求延迟关闭。
// 入参：owner/icon/cancel为宿主提供的窗口资源及窄取消通知。返回：创建成功为true。
bool OcrResultWindow::Show(HWND owner, HICON icon, std::function<void()> cancel)
{
    this->cancel_ = std::move(cancel);
    this->renderer_ = std::make_unique<WindowRenderer>();
    auto& renderer = *this->renderer_;
    if (!renderer.LoadLayout(nlohmann::json::parse(R"({
      "schemaVersion":1,"window":{"titleKey":"ocr.title","initialSize":[620,520],"minSize":[440,360],"resizable":true},
      "content":{"type":"column","id":"body","padding":16,"gap":12,"children":[
        {"type":"text","id":"hint","textKey":"ocr.snapshot_help"},
        {"type":"edit","id":"result","labelKey":"ocr.result","multiline":true,"visibleLines":14,"verticalScroll":true,"maxLength":1000000,"width":"fill"}]},
      "footer":{"leading":[{"type":"button","id":"copy","textKey":"ocr.copy_all"}],
        "trailing":[{"type":"button","id":"close","textKey":"ocr.close"}]}})")) ||
        !renderer.SetTextResolver([](std::string_view key) { return GetUiText(key); }) ||
        !renderer.BindString(
            "result", [this]() { return RendererStringResult{true, this->draft_, {}}; },
            // 草稿在窗口内拥有，刷新阶段不覆盖它。
            // 入参：value为完整UTF-8正文。返回：接受或超限错误。
            [this](std::string_view value)
            {
                this->draft_ = value;
                (void)this->renderer_->SetEnabled("copy", !this->draft_.empty());
                return RendererChangeResult{};
            }) ||
        !renderer.BindAction(
            "copy",
            // 将编辑后的UTF-8草稿适配至Unicode剪贴板入口，不记录正文。
            // 入参：无。返回：无，失败保留内容。
            [this]()
            {
                const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, this->draft_.data(),
                                                       static_cast<int>(this->draft_.size()), nullptr, 0);
                if (length <= 0)
                {
                    this->Status("ocr.copy_failed");
                    return;
                }
                std::wstring text(static_cast<std::size_t>(length), L'\0');
                if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, this->draft_.data(),
                                        static_cast<int>(this->draft_.size()), text.data(), length) != length)
                {
                    this->Status("ocr.copy_failed");
                    return;
                }
                std::wstring error;
                this->Status(CopyTextToClipboard(this->renderer_->NativeHandle(), text, error) ? "ocr.copied"
                                                                                               : "ocr.copy_failed");
            }) ||
        !renderer.BindAction("close", [this]() { this->Close(); }) ||
        !renderer.SetCloseHandler([this]() { this->Close(); }) || !renderer.SetDefaultAction("close"))
        return false;
    if (!renderer.Show({owner, icon, SW_SHOWNORMAL, true}))
        return false;
    (void)renderer.SetEnabled("result", false);
    (void)renderer.SetEnabled("copy", false);
    this->Status("ocr.preparing");
    return true;
}
// 更新状态标签而不刷新输入框。
// 入参：key为状态键。返回：无。
void OcrResultWindow::Status(std::string_view key)
{
    if (this->renderer_)
        (void)this->renderer_->SetStatus(GetUiText(key));
}
// 首次成功结果转为窗口自有草稿。
// 入参：text为有效UTF-16。返回：无。
void OcrResultWindow::SetResult(std::wstring_view text)
{
    this->draft_ = WideToUtf8(text);
    (void)this->renderer_->RefreshValue("result");
    (void)this->renderer_->SetEnabled("result", true);
    (void)this->renderer_->SetEnabled("copy", !this->draft_.empty());
}
// 先撤销结果发布资格，再请求公共延迟关闭。
// 入参：无。返回：无。
void OcrResultWindow::Close() noexcept
{
    try
    {
        if (this->cancel_)
            this->cancel_();
        if (this->renderer_)
            (void)this->renderer_->RequestClose();
    }
    catch (...)
    {
    }
}
// 激活已有结果，避免重复任务。
// 入参：无。返回：无。
void OcrResultWindow::Activate() noexcept
{
    if (this->IsOpen())
    {
        ShowWindow(this->renderer_->NativeHandle(), SW_RESTORE);
        SetForegroundWindow(this->renderer_->NativeHandle());
    }
}
// 返回窗口存在状态。
// 入参：无。返回：存在为true。
bool OcrResultWindow::IsOpen() const noexcept
{
    return this->renderer_ && IsWindow(this->renderer_->NativeHandle());
}
// 复用公共消息路由。
// 入参：message为当前消息。返回：消费结果。
bool OcrResultWindow::Process(MSG& message)
{
    return this->renderer_ && this->renderer_->ProcessDialogMessage(message);
}
// 复用公共编辑热键路由，避免全局注册吞掉编辑键。
// 入参：modifiers/key为已核验组合。返回：消费结果。
bool OcrResultWindow::RegisteredHotkey(UINT modifiers, UINT key) noexcept
{
    if (!this->renderer_ || GetForegroundWindow() != this->renderer_->NativeHandle())
        return false;
    if (!IsWindowEnabled(this->renderer_->NativeHandle()))
        return true;
    (void)this->renderer_->ProcessRegisteredEditHotkey(modifiers, key);
    return true;
}
// 模态交互时复用系统窗口输入禁用，不建立另一套消息循环。
// 入参：paused为宿主准入状态。返回：无。
void OcrResultWindow::Pause(bool paused) noexcept
{
    if (this->IsOpen())
        EnableWindow(this->renderer_->NativeHandle(), !paused);
}
} // namespace open_st
