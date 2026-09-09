#include "capture_command_gate.h"
#include "capture_completion.h"
#include "capture_toolbar_monitor.h"
#include "save_image_dialog.h"
#include "simple_message_window.h"
#include <app.h>
#include <array>
#include <capture_toolbar.h>
#include <clipboard_writer.h>
#include <desktop_capturer.h>
#include <desktop_preview.h>
#include <frozen_desktop_frame.h>
#include <image_file_writer.h>
#include <log.h>
#include <overlay_renderer.h>
#include <selection_model.h>
#include <selection_output_renderer.h>
#include <settings.h>
#include <settings_window.h>
#include <single_instance.h>
#include <startup_registration.h>
#include <ui_text.h>
#include <vector>
#include <welcome_window.h>
#include <window_renderer.h>

#include "capture_overlay_session.h"
#include "open_st/version.h"
#include "resource.h"

#include <exception>
#include <limits>
#include <objbase.h>
#include <optional>
#include <shellapi.h>
#include <string>
#include <string_view>
#include <utility>

namespace
{
// 所有 Win32 类名和消息/命令 ID 都限制在本翻译单元，避免成为跨模块协议。
// 两层宏用于让 CMake 生成的窄字符串版本号在编译期转换成宽字符串。
#define OPEN_ST_WIDEN_IMPL(value) L##value
#define OPEN_ST_WIDEN(value) OPEN_ST_WIDEN_IMPL(value)
constexpr wchar_t MESSAGE_CLASS[] = L"OpenST.MessageWindow";
constexpr wchar_t OVERLAY_CLASS[] = L"OpenST.CaptureOverlay";
constexpr UINT LAUNCH_MESSAGE = WM_APP + 3;
constexpr UINT TOOLBAR_COMMAND_MESSAGE = WM_APP + 4;
constexpr UINT PREPARE_OUTPUT_MESSAGE = WM_APP + 2;
constexpr UINT TRAY_MESSAGE = WM_APP + 1;
constexpr UINT TRAY_ID = 1;
constexpr int CAPTURE_HOTKEY_ID = 1;

// 让包含本地化错误弹窗在内的同步完成流程在所有异常路径恢复 busy。
class CompletionBusyGuard final
{
  public:
    // 借用应用状态，开始阻止会话消息重入。
    explicit CompletionBusyGuard(bool& busy, std::function<void()> changed = {})
        : busy_(busy), changed_(std::move(changed))
    {
        this->busy_ = true;
        if (this->changed_)
        {
            this->changed_();
        }
    }
    // 提示或资源分配抛异常时也恢复可操作状态。
    ~CompletionBusyGuard()
    {
        this->Release();
    }
    // 禁止复制状态守卫。
    CompletionBusyGuard(const CompletionBusyGuard&) = delete;
    // 禁止复制赋值。
    CompletionBusyGuard& operator=(const CompletionBusyGuard&) = delete;
    // 正常收尾前先解除忙状态，使 CloseOverlay 可以释放资源。
    void Release() noexcept
    {
        if (!this->released_)
        {
            this->busy_ = false;
            this->released_ = true;
            if (this->changed_)
            {
                this->changed_();
            }
        }
    }

  private:
    bool& busy_;
    std::function<void()> changed_;
    bool released_{};
};

// 系统模态对话框期间暂停设置输入，保留原来的启用状态。
class WindowDisableGuard final
{
  public:
    // 只禁用调用前已启用的窗口。
    explicit WindowDisableGuard(HWND window) : window_(window), restore_(window != nullptr && IsWindowEnabled(window))
    {
        if (this->restore_)
        {
            EnableWindow(this->window_, FALSE);
        }
    }
    // 异常和取消同样恢复交互。
    ~WindowDisableGuard()
    {
        if (this->restore_ && IsWindow(this->window_))
        {
            EnableWindow(this->window_, TRUE);
        }
    }

  private:
    HWND window_;
    bool restore_;
};

// 日志文件使用 UTF-8；底层捕获模块使用宽字符串返回 Win32/DXGI 诊断信息。
std::string WideToUtf8(std::wstring_view value) noexcept
{
    if (value.empty())
    {
        return "<empty diagnostic>";
    }
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    {
        return "<diagnostic is too long>";
    }

    const int sourceLength = static_cast<int>(value.size());
    const int requiredBytes =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), sourceLength, nullptr, 0, nullptr, nullptr);
    if (requiredBytes <= 0)
    {
        return "<failed to convert diagnostic to UTF-8>";
    }

    std::string result(static_cast<std::size_t>(requiredBytes), '\0');
    const int convertedBytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), sourceLength,
                                                   result.data(), requiredBytes, nullptr, nullptr);
    return convertedBytes == requiredBytes ? result : "<failed to convert diagnostic to UTF-8>";
}

// 读取当前光标的虚拟桌面物理坐标；系统查询失败时不修改调用方流程并返回 false。
bool TryGetCursorPoint(open_st::PointI& point) noexcept
{
    POINT cursor{};
    if (GetCursorPos(&cursor) == FALSE)
    {
        return false;
    }
    point = {cursor.x, cursor.y};
    return true;
}

// 把八个缩放控制点方向映射为对应的 Win32 尺寸调整光标。
HCURSOR CursorForHandle(open_st::SelectionHandle handle) noexcept
{
    switch (handle)
    {
    case open_st::SelectionHandle::TopLeft:
    case open_st::SelectionHandle::BottomRight:
        return LoadCursorW(nullptr, IDC_SIZENWSE);
    case open_st::SelectionHandle::TopRight:
    case open_st::SelectionHandle::BottomLeft:
        return LoadCursorW(nullptr, IDC_SIZENESW);
    case open_st::SelectionHandle::Top:
    case open_st::SelectionHandle::Bottom:
        return LoadCursorW(nullptr, IDC_SIZENS);
    case open_st::SelectionHandle::Right:
    case open_st::SelectionHandle::Left:
        return LoadCursorW(nullptr, IDC_SIZEWE);
    case open_st::SelectionHandle::None:
        return LoadCursorW(nullptr, IDC_ARROW);
    }
    return LoadCursorW(nullptr, IDC_ARROW);
}

// 根据当前操作及悬停命中结果选择创建、移动、缩放或普通箭头光标。
HCURSOR CursorForSelection(const open_st::SelectionModel& selection, open_st::PointI point) noexcept
{
    switch (selection.Operation())
    {
    case open_st::SelectionOperation::Creating:
        return LoadCursorW(nullptr, IDC_CROSS);
    case open_st::SelectionOperation::Moving:
        return LoadCursorW(nullptr, IDC_SIZEALL);
    case open_st::SelectionOperation::Resizing:
        return CursorForHandle(selection.ActiveHandle());
    case open_st::SelectionOperation::None:
        break;
    }

    if (selection.Phase() == open_st::SelectionPhase::Unselected)
    {
        return LoadCursorW(nullptr, IDC_CROSS);
    }
    const open_st::SelectionHandle handle = selection.HitTestHandle(point);
    if (handle != open_st::SelectionHandle::None)
    {
        return CursorForHandle(handle);
    }
    return selection.Contains(point) ? LoadCursorW(nullptr, IDC_SIZEALL) : LoadCursorW(nullptr, IDC_ARROW);
}

// 读取最新屏幕坐标并立即把覆盖窗口光标更新为选区状态对应形状。
void UpdateOverlayCursor(const open_st::SelectionModel& selection) noexcept
{
    open_st::PointI point{};
    if (TryGetCursorPoint(point))
    {
        SetCursor(CursorForSelection(selection, point));
    }
}
} // namespace

