// 文件职责：声明截图会话私有窗口快照，以一次系统读取生成不依赖 HWND 生命周期的候选几何。

#pragma once

#include <selection_model.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace open_st
{
enum class WindowSelectionFailure
{
    None,
    InvalidGeometry,
    Enumeration,
    Query,
    Topology,
    Budget,
    Allocation,
};

enum class WindowSelectionFrameSource
{
    Dwm,
    PerMonitorV2WindowRect,
};

// 仅在快照构建期间存在的系统元数据；整数标识只用于内存链校验，不用于后续句柄查询。
struct WindowSelectionNode final
{
    std::uintptr_t id{};
    std::uintptr_t predecessor{};
    bool root{true};
    bool valid{true};
    bool visible{true};
    bool minimized{};
    bool cloakedKnown{true};
    bool cloaked{};
    bool frameKnown{true};
    WindowSelectionFrameSource frameSource{WindowSelectionFrameSource::Dwm};
    RectI rectangle{};
    std::uint32_t extendedStyle{};
    std::wstring className;
};

class WindowSelectionSnapshot final
{
  public:
    // 枚举一次当前桌面窗口，在合作预算内建立只读候选快照。
    // 入参：desktop 是冻结桌面边界，outputs 是实际捕获输出的物理像素矩形。
    // 返回：完整发布时为 true；任何采集失败均清空候选并返回 false，调用者继续手动框选。
    [[nodiscard]] bool Capture(RectI desktop, std::span<const RectI> outputs) noexcept;

    // 从可控元数据构建与真实采集相同的分类和排序结果，供私有边界测试使用。
    // 入参：nodes 为无序完整节点，desktop/outputs 为物理几何，collectionComplete 指示枚举完整性。
    // 返回：链与可见元数据完整时为 true；失败时不保留上次或部分候选。
    [[nodiscard]] bool Build(std::span<const WindowSelectionNode> nodes, RectI desktop, std::span<const RectI> outputs,
                             bool collectionComplete = true) noexcept;

    // 只查询快照中的物理矩形，返回鼠标命中的最上层可选窗口。
    // 入参：point 是虚拟桌面物理像素点；实际输出间隙不提供候选。
    // 返回：可选窗口的裁切矩形；命中阻挡、输出之外或无快照时为空。
    [[nodiscard]] std::optional<RectI> Candidate(PointI point) const noexcept;

    // 丢弃会话内候选几何和诊断状态。
    // 入参：无。
    // 返回：无返回值；后续查询均无候选。
    void Clear() noexcept;

    // 提供本次快照失败的粗粒度原因，供宿主每会话记录一次。
    // 入参：无。
    // 返回：成功或显式清理时为 None，否则为最近构建失败类别。
    [[nodiscard]] WindowSelectionFailure Failure() const noexcept;

  private:
    friend struct WindowSelectionSnapshotTestAccess;

    struct Entry final
    {
        RectI rectangle{};
        bool selectable{};
        WindowSelectionFrameSource frameSource{WindowSelectionFrameSource::Dwm};
    };

    // 统一执行内存链校验、分类和发布，可接入真实采集的同一合作预算。
    // 入参：nodes/desktop/outputs 是待发布数据，deadline 非空时在节点之间检查超时。
    // 返回：完整发布为 true；任意异常或中断时清空快照并给出失败原因。
    [[nodiscard]] bool BuildInternal(std::span<const WindowSelectionNode> nodes, RectI desktop,
                                     std::span<const RectI> outputs,
                                     const std::chrono::steady_clock::time_point* deadline) noexcept;

    // 清除全部可查询数据并记录失败类别。
    // 入参：failure 是本次失败的粗粒度原因。
    // 返回：固定返回 false，便于构建路径统一失败退出。
    [[nodiscard]] bool Fail(WindowSelectionFailure failure) noexcept;

    std::vector<Entry> entries_;
    std::vector<RectI> outputs_;
    WindowSelectionFailure failure_{WindowSelectionFailure::None};
};
} // namespace open_st
