#pragma once

#include <geometry.h>

#include <array>

namespace open_st
{
// 虚拟桌面物理像素点；允许负坐标，与桌面冻结帧的 RectI 使用同一坐标系。
struct PointI final
{
    int x{};
    int y{};
};

enum class SelectionPhase
{
    Unselected,
    Dragging,
    Selected,
};

enum class SelectionOperation
{
    None,
    Creating,
    Moving,
    Resizing,
};

enum class SelectionHandle
{
    None,
    TopLeft,
    Top,
    TopRight,
    Right,
    BottomRight,
    Bottom,
    BottomLeft,
    Left,
};

struct SelectionHandlePosition final
{
    SelectionHandle handle{SelectionHandle::None};
    PointI center{};
};

// 渲染器按值消费快照，避免持有或修改输入状态机。
struct SelectionSnapshot final
{
    SelectionPhase phase{SelectionPhase::Unselected};
    SelectionOperation operation{SelectionOperation::None};
    RectI rectangle{};
    std::array<SelectionHandlePosition, 8> handles{};
    bool hasSelection{};
    bool showHandles{};
};

// 纯 C++ 选区状态机；输入、边界和输出均使用虚拟桌面物理像素。
class SelectionModel final
{
  public:
    // 更新允许选区使用的半开虚拟桌面边界；现有选区尽量保持尺寸并收敛到新边界内。
    void SetBounds(RectI bounds) noexcept;
    // 清空选区和当前交互，但保留已设置的桌面边界以便开始下一次创建。
    void Reset() noexcept;

    // 返回当前对外状态阶段。
    [[nodiscard]] SelectionPhase Phase() const noexcept;
    // 返回拖动阶段正在执行的内部操作；非拖动阶段返回 None。
    [[nodiscard]] SelectionOperation Operation() const noexcept;
    // 报告当前是否存在非空选区。
    [[nodiscard]] bool HasSelection() const noexcept;
    // 返回缩放中当前随鼠标移动的控制点；其他操作返回 None。
    [[nodiscard]] SelectionHandle ActiveHandle() const noexcept;
    // 按值返回供渲染器使用的不可变状态快照。
    [[nodiscard]] SelectionSnapshot Snapshot() const noexcept;

    // 开始创建、移动或缩放；已有选区时，选区外按下返回 false 且不改变状态。
    [[nodiscard]] bool Begin(PointI point) noexcept;
    // 使用最新鼠标点更新当前拖动，矩形实际发生变化时返回 true。
    [[nodiscard]] bool Update(PointI point) noexcept;
    // 提交当前拖动；零面积创建被清空，零面积缩放恢复操作前选区。
    [[nodiscard]] bool End(PointI point) noexcept;

    // 创建操作取消后回到未选择；移动或调整取消后恢复操作前的矩形。
    [[nodiscard]] bool CancelInteraction() noexcept;

    // 按半开矩形规则判断虚拟桌面点是否位于选区内部。
    [[nodiscard]] bool Contains(PointI point) const noexcept;
    // 控制点命中区固定为以中心为基准的 12×12 半开物理像素矩形。
    [[nodiscard]] SelectionHandle HitTestHandle(PointI point) const noexcept;
    // 按固定顺序返回四角和四边中点的八个控制点。
    [[nodiscard]] std::array<SelectionHandlePosition, 8> Handles() const noexcept;

  private:
    // 把输入点限制到当前虚拟桌面半开边界的闭合坐标范围。
    [[nodiscard]] PointI ClampPoint(PointI point) const noexcept;
    // 在边界变化后尽量保持选区尺寸，并把矩形整体收敛到新边界。
    void FitSelectionToBounds() noexcept;
    // 清理一次成功或恢复后的拖动上下文，并进入 Selected 状态。
    void FinishInteraction() noexcept;
    // 根据起点和当前点更新规范化创建矩形，并处理右下 exclusive 边界吸附。
    void UpdateCreating(PointI point) noexcept;
    // 按起点位移整体移动选区，同时保持宽高并限制在桌面内。
    void UpdateMoving(PointI point) noexcept;
    // 围绕固定对边更新缩放矩形，并在跨边时翻转活动控制点。
    void UpdateResizing(PointI point) noexcept;

    RectI bounds_{};
    RectI rectangle_{};
    RectI operationStartRectangle_{};
    PointI dragOrigin_{};
    int resizeFixedX_{};
    int resizeFixedY_{};
    bool resizesHorizontally_{};
    bool resizesVertically_{};
    SelectionPhase phase_{SelectionPhase::Unselected};
    SelectionOperation operation_{SelectionOperation::None};
    SelectionHandle activeHandle_{SelectionHandle::None};
};
} // namespace open_st
