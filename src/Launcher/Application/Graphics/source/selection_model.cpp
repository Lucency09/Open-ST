#include <selection_model.h>

#include <algorithm>
#include <cstdint>
#include <limits>

namespace
{
constexpr int HANDLE_HIT_HALF_SIZE = 6;

// 比较两个整数矩形的四条边是否完全一致。
bool RectanglesEqual(const open_st::RectI& left, const open_st::RectI& right) noexcept
{
    return left.left == right.left && left.top == right.top && left.right == right.right &&
           left.bottom == right.bottom;
}

// 把任意方向的两个端点转换为非负宽高的半开矩形。
open_st::RectI Normalize(open_st::PointI first, open_st::PointI second) noexcept
{
    return {std::min(first.x, second.x), std::min(first.y, second.y), std::max(first.x, second.x),
            std::max(first.y, second.y)};
}

// 判断控制点是否位于选区四角，用于重叠命中时提高角点优先级。
bool IsCorner(open_st::SelectionHandle handle) noexcept
{
    return handle == open_st::SelectionHandle::TopLeft || handle == open_st::SelectionHandle::TopRight ||
           handle == open_st::SelectionHandle::BottomRight || handle == open_st::SelectionHandle::BottomLeft;
}

// 把控制点映射为水平移动方向：左为 -1、无水平分量为 0、右为 1。
int HorizontalDirection(open_st::SelectionHandle handle) noexcept
{
    switch (handle)
    {
    case open_st::SelectionHandle::TopLeft:
    case open_st::SelectionHandle::BottomLeft:
    case open_st::SelectionHandle::Left:
        return -1;
    case open_st::SelectionHandle::TopRight:
    case open_st::SelectionHandle::Right:
    case open_st::SelectionHandle::BottomRight:
        return 1;
    case open_st::SelectionHandle::None:
    case open_st::SelectionHandle::Top:
    case open_st::SelectionHandle::Bottom:
        return 0;
    }
    return 0;
}

// 把控制点映射为垂直移动方向：上为 -1、无垂直分量为 0、下为 1。
int VerticalDirection(open_st::SelectionHandle handle) noexcept
{
    switch (handle)
    {
    case open_st::SelectionHandle::TopLeft:
    case open_st::SelectionHandle::Top:
    case open_st::SelectionHandle::TopRight:
        return -1;
    case open_st::SelectionHandle::BottomRight:
    case open_st::SelectionHandle::Bottom:
    case open_st::SelectionHandle::BottomLeft:
        return 1;
    case open_st::SelectionHandle::None:
    case open_st::SelectionHandle::Right:
    case open_st::SelectionHandle::Left:
        return 0;
    }
    return 0;
}

// 根据水平和垂直方向重新组合控制点，供跨越固定对边后翻转使用。
open_st::SelectionHandle HandleFromDirections(int horizontal, int vertical) noexcept
{
    if (horizontal < 0 && vertical < 0)
    {
        return open_st::SelectionHandle::TopLeft;
    }
    if (horizontal == 0 && vertical < 0)
    {
        return open_st::SelectionHandle::Top;
    }
    if (horizontal > 0 && vertical < 0)
    {
        return open_st::SelectionHandle::TopRight;
    }
    if (horizontal > 0 && vertical == 0)
    {
        return open_st::SelectionHandle::Right;
    }
    if (horizontal > 0 && vertical > 0)
    {
        return open_st::SelectionHandle::BottomRight;
    }
    if (horizontal == 0 && vertical > 0)
    {
        return open_st::SelectionHandle::Bottom;
    }
    if (horizontal < 0 && vertical > 0)
    {
        return open_st::SelectionHandle::BottomLeft;
    }
    if (horizontal < 0 && vertical == 0)
    {
        return open_st::SelectionHandle::Left;
    }
    return open_st::SelectionHandle::None;
}
} // namespace

