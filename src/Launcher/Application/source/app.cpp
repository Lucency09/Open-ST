// 实现应用启动退出、托盘与消息分派，并协调截图、工具栏、设置及图像输出。

#include "capture_command_gate.h"
#include "capture_completion.h"
#include "capture_toolbar_monitor.h"
#include "diagnostic_text.h"
#include "save_directory.h"
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
#include <pin_window_manager.h>
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
// 把已展开的版本字符串字面量转换为宽字符串字面量。
// 入参：value 为待添加 L 前缀的字符串字面量预处理记号。
// 返回：预处理展开结果为带 L 前缀的宽字符串字面量，无运行时调用。
#define OPEN_ST_WIDEN_IMPL(value) L##value
// 先展开 CMake 版本宏，再将版本字符串转换为宽字符串。
// 入参：value 为窄字符串字面量或展开后得到该字面量的宏。
// 返回：宽字符串字面量预处理记号，无运行时调用。
#define OPEN_ST_WIDEN(value) OPEN_ST_WIDEN_IMPL(value)
constexpr wchar_t MESSAGE_CLASS[] = L"OpenST.MessageWindow";
constexpr wchar_t OVERLAY_CLASS[] = L"OpenST.CaptureOverlay";
constexpr UINT LAUNCH_MESSAGE = WM_APP + 3;
constexpr UINT TOOLBAR_COMMAND_MESSAGE = WM_APP + 4;
constexpr UINT PIN_COMMAND_MESSAGE = WM_APP + 5;
constexpr UINT PIN_STOPPED_MESSAGE = WM_APP + 6;
constexpr UINT PREPARE_OUTPUT_MESSAGE = WM_APP + 2;
constexpr UINT TRAY_MESSAGE = WM_APP + 1;
constexpr UINT TRAY_ID = 1;
constexpr int CAPTURE_HOTKEY_ID = 1;

