// 验证三个真实重叠窗口的焦点与固定层级；使用系统输入，CTest 负责整体硬超时。
#include "pin_window.h"
#include <array>
#include <dwmapi.h>
#include <gtest/gtest.h>
#include <objbase.h>
#include <pin_image.h>
#include <pin_window_manager.h>
#include <vector>

namespace open_st
{
struct PinWindowGateTestAccess final
{
    // 调用菜单最终使用的提层操作，不模拟菜单视觉验收。
    // 入参：manager 为被测管理器；id 为稳定标识。
    // 返回：无返回值。
    static void Raise(PinWindowManager& manager, PinId id)
    {
        manager.Raise(id);
    }
    // 查询显示不透明度，验证真实 Ctrl 滚轮只改变显示状态。
    // 入参：manager 为被测管理器；id 为存活贴图。
    // 返回：零至一的不透明度；不存在时为负一。
    static float Opacity(const PinWindowManager& manager, PinId id)
    {
        const PinWindow* pin = manager.Find(id);
        return pin ? pin->state.opacity : -1.0F;
    }
};
} // namespace open_st

namespace
{
// 在有限时间内处理测试线程窗口消息，让系统输入完成投递。
// 入参：milliseconds 为最长处理时间，单位毫秒。
// 返回：无返回值；不会等待真人关闭。
void Pump(DWORD milliseconds)
{
    const ULONGLONG until = GetTickCount64() + milliseconds;
    do
    {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        MsgWaitForMultipleObjectsEx(0, nullptr, 5, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    } while (GetTickCount64() < until);
}

// 投递一次真实滚轮输入，保持光标和焦点由操作系统决定。
// 入参：delta 为系统滚轮量，120 代表一格。
// 返回：系统接受事件时为 true。
bool Wheel(int delta)
{
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_WHEEL;
    input.mi.mouseData = static_cast<DWORD>(delta);
    return SendInput(1, &input, sizeof(INPUT)) == 1;
}

// 查询窗口尺寸和位置供变化前后比较。
// 入参：window 为有效贴图 HWND。
// 返回：屏幕物理像素矩形。
RECT Rectangle(HWND window)
{
    RECT rectangle{};
    EXPECT_TRUE(GetWindowRect(window, &rectangle));
    return rectangle;
}

// 从真实桌面 Z 序提取当前三张图的顺序，忽略其他程序窗口。
// 入参：windows 为待比较的三个 HWND。
// 返回：桌面从上到下排列的自有窗口。
std::vector<HWND> Order(const std::array<HWND, 3>& windows)
{
    std::vector<HWND> result;
    for (HWND window = GetTopWindow(nullptr); window != nullptr; window = GetWindow(window, GW_HWNDNEXT))
        for (HWND candidate : windows)
            if (window == candidate)
                result.push_back(window);
    return result;
}

// 保证失败断言提前退出时也恢复鼠标和 COM；管理器须先于此对象销毁。
struct DesktopScope final
{
    POINT cursor{};
    HWND foreground{};
    HRESULT com{E_FAIL};
    // 保存桌面交互状态并建立 COM 单线程单元。
    // 入参：无。
    // 返回：无返回值；初始化结果由测试检查。
    DesktopScope()
    {
        GetCursorPos(&this->cursor);
        this->foreground = GetForegroundWindow();
        this->com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    }
    // 恢复鼠标和原前台窗口，防止测试留下按键或 COM 状态。
    // 入参：无。
    // 返回：无返回值。
    ~DesktopScope()
    {
        INPUT key{};
        key.type = INPUT_KEYBOARD;
        key.ki.wVk = VK_CONTROL;
        key.ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(1, &key, sizeof(INPUT));
        INPUT release{};
        release.type = INPUT_MOUSE;
        release.mi.dwFlags = MOUSEEVENTF_LEFTUP;
        SendInput(1, &release, sizeof(INPUT));
        SetCursorPos(this->cursor.x, this->cursor.y);
        if (IsWindow(this->foreground))
            SetForegroundWindow(this->foreground);
        if (SUCCEEDED(this->com))
            CoUninitialize();
    }
};

// 验证点击最下层 A 后真实前台和键盘焦点属于 A，而 Z 序仍为 C/B/A。
// 入参：无运行时入参；测试宏参数为注册名称。
// 返回：无返回值；真实输入或焦点失败按失败报告，无可见闪烁仍需人工验收。
TEST(PinWindowGateTest, real_click_focus_keeps_three_window_order)
{
    BOOL composition = FALSE;
    if (FAILED(DwmIsCompositionEnabled(&composition)) || !composition)
        GTEST_SKIP() << "Desktop composition unavailable.";
    DesktopScope desktop;
    ASSERT_TRUE(SUCCEEDED(desktop.com) || desktop.com == RPC_E_CHANGED_MODE);
    RECT work{};
    ASSERT_TRUE(SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0));
    open_st::PinWindowCallbacks callbacks;
    // 提供隔离的测试标题，不读取产品业务资源。
    // 入参：key 为被请求的文本键。
    // 返回：固定窗口标题。
    callbacks.text = [](std::string_view) { return std::wstring(L"Open-ST pin gate"); };
    open_st::PinWindowManager manager(GetModuleHandleW(nullptr), std::move(callbacks));
    std::vector<std::uint8_t> pixels(200U * 160U * 4U, 127);
    std::wstring error;
    const std::shared_ptr<const open_st::PinImage> image = open_st::PinImage::Create(200, 160, 800, pixels, error);
    ASSERT_NE(image, nullptr);
    std::array<open_st::PinId, 3> ids{};
    std::array<HWND, 3> windows{};
    for (std::size_t index = 0; index < ids.size(); ++index)
    {
        const LONG offset = static_cast<LONG>(index) * 55;
        ASSERT_TRUE(manager.Prepare(image, {work.left + 60 + offset, work.top + 60 + offset}, ids[index], error))
            << error;
        manager.Show(ids[index]);
        windows[index] = manager.Window(ids[index]);
    }
    Pump(120);
    const std::vector<HWND> expected{windows[2], windows[1], windows[0]};
    ASSERT_EQ(Order(windows), expected);
    ASSERT_TRUE(SetCursorPos(work.left + 80, work.top + 80));
    ASSERT_EQ(WindowFromPoint({work.left + 80, work.top + 80}), windows[0]);
    const RECT initial = Rectangle(windows[0]);
    ASSERT_TRUE(Wheel(WHEEL_DELTA));
    Pump(80);
    EXPECT_EQ(Rectangle(windows[0]).right, initial.right);
    std::array<INPUT, 2> click{};
    click[0].type = INPUT_MOUSE;
    click[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    click[1].type = INPUT_MOUSE;
    click[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    ASSERT_EQ(SendInput(static_cast<UINT>(click.size()), click.data(), sizeof(INPUT)), click.size());
    Pump(180);
    EXPECT_EQ(GetForegroundWindow(), windows[0]);
    EXPECT_EQ(GetFocus(), windows[0]);
    EXPECT_EQ(Order(windows), expected);
    // 保持 A 点击资格，真实拖动只改变位置，不改变 C/B/A 顺序。
    // 与前一次点击隔开系统双击间隔，避免把拖动起点解释为恢复原始缩放的双击。
    Pump(GetDoubleClickTime() + 20);
    ASSERT_TRUE(SetCursorPos(work.left + 80, work.top + 80));
    ASSERT_EQ(SendInput(1, click.data(), sizeof(INPUT)), 1U);
    Pump(40);
    ASSERT_TRUE(SetCursorPos(work.left + 100, work.top + 90));
    Pump(80);
    ASSERT_EQ(SendInput(1, &click[1], sizeof(INPUT)), 1U);
    Pump(40);
    EXPECT_EQ(Rectangle(windows[0]).left, initial.left + 20);
    EXPECT_EQ(Rectangle(windows[0]).top, initial.top + 10);
    EXPECT_EQ(Order(windows), expected);
    ASSERT_TRUE(Wheel(WHEEL_DELTA));
    Pump(80);
    const RECT zoomed = Rectangle(windows[0]);
    EXPECT_GT(zoomed.right - zoomed.left, initial.right - initial.left);
    EXPECT_EQ(Order(windows), expected);
    INPUT control{};
    control.type = INPUT_KEYBOARD;
    control.ki.wVk = VK_CONTROL;
    ASSERT_EQ(SendInput(1, &control, sizeof(INPUT)), 1U);
    Pump(30);
    ASSERT_TRUE(Wheel(-WHEEL_DELTA));
    Pump(80);
    control.ki.dwFlags = KEYEVENTF_KEYUP;
    ASSERT_EQ(SendInput(1, &control, sizeof(INPUT)), 1U);
    Pump(30);
    EXPECT_NEAR(open_st::PinWindowGateTestAccess::Opacity(manager, ids[0]), 0.95F, 0.001F);
    // 光标移到 B 的可见区域，但焦点仍属于 A，两图均不应响应滚轮。
    ASSERT_TRUE(SetCursorPos(work.left + 125, work.top + 125));
    ASSERT_EQ(WindowFromPoint({work.left + 125, work.top + 125}), windows[1]);
    const RECT beforeA = Rectangle(windows[0]);
    const RECT beforeB = Rectangle(windows[1]);
    ASSERT_TRUE(Wheel(WHEEL_DELTA));
    Pump(80);
    EXPECT_EQ(Rectangle(windows[0]).right, beforeA.right);
    EXPECT_EQ(Rectangle(windows[1]).right, beforeB.right);
    ASSERT_TRUE(manager.BeginCapture(error)) << error;
    EXPECT_TRUE(IsWindowVisible(windows[0]));
    EXPECT_TRUE(IsWindowVisible(windows[1]));
    EXPECT_TRUE(IsWindowVisible(windows[2]));
    ASSERT_TRUE(SetCursorPos(work.left + 100, work.top + 90));
    ASSERT_EQ(WindowFromPoint({work.left + 100, work.top + 90}), windows[0]);
    const RECT pausedA = Rectangle(windows[0]);
    ASSERT_TRUE(Wheel(WHEEL_DELTA));
    Pump(60);
    EXPECT_EQ(Rectangle(windows[0]).right, pausedA.right);
    manager.EndCapture();
    Pump(80);
    EXPECT_EQ(Order(windows), expected);
    EXPECT_NEAR(open_st::PinWindowGateTestAccess::Opacity(manager, ids[0]), 0.95F, 0.001F);
    ASSERT_TRUE(Wheel(WHEEL_DELTA));
    Pump(60);
    EXPECT_EQ(Rectangle(windows[0]).right, pausedA.right);
    open_st::PinWindowGateTestAccess::Raise(manager, ids[0]);
    Pump(60);
    EXPECT_EQ(Order(windows), (std::vector<HWND>{windows[0], windows[2], windows[1]}));
    ASSERT_TRUE(manager.BeginModal(ids[0]));
    manager.Close(ids[0]);
    EXPECT_TRUE(IsWindow(windows[0]));
    manager.EndModal();
    EXPECT_FALSE(IsWindow(windows[0]));
    manager.Close(ids[0]);
    Pump(50);
    EXPECT_EQ(Order(windows), (std::vector<HWND>{windows[2], windows[1]}));
}
} // namespace
