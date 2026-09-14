// 文件职责：验证窗口快照的完整链、遮挡分类、物理几何、失败降级和真实自有窗口生命周期。

#include "window_selection_snapshot.h"

#include <Windows.h>
#include <dwmapi.h>
#include <gtest/gtest.h>

#include <array>
#include <vector>

namespace open_st
{
struct WindowSelectionSnapshotTestAccess final
{
    // 使用已经到期的合作预算验证真实构建路径不会发布截断候选。
    // 入参：snapshot 是待构建快照，nodes/desktop/outputs 是完整有效输入。
    // 返回：预算过期时固定失败，错误类别由生产路径设置。
    static bool BuildExpired(WindowSelectionSnapshot& snapshot, std::span<const WindowSelectionNode> nodes,
                             RectI desktop, std::span<const RectI> outputs)
    {
        const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now();
        return snapshot.BuildInternal(nodes, desktop, outputs, &deadline);
    }
};
} // namespace open_st

namespace
{
using open_st::PointI;
using open_st::RectI;
using open_st::WindowSelectionFailure;
using open_st::WindowSelectionNode;
using open_st::WindowSelectionSnapshot;

constexpr RectI DESKTOP{-200, -100, 500, 400};
constexpr std::array<RectI, 1> OUTPUTS{DESKTOP};

// 生成默认普通顶层节点，让各用例只覆写待验证的属性。
// 入参：id/predecessor 是内存链标识，rectangle 是窗口物理范围。
// 返回：所有查询已确认的普通可见窗口节点。
WindowSelectionNode Node(std::uintptr_t id, std::uintptr_t predecessor, RectI rectangle = {0, 0, 100, 100})
{
    WindowSelectionNode node{};
    node.id = id;
    node.predecessor = predecessor;
    node.rectangle = rectangle;
    node.className = L"OpenSTSnapshotOrdinaryWindow";
    return node;
}

// 独立比较候选四边，避免测试只重复生产排序或分类逻辑。
// 入参：actual 是实际可选结果，expected 是测试明确指定的矩形。
// 返回：无返回值；不匹配时记录断言失败。
void ExpectRectangle(const std::optional<RectI>& actual, RectI expected)
{
    ASSERT_TRUE(actual.has_value());
    EXPECT_EQ(actual->left, expected.left);
    EXPECT_EQ(actual->top, expected.top);
    EXPECT_EQ(actual->right, expected.right);
    EXPECT_EQ(actual->bottom, expected.bottom);
}

// 验证回调输入顺序不影响前驱链恢复；局部遮挡仍选择下层完整窗口范围。
// 入参：无；测试内部构造本用例所需的节点或自有窗口。
// 返回：无返回值；通过断言核验预期结果与边界。
TEST(WindowSelectionSnapshotTest, unordered_nodes_follow_predecessors_and_preserve_full_bounds)
{
    const RectI upper{20, 20, 80, 80};
    const RectI lower{0, 0, 120, 120};
    const std::array nodes{Node(2, 1, lower), Node(1, 0, upper)};
    WindowSelectionSnapshot snapshot;
    ASSERT_TRUE(snapshot.Build(nodes, DESKTOP, OUTPUTS));
    ExpectRectangle(snapshot.Candidate({30, 30}), upper);
    ExpectRectangle(snapshot.Candidate({10, 10}), lower);
    EXPECT_FALSE(snapshot.Candidate({150, 150}).has_value());
}

// 验证隐藏、最小化和 cloaked 节点保留链位置却不提供候选或阻挡。
// 入参：无；测试内部构造本用例所需的节点或自有窗口。
// 返回：无返回值；通过断言核验预期结果与边界。
TEST(WindowSelectionSnapshotTest, hidden_minimized_and_cloaked_nodes_are_skipped_after_sorting)
{
    std::array nodes{Node(1, 0), Node(2, 1), Node(3, 2), Node(4, 3, {0, 0, 150, 150})};
    nodes[0].visible = false;
    nodes[0].cloakedKnown = false;
    nodes[1].minimized = true;
    nodes[1].frameKnown = false;
    nodes[2].cloaked = true;
    nodes[2].frameKnown = false;
    WindowSelectionSnapshot snapshot;
    ASSERT_TRUE(snapshot.Build(nodes, DESKTOP, OUTPUTS));
    ExpectRectangle(snapshot.Candidate({30, 30}), nodes[3].rectangle);
}

// 验证桌面、任务栏、标准菜单、提示和非激活工具浮层阻挡下层普通窗口。
// 入参：无；测试内部构造本用例所需的节点或自有窗口。
// 返回：无返回值；通过断言核验预期结果与边界。
TEST(WindowSelectionSnapshotTest, unsupported_visible_regions_block_instead_of_clicking_through)
{
    constexpr std::array classes{L"Progman", L"WorkerW",          L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd",
                                 L"#32768",  L"tooltips_class32", L"SysShadow"};
    for (const wchar_t* className : classes)
    {
        SCOPED_TRACE(::testing::Message() << className);
        std::array nodes{Node(1, 0, {20, 20, 80, 80}), Node(2, 1)};
        nodes[0].className = className;
        WindowSelectionSnapshot snapshot;
        ASSERT_TRUE(snapshot.Build(nodes, DESKTOP, OUTPUTS));
        EXPECT_FALSE(snapshot.Candidate({30, 30}).has_value());
        ExpectRectangle(snapshot.Candidate({10, 10}), nodes[1].rectangle);
    }
    std::array nodes{Node(1, 0), Node(2, 1)};
    nodes[0].extendedStyle = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    WindowSelectionSnapshot snapshot;
    ASSERT_TRUE(snapshot.Build(nodes, DESKTOP, OUTPUTS));
    EXPECT_FALSE(snapshot.Candidate({30, 30}).has_value());
}

// 验证独立对话框、无边框窗口、透明属性和可激活工具窗不会被整类误排除。
// 入参：无；测试内部构造本用例所需的节点或自有窗口。
// 返回：无返回值；通过断言核验预期结果与边界。
TEST(WindowSelectionSnapshotTest, dialogs_borderless_windows_and_activatable_pins_remain_selectable)
{
    constexpr std::array<std::uint32_t, 4> styles{0, WS_EX_TOOLWINDOW, WS_EX_TRANSPARENT,
                                                  WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT};
    for (const std::uint32_t style : styles)
    {
        std::array nodes{Node(1, 0)};
        nodes[0].extendedStyle = style;
        nodes[0].className = style == 0 ? L"#32770" : L"OpenSTPinWindow";
        WindowSelectionSnapshot snapshot;
        ASSERT_TRUE(snapshot.Build(nodes, DESKTOP, OUTPUTS));
        ExpectRectangle(snapshot.Candidate({30, 30}), nodes[0].rectangle);
    }
}

// 验证查询未知和读取失效整体关闭候选，包括清除以前成功发布的结果。
// 入参：无；测试内部构造本用例所需的节点或自有窗口。
// 返回：无返回值；通过断言核验预期结果与边界。
TEST(WindowSelectionSnapshotTest, uncertain_visible_metadata_invalidates_the_entire_snapshot)
{
    const std::array valid{Node(1, 0)};
    for (int variation = 0; variation < 3; ++variation)
    {
        std::array nodes{Node(1, 0), Node(2, 1, {200, 200, 250, 250})};
        if (variation == 0)
        {
            nodes[1].valid = false;
        }
        else if (variation == 1)
        {
            nodes[1].cloakedKnown = false;
        }
        else
        {
            nodes[1].frameKnown = false;
        }
        WindowSelectionSnapshot snapshot;
        ASSERT_TRUE(snapshot.Build(valid, DESKTOP, OUTPUTS));
        EXPECT_FALSE(snapshot.Build(nodes, DESKTOP, OUTPUTS));
        EXPECT_EQ(snapshot.Failure(), WindowSelectionFailure::Query);
        EXPECT_FALSE(snapshot.Candidate({30, 30}).has_value());
    }
}

// 验证重复、缺前驱、自环、长环、分叉、多根和部分链均不发布猜测顺序。
// 入参：无；测试内部构造本用例所需的节点或自有窗口。
// 返回：无返回值；通过断言核验预期结果与边界。
TEST(WindowSelectionSnapshotTest, malformed_topology_never_publishes_partial_candidates)
{
    const std::vector<std::vector<WindowSelectionNode>> cases{{Node(1, 0), Node(1, 1)},
                                                              {Node(1, 99)},
                                                              {Node(1, 1)},
                                                              {Node(1, 2), Node(2, 1)},
                                                              {Node(1, 0), Node(2, 1), Node(3, 1)},
                                                              {Node(1, 0), Node(2, 0)},
                                                              {Node(1, 0), Node(2, 3), Node(3, 2)},
                                                              {Node(0, 0)}};
    for (std::size_t index = 0; index < cases.size(); ++index)
    {
        SCOPED_TRACE(index);
        WindowSelectionSnapshot snapshot;
        EXPECT_FALSE(snapshot.Build(cases[index], DESKTOP, OUTPUTS));
        EXPECT_EQ(snapshot.Failure(), WindowSelectionFailure::Topology);
        EXPECT_FALSE(snapshot.Candidate({30, 30}).has_value());
    }
}

// 验证系统非根节点不混入顶层兄弟链，顶层节点错误引用它时仍整体拒绝。
// 入参：无；测试内部构造本用例所需的节点或自有窗口。
// 返回：无返回值；通过断言核验预期结果与边界。
TEST(WindowSelectionSnapshotTest, non_root_system_nodes_are_excluded_from_the_top_level_chain)
{
    std::array nodes{Node(1, 0), Node(2, 1), Node(3, 999)};
    nodes[2].root = false;
    WindowSelectionSnapshot snapshot;
    ASSERT_TRUE(snapshot.Build(nodes, DESKTOP, OUTPUTS));
    ExpectRectangle(snapshot.Candidate({30, 30}), nodes[0].rectangle);
    nodes[1].predecessor = 3;
    EXPECT_FALSE(snapshot.Build(nodes, DESKTOP, OUTPUTS));
    EXPECT_EQ(snapshot.Failure(), WindowSelectionFailure::Topology);
}

// 验证负坐标、跨屏间隙、桌面裁切和半开边界，DWM 物理框不做第二次缩放。
// 入参：无；测试内部构造本用例所需的节点或自有窗口。
// 返回：无返回值；通过断言核验预期结果与边界。
TEST(WindowSelectionSnapshotTest, physical_frames_clip_without_scaling_or_filling_output_gaps)
{
    constexpr std::array<RectI, 2> outputs{{{-200, -100, 0, 200}, {100, 0, 500, 400}}};
    std::array nodes{Node(1, 0, {-250, -120, 350, 350})};
    WindowSelectionSnapshot snapshot;
    ASSERT_TRUE(snapshot.Build(nodes, DESKTOP, outputs));
    ExpectRectangle(snapshot.Candidate({-200, -100}), {-200, -100, 350, 350});
    ExpectRectangle(snapshot.Candidate({349, 349}), {-200, -100, 350, 350});
    EXPECT_FALSE(snapshot.Candidate({0, 50}).has_value());
    EXPECT_FALSE(snapshot.Candidate({50, 50}).has_value());
    EXPECT_FALSE(snapshot.Candidate({350, 349}).has_value());
    EXPECT_FALSE(snapshot.Candidate({-201, -100}).has_value());
    nodes[0].frameSource = open_st::WindowSelectionFrameSource::PerMonitorV2WindowRect;
    ASSERT_TRUE(snapshot.Build(nodes, DESKTOP, outputs));
    ExpectRectangle(snapshot.Candidate({100, 0}), {-200, -100, 350, 350});
}

// 验证空矩形、完全在桌面外和只落在屏间隙的窗口不产生候选。
// 入参：无；测试内部构造本用例所需的节点或自有窗口。
// 返回：无返回值；通过断言核验预期结果与边界。
TEST(WindowSelectionSnapshotTest, empty_off_desktop_and_gap_only_frames_are_ignored)
{
    constexpr std::array<RectI, 2> outputs{{{-200, -100, 0, 200}, {100, 0, 500, 400}}};
    const std::array nodes{Node(1, 0, {0, 0, 0, 100}), Node(2, 1, {700, 0, 800, 100}), Node(3, 2, {20, 20, 80, 80})};
    WindowSelectionSnapshot snapshot;
    ASSERT_TRUE(snapshot.Build(nodes, DESKTOP, outputs));
    EXPECT_FALSE(snapshot.Candidate({-50, 50}).has_value());
    EXPECT_FALSE(snapshot.Candidate({150, 50}).has_value());
}

// 验证不完整采集和节点预算拒绝部分结果；恰好达到 2048 个完整节点可发布。
// 入参：无；测试内部构造本用例所需的节点或自有窗口。
// 返回：无返回值；通过断言核验预期结果与边界。
TEST(WindowSelectionSnapshotTest, incomplete_collection_and_node_budget_disable_all_candidates)
{
    std::vector<WindowSelectionNode> nodes;
    for (std::uintptr_t id = 1; id <= 2048; ++id)
    {
        nodes.push_back(Node(id, id - 1));
    }
    WindowSelectionSnapshot snapshot;
    ASSERT_TRUE(snapshot.Build(nodes, DESKTOP, OUTPUTS));
    EXPECT_FALSE(snapshot.Build(nodes, DESKTOP, OUTPUTS, false));
    EXPECT_EQ(snapshot.Failure(), WindowSelectionFailure::Enumeration);
    EXPECT_FALSE(snapshot.Candidate({30, 30}).has_value());
    nodes.push_back(Node(2049, 2048));
    EXPECT_FALSE(snapshot.Build(nodes, DESKTOP, OUTPUTS));
    EXPECT_EQ(snapshot.Failure(), WindowSelectionFailure::Budget);
    EXPECT_FALSE(open_st::WindowSelectionSnapshotTestAccess::BuildExpired(snapshot, std::span(nodes).first(1), DESKTOP,
                                                                          OUTPUTS));
    EXPECT_EQ(snapshot.Failure(), WindowSelectionFailure::Budget);
    EXPECT_FALSE(snapshot.Candidate({30, 30}).has_value());
}

// 验证发布后不再依赖元数据或源标识，显式 Clear 才丢弃会话矩形。
// 入参：无；测试内部构造本用例所需的节点或自有窗口。
// 返回：无返回值；通过断言核验预期结果与边界。
TEST(WindowSelectionSnapshotTest, published_geometry_survives_source_changes_until_clear)
{
    std::array nodes{Node(1, 0)};
    WindowSelectionSnapshot snapshot;
    ASSERT_TRUE(snapshot.Build(nodes, DESKTOP, OUTPUTS));
    nodes[0].id = 0;
    nodes[0].rectangle = {};
    nodes[0].className = L"Shell_TrayWnd";
    ExpectRectangle(snapshot.Candidate({30, 30}), {0, 0, 100, 100});
    snapshot.Clear();
    EXPECT_FALSE(snapshot.Candidate({30, 30}).has_value());
    EXPECT_EQ(snapshot.Failure(), WindowSelectionFailure::None);
}

// 验证无有效冻结输出时关闭自动候选，不把无效桌面解释成整屏选择。
// 入参：无；测试内部构造本用例所需的节点或自有窗口。
// 返回：无返回值；通过断言核验预期结果与边界。
TEST(WindowSelectionSnapshotTest, invalid_desktop_or_missing_outputs_cannot_publish)
{
    const std::array nodes{Node(1, 0)};
    WindowSelectionSnapshot snapshot;
    EXPECT_FALSE(snapshot.Build(nodes, {}, OUTPUTS));
    EXPECT_FALSE(snapshot.Build(nodes, DESKTOP, {}));
    const std::array<RectI, 1> outside{{{1000, 1000, 1100, 1100}}};
    EXPECT_FALSE(snapshot.Build(nodes, DESKTOP, outside));
    EXPECT_EQ(snapshot.Failure(), WindowSelectionFailure::InvalidGeometry);
}

struct OwnedDesktopWindows final
{
    DPI_AWARENESS_CONTEXT previousDpi{};
    ATOM windowClass{};
    HWND lower{};
    HWND upper{};