namespace open_st
{
// 保存进程模块实例，窗口和其他系统资源留到 Run/StartCapture 中按需初始化。
App::App(HINSTANCE instance) noexcept : instance_(instance) {}

// 按截图会话、设置、本地化、托盘、消息窗口、互斥体和日志的依赖逆序清理。
App::~App()
{
    if (this->singleInstance_ != nullptr)
    {
        this->singleInstance_->Stop();
    }
    OPEN_ST_LOG_INFO("Application shutting down.");
    // 按“会话资源 → 系统集成 → 消息窗口 → 互斥体”的逆初始化顺序清理。
    if (this->settingsWindow_ != nullptr)
    {
        this->settingsWindow_->Close();
        this->settingsWindow_.reset();
    }
    ShutdownSettings();
    ShutdownUiText();
    this->CloseOverlay();
    this->RemoveTrayIcon();
    if (this->largeIcon_ != nullptr)
    {
        DestroyIcon(this->largeIcon_);
    }
    if (this->smallIcon_ != nullptr)
    {
        DestroyIcon(this->smallIcon_);
    }
    if (this->messageWindow_ != nullptr)
    {
        UnregisterHotKey(this->messageWindow_, CAPTURE_HOTKEY_ID);
        DestroyWindow(this->messageWindow_);
    }
    this->completion_.reset();
    this->outputRenderer_.reset();
    if (this->comInitialized_)
    {
        CoUninitialize();
    }
    ShutdownLogging();
}

// 初始化进程级服务、托盘与全局热键，并阻塞运行 Win32 消息循环直至退出。
int App::Run(int)
{
    // 先确定实例身份，第二实例只读取语言，不创建设置或日志。
    this->singleInstance_ = std::make_unique<SingleInstance>();
    const InstanceStatus instanceStatus = this->singleInstance_->Acquire();
    int argumentCount = 0;
    LPWSTR* argumentValues = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    std::vector<std::wstring> arguments;
    if (argumentValues != nullptr)
    {
        for (int index = 1; index < argumentCount; ++index)
        {
            arguments.emplace_back(argumentValues[index]);
        }
        LocalFree(argumentValues);
    }
    LaunchCommand command{};
    const bool argumentsValid = argumentValues != nullptr && ParseLaunchCommand(arguments, command);
    if (argumentsValid && command == LaunchCommand::Startup && instanceStatus.result == InstanceResult::Forwarded)
    {
        return 0;
    }
    if (!InitializeUiText() || !IsUiTextAvailable())
    {
        OPEN_ST_LOG_FATAL("Failed to initialize UI text resources.");
        (void)MessageBoxW(
            nullptr,
            L"Language resources could not be loaded.\n\n无法加载语言资源。\n\n言語リソースを読み込めませんでした。",
            L"Open-ST", MB_OK | MB_ICONERROR);
        return 1;
    }
    const std::optional<std::string> startupLanguage = ReadStartupLanguage();
    if (startupLanguage.has_value())
    {
        (void)SetUiLanguage(*startupLanguage);
    }
    if (!argumentsValid || instanceStatus.result == InstanceResult::Failed)
    {
        const std::wstring message = GetUiText(!argumentsValid ? "launch.invalid_arguments" : "launch.failed");
        (void)MessageBoxW(nullptr, message.c_str(), GetUiText("app.title").c_str(), MB_OK | MB_ICONERROR);
        return 1;
    }
    if (instanceStatus.result != InstanceResult::Primary)
    {
        const InstanceStatus forwarded = this->singleInstance_->Forward(command);
        if (forwarded.result == InstanceResult::Forwarded)
        {
            return 0;
        }
        const std::wstring message = GetUiText(forwarded.result == InstanceResult::OtherSession ? "launch.other_session"
                                               : forwarded.error == ERROR_BUSY                  ? "launch.busy"
                                                                               : "launch.forward_failed");
        (void)MessageBoxW(nullptr, message.c_str(), GetUiText("app.title").c_str(), MB_OK | MB_ICONWARNING);
        return 1;
    }
    (void)InitializeLogging();
    OPEN_ST_LOG_INFO("Application starting.");

    // 只有确认当前实例唯一后才读取或创建用户设置，避免第二实例参与配置写入。
    const bool settingsInitialized = InitializeSettings();
    if (settingsInitialized)
    {
        const std::optional<std::string> configuredLanguage = GetStringSetting("ui.language");
        if (configuredLanguage.has_value())
        {
            (void)SetUiLanguage(*configuredLanguage);
        }
    }
    // 托盘和 RegisterHotKey 都需要一个稳定的 HWND 接收回调，但程序不需要主界面，
    // 因此使用不可见的 message-only window 作为整个进程的消息中枢。
    if (!this->CreateMessageWindow())
    {
        OPEN_ST_LOG_FATAL("Failed to create the application message window.");
        return 1;
    }

    if (!settingsInitialized)
    {
        const std::wstring message = GetUiText("settings.load_failed");
        const std::wstring title = GetUiText("app.title");
        (void)MessageBoxW(this->DialogOwner(), message.c_str(), title.c_str(), MB_OK | MB_ICONWARNING);
    }
    else if (!IsSettingsPersistenceAvailable())
    {
        const std::wstring message = GetUiText("settings.persistence_unavailable");
        const std::wstring title = GetUiText("app.title");
        (void)MessageBoxW(this->DialogOwner(), message.c_str(), title.c_str(), MB_OK | MB_ICONWARNING);
    }
    if (!settingsInitialized || !IsSettingsPersistenceAvailable())
    {
        // 启动失败已由上面的专用提示告知，避免随后为同一次设置故障重复弹窗。
        (void)ConsumeSettingsReadWarning();
    }
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(comResult))
    {
        const std::wstring error = GetUiText("export.com_failed");
        const std::wstring title = GetUiText("app.title");
        (void)MessageBoxW(this->DialogOwner(), error.c_str(), title.c_str(), MB_OK | MB_ICONERROR);
        return 1;
    }
    this->comInitialized_ = true;
    this->outputRenderer_ = std::make_unique<SelectionOutputRenderer>();
    this->completion_ = std::make_unique<CaptureCompletion>();
    this->AddTrayIcon();
    std::array<wchar_t, 32768> executable{};
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    this->singleInstance_->SetCaptureGate(
        [this]() -> std::optional<LPARAM>
        {
            const std::uint64_t gate = this->captureGate_.load(std::memory_order_acquire);
            return (gate & 1) != 0 ? std::nullopt : std::optional<LPARAM>(static_cast<LPARAM>(gate));
        });
    if (length == 0 || length >= executable.size() ||
        !this->singleInstance_->StartListening(this->messageWindow_, LAUNCH_MESSAGE))
    {
        (void)MessageBoxW(this->DialogOwner(), GetUiText("launch.failed").c_str(), GetUiText("app.title").c_str(),
                          MB_OK | MB_ICONERROR);
        return 1;
    }
    this->startup_ = std::make_unique<StartupRegistration>(std::wstring(executable.data(), length));
    if (!GetBoolSetting("onboarding.completed").value_or(false))
    {
        CompletionBusyGuard welcomeGuard(this->welcoming_, [this]() { this->UpdateCaptureGate(); });
        this->welcomeWindow_ = std::make_unique<WelcomeWindow>();
        const bool accepted = this->welcomeWindow_->ShowModal(this->instance_, this->MakeSettingsCallbacks());
        const bool failed = this->welcomeWindow_->Failed();
        this->welcomeWindow_.reset();
        if (!accepted)
        {
            if (failed)
            {
                (void)MessageBoxW(this->DialogOwner(), GetUiText("welcome.failed").c_str(),
                                  GetUiText("welcome.title").c_str(), MB_OK | MB_ICONERROR);
            }
            return failed ? 1 : 0;
        }
    }
    else
    {
        const StartupStatus status = this->startup_->Query();
        const bool wanted = GetBoolSetting("startup.enabled").value_or(false);
        if ((wanted && status.state != StartupState::CurrentPath) || (!wanted && status.state != StartupState::Missing))
        {
            this->ShowSimpleMessage([this]()
                                    { return GetUiText("startup.mismatch") + L"\n\n" + this->StartupStatusText(); },
                                    []() { return GetUiText("app.title"); }, MB_OK | MB_ICONWARNING);
        }
    }
    (void)PostMessageW(this->messageWindow_, PREPARE_OUTPUT_MESSAGE, 0, 0);
    if (!RegisterHotKey(this->messageWindow_, CAPTURE_HOTKEY_ID, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'Q'))
    {
        const DWORD error = GetLastError();
        OPEN_ST_LOG_WARNING("Failed to register the capture hotkey. win32_error=", error);
        const std::wstring message = GetUiText("hotkey.registration_failed");
        const std::wstring title = GetUiText("app.title");
        (void)MessageBoxW(this->DialogOwner(), message.c_str(), title.c_str(), MB_OK | MB_ICONWARNING);
    }