// 让包含本地化错误弹窗在内的同步完成流程在所有异常路径恢复 busy。
class CompletionBusyGuard final
{
  public:
    // 在模态或截图完成流程中设置忙状态并通知准入门禁。
    // 入参：busy：借用的忙标记；changed：可选状态变更回调，构造和释放时调用，必须不抛异常。
    // 返回：构造函数无返回值；设置 busy 为 true 并调用非空通知。
    explicit CompletionBusyGuard(bool& busy, std::function<void()> changed = {})
        : busy_(busy), changed_(std::move(changed))
    {
        this->busy_ = true;
        if (this->changed_)
        {
            this->changed_();
        }
    }
    // 在守卫退出时解除忙状态，确保异常路径也恢复准入。
    // 入参：无。
    // 返回：析构函数无返回值；委托 Release，只释放一次。
    ~CompletionBusyGuard()
    {
        this->Release();
    }
    // 禁止复制构造，确保完成流程的忙标记只由原对象管理。
    // 入参：未命名的同类型 const 引用：拟复制的源对象。
    // 返回：函数已删除，调用会导致编译错误，无运行时返回结果。
    CompletionBusyGuard(const CompletionBusyGuard&) = delete;
    // 禁止复制赋值，避免完成流程的忙标记出现多个所有者。
    // 入参：未命名的同类型 const 引用：拟复制的源对象。
    // 返回：函数已删除，调用会导致编译错误，无运行时返回结果。
    CompletionBusyGuard& operator=(const CompletionBusyGuard&) = delete;
    // 提前解除当前守卫的忙状态，以便安全关闭截图会话。
    // 入参：无。
    // 返回：无返回值；首次调用将 busy 置 false 并通知 changed，后续调用不操作。
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
    // 暂时禁用已启用的所属窗口以避免系统模态期间继续交互。
    // 入参：window：借用的所属窗口句柄，允许 nullptr。
    // 返回：构造函数无返回值；只记录并禁用原本启用的窗口。
    explicit WindowDisableGuard(HWND window) : window_(window), restore_(window != nullptr && IsWindowEnabled(window))
    {
        if (this->restore_)
        {
            EnableWindow(this->window_, FALSE);
        }
    }
    // 恢复由当前守卫暂时禁用的窗口。
    // 入参：无。
    // 返回：析构函数无返回值；仅在窗口仍存在且原本启用时恢复。
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

// 将转换结果转换为日志可用视图，并在空结果时提供静态兜底文本。
// 入参：value：当前完整表达式内借用的转换结果。
// 返回：非空时返回 value 的视图；空结果时返回不分配的静态英文诊断。
std::string_view DiagnosticOrFallback(const std::string& value) noexcept
{
    return value.empty() ? std::string_view{"<diagnostic unavailable>"} : std::string_view{value};
}

// 查询当前鼠标在虚拟桌面上的位置。
// 入参：point：输出参数，成功时写入虚拟桌面物理像素坐标。
// 返回：查询成功 true；失败 false 且不修改 point。
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

// 为选区缩放控制点选择对应方向的系统光标。
// 入参：handle：命中的选区控制点方向枚举。
// 返回：对应方向的共享系统光标；无控制点时用箭头，加载失败可能返回 nullptr，无需调用方销毁。
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

// 根据选区操作和鼠标命中位置选择交互光标。
// 入参：selection：只读选区模型；point：鼠标的虚拟桌面物理像素坐标。
// 返回：创建、移动、缩放或箭头的共享系统光标句柄，不转移所有权。
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

// 按最新鼠标位置和选区操作状态更新覆盖窗口光标。
// 入参：selection：用于判断当前操作及命中位置的只读选区模型。
// 返回：无返回值；鼠标位置查询失败时保留现有光标。
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
// 创建应用协调器并保存进程模块实例。
// 入参：instance：借用的当前程序模块句柄。
// 返回：构造函数无返回值；窗口、设置及捕获资源留待运行时初始化。
App::App(HINSTANCE instance) noexcept : instance_(instance) {}

// 关闭应用持有的业务窗口和进程级服务并释放系统资源。
// 入参：无。
// 返回：析构函数无返回值；停止实例监听，关闭设置和截图会话，移除托盘并释放图标、消息窗口、COM 和日志。
App::~App()
{
    this->shuttingDown_ = true;
    this->pendingPinId_ = 0;
    if (this->singleInstance_ != nullptr)
    {
        this->singleInstance_->Stop();
    }
    OPEN_ST_LOG_INFO("Application shutting down.");
    if (this->pinManager_)
        this->pinManager_->Shutdown();
    this->CloseOverlay();
    this->pinManager_.reset();
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

// 初始化应用服务并运行主消息循环直到退出。
// 入参：showCommand（定义中未命名的 int）：Win32 初始显示方式，当前托盘应用不使用该值。
// 返回：正常退出返回 WM_QUIT 的退出码；初始化或消息循环失败返回 1；成功转发第二实例请求返回 0。
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
    this->pinManager_ = std::make_unique<PinWindowManager>(this->instance_, this->MakePinCallbacks());
    this->AddTrayIcon();
    std::array<wchar_t, 32768> executable{};
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    this->singleInstance_->SetCaptureGate(this->MakeCaptureGateCallback());
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
        // 在模态业务忙状态变化后同步截图准入门禁。
        // 入参：无显式入参；捕获存活中的 App 指针。
        // 返回：无返回值；调用 UpdateCaptureGate 发布最新状态及代次。
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
            // 生成启动项配置不一致提示正文。
            // 入参：无显式入参；捕获 App 以查询当前启动状态。
            // 返回：当前语言的不一致说明及系统启动项状态宽字符串。
            this->ShowSimpleMessage([this]()
                                    { return GetUiText("startup.mismatch") + L"\n\n" + this->StartupStatusText(); },
                                    // 提供应用提示窗口的当前语言标题。
                                    // 入参：无。
                                    // 返回：app.title 对应的本地化宽字符串。
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

// 合并并显示设置或文本资源读取失败的待提示告警。
// 入参：无。
// 返回：无返回值；截图或模态提示期间延后处理，其余时消费告警并显示一次合并提示。
void App::ReportDataReadWarnings()
{
    if (this->overlaySession_ != nullptr || this->overlayPreparing_ || this->dialogActive_ || this->shuttingDown_)
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
    // 以已解析快照提示读取故障，模态期间暂停全部贴图。
    // 入参：无；按值保留本次正文，避免失败资源被反复读取。
    // 返回：错误正文。
    this->ShowSimpleMessage([message]() { return message; },
                            // 返回已解析的告警标题。
                            // 入参：无。
                            // 返回：标题快照。
                            [title]() { return title; }, MB_OK | MB_ICONWARNING);
}

// 注册并创建接收热键、托盘和业务命令的隐藏消息窗口。
// 入参：无。
// 返回：消息窗口创建成功时 true；注册或创建失败时 false 并记录日志。
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

// 建立隐藏消息窗口与 App 的关联并转发窗口消息。
// 入参：window：接收消息的窗口句柄；message：Win32 消息编号；wParam、lParam：对应消息的附加数据。
// 返回：App 实例处理消息的结果；尚未绑定实例时返回 DefWindowProcW 的结果。
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

// 处理应用消息中枢收到的启动请求、热键、托盘和退出命令。
// 入参：window：接收消息的窗口句柄；message：Win32 消息编号；wParam、lParam：对应消息的附加数据。
// 返回：已消费业务消息的处理结果；其他消息返回 DefWindowProcW 的结果。
LRESULT App::HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == PIN_STOPPED_MESSAGE)
    {
        this->UpdateCaptureGate();
        return 0;
    }
    if (message == PIN_COMMAND_MESSAGE)
    {
        this->DispatchPinCommand(static_cast<PinCommand>(wParam), static_cast<std::uint64_t>(lParam));
        return 0;
    }
    if (message == WM_CLOSE)
    {
        this->shuttingDown_ = true;
        this->pendingPinId_ = 0;
        if (this->pinManager_)
            this->pinManager_->Shutdown();
        this->CloseOverlay();
        this->UpdateCaptureGate();
        return 0;
    }
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
            OPEN_ST_LOG_WARNING("HDR output warm-up failed. detail=", DiagnosticOrFallback(WideToUtf8(error)));
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
            SendMessageW(this->messageWindow_, WM_CLOSE, 0, 0);
            return 0;
        case ID_TRAY_PIN_CLOSE_ALL:
            this->pendingPinId_ = 0;
            if (this->pinManager_)
                this->pinManager_->CloseAll();
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

// 为常驻应用添加系统托盘入口并加载图标。
// 入参：无。
// 返回：无返回值；成功后记录托盘状态，失败仅记日志，不阻止消息循环。
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

// 移除应用已经添加的系统托盘图标。
// 入参：无。
// 返回：无返回值；清除本地添加标记，未添加时不操作。
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

// 在语言切换后刷新现有窗口和托盘中的本地化文本。
// 入参：无。
// 返回：无返回值；更新已存在的界面对象，不创建新的业务窗口。
void App::RefreshLocalizedUi()
{
    if (this->pinManager_)
        this->pinManager_->RefreshText();
    if (this->captureToolbar_ != nullptr)
    {
        const ToolbarResult result = this->captureToolbar_->RefreshTexts();
        if (!result.success)
        {
            OPEN_ST_LOG_WARNING("Failed to refresh capture toolbar texts. detail=",
                                DiagnosticOrFallback(WideToUtf8(result.error)));
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

// 在当前光标位置提供截图、设置、关于及退出托盘菜单。
// 入参：无。
// 返回：无返回值；用户选中的命令通过窗口消息交回 App 处理。
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
    if (this->pinManager_ && this->pinManager_->Count() != 0)
    {
        const std::wstring closePinsText = GetUiText("tray.pin_close_all");
        (void)AppendMenuW(menu, MF_STRING, ID_TRAY_PIN_CLOSE_ALL, closePinsText.c_str());
    }
    (void)AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    (void)AppendMenuW(menu, MF_STRING, ID_TRAY_SETTINGS, settingsText.c_str());
    (void)AppendMenuW(menu, MF_STRING, ID_TRAY_ABOUT, aboutText.c_str());
    (void)AppendMenuW(menu, MF_STRING, ID_TRAY_CLEANUP, cleanupText.c_str());
    (void)AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, exitText.c_str());
    // Win32 托盘菜单需要先把所属窗口设为前台，否则用户点击菜单外部时菜单可能无法自动收起。
    SetForegroundWindow(this->messageWindow_);
    const bool pausePins = this->pinManager_ && this->pinManager_->BeginModal(0);
    const UINT selected = static_cast<UINT>(TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                                           cursor.x, cursor.y, 0, this->messageWindow_, nullptr));
    DestroyMenu(menu);
    if (pausePins)
        this->pinManager_->EndModal();
    if (selected != 0)
        SendMessageW(this->messageWindow_, WM_COMMAND, selected, 0);
}

// 打开或激活应用的唯一非模态设置窗口。
// 入参：无。
// 返回：无返回值；首次创建时注入业务回调，忙状态下不重复开启。
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
        // 查询设置窗口创建失败提示，使用统一模态门禁暂停贴图。
        // 入参：无。
        // 返回：当前语言的错误文字。
        this->ShowSimpleMessage([]() { return GetUiText("settings.open_failed"); },
                                // 查询提示标题。
                                // 入参：无。
                                // 返回：当前语言的应用名称。
                                []() { return GetUiText("app.title"); }, MB_OK | MB_ICONWARNING);
    }
}

