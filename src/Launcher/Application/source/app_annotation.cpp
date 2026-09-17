// 协调标注工具、样式与统一输出；不修改原冻结桌面或兄弟模块的私有状态。
#include "annotation_style_dialog.h"
#include "capture_annotation_state.h"
#include "capture_overlay_session.h"
#include <app.h>
#include <array>
#include <capture_toolbar.h>
#include <log.h>
#include <selection_output_renderer.h>
#include <ui_text.h>

namespace open_st
{
// 查询绘图模式，仅对已确认的截图区域生效。
// 入参：无。返回：需要阻止选区控制点输入时 true。
bool App::IsAnnotationToolActive() const noexcept
{
    return this->annotation_ && this->selectionModel_ && this->selectionModel_->HasSelection() &&
           this->annotation_->Tool() != CaptureAnnotationTool::Select;
}
// 借用共享不可变预览，所有屏幕使用同一版。
// 入参：无。返回：可为空的标注快照。
AnnotationSnapshot App::AnnotationForDrawing() const noexcept
{
    return this->annotation_ ? this->annotation_->Preview() : AnnotationSnapshot{};
}
// 更新互斥工具和撤销按钮，不改变完成命令的锁。
// 入参：无。返回：无，失败只记录日志。
void App::RefreshAnnotationToolbar() noexcept
try
{
    if (!this->annotation_ || !this->captureToolbar_)
        return;
    const CaptureAnnotationTool tool = this->annotation_->Tool();
    const std::array<ToolbarButtonState, 14> states{
        {{CaptureToolbarCommand::SelectTool, true, true, tool == CaptureAnnotationTool::Select},
         {CaptureToolbarCommand::RectangleTool, true, true, tool == CaptureAnnotationTool::Rectangle},
         {CaptureToolbarCommand::ArrowTool, true, true, tool == CaptureAnnotationTool::Arrow},
         {CaptureToolbarCommand::FilledRectangleTool, true, true, tool == CaptureAnnotationTool::FilledRectangle},
         {CaptureToolbarCommand::RoundedRectangleTool, true, true, tool == CaptureAnnotationTool::RoundedRectangle},
         {CaptureToolbarCommand::PenTool, true, true, tool == CaptureAnnotationTool::Pen},
         {CaptureToolbarCommand::LineTool, true, true, tool == CaptureAnnotationTool::Line},
         {CaptureToolbarCommand::EllipseTool, true, true, tool == CaptureAnnotationTool::Ellipse},
         {CaptureToolbarCommand::EraserTool, true, true, tool == CaptureAnnotationTool::Eraser},
         {CaptureToolbarCommand::TextTool, true, true, tool == CaptureAnnotationTool::Text},
         {CaptureToolbarCommand::MosaicTool, true, true, tool == CaptureAnnotationTool::Mosaic},
         {CaptureToolbarCommand::Undo, true, this->annotation_->CanUndo(), false},
         {CaptureToolbarCommand::Redo, true, this->annotation_->CanRedo(), false},
         {CaptureToolbarCommand::Style, true, tool != CaptureAnnotationTool::Select, false}}};
    if (!this->captureToolbar_->UpdateButtonStates(states).success)
        OPEN_ST_LOG_WARNING("Cannot refresh annotation toolbar states.");
}
catch (...)
{
    OPEN_ST_LOG_WARNING("Cannot allocate annotation toolbar state.");
}
// 消费已通过代次校验的工具操作。
// 入参：command 为工具栏命令。返回：无。
void App::HandleAnnotationCommand(CaptureToolbarCommand command) noexcept
{
    if (!this->annotation_ || this->annotation_->Active())
        return;
    switch (command)
    {
    case CaptureToolbarCommand::TextTool:
        this->annotation_->SetTool(CaptureAnnotationTool::Text);
        break;
    case CaptureToolbarCommand::MosaicTool:
        this->annotation_->SetTool(CaptureAnnotationTool::Mosaic);
        break;
    case CaptureToolbarCommand::PenTool:
        this->annotation_->SetTool(CaptureAnnotationTool::Pen);
        break;
    case CaptureToolbarCommand::LineTool:
        this->annotation_->SetTool(CaptureAnnotationTool::Line);
        break;
    case CaptureToolbarCommand::EllipseTool:
        this->annotation_->SetTool(CaptureAnnotationTool::Ellipse);
        break;
    case CaptureToolbarCommand::EraserTool:
        this->annotation_->SetTool(CaptureAnnotationTool::Eraser);
        break;
    case CaptureToolbarCommand::SelectTool:
        this->annotation_->SetTool(CaptureAnnotationTool::Select);
        break;
    case CaptureToolbarCommand::RectangleTool:
        this->annotation_->SetTool(CaptureAnnotationTool::Rectangle);
        break;
    case CaptureToolbarCommand::ArrowTool:
        this->annotation_->SetTool(CaptureAnnotationTool::Arrow);
        break;
    case CaptureToolbarCommand::FilledRectangleTool:
        this->annotation_->SetTool(CaptureAnnotationTool::FilledRectangle);
        break;
    case CaptureToolbarCommand::RoundedRectangleTool:
        this->annotation_->SetTool(CaptureAnnotationTool::RoundedRectangle);
        break;
    case CaptureToolbarCommand::Undo:
        this->RestoreAnnotationEdit(false);
        return;
    case CaptureToolbarCommand::Redo:
        this->RestoreAnnotationEdit(true);
        return;
    case CaptureToolbarCommand::Style:
        this->EditAnnotationStyle();
        return;
    default:
        return;
    }
    if (this->overlaySession_)
        this->overlaySession_->Invalidate();
    this->InvalidateToolbarCommands();
}
// 撤销/重做选区和标注，当前未提交手势只回滚。
// 入参：redo 为重做方向。返回：无。
void App::RestoreAnnotationEdit(bool redo) noexcept
{
    if (!this->annotation_ || !this->selectionModel_ || !this->overlaySession_ || this->CompletionBusy() ||
        this->dialogActive_ || this->settingsBusy_ || this->shuttingDown_ || this->overlayRendering_ ||
        this->overlayInvalidated_)
        return;
    this->CancelAnnotationPropertyRequest();
    if (this->HasSelectionInteraction())
    {
        if (redo)
            return;
        (void)this->CancelSelectionInput();
        this->overlaySession_->ReleaseMouse();
    }
    else
    {
        RectI restored{};
        if (!this->annotation_->Restore(redo, this->selectionModel_->Snapshot().rectangle, restored))
            return;
        this->selectionModel_->Reset();
        if (!this->selectionModel_->SelectRectangle(restored))
        {
            OPEN_ST_LOG_ERROR("Cannot restore annotation selection geometry.");
            this->CloseOverlay();
            return;
        }
    }
    this->overlaySession_->Invalidate();
    this->InvalidateToolbarCommands(true);
}
// 在完成忙状态中运行样式窗口，所有值有效且确认后才更新默认值。
// 入参：无。返回：无，布局失效在模态返回后回收。
void App::EditAnnotationStyle() noexcept
{
    if (!this->CanSubmitToolbarCommand() || !this->IsAnnotationToolActive())
        return;
    const AnnotationStyle original = this->annotation_->Style();
    AnnotationStyle candidate = original;
    const CaptureAnnotationTool tool = this->annotation_->Tool();
    const bool width = tool == CaptureAnnotationTool::Rectangle || tool == CaptureAnnotationTool::Arrow ||
                       tool == CaptureAnnotationTool::Pen || tool == CaptureAnnotationTool::Line ||
                       tool == CaptureAnnotationTool::Ellipse;
    unsigned eraserSize = this->annotation_->EraserDiameter();
    unsigned mosaicSize = this->annotation_->MosaicBlockSize();
    AnnotationTextStyle textStyle{this->annotation_->TextFontSize(), false, false};
    const HWND owner = this->overlaySession_->ActivationWindow();
    this->completionBusy_ = true;
    this->UpdateCaptureGate();
    AnnotationStyleResult result = AnnotationStyleResult::Failed;
    std::wstring error;
    try
    {
        if (tool == CaptureAnnotationTool::Eraser)
        {
            result = ShowAnnotationChoiceDialog(owner, eraserSize, CAPTURE_ANNOTATION_ERASER_DIAMETERS,
                                                "annotation.eraser.title", "annotation.eraser.diameter",
                                                this->MakeToolbarTextResolver(), error);
        }
        else if (tool == CaptureAnnotationTool::Mosaic)
        {
            result =
                ShowAnnotationChoiceDialog(owner, mosaicSize, ANNOTATION_MOSAIC_BLOCK_SIZES, "annotation.mosaic.title",
                                           "annotation.mosaic.block", this->MakeToolbarTextResolver(), error);
        }
        else if (tool == CaptureAnnotationTool::Text)
            result = ShowAnnotationTextStyleDialog(owner, candidate, textStyle, this->MakeToolbarTextResolver(), error);
        else
            result = ShowAnnotationStyleDialog(owner, candidate, width, this->MakeToolbarTextResolver(), error);
    }
    catch (...)
    {
        OPEN_ST_LOG_ERROR("Annotation style dialog failed.");
    }
    this->completionBusy_ = false;
    if (!this->overlayInvalidated_ && !this->shuttingDown_ && result == AnnotationStyleResult::Accepted)
    {
        if (tool == CaptureAnnotationTool::Eraser)
            (void)this->annotation_->SetEraserDiameter(eraserSize);
        else if (tool == CaptureAnnotationTool::Mosaic)
            (void)this->annotation_->SetMosaicBlockSize(mosaicSize);
        else
        {
            (void)this->annotation_->SetStyle(candidate);
            if (tool == CaptureAnnotationTool::Text)
                (void)this->annotation_->SetTextFontSize(textStyle.fontSize);
        }
    }
    if (result == AnnotationStyleResult::Failed)
        OPEN_ST_LOG_ERROR("Cannot edit annotation style.");
    if (result == AnnotationStyleResult::Failed)
        this->annotationFailure_ = true;
    if (this->overlayInvalidated_ || this->shuttingDown_)
        this->CloseOverlay();
    else if (this->overlaySession_)
        this->overlaySession_->RestoreFocus();
    this->UpdateCaptureGate();
}
// 仅在安全的消息边界显示提交失败，保留编辑文档供重试。
// 入参：无。返回：无，忙状态下继续保留待提示标记。
void App::ReportAnnotationFailure() noexcept
{
    if (!this->annotationFailure_ || this->CompletionBusy() || this->dialogActive_ || this->overlayRendering_ ||
        this->HasSelectionInteraction())
        return;
    this->annotationFailure_ = false;
    if (this->shuttingDown_)
        return;
    this->completionBusy_ = true;
    this->UpdateCaptureGate();
    try
    {
        // 完成忙状态保护全部遮罩，而不仅禁用提示的 owner，避免另一屏继续编辑。
        // 入参：无。返回：本地化失败文字。
        this->ShowSimpleMessage([]() { return GetUiText("annotation.edit_failed"); },
                                // 取得统一应用标题。
                                // 入参：无。返回：本地化标题。
                                []() { return GetUiText("app.title"); }, MB_OK | MB_ICONERROR);
    }
    catch (...)
    {
        OPEN_ST_LOG_ERROR("Cannot show annotation failure notice.");
    }
    this->completionBusy_ = false;
    if (this->overlayInvalidated_ || this->shuttingDown_)
        this->CloseOverlay();
    this->UpdateCaptureGate();
}
// 由复制、保存和贴图共享完整的最终合成链。
// 入参：selection/annotations 为开始完成操作时固定的值；frame/error 接收结果。返回：完整成功为 true。
bool App::GenerateAnnotatedSelection(RectI selection, const AnnotationSnapshot& annotations, SdrSelectionFrame& frame,
                                     std::wstring& error)
{
    if (this->overlayInvalidated_ || this->annotationPreparing_ || this->overlayRendering_ ||
        !this->frozenDesktopFrame_ || !this->outputRenderer_)
    {
        frame = {};
        error = L"截图会话已失效，无法生成标注图像。";
        return false;
    }
    bool rendered = false;
    {
        struct PreparationGuard
        {
            bool& active;
            // 异常展开也恢复准备标记，避免之后截图永久忙。
            // 入参：无。返回：无。
            ~PreparationGuard()
            {
                this->active = false;
            }
        } guard{this->annotationPreparing_};
        this->annotationPreparing_ = true;
        rendered = this->outputRenderer_->Render(*this->frozenDesktopFrame_, selection, annotations, frame, error,
                                                 this->annotationSource_.get());
    }
    if (this->overlaySession_)
        this->overlaySession_->Invalidate();
    if (!rendered || this->overlayInvalidated_ || this->shuttingDown_)
    {
        frame = {};
        return false;
    }
    return true;
}
} // namespace open_st