    this->ReportDataReadWarnings();
    this->UpdateCaptureGate();
    if (command == LaunchCommand::Capture)
    {
        (void)PostMessageW(this->messageWindow_, LAUNCH_MESSAGE, static_cast<WPARAM>(LaunchCommand::Capture),
                           static_cast<LPARAM>(this->captureGate_.load(std::memory_order_acquire)));
    }
    // 程序空闲时 GetMessageW 会阻塞，不轮询桌面，所以托盘常驻阶段几乎不消耗 CPU。
    MSG message{};
    BOOL messageResult = 0;
    while ((messageResult = GetMessageW(&message, nullptr, 0, 0)) > 0)
    {
        if (this->settingsWindow_ != nullptr && this->settingsWindow_->ProcessDialogMessage(message))
        {
            this->ReportDataReadWarnings();
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
        this->ReportDataReadWarnings();
    }
    if (messageResult == -1)
    {
        const DWORD error = GetLastError();
        OPEN_ST_LOG_ERROR("The application message loop failed. win32_error=", error);
        return 1;
    }
    return static_cast<int>(message.wParam);
}

// 消费模块去重告警后再取得提示文本；截图期间推迟提示，避免抢夺捕获和选区交互。
void App::ReportDataReadWarnings()
{
    if (this->overlaySession_ != nullptr || this->overlayPreparing_ || this->dialogActive_)
    {
        return;
    }
    const bool settingsFailed = ConsumeSettingsReadWarning();
    const bool textsFailed = ConsumeUiTextReadWarning();
    if (!settingsFailed && !textsFailed)
    {
        return;
    }
    const std::wstring message = GetUiText("common_resources.read_failed");
    const std::wstring title = GetUiText("app.title");
    // 读取告警文本本身若遇到新故障，本次合并提示已经覆盖，不留到下一条消息重复报告。
    (void)ConsumeUiTextReadWarning();
    (void)MessageBoxW(this->DialogOwner(), message.c_str(), title.c_str(), MB_OK | MB_ICONWARNING);
}

// 注册并创建不可见的 message-only window，供热键、托盘和退出事件统一投递。
bool App::CreateMessageWindow()
{
    // 描述窗口类的结构体，必须在创建窗口通过其注册窗口类。
    // cbSize = sizeof(windowClass)
    WNDCLASSEXW windowClass{sizeof(windowClass)};
    // 指定窗口过程；由该窗口类创建的窗口会通过此回调接收窗口过程消息。
    windowClass.lpfnWndProc = App::WindowProc;
    windowClass.hInstance = this->instance_;
    windowClass.lpszClassName = MESSAGE_CLASS;
    // 实际注册窗口类时，如果类名已存在，RegisterClassExW 会返回 0 并设置 GetLastError() 为 ERROR_CLASS_ALREADY_EXISTS。
    if (RegisterClassExW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        const DWORD error = GetLastError();
        OPEN_ST_LOG_ERROR("Failed to register the message window class. win32_error=", error);
        return false;
    }

    // HWND_MESSAGE 创建的窗口不可见、没有任务栏按钮，也不会参与普通顶层窗口枚举。
    // 最后的 this 会通过 WM_NCCREATE 传给 WindowProc，建立 HWND 到 App 的绑定。
    const std::wstring title = GetUiText("window.message.title");
    this->messageWindow_ =
        CreateWindowExW(0, MESSAGE_CLASS, title.c_str(), 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, this->instance_, this);
    if (this->messageWindow_ == nullptr)
    {
        const DWORD error = GetLastError();
        OPEN_ST_LOG_ERROR("Failed to create the message window. win32_error=", error);
    }
    return this->messageWindow_ != nullptr;
}

// 建立隐藏 HWND 到 App 的绑定，并把后续消息转交给实例 HandleMessage。
LRESULT CALLBACK App::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    // 从当前 HWND 的用户数据槽取回此前绑定的 App 指针；首次进入 WM_NCCREATE 时尚未绑定，因此为空。
    App* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    // WM_NCCREATE 在窗口创建早期、WM_CREATE 之前发送；此时 lParam 指向 CREATESTRUCTW，
    // 其中 lpCreateParams 保存了 CreateWindowExW 的最后一个参数。
    if (message == WM_NCCREATE)
    {
        // 此时 lpCreateParams 正是 CreateWindowExW 的最后一个参数；保存后，后续静态回调
        // 都能通过 GWLP_USERDATA 找回对应的 App 实例。
        const CREATESTRUCTW* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        app = static_cast<App*>(create->lpCreateParams);
        // 将app指针存储在窗口的用户数据中，以便在后续的GetWindowLongPtrW()调用中检索。
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    return app != nullptr ? app->HandleMessage(window, message, wParam, lParam)
                          : DefWindowProcW(window, message, wParam, lParam);
}

// 真正的窗口消息处理逻辑在实例方法中实现，静态窗口过程负责绑定或找回 App 实例，并将消息转发给它。
LRESULT App::HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == TOOLBAR_COMMAND_MESSAGE)
    {
        this->DispatchToolbarCommand(static_cast<CaptureToolbarCommand>(wParam), static_cast<std::uint64_t>(lParam));
        return 0;
    }
    if (message == LAUNCH_MESSAGE)
    {
        if (this->welcoming_)
        {
            if (this->welcomeWindow_ != nullptr)
            {
                this->welcomeWindow_->Activate();
            }
            return 0;
        }
        if (this->completionBusy_ || this->dialogActive_ || this->shuttingDown_)
        {
            return 0;
        }
        if (wParam == static_cast<WPARAM>(LaunchCommand::Normal))
        {
            this->ShowSettings();
        }
        else if (wParam == static_cast<WPARAM>(LaunchCommand::Capture))
        {
            const std::uint64_t gate = this->captureGate_.load(std::memory_order_acquire);
            if ((gate & 1) == 0 && static_cast<std::uint64_t>(lParam) == gate)
            {
                this->StartCapture();
            }
        }
        return 0;
    }
    if ((this->completionBusy_ || this->dialogActive_ || this->welcoming_ || this->shuttingDown_ ||
         this->settingsBusy_) &&
        (message == WM_HOTKEY || message == WM_COMMAND || message == TRAY_MESSAGE))
    {
        return 0;
    }
    switch (message)
    {
    case PREPARE_OUTPUT_MESSAGE:
    {
        std::wstring error;
        if (!this->outputRenderer_->Prepare(error))
        {
            OPEN_ST_LOG_WARNING("HDR output warm-up failed. detail=", WideToUtf8(error));
        }
        return 0;
    }
    // RegisterHotKey 注册的全局快捷键被触发；wParam 是快捷键 ID，lParam 包含修饰键和虚拟键码。
    case WM_HOTKEY:
        this->StartCapture();
        return 0;
    // 菜单项产生的命令消息；LOWORD(wParam) 是资源文件中定义的菜单命令 ID。
    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        // 用户选择托盘菜单中的“截图”。
        case ID_TRAY_CAPTURE:
            this->StartCapture();
            return 0;
        // 用户选择托盘菜单中的“设置”。
        case ID_TRAY_SETTINGS:
            this->ShowSettings();
            return 0;
        // 用户选择托盘菜单中的“关于”。
        case ID_TRAY_ABOUT:
            this->ShowAbout();
            return 0;
        case ID_TRAY_CLEANUP:
            this->ShowCleanup();
            return 0;
        // 用户选择托盘菜单中的“退出”，向当前线程投递 WM_QUIT 以结束消息循环。
        case ID_TRAY_EXIT:
            PostQuitMessage(0);
            return 0;
        default:
            break;
        }
        break;
    // Shell_NotifyIconW 约定的托盘回调消息；LOWORD(lParam) 表示托盘图标上的鼠标事件。
    case TRAY_MESSAGE:
        if (LOWORD(lParam) == WM_LBUTTONUP)
        {
            this->StartCapture();
        }
        else if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU)
        {
            this->ShowTrayMenu();
        }
        return 0;
    // 当前窗口正在销毁；隐藏消息窗口销毁时投递 WM_QUIT，结束应用消息循环。
    case WM_DESTROY:
        if (window == this->messageWindow_)
        {
            PostQuitMessage(0);
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

// 把应用图标加入系统通知区域；失败只记日志，不阻断常驻消息循环。
void App::AddTrayIcon()
{
    NOTIFYICONDATAW data{sizeof(data)};
    data.hWnd = this->messageWindow_;
    data.uID = TRAY_ID;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = TRAY_MESSAGE;
    // 非共享加载避免同名资源缓存忽略尺寸；重复添加托盘时复用自有句柄。
    if (this->largeIcon_ == nullptr)
    {
        this->largeIcon_ = static_cast<HICON>(LoadImageW(this->instance_, L"OPEN_ST_ICON", IMAGE_ICON,
                                                         GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0));
    }
    if (this->smallIcon_ == nullptr)
    {
        this->smallIcon_ =
            static_cast<HICON>(LoadImageW(this->instance_, L"OPEN_ST_ICON", IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                          GetSystemMetrics(SM_CYSMICON), 0));
    }
    if (this->largeIcon_ == nullptr || this->smallIcon_ == nullptr)
    {
        OPEN_ST_LOG_WARNING("Failed to load application icon resources. win32_error=", GetLastError());
    }
    // 加载失败时借用系统图标维持托盘入口，不把共享句柄放入自有成员。
    data.hIcon = this->smallIcon_ != nullptr ? this->smallIcon_ : LoadIconW(nullptr, IDI_APPLICATION);
    const std::wstring tooltip = GetUiText("tray.tooltip");
    (void)wcsncpy_s(data.szTip, tooltip.c_str(), _TRUNCATE);
    this->trayAdded_ = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
    if (!this->trayAdded_)
    {
        const DWORD error = GetLastError();
        OPEN_ST_LOG_WARNING("Failed to add the notification-area icon. win32_error=", error);
    }
}

// 在托盘图标曾成功添加时发送删除请求，并收敛本地标志。
void App::RemoveTrayIcon() noexcept
{
    if (!this->trayAdded_)
    {
        return;
    }
    NOTIFYICONDATAW data{sizeof(data)};
    data.hWnd = this->messageWindow_;
    data.uID = TRAY_ID;
    Shell_NotifyIconW(NIM_DELETE, &data);
    this->trayAdded_ = false;
}

// 设置窗口确认新的界面语言后，刷新所有窗口标题和托盘提示文本。
void App::RefreshLocalizedUi()
{
    if (this->captureToolbar_ != nullptr)
    {
        const ToolbarResult result = this->captureToolbar_->RefreshTexts();
        if (!result.success)
        {
            OPEN_ST_LOG_WARNING("Failed to refresh capture toolbar texts.");
        }
    }
    if (this->settingsWindow_ != nullptr)
    {
        this->settingsWindow_->RefreshTexts();
    }
    if (this->welcomeWindow_ != nullptr)
    {
        this->welcomeWindow_->RefreshTexts();
    }
    if (this->messageRenderer_ != nullptr)
    {
        (void)this->messageRenderer_->RefreshTexts();
        if (this->messageStatusRefresh_)
        {
            this->messageStatusRefresh_();
        }
    }
    if (this->messageWindow_ != nullptr)
    {
        const std::wstring messageWindowTitle = GetUiText("window.message.title");
        (void)SetWindowTextW(this->messageWindow_, messageWindowTitle.c_str());
    }
    if (this->overlaySession_ != nullptr)
    {
        const std::wstring overlayTitle = GetUiText("window.capture.title");
        this->overlaySession_->SetTitle(overlayTitle);
    }
    if (this->trayAdded_)
    {
        NOTIFYICONDATAW data{sizeof(data)};
        data.hWnd = this->messageWindow_;
        data.uID = TRAY_ID;
        data.uFlags = NIF_TIP;
        const std::wstring tooltip = GetUiText("tray.tooltip");
        (void)wcsncpy_s(data.szTip, tooltip.c_str(), _TRUNCATE);
        if (Shell_NotifyIconW(NIM_MODIFY, &data) == FALSE)
        {
            OPEN_ST_LOG_WARNING("Failed to refresh the localized notification-area tooltip. win32_error=",
                                GetLastError());
        }
    }
}

// 在当前鼠标位置构造并显示本地化托盘菜单，选择结果通过 WM_COMMAND 返回。
void App::ShowTrayMenu()
{
    POINT cursor{};
    GetCursorPos(&cursor);
    HMENU menu = CreatePopupMenu();
    const std::wstring captureText = GetUiText("tray.capture");
    const std::wstring settingsText = GetUiText("tray.settings");
    const std::wstring aboutText = GetUiText("tray.about");
    const std::wstring exitText = GetUiText("tray.exit");
    const std::wstring cleanupText = GetUiText("tray.cleanup");
    (void)AppendMenuW(menu, MF_STRING, ID_TRAY_CAPTURE, captureText.c_str());
    (void)AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    (void)AppendMenuW(menu, MF_STRING, ID_TRAY_SETTINGS, settingsText.c_str());
    (void)AppendMenuW(menu, MF_STRING, ID_TRAY_ABOUT, aboutText.c_str());
    (void)AppendMenuW(menu, MF_STRING, ID_TRAY_CLEANUP, cleanupText.c_str());
    (void)AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, exitText.c_str());
    // Win32 托盘菜单需要先把所属窗口设为前台，否则用户点击菜单外部时菜单可能无法自动收起。
    SetForegroundWindow(this->messageWindow_);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, this->messageWindow_, nullptr);
    DestroyMenu(menu);
}

