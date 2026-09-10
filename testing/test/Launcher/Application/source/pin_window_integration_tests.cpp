// 验证 App 贴图命令的真实消息排队、请求失效与关闭边界，避免触发剪贴板或文件副作用。

#include "capture_command_gate.h"
#include "capture_overlay_session.h"
#include "pin_window.h"
#include <app.h>
#include <array>
#include <capture_toolbar.h>
#include <frozen_desktop_frame.h>
#include <gtest/gtest.h>
#include <objbase.h>
#include <pin_image.h>
#include <pin_window_manager.h>
#include <selection_model.h>
#include <selection_output_renderer.h>
#include <vector>

namespace open_st
{
struct AppPinTestAccess
{
    // 借用正式管理器，测试公开的可见性和模态生命周期，不改变生产接口。
    // 入参：app 为隔离宿主。
    // 返回：仅在 app 存活期间有效的管理器引用。
    static PinWindowManager& Manager(App& app)
    {
        return *app.pinManager_;
    }
    // 经正式取消入口释放合成截图，验证结束旧贴图输入暂停的应用接线。
    // 入参：app 为隔离宿主。
    // 返回：无返回值。
    static void CancelCapture(App& app)
    {
        app.CloseOverlay();
    }
    // 借用 App 消息窗口，供真实关闭消息和销毁断言使用。
    // 入参：app 为隔离宿主。
    // 返回：借用 HWND，不转移窗口所有权。
    static HWND MessageWindow(const App& app)
    {
        return app.messageWindow_;
    }
    // 查询正式提示窗口 owner，避免测试重新实现窗口选择规则。
    // 入参：app 为隔离宿主。
    // 返回：App 当前选择的借用窗口句柄。
    static HWND DialogOwner(const App& app)
    {
        return app.DialogOwner();
    }
    // 消费管理器的异步停止通知，保留正式 App 退出准入路径。
    // 入参：app 为隔离宿主。
    // 返回：找到并分派停止通知时 true，否则 false。
    static bool DispatchStopped(App& app)
    {
        MSG message{};
        if (!PeekMessageW(&message, app.messageWindow_, WM_APP + 6, WM_APP + 6, PM_REMOVE) ||
            message.message != WM_APP + 6)
            return false;
        DispatchMessageW(&message);
        return true;
    }
    // 按主循环顺序消费完整线程队列，等待停止通知之后的最终退出消息。
    // 入参：quitMessage 接收实际取出的 WM_QUIT 及退出码。
    // 返回：两秒内收到 WM_QUIT 时 true；超时或等待失败时 false，不自行生成退出消息。
    static bool PumpUntilQuit(MSG& quitMessage)
    {
        const ULONGLONG deadline = GetTickCount64() + 2000;
        while (GetTickCount64() < deadline)
        {
            MSG message{};
            if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                if (message.message == WM_QUIT)
                {
                    quitMessage = message;
                    return true;
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            else if (MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE) == WAIT_FAILED)
            {
                return false;
            }
        }
        return false;
    }
    // 枚举仅属于目标管理器的存活贴图 ID，供成功创建后识别新窗口。
    // 入参：app 为隔离宿主。
    // 返回：当前线程中目标管理器的窗口 ID 集合，不依赖 ID 连续性或枚举顺序。
    static std::vector<PinId> WindowIds(App& app)
    {
        struct Search
        {
            App* app;
            std::vector<PinId> ids;
        } search{&app, {}};
        EnumThreadWindows(
            GetCurrentThreadId(),
            // 只读取已核对类名和管理器归属的窗口记录。
            // 入参：window 为线程窗口；parameter 为借用的搜索状态。
            // 返回：始终继续枚举，完整收集本管理器窗口。
            [](HWND window, LPARAM parameter) -> BOOL
            {
                std::array<wchar_t, 64> name{};
                GetClassNameW(window, name.data(), static_cast<int>(name.size()));
                if (std::wstring_view(name.data()) != L"OpenST.PinWindow")
                    return TRUE;
                Search& state = *reinterpret_cast<Search*>(parameter);
                const PinWindow* pin = reinterpret_cast<const PinWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
                if (pin && state.app->pinManager_->Window(pin->Id()) == window)
                    state.ids.push_back(pin->Id());
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&search));
        return search.ids;
    }
    // 注入可预测的冻结 SDR 图像和稳定选区，不读取实际桌面。
    // 入参：app 为隔离宿主。
    // 返回：无返回值，选区为原图内的 3×2 物理像素矩形。
    static void FrozenSelection(App& app)
    {
        app.CloseOverlay();
        if (!app.toolbarGate_)
            app.toolbarGate_ = std::make_unique<CaptureCommandGate>();
        app.toolbarGate_->Invalidate();
        app.overlaySession_ = std::make_unique<CaptureOverlaySession>();
        app.selectionModel_ = std::make_unique<SelectionModel>();
        app.selectionModel_->SetBounds({100, 100, 108, 106});
        (void)app.selectionModel_->Begin({102, 101});
        (void)app.selectionModel_->End({105, 103});
        std::vector<std::uint8_t> pixels(8U * 6U * 4U);
        for (std::size_t offset = 0; offset < pixels.size(); ++offset)
            pixels[offset] = static_cast<std::uint8_t>(offset);
        std::vector<CapturedOutputPlane> outputs;
        outputs.emplace_back(RectI{100, 100, 108, 106}, CapturedPixelFormat::Bgra8Unorm,
                             CapturedColorSpace::SdrGamma22P709, OutputColorMetadata{}, std::move(pixels));
        app.frozenDesktopFrame_ = std::make_unique<FrozenDesktopFrame>(RectI{100, 100, 108, 106}, std::move(outputs));
        app.outputRenderer_ = std::make_unique<SelectionOutputRenderer>();
    }
    // 经正式工具栏准入提交 Pin，不直接调用业务完成函数。
    // 入参：app 为隔离宿主。
    // 返回：成功投递真实 App 消息时 true。
    static bool PostSelection(App& app)
    {
        return app.PostToolbarCommand(CaptureToolbarCommand::Pin, app.toolbarGate_->Token());
    }
    // 分派一条真实工具栏消息，保持正式窗口过程和命令消费路径。
    // 入参：app 为隔离宿主。
    // 返回：已分派消息时 true。
    static bool DispatchSelection(App& app)
    {
        MSG message{};
        if (!PeekMessageW(&message, app.messageWindow_, WM_APP + 4, WM_APP + 4, PM_REMOVE))
            return false;
        DispatchMessageW(&message);
        return true;
    }
    // 检查成功贴图后原会话及冻结帧已经释放。
    // 入参：app 为隔离宿主。
    // 返回：两个源对象均已释放时 true。
    static bool CaptureReleased(const App& app)
    {
        return !app.overlaySession_ && !app.frozenDesktopFrame_;
    }
    // 检查未完成截图仍保留原冻结图和稳定选区。
    // 入参：app 为隔离宿主。
    // 返回：会话、冻结帧及原 3×2 选区均有效时 true。
    static bool SelectionRetained(const App& app)
    {
        if (!app.overlaySession_ || !app.frozenDesktopFrame_ || !app.selectionModel_)
            return false;
        const SelectionSnapshot selection = app.selectionModel_->Snapshot();
        return app.frozenDesktopFrame_->IsValid() && selection.phase == SelectionPhase::Selected &&
               selection.rectangle.left == 102 && selection.rectangle.top == 101 && selection.rectangle.right == 105 &&
               selection.rectangle.bottom == 103;
    }
    // 模拟截图输出服务不可用，验证 Pin 准入失败不会结束已有截图。
    // 入参：app 为隔离宿主。
    // 返回：无返回值，移除测试初始化的输出器。
    static void RemoveOutputRenderer(App& app)
    {
        app.outputRenderer_.reset();
    }
    // 在当前线程窗口中读取属于本 Manager 的唯一输出图像。
    // 入参：app 为隔离宿主。
    // 返回：共享不可变快照，缺少输出时为空。
    static std::shared_ptr<const PinImage> Output(App& app)
    {
        struct Search
        {
            App* app;
            std::shared_ptr<const PinImage> image;
        } search{&app, {}};
        EnumThreadWindows(
            GetCurrentThreadId(),
            // 只观察当前进程的贴图记录，确认 HWND 属于目标 Manager 后取得图像快照。
            // 入参：window 为线程窗口；parameter 为借用的搜索状态。
            // 返回：找到目标图像后停止枚举。
            [](HWND window, LPARAM parameter) -> BOOL
            {
                std::array<wchar_t, 64> name{};
                GetClassNameW(window, name.data(), static_cast<int>(name.size()));
                if (std::wstring_view(name.data()) != L"OpenST.PinWindow")
                    return TRUE;
                Search& state = *reinterpret_cast<Search*>(parameter);
                const PinWindow* pin = reinterpret_cast<const PinWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
                if (!pin || state.app->pinManager_->Window(pin->Id()) != window)
                    return TRUE;
                state.image = state.app->pinManager_->Image(pin->Id());
                return FALSE;
            },
            reinterpret_cast<LPARAM>(&search));
        return search.image;
    }
    // 建立真实消息窗口与隐藏贴图管理器，保留正式 App 回调接线。
    // 入参：app 为测试持有的应用实例。
    // 返回：消息窗口创建成功时 true，失败时 false。
    static bool Initialize(App& app)
    {
        if (!app.CreateMessageWindow())
            return false;
        app.pinManager_ = std::make_unique<PinWindowManager>(GetModuleHandleW(nullptr), app.MakePinCallbacks());
        return true;
    }
    // 准备隐藏的小图像窗口，供 ID 与消息生命周期测试使用。
    // 入参：app 为测试宿主。
    // 返回：新 ID；图像或窗口初始化失败时返回零。
    static PinId Prepare(App& app)
    {
        const std::array<std::uint8_t, 16> bytes{1, 2, 3, 0, 4, 5, 6, 7, 8, 9, 10, 0, 11, 12, 13, 255};
        std::wstring error;
        const std::shared_ptr<const PinImage> image = PinImage::Create(2, 2, 8, bytes, error);
        PinId id{};
        if (!image || !app.pinManager_->Prepare(image, {0, 0}, id, error))
            return 0;
        return id;
    }
    // 通过真实准入入口提交贴图命令。
    // 入参：app 为宿主；id 为目标贴图；command 为复制或保存命令。
    // 返回：命令成功预订并排队时 true，否则 false。
    static bool Post(App& app, PinId id, PinCommand command)
    {
        return app.PostPinCommand(id, command);
    }
    // 分派一条 App 贴图消息；隐藏窗口的输出命令会被消费时校验拒绝。
    // 入参：app 为目标宿主。
    // 返回：实际取得并分派消息时 true，否则 false。
    static bool Dispatch(App& app)
    {
        MSG message{};
        if (!PeekMessageW(&message, app.messageWindow_, WM_APP + 5, WM_APP + 5, PM_REMOVE))
            return false;
        DispatchMessageW(&message);
        return true;
    }
    // 检查宿主是否仍持有未消费的贴图请求。
    // 入参：app 为宿主。
    // 返回：待处理 ID 非零时 true。
    static bool Pending(const App& app)
    {
        return app.pendingPinId_ != 0;
    }
    // 关闭目标贴图，模拟菜单关闭先于排队业务命令执行。
    // 入参：app 为宿主；id 为拟关闭的贴图。
    // 返回：无返回值。
    static void Close(App& app, PinId id)
    {
        app.pinManager_->Close(id);
    }
    // 改变应用完成流程忙状态以验证准入。
    // 入参：app 为宿主；busy 为新的忙状态。
    // 返回：无返回值。
    static void SetBusy(App& app, bool busy)
    {
        app.completionBusy_ = busy;
    }
    // 查询存活图像，确认旧请求没有关闭后来的贴图。
    // 入参：app 为宿主；id 为拟查询图像的 ID。
    // 返回：目标仍存活时 true。
    static bool Exists(App& app, PinId id)
    {
        return app.pinManager_->Image(id) != nullptr;
    }
    // 撤销待处理请求，模拟进入截图等业务使旧请求失效。
    // 入参：app 为宿主。
    // 返回：无返回值，队列中的旧消息仍保留以验证消费检查。
    static void Invalidate(App& app)
    {
        app.pendingPinId_ = 0;
    }
    // 在消息窗口失效时提交命令，验证失败预订会被撤销。
    // 入参：app 为宿主；id 为存活贴图。
    // 返回：真实投递结果，预期为 false。
    static bool PostToInvalidWindow(App& app, PinId id)
    {
        const HWND original = app.messageWindow_;
        app.messageWindow_ = reinterpret_cast<HWND>(static_cast<INT_PTR>(-1));
        const bool accepted = app.PostPinCommand(id, PinCommand::Copy);
        app.messageWindow_ = original;
        return accepted;
    }
};

class PinQueueIntegrationTest : public ::testing::Test
{
  protected:
    // 创建隔离的 App 和消息窗口，不启动实际捕获或导出服务。
    // 入参：无。
    // 返回：无返回值，创建失败通过断言终止本用例。
    void SetUp() override
    {
        this->com_ = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        ASSERT_TRUE(SUCCEEDED(this->com_) || this->com_ == RPC_E_CHANGED_MODE);
        this->app_ = std::make_unique<App>(GetModuleHandleW(nullptr));
        ASSERT_TRUE(AppPinTestAccess::Initialize(*this->app_));
    }
    // 释放窗口并消费该 App 析构产生的退出通知。
    // 入参：无。
    // 返回：无返回值。
    void TearDown() override
    {
        if (this->app_)
            AppPinTestAccess::SetBusy(*this->app_, false);
        this->app_.reset();
        MSG message{};
        while (PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE))
        {
        }
        if (SUCCEEDED(this->com_))
            CoUninitialize();
    }
    HRESULT com_{E_FAIL};
    std::unique_ptr<App> app_;
};

