#include "capture_overlay_session.h"

#include <gtest/gtest.h>
#include <desktop_capturer.h>
#include <desktop_preview.h>
#include <selection_model.h>
#include <iostream>

namespace
{
constexpr wchar_t TEST_CLASS[] = L"OpenST.TestCaptureSession";
constexpr wchar_t PROBE_PROPERTY[] = L"OpenST.DestroyProbe";

struct DestroyProbe final
{
    int destroyed{};
    bool detached{true};
};

// 通过独立窗口属性记录销毁时的 App 绑定，避免探针本身依赖待清空的 GWLP_USERDATA。
LRESULT CALLBACK ProbeWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_DESTROY)
    {
        DestroyProbe* probe = static_cast<DestroyProbe*>(GetPropW(window, PROBE_PROPERTY));
        if (probe != nullptr)
        {
            ++probe->destroyed;
            probe->detached = probe->detached && GetWindowLongPtrW(window, GWLP_USERDATA) == 0;
            RemovePropW(window, PROBE_PROPERTY);
        }
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

// 创建不显示的测试窗口；登记成功后由 CaptureOverlaySession 接管销毁责任。
HWND CreateProbeWindow(DestroyProbe& probe, open_st::RectI bounds = {0, 0, 16, 16})
{
    WNDCLASSW windowClass{};
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpfnWndProc = ProbeWindowProc;
    windowClass.lpszClassName = TEST_CLASS;
    if (RegisterClassW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        return nullptr;
    }
    const HWND window = CreateWindowExW(0, TEST_CLASS, L"", WS_POPUP, bounds.left, bounds.top,
                                        bounds.Width(), bounds.Height(), nullptr, nullptr,
                                        windowClass.hInstance, nullptr);
    if (window != nullptr)
    {
        SetPropW(window, PROBE_PROPERTY, &probe);
        SetWindowLongPtrW(window, GWLP_USERDATA, 1);
    }
    return window;
}

// 临时使用产品相同的 PMv2 坐标上下文，测试结束后恢复调用线程状态。
class PhysicalPixelsGuard final
{
  public:
    // 仅改变本测试线程，不影响同时存在的其他测试线程或系统设置。
    PhysicalPixelsGuard() : previous_(SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {}
    // 恢复进入测试前的 DPI 上下文。
    ~PhysicalPixelsGuard()
    {
        if (this->previous_ != nullptr)
        {
            SetThreadDpiAwarenessContext(this->previous_);
        }
    }
    // 禁止复制临时线程状态恢复责任。
    PhysicalPixelsGuard(const PhysicalPixelsGuard&) = delete;
    // 禁止复制赋值，避免多次恢复线程上下文。
    PhysicalPixelsGuard& operator=(const PhysicalPixelsGuard&) = delete;

  private:
    DPI_AWARENESS_CONTEXT previous_{};
};
} // namespace

// 验证多个隐藏输出只关闭一次，且任何 WM_DESTROY 发生前都已经解除 App 回调绑定。
TEST(CaptureOverlaySessionTest, closes_all_outputs_after_detaching_callbacks)
{
    DestroyProbe probe;
    open_st::CaptureOverlaySession session;
    const HWND first = CreateProbeWindow(probe);
    ASSERT_NE(first, nullptr);
    session.Add(first);
    const HWND second = CreateProbeWindow(probe);
    ASSERT_NE(second, nullptr);
    session.Add(second);
    ASSERT_NE(session.Find(first), nullptr);
    ASSERT_NE(session.Find(second), nullptr);
    session.Close();
    session.Close();
    EXPECT_EQ(probe.destroyed, 2);
    EXPECT_TRUE(probe.detached);
    EXPECT_EQ(IsWindow(first), FALSE);
    EXPECT_EQ(IsWindow(second), FALSE);
    EXPECT_EQ(session.Find(first), nullptr);
}

// 验证初始化失败时空记录与已创建输出可以一起回收，不要求已经建立渲染器。
TEST(CaptureOverlaySessionTest, partially_prepared_session_is_released_by_destructor)
{
    DestroyProbe probe;
    HWND window{};
    {
        open_st::CaptureOverlaySession session;
        session.Add(nullptr);
        window = CreateProbeWindow(probe);
        ASSERT_NE(window, nullptr);
        session.Add(window);
    }
    EXPECT_EQ(probe.destroyed, 1);
    EXPECT_TRUE(probe.detached);
    EXPECT_EQ(IsWindow(window), FALSE);
}

// 验证同一个物理像素选区可以跨负坐标双屏创建、移动和缩放，无显示器局部坐标跳变。
TEST(CaptureOverlaySessionTest, shared_selection_crosses_monitor_boundary)
{
    open_st::SelectionModel selection;
    selection.SetBounds({-1920, 0, 2560, 1440});
    ASSERT_TRUE(selection.Begin({-300, 200}));
    ASSERT_TRUE(selection.End({400, 800}));
    ASSERT_TRUE(selection.Begin({100, 400}));
    ASSERT_TRUE(selection.End({-400, 450}));
    EXPECT_EQ(selection.Snapshot().rectangle.left, -800);
    EXPECT_EQ(selection.Snapshot().rectangle.right, -100);
    ASSERT_TRUE(selection.Begin({-100, 850}));
    ASSERT_TRUE(selection.End({900, 1000}));
    EXPECT_EQ(selection.Snapshot().rectangle.left, -800);
    EXPECT_EQ(selection.Snapshot().rectangle.right, 900);
    EXPECT_EQ(selection.Snapshot().rectangle.bottom, 1000);
}

// 在真实交互式桌面验证原生捕获到每屏隐藏交换链的完整准备链路；不显示窗口或保存截图。
TEST(CaptureOverlaySessionTest, captures_and_prepares_all_real_outputs)
{
    const HDESK desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (desktop == nullptr || GetSystemMetrics(SM_CMONITORS) <= 0)
    {
        if (desktop != nullptr)
        {
            CloseDesktop(desktop);
        }
        GTEST_SKIP() << "No active interactive desktop is available.";
    }
    CloseDesktop(desktop);
    PhysicalPixelsGuard physicalPixels;
    open_st::DesktopCapturer capturer;
    open_st::FrozenDesktopFrame frame;
    std::wstring error;
    ASSERT_TRUE(capturer.Capture(frame, error)) << error;
    DestroyProbe probe;
    open_st::CaptureOverlaySession session;
    for (const open_st::CapturedOutputPlane& plane : frame.Outputs())
    {
        open_st::OutputPreviewFrame preview;
        ASSERT_TRUE(open_st::BuildOutputPreview(plane, preview, error)) << error;
        const HWND window = CreateProbeWindow(probe, plane.Bounds());
        ASSERT_NE(window, nullptr);
        open_st::CaptureOverlayOutput& output = session.Add(window);
        output.renderer = std::make_unique<open_st::OverlayRenderer>();
        ASSERT_TRUE(output.renderer->Initialize(window, preview, std::nullopt, error)) << error;
        ASSERT_TRUE(output.renderer->Render({}, error)) << error;
        std::wcout << plane.ColorMetadata().deviceName.data() << L" format="
                   << static_cast<int>(preview.pixelFormat) << L" white_scale=" << preview.uiWhiteScale
                   << L" compatibility=" << preview.compatibilityMode << std::endl;
    }
    session.Close();
    EXPECT_EQ(probe.destroyed, static_cast<int>(frame.Outputs().size()));
    EXPECT_TRUE(probe.detached);
}