// 懒创建或激活设置窗口，并在语言保存成功后刷新应用现有界面。
void App::ShowSettings()
{
    if (this->dialogActive_ || this->shuttingDown_)
    {
        return;
    }
    if (this->settingsWindow_ == nullptr)
    {
        this->settingsWindow_ = std::make_unique<SettingsWindow>();
    }
    if (!this->settingsWindow_->Show(this->instance_, this->MakeSettingsCallbacks()))
    {
        const std::wstring message = GetUiText("settings.open_failed");
        const std::wstring title = GetUiText("app.title");
        (void)MessageBoxW(this->DialogOwner(), message.c_str(), title.c_str(), MB_OK | MB_ICONWARNING);
    }
}

// 将业务能力注入 Settings，避免设置窗口直接依赖系统集成和本地化模块。
SettingsWindowCallbacks App::MakeSettingsCallbacks()
{
    SettingsWindowCallbacks callbacks;
    // 只向设置窗口传入所请求键的文本，不暴露本地化模块类型或状态。
    callbacks.text = [](std::string_view key) { return GetUiText(key); };
    // 当前语言仅用于设置缺失时选中运行时语言。
    callbacks.currentLanguage = []() { return CurrentUiLanguageCode(); };
    // 每次查询读取本地化模块动态提供的语言集合。
    callbacks.availableLanguages = []()
    { return IsUiTextAvailable() ? GetAvailableUiLanguages() : std::vector<std::string>{}; };
    // 设置持久化成功后才切换运行语言，并刷新现有应用界面。
    callbacks.languageApplied = [this](std::string_view language)
    {
        const bool applied = SetUiLanguage(language);
        this->RefreshLocalizedUi();
        return applied;
    };
    callbacks.largeIcon = this->largeIcon_;
    callbacks.smallIcon = this->smallIcon_;
    callbacks.startupApplied = [this](bool enabled) { return this->ApplyStartup(enabled); };
    callbacks.startupStatus = [this]() { return this->StartupStatusText(); };
    callbacks.busyChanged = [this](bool busy)
    {
        this->settingsBusy_ = busy;
        this->UpdateCaptureGate();
    };
    return callbacks;
}

// 仅报告已登记入口状态，Windows 的启动许可仍由系统管理。
std::wstring App::StartupStatusText() const
{
    const StartupStatus status = this->startup_ != nullptr ? this->startup_->Query() : StartupStatus{};
    const char* key = status.state == StartupState::CurrentPath ? "startup.current"
                      : status.state == StartupState::Missing   ? "startup.missing"
                      : status.state == StartupState::OtherPath ? "startup.other_path"
                      : status.state == StartupState::Conflict  ? "startup.conflict"
                                                                : "startup.failed";
    return GetUiText(key);
}

// 仅由欢迎确认、设置应用或明确修复触发系统写入。
bool App::ApplyStartup(bool enabled)
{
    if (this->startup_ == nullptr)
    {
        return false;
    }
    const StartupStatus result = this->startup_->Apply(enabled, true);
    const bool applied = result.state == (enabled ? StartupState::CurrentPath : StartupState::Missing);
    if (!applied)
    {
        OPEN_ST_LOG_ERROR("Startup registration failed. state=", static_cast<int>(result.state),
                          " error=", result.error);
    }
    return applied;
}

// 原生模态提示使用可见 owner，Windows 会在提示期间禁用该窗口。
HWND App::DialogOwner() const noexcept
{
    if (this->settingsWindow_ != nullptr && this->settingsWindow_->IsOpen())
    {
        return this->settingsWindow_->NativeHandle();
    }
    return this->messageWindow_;
}

// 所有布尔忙状态仅在 UI 线程改变；一次原子发布同时传递暂停状态和新代次。
void App::UpdateCaptureGate() noexcept
{
    const bool paused =
        this->completionBusy_ || this->dialogActive_ || this->welcoming_ || this->shuttingDown_ || this->settingsBusy_;
    const std::uint64_t next = (this->captureGate_.load(std::memory_order_relaxed) & ~std::uint64_t{1}) + 2;
    this->captureGate_.store(next | static_cast<std::uint64_t>(paused), std::memory_order_release);
    this->InvalidateToolbarCommands();
}

// 判断当前输入能否进入统一完成命令队列。
bool App::CanSubmitToolbarCommand() const noexcept
{
    return !this->completionBusy_ && !this->dialogActive_ && !this->welcoming_ && !this->shuttingDown_ &&
           !this->settingsBusy_ && !this->overlayPreparing_ && !this->overlayInvalidated_ &&
           this->overlaySession_ != nullptr && this->selectionModel_ != nullptr &&
           this->selectionModel_->Phase() == SelectionPhase::Selected && this->selectionModel_->HasSelection();
}

// 投递固定数值而非裸指针；预订后按钮和快捷键均不能再次进入。
bool App::PostToolbarCommand(CaptureToolbarCommand command, std::uint64_t token) noexcept
{
    if (command != CaptureToolbarCommand::Cancel && command != CaptureToolbarCommand::Save &&
        command != CaptureToolbarCommand::Copy)
    {
        return false;
    }
    if (this->toolbarGate_ == nullptr ||
        !this->toolbarGate_->Reserve(token, static_cast<std::uint32_t>(command), this->CanSubmitToolbarCommand()))
    {
        return false;
    }
    if (!PostMessageW(this->messageWindow_, TOOLBAR_COMMAND_MESSAGE, static_cast<WPARAM>(command),
                      static_cast<LPARAM>(token)))
    {
        const DWORD error = GetLastError();
        this->InvalidateToolbarCommands();
        OPEN_ST_LOG_WARNING("Failed to post capture toolbar command. win32_error=", error);
        return false;
    }
    if (this->captureToolbar_ != nullptr)
    {
        this->captureToolbar_->SetBusy(true);
    }
    return true;
}

