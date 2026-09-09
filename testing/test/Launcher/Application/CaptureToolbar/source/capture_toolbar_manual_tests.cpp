// 提供截图工具栏的独立人工交互宿主，使用模拟命令等待真人验收。

#include <capture_toolbar.h>

#include <array>
#include <gtest/gtest.h>
#include <string_view>

namespace
{
constexpr wchar_t MANUAL_CLASS[] = L"OpenST.CaptureToolbar.ManualTest";
constexpr UINT COMMAND_MESSAGE = WM_APP + 17;

// 仅改变本测试 UI 线程的 DPI 上下文，结束后恢复原状态。
class DpiScope final
{
  public:
    // 为人工工具栏测试临时启用 Per-Monitor V2 DPI 坐标语义。
    // 入参：无显式入参。
    // 返回：无返回值。
    DpiScope() : previous_(SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {}
    // 恢复人工工具栏测试前的线程 DPI 上下文。
    // 入参：无显式入参。
    // 返回：无返回值。
    ~DpiScope()
    {
        if (this->previous_ != nullptr)
        {
            SetThreadDpiAwarenessContext(this->previous_);
        }
    }

  private:
    DPI_AWARENESS_CONTEXT previous_{};
};

struct ManualWindow final
{
    open_st::CaptureToolbar toolbar;
    HWND window{};
    const wchar_t* status{L"请悬停、点击按钮，或移动窗口检查布局。保存、复制仅更新此提示。"};
    bool failed{};

    // 提前断开工具栏回调，再清理可能因断言失败而遗留的宿主。
    // 入参：无显式入参。
    // 返回：无返回值。
    ~ManualWindow()
    {
        this->toolbar.Close();
        if (IsWindow(this->window))
        {
            SetWindowLongPtrW(this->window, GWLP_USERDATA, 0);
            DestroyWindow(this->window);
        }
    }

    // 计算人工测试宿主中的示意选区，使边距随当前 DPI 缩放。
    // 入参：无显式入参。
    // 返回：宿主客户区内示意选区的物理像素矩形，边距按当前 DPI 从 DIP 换算。
    RECT Selection() const noexcept
    {
        RECT client{};
        GetClientRect(this->window, &client);
        const UINT dpi = GetDpiForWindow(this->window);
        const int horizontal = MulDiv(55, static_cast<int>(dpi), 96);
        const int top = MulDiv(65, static_cast<int>(dpi), 96);
        const int bottom = MulDiv(105, static_cast<int>(dpi), 96);
        return {horizontal, top, client.right - horizontal, client.bottom - bottom};
    }

    // 将示意选区适配为物理屏幕坐标，不借用真实截图模块。
    // 入参：无显式入参。
    // 返回：无返回值。
    void Place()
    {
        if (this->toolbar.NativeHandle() == nullptr || IsIconic(this->window))
        {
            this->toolbar.Hide();
            return;
        }
        RECT selection = this->Selection();
        if (selection.left >= selection.right || selection.top >= selection.bottom)
        {
            this->toolbar.Hide();
            return;
        }
        MapWindowPoints(this->window, nullptr, reinterpret_cast<POINT*>(&selection), 2);
        MONITORINFO monitor{sizeof(monitor)};
        if (!GetMonitorInfoW(MonitorFromWindow(this->window, MONITOR_DEFAULTTONEAREST), &monitor) ||
            !this->toolbar.UpdatePlacement(selection, monitor.rcWork, GetDpiForWindow(this->window)).success ||
            !this->toolbar.Show(1).success)
        {
            this->failed = true;
            this->status = L"工具栏布局失败，请关闭窗口查看测试结果。";
        }
    }

    // 用系统借用画刷绘制背景和选区，不捕获桌面也不创建输出文件。
    // 入参：无显式入参。
    // 返回：无返回值。
    void Paint() const noexcept
    {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(this->window, &paint);
        RECT client{};
        GetClientRect(this->window, &client);
        HBRUSH brush = static_cast<HBRUSH>(GetStockObject(DC_BRUSH));
        SetDCBrushColor(dc, RGB(35, 35, 35));
        FillRect(dc, &client, brush);
        const RECT selection = this->Selection();
        SetDCBrushColor(dc, RGB(52, 52, 52));
        FillRect(dc, &selection, brush);
        SetDCBrushColor(dc, RGB(60, 165, 232));
        FrameRect(dc, &selection, brush);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(238, 238, 238));
        HGDIOBJ previous = SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
        RECT instruction{20, 12, client.right - 20, selection.top - 8};
        DrawTextW(dc, L"截图工具栏人工验收：点击叉号或关闭宿主窗口结束。可拖到其他屏幕检查 DPI。", -1, &instruction,
                  DT_LEFT | DT_WORDBREAK);
        RECT feedback{20, client.bottom - 48, client.right - 20, client.bottom - 8};
        DrawTextW(dc, this->status, -1, &feedback, DT_LEFT | DT_WORDBREAK);
        SelectObject(dc, previous);
        EndPaint(this->window, &paint);
    }