// 验证 Pin 消息从合成冻结帧裁切独立原图，释放截图会话后所有输出字节仍与源选区一致。
// 入参：无运行入参，宏参数用于测试注册。
// 返回：无返回值；不捕获桌面、不访问系统剪贴板或写出文件。
TEST_F(PinQueueIntegrationTest, toolbar_pin_crops_frozen_pixels_and_releases_capture)
{
    AppPinTestAccess::FrozenSelection(*this->app_);
    ASSERT_TRUE(AppPinTestAccess::PostSelection(*this->app_));
    ASSERT_TRUE(AppPinTestAccess::DispatchSelection(*this->app_));
    EXPECT_TRUE(AppPinTestAccess::CaptureReleased(*this->app_));
    const std::shared_ptr<const PinImage> image = AppPinTestAccess::Output(*this->app_);
    ASSERT_NE(image, nullptr);
    ASSERT_EQ(image->Width(), 3);
    ASSERT_EQ(image->Height(), 2);
    ASSERT_EQ(image->Stride(), 12U);
    ASSERT_EQ(image->Pixels().size(), 24U);
    for (std::size_t row = 0; row < 2; ++row)
        for (std::size_t column = 0; column < 12; ++column)
            EXPECT_EQ(image->Pixels()[row * 12 + column], static_cast<std::uint8_t>((row + 1) * 32 + 8 + column));
    this->app_.reset();
    EXPECT_EQ(image->Pixels()[0], 40);
}