// 在按钮窗口过程返回后执行业务，避免回调栈中同步销毁工具栏。
void App::DispatchToolbarCommand(CaptureToolbarCommand command, std::uint64_t token)
{
    if (this->toolbarGate_ == nullptr ||
        !this->toolbarGate_->Consume(token, static_cast<std::uint32_t>(command), this->CanSubmitToolbarCommand()))
    {
        this->RefreshCaptureToolbar();
        return;
    }
    switch (command)
    {
    case CaptureToolbarCommand::Cancel:
        this->CloseOverlay();
        break;
    case CaptureToolbarCommand::Save:
        this->SaveSelection();
        break;
    case CaptureToolbarCommand::Copy:
        this->CopySelection();
        break;
    }
    this->RefreshCaptureToolbar();
}

// 所有取消和模态切换都同步撤销旧请求，工具栏的新 token 随后统一刷新。
void App::InvalidateToolbarCommands(bool updateMonitor) noexcept
{
    if (this->toolbarGate_ != nullptr)
    {
        this->toolbarGate_->Invalidate();
        this->toolbarGate_->SetInputBarrier(GetTickCount());
    }
    this->RefreshCaptureToolbar(updateMonitor);
}

// 按当前稳定选区展示一条工具栏，目标屏幕只在选区操作结束时改变。
void App::RefreshCaptureToolbar(bool updateMonitor) noexcept
try
{
    if (this->captureToolbar_ == nullptr || this->toolbarGate_ == nullptr)
    {
        return;
    }
    if (!this->CanSubmitToolbarCommand())
    {
        this->captureToolbar_->Hide();
        return;
    }
    const RectI rectangle = this->selectionModel_->Snapshot().rectangle;
    const RECT selection{rectangle.left, rectangle.top, rectangle.right, rectangle.bottom};
    if (updateMonitor || this->toolbarMonitor_ == nullptr)
    {
        std::vector<HMONITOR> monitors;
        const BOOL enumerated = EnumDisplayMonitors(
            nullptr, nullptr,
            [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL
            {
                try
                {
                    reinterpret_cast<std::vector<HMONITOR>*>(data)->push_back(monitor);
                    return TRUE;
                }
                catch (...)
                {
                    return FALSE;
                }
            },
            reinterpret_cast<LPARAM>(&monitors));
        if (!enumerated)
        {
            this->captureToolbar_->Hide();
            return;
        }
        std::vector<RECT> bounds;
        for (HMONITOR monitor : monitors)
        {
            MONITORINFO info{sizeof(info)};
            if (!GetMonitorInfoW(monitor, &info))
            {
                this->captureToolbar_->Hide();
                return;
            }
            bounds.push_back(info.rcMonitor);
        }
        POINT endpoint{selection.right - 1, selection.bottom - 1};
        (void)GetCursorPos(&endpoint);
        const std::size_t chosen = SelectToolbarMonitor(selection, endpoint, bounds);
        if (chosen == monitors.size())
        {
            this->captureToolbar_->Hide();
            return;
        }
        this->toolbarMonitor_ = monitors[chosen];
    }
    MONITORINFO monitorInfo{sizeof(monitorInfo)};
    if (!GetMonitorInfoW(this->toolbarMonitor_, &monitorInfo))
    {
        this->captureToolbar_->Hide();
        return;
    }
    const HWND output = this->overlaySession_->WindowForMonitor(this->toolbarMonitor_);
    const UINT dpi = output != nullptr ? GetDpiForWindow(output) : 96;
    const ToolbarResult placed =
        this->captureToolbar_->UpdatePlacement(selection, monitorInfo.rcWork, dpi == 0 ? 96 : dpi);
    if (!placed.success)
    {
        this->captureToolbar_->Hide();
        OPEN_ST_LOG_WARNING("Failed to position capture toolbar.");
        return;
    }
    this->captureToolbar_->SetBusy(this->toolbarGate_->Pending());
    const ToolbarResult shown = this->captureToolbar_->Show(this->toolbarGate_->Token());
    if (!shown.success)
    {
        OPEN_ST_LOG_WARNING("Failed to show capture toolbar.");
    }
}
catch (...)
{
    if (this->captureToolbar_ != nullptr)
    {
        this->captureToolbar_->Hide();
    }
    OPEN_ST_LOG_WARNING("Failed to update capture toolbar.");
}

// 仅在冻结帧及覆盖窗口准备完毕后创建工具栏；失败不影响原有截图快捷键。
void App::CreateCaptureToolbar() noexcept
{
    try
    {
        this->captureToolbar_ = std::make_unique<CaptureToolbar>();
        std::vector<ToolbarButtonSpec> buttons{
            {CaptureToolbarCommand::Cancel, ToolbarIcon::Cancel, "capture.toolbar.cancel", 0},
            {CaptureToolbarCommand::Save, ToolbarIcon::Save, "capture.toolbar.save", 1},
            {CaptureToolbarCommand::Copy, ToolbarIcon::Copy, "capture.toolbar.copy", 1}};
        const ToolbarResult result = this->captureToolbar_->Create(
            this->instance_, this->overlaySession_->ActivationWindow(), std::move(buttons), [](std::string_view key)
            { return GetUiText(key); }, [this](CaptureToolbarCommand command, std::uint64_t token)
            { return this->PostToolbarCommand(command, token); });
        if (result.success)
        {
            return;
        }
        OPEN_ST_LOG_WARNING("Failed to create capture toolbar. detail=", WideToUtf8(result.error));
    }
    catch (...)
    {
        OPEN_ST_LOG_WARNING("Failed to allocate capture toolbar.");
    }
    this->captureToolbar_.reset();
    // 同步提示期间冻结会话输入，返回后先检查显示布局是否仍有效。
    try
    {
        CompletionBusyGuard guard(this->completionBusy_, [this]() { this->UpdateCaptureGate(); });
        (void)MessageBoxW(this->overlaySession_->ActivationWindow(), GetUiText("capture.toolbar.failed").c_str(),
                          GetUiText("app.title").c_str(), MB_OK | MB_ICONWARNING);
        guard.Release();
        if (this->overlayInvalidated_)
        {
            this->CloseOverlay();
        }
        else if (this->overlaySession_ != nullptr)
        {
            this->overlaySession_->RestoreFocus();
        }
    }
    catch (...)
    {
        OPEN_ST_LOG_WARNING("Failed to present capture toolbar warning.");
    }
}

// 在显示遮罩前冻结虚拟桌面，并建立覆盖窗口、渲染器和选区模型的一次会话。
void App::StartCapture()
try
{
    if (this->completionBusy_ || this->dialogActive_ || this->welcoming_ || this->shuttingDown_ || this->settingsBusy_)
    {
        return;
    }
    OPEN_ST_LOG_DEBUG("Capture requested.");
    if (this->overlaySession_ != nullptr)
    {
        // 同一时刻只允许一个截图会话；重复快捷键只把现有覆盖窗口带回前台。
        SetForegroundWindow(this->overlaySession_->ActivationWindow());
        this->RefreshCaptureToolbar();
        return;
    }

    // 产品约束要求先冻结全部原生显示输出，再生成预览并显示遮罩，防止把界面截入结果。
    std::unique_ptr<FrozenDesktopFrame> capturedFrame = std::make_unique<FrozenDesktopFrame>();
    // 捕获当前虚拟桌面的原生 SDR/HDR plane；底层诊断不直接显示给用户。
    DesktopCapturer capturer;
    std::wstring captureError;
    if (!capturer.Capture(*capturedFrame, captureError))
    {
        OPEN_ST_LOG_ERROR("Desktop capture failed. detail=", WideToUtf8(captureError));
        this->ShowCaptureError("capture.error.unknown");
        return;
    }

    // 一个注册窗口类的描述结构体，必须在创建窗口通过其注册窗口类。
    WNDCLASSEXW overlayClass{sizeof(overlayClass)};
    overlayClass.lpfnWndProc = App::OverlayProc;                                        // 设置窗口过程回调函数
    overlayClass.hInstance = this->instance_;                                           // 设置窗口类所属的实例句柄
    overlayClass.hCursor = LoadCursorW(nullptr, IDC_CROSS);                             // 设置鼠标光标为十字准星
    overlayClass.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)); // 设置背景画刷为黑色
    overlayClass.lpszClassName = OVERLAY_CLASS; // 设置窗口类名为 OVERLAY_CLASS L"OpenST.CaptureOverlay"
    // 注册窗口类；已经由上一截图会话注册不属于错误。
    if (RegisterClassExW(&overlayClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        const DWORD error = GetLastError();
        OPEN_ST_LOG_ERROR("Failed to register the capture overlay class. win32_error=", error);
        this->ShowCaptureError("capture.error.register_overlay_class");
        return;
    }

    const RectI bounds = capturedFrame->Bounds();
    // 先把原生冻结帧所有权交给 App，再创建可能同步发送窗口消息的 HWND，保证回调期间始终有效。
    // bounds 是虚拟桌面物理像素，left/top 可以为负数。
    this->frozenDesktopFrame_ = std::move(capturedFrame);
    this->selectionModel_ = std::make_unique<SelectionModel>();
    this->selectionModel_->SetBounds(bounds);
    this->overlaySession_ = std::make_unique<CaptureOverlaySession>();
    if (this->toolbarGate_ == nullptr)
    {
        this->toolbarGate_ = std::make_unique<CaptureCommandGate>();
    }
    this->toolbarGate_->Invalidate();
    this->toolbarGate_->SetInputBarrier(GetTickCount());
    this->overlayPreparing_ = true;
    this->overlayInvalidated_ = false;

    // 每个交换链仅覆盖其捕获来源显示器，避免 Windows 按单一屏幕解释跨屏 HDR 表面。
    const std::wstring overlayTitle = GetUiText("window.capture.title");
    // 同一次会话只读取一次业务设置；各显示器初始化时借用同一份颜色字符串。
    const std::optional<std::string> configuredBorderColor = GetStringSetting("capture.selection_border_color");
    const std::optional<std::string_view> borderColor =
        configuredBorderColor.has_value() ? std::optional<std::string_view>(*configuredBorderColor) : std::nullopt;
    for (const CapturedOutputPlane& plane : this->frozenDesktopFrame_->Outputs())
    {
        OutputPreviewFrame previewFrame;
        if (!BuildOutputPreview(plane, previewFrame, captureError))
        {
            OPEN_ST_LOG_ERROR("Output preview generation failed. detail=", WideToUtf8(captureError));
            this->CloseOverlay();
            this->ShowCaptureError("capture.error.unknown");
            return;
        }
        const RectI outputBounds = plane.Bounds();
        // 先登记空记录，再创建 HWND；记录分配失败时尚未拥有窗口，避免未登记 HWND 泄漏。
        CaptureOverlayOutput& output = this->overlaySession_->Add(nullptr);
        output.window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, OVERLAY_CLASS, overlayTitle.c_str(), WS_POPUP,
                                        outputBounds.left, outputBounds.top, outputBounds.Width(),
                                        outputBounds.Height(), nullptr, nullptr, this->instance_, this);
        if (output.window == nullptr)
        {
            OPEN_ST_LOG_ERROR("Failed to create a capture output window. win32_error=", GetLastError());
            this->CloseOverlay();
            this->ShowCaptureError("capture.error.create_overlay_window");
            return;
        }
        output.renderer = std::make_unique<OverlayRenderer>();
        if (!output.renderer->Initialize(output.window, previewFrame, borderColor, captureError) ||
            !output.renderer->Render(this->selectionModel_->Snapshot(), captureError))
        {
            OPEN_ST_LOG_ERROR("Failed to prepare a capture output renderer. detail=", WideToUtf8(captureError));
            this->CloseOverlay();
            this->ShowCaptureError("capture.error.unknown");
            return;
        }
        OPEN_ST_LOG_INFO("Capture output prepared. display=", WideToUtf8(plane.ColorMetadata().deviceName.data()),
                         ", format=", static_cast<int>(previewFrame.pixelFormat),
                         ", compatibility=", previewFrame.compatibilityMode,
                         ", ui_white_scale=", previewFrame.uiWhiteScale);
    }

    // 所有资源均已在隐藏状态初始化和首帧绘制；任一输出失败均不会显示残缺会话。
    if (!this->overlayInvalidated_)
    {
        this->overlaySession_->Show();
    }
    this->overlayPreparing_ = false;
    if (this->overlayInvalidated_)
    {
        OPEN_ST_LOG_WARNING("Capture configuration changed while preparing output windows.");
        this->CloseOverlay();
        return;
    }
    OPEN_ST_LOG_DEBUG("All capture output overlays displayed.");
    this->CreateCaptureToolbar();
}
catch (const std::exception&)
{
    // 预览、记录或渲染器分配失败不能让已准备的隐藏 HWND 和冻结帧遗留在半初始化会话中。
    OPEN_ST_LOG_ERROR("Failed to allocate capture overlay session resources.");
    this->CloseOverlay();
    this->ShowCaptureError("capture.error.unknown");
}

