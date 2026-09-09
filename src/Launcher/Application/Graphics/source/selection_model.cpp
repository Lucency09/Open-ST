// 文件职责：实现选区创建、命中、拖动、八向缩放和取消回退，处理边界约束及控制点翻转。

#include <selection_model.h>

#include <algorithm>
#include <cstdint>
#include <limits>

namespace
{
constexpr int HANDLE_HIT_HALF_SIZE = 6;

// 检查选区四条边界是否改变，用于决定是否需要刷新。
// 入参：left、right：待比较的虚拟桌面物理像素半开矩形。
// 返回：四边均相等时为 true，否则为 false。
bool RectanglesEqual(const open_st::RectI& left, const open_st::RectI& right) noexcept
{
    return left.left == right.left && left.top == right.top && left.right == right.right &&
           left.bottom == right.bottom;
}

// 将任意拖动方向的两个端点整理为规范化选区矩形。
// 入参：first、second：虚拟桌面物理像素坐标的两个端点。
// 返回：以各轴较小值为左上边界、较大值为右下边界的矩形。
open_st::RectI Normalize(open_st::PointI first, open_st::PointI second) noexcept
{
    return {std::min(first.x, second.x), std::min(first.y, second.y), std::max(first.x, second.x),
            std::max(first.y, second.y)};
}

// 判断命中的控制点是否为角点，供命中距离相同时确定优先级。
// 入参：handle：待判断的选区控制点枚举。
// 返回：四个角点返回 true，边中点及 None 返回 false。
bool IsCorner(open_st::SelectionHandle handle) noexcept
{
    return handle == open_st::SelectionHandle::TopLeft || handle == open_st::SelectionHandle::TopRight ||
           handle == open_st::SelectionHandle::BottomRight || handle == open_st::SelectionHandle::BottomLeft;
}

// 提取控制点负责调整的水平方向。
// 入参：handle：待解析的选区控制点。
// 返回：左侧返回 -1，右侧返回 1，不负责水平调整时返回 0。
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

// 提取控制点负责调整的垂直方向。
// 入参：handle：待解析的选区控制点。
// 返回：上侧返回 -1，下侧返回 1，不负责垂直调整时返回 0。
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

// 根据当前水平与垂直方向重建缩放控制点，支持越过固定边后翻转。
// 入参：horizontal：水平方向，左为 -1、右为 1、无为 0；vertical：垂直方向，上为 -1、下为 1、无为 0。
// 返回：与方向组合对应的边或角控制点；两方向均为零时返回 None。
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
// 更新选区允许使用的桌面范围，并使已有选区适应新范围。
// 入参：bounds：虚拟桌面物理像素半开边界，可含负坐标。
// 返回：无返回值；边界被保存，已有选区按新范围收敛，无效边界使选区复位。
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

// 清除当前选区及拖动上下文，为同一桌面范围重新选择区域做准备。
// 入参：无。
// 返回：无返回值；阶段恢复 Unselected，已配置的桌面边界保留。
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

// 查询选区是否处于未选择、拖动或稳定选择阶段。
// 入参：无。
// 返回：当前 SelectionPhase 状态值。
SelectionPhase SelectionModel::Phase() const noexcept
{
    return this->phase_;
}

// 查询正在执行的选区创建、移动或缩放操作。
// 入参：无。
// 返回：当前 SelectionOperation；没有进行中的操作时为 None。
SelectionOperation SelectionModel::Operation() const noexcept
{
    return this->operation_;
}

// 判断当前模型是否存在可用的非空选区。
// 入参：无。
// 返回：当前矩形具有正面积时为 true，否则为 false。
bool SelectionModel::HasSelection() const noexcept
{
    return !this->rectangle_.IsEmpty();
}

// 查询当前缩放所使用的控制点，供光标及交互反馈使用。
// 入参：无。
// 返回：缩放操作中的活动控制点；其他操作返回 None。
SelectionHandle SelectionModel::ActiveHandle() const noexcept
{
    return this->activeHandle_;
}

// 生成独立选区状态快照，供渲染器读取而不持有模型。
// 入参：无。
// 返回：包含阶段、操作、矩形和八个控制点的值副本，后续模型变化不会修改该副本。
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

// 按按下位置开始创建、移动或缩放选区。
// 入参：point：鼠标按下的虚拟桌面物理像素坐标。
// 返回：操作被接受并进入拖动状态时为 true；边界无效、已经拖动或点在已有选区外时为 false。
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

// 根据最新鼠标位置推进当前拖动操作并检测选区变化。
// 入参：point：最新鼠标位置，单位为虚拟桌面物理像素。
// 返回：选区矩形实际变化时为 true；没有进行中的拖动或矩形未变时为 false。
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

// 应用最终鼠标位置并结束拖动，对零面积结果执行清空或恢复。
// 入参：point：鼠标释放的虚拟桌面物理像素坐标。
// 返回：处理了进行中的拖动时为 true，包括零面积回退；原先没有拖动时为 false。
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

// 撤销进行中的拖动，恢复用户开始本次操作前的选择状态。
// 入参：无。
// 返回：成功取消拖动时为 true；没有进行中的拖动时为 false；创建被清空，移动和缩放恢复原矩形。
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

// 按半开边界判断鼠标位置是否位于现有选区内部。
// 入参：point：待判断的虚拟桌面物理像素坐标。
// 返回：存在非空选区且点落在左上包含、右下排除的范围内时为 true，否则为 false。
bool SelectionModel::Contains(PointI point) const noexcept
{
    return !this->rectangle_.IsEmpty() && point.x >= this->rectangle_.left && point.x < this->rectangle_.right &&
           point.y >= this->rectangle_.top && point.y < this->rectangle_.bottom;
}

// 在八个控制点的命中区中选出最接近鼠标的控制点。
// 入参：point：待命中的虚拟桌面物理像素坐标。
// 返回：稳定选区中的命中控制点；没有命中或未处于稳定选择状态时返回 None，等距时角点优先。
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

// 计算当前选区四角和四边中点，供渲染与命中检测共用。
// 入参：无。
// 返回：按固定顺序排列的八个控制点及其虚拟桌面物理像素坐标。
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

// 把鼠标坐标限制在选区允许使用的桌面边界内。
// 入参：point：原始虚拟桌面物理像素坐标。
// 返回：限制后的坐标；允许触及半开矩形的 right、bottom 端点以生成完整边界。
PointI SelectionModel::ClampPoint(PointI point) const noexcept
{
    point.x = std::clamp(point.x, this->bounds_.left, this->bounds_.right);
    point.y = std::clamp(point.y, this->bounds_.top, this->bounds_.bottom);
    return point;
}

// 将已有选区收敛到更新后的桌面范围。
// 入参：无。
// 返回：无返回值；优先平移保留原尺寸，只有边界无法容纳时才缩小选区。
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

// 清理已结束操作的拖动上下文并发布稳定选区状态。
// 入参：无。
// 返回：无返回值；阶段切换为 Selected，操作和活动控制点复位，拖动基线被清理。
void SelectionModel::FinishInteraction() noexcept
{
    this->operationStartRectangle_ = {};
    this->phase_ = SelectionPhase::Selected;
    this->operation_ = SelectionOperation::None;
    this->activeHandle_ = SelectionHandle::None;
    this->resizesHorizontally_ = false;
    this->resizesVertically_ = false;
}

// 根据拖动起点和当前坐标更新新建选区，支持到达右下排除边界。
// 入参：point：已限制到桌面边界的当前鼠标物理像素坐标。
// 返回：无返回值；rectangle_ 更新为本次创建操作的规范化半开矩形。
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

// 按相对拖动起点的位移整体移动选区并避免越界。
// 入参：point：已限制到桌面边界的当前鼠标物理像素坐标。
// 返回：无返回值；选区位置被更新，宽高保持不变，整体收敛在桌面边界内。
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

// 围绕固定对边调整选区尺寸，并在越过对边后翻转活动控制点。
// 入参：point：已限制到桌面边界的当前鼠标物理像素坐标。
// 返回：无返回值；rectangle_ 与活动缩放控制点更新，未负责调整的轴保持原边界。
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
