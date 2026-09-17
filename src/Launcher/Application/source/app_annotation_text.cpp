// 将截图会话上下文适配给标注交互控制器，原位输入状态由控制器唯一管理。
#include "annotation_interaction_controller.h"
#include "capture_overlay_session.h"
#include <app.h>
#include <ui_text.h>

namespace open_st
{
// 汇总跨流程忙状态，文字活动状态只从控制器读取，不保存镜像。
// 入参：无。返回：完成流程或原位文字活动时 true。
bool App::CompletionBusy() const noexcept
{
    return this->completionBusy_ || this->annotationInteraction_->TextActive();
}
// 开始文字交互，宿主只提供会话准入、通知窗口与本地化接口。
// 入参：point 为新建落点；id 非零时编辑已有文字。返回：无。
void App::BeginAnnotationText(PointI point, std::uint64_t id) noexcept
{
    if (!this->CanSubmitToolbarCommand() || !this->annotation_)
        return;
    try
    {
        AnnotationTextHost host;
        host.owner = this->overlaySession_->ActivationWindow();
        host.notificationWindow = this->messageWindow_;
        const CaptureAnnotationState* const state = this->annotation_.get();
        // 借用期间只检查会话身份和失效，不读写控制器私有状态。
        // 入参：无。返回：当前事务仍属于有效截图时 true。
        host.valid = [this, state]()
        { return this->annotation_.get() == state && !this->overlayInvalidated_ && !this->shuttingDown_; };
        // 输入窗口创建前同步更新全局准入，覆盖同步窗口消息。
        // 入参：无。返回：无。
        host.changed = [this]() { this->UpdateCaptureGate(); };
        // 产品文字由宿主查询，控制器不读取资源文件。
        // 入参：key 为文字键。返回：本地化文字。
        host.text = [](std::string_view key) { return GetUiText(key); };
        if (!this->annotationInteraction_->BeginText(*this->annotation_, this->selectionModel_->Snapshot().rectangle,
                                                     point, id, std::move(host)))
            this->annotationFailure_ = true;
    }
    catch (...)
    {
        this->annotationFailure_ = true;
    }
    this->CompleteAnnotationTextBoundary();
}
// 把遮罩点击交给输入控制器，不持有编辑缓存。
// 入参：point 为桌面物理位置。返回：无。
void App::HandleAnnotationTextClick(PointI point) noexcept
{
    if (this->selectionModel_)
        this->annotationInteraction_->ClickText(point, this->selectionModel_->Snapshot().rectangle);
}
// 在消息边界消费完成请求，安全结束后协调重绘与焦点。
// 入参：serial 为请求号；accept 为确认意图。返回：无。
void App::FinishAnnotationText(std::uint64_t serial, bool accept) noexcept
{
    if (this->annotationInteraction_->FinishText(serial, accept, this->annotationPreparing_ || this->overlayRendering_))
        this->CompleteAnnotationTextBoundary();
}
// 处理图形重入之后延后的结束和会话失效。
// 入参：无。返回：无。
void App::DrainAnnotationText() noexcept
{
    if (this->annotationInteraction_->DrainText(this->annotationPreparing_ || this->overlayRendering_))
        this->CompleteAnnotationTextBoundary();
}
// 交互状态变化后协调截图生命周期；活动输入期间不抢回焦点。
// 入参：无。返回：无。
void App::CompleteAnnotationTextBoundary() noexcept
{
    if (this->overlayInvalidated_ || this->shuttingDown_)
        this->CloseOverlay();
    else if (this->overlaySession_)
    {
        this->overlaySession_->Invalidate();
        if (!this->annotationInteraction_->TextActive())
            this->overlaySession_->RestoreFocus();
    }
    this->UpdateCaptureGate();
}
} // namespace open_st