// 查询当前用户启动入口并生成可显示的本地化状态。
// 入参：无。
// 返回：与当前启动注册状态对应的宽字符串；不代表 Windows 系统启动许可已开启。
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

// 按用户已提交的设置启用或移除当前用户启动入口。
// 入参：enabled：true 请求登记当前程序启动路径，false 请求移除入口。
// 返回：系统入口达到请求状态时 true；系统集成服务缺失或操作失败时 false。
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

// 选择系统模态提示的所属窗口。
// 入参：无。
// 返回：依次选择有效截图窗口、可见贴图、设置或隐藏消息窗口；不转移所有权。
HWND App::DialogOwner() const noexcept
{
    if (this->overlaySession_ && IsWindow(this->overlaySession_->ActivationWindow()))
        return this->overlaySession_->ActivationWindow();
    if (this->pinManager_)
    {
        const HWND pin = this->pinManager_->ModalOwner();
        if (pin)
            return pin;
    }
    if (this->settingsWindow_ != nullptr && this->settingsWindow_->IsOpen())
    {
        return this->settingsWindow_->NativeHandle();
    }
    return this->messageWindow_;
}

// 根据应用忙状态发布跨线程截图准入状态并更新代次。
// 入参：无。
// 返回：无返回值；原子更新暂停标记和代次，使较早排队的截图请求失效。
void App::UpdateCaptureGate() noexcept
{
    const bool paused =
        this->completionBusy_ || this->dialogActive_ || this->welcoming_ || this->shuttingDown_ || this->settingsBusy_;
    const std::uint64_t next = (this->captureGate_.load(std::memory_order_relaxed) & ~std::uint64_t{1}) + 2;
    this->captureGate_.store(next | static_cast<std::uint64_t>(paused), std::memory_order_release);
    if (paused)
        this->pendingPinId_ = 0;
    const bool pausePins = this->dialogActive_ || this->settingsBusy_ || this->welcoming_;
    if (this->pinManager_ && pausePins && !this->pinModalHeld_)
        this->pinModalHeld_ = this->pinManager_->BeginModal(0);
    else if (this->pinManager_ && !pausePins && this->pinModalHeld_)
    {
        this->pinModalHeld_ = false;
        this->pinManager_->EndModal();
    }
    this->InvalidateToolbarCommands();
    if (this->shuttingDown_ && !this->completionBusy_ && !this->dialogActive_ && !this->settingsBusy_ &&
        !this->welcoming_ && (!this->pinManager_ || !this->pinManager_->IsBusy()))
        PostQuitMessage(0);
}

