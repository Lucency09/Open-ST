// 适配截图临时设置与图形预览，不拥有第二份参数或像素处理算法。
#include "capture_visual_session.h"
#include "capture_overlay_session.h"
#include <annotation_mosaic_source.h>
#include <app.h>
#include <capture_toolbar.h>
#include <log.h>
#include <selection_model.h>
#include <ui_text.h>
#include <utility>
namespace open_st
{
// 参数和几何变化统一请求预览，无变化时不主动循环绘制。
// 入参：无。返回：无；失败标记会话失效。
void App::RefreshCaptureVisualPreview() noexcept
try
{
    if (!this->captureVisual_ || !this->overlaySession_ || !this->selectionModel_ || this->overlayInvalidated_ ||
        this->shuttingDown_ || this->overlayRendering_ || this->annotationPreparing_)
        return;
    if (!this->overlaySession_->SetMaskOpacityPercent(this->captureVisual_->Mask()))
    {
        this->overlayInvalidated_ = true;
        return;
    }
    if (this->annotationSource_ &&
        (!this->annotationSource_->SetBrightnessPercent(this->captureVisual_->Brightness()) ||
         !this->annotationSource_->SetToneMappingOptions(this->captureVisual_->ToneMappingOptions())))
    {
        this->overlayInvalidated_ = true;
        return;
    }
    const SelectionSnapshot snapshot = this->selectionModel_->Snapshot();
    const std::uint64_t before = this->captureVisual_->Version();
    if (!snapshot.hasSelection)
    {
        std::wstring error;
        (void)this->captureVisual_->Request({});
        (void)this->overlaySession_->SetSelectionPreview(nullptr, error);
    }
    else if (!this->captureVisual_->Request(snapshot.rectangle))
        this->overlayInvalidated_ = true;
    if (before != this->captureVisual_->Version())
        this->overlaySession_->Invalidate();
}
catch (...)
{
    OPEN_ST_LOG_ERROR("Cannot request capture visual preview.");
    this->overlayInvalidated_ = true;
}
// 为未完成的底图保留唯一输出动作，主消息循环继续运行。
// 入参：action 为完成动作。返回：进入等待状态时为 true。
bool App::DeferCaptureOutput(std::function<void()> action)
{
    if (!this->captureVisual_)
        return false;
    this->RefreshCaptureVisualPreview();
    if (!this->captureVisual_->DeferOutput(std::move(action)))
        return false;
    this->completionBusy_ = true;
    this->UpdateCaptureGate();
    return true;
}
// 上传和马赛克准备全程受重入屏障保护，任何退出只能延后回收。
// 入参：无。返回：无，失败清空整批预览并报告。
void App::DrainCaptureVisualPreview() noexcept
try
{
    // 等待底图时也必须响应显示失效与取消，不能让完成忙状态阻止自身回收。
    if (this->captureVisual_ && this->captureVisual_->HasDeferredOutput() &&
        (this->overlayInvalidated_ || this->shuttingDown_) && !this->overlayRendering_ && !this->annotationPreparing_)
    {
        this->completionBusy_ = false;
        this->CloseOverlay();
        this->UpdateCaptureGate();
        return;
    }
    if (!this->captureVisual_ || !this->overlaySession_ || !this->selectionModel_ ||
        (this->CompletionBusy() && !this->captureVisual_->HasDeferredOutput()) || this->dialogActive_ ||
        this->settingsBusy_ || this->overlayRendering_ || this->annotationPreparing_ || this->overlayInvalidated_ ||
        this->shuttingDown_)
        return;
    bool changed = false;
    bool failed = false;
    std::wstring error;
    {
        struct PublicationGuard
        {
            bool& active;
            // 构建发布屏障，嵌套绘制或退出不释放本次图形资源。
            // 入参：value 为宿主渲染标记。返回：无。
            explicit PublicationGuard(bool& value) : active(value)
            {
                this->active = true;
            }
            // 异常退出也解除屏障，随后由外层消息回收。
            // 入参：无。返回：无。
            ~PublicationGuard()
            {
                this->active = false;
            }
        } guard{this->overlayRendering_};
        changed = this->captureVisual_->Poll(error);
        failed = !error.empty();
        if (changed && !this->overlayInvalidated_ && !this->shuttingDown_)
        {
            const AnnotationSnapshot annotations = this->AnnotationForDrawing();
            const RectI selection = this->selectionModel_->Snapshot().rectangle;
            failed = !this->annotationSource_ ||
                     !this->annotationSource_->SetBrightnessPercent(this->captureVisual_->Brightness()) ||
                     !this->annotationSource_->SetToneMappingOptions(this->captureVisual_->ToneMappingOptions());
            if (!failed)
            {
                failed = !this->annotationSource_->SetSelectionPreview(this->captureVisual_->SharedPreview()) ||
                         !this->annotationSource_->Prepare(annotations, selection, error) ||
                         !this->overlaySession_->SetSelectionPreview(this->captureVisual_->Preview(), error);
            }
        }
        if (failed)
            (void)this->overlaySession_->SetSelectionPreview(nullptr, error);
    }
    if (failed || this->overlayInvalidated_ || this->shuttingDown_)
    {
        this->overlayInvalidated_ = true;
        this->CloseOverlay();
        if (failed && !this->shuttingDown_)
            this->ShowCaptureError("capture.error.unknown");
        this->UpdateCaptureGate();
        return;
    }
    if (!changed)
        return;
    this->overlaySession_->Invalidate();
    this->RefreshCaptureToolbar();
    std::function<void()> output = this->captureVisual_->TakeDeferredOutput();
    if (output)
    {
        this->completionBusy_ = false;
        this->UpdateCaptureGate();
        output();
    }
}
catch (...)
{
    OPEN_ST_LOG_ERROR("Unexpected capture visual publication failure.");
    this->overlayInvalidated_ = true;
    this->CloseOverlay();
    this->UpdateCaptureGate();
}
// 显示持续调节本次截图的齿轮面板。
// 入参：无。返回：无；失败保留截图并报告。
void App::ShowCaptureSettings() noexcept
try
{
    if (!this->CanSubmitToolbarCommand() || !this->captureVisual_)
        return;
    if (!this->captureVisual_->Show(this->overlaySession_->ActivationWindow(), this->captureToolbar_->NativeHandle(),
                                    this->smallIcon_, this->MakeToolbarTextResolver()))
        this->annotationFailure_ = true;
}
catch (...)
{
    this->annotationFailure_ = true;
    OPEN_ST_LOG_ERROR("Cannot show capture settings panel.");
}
} // namespace open_st
