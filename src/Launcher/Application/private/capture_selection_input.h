// 文件职责：区分窗口候选的单击采用与自由框选起步，不持有选区模型或原生输入资源。

#pragma once

#include <selection_model.h>

namespace open_st
{
enum class CaptureSelectionAction
{
    None,
    SelectCandidate,
    BeginDrag,
};

// 抬起时的单次判定；只有非 None 结果携带本次按下起点与锁定候选。
struct CaptureSelectionFinish final
{
    CaptureSelectionAction action{CaptureSelectionAction::None};
    PointI origin{};
    RectI rectangle{};
};

class CaptureSelectionInput final
{
  public:
    // 锁定按下时的窗口候选及物理像素拖动容差，开始一次待判定交互。
    // 入参：origin：按下点；candidate：非空半开候选；dragWidth、dragHeight：正数容差全宽和全高。
    // 返回：成功进入待判定状态时为 true；参数无效或已有待判定交互时为 false 且保持原状态。
    [[nodiscard]] bool Begin(PointI origin, RectI candidate, int dragWidth, int dragHeight) noexcept;
    // 查询是否仍在等待单击或拖动判定。
    // 入参：无。
    // 返回：尚未越过容差或结束交互时为 true，否则为 false。
    [[nodiscard]] bool Pending() const noexcept;
    // 查询最后一次成功按下的物理像素起点，供越界后从原点创建选区。
    // 入参：无。
    // 返回：起点值副本；Move 切换为拖动后仍保留，Cancel 或 Finish 后清零。
    [[nodiscard]] PointI Origin() const noexcept;
    // 查询按下时锁定的候选范围，供待判定期间维持预选显示。
    // 入参：无。
    // 返回：候选半开矩形副本；取消或结束后为空。
    [[nodiscard]] RectI Candidate() const noexcept;
    // 检测最新移动是否首次离开按下位置的半开容差矩形。
    // 入参：point：最新虚拟桌面物理像素位置。
    // 返回：首次越界时为 true 并结束待判定；其余情况为 false，后续拖动由调用方模型处理。
    [[nodiscard]] bool Move(PointI point) noexcept;
    // 以抬起位置完成判定，覆盖没有收到移动消息就越界的情况。
    // 入参：point：抬起时的虚拟桌面物理像素位置。
    // 返回：容差内为 SelectCandidate，容差外为 BeginDrag；无待判定时为 None，随后清空上下文。
    [[nodiscard]] CaptureSelectionFinish Finish(PointI point) noexcept;
    // 清空待判定及锁定上下文，供取消、失捕和会话回收统一使用。
    // 入参：无。
    // 返回：无返回值；后续移动与抬起不再产生操作。
    void Cancel() noexcept;

  private:
    // 使用宽整数判断点是否落在起点周围的半开容差内，避免跨屏和极值坐标溢出。
    // 入参：point：待判断的物理像素点。
    // 返回：点位于左上包含、右下排除的容差内时为 true，否则为 false。
    [[nodiscard]] bool WithinTolerance(PointI point) const noexcept;

    PointI origin_{};
    RectI candidate_{};
    int dragWidth_{};
    int dragHeight_{};
    bool pending_{};
};
} // namespace open_st