// 验证取消截图结束暂停，始终可见的旧贴图、隐藏的准备窗口及图像身份均保持不变。
// 入参：无运行入参，宏参数用于测试注册。
// 返回：无返回值；仅使用合成冻结图，不读取桌面或输出文件。
TEST_F(PinQueueIntegrationTest, cancelling_capture_keeps_pin_visibility_and_original_image)
{
    PinWindowManager& manager = AppPinTestAccess::Manager(*this->app_);
    const PinId visibleId = AppPinTestAccess::Prepare(*this->app_);
    const PinId hiddenId = AppPinTestAccess::Prepare(*this->app_);
    ASSERT_NE(visibleId, 0U);
    ASSERT_NE(hiddenId, 0U);
    const HWND visibleWindow = manager.Window(visibleId);
    const HWND hiddenWindow = manager.Window(hiddenId);
    const std::shared_ptr<const PinImage> originalImage = manager.Image(visibleId);
    manager.Show(visibleId);
    ASSERT_TRUE(IsWindowVisible(visibleWindow));
    ASSERT_FALSE(IsWindowVisible(hiddenWindow));
    AppPinTestAccess::FrozenSelection(*this->app_);
    std::wstring error;
    ASSERT_TRUE(manager.BeginCapture(error));
    ASSERT_TRUE(IsWindowVisible(visibleWindow));
    ASSERT_TRUE(AppPinTestAccess::SelectionRetained(*this->app_));

    AppPinTestAccess::CancelCapture(*this->app_);

    EXPECT_TRUE(AppPinTestAccess::CaptureReleased(*this->app_));
    EXPECT_EQ(manager.Count(), 2U);
    EXPECT_TRUE(IsWindowVisible(visibleWindow));
    EXPECT_FALSE(IsWindowVisible(hiddenWindow));
    EXPECT_EQ(manager.Window(visibleId), visibleWindow);
    EXPECT_EQ(manager.Image(visibleId), originalImage);
}