    // 建立本线程的物理坐标上下文和无用户内容的专用窗口类。
    // 入参：无。
    // 返回：构造完成后通过字段判断环境和类注册是否成功。
    OwnedDesktopWindows()
    {
        this->previousDpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        WNDCLASSW definition{};
        definition.lpfnWndProc = DefWindowProcW;
        definition.hInstance = GetModuleHandleW(nullptr);
        definition.lpszClassName = L"OpenSTWindowSnapshotIntegration";
        this->windowClass = RegisterClassW(&definition);
    }

    // 只销毁本用例创建的两个窗口、窗口类并恢复 DPI 上下文。
    // 入参：无。
    // 返回：析构完成后不保留窗口或改变其他应用的位置。
    ~OwnedDesktopWindows()
    {
        if (this->upper != nullptr)
        {
            DestroyWindow(this->upper);
        }
        if (this->lower != nullptr)
        {
            DestroyWindow(this->lower);
        }
        if (this->windowClass != 0)
        {
            UnregisterClassW(MAKEINTATOM(this->windowClass), GetModuleHandleW(nullptr));
        }
        if (this->previousDpi != nullptr)
        {
            SetThreadDpiAwarenessContext(this->previousDpi);
        }
    }
};

// 收集真实显示器物理范围，避免把虚拟桌面外接矩形中的间隙冒充可捕获输出。
// 入参：rectangle 是系统显示器矩形，parameter 借用本用例的输出数组，其余参数不使用。
// 返回：成功收集为 TRUE；分配异常时 FALSE，不让异常穿过系统回调。
BOOL CALLBACK CollectMonitorRectangle(HMONITOR, HDC, LPRECT rectangle, LPARAM parameter) noexcept
{
    try
    {
        std::vector<RectI>& outputs = *reinterpret_cast<std::vector<RectI>*>(parameter);
        outputs.push_back({rectangle->left, rectangle->top, rectangle->right, rectangle->bottom});
        return TRUE;
    }
    catch (...)
    {
        return FALSE;
    }
}

// 验证真实自有普通窗口的重叠顺序、DWM 物理边界，以及源 HWND 销毁后的纯快照查询。
// 入参：无；测试内部构造本用例所需的节点或自有窗口。
// 返回：无返回值；通过断言核验预期结果与边界。
TEST(WindowSelectionSnapshotDesktopTest, owned_overlapping_windows_keep_frozen_bounds_after_destruction)
{
    HDESK desktopHandle = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (desktopHandle == nullptr)
    {
        GTEST_SKIP() << "没有可访问的交互桌面，无法创建真实可见窗口。";
    }
    CloseDesktop(desktopHandle);
    OwnedDesktopWindows windows;
    ASSERT_NE(windows.previousDpi, nullptr);
    ASSERT_NE(windows.windowClass, 0);
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    ASSERT_TRUE(GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor));
    const PointI origin{monitor.rcMonitor.left + 100, monitor.rcMonitor.top + 100};
    windows.lower = CreateWindowExW(WS_EX_TOPMOST, MAKEINTATOM(windows.windowClass), L"", WS_POPUP, origin.x, origin.y,
                                    160, 120, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    windows.upper = CreateWindowExW(WS_EX_TOPMOST, MAKEINTATOM(windows.windowClass), L"", WS_POPUP, origin.x + 40,
                                    origin.y + 30, 160, 120, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ASSERT_NE(windows.lower, nullptr);
    ASSERT_NE(windows.upper, nullptr);
    ASSERT_TRUE(SetWindowPos(windows.lower, HWND_TOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW));
    ASSERT_TRUE(SetWindowPos(windows.upper, HWND_TOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW));
    RECT lowerBounds{};
    RECT upperBounds{};
    ASSERT_TRUE(SUCCEEDED(
        DwmGetWindowAttribute(windows.lower, DWMWA_EXTENDED_FRAME_BOUNDS, &lowerBounds, sizeof(lowerBounds))));
    ASSERT_TRUE(SUCCEEDED(
        DwmGetWindowAttribute(windows.upper, DWMWA_EXTENDED_FRAME_BOUNDS, &upperBounds, sizeof(upperBounds))));
    const RectI lower{lowerBounds.left, lowerBounds.top, lowerBounds.right, lowerBounds.bottom};
    const RectI upper{upperBounds.left, upperBounds.top, upperBounds.right, upperBounds.bottom};
    const RectI desktop{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                        GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
                        GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
    std::vector<RectI> outputs;
    ASSERT_TRUE(EnumDisplayMonitors(nullptr, nullptr, CollectMonitorRectangle, reinterpret_cast<LPARAM>(&outputs)));
    ASSERT_FALSE(outputs.empty());
    ASSERT_EQ(WindowFromPhysicalPoint({origin.x + 10, origin.y + 10}), windows.lower);
    ASSERT_EQ(WindowFromPhysicalPoint({origin.x + 60, origin.y + 50}), windows.upper);
    WindowSelectionSnapshot snapshot;
    ASSERT_TRUE(snapshot.Capture(desktop, outputs)) << static_cast<int>(snapshot.Failure());
    ExpectRectangle(snapshot.Candidate({origin.x + 10, origin.y + 10}), lower);
    ExpectRectangle(snapshot.Candidate({origin.x + 60, origin.y + 50}), upper);
    ASSERT_TRUE(DestroyWindow(windows.upper));
    windows.upper = nullptr;
    ASSERT_TRUE(DestroyWindow(windows.lower));
    windows.lower = nullptr;
    ExpectRectangle(snapshot.Candidate({origin.x + 60, origin.y + 50}), upper);
}
} // namespace
