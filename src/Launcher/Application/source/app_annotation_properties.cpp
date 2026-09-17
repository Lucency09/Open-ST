// 实现右键元素命中、请求代次和单元素属性事务，保留工具默认样式及 Esc 行为。
#include "annotation_interaction_controller.h"
#include "annotation_style_dialog.h"
#include "capture_annotation_state.h"
#include "capture_command_gate.h"
#include "capture_overlay_session.h"
#include <annotation_document.h>
#include <app.h>
#include <capture_toolbar.h>
#include <ui_text.h>

namespace open_st
{
// 查询右键交互所需的当前会话值，不把 App 交给控制器。
// 入参：无。返回：只在本次同步调用中借用的状态与准入。
AnnotationPropertyContext App::AnnotationPropertyContextForInput() const noexcept
{
    return {this->annotation_.get(), this->selectionModel_ ? this->selectionModel_->Snapshot().rectangle : RectI{},
            this->toolbarGate_ ? this->toolbarGate_->Token() : 0U,
            this->CanSubmitToolbarCommand() && this->toolbarGate_ && !this->toolbarGate_->Pending()};
}
// 将右键点击交给独占请求状态的控制器。
// 入参：window、point 为输入窗口和物理坐标。返回：无。
void App::BeginAnnotationPropertyClick(HWND window, PointI point) noexcept
try
{
    if (!this->annotationInteraction_->BeginProperty(window, point,
                                                     // 命中完成后重新读取上下文，防止图形调用重入使请求失效。
                                                     // 入参：无。返回：当前输入准入。
                                                     [this]() { return this->AnnotationPropertyContextForInput(); }))
        this->annotationFailure_ = true;
}
catch (...)
{
    this->annotationInteraction_->LeaveProperty();
    this->annotationFailure_ = true;
}
// 转发右键移动，容差规则由控制器管理。
// 入参：point 为桌面物理坐标。返回：无。
void App::UpdateAnnotationPropertyClick(PointI point) noexcept
{
    this->annotationInteraction_->MoveProperty(point);
}
// 排队完成后同步工具栏显示，宿主不持有第二份请求。
// 入参：point 为抬起位置。返回：无。
void App::EndAnnotationPropertyClick(PointI point) noexcept
{
    if (!this->annotationInteraction_->EndProperty(point, this->AnnotationPropertyContextForInput(),
                                                   this->messageWindow_))
    {
        this->annotationFailure_ = true;
        this->RefreshCaptureToolbar();
    }
    else if (this->annotationInteraction_->PendingProperty() && this->captureToolbar_)
        this->captureToolbar_->SetBusy(true);
}
// 撤销请求，保留控制器内部的跨会话序号。
// 入参：无。返回：无。
void App::CancelAnnotationPropertyRequest() noexcept
{
    this->annotationInteraction_->CancelProperties();
}
// 消费身份匹配的请求后编排属性表单；旧消息不影响新请求。
// 入参：serial 为窗口消息携带的身份。返回：无。
void App::DispatchAnnotationProperties(std::uint64_t serial) noexcept
{
    const std::optional<AnnotationPropertyRequest> request = this->annotationInteraction_->TakeProperty(serial);
    if (!request)
    {
        if (!this->annotationInteraction_->PendingProperty())
            this->RefreshCaptureToolbar();
        return;
    }
    const AnnotationPropertyContext context = this->AnnotationPropertyContextForInput();
    if (context.ready && context.state && request->token == context.token &&
        request->revision == context.state->Revision())
        this->EditAnnotationElement(request->id, request->revision);
    this->RefreshCaptureToolbar();
}
// 在固定对象事务内复用属性表单，正文编辑等属性确认后再启动。
// 入参：id为稳定ID，revision为固定修订。返回：无，失败保留原文档。
void App::EditAnnotationElement(std::uint64_t id, std::uint64_t revision) noexcept
{
    if (!this->CanSubmitToolbarCommand() || !this->annotation_ || this->annotation_->Revision() != revision)
        return;
    const AnnotationSnapshot document = this->annotation_->Committed();
    const AnnotationObject* const found = FindAnnotation(document, id);
    const RectI selection = this->selectionModel_->Snapshot().rectangle;
    if (!found || !this->annotation_->BeginObjectPreview(id, revision, selection))
        return;
    const AnnotationKind kind = found->kind;
    AnnotationProperties candidate = PropertiesOf(*found);
    const CaptureAnnotationState* const state = this->annotation_.get();
    const HWND owner = this->overlaySession_->ActivationWindow();
    this->completionBusy_ = true;
    this->UpdateCaptureGate();
    bool editText = false;
    AnnotationStyleResult result = AnnotationStyleResult::Failed;
    try
    {
        std::wstring error;
        // 只做会话准入与请求重绘，所有属性变更由编辑状态执行。
        // 入参：properties为候选属性。返回：完整预览成功时true。
        const auto apply = [this, state, revision](const AnnotationProperties& properties)
        {
            if (!this->CompletionBusy() || this->overlayInvalidated_ || this->shuttingDown_ || !this->overlaySession_ ||
                this->annotation_.get() != state || this->annotation_->Revision() != revision ||
                !this->annotation_->UpdateObjectPreview(properties))
                return false;
            this->overlaySession_->Invalidate();
            return true;
        };
        // 元素属性与默认样式使用不同标题，其余文字仍共用本地化。
        // 入参：key为表单文本键。返回：本地化文字。
        const auto text = [](std::string_view key)
        { return GetUiText(key == "annotation.style.title" ? "annotation.element.properties" : key); };
        if (kind == AnnotationKind::Mosaic)
        {
            unsigned size = *candidate.blockSize;
            result = ShowAnnotationChoiceDialog(owner, size, ANNOTATION_MOSAIC_BLOCK_SIZES,
                                                "annotation.element.properties", "annotation.mosaic.block", text, error,
                                                // 马赛克只修改块大小，不引入颜色或弱化透明度。
                                                // 入参：value为块边长。返回：候选来源完整准备时true。
                                                [&apply, candidate](unsigned value) mutable
                                                {
                                                    candidate.blockSize = value;
                                                    return apply(candidate);
                                                });
            candidate.blockSize = size;
        }
        else if (kind == AnnotationKind::Text)
        {
            AnnotationTextStyle settings{static_cast<unsigned>(*candidate.fontSize), true, false};
            result =
                ShowAnnotationTextStyleDialog(owner, candidate.style, settings, text, error,
                                              // 字号与颜色透明度作为同一次对象属性预览。
                                              // 入参：style和size为完整候选。返回：预览成功时true。
                                              [&apply, candidate](const AnnotationStyle& style, unsigned size) mutable
                                              {
                                                  candidate.style = style;
                                                  candidate.fontSize = size;
                                                  return apply(candidate);
                                              });
            candidate.fontSize = settings.fontSize;
            editText = settings.editRequested;
        }
        else
        {
            const bool width = kind != AnnotationKind::FilledRectangle && kind != AnnotationKind::RoundedRectangle;
            result = ShowAnnotationStyleDialog(owner, candidate.style, width, text, error,
                                               // 普通几何复用同一对象事务，不创建第二套样式历史。
                                               // 入参：style为候选。返回：预览成功时true。
                                               [&apply, candidate](const AnnotationStyle& style) mutable
                                               {
                                                   candidate.style = style;
                                                   return apply(candidate);
                                               });
        }
        if (result == AnnotationStyleResult::Accepted && !this->overlayInvalidated_ && !this->shuttingDown_ &&
            this->annotation_.get() == state && this->annotation_->Revision() == revision)
        {
            const AnnotationCommitResult changed = this->annotation_->UpdateObjectPreview(candidate)
                                                       ? this->annotation_->EndObjectPreview(selection, true)
                                                       : AnnotationCommitResult::Failed;
            if (changed == AnnotationCommitResult::Failed)
            {
                this->annotationFailure_ = true;
                editText = false;
            }
        }
        else
            editText = false;
    }
    catch (...)
    {
        result = AnnotationStyleResult::Failed;
        editText = false;
    }
    if (this->annotation_.get() == state)
        (void)this->annotation_->EndObjectPreview(selection, false);
    if (result == AnnotationStyleResult::Failed)
        this->annotationFailure_ = true;
    this->completionBusy_ = false;
    if (this->overlayInvalidated_ || this->shuttingDown_)
        this->CloseOverlay();
    else if (this->overlaySession_)
    {
        this->overlaySession_->Invalidate();
        this->overlaySession_->RestoreFocus();
    }
    this->UpdateCaptureGate();
    if (editText)
        this->BeginAnnotationText({}, id);
}
} // namespace open_st
