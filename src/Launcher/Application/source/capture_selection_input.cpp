// 文件职责：实现锁定窗口候选、半开拖动容差及单次抬起决策，不查询系统 DPI 或捕获鼠标。

#include "capture_selection_input.h"

#include <cstdint>

namespace open_st
{
// 锁定按下时的窗口候选及物理像素拖动容差，开始一次待判定交互。
// 入参：origin：按下点；candidate：非空半开候选；dragWidth、dragHeight：正数容差全宽和全高。
// 返回：成功进入待判定状态时为 true；参数无效或已有待判定交互时为 false 且保持原状态。
bool CaptureSelectionInput::Begin(PointI origin, RectI candidate, int dragWidth, int dragHeight) noexcept
{
    if (this->pending_ || candidate.IsEmpty() || dragWidth <= 0 || dragHeight <= 0)
    {
        return false;
    }
    this->origin_ = origin;
    this->candidate_ = candidate;
    this->dragWidth_ = dragWidth;
    this->dragHeight_ = dragHeight;
    this->pending_ = true;
    return true;
}

// 查询是否仍在等待单击或拖动判定。
// 入参：无。
// 返回：尚未越过容差或结束交互时为 true，否则为 false。
bool CaptureSelectionInput::Pending() const noexcept
{
    return this->pending_;
}

// 查询最后一次成功按下的物理像素起点，供越界后从原点创建选区。
// 入参：无。
// 返回：起点值副本；Move 切换为拖动后仍保留，Cancel 或 Finish 后清零。
PointI CaptureSelectionInput::Origin() const noexcept
{
    return this->origin_;
}

// 查询按下时锁定的候选范围，供待判定期间维持预选显示。
// 入参：无。
// 返回：候选半开矩形副本；取消或结束后为空。
RectI CaptureSelectionInput::Candidate() const noexcept
{
    return this->candidate_;
}

// 检测最新移动是否首次离开按下位置的半开容差矩形。
// 入参：point：最新虚拟桌面物理像素位置。
// 返回：首次越界时为 true 并结束待判定；其余情况为 false，后续拖动由调用方模型处理。
bool CaptureSelectionInput::Move(PointI point) noexcept
{
    if (!this->pending_ || this->WithinTolerance(point))
    {
        return false;
    }
    this->pending_ = false;
    return true;
}

// 以抬起位置完成判定，覆盖没有收到移动消息就越界的情况。
// 入参：point：抬起时的虚拟桌面物理像素位置。
// 返回：容差内为 SelectCandidate，容差外为 BeginDrag；无待判定时为 None，随后清空上下文。
CaptureSelectionFinish CaptureSelectionInput::Finish(PointI point) noexcept
{
    CaptureSelectionFinish result{};
    if (this->pending_)
    {
        result.action =
            this->WithinTolerance(point) ? CaptureSelectionAction::SelectCandidate : CaptureSelectionAction::BeginDrag;
        result.origin = this->origin_;
        result.rectangle = this->candidate_;
    }
    this->Cancel();
    return result;
}

// 清空待判定及锁定上下文，供取消、失捕和会话回收统一使用。
// 入参：无。
// 返回：无返回值；后续移动与抬起不再产生操作。
void CaptureSelectionInput::Cancel() noexcept
{
    this->pending_ = false;
    this->origin_ = {};
    this->candidate_ = {};
    this->dragWidth_ = 0;
    this->dragHeight_ = 0;
}

// 使用宽整数判断点是否落在起点周围的半开容差内，避免跨屏和极值坐标溢出。
// 入参：point：待判断的物理像素点。
// 返回：点位于左上包含、右下排除的容差内时为 true，否则为 false。
bool CaptureSelectionInput::WithinTolerance(PointI point) const noexcept
{
    const std::int64_t left = static_cast<std::int64_t>(this->origin_.x) - this->dragWidth_ / 2;
    const std::int64_t top = static_cast<std::int64_t>(this->origin_.y) - this->dragHeight_ / 2;
    return point.x >= left && point.x < left + this->dragWidth_ && point.y >= top && point.y < top + this->dragHeight_;
}
} // namespace open_st