// 判断当前截图会话能否接受新的完成命令。
// 入参：无。
// 返回：会话有效、选区稳定且不处于准备或模态忙状态时 true，否则 false。
bool App::CanSubmitToolbarCommand() const noexcept
{
    return !this->completionBusy_ && !this->dialogActive_ && !this->welcoming_ && !this->shuttingDown_ &&
           !this->settingsBusy_ && !this->overlayPreparing_ && !this->overlayInvalidated_ &&
           this->overlaySession_ != nullptr && this->selectionModel_ != nullptr &&
           this->selectionModel_->Phase() == SelectionPhase::Selected && this->selectionModel_->HasSelection();
}

// 为截图完成命令预订处理位置并投递到 App 消息队列。
// 入参：command：稳定的工具栏命令 ID；token：提交时的选区代次。
// 返回：预订及消息投递成功时 true；状态无效、已有请求或投递失败时 false，并撤销失败预订。
bool App::PostToolbarCommand(CaptureToolbarCommand command, std::uint64_t token) noexcept
{
    if (command != CaptureToolbarCommand::Cancel && command != CaptureToolbarCommand::Save &&
        command != CaptureToolbarCommand::Copy && command != CaptureToolbarCommand::Pin)
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

// 校验并消费排队的工具栏命令，调用对应截图完成入口。
// 入参：command：消息携带的命令 ID；token：消息携带的选区代次。
// 返回：无返回值；过期或不匹配请求被丢弃，有效请求在按钮回调返回后执行业务。
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
    case CaptureToolbarCommand::Pin:
        this->PinSelection();
        break;
    }
    this->RefreshCaptureToolbar();
}

// 撤销旧选区的排队命令并刷新工具栏。
// 入参：updateMonitor：是否同时重新选择工具栏目标显示器，默认 false。
// 返回：无返回值；更新命令代次及输入时间边界后同步工具栏。
void App::InvalidateToolbarCommands(bool updateMonitor) noexcept
{
    if (this->toolbarGate_ != nullptr)
    {
        this->toolbarGate_->Invalidate();
        this->toolbarGate_->SetInputBarrier(GetTickCount());
    }
    this->RefreshCaptureToolbar(updateMonitor);
}