    // 窗口过程负责假命令和移动通知；按钮回调只投递消息，避免同步销毁。
    // 入参：window 为接收消息的测试宿主窗口；message 为消息编号；wParam、lParam 为消息专属附加数据。
    // 返回：已处理消息对应的 Win32 结果；其余消息返回 DefWindowProcW 的处理结果。
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        ManualWindow* state = reinterpret_cast<ManualWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            state = static_cast<ManualWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            state->window = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        }
        if (state == nullptr)
        {
            return DefWindowProcW(window, message, wParam, lParam);
        }
        try
        {
            switch (message)
            {
            case WM_PAINT:
                state->Paint();
                return 0;
            case WM_ERASEBKGND:
                return 1;
            case WM_MOVE:
            case WM_SIZE:
                state->Place();
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            case WM_DPICHANGED:
            {
                const RECT& rectangle = *reinterpret_cast<const RECT*>(lParam);
                SetWindowPos(window, nullptr, rectangle.left, rectangle.top, rectangle.right - rectangle.left,
                             rectangle.bottom - rectangle.top, SWP_NOZORDER | SWP_NOACTIVATE);
                state->Place();
                return 0;
            }
            case COMMAND_MESSAGE:
                if (static_cast<open_st::CaptureToolbarCommand>(wParam) == open_st::CaptureToolbarCommand::Cancel)
                {
                    SendMessageW(window, WM_CLOSE, 0, 0);
                    return 0;
                }
                state->status =
                    static_cast<open_st::CaptureToolbarCommand>(wParam) == open_st::CaptureToolbarCommand::Save
                        ? L"已收到保存命令。此人工测试没有写入文件，可继续检查或关闭。"
                        : L"已收到复制命令。此人工测试没有改动剪贴板，可继续检查或关闭。";
                state->toolbar.SetBusy(false);
                state->Place();
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            case WM_CLOSE:
                state->toolbar.Close();
                DestroyWindow(window);
                return 0;
            case WM_NCDESTROY:
                state->window = nullptr;
                SetWindowLongPtrW(window, GWLP_USERDATA, 0);
                // 外部同步关窗可能发生在 GetMessage 内，投递空消息使循环重新检查窗口是否存在。
                PostThreadMessageW(GetCurrentThreadId(), WM_NULL, 0, 0);
                break;
            default:
                break;
            }
        }
        catch (...)
        {
            state->failed = true;
            state->status = L"工具栏遇到异常，请关闭窗口查看测试结果。";
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }
};

// 验证显式开启后主动显示真实工具栏并无限等待人工关闭；复制、保存使用无输出假命令。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST(ToolbarManualTest, waits_for_user_close)
{
    std::array<wchar_t, 8> enabled{};
    if (GetEnvironmentVariableW(L"OPEN_ST_INTERACTIVE_UI_TESTS", enabled.data(), static_cast<DWORD>(enabled.size())) !=
            1 ||
        enabled[0] != L'1')
    {
        GTEST_SKIP() << "Set OPEN_ST_INTERACTIVE_UI_TESTS=1 to inspect and close the toolbar manually.";
    }
    DpiScope dpi;
    ManualWindow state;
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW windowClass{};
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = ManualWindow::WindowProc;
    windowClass.lpszClassName = MANUAL_CLASS;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    ASSERT_TRUE(RegisterClassW(&windowClass) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS);
    ASSERT_NE(CreateWindowExW(0, MANUAL_CLASS, L"Open-ST 截图工具栏人工验收", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                              CW_USEDEFAULT, 800, 520, nullptr, nullptr, instance, &state),
              nullptr);
    const open_st::ToolbarResult created = state.toolbar.Create(
        instance, state.window,
        {{open_st::CaptureToolbarCommand::Cancel, open_st::ToolbarIcon::Cancel, "cancel", 0},
         {open_st::CaptureToolbarCommand::Save, open_st::ToolbarIcon::Save, "save", 1},
         {open_st::CaptureToolbarCommand::Copy, open_st::ToolbarIcon::Copy, "copy", 1}},
        // 提供人工工具栏标题和按钮提示文本，未知键原样显示。
        // 入参：key 为待查询的测试界面文本键。
        // 返回：人工工具栏标题或按键对应的取消、保存、复制提示文字。
        [](std::string_view key)
        {
            if (key == "capture.toolbar.title")
            {
                return std::wstring(L"截图工具栏");
            }
            return key == "cancel" ? std::wstring(L"取消截图")
                   : key == "save" ? std::wstring(L"保存（Ctrl+S）")
                                   : std::wstring(L"复制（Ctrl+C / Enter）");
        },
        // 将工具栏命令和代次异步投递给测试宿主，不执行真实业务输出。
        // 入参：command 为人工工具栏提交的命令；token 为工具栏携带的代次，按原值投递给宿主。
        // 返回：成功向测试宿主投递命令消息时为 true，否则为 false。
        [&state](open_st::CaptureToolbarCommand command, std::uint64_t token)
        {
            return PostMessageW(state.window, COMMAND_MESSAGE, static_cast<WPARAM>(command),
                                static_cast<LPARAM>(token)) != FALSE;
        });
    ASSERT_TRUE(created.success);
    ShowWindow(state.window, SW_SHOW);
    // 显式人工验收必须可见，不继承启动器可能提供的 SW_HIDE 首次显示状态。
    ASSERT_TRUE(
        SetWindowPos(state.window, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_SHOWWINDOW));
    UpdateWindow(state.window);
    state.Place();
    MSG message{};
    while (IsWindow(state.window))
    {
        const BOOL received = GetMessageW(&message, nullptr, 0, 0);
        if (received <= 0)
        {
            state.failed = true;
            if (received == 0)
            {
                PostQuitMessage(static_cast<int>(message.wParam));
            }
            break;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    EXPECT_FALSE(state.failed);
}
} // namespace
