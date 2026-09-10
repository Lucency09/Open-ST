// 提供三张重叠贴图的人工视觉验收，显式开启后等待真人关闭全部窗口。

#include <array>
#include <dwmapi.h>
#include <gtest/gtest.h>
#include <objbase.h>
#include <pin_image.h>
#include <pin_window_manager.h>
#include <vector>

namespace
{
// 分派人工验收控制窗按钮，使用户能一次关闭全部测试贴图。
// 入参：window/message/wParam/lParam 为原生消息；管理器仅在控制窗存活期间借用。
// 返回：窗口消息结果；关闭控制窗等价于用户关闭全部测试贴图。
LRESULT CALLBACK ControlProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_NCCREATE)
    {
        const CREATESTRUCTW* creation = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(creation->lpCreateParams));
    }
    open_st::PinWindowManager* manager =
        reinterpret_cast<open_st::PinWindowManager*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (manager && message == WM_COMMAND)
    {
        if (LOWORD(wParam) == 2)
            manager->CloseAll();
        return 0;
    }
    if (manager && message == WM_CLOSE)
    {
        manager->CloseAll();
        DestroyWindow(window);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

struct ControlWindow final
{
    HWND handle{};
    // 在测试结束时销毁控制窗，不发送线程退出通知。
    // 入参：无。
    // 返回：无返回值；析构发生在管理器释放之前。
    ~ControlWindow()
    {
        if (IsWindow(this->handle))
            DestroyWindow(this->handle);
    }
};

// 验证点击下层贴图、拖动、菜单和滚轮时的真实视觉表现，自动测试不代替人工判断。
// 入参：无运行时入参；测试宏参数为注册名称。
// 返回：无返回值；默认跳过，显式开启后直到真人关闭全部贴图才结束。
TEST(PinWindowManualTest, waits_for_user_close)
{
    wchar_t enabled[2]{};
    if (GetEnvironmentVariableW(L"OPEN_ST_INTERACTIVE_UI_TESTS", enabled, 2) != 1 || enabled[0] != L'1')
    {
        GTEST_SKIP() << "Set OPEN_ST_INTERACTIVE_UI_TESTS=1 for the manual three-window visual gate.";
    }
    BOOL composition = FALSE;
    if (FAILED(DwmIsCompositionEnabled(&composition)) || !composition)
    {
        GTEST_SKIP() << "Desktop composition is unavailable.";
    }
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ASSERT_TRUE(SUCCEEDED(com) || com == RPC_E_CHANGED_MODE);
    {
        open_st::PinWindowCallbacks callbacks;
        // 使用测试文字标识菜单，避免人工窗口依赖业务资源或触发复制保存副作用。
        // 入参：key 为请求的文本键。
        // 返回：可识别的菜单文字。
        callbacks.text = [](std::string_view key)
        {
            if (key == "pin.title")
                return std::wstring(L"Open-ST 贴图验收");
            if (key == "pin.raise")
                return std::wstring(L"调整为最上层");
            if (key == "pin.close")
                return std::wstring(L"关闭");
            if (key == "pin.copy")
                return std::wstring(L"复制（测试中无副作用）");
            if (key == "pin.save")
                return std::wstring(L"保存（测试中无副作用）");
            return std::wstring(key.begin(), key.end());
        };
        open_st::PinWindowManager manager(GetModuleHandleW(nullptr), std::move(callbacks));
        const std::array<std::array<std::uint8_t, 4>, 3> colors{
            {{{70, 80, 220, 0}}, {{70, 200, 90, 0}}, {{220, 110, 70, 0}}}};
        RECT work{};
        EXPECT_TRUE(SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0));
        WNDCLASSW controlClass{};
        controlClass.hInstance = GetModuleHandleW(nullptr);
        controlClass.lpfnWndProc = ControlProc;
        controlClass.lpszClassName = L"OpenST.PinManualControl";
        controlClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        controlClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        ASSERT_TRUE(RegisterClassW(&controlClass) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS);
        ControlWindow control;
        control.handle = CreateWindowExW(WS_EX_TOPMOST, controlClass.lpszClassName, L"贴图人工验收控制",
                                         WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE, work.right - 440,
                                         work.top + 80, 420, 180, nullptr, nullptr, controlClass.hInstance, &manager);
        ASSERT_NE(control.handle, nullptr);
        ASSERT_NE(CreateWindowW(L"STATIC", L"红 A 在底，绿 B 居中，蓝 C 在顶。\n点击、拖动红图，观察交叠区域有无闪烁。",
                                WS_CHILD | WS_VISIBLE, 12, 12, 390, 45, control.handle, nullptr, controlClass.hInstance,
                                nullptr),
                  nullptr);
        ASSERT_NE(CreateWindowW(L"BUTTON", L"关闭全部并结束测试", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 12, 75, 190,
                                30, control.handle, reinterpret_cast<HMENU>(2), controlClass.hInstance, nullptr),
                  nullptr);
        for (int index = 0; index < 3; ++index)
        {
            std::vector<std::uint8_t> pixels(320U * 240U * 4U);
            const std::array<std::uint8_t, 4>& color = colors[static_cast<std::size_t>(index)];
            for (std::size_t offset = 0; offset < pixels.size(); offset += 4)
                for (std::size_t channel = 0; channel < color.size(); ++channel)
                    pixels[offset + channel] = color[channel];
            std::wstring error;
            const std::shared_ptr<const open_st::PinImage> image =
                open_st::PinImage::Create(320, 240, 1280, pixels, error);
            open_st::PinId id{};
            const bool prepared =
                manager.Prepare(image, {work.left + 80 + index * 90, work.top + 80 + index * 60}, id, error);
            EXPECT_TRUE(prepared) << error;
            if (!prepared)
                break;
            manager.Show(id);
        }
        while (!testing::Test::HasFailure() && manager.Count() != 0)
        {
            MSG message{};
            const BOOL result = GetMessageW(&message, nullptr, 0, 0);
            if (result <= 0)
            {
                ADD_FAILURE() << "Message loop ended before the user closed every pin.";
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (SUCCEEDED(com))
        CoUninitialize();
}
} // namespace