// 驱动覆盖窗口绘制、选区鼠标捕获、光标反馈、分层取消和会话销毁。
LRESULT CALLBACK App::OverlayProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    App* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        // 覆盖窗口与隐藏消息窗口使用同一种 HWND → App 绑定方式。
        const CREATESTRUCTW* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        app = static_cast<App*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }

    CaptureOverlayOutput* output =
        app != nullptr && app->overlaySession_ != nullptr ? app->overlaySession_->Find(window) : nullptr;

    // CreateWindow/ShowWindow 会同步派发消息；将失效延迟到调用返回，避免删除调用栈正在使用的记录。
    if (app != nullptr && app->overlayPreparing_)
    {
        if (message == WM_DISPLAYCHANGE || message == WM_SETTINGCHANGE || message == WM_DPICHANGED ||
            message == WM_DESTROY)
        {
            app->overlayInvalidated_ = true;
        }
        if (message == WM_ERASEBKGND)
        {
            return 1;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    if (app != nullptr && app->completionBusy_)
    {
        if (message == WM_DISPLAYCHANGE || message == WM_SETTINGCHANGE || message == WM_DPICHANGED ||
            message == WM_CLOSE || message == WM_DESTROY)
        {
            app->overlayInvalidated_ = true;
            if (message == WM_DESTROY && output != nullptr)
            {
                output->window = nullptr;
            }
            return 0;
        }
        if ((message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) ||
            (message >= WM_KEYFIRST && message <= WM_KEYLAST) || message == WM_CANCELMODE ||
            message == WM_CAPTURECHANGED)
        {
            return 0;
        }
    }
    switch (message)
    {
    case WM_ERASEBKGND:
        // Direct2D 会覆盖整个客户区，跳过 GDI 背景擦除可避免闪烁。
        return 1;
    case WM_PAINT:
    {
        // BeginPaint/EndPaint 只负责验证 Win32 的无效区域；真正绘制由后面的 D2D 交换链完成。
        PAINTSTRUCT paint{};
        BeginPaint(window, &paint);
        EndPaint(window, &paint);
        if (output != nullptr && output->renderer != nullptr && app->selectionModel_ != nullptr &&
            !(app->completionBusy_ && app->overlayInvalidated_))
        {
            std::wstring rendererError;
            const SelectionSnapshot snapshot = app->selectionModel_->Snapshot();
            if (!output->renderer->Render(snapshot, rendererError))
            {
                OPEN_ST_LOG_ERROR("Capture overlay rendering failed. detail=", WideToUtf8(rendererError));
                app->CloseOverlay();
                if (!app->completionBusy_)
                {
                    app->ShowCaptureError("capture.error.unknown");
                }
            }
        }
        return 0;
    }
    case WM_LBUTTONDOWN:
        if (app != nullptr && app->selectionModel_ != nullptr)
        {
            PointI point{};
            if (TryGetCursorPoint(point) && app->selectionModel_->Begin(point))
            {
                app->InvalidateToolbarCommands();
                SetFocus(window);
                SetCapture(window);
                if (GetCapture() != window)
                {
                    OPEN_ST_LOG_WARNING("Failed to capture the mouse for a selection interaction.");
                    (void)app->selectionModel_->CancelInteraction();
                    app->InvalidateToolbarCommands();
                    UpdateOverlayCursor(*app->selectionModel_);
                    app->overlaySession_->Invalidate();
                    return 0;
                }
                UpdateOverlayCursor(*app->selectionModel_);
                app->overlaySession_->Invalidate();
            }
        }
        return 0;
    case WM_MOUSEMOVE:
        if (app != nullptr && app->selectionModel_ != nullptr)
        {
            PointI point{};
            if (app->selectionModel_->Phase() == SelectionPhase::Dragging && TryGetCursorPoint(point) &&
                app->selectionModel_->Update(point))
            {
                app->overlaySession_->Invalidate();
            }
            UpdateOverlayCursor(*app->selectionModel_);
        }
        return 0;
    case WM_LBUTTONUP:
        if (app != nullptr && app->selectionModel_ != nullptr &&
            app->selectionModel_->Phase() == SelectionPhase::Dragging)
        {
            PointI point{};
            if (TryGetCursorPoint(point))
            {
                (void)app->selectionModel_->End(point);
            }
            else
            {
                (void)app->selectionModel_->CancelInteraction();
            }
            if (GetCapture() == window)
            {
                ReleaseCapture();
            }
            UpdateOverlayCursor(*app->selectionModel_);
            app->overlaySession_->Invalidate();
            app->InvalidateToolbarCommands(true);
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (app != nullptr && app->selectionModel_ != nullptr &&
            app->selectionModel_->Phase() == SelectionPhase::Dragging)
        {
            (void)app->selectionModel_->CancelInteraction();
            app->InvalidateToolbarCommands();
            UpdateOverlayCursor(*app->selectionModel_);
            app->overlaySession_->Invalidate();
        }
        return 0;
    case WM_CANCELMODE:
        if (app != nullptr && app->selectionModel_ != nullptr &&
            app->selectionModel_->Phase() == SelectionPhase::Dragging)
        {
            (void)app->selectionModel_->CancelInteraction();
            app->InvalidateToolbarCommands();
            if (GetCapture() == window)
            {
                ReleaseCapture();
            }
            UpdateOverlayCursor(*app->selectionModel_);
            app->overlaySession_->Invalidate();
        }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT && app != nullptr && app->selectionModel_ != nullptr)
        {
            UpdateOverlayCursor(*app->selectionModel_);
            return TRUE;
        }
        break;
    case WM_SIZE:
        if (output != nullptr && output->renderer != nullptr && wParam != SIZE_MINIMIZED &&
            !(app->completionBusy_ && app->overlayInvalidated_))
        {
            RECT client{};
            GetClientRect(window, &client);
            std::wstring rendererError;
            if (!output->renderer->Resize(static_cast<unsigned int>(client.right - client.left),
                                          static_cast<unsigned int>(client.bottom - client.top), rendererError))
            {
                OPEN_ST_LOG_ERROR("Failed to resize the capture overlay renderer.");
                app->CloseOverlay();
                if (!app->completionBusy_)
                {
                    app->ShowCaptureError("capture.error.unknown");
                }
            }
        }
        return 0;
    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE:
    case WM_DPICHANGED:
        // 显示器布局变化后旧帧坐标已失效，直接结束本次会话，避免错误裁切。
        if (app != nullptr)
        {
            OPEN_ST_LOG_WARNING("Display configuration changed; the active capture was cancelled.");
            app->CloseOverlay();
        }
        return 0;
    case WM_KEYDOWN:
        if (app != nullptr && (lParam & (static_cast<LPARAM>(1) << 30)) == 0 && (GetKeyState(VK_MENU) & 0x8000) == 0 &&
            (GetKeyState(VK_SHIFT) & 0x8000) == 0)
        {
            const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if ((wParam == VK_RETURN && !control) || (wParam == 'C' && control))
            {
                if (app->toolbarGate_ != nullptr &&
                    app->toolbarGate_->AcceptsInput(static_cast<DWORD>(GetMessageTime())))
                {
                    (void)app->PostToolbarCommand(CaptureToolbarCommand::Copy, app->toolbarGate_->Token());
                }
                return 0;
            }
            if (wParam == 'S' && control)
            {
                if (app->toolbarGate_ != nullptr &&
                    app->toolbarGate_->AcceptsInput(static_cast<DWORD>(GetMessageTime())))
                {
                    (void)app->PostToolbarCommand(CaptureToolbarCommand::Save, app->toolbarGate_->Token());
                }
                return 0;
            }
        }
        if (wParam == VK_ESCAPE && app != nullptr)
        {
            app->CancelSelectionOrClose();
            return 0;
        }
        break;
    case WM_RBUTTONUP:
        if (app != nullptr)
        {
            app->CancelSelectionOrClose();
        }
        return 0;
    case WM_DESTROY:
        if (app != nullptr)
        {
            // 正常关闭已经解绑回调；单屏被外部销毁时取消整个会话，避免只剩部分显示器。
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            if (output != nullptr)
            {
                output->window = nullptr;
            }
            app->CloseOverlay();
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

// 依次取消当前拖动、清空已有选区或关闭未选择状态的覆盖窗口。
void App::CancelSelectionOrClose() noexcept
{
    if (this->completionBusy_)
    {
        return;
    }
    if (this->selectionModel_ != nullptr && this->selectionModel_->Phase() == SelectionPhase::Dragging)
    {
        (void)this->selectionModel_->CancelInteraction();
        this->InvalidateToolbarCommands();
        if (this->overlaySession_ != nullptr)
        {
            this->overlaySession_->ReleaseMouse();
        }
        UpdateOverlayCursor(*this->selectionModel_);
        if (this->overlaySession_ != nullptr)
        {
            this->overlaySession_->Invalidate();
        }
        return;
    }
    if (this->selectionModel_ != nullptr && this->selectionModel_->Phase() == SelectionPhase::Selected)
    {
        this->selectionModel_->Reset();
        this->InvalidateToolbarCommands();
        UpdateOverlayCursor(*this->selectionModel_);
        if (this->overlaySession_ != nullptr)
        {
            this->overlaySession_->Invalidate();
        }
        return;
    }
    this->CloseOverlay();
}

// 释放鼠标捕获与截图会话对象，并安全销毁当前覆盖窗口。
void App::CloseOverlay() noexcept
{
    if (this->toolbarGate_ != nullptr)
    {
        this->toolbarGate_->Invalidate();
        this->toolbarGate_->SetInputBarrier(GetTickCount());
    }
    if (this->captureToolbar_ != nullptr)
    {
        this->captureToolbar_->Hide();
    }
    if (this->completionBusy_)
    {
        this->overlayInvalidated_ = true;
        return;
    }
    if (this->captureToolbar_ != nullptr)
    {
        this->captureToolbar_->Close();
        this->captureToolbar_.reset();
    }
    this->toolbarMonitor_ = nullptr;
    if (this->outputRenderer_ != nullptr)
    {
        this->outputRenderer_->ReleaseImageResources();
    }
    if (this->overlaySession_ != nullptr)
    {
        // 先断开全部窗口回调，再销毁呈现资源，最后释放共享数据。
        this->overlaySession_->Close();
        this->overlaySession_.reset();
    }
    this->selectionModel_.reset();
    this->frozenDesktopFrame_.reset();
    this->overlayPreparing_ = false;
    this->overlayInvalidated_ = false;
}

// 快捷键和后续菜单共享复制业务入口。
void App::CopySelection()
{
    this->CompleteSelection(false);
}

// 快捷键和后续菜单共享保存业务入口。
void App::SaveSelection()
{
    this->CompleteSelection(true);
}

// 在冻结帧存活期间完成转换和输出；所有模态消息返回后才关闭或恢复会话。
void App::CompleteSelection(bool save)
try
{
    if (this->completionBusy_ || this->dialogActive_ || this->overlayPreparing_ || this->completion_ == nullptr ||
        this->overlaySession_ == nullptr || this->frozenDesktopFrame_ == nullptr || this->selectionModel_ == nullptr ||
        this->selectionModel_->Phase() != SelectionPhase::Selected || !this->selectionModel_->HasSelection())
    {
        return;
    }
    CompletionBusyGuard busyGuard(this->completionBusy_, [this]() { this->UpdateCaptureGate(); });
    WindowDisableGuard settingsGuard(this->settingsWindow_ != nullptr ? this->settingsWindow_->NativeHandle()
                                                                      : nullptr);
    CompletionResult result = save ? CompletionResult::SaveFailed : CompletionResult::CopyFailed;
    std::wstring error;
    try
    {
        const RectI selection = this->selectionModel_->Snapshot().rectangle;
        const HWND owner = this->overlaySession_->ActivationWindow();
        SdrSelectionFrame frame;
        SaveImageTarget target;
        CompletionActions actions;
        // 对话框期间布局失效时不再转换或写入旧坐标对应的图像。
        actions.generate = [this, selection, &frame, &error]()
        {
            return !this->overlayInvalidated_ &&
                   this->outputRenderer_->Render(*this->frozenDesktopFrame_, selection, frame, error);
        };
        // Export 只借用 BGRX 视图，不依赖 Graphics 模型，也不复制整张输出。
        actions.copy = [owner, &frame, &error]()
        {
            const SdrImageView image{static_cast<std::uint32_t>(frame.Bounds().Width()),
                                     static_cast<std::uint32_t>(frame.Bounds().Height()), frame.Stride(),
                                     frame.Pixels()};
            return CopyImageToClipboard(owner, image, error);
        };
        // 上次目录由 Settings 动态键读取；缺失时交给系统选择默认位置。
        actions.chooseSave = [owner, &target, &error]()
        {
            const std::optional<std::string> directory = GetStringSetting("capture.last_save_directory");
            const std::filesystem::path last =
                directory.has_value() ? std::filesystem::path(std::u8string_view(
                                            reinterpret_cast<const char8_t*>(directory->data()), directory->size()))
                                      : std::filesystem::path{};
            return ShowSaveImageDialog(owner, last, target, error);
        };
        // 目标确认及转换成功后再进入编码和文件写入。
        actions.save = [this, &frame, &target, &error]()
        {
            const SdrImageView image{static_cast<std::uint32_t>(frame.Bounds().Width()),
                                     static_cast<std::uint32_t>(frame.Bounds().Height()), frame.Stride(),
                                     frame.Pixels()};
            return !this->overlayInvalidated_ && WriteImageFile(image, target.path, target.format, error);
        };
        // 目录持久化失败不撤销已经成功保存的截图。
        actions.rememberDirectory = [&target]()
        {
            const std::u8string directory = target.path.parent_path().u8string();
            return SetStringSetting(
                "capture.last_save_directory",
                std::string_view(reinterpret_cast<const char*>(directory.data()), directory.size()));
        };
        result =
            save ? this->completion_->SaveSelection(true, actions) : this->completion_->CopySelection(true, actions);
    }
    catch (const std::exception&)
    {
        OPEN_ST_LOG_ERROR("Capture completion encountered a resource or system exception.");
    }
    this->outputRenderer_->ReleaseImageResources();
    const bool success = result == CompletionResult::Copied || result == CompletionResult::Saved ||
                         result == CompletionResult::SavedDirectoryWarning;
    if (!success && result != CompletionResult::Cancelled && result != CompletionResult::Ignored &&
        !this->overlayInvalidated_)
    {
        OPEN_ST_LOG_ERROR("Capture completion failed. stage=", static_cast<int>(result),
                          ", detail=", WideToUtf8(error));
        const char* key = result == CompletionResult::ConversionFailed ? "export.generate_failed"
                          : result == CompletionResult::CopyFailed     ? "export.copy_failed"
                                                                       : "export.save_failed";
        const std::wstring message = GetUiText(key);
        const std::wstring title = GetUiText("app.title");
        (void)MessageBoxW(this->overlaySession_->ActivationWindow(), message.c_str(), title.c_str(),
                          MB_OK | MB_ICONERROR);
    }
    busyGuard.Release();
    if (success || this->overlayInvalidated_)
    {
        this->CloseOverlay();
    }
    else if (this->overlaySession_ != nullptr)
    {
        this->overlaySession_->RestoreFocus();
        this->RefreshCaptureToolbar();
    }
    if (result == CompletionResult::SavedDirectoryWarning)
    {
        const std::wstring message = GetUiText("export.directory_failed");
        const std::wstring title = GetUiText("app.title");
        (void)MessageBoxW(this->DialogOwner(), message.c_str(), title.c_str(), MB_OK | MB_ICONWARNING);
    }
}
catch (const std::exception&)
{
    // Win32 回调不能传播 C++ 异常；守卫已复位，失效会话在此安全回收。
    OPEN_ST_LOG_ERROR("Failed to present capture completion status.");
    if (this->overlayInvalidated_)
    {
        this->CloseOverlay();
    }
    else if (this->overlaySession_ != nullptr)
    {
        this->overlaySession_->RestoreFocus();
        this->RefreshCaptureToolbar();
    }
}

// 清理系统入口后再关闭业务和日志，保留窗口报告日志清理失败。
void App::ShowCleanup()
{
    if (this->dialogActive_ || this->completionBusy_ || this->welcoming_)
    {
        return;
    }
    CompletionBusyGuard guard(this->dialogActive_, [this]() { this->UpdateCaptureGate(); });
    WindowRenderer renderer;
    struct RefreshGuard
    {
        WindowRenderer*& renderer;
        std::function<void()>& status;
        // 在局部窗口销毁前撤销所有借用，避免语言通知使用悬空引用。
        ~RefreshGuard()
        {
            this->renderer = nullptr;
            this->status = {};
        }
    } refreshGuard{this->messageRenderer_, this->messageStatusRefresh_};
    this->messageRenderer_ = &renderer;
    bool deleteLogs = false;
    bool stopped = false;
    bool exitRequested = false;
    std::string statusKey;
    this->messageStatusRefresh_ = [&renderer, &statusKey]()
    { (void)renderer.SetStatus(statusKey.empty() ? L"" : GetUiText(statusKey)); };
    const nlohmann::json layout = nlohmann::json::parse(R"({
        "schemaVersion":1,
        "window":{"titleKey":"cleanup.title","initialSize":[560,320],"minSize":[440,280],"resizable":true},
        "content":{"type":"column","id":"cleanupBody","padding":20,"gap":12,"children":[
            {"type":"text","id":"cleanupText","textKey":"cleanup.body"},
            {"type":"checkbox","id":"deleteLogs","labelKey":"cleanup.logs"}]},
        "footer":{"leading":[],"trailing":[
            {"type":"button","id":"cleanupConfirm","textKey":"cleanup.confirm"},
            {"type":"button","id":"cleanupCancel","textKey":"cleanup.cancel"}]}})");
    const auto require = [](const RendererResult& result)
    {
        if (!result)
        {
            throw std::runtime_error("Cleanup window failed");
        }
    };
    try
    {
        require(renderer.LoadLayout(layout));
        require(renderer.SetTextResolver(
            [&stopped](std::string_view key)
            { return GetUiText(key == "cleanup.cancel" && stopped ? "cleanup.keep_logs_exit" : key); }));
        require(renderer.BindBool(
            "deleteLogs", [&deleteLogs]() { return RendererBoolResult{true, deleteLogs, {}}; },
            [&deleteLogs](bool value)
            {
                deleteLogs = value;
                return RendererChangeResult{};
            }));
        const auto cancel = [&]()
        {
            exitRequested = stopped;
            (void)renderer.RequestClose();
        };
        require(renderer.BindAction("cleanupCancel", cancel));
        require(renderer.SetCloseHandler(cancel));
        require(renderer.SetDefaultAction("cleanupCancel"));
        require(renderer.BindAction("cleanupConfirm",
                                    [&]()
                                    {
                                        if (!stopped)
                                        {
                                            if (!SetBoolSetting("startup.enabled", false))
                                            {
                                                statusKey = "cleanup.save_failed";
                                                (void)renderer.SetStatus(GetUiText(statusKey));
                                                return;
                                            }
                                            if (!this->ApplyStartup(false))
                                            {
                                                statusKey = "cleanup.startup_failed";
                                                (void)renderer.SetStatus(GetUiText(statusKey));
                                                return;
                                            }
                                            this->shuttingDown_ = true;
                                            if (this->settingsWindow_ != nullptr)
                                            {
                                                this->settingsWindow_->Close();
                                            }
                                            this->CloseOverlay();
                                            UnregisterHotKey(this->messageWindow_, CAPTURE_HOTKEY_ID);
                                            if (!deleteLogs)
                                            {
                                                ShutdownLogging();
                                            }
                                            stopped = true;
                                            (void)renderer.SetEnabled("deleteLogs", false);
                                            (void)renderer.RefreshTexts();
                                        }
                                        if (deleteLogs && !Logger::ShutdownAndClear())
                                        {
                                            statusKey = "cleanup.logs_failed";
                                            (void)renderer.SetStatus(GetUiText(statusKey));
                                            return;
                                        }
                                        exitRequested = true;
                                        (void)renderer.RequestClose();
                                    }));
        RendererWindowOptions options;
        options.owner = this->messageWindow_;
        options.icon = this->largeIcon_;
        require(renderer.ShowModal(options));
    }
    catch (...)
    {
        (void)MessageBoxW(this->DialogOwner(), GetUiText("cleanup.failed").c_str(), GetUiText("cleanup.title").c_str(),
                          MB_OK | MB_ICONERROR);
        exitRequested = stopped;
    }
    if (exitRequested)
    {
        this->RemoveTrayIcon();
        PostQuitMessage(0);
    }
}

// 显示包含当前构建版本号的本地化关于对话框。
void App::ShowAbout()
{
    this->ShowSimpleMessage([]() { return GetUiText("about.body", {{L"version", OPEN_ST_WIDEN(OPEN_ST_VERSION)}}); },
                            []() { return GetUiText("about.title"); }, MB_OK | MB_ICONINFORMATION);
}

// 将底层截图失败详情嵌入本地化消息外壳并显示错误对话框。
void App::ShowCaptureError(std::string_view detailKey)
{
    this->ShowSimpleMessage([detailKey]()
                            { return GetUiText("capture.error.message", {{L"detail", GetUiText(detailKey)}}); },
                            []() { return GetUiText("app.title"); }, MB_OK | MB_ICONERROR);
}

// 守卫覆盖 Renderer 与系统兜底两条路径；正常关闭和 WM_QUIT 返回不再次弹窗。
void App::ShowSimpleMessage(std::function<std::wstring()> message, std::function<std::wstring()> title,
                            UINT fallbackFlags)
{
    if (this->dialogActive_)
    {
        return;
    }
    CompletionBusyGuard dialogGuard(this->dialogActive_, [this]() { this->UpdateCaptureGate(); });
    bool shown = false;
    try
    {
        shown = TryShowSimpleMessageWindow(
            this->messageWindow_, this->largeIcon_, title, message, []() { return GetUiText("dialog.ok"); },
            this->messageRenderer_, [this](MSG& threadMessage)
            { return this->settingsWindow_ != nullptr && this->settingsWindow_->ProcessDialogMessage(threadMessage); });
    }
    catch (...)
    {
        OPEN_ST_LOG_ERROR("Failed to prepare the application message window.");
    }
    if (!shown)
    {
        (void)MessageBoxW(this->DialogOwner(), message().c_str(), title().c_str(), fallbackFlags);
    }
    dialogGuard.Release();
    if (this->overlaySession_ != nullptr)
    {
        const HWND foreground = GetForegroundWindow();
        if (foreground != nullptr &&
            (foreground == this->messageWindow_ || this->overlaySession_->Find(foreground) != nullptr))
        {
            this->overlaySession_->RestoreFocus();
            this->RefreshCaptureToolbar();
        }
    }
}
} // namespace open_st