// 验证成功 Pin 释放截图后显示新图，旧图始终可见且原本隐藏的准备窗口不自动显示。
// 入参：无运行入参，宏参数用于测试注册。
// 返回：无返回值；经正式工具栏消息完成，不假定新贴图 ID 连续或窗口枚举顺序。
TEST_F(PinQueueIntegrationTest, successful_pin_keeps_old_windows_visible_and_shows_new_image)
{
    PinWindowManager& manager = AppPinTestAccess::Manager(*this->app_);
    const PinId visibleId = AppPinTestAccess::Prepare(*this->app_);
    const PinId hiddenId = AppPinTestAccess::Prepare(*this->app_);
    ASSERT_NE(visibleId, 0U);
    ASSERT_NE(hiddenId, 0U);
    const HWND visibleWindow = manager.Window(visibleId);
    const HWND hiddenWindow = manager.Window(hiddenId);
    const std::shared_ptr<const PinImage> originalImage = manager.Image(visibleId);
    manager.Show(visibleId);
    ASSERT_TRUE(IsWindowVisible(visibleWindow));
    AppPinTestAccess::FrozenSelection(*this->app_);
    std::wstring error;
    ASSERT_TRUE(manager.BeginCapture(error));
    ASSERT_TRUE(IsWindowVisible(visibleWindow));

    ASSERT_TRUE(AppPinTestAccess::PostSelection(*this->app_));
    ASSERT_TRUE(AppPinTestAccess::DispatchSelection(*this->app_));

    EXPECT_TRUE(AppPinTestAccess::CaptureReleased(*this->app_));
    EXPECT_EQ(manager.Count(), 3U);
    EXPECT_TRUE(IsWindowVisible(visibleWindow));
    EXPECT_FALSE(IsWindowVisible(hiddenWindow));
    EXPECT_EQ(manager.Image(visibleId), originalImage);
    const std::vector<PinId> ids = AppPinTestAccess::WindowIds(*this->app_);
    ASSERT_EQ(ids.size(), 3U);
    PinId newId{};
    for (const PinId id : ids)
        if (id != visibleId && id != hiddenId)
            newId = id;
    ASSERT_NE(newId, 0U);
    EXPECT_TRUE(IsWindowVisible(manager.Window(newId)));
    const std::shared_ptr<const PinImage> image = manager.Image(newId);
    ASSERT_NE(image, nullptr);
    EXPECT_NE(image, originalImage);
    EXPECT_EQ(image->Width(), 3);
    EXPECT_EQ(image->Height(), 2);
    EXPECT_FALSE(AppPinTestAccess::DispatchSelection(*this->app_));
}

