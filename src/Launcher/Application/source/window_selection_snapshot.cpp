// 文件职责：一次采集窗口元数据，验证完整 Z 序，再发布仅含物理矩形的截图预选快照。

#include "window_selection_snapshot.h"

#include <Windows.h>
#include <dwmapi.h>

#include <algorithm>
#include <array>
#include <unordered_map>
#include <utility>

namespace
{
constexpr std::size_t MAXIMUM_NODES = 2048;
constexpr std::chrono::milliseconds CAPTURE_BUDGET{32};

// 对半开物理矩形求交，不进行坐标平移或 DPI 缩放。
// 入参：first/second 是同一物理坐标系中的矩形。
// 返回：两矩形交集；无交集时返回空或反向矩形。
open_st::RectI Intersect(open_st::RectI first, open_st::RectI second) noexcept
{
    return {std::max(first.left, second.left), std::max(first.top, second.top), std::min(first.right, second.right),
            std::min(first.bottom, second.bottom)};
}

// 按左上包含、右下排除判断鼠标命中。
// 入参：rectangle 是物理矩形，point 是物理位置。
// 返回：点位于矩形内部时为 true。
bool Contains(open_st::RectI rectangle, open_st::PointI point) noexcept
{
    return point.x >= rectangle.left && point.x < rectangle.right && point.y >= rectangle.top &&
           point.y < rectangle.bottom;
}

// 识别明确不纳入首版的系统角色和非激活工具浮层，保留其遮挡。
// 入参：node 是已确认可见的窗口元数据，不读取标题或进程信息。
// 返回：普通窗口、对话框及可激活工具窗口为 true，已知系统阻挡为 false。
bool IsSelectable(const open_st::WindowSelectionNode& node) noexcept
{
    constexpr std::array<const wchar_t*, 7> BLOCKING_CLASSES{
        L"Progman", L"WorkerW",          L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd",
        L"#32768",  L"tooltips_class32", L"SysShadow"};
    for (const wchar_t* className : BLOCKING_CLASSES)
    {
        if (_wcsicmp(node.className.c_str(), className) == 0)
        {
            return false;
        }
    }
    constexpr std::uint32_t TOOL_OVERLAY = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    return (node.extendedStyle & TOOL_OVERLAY) != TOOL_OVERLAY;
}

struct CaptureContext final
{
    std::vector<open_st::WindowSelectionNode> nodes;
    std::chrono::steady_clock::time_point deadline;
    open_st::WindowSelectionFailure failure{open_st::WindowSelectionFailure::None};
    bool perMonitorV2{};
};

// 读取一个节点的系统属性，在同一回调内检测句柄失效，不向目标发送窗口消息。
// 入参：window 是枚举句柄，context 是本次采集上下文，node 接收元数据。
// 返回：读取可靠时为 true；可见节点的重要查询失败或句柄消失时为 false。
bool ReadNode(HWND window, const CaptureContext& context, open_st::WindowSelectionNode& node)
{
    if (!IsWindow(window))
    {
        return false;
    }
    node.id = reinterpret_cast<std::uintptr_t>(window);
    const HWND root = GetAncestor(window, GA_ROOT);
    if (root == nullptr)
    {
        return false;
    }
    node.root = root == window;
    if (!node.root)
    {
        return IsWindow(window) != FALSE;
    }
    node.predecessor = reinterpret_cast<std::uintptr_t>(GetWindow(window, GW_HWNDPREV));
    node.visible = IsWindowVisible(window) != FALSE;
    node.minimized = IsIconic(window) != FALSE;
    if (!node.visible || node.minimized)
    {
        return IsWindow(window) != FALSE;
    }
    DWORD cloaked = 0;
    if (FAILED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))))
    {
        return false;
    }
    node.cloaked = cloaked != 0;
    if (node.cloaked)
    {
        return IsWindow(window) != FALSE;
    }
    RECT rectangle{};
    if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &rectangle, sizeof(rectangle))))
    {
        if (!context.perMonitorV2 || !GetWindowRect(window, &rectangle))
        {
            return false;
        }
        node.frameSource = open_st::WindowSelectionFrameSource::PerMonitorV2WindowRect;
    }
    node.rectangle = {rectangle.left, rectangle.top, rectangle.right, rectangle.bottom};
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR extendedStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if (extendedStyle == 0 && GetLastError() != ERROR_SUCCESS)
    {
        return false;
    }
    node.extendedStyle = static_cast<std::uint32_t>(extendedStyle);
    std::array<wchar_t, 256> className{};
    const int length = GetClassNameW(window, className.data(), static_cast<int>(className.size()));
    if (length == 0)
    {
        return false;
    }
    node.className.assign(className.data(), static_cast<std::size_t>(length));
    return IsWindow(window) != FALSE;
}