// 按当前选区状态同步工具栏显示位置和可见性。
// 入参：updateMonitor：true 重新选择目标显示器；false 保留当前目标屏幕。
// 返回：无返回值；无稳定选区时隐藏工具栏，定位或显示失败记录诊断。
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
            // 收集当前枚举到的显示器以选择工具栏目标屏幕。
            // 入参：monitor：当前显示器；未命名
            // HDC、LPRECT：本回调不使用的设备上下文与矩形；data：借用的显示器向量指针。
            // 返回：追加成功 TRUE
            // 继续枚举；内存分配等异常时 FALSE 终止枚举。
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
        OPEN_ST_LOG_WARNING("Failed to position capture toolbar. detail=",
                            DiagnosticOrFallback(WideToUtf8(placed.error)));
        return;
    }
    this->captureToolbar_->SetBusy(this->toolbarGate_->Pending());
    const ToolbarResult shown = this->captureToolbar_->Show(this->toolbarGate_->Token());
    if (!shown.success)
    {
        OPEN_ST_LOG_WARNING("Failed to show capture toolbar. detail=", DiagnosticOrFallback(WideToUtf8(shown.error)));
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

// 为已建立的截图覆盖会话创建无激活工具栏。
// 入参：无。
// 返回：无返回值；失败记录并提示，截图会话仍可通过键盘操作。
void App::CreateCaptureToolbar() noexcept
{
    try
    {
        this->captureToolbar_ = std::make_unique<CaptureToolbar>();
        std::vector<ToolbarButtonSpec> buttons{
            {CaptureToolbarCommand::Cancel, ToolbarIcon::Cancel, "capture.toolbar.cancel", 0},
            {CaptureToolbarCommand::Pin, ToolbarIcon::Pin, "capture.toolbar.pin", 1},
            {CaptureToolbarCommand::Save, ToolbarIcon::Save, "capture.toolbar.save", 1},
            {CaptureToolbarCommand::Copy, ToolbarIcon::Copy, "capture.toolbar.copy", 1}};
        const ToolbarResult result = this->captureToolbar_->Create(
            this->instance_, this->overlaySession_->ActivationWindow(), std::move(buttons),
            this->MakeToolbarTextResolver(), this->MakeToolbarCommandHandler());
        if (result.success)
        {
            return;
        }
        OPEN_ST_LOG_WARNING("Failed to create capture toolbar. detail=",
                            DiagnosticOrFallback(WideToUtf8(result.error)));
    }
    catch (...)
    {
        OPEN_ST_LOG_WARNING("Failed to allocate capture toolbar.");
    }
    this->captureToolbar_.reset();
    // 同步提示期间冻结会话输入，返回后先检查显示布局是否仍有效。
    try
    {
        // 在模态业务忙状态变化后同步截图准入门禁。
        // 入参：无显式入参；捕获存活中的 App 指针。
        // 返回：无返回值；调用 UpdateCaptureGate 发布最新状态及代次。
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

// 捕获冻结桌面并建立一次跨显示器截图选区会话。
// 入参：无。
// 返回：无返回值；成功显示覆盖窗口并创建工具栏，稳定选区后再显示工具栏；失败清理已创建资源并提示原因。
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

    if (this->pinManager_ && this->pinManager_->IsBusy())
        return;
    this->pendingPinId_ = 0;
    struct CloseCaptureOnFailure final
    {
        App& app;
        bool keepSession{};
        // 在失败或异常时统一清理，保证遮罩销毁后才恢复贴图交互。
        // 入参：无。
        // 返回：无返回值；退出中的管理器自行禁止恢复。
        ~CloseCaptureOnFailure()
        {
            if (!this->keepSession)
                this->app.CloseOverlay();
        }
    } captureGuard{*this};
    std::wstring pinError;
    if (this->pinManager_ && !this->pinManager_->BeginCapture(pinError))
    {
        OPEN_ST_LOG_ERROR("Cannot pause existing pins before capture. detail=",
                          DiagnosticOrFallback(WideToUtf8(pinError)));
        this->ShowCaptureError("capture.error.unknown");
        return;
    }
    // 旧贴图保持可见，当前缩放、透明度和叠放外观一并进入冻结帧；截图遮罩此时尚未显示。
    std::unique_ptr<FrozenDesktopFrame> capturedFrame = std::make_unique<FrozenDesktopFrame>();
    // 捕获当前虚拟桌面的原生 SDR/HDR plane；底层诊断不直接显示给用户。
    DesktopCapturer capturer;
    std::wstring captureError;
    if (!capturer.Capture(*capturedFrame, captureError))
    {
        OPEN_ST_LOG_ERROR("Desktop capture failed. detail=", DiagnosticOrFallback(WideToUtf8(captureError)));
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
            OPEN_ST_LOG_ERROR("Output preview generation failed. detail=",
                              DiagnosticOrFallback(WideToUtf8(captureError)));
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
            OPEN_ST_LOG_ERROR("Failed to prepare a capture output renderer. detail=",
                              DiagnosticOrFallback(WideToUtf8(captureError)));
            this->CloseOverlay();
            this->ShowCaptureError("capture.error.unknown");
            return;
        }
        OPEN_ST_LOG_INFO("Capture output prepared. display=",
                         DiagnosticOrFallback(WideToUtf8(plane.ColorMetadata().deviceName.data())),
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
    captureGuard.keepSession = this->overlaySession_ != nullptr;
}
catch (const std::exception&)
{
    // 预览、记录或渲染器分配失败不能让已准备的隐藏 HWND 和冻结帧遗留在半初始化会话中。
    OPEN_ST_LOG_ERROR("Failed to allocate capture overlay session resources.");
    this->CloseOverlay();
    this->ShowCaptureError("capture.error.unknown");
}

// 处理截图覆盖窗口的绘制、选区输入及会话失效消息。
// 入参：window：接收消息的窗口句柄；message：Win32 消息编号；wParam、lParam：对应消息的附加数据。
// 返回：已处理消息的 Win32 结果；无应用关联或未处理消息交给默认窗口过程。
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
                OPEN_ST_LOG_ERROR("Capture overlay rendering failed. detail=",
                                  DiagnosticOrFallback(WideToUtf8(rendererError)));
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

// 按层级处理 Esc 或右键取消操作。
// 入参：无。
// 返回：无返回值；优先取消拖动，再清空已有选区，最后关闭无选区的覆盖会话。
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

// 结束当前截图会话并释放覆盖窗口及相关资源。
// 入参：无。
// 返回：无返回值；完成或模态忙状态下先标记失效，待同步调用结束后清理；其余情况立即释放。
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
    if (this->pinManager_ && !this->shuttingDown_)
        this->pinManager_->EndCapture();
}

// 把当前稳定截图选区复制到系统剪贴板。
// 入参：无。
// 返回：无返回值；经统一完成流程处理，不允许输出时忽略，失败显示本地化提示。
void App::CopySelection()
{
    this->CompleteSelection(false);
}

// 让用户选择文件目标并保存当前稳定截图选区。
// 入参：无。
// 返回：无返回值；经统一完成流程处理，取消或失败保留仍有效的截图会话。
void App::SaveSelection()
{
    this->CompleteSelection(true);
}

// 协调选区图像转换、复制或保存及成功后的会话收尾。
// 入参：save：true 选择路径并保存文件，false 写入剪贴板。
// 返回：无返回值；成功关闭会话，取消或失败恢复仍有效的选区，布局失效则安全释放。
void App::CompleteSelection(bool save)
try
{
    if (this->completionBusy_ || this->dialogActive_ || this->overlayPreparing_ || this->completion_ == nullptr ||
        this->overlaySession_ == nullptr || this->frozenDesktopFrame_ == nullptr || this->selectionModel_ == nullptr ||
        this->selectionModel_->Phase() != SelectionPhase::Selected || !this->selectionModel_->HasSelection())
    {
        return;
    }
    // 在模态业务忙状态变化后同步截图准入门禁。
    // 入参：无显式入参；捕获存活中的 App 指针。
    // 返回：无返回值；调用 UpdateCaptureGate 发布最新状态及代次。
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
        // 从冻结桌面生成当前选区的 SDR 输出图像。
        // 入参：无显式入参；捕获 selection 及 App，借用输出 frame 和诊断 error。
        // 返回：会话未失效且转换成功时 true；布局失效或转换失败 false。
        actions.generate = [this, selection, &frame, &error]()
        {
            return !this->overlayInvalidated_ &&
                   this->outputRenderer_->Render(*this->frozenDesktopFrame_, selection, frame, error);
        };
        // 把已生成的选区图像适配为 Export 视图并发布到剪贴板。
        // 入参：无显式入参；捕获 owner，借用 frame 像素和 error 诊断输出。
        // 返回：剪贴板发布成功 true；失败 false 并写入诊断，不转移 frame 像素所有权。
        actions.copy = [owner, &frame, &error]()
        {
            const SdrImageView image{static_cast<std::uint32_t>(frame.Bounds().Width()),
                                     static_cast<std::uint32_t>(frame.Bounds().Height()), frame.Stride(),
                                     frame.Pixels()};
            return CopyImageToClipboard(owner, image, error);
        };
        // 读取上次保存目录并显示系统图片保存对话框。
        // 入参：无显式入参；捕获 owner，借用 target 和 error 作为选择结果和诊断输出。
        // 返回：ShowSaveImageDialog 的 Accepted、Cancelled 或 Failed 状态。
        actions.chooseSave = [owner, &target, &error]()
        { return ShowSaveImageDialog(owner, LastSaveDirectory(), target, error); };
        // 将生成的 SDR 选区图像写入用户已确认的文件目标。
        // 入参：无显式入参；借用 frame、target、error，捕获 App 校验会话有效性。
        // 返回：会话有效且文件写入成功 true；会话失效或写入失败 false。
        actions.save = [this, &frame, &target, &error]()
        {
            const SdrImageView image{static_cast<std::uint32_t>(frame.Bounds().Width()),
                                     static_cast<std::uint32_t>(frame.Bounds().Height()), frame.Stride(),
                                     frame.Pixels()};
            return !this->overlayInvalidated_ && WriteImageFile(image, target.path, target.format, error);
        };
        // 记住成功保存截图的父目录以便下次打开保存对话框。
        // 入参：无显式入参；借用 target 获取父目录。
        // 返回：设置写入成功 true；写入失败 false，不撤销已保存的图片。
        actions.rememberDirectory = [&target]() { return RememberSaveDirectory(target.path); };
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
                          ", detail=", DiagnosticOrFallback(WideToUtf8(error)));
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
        // 在旧贴图恢复后仍对目录告警应用全局模态暂停。
        // 入参：无。
        // 返回：当前语言的保存目录告警。
        this->ShowSimpleMessage([]() { return GetUiText("export.directory_failed"); },
                                // 查询应用告警标题。
                                // 入参：无。
                                // 返回：应用名称。
                                []() { return GetUiText("app.title"); }, MB_OK | MB_ICONWARNING);
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

// 显示可重试的退出清理窗口并执行用户选择的清理步骤。
// 入参：无。
// 返回：无返回值；失败保留状态提示，用户完成清理或确认退出后请求结束应用。
void App::ShowCleanup()
{
    if (this->dialogActive_ || this->completionBusy_ || this->welcoming_)
    {
        return;
    }
    // 在模态业务忙状态变化后同步截图准入门禁。
    // 入参：无显式入参；捕获存活中的 App 指针。
    // 返回：无返回值；调用 UpdateCaptureGate 发布最新状态及代次。
    CompletionBusyGuard guard(this->dialogActive_, [this]() { this->UpdateCaptureGate(); });
    WindowRenderer renderer;
    struct RefreshGuard
    {
        WindowRenderer*& renderer;
        std::function<void()>& status;
        // 在清理窗口退出时撤销 App 对局部窗口及刷新回调的借用。
        // 入参：无。
        // 返回：析构函数无返回值；清空 messageRenderer_ 和 messageStatusRefresh_。
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
    // 按当前语言刷新清理窗口的操作状态文字。
    // 入参：无显式入参；借用 renderer 和 statusKey。
    // 返回：无返回值；状态键为空时清空提示，否则写入本地化状态。
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
    // 检查清理窗口准备步骤是否成功，统一进入异常处理。
    // 入参：result：布局加载或回调绑定的 RendererResult。
    // 返回：无返回值；失败时抛出 runtime_error，成功时继续。
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
            // 解析清理窗口文本并在业务已停止时切换退出按钮文案。
            // 入参：key：布局文本键；借用 stopped 判断业务是否已经停止。
            // 返回：当前语言文本；已停止时将取消按钮替换为保留日志并退出的文字。
            [&stopped](std::string_view key)
            { return GetUiText(key == "cleanup.cancel" && stopped ? "cleanup.keep_logs_exit" : key); }));
        require(renderer.BindBool(
            "deleteLogs",
            // 读取本次退出清理的删除日志选项。
            // 入参：无显式入参；借用 deleteLogs 草稿。
            // 返回：成功的 RendererBoolResult，其 value 为当前删除日志选项。
            [&deleteLogs]() { return RendererBoolResult{true, deleteLogs, {}}; },
            // 更新本次退出清理的删除日志草稿。
            // 入参：value：复选框新值；借用 deleteLogs 存储草稿。
            // 返回：表示变更成功的 RendererChangeResult，不写持久化设置。
            [&deleteLogs](bool value)
            {
                deleteLogs = value;
                return RendererChangeResult{};
            }));
        // 处理用户取消清理窗口的操作。
        // 入参：无显式入参；借用 stopped、exitRequested 和 renderer。
        // 返回：无返回值；请求延迟关闭，业务已经停止时同时记录退出意图。
        const auto cancel = [&]()
        {
            exitRequested = stopped;
            (void)renderer.RequestClose();
        };
        require(renderer.BindAction("cleanupCancel", cancel));
        require(renderer.SetCloseHandler(cancel));
        require(renderer.SetDefaultAction("cleanupCancel"));
        require(renderer.BindAction("cleanupConfirm",
                                    // 执行用户确认的启动项清理、业务停止和可选日志删除。
                                    // 入参：无显式入参；借用清理窗口状态与 renderer，捕获 App。
                                    // 返回：无返回值；失败显示状态并允许重试，成功请求关闭窗口并退出。
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
                                        if (deleteLogs && !ShutdownAndClearLogging())
                                        {
                                            statusKey = "cleanup.logs_failed";
                                            (void)renderer.SetStatus(GetUiText(statusKey));
                                            return;
                                        }
                                        exitRequested = true;
                                        (void)renderer.RequestClose();
                                    }));
        RendererWindowOptions options;
        options.owner = this->DialogOwner();
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

// 显示包含当前构建版本的本地化关于窗口。
// 入参：无。
// 返回：无返回值；复用简单模态消息入口。
void App::ShowAbout()
{
    // 生成包含构建版本号的关于窗口正文。
    // 入参：无。
    // 返回：当前语言的关于说明宽字符串，其中包含 OPEN_ST_VERSION。
    this->ShowSimpleMessage([]() { return GetUiText("about.body", {{L"version", OPEN_ST_WIDEN(OPEN_ST_VERSION)}}); },
                            // 提供关于窗口的本地化标题。
                            // 入参：无。
                            // 返回：about.title 对应的当前语言宽字符串。
                            []() { return GetUiText("about.title"); }, MB_OK | MB_ICONINFORMATION);
}

// 显示截图失败的本地化原因。
// 入参：detailKey：描述失败原因的本地化文本键，仅在本次同步提示中借用。
// 返回：无返回值；将原因嵌入截图错误消息正文后显示。
void App::ShowCaptureError(std::string_view detailKey)
{
    // 生成包含具体原因的截图错误提示正文。
    // 入参：无显式入参；捕获借用的 detailKey，调用期间须保持有效。
    // 返回：本地化截图错误外壳与 detailKey 对应原因组合后的宽字符串。
    this->ShowSimpleMessage([detailKey]()
                            { return GetUiText("capture.error.message", {{L"detail", GetUiText(detailKey)}}); },
                            // 提供应用提示窗口的当前语言标题。
                            // 入参：无。
                            // 返回：app.title 对应的本地化宽字符串。
                            []() { return GetUiText("app.title"); }, MB_OK | MB_ICONERROR);
}

// 显示可随语言变化刷新的简单模态提示，并在 Renderer 失败时回退系统提示。
// 入参：message、title：正文和标题查询回调；fallbackFlags：系统提示标志；owner：可选的受保护所属窗口。
// 返回：无返回值；守卫覆盖自定义和系统提示两条路径，阻止模态期间再次进入业务。
void App::ShowSimpleMessage(std::function<std::wstring()> message, std::function<std::wstring()> title,
                            UINT fallbackFlags, HWND owner)
{
    if (this->dialogActive_)
    {
        return;
    }
    // 在模态业务忙状态变化后同步截图准入门禁。
    // 入参：无显式入参；捕获存活中的 App 指针。
    // 返回：无返回值；调用 UpdateCaptureGate 发布最新状态及代次。
    CompletionBusyGuard dialogGuard(this->dialogActive_, [this]() { this->UpdateCaptureGate(); });
    const HWND effectiveOwner = IsWindow(owner) ? owner : this->DialogOwner();
    bool shown = false;
    try
    {
        shown = TryShowSimpleMessageWindow(
            effectiveOwner, this->largeIcon_, title, message,
            // 提供简单提示窗口确认按钮的当前语言文字。
            // 入参：无。
            // 返回：当前语言的确认按钮文本宽字符串。
            []() { return GetUiText("dialog.ok"); }, this->messageRenderer_,
            // 在简单提示的模态循环中处理设置窗口键盘导航。
            // 入参：threadMessage：可由设置窗口消费的线程消息引用；捕获 App。
            // 返回：设置窗口存在且消费消息时 true，否则 false。
            [this](MSG& threadMessage)
            { return this->settingsWindow_ != nullptr && this->settingsWindow_->ProcessDialogMessage(threadMessage); });
    }
    catch (...)
    {
        OPEN_ST_LOG_ERROR("Failed to prepare the application message window.");
    }
    if (!shown)
    {
        (void)MessageBoxW(effectiveOwner, message().c_str(), title().c_str(), fallbackFlags);
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
