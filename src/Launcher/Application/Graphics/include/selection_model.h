// 文件职责：定义选区几何快照与创建、移动、缩放状态机，统一虚拟桌面物理像素输入。

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
    // 更新选区允许使用的桌面范围，并使已有选区适应新范围。
    // 入参：bounds：虚拟桌面物理像素半开边界，可含负坐标。
    // 返回：无返回值；边界被保存，已有选区按新范围收敛，无效边界使选区复位。
    void SetBounds(RectI bounds) noexcept;
    // 清除当前选区及拖动上下文，为同一桌面范围重新选择区域做准备。
    // 入参：无。
    // 返回：无返回值；阶段恢复 Unselected，已配置的桌面边界保留。
    void Reset() noexcept;

    // 查询选区是否处于未选择、拖动或稳定选择阶段。
    // 入参：无。
    // 返回：当前 SelectionPhase 状态值。
    [[nodiscard]] SelectionPhase Phase() const noexcept;
    // 查询正在执行的选区创建、移动或缩放操作。
    // 入参：无。
    // 返回：当前 SelectionOperation；没有进行中的操作时为 None。
    [[nodiscard]] SelectionOperation Operation() const noexcept;
    // 判断当前模型是否存在可用的非空选区。
    // 入参：无。
    // 返回：当前矩形具有正面积时为 true，否则为 false。
    [[nodiscard]] bool HasSelection() const noexcept;
    // 查询当前缩放所使用的控制点，供光标及交互反馈使用。
    // 入参：无。
    // 返回：缩放操作中的活动控制点；其他操作返回 None。
    [[nodiscard]] SelectionHandle ActiveHandle() const noexcept;
    // 生成独立选区状态快照，供渲染器读取而不持有模型。
    // 入参：无。
    // 返回：包含阶段、操作、矩形和八个控制点的值副本，后续模型变化不会修改该副本。
    [[nodiscard]] SelectionSnapshot Snapshot() const noexcept;

    // 按按下位置开始创建、移动或缩放选区。
    // 入参：point：鼠标按下的虚拟桌面物理像素坐标。
    // 返回：操作被接受并进入拖动状态时为 true；边界无效、已经拖动或点在已有选区外时为 false。
    [[nodiscard]] bool Begin(PointI point) noexcept;
    // 根据最新鼠标位置推进当前拖动操作并检测选区变化。
    // 入参：point：最新鼠标位置，单位为虚拟桌面物理像素。
    // 返回：选区矩形实际变化时为 true；没有进行中的拖动或矩形未变时为 false。
    [[nodiscard]] bool Update(PointI point) noexcept;
    // 应用最终鼠标位置并结束拖动，对零面积结果执行清空或恢复。
    // 入参：point：鼠标释放的虚拟桌面物理像素坐标。
    // 返回：处理了进行中的拖动时为 true，包括零面积回退；原先没有拖动时为 false。
    [[nodiscard]] bool End(PointI point) noexcept;

    // 撤销进行中的拖动，恢复用户开始本次操作前的选择状态。
    // 入参：无。
    // 返回：成功取消拖动时为 true；没有进行中的拖动时为 false；创建被清空，移动和缩放恢复原矩形。
    [[nodiscard]] bool CancelInteraction() noexcept;

    // 按半开边界判断鼠标位置是否位于现有选区内部。
    // 入参：point：待判断的虚拟桌面物理像素坐标。
    // 返回：存在非空选区且点落在左上包含、右下排除的范围内时为 true，否则为 false。
    [[nodiscard]] bool Contains(PointI point) const noexcept;
    // 在八个控制点的命中区中选出最接近鼠标的控制点。
    // 入参：point：待命中的虚拟桌面物理像素坐标。
    // 返回：稳定选区中的命中控制点；没有命中或未处于稳定选择状态时返回 None，等距时角点优先。
    [[nodiscard]] SelectionHandle HitTestHandle(PointI point) const noexcept;
    // 计算当前选区四角和四边中点，供渲染与命中检测共用。
    // 入参：无。
    // 返回：按固定顺序排列的八个控制点及其虚拟桌面物理像素坐标。
    [[nodiscard]] std::array<SelectionHandlePosition, 8> Handles() const noexcept;

  private:
    // 把鼠标坐标限制在选区允许使用的桌面边界内。
    // 入参：point：原始虚拟桌面物理像素坐标。
    // 返回：限制后的坐标；允许触及半开矩形的 right、bottom 端点以生成完整边界。
    [[nodiscard]] PointI ClampPoint(PointI point) const noexcept;
    // 将已有选区收敛到更新后的桌面范围。
    // 入参：无。
    // 返回：无返回值；优先平移保留原尺寸，只有边界无法容纳时才缩小选区。
    void FitSelectionToBounds() noexcept;
    // 清理已结束操作的拖动上下文并发布稳定选区状态。
    // 入参：无。
    // 返回：无返回值；阶段切换为 Selected，操作和活动控制点复位，拖动基线被清理。
    void FinishInteraction() noexcept;
    // 根据拖动起点和当前坐标更新新建选区，支持到达右下排除边界。
    // 入参：point：已限制到桌面边界的当前鼠标物理像素坐标。
    // 返回：无返回值；rectangle_ 更新为本次创建操作的规范化半开矩形。
    void UpdateCreating(PointI point) noexcept;
    // 按相对拖动起点的位移整体移动选区并避免越界。
    // 入参：point：已限制到桌面边界的当前鼠标物理像素坐标。
    // 返回：无返回值；选区位置被更新，宽高保持不变，整体收敛在桌面边界内。
    void UpdateMoving(PointI point) noexcept;
    // 围绕固定对边调整选区尺寸，并在越过对边后翻转活动控制点。
    // 入参：point：已限制到桌面边界的当前鼠标物理像素坐标。
    // 返回：无返回值；rectangle_ 与活动缩放控制点更新，未负责调整的轴保持原边界。
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