// 验证 App 关闭在嵌套 Manager 模态中保留 HWND，最外层退出并消费通知后才发出 WM_QUIT。
// 入参：无运行入参，宏参数用于测试注册。
// 返回：无返回值；不打开真实导出对话框，通过真实窗口关闭和停止消息验证退出接线。
TEST_F(PinQueueIntegrationTest, app_close_waits_for_outermost_pin_modal_before_quit)
{
    PinWindowManager& manager = AppPinTestAccess::Manager(*this->app_);
    const PinId id = AppPinTestAccess::Prepare(*this->app_);
    ASSERT_NE(id, 0U);
    manager.Show(id);
    const HWND pinWindow = manager.Window(id);
    const HWND messageWindow = AppPinTestAccess::MessageWindow(*this->app_);
    MSG message{};
    ASSERT_FALSE(PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE));
    EXPECT_TRUE(manager.BeginModal(id));
    EXPECT_TRUE(manager.BeginModal(0));

    SendMessageW(messageWindow, WM_CLOSE, 0, 0);

    EXPECT_TRUE(manager.IsBusy());
    EXPECT_EQ(manager.Count(), 0U);
    EXPECT_EQ(manager.Image(id), nullptr);
    EXPECT_TRUE(IsWindow(pinWindow));
    EXPECT_TRUE(IsWindow(messageWindow));
    EXPECT_FALSE(AppPinTestAccess::Post(*this->app_, id, PinCommand::Copy));
    EXPECT_FALSE(PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE));
    EXPECT_FALSE(AppPinTestAccess::DispatchStopped(*this->app_));

    manager.EndModal();
    EXPECT_TRUE(manager.IsBusy());
    EXPECT_TRUE(IsWindow(pinWindow));
    EXPECT_FALSE(PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE));
    EXPECT_FALSE(AppPinTestAccess::DispatchStopped(*this->app_));

    manager.EndModal();
    EXPECT_FALSE(manager.IsBusy());
    EXPECT_FALSE(IsWindow(pinWindow));
    EXPECT_TRUE(IsWindow(messageWindow));
    EXPECT_FALSE(PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE));
    ASSERT_TRUE(AppPinTestAccess::DispatchStopped(*this->app_));
    ASSERT_TRUE(AppPinTestAccess::PumpUntilQuit(message));
    EXPECT_EQ(message.message, WM_QUIT);
    EXPECT_EQ(message.wParam, 0U);
    this->app_.reset();
    EXPECT_FALSE(IsWindow(messageWindow));
}