namespace open_st
{
// 设置虚拟桌面边界；若边界变化发生在拖动中，先取消拖动再收敛已有选区。
void SelectionModel::SetBounds(RectI bounds) noexcept
{
    if (this->phase_ == SelectionPhase::Dragging)
    {
        (void)this->CancelInteraction();
    }
    if (bounds.IsEmpty())
    {
        this->Reset();
        this->bounds_ = {};
        return;
    }

    this->bounds_ = bounds;
    this->FitSelectionToBounds();
}

// 清空选区和交互上下文，同时保留 bounds_ 供同一截图会话再次创建选区。
void SelectionModel::Reset() noexcept
{
    this->rectangle_ = {};
    this->operationStartRectangle_ = {};
    this->dragOrigin_ = {};
    this->resizeFixedX_ = 0;
    this->resizeFixedY_ = 0;
    this->resizesHorizontally_ = false;
    this->resizesVertically_ = false;
    this->phase_ = SelectionPhase::Unselected;
    this->operation_ = SelectionOperation::None;
    this->activeHandle_ = SelectionHandle::None;
}

// 返回当前对外状态阶段。
SelectionPhase SelectionModel::Phase() const noexcept
{
    return this->phase_;
}

// 返回拖动阶段的内部操作类型。
SelectionOperation SelectionModel::Operation() const noexcept
{
    return this->operation_;
}

// 通过半开矩形是否非空判断当前是否存在有效选区。
bool SelectionModel::HasSelection() const noexcept
{
    return !this->rectangle_.IsEmpty();
}

// 返回缩放中随鼠标移动的控制点，供光标反馈使用。
SelectionHandle SelectionModel::ActiveHandle() const noexcept
{
    return this->activeHandle_;
}

// 复制当前矩形、状态和控制点，形成不借用模型内存的渲染快照。
SelectionSnapshot SelectionModel::Snapshot() const noexcept
{
    SelectionSnapshot snapshot{};
    snapshot.phase = this->phase_;
    snapshot.operation = this->operation_;
    snapshot.rectangle = this->rectangle_;
    snapshot.handles = this->Handles();
    snapshot.hasSelection = this->HasSelection();
    snapshot.showHandles = snapshot.hasSelection &&
                           (this->phase_ == SelectionPhase::Selected ||
                            this->operation_ == SelectionOperation::Moving ||
                            this->operation_ == SelectionOperation::Resizing);
    return snapshot;
}

// 根据当前阶段和命中优先级开始创建、缩放或整体移动操作。
bool SelectionModel::Begin(PointI point) noexcept
{
    if (this->bounds_.IsEmpty() || this->phase_ == SelectionPhase::Dragging)
    {
        return false;
    }

    this->operationStartRectangle_ = this->rectangle_;
    this->activeHandle_ = SelectionHandle::None;
    this->resizesHorizontally_ = false;
    this->resizesVertically_ = false;

    if (this->phase_ == SelectionPhase::Unselected)
    {
        point = this->ClampPoint(point);
        this->dragOrigin_ = point;
        this->rectangle_ = {point.x, point.y, point.x, point.y};
        this->operation_ = SelectionOperation::Creating;
    }
    else
    {
        const SelectionHandle handle = this->HitTestHandle(point);
        if (handle != SelectionHandle::None)
        {
            this->dragOrigin_ = point;
            this->activeHandle_ = handle;
            this->operation_ = SelectionOperation::Resizing;
            this->resizesHorizontally_ = HorizontalDirection(handle) != 0;
            this->resizesVertically_ = VerticalDirection(handle) != 0;
            if (HorizontalDirection(handle) < 0)
            {
                this->resizeFixedX_ = this->rectangle_.right;
            }
            else if (HorizontalDirection(handle) > 0)
            {
                this->resizeFixedX_ = this->rectangle_.left;
            }
            if (VerticalDirection(handle) < 0)
            {
                this->resizeFixedY_ = this->rectangle_.bottom;
            }
            else if (VerticalDirection(handle) > 0)
            {
                this->resizeFixedY_ = this->rectangle_.top;
            }
        }
        else if (this->Contains(point))
        {
            this->dragOrigin_ = point;
            this->operation_ = SelectionOperation::Moving;
        }
        else
        {
            this->operationStartRectangle_ = {};
            return false;
        }
    }

    this->phase_ = SelectionPhase::Dragging;
    return true;
}

// 把最新鼠标点分派给当前拖动操作，并报告矩形是否实际变化。
bool SelectionModel::Update(PointI point) noexcept
{
    if (this->phase_ != SelectionPhase::Dragging)
    {
        return false;
    }

    const RectI previous = this->rectangle_;
    point = this->ClampPoint(point);
    switch (this->operation_)
    {
    case SelectionOperation::Creating:
        this->UpdateCreating(point);
        break;
    case SelectionOperation::Moving:
        this->UpdateMoving(point);
        break;
    case SelectionOperation::Resizing:
        this->UpdateResizing(point);
        break;
    case SelectionOperation::None:
        return false;
    }
    return !RectanglesEqual(previous, this->rectangle_);
}

// 应用最终鼠标点并提交拖动；不接受零面积结果，按操作类型清空或恢复。
bool SelectionModel::End(PointI point) noexcept
{
    if (this->phase_ != SelectionPhase::Dragging)
    {
        return false;
    }

    (void)this->Update(point);
    if (this->rectangle_.IsEmpty())
    {
        if (this->operation_ == SelectionOperation::Creating)
        {
            this->Reset();
        }
        else
        {
            this->rectangle_ = this->operationStartRectangle_;
            this->FinishInteraction();
        }
        return true;
    }

    this->FinishInteraction();
    return true;
}

// 取消当前拖动：创建时清空，移动或缩放时恢复操作开始前的矩形。
bool SelectionModel::CancelInteraction() noexcept
{
    if (this->phase_ != SelectionPhase::Dragging)
    {
        return false;
    }

    if (this->operation_ == SelectionOperation::Creating)
    {
        this->Reset();
        return true;
    }

    this->rectangle_ = this->operationStartRectangle_;
    this->FinishInteraction();
    return true;
}

// 按左上包含、右下排除的半开规则判断点是否位于选区内部。
bool SelectionModel::Contains(PointI point) const noexcept
{
    return !this->rectangle_.IsEmpty() && point.x >= this->rectangle_.left && point.x < this->rectangle_.right &&
           point.y >= this->rectangle_.top && point.y < this->rectangle_.bottom;
}

// 在八个 12×12 命中区中选择最近控制点，等距重叠时优先返回角点。
SelectionHandle SelectionModel::HitTestHandle(PointI point) const noexcept
{
    if (this->phase_ != SelectionPhase::Selected || this->rectangle_.IsEmpty())
    {
        return SelectionHandle::None;
    }

    std::int64_t nearestDistance = std::numeric_limits<std::int64_t>::max();
    SelectionHandle nearest = SelectionHandle::None;
    for (const SelectionHandlePosition& position : this->Handles())
    {
        const std::int64_t dx = static_cast<std::int64_t>(point.x) - position.center.x;
        const std::int64_t dy = static_cast<std::int64_t>(point.y) - position.center.y;
        if (dx < -HANDLE_HIT_HALF_SIZE || dx >= HANDLE_HIT_HALF_SIZE || dy < -HANDLE_HIT_HALF_SIZE ||
            dy >= HANDLE_HIT_HALF_SIZE)
        {
            continue;
        }
        const std::int64_t distance = dx * dx + dy * dy;
        if (distance < nearestDistance ||
            (distance == nearestDistance && IsCorner(position.handle) && !IsCorner(nearest)))
        {
            nearestDistance = distance;
            nearest = position.handle;
        }
    }
    return nearest;
}

// 根据当前矩形计算四角和四边中点的八个控制点中心。
std::array<SelectionHandlePosition, 8> SelectionModel::Handles() const noexcept
{
    const int horizontalCenter = this->rectangle_.left + this->rectangle_.Width() / 2;
    const int verticalCenter = this->rectangle_.top + this->rectangle_.Height() / 2;
    return {{{SelectionHandle::TopLeft, {this->rectangle_.left, this->rectangle_.top}},
             {SelectionHandle::Top, {horizontalCenter, this->rectangle_.top}},
             {SelectionHandle::TopRight, {this->rectangle_.right, this->rectangle_.top}},
             {SelectionHandle::Right, {this->rectangle_.right, verticalCenter}},
             {SelectionHandle::BottomRight, {this->rectangle_.right, this->rectangle_.bottom}},
             {SelectionHandle::Bottom, {horizontalCenter, this->rectangle_.bottom}},
             {SelectionHandle::BottomLeft, {this->rectangle_.left, this->rectangle_.bottom}},
             {SelectionHandle::Left, {this->rectangle_.left, verticalCenter}}}};
}

// 将鼠标点钳制到桌面边界坐标，允许 right/bottom 作为 half-open 端点。
PointI SelectionModel::ClampPoint(PointI point) const noexcept
{
    point.x = std::clamp(point.x, this->bounds_.left, this->bounds_.right);
    point.y = std::clamp(point.y, this->bounds_.top, this->bounds_.bottom);
    return point;
}

// 边界变化后尽量平移保留原尺寸；无法容纳时才把对应维度收缩到完整边界。
void SelectionModel::FitSelectionToBounds() noexcept
{
    if (this->rectangle_.IsEmpty())
    {
        return;
    }

    const int width = this->rectangle_.Width();
    const int height = this->rectangle_.Height();
    if (width >= this->bounds_.Width())
    {
        this->rectangle_.left = this->bounds_.left;
        this->rectangle_.right = this->bounds_.right;
    }
    else
    {
        this->rectangle_.left =
            std::clamp(this->rectangle_.left, this->bounds_.left, this->bounds_.right - width);
        this->rectangle_.right = this->rectangle_.left + width;
    }
    if (height >= this->bounds_.Height())
    {
        this->rectangle_.top = this->bounds_.top;
        this->rectangle_.bottom = this->bounds_.bottom;
    }
    else
    {
        this->rectangle_.top =
            std::clamp(this->rectangle_.top, this->bounds_.top, this->bounds_.bottom - height);
        this->rectangle_.bottom = this->rectangle_.top + height;
    }
}

// 清理一次拖动的临时字段，并把非空矩形收敛为稳定 Selected 状态。
void SelectionModel::FinishInteraction() noexcept
{
    this->operationStartRectangle_ = {};
    this->phase_ = SelectionPhase::Selected;
    this->operation_ = SelectionOperation::None;
    this->activeHandle_ = SelectionHandle::None;
    this->resizesHorizontally_ = false;
    this->resizesVertically_ = false;
}

// 从固定创建起点生成规范化矩形，并让真实可达的右下边缘覆盖 exclusive 边界。
void SelectionModel::UpdateCreating(PointI point) noexcept
{
    PointI origin = this->dragOrigin_;
    // 光标最右/最下只能到达 half-open 边界前一像素；向该方向拖动时吸附到 exclusive 边界，
    // 否则用户无法选中虚拟桌面的最后一列或最后一行。
    if (point.x > origin.x && point.x == this->bounds_.right - 1)
    {
        point.x = this->bounds_.right;
    }
    else if (point.x < origin.x && origin.x == this->bounds_.right - 1)
    {
        origin.x = this->bounds_.right;
    }
    if (point.y > origin.y && point.y == this->bounds_.bottom - 1)
    {
        point.y = this->bounds_.bottom;
    }
    else if (point.y < origin.y && origin.y == this->bounds_.bottom - 1)
    {
        origin.y = this->bounds_.bottom;
    }
    this->rectangle_ = Normalize(origin, point);
}

// 按鼠标相对起点的位移整体平移原矩形，保持宽高并限制在桌面内。
void SelectionModel::UpdateMoving(PointI point) noexcept
{
    const std::int64_t width = this->operationStartRectangle_.Width();
    const std::int64_t height = this->operationStartRectangle_.Height();
    const std::int64_t desiredLeft = static_cast<std::int64_t>(this->operationStartRectangle_.left) + point.x -
                                     this->dragOrigin_.x;
    const std::int64_t desiredTop = static_cast<std::int64_t>(this->operationStartRectangle_.top) + point.y -
                                    this->dragOrigin_.y;
    const std::int64_t minimumLeft = this->bounds_.left;
    const std::int64_t maximumLeft = static_cast<std::int64_t>(this->bounds_.right) - width;
    const std::int64_t minimumTop = this->bounds_.top;
    const std::int64_t maximumTop = static_cast<std::int64_t>(this->bounds_.bottom) - height;
    const int left = static_cast<int>(std::clamp(desiredLeft, minimumLeft, maximumLeft));
    const int top = static_cast<int>(std::clamp(desiredTop, minimumTop, maximumTop));
    this->rectangle_ = {left, top, static_cast<int>(left + width), static_cast<int>(top + height)};
}

// 移动控制点负责的边、保持对边固定，并在跨边后更新控制点方向。
void SelectionModel::UpdateResizing(PointI point) noexcept
{
    this->rectangle_ = this->operationStartRectangle_;
    int horizontalDirection = HorizontalDirection(this->activeHandle_);
    int verticalDirection = VerticalDirection(this->activeHandle_);
    if (this->resizesHorizontally_)
    {
        if (point.x > this->resizeFixedX_ && point.x == this->bounds_.right - 1)
        {
            point.x = this->bounds_.right;
        }
        this->rectangle_.left = std::min(point.x, this->resizeFixedX_);
        this->rectangle_.right = std::max(point.x, this->resizeFixedX_);
        if (point.x < this->resizeFixedX_)
        {
            horizontalDirection = -1;
        }
        else if (point.x > this->resizeFixedX_)
        {
            horizontalDirection = 1;
        }
    }
    if (this->resizesVertically_)
    {
        if (point.y > this->resizeFixedY_ && point.y == this->bounds_.bottom - 1)
        {
            point.y = this->bounds_.bottom;
        }
        this->rectangle_.top = std::min(point.y, this->resizeFixedY_);
        this->rectangle_.bottom = std::max(point.y, this->resizeFixedY_);
        if (point.y < this->resizeFixedY_)
        {
            verticalDirection = -1;
        }
        else if (point.y > this->resizeFixedY_)
        {
            verticalDirection = 1;
        }
    }
    this->activeHandle_ = HandleFromDirections(horizontalDirection, verticalDirection);
}
} // namespace open_st
