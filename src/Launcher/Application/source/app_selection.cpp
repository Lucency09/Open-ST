// 协调窗口预选与单击／拖动输入，保持绘制候选和正式选区的输出资格分离。

#include "capture_selection_input.h"
#include "window_selection_snapshot.h"

#include <algorithm>
#include <app.h>
#include <frozen_desktop_frame.h>
#include <log.h>
#include <selection_model.h>
#include <vector>

namespace open_st
{
// 采集冻结桌面的窗口范围，并在遮罩首帧之前初始化候选。
// 入参：无；借用本次冻结帧，不访问目标应用内容。
// 返回：无返回值；采集失败仅退回自由框选，分配异常交由 StartCapture 保护处理。
void App::InitializeWindowSelection()
{
    this->windowCandidate_.reset();
    this->selectionInput_ = std::make_unique<CaptureSelectionInput>();
    this->windowSelection_ = std::make_unique<WindowSelectionSnapshot>();
    std::vector<RectI> outputs;
    for (const CapturedOutputPlane& plane : this->frozenDesktopFrame_->Outputs())
        outputs.push_back(plane.Bounds());
    if (!this->windowSelection_->Capture(this->frozenDesktopFrame_->Bounds(), outputs))
        OPEN_ST_LOG_WARNING("Window preselection unavailable for this capture. reason=",
                            static_cast<int>(this->windowSelection_->Failure()));
    this->RefreshWindowCandidate();
}

// 生成独立的绘制视图，正式模型始终保留原来的选区资格。
// 入参：无。
// 返回：正式模型快照，或只带边框和外部遮罩的候选副本。
SelectionSnapshot App::SelectionForDrawing() const noexcept
{
    SelectionSnapshot snapshot = this->selectionModel_ ? this->selectionModel_->Snapshot() : SelectionSnapshot{};
    if (snapshot.phase == SelectionPhase::Unselected && this->windowCandidate_)
    {
        const RECT rectangle = *this->windowCandidate_;
        snapshot.rectangle = {rectangle.left, rectangle.top, rectangle.right, rectangle.bottom};
        snapshot.hasSelection = true;
        snapshot.showHandles = false;
    }
    return snapshot;
}

// 仅在未确认且没有按下锁定时查询本地几何候选。
// 入参：point 为物理屏幕坐标。
// 返回：候选变化时 true，不变化时 false。
bool App::UpdateWindowCandidate(PointI point) noexcept
{
    if (!this->selectionModel_ || this->selectionModel_->Phase() != SelectionPhase::Unselected ||
        (this->selectionInput_ && this->selectionInput_->Pending()))
        return false;
    const std::optional<RectI> candidate =
        this->windowSelection_ ? this->windowSelection_->Candidate(point) : std::nullopt;
    if (!candidate)
    {
        const bool changed = this->windowCandidate_.has_value();
        this->windowCandidate_.reset();
        return changed;
    }
    const RECT rectangle{candidate->left, candidate->top, candidate->right, candidate->bottom};
    const bool changed = !this->windowCandidate_ || !EqualRect(&*this->windowCandidate_, &rectangle);
    this->windowCandidate_ = rectangle;
    return changed;
}

// 在取消或会话初始时从当前鼠标位置恢复候选。
// 入参：无。
// 返回：无返回值；鼠标读取失败时不继续显示旧候选。
void App::RefreshWindowCandidate() noexcept
{
    POINT point{};
    if (GetCursorPos(&point))
        (void)this->UpdateWindowCandidate({point.x, point.y});
    else
        this->windowCandidate_.reset();
}

// 根据正式选区或当前候选开始交互，获取捕获仍由窗口消息入口负责。
// 入参：window 为按下所在遮罩；point 为物理坐标。
// 返回：已开始待判定或模型交互时 true。
bool App::BeginSelectionInput(HWND window, PointI point) noexcept
{
    if (!this->selectionModel_ || this->HasSelectionInteraction())
        return false;
    (void)this->UpdateWindowCandidate(point);
    if (this->selectionModel_->Phase() == SelectionPhase::Unselected && this->windowCandidate_ && this->selectionInput_)
    {
        const RECT candidate = *this->windowCandidate_;
        const UINT windowDpi = GetDpiForWindow(window);
        const UINT dpi = windowDpi != 0 ? windowDpi : USER_DEFAULT_SCREEN_DPI;
        return this->selectionInput_->Begin(point, {candidate.left, candidate.top, candidate.right, candidate.bottom},
                                            std::max(1, GetSystemMetricsForDpi(SM_CXDRAG, dpi)),
                                            std::max(1, GetSystemMetricsForDpi(SM_CYDRAG, dpi)));
    }
    this->windowCandidate_.reset();
    return this->selectionModel_->Begin(point);
}

// 首次超过容差时从原按下点创建自由选区，其余输入沿用模型。
// 入参：point 为物理坐标。
// 返回：需要重绘时 true。
bool App::UpdateSelectionInput(PointI point) noexcept
{
    if (!this->selectionModel_)
        return false;
    if (this->selectionInput_ && this->selectionInput_->Pending())
    {
        const PointI origin = this->selectionInput_->Origin();
        if (!this->selectionInput_->Move(point))
            return false;
        this->selectionInput_->Cancel();
        this->windowCandidate_.reset();
        if (this->selectionModel_->Begin(origin))
            (void)this->selectionModel_->Update(point);
        return true;
    }
    if (this->selectionModel_->Phase() == SelectionPhase::Dragging)
        return this->selectionModel_->Update(point);
    return this->UpdateWindowCandidate(point);
}

// 消费待判定结果或结束现有拖动，先完成状态转换再允许释放捕获。
// 入参：point 为物理抬起坐标。
// 返回：无返回值；没有收到移动消息时仍根据抬起位置区分点击与拖动。
void App::EndSelectionInput(PointI point) noexcept
{
    if (!this->selectionModel_)
        return;
    if (this->selectionInput_ && this->selectionInput_->Pending())
    {
        const CaptureSelectionFinish finish = this->selectionInput_->Finish(point);
        this->windowCandidate_.reset();
        if (finish.action == CaptureSelectionAction::SelectCandidate)
            (void)this->selectionModel_->SelectRectangle(finish.rectangle);
        else if (finish.action == CaptureSelectionAction::BeginDrag && this->selectionModel_->Begin(finish.origin))
            (void)this->selectionModel_->End(point);
    }
    else if (this->selectionModel_->Phase() == SelectionPhase::Dragging)
        (void)this->selectionModel_->End(point);
    (void)this->UpdateWindowCandidate(point);
}

// 同时检查候选点击和原有创建／移动／缩放交互。
// 入参：无。
// 返回：至少一种交互进行中时 true。
bool App::HasSelectionInteraction() const noexcept
{
    return (this->selectionInput_ && this->selectionInput_->Pending()) ||
           (this->selectionModel_ && this->selectionModel_->Phase() == SelectionPhase::Dragging);
}

// 先清空候选点击，再撤销模型交互并恢复未选中阶段的候选。
// 入参：无。
// 返回：处理了交互时 true，否则 false。
bool App::CancelSelectionInput() noexcept
{
    if (!this->HasSelectionInteraction())
        return false;
    if (this->selectionInput_)
        this->selectionInput_->Cancel();
    if (this->selectionModel_)
        (void)this->selectionModel_->CancelInteraction();
    this->RefreshWindowCandidate();
    return true;
}
} // namespace open_st