// 验证正式多屏遮罩显示能覆盖始终可见的旧贴图，包括未被激活的屏幕，且无需测试额外提层。
// 入参：无运行入参，宏参数用于测试注册。
// 返回：无返回值；普通原生遮罩不捕获桌面或渲染图像，退出时恢复原前台、焦点和 DPI 上下文。
TEST_F(PinQueueIntegrationTest, capture_session_show_covers_visible_pins_on_every_monitor)
{
    struct RestoreDesktop final
    {
        std::unique_ptr<App>& app;
        HWND foreground;
        HWND focus;
        DPI_AWARENESS_CONTEXT dpi;
        // 在遮罩会话已销毁后释放旧贴图，并恢复进入测试前的桌面状态。
        // 入参：无。
        // 返回：无返回值；已失效的窗口不再请求激活。
        ~RestoreDesktop()
        {
            this->app.reset();
            if (IsWindow(this->foreground))
                SetForegroundWindow(this->foreground);
            if (IsWindow(this->focus))
                SetFocus(this->focus);
            if (this->dpi)
                SetThreadDpiAwarenessContext(this->dpi);
        }
    } restore{this->app_, GetForegroundWindow(), GetFocus(),
              SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)};
    ASSERT_NE(restore.dpi, nullptr);
    struct MonitorSearch final
    {
        std::vector<MONITORINFO> monitors;
        bool valid{true};
    } search;
    ASSERT_TRUE(EnumDisplayMonitors(
        nullptr, nullptr,
        // 收集所有显示器的完整区域和工作区，避免仅验证当前活动显示器。
        // 入参：monitor 为枚举句柄；其余绘图参数未使用；parameter 为借用的收集状态。
        // 返回：成功时继续枚举，查询失败停止并标记失败。
        [](HMONITOR monitor, HDC, LPRECT, LPARAM parameter) -> BOOL
        {
            MonitorSearch& state = *reinterpret_cast<MonitorSearch*>(parameter);
            MONITORINFO information{sizeof(information)};
            if (!GetMonitorInfoW(monitor, &information))
            {
                state.valid = false;
                return FALSE;
            }
            state.monitors.push_back(information);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search)));
    ASSERT_TRUE(search.valid);
    ASSERT_FALSE(search.monitors.empty());
    PinWindowManager& manager = AppPinTestAccess::Manager(*this->app_);
    std::wstring error;
    const std::vector<std::uint8_t> pixels(32U * 24U * 4U, 127);
    const std::shared_ptr<const PinImage> image = PinImage::Create(32, 24, 128, pixels, error);
    ASSERT_NE(image, nullptr);
    std::vector<HWND> pinWindows;
    std::vector<POINT> hitPoints;
    for (const MONITORINFO& monitor : search.monitors)
    {
        ASSERT_GE(monitor.rcWork.right - monitor.rcWork.left, 32);
        ASSERT_GE(monitor.rcWork.bottom - monitor.rcWork.top, 24);
        const POINT origin{monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - 32) / 2,
                           monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - 24) / 2};
        PinId id{};
        ASSERT_TRUE(manager.Prepare(image, origin, id, error)) << error;
        manager.Show(id);
        const HWND pinWindow = manager.Window(id);
        ASSERT_TRUE(IsWindowVisible(pinWindow));
        RECT rectangle{};
        ASSERT_TRUE(GetWindowRect(pinWindow, &rectangle));
        const POINT point{rectangle.left + (rectangle.right - rectangle.left) / 2,
                          rectangle.top + (rectangle.bottom - rectangle.top) / 2};
        ASSERT_EQ(WindowFromPoint(point), pinWindow);
        pinWindows.push_back(pinWindow);
        hitPoints.push_back(point);
    }

    ASSERT_TRUE(manager.BeginCapture(error)) << error;
    WNDCLASSW overlayClass{};
    overlayClass.hInstance = GetModuleHandleW(nullptr);
    overlayClass.lpszClassName = L"OpenST.TestVisiblePinOverlay";
    overlayClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    // 使用普通窗口客户区命中规则，避免 STATIC 控件默认透明命中使鼠标检查穿透遮罩。
    // 入参：window 为测试遮罩；message、wParam、lParam 为原生窗口消息及附加数据。
    // 返回：命中测试返回 HTCLIENT，其他消息交给正式 Win32 默认窗口过程。
    overlayClass.lpfnWndProc = [](HWND window, UINT message, WPARAM wParam, LPARAM lParam) -> LRESULT
    {
        if (message == WM_NCHITTEST)
            return HTCLIENT;
        return DefWindowProcW(window, message, wParam, lParam);
    };
    ASSERT_TRUE(RegisterClassW(&overlayClass) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS);
    CaptureOverlaySession session;
    std::vector<HWND> overlays;
    for (const MONITORINFO& monitor : search.monitors)
    {
        CaptureOverlayOutput& output = session.Add(nullptr);
        output.window = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW, overlayClass.lpszClassName, L"", WS_POPUP, monitor.rcMonitor.left,
            monitor.rcMonitor.top, monitor.rcMonitor.right - monitor.rcMonitor.left,
            monitor.rcMonitor.bottom - monitor.rcMonitor.top, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ASSERT_NE(output.window, nullptr);
        overlays.push_back(output.window);
    }
    session.Show();
    for (std::size_t index = 0; index < overlays.size(); ++index)
    {
        SCOPED_TRACE(index);
        EXPECT_TRUE(IsWindowVisible(pinWindows[index]));
        EXPECT_TRUE(IsWindowVisible(overlays[index]));
        bool overlayAbovePin = false;
        for (HWND above = GetWindow(pinWindows[index], GW_HWNDPREV); above; above = GetWindow(above, GW_HWNDPREV))
            if (above == overlays[index])
            {
                overlayAbovePin = true;
                break;
            }
        EXPECT_TRUE(overlayAbovePin);
        EXPECT_EQ(WindowFromPoint(hitPoints[index]), overlays[index]);
    }
    session.Close();
    manager.EndCapture();
    for (const HWND pinWindow : pinWindows)
        EXPECT_TRUE(IsWindowVisible(pinWindow));
}

