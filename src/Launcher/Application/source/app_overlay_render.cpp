// 协调截图会话批次重绘、同一选区快照以及渲染返回后的失败和退出清理。

#include "capture_overlay_session.h"
#include "diagnostic_text.h"

#include <app.h>
#include <log.h>
#include <selection_model.h>

namespace open_st
{
// 用一次选区快照协调所有待绘制输出，并在图形调用返回后处理延迟关闭。
// 入参：window 为触发绘制的遮罩；drawOutput 为同步绘制边界，借用输出、快照和错误文本。
// 返回：无；失败取消会话，模态忙状态保持原有延迟回收；重入不启动嵌套批次。
void App::RenderOverlays(
    HWND window, const std::function<bool(CaptureOverlayOutput&, const SelectionSnapshot&, std::wstring&)>& drawOutput)
{
    if (!this->overlaySession_ || !this->selectionModel_ || this->overlayInvalidated_ || this->shuttingDown_)
    {
        PAINTSTRUCT paint{};
        BeginPaint(window, &paint);
        EndPaint(window, &paint);
        return;
    }
    if (this->overlayRendering_)
    {
        // 会话在进入任何可重入 Win32 调用之前已经占有批次；此调用仅验证并记录重绘。
        std::wstring error;
        (void)this->overlaySession_->Paint(window, {}, {}, error);
        return;
    }

    this->overlayRendering_ = true;
    OverlayPaintResult result = OverlayPaintResult::Failed;
    std::wstring error;
    try
    {
        const SelectionSnapshot snapshot = this->SelectionForDrawing();
        result = this->overlaySession_->Paint(
            window,
            // 同一批次所有输出借用同一份不可变快照，输入重入只影响下一批。
            // 入参：output 为当前会话输出；renderError 接收绘制边界失败原因。
            // 返回：该屏绘制成功为 true。
            [&drawOutput, &snapshot](CaptureOverlayOutput& output, std::wstring& renderError)
            { return drawOutput(output, snapshot, renderError); },
            // 图形调用前后检查显示失效或退出，避免继续访问下一输出。
            // 入参：无。返回：需要停止当前批次时 true。
            [this]() { return this->overlayInvalidated_ || this->shuttingDown_; }, error);
    }
    catch (...)
    {
        OPEN_ST_LOG_ERROR("Unexpected failure while coordinating capture overlay painting.");
    }
    this->overlayRendering_ = false;

    const bool failed = result == OverlayPaintResult::Failed;
    if (failed)
    {
        OPEN_ST_LOG_ERROR("Capture overlay batch rendering failed. detail=", WideToUtf8(error));
        this->overlayInvalidated_ = true;
    }
    // 整批图形调用已返回，现可应用重入期间保留的输入与取消屏障。
    // 它们产生下一批失效，不能改变刚完成批次的固定快照。
    if (!failed && !this->overlayInvalidated_ && !this->shuttingDown_)
        this->DrainOverlayPointers();
    if (this->overlayInvalidated_ || this->shuttingDown_)
    {
        this->CloseOverlay();
        if (failed && !this->CompletionBusy() && !this->shuttingDown_)
        {
            this->ShowCaptureError("capture.error.unknown");
        }
    }
    if (this->shuttingDown_)
    {
        // 退出消息可能在 Present 内重入；现在资源已经可安全回收，重新推进退出。
        this->UpdateCaptureGate();
    }
}
} // namespace open_st