// 在 EnumWindows 回调边界收集节点，并阻止异常越过 Win32 ABI。
// 入参：window 是本次枚举节点，parameter 借用仍在栈上的 CaptureContext。
// 返回：继续枚举时 TRUE；查询失败、预算耗尽或分配异常时 FALSE。
BOOL CALLBACK CollectWindow(HWND window, LPARAM parameter) noexcept
{
    CaptureContext& context = *reinterpret_cast<CaptureContext*>(parameter);
    if (context.nodes.size() >= MAXIMUM_NODES || std::chrono::steady_clock::now() >= context.deadline)
    {
        context.failure = open_st::WindowSelectionFailure::Budget;
        return FALSE;
    }
    try
    {
        open_st::WindowSelectionNode node{};
        if (!ReadNode(window, context, node))
        {
            context.failure = open_st::WindowSelectionFailure::Query;
            return FALSE;
        }
        context.nodes.push_back(std::move(node));
        return TRUE;
    }
    catch (...)
    {
        context.failure = open_st::WindowSelectionFailure::Allocation;
        return FALSE;
    }
}
} // namespace

namespace open_st
{
// 枚举一次系统窗口，并将采集和内存排序纳入同一合作式预算。
// 入参：desktop/outputs 是已捕获冻结桌面的物理边界与输出区域。
// 返回：完整快照发布时为 true；失败时保留粗粒度诊断并关闭本次自动预选。
bool WindowSelectionSnapshot::Capture(RectI desktop, std::span<const RectI> outputs) noexcept
{
    this->Clear();
    try
    {
        CaptureContext context{};
        context.deadline = std::chrono::steady_clock::now() + CAPTURE_BUDGET;
        context.perMonitorV2 = AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(),
                                                            DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE;
        context.nodes.reserve(MAXIMUM_NODES);
        if (!EnumWindows(CollectWindow, reinterpret_cast<LPARAM>(&context)))
        {
            return this->Fail(context.failure == WindowSelectionFailure::None ? WindowSelectionFailure::Enumeration
                                                                              : context.failure);
        }
        return this->BuildInternal(context.nodes, desktop, outputs, &context.deadline);
    }
    catch (...)
    {
        return this->Fail(WindowSelectionFailure::Allocation);
    }
}

// 使用确定性节点输入执行与真实采集相同的构建路径。
// 入参：nodes/desktop/outputs 是本次输入，collectionComplete 为 false 表示采集中断。
// 返回：成功时发布候选；采集不完整或数据错误时返回 false 并清空旧候选。
bool WindowSelectionSnapshot::Build(std::span<const WindowSelectionNode> nodes, RectI desktop,
                                    std::span<const RectI> outputs, bool collectionComplete) noexcept
{
    if (!collectionComplete)
    {
        return this->Fail(WindowSelectionFailure::Enumeration);
    }
    return this->BuildInternal(nodes, desktop, outputs, nullptr);
}

// 验证唯一完整的顶层前驱链，在纯内存中按从上到下的顺序生成候选与阻挡。
// 入参：nodes/desktop/outputs 是完整数据，deadline 是可选合作预算结束时间。
// 返回：完整发布时为 true；不发布任何错误链或预算内截断的部分结果。
bool WindowSelectionSnapshot::BuildInternal(std::span<const WindowSelectionNode> nodes, RectI desktop,
                                            std::span<const RectI> outputs,
                                            const std::chrono::steady_clock::time_point* deadline) noexcept
{
    this->Clear();
    if (desktop.IsEmpty() || outputs.empty())
    {
        return this->Fail(WindowSelectionFailure::InvalidGeometry);
    }
    if (nodes.size() > MAXIMUM_NODES)
    {
        return this->Fail(WindowSelectionFailure::Budget);
    }
    try
    {
        std::unordered_map<std::uintptr_t, const WindowSelectionNode*> roots;
        std::unordered_map<std::uintptr_t, const WindowSelectionNode*> successors;
        std::unordered_map<std::uintptr_t, bool> identifiers;
        roots.reserve(nodes.size());
        successors.reserve(nodes.size());
        identifiers.reserve(nodes.size());
        for (const WindowSelectionNode& node : nodes)
        {
            if (deadline != nullptr && std::chrono::steady_clock::now() >= *deadline)
            {
                return this->Fail(WindowSelectionFailure::Budget);
            }
            if (!node.valid)
            {
                return this->Fail(WindowSelectionFailure::Query);
            }
            if (node.id == 0 || !identifiers.emplace(node.id, true).second)
            {
                return this->Fail(WindowSelectionFailure::Topology);
            }
            if (node.root)
            {
                roots.emplace(node.id, &node);
                if (node.predecessor == node.id || !successors.emplace(node.predecessor, &node).second)
                {
                    return this->Fail(WindowSelectionFailure::Topology);
                }
            }
        }
        for (const WindowSelectionNode& node : nodes)
        {
            if (deadline != nullptr && std::chrono::steady_clock::now() >= *deadline)
            {
                return this->Fail(WindowSelectionFailure::Budget);
            }
            if (node.root && node.predecessor != 0 && !roots.contains(node.predecessor))
            {
                return this->Fail(WindowSelectionFailure::Topology);
            }
        }
        std::vector<RectI> validOutputs;
        validOutputs.reserve(outputs.size());
        for (const RectI output : outputs)
        {
            const RectI clipped = Intersect(output, desktop);
            if (!clipped.IsEmpty())
            {
                validOutputs.push_back(clipped);
            }
        }
        if (validOutputs.empty())
        {
            return this->Fail(WindowSelectionFailure::InvalidGeometry);
        }
        std::vector<Entry> entries;
        entries.reserve(roots.size());
        std::uintptr_t predecessor = 0;
        std::size_t visited = 0;
        while (successors.contains(predecessor))
        {
            if (visited >= MAXIMUM_NODES || (deadline != nullptr && std::chrono::steady_clock::now() >= *deadline))
            {
                return this->Fail(WindowSelectionFailure::Budget);
            }
            const WindowSelectionNode& node = *successors.at(predecessor);
            predecessor = node.id;
            ++visited;
            if (!node.visible || node.minimized)
            {
                continue;
            }
            if (!node.cloakedKnown)
            {
                return this->Fail(WindowSelectionFailure::Query);
            }
            if (node.cloaked)
            {
                continue;
            }
            if (!node.frameKnown)
            {
                return this->Fail(WindowSelectionFailure::Query);
            }
            const RectI clipped = Intersect(node.rectangle, desktop);
            bool intersectsOutput = false;
            for (const RectI output : validOutputs)
            {
                intersectsOutput = intersectsOutput || !Intersect(clipped, output).IsEmpty();
            }
            if (intersectsOutput)
            {
                entries.push_back({clipped, IsSelectable(node), node.frameSource});
            }
        }
        if (visited != roots.size())
        {
            return this->Fail(WindowSelectionFailure::Topology);
        }
        if (deadline != nullptr && std::chrono::steady_clock::now() >= *deadline)
        {
            return this->Fail(WindowSelectionFailure::Budget);
        }
        this->entries_ = std::move(entries);
        this->outputs_ = std::move(validOutputs);
        return true;
    }
    catch (...)
    {
        return this->Fail(WindowSelectionFailure::Allocation);
    }
}

// 查询纯值快照中的最上层矩形，明确阻止穿透不支持窗口和输出间隙。
// 入参：point 是冻结桌面物理坐标。
// 返回：首个命中的可选矩形；未命中或首个命中为阻挡时返回空值。
std::optional<RectI> WindowSelectionSnapshot::Candidate(PointI point) const noexcept
{
    bool onOutput = false;
    for (const RectI output : this->outputs_)
    {
        onOutput = onOutput || Contains(output, point);
    }
    if (!onOutput)
    {
        return std::nullopt;
    }
    for (const Entry& entry : this->entries_)
    {
        if (Contains(entry.rectangle, point))
        {
            return entry.selectable ? std::optional<RectI>{entry.rectangle} : std::nullopt;
        }
    }
    return std::nullopt;
}

// 清空会话几何和失败诊断，不保留源窗口句柄。
// 入参：无。
// 返回：无返回值。
void WindowSelectionSnapshot::Clear() noexcept
{
    this->entries_.clear();
    this->outputs_.clear();
    this->failure_ = WindowSelectionFailure::None;
}

// 获取最近一次完整构建或失败的粗粒度状态。
// 入参：无。
// 返回：None 表示无失败，否则表示禁用本会话自动候选的原因。
WindowSelectionFailure WindowSelectionSnapshot::Failure() const noexcept
{
    return this->failure_;
}

// 统一撤销整个候选快照，不允许调用者消费部分构建结果。
// 入参：failure 为当前失败原因。
// 返回：固定为 false。
bool WindowSelectionSnapshot::Fail(WindowSelectionFailure failure) noexcept
{
    this->Clear();
    this->failure_ = failure;
    return false;
}
} // namespace open_st