// 验证提示 owner 选择最高可见贴图，跳过隐藏窗口，并在贴图关闭后回退到消息窗口。
// 入参：无运行入参，宏参数用于测试注册。
// 返回：无返回值；只查询正式 owner 选择，不弹出真实提示或改变系统数据。
TEST_F(PinQueueIntegrationTest, dialog_owner_uses_visible_pin_and_falls_back_after_close)
{
    PinWindowManager& manager = AppPinTestAccess::Manager(*this->app_);
    const HWND fallback = AppPinTestAccess::MessageWindow(*this->app_);
    EXPECT_EQ(AppPinTestAccess::DialogOwner(*this->app_), fallback);
    const PinId lowerId = AppPinTestAccess::Prepare(*this->app_);
    const PinId upperId = AppPinTestAccess::Prepare(*this->app_);
    ASSERT_NE(lowerId, 0U);
    ASSERT_NE(upperId, 0U);
    EXPECT_EQ(AppPinTestAccess::DialogOwner(*this->app_), fallback);
    manager.Show(lowerId);
    EXPECT_EQ(AppPinTestAccess::DialogOwner(*this->app_), manager.Window(lowerId));
    manager.Show(upperId);
    EXPECT_EQ(AppPinTestAccess::DialogOwner(*this->app_), manager.Window(upperId));
    manager.Close(upperId);
    EXPECT_EQ(AppPinTestAccess::DialogOwner(*this->app_), manager.Window(lowerId));
    manager.Close(lowerId);
    EXPECT_EQ(AppPinTestAccess::DialogOwner(*this->app_), fallback);
}

// 验证输出服务不可用时消费 Pin 不创建图像，也不丢弃原冻结帧及稳定选区。
// 入参：无运行入参，宏参数用于测试注册。
// 返回：无返回值；此用例覆盖服务缺失准入，不代替图形设备失败提示的模态验收。
TEST_F(PinQueueIntegrationTest, unavailable_output_service_retains_frozen_selection)
{
    AppPinTestAccess::FrozenSelection(*this->app_);
    AppPinTestAccess::RemoveOutputRenderer(*this->app_);
    ASSERT_TRUE(AppPinTestAccess::PostSelection(*this->app_));
    ASSERT_TRUE(AppPinTestAccess::DispatchSelection(*this->app_));
    EXPECT_TRUE(AppPinTestAccess::SelectionRetained(*this->app_));
    EXPECT_EQ(AppPinTestAccess::Output(*this->app_), nullptr);
    EXPECT_FALSE(AppPinTestAccess::DispatchSelection(*this->app_));
}

// 验证旧截图的 Pin 消息不能对新选区创建贴图，随后新消息仍可完成且仅产生新图。
// 入参：无运行入参，宏参数用于测试注册。
// 返回：无返回值；以真实消息队列检查代次失效，不直接调用 PinSelection。
TEST_F(PinQueueIntegrationTest, stale_toolbar_pin_does_not_complete_replacement_capture)
{
    AppPinTestAccess::FrozenSelection(*this->app_);
    ASSERT_TRUE(AppPinTestAccess::PostSelection(*this->app_));
    AppPinTestAccess::FrozenSelection(*this->app_);
    ASSERT_TRUE(AppPinTestAccess::PostSelection(*this->app_));
    ASSERT_TRUE(AppPinTestAccess::DispatchSelection(*this->app_));
    EXPECT_TRUE(AppPinTestAccess::SelectionRetained(*this->app_));
    EXPECT_EQ(AppPinTestAccess::Output(*this->app_), nullptr);
    ASSERT_TRUE(AppPinTestAccess::DispatchSelection(*this->app_));
    EXPECT_TRUE(AppPinTestAccess::CaptureReleased(*this->app_));
    EXPECT_NE(AppPinTestAccess::Output(*this->app_), nullptr);
    EXPECT_FALSE(AppPinTestAccess::DispatchSelection(*this->app_));
}

// 验证稳定 ID 和命令白名单过滤无效请求，且不污染后续排队状态。
// 入参：无运行入参，宏参数用于测试注册。
// 返回：无返回值，通过断言报告结果。
TEST_F(PinQueueIntegrationTest, rejects_missing_target_and_unknown_command)
{
    EXPECT_FALSE(AppPinTestAccess::Post(*this->app_, 0, PinCommand::Copy));
    EXPECT_FALSE(AppPinTestAccess::Post(*this->app_, 999, PinCommand::Save));
    const PinId id = AppPinTestAccess::Prepare(*this->app_);
    ASSERT_NE(id, 0U);
    EXPECT_FALSE(AppPinTestAccess::Post(*this->app_, id, static_cast<PinCommand>(99)));
    EXPECT_FALSE(AppPinTestAccess::Pending(*this->app_));
}

// 验证命令排队后关闭目标，消费旧消息时不会访问失效窗口或误伤新图。
// 入参：无运行入参，宏参数用于测试注册。
// 返回：无返回值，通过断言报告结果。
TEST_F(PinQueueIntegrationTest, closing_queued_target_keeps_replacement_alive)
{
    const PinId oldId = AppPinTestAccess::Prepare(*this->app_);
    ASSERT_NE(oldId, 0U);
    ASSERT_TRUE(AppPinTestAccess::Post(*this->app_, oldId, PinCommand::Copy));
    EXPECT_FALSE(AppPinTestAccess::Post(*this->app_, oldId, PinCommand::Save));
    AppPinTestAccess::Close(*this->app_, oldId);
    const PinId newId = AppPinTestAccess::Prepare(*this->app_);
    ASSERT_NE(newId, 0U);
    ASSERT_NE(newId, oldId);
    EXPECT_TRUE(AppPinTestAccess::Dispatch(*this->app_));
    EXPECT_FALSE(AppPinTestAccess::Pending(*this->app_));
    EXPECT_TRUE(AppPinTestAccess::Exists(*this->app_, newId));
}

// 验证忙时拒绝新请求，排队后进入忙状态也会在消费时丢弃请求。
// 入参：无运行入参，宏参数用于测试注册。
// 返回：无返回值，通过断言报告结果。
TEST_F(PinQueueIntegrationTest, busy_state_rejects_and_discards_without_replay)
{
    const PinId id = AppPinTestAccess::Prepare(*this->app_);
    ASSERT_NE(id, 0U);
    AppPinTestAccess::SetBusy(*this->app_, true);
    EXPECT_FALSE(AppPinTestAccess::Post(*this->app_, id, PinCommand::Copy));
    AppPinTestAccess::SetBusy(*this->app_, false);
    ASSERT_TRUE(AppPinTestAccess::Post(*this->app_, id, PinCommand::Copy));
    AppPinTestAccess::SetBusy(*this->app_, true);
    EXPECT_TRUE(AppPinTestAccess::Dispatch(*this->app_));
    AppPinTestAccess::SetBusy(*this->app_, false);
    EXPECT_FALSE(AppPinTestAccess::Pending(*this->app_));
    EXPECT_FALSE(AppPinTestAccess::Dispatch(*this->app_));
    EXPECT_TRUE(AppPinTestAccess::Exists(*this->app_, id));
}

// 验证同一图像的新请求不会被相同命令的旧消息消费，请求代次独立于图像 ID。
// 入参：无运行入参，宏参数用于测试注册。
// 返回：无返回值，通过断言报告结果。
TEST_F(PinQueueIntegrationTest, old_request_cannot_consume_new_request_for_same_image)
{
    const PinId id = AppPinTestAccess::Prepare(*this->app_);
    ASSERT_NE(id, 0U);
    ASSERT_TRUE(AppPinTestAccess::Post(*this->app_, id, PinCommand::Copy));
    AppPinTestAccess::Invalidate(*this->app_);
    ASSERT_TRUE(AppPinTestAccess::Post(*this->app_, id, PinCommand::Copy));
    ASSERT_TRUE(AppPinTestAccess::Dispatch(*this->app_));
    EXPECT_TRUE(AppPinTestAccess::Pending(*this->app_));
    ASSERT_TRUE(AppPinTestAccess::Dispatch(*this->app_));
    EXPECT_FALSE(AppPinTestAccess::Pending(*this->app_));
    EXPECT_TRUE(AppPinTestAccess::Exists(*this->app_, id));
}

// 验证消息投递失败解除预订，之后有效请求仍能进入队列。
// 入参：无运行入参，宏参数用于测试注册。
// 返回：无返回值，通过断言报告结果。
TEST_F(PinQueueIntegrationTest, failed_post_rolls_back_reservation)
{
    const PinId id = AppPinTestAccess::Prepare(*this->app_);
    ASSERT_NE(id, 0U);
    EXPECT_FALSE(AppPinTestAccess::PostToInvalidWindow(*this->app_, id));
    EXPECT_FALSE(AppPinTestAccess::Pending(*this->app_));
    EXPECT_TRUE(AppPinTestAccess::Post(*this->app_, id, PinCommand::Copy));
    EXPECT_TRUE(AppPinTestAccess::Dispatch(*this->app_));
}
} // namespace open_st
