// 真实 App 命令、输入和退出编排；所有任务、截图、启动项和剪贴板均隔离。
#include "capture_overlay_session.h"
#include "hotkeys_test_access.h"
#include "json_file_test_access.h"
#include "resource.h"
#include "settings_internal.h"
#include "simple_message_window.h"
#include "text_session.h"
#include "ui_text_internal.h"
#include <app.h>
#include <clipboard_writer.h>
#include <desktop_capturer.h>
#include <frozen_desktop_frame.h>
#include <fstream>
#include <functional>
#include <gtest/gtest.h>
#include <sdr_selection_frame.h>
#include <settings.h>
#include <settings_window.h>
#include <startup_registration.h>
#include <text_task_fake.h>
#include <ui_text.h>
#include <window_renderer.h>

namespace open_st
{
namespace
{
std::function<void()> duringCapture;
std::function<void()> modalTick;
unsigned captureCalls{};
unsigned noticeCalls{};
} // namespace
// 观察正式捕获调用时的可见性，并固定失败，不读取真实桌面。
// 入参：frame/error 为正式输出。返回：固定失败。
bool DesktopCapturer::Capture(FrozenDesktopFrame& frame, std::wstring& error) const
{
    ++captureCalls;
    if (duringCapture)
        duringCapture();
    frame.Clear();
    error = L"Injected capture failure";
    return false;
}
// 阻止测试触及用户剪贴板。入参：正式输出参数。返回：固定失败。
bool CopyTextToClipboard(HWND, std::wstring_view, std::wstring& error)
{
    ADD_FAILURE() << "App integration must not write the user clipboard";
    error = L"Unexpected clipboard request";
    return false;
}
// 图像与文本导出在同一静态库对象，均提供隔离边界。入参：正式输出。返回：固定失败。
bool CopyImageToClipboard(HWND, const SdrImageView&, std::wstring&)
{
    ADD_FAILURE() << "Unexpected image clipboard request";
    return false;
}
// 保留 App 模态编排而不等待用户确认错误。入参：正式提示参数。返回：固定成功。
bool TryShowSimpleMessageWindow(HWND, HICON, const std::function<std::wstring()>&, const std::function<std::wstring()>&,
                                const std::function<std::wstring()>&, WindowRenderer*&,
                                const std::function<bool(MSG&)>&) noexcept
{
    ++noticeCalls;
    return true;
}
// 启动项替身完全不访问注册表。入参：正式配置。返回：仅建立空状态。
StartupRegistration::StartupRegistration(std::wstring, std::wstring, std::wstring) {}
// 查询替身未启用状态。入参：无。返回：缺失。
StartupStatus StartupRegistration::Query() const
{
    return {StartupState::Missing, ERROR_SUCCESS};
}
// 模拟清理启动项成功。入参：目标状态。返回：目标状态。
StartupStatus StartupRegistration::Apply(bool enabled, bool) const
{
    return {enabled ? StartupState::CurrentPath : StartupState::Missing, ERROR_SUCCESS};
}

struct AppTextTestAccess final
{
    // 建立正式消息宿主，不启动托盘、后台或注册系统快捷键。
    // 入参：app 为测试实例。返回：消息窗口创建结果。
    static bool Initialize(App& app)
    {
        app.startup_ = std::make_unique<StartupRegistration>(L"unused");
        if (!app.CreateMessageWindow())
            return false;
        app.UpdateCaptureGate();
        return true;
    }
    // 借用消息句柄，命令仍由真实 WindowProc 分派。入参：app。返回：句柄。
    static HWND Owner(const App& app)
    {
        return app.messageWindow_;
    }
    // 在同一宿主创建截图来源会话；只用正式入口初始化输入。
    // 入参：app。返回：借用会话。
    static TextSession& CaptureSession(App& app)
    {
        return app.EnsureTextSession(app.textSession_, CaptureTextPollTimer);
    }
    // 观察两个真实实例的所有权。入参：app。返回：借用会话。
    static TextSession* Manual(const App& app)
    {
        return app.translationWindowSession_.get();
    }
    // 取得设置窗口真实适配。入参：app。返回：产品回调。
    static SettingsWindowCallbacks Callbacks(App& app)
    {
        return app.MakeSettingsCallbacks();
    }
    // 切换准入事实并发布正式门禁。入参：app/index/value。返回：无。
    static void Gate(App& app, int index, bool value)
    {
        switch (index)
        {
        case 0:
            app.dialogActive_ = value;
            break;
        case 1:
            app.settingsBusy_ = value;
            break;
        case 2:
            app.hotkeyRecording_ = value;
            break;
        case 3:
            app.welcoming_ = value;
            break;
        case 4:
            app.completionBusy_ = value;
            break;
        case 5:
            app.overlayPreparing_ = value;
            break;
        case 6:
            app.overlayRendering_ = value;
            break;
        case 7:
            app.annotationPreparing_ = value;
            break;
        case 8:
            app.shuttingDown_ = value;
            break;
        }
        app.UpdateCaptureGate();
    }
    // 构造无桌面 HWND 的截图会话以验证结束只关闭截图来源。
    // 入参：app/open。返回：无。
    static void Overlay(App& app, bool open)
    {
        if (open)
            app.overlaySession_ = std::make_unique<CaptureOverlaySession>();
        else
            app.CloseOverlay();
    }
    // 查询退出事实及后台屏障。入参：app。返回：组合条件。
    static bool Stopping(const App& app)
    {
        return app.shuttingDown_;
    }
    // 查询两个会话及共享翻译客户端的实际释放屏障。
    // 入参：app为隔离宿主。返回：全部后台已完成时为true。
    static bool Complete(const App& app)
    {
        return app.TextSessionsShutdownComplete() && app.TranslationShutdownComplete();
    }
    // 借用正式清理模态窗口，让定时驱动不依赖本地化标题或系统 owner 调整。
    // 入参：app。返回：当前模态 HWND 或空。
    static HWND Modal(const App& app)
    {
        return app.messageRenderer_ ? app.messageRenderer_->NativeHandle() : nullptr;
    }
    // 使用主循环相同入口补收消息。入参：app/message。返回：是否消费。
    static bool Process(App& app, MSG& message)
    {
        return app.ProcessApplicationMessage(message);
    }
    // 给真实快捷键管理器注入无系统注册后端。入参：app/backend。返回：发布结果。
    static bool Hotkey(App& app, HotkeyBackend& backend)
    {
        app.hotkeys_ = HotkeyManagerTestAccess::Create(app.messageWindow_, backend);
        if (!app.hotkeys_->Prepare({MOD_CONTROL, 'A'}) || !app.hotkeys_->Finish(true))
            return false;
        app.hotkeyBoundary_ = 1;
        return true;
    }
    // 经产品身份及时效校验分派编辑键。入参：app/id/time。返回：无。
    static void Dispatch(App& app, int id, DWORD time)
    {
        app.DispatchHotkey(static_cast<WPARAM>(id), MAKELPARAM(MOD_CONTROL, 'A'), time);
    }
};

namespace
{
// 按线程、owner 与标题查询本测试窗口，不接触其他应用。
// 入参：owner/title。返回：匹配窗口。
HWND FindTextWindow(HWND owner, std::wstring title, HWND excluded = nullptr)
{
    struct Query
    {
        HWND owner{};
        std::wstring title;
        HWND found{};
        HWND excluded{};
    } query{owner, std::move(title), nullptr, excluded};
    // 枚举本线程窗口并匹配完整所有者及标题。
    // 入参：window为候选，value为查询。返回：找到匹配后停止。
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND window, LPARAM value) -> BOOL
        {
            Query& current = *reinterpret_cast<Query*>(value);
            std::wstring text(static_cast<std::size_t>(GetWindowTextLengthW(window)) + 1, L'\0');
            text.resize(static_cast<std::size_t>(GetWindowTextW(window, text.data(), static_cast<int>(text.size()))));
            if (window != current.excluded && GetWindow(window, GW_OWNER) == current.owner && text == current.title)
            {
                current.found = window;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&query));
    return query.found;
}
// 定位真实原文编辑控件。入参：window。返回：借用 HWND。
HWND SourceEdit(HWND window)
{
    return FindWindowExW(FindWindowExW(window, nullptr, L"OpenST.WindowRendererPage", nullptr), nullptr, L"EDIT",
                         nullptr);
}
// 读取完整控件正文。入参：window。返回：UTF-16。
std::wstring ReadText(HWND window)
{
    std::wstring text(static_cast<std::size_t>(GetWindowTextLengthW(window)) + 1, L'\0');
    text.resize(static_cast<std::size_t>(GetWindowTextW(window, text.data(), static_cast<int>(text.size()))));
    return text;
}
// 临时借用真实前台输入队列完成激活，调用产品前立即解除连接。
// 入参：window为本测试自有窗口。返回：激活且解除输入队列连接成功时为true。
bool ActivateTextTestWindow(HWND window)
{
    (void)SetForegroundWindow(window);
    if (GetForegroundWindow() == window)
        return true;
    const DWORD foregroundThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    const DWORD testThread = GetCurrentThreadId();
    if (foregroundThread == 0 || foregroundThread == testThread ||
        !AttachThreadInput(testThread, foregroundThread, TRUE))
        return false;
    (void)SetForegroundWindow(window);
    const bool activated = GetForegroundWindow() == window;
    const bool detached = AttachThreadInput(testThread, foregroundThread, FALSE) != FALSE;
    return activated && detached;
}
// 检查真实子控件中是否存在完整本地化提示，正文也由独立断言核对。
// 入参：window为根窗口，text为期望提示。返回：存在完整匹配时为true。
bool HasChildText(HWND window, const std::wstring& text)
{
    struct Query
    {
        const std::wstring& text;
        bool found{};
    } query{text};
    // 读取每个子控件的实际文字，不依赖Renderer生成的控件编号。
    // 入参：child为候选，value为查询。返回：找到完整匹配后停止。
    EnumChildWindows(
        window,
        [](HWND child, LPARAM value) -> BOOL
        {
            Query& current = *reinterpret_cast<Query*>(value);
            current.found = ReadText(child) == current.text;
            return !current.found;
        },
        reinterpret_cast<LPARAM>(&query));
    return query.found;
}
// 记录可控快捷键身份，不注册系统热键。入参：正式参数。返回：固定成功。
class TextHotkeyBackend final : public HotkeyBackend
{
  public:
    int id{};
    // 记录正式管理器分配的身份，不调用RegisterHotKey。
    // 入参：value为注册ID，其他参数为正式注册上下文，error为错误输出。返回：固定成功。
    bool Register(HWND, int value, UINT, UINT, DWORD& error) noexcept override
    {
        this->id = value;
        error = ERROR_SUCCESS;
        return true;
    }
    // 模拟释放测试身份，不操作任何系统注册。
    // 入参：窗口及ID为待释放身份，error为错误输出。返回：固定成功。
    bool Unregister(HWND, int, DWORD& error) noexcept override
    {
        error = ERROR_SUCCESS;
        return true;
    }
};

class AppTextIntegrationTest : public testing::Test
{
  protected:
    test_text::TextTaskState state_;
    TextHotkeyBackend hotkeys_;
    std::filesystem::path root_;
    std::unique_ptr<App> app_;
    UINT_PTR modalTimer_{};
    // 正式本地化与设置绑定测试目录，避免读取或覆盖用户配置。
    // 入参：无。返回：无。
    void SetUp() override
    {
        test_text::state = &this->state_;
        captureCalls = 0;
        noticeCalls = 0;
        ShutdownSettings();
        ShutdownUiText();
        ASSERT_TRUE(JsonFileTestAccess::ReleaseFile("settings.user"));
        ASSERT_TRUE(JsonFileTestAccess::ReleaseFile("settings.default"));
        ASSERT_TRUE(JsonFileTestAccess::ReleaseFile("localization.ui_text"));
        this->root_ = std::filesystem::temp_directory_path() /
                      ("Open-ST-app-text-" + std::to_string(GetCurrentProcessId()) + "-" +
                       testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::create_directories(this->root_ / "resources");
        std::filesystem::create_directories(this->root_ / "data");
        const std::filesystem::path source(OPEN_ST_TEST_SOURCE_ROOT);
        std::filesystem::copy_file(source / "resources/default_settings.json",
                                   this->root_ / "resources/default_settings.json",
                                   std::filesystem::copy_options::overwrite_existing);
        std::filesystem::copy_file(source / "resources/ui_text.json", this->root_ / "resources/ui_text.json",
                                   std::filesystem::copy_options::overwrite_existing);
        ASSERT_TRUE(InitializeSettings(this->root_));
        ASSERT_TRUE(InitializeUiText(this->root_));
        ASSERT_TRUE(SetUiLanguage("en-US"));
        this->app_ = std::make_unique<App>(GetModuleHandleW(nullptr));
        ASSERT_TRUE(AppTextTestAccess::Initialize(*this->app_));
    }
    // 回收自有窗口、计时器与临时配置，停止事实由 fake 明确给出。
    // 入参：无。返回：无。
    void TearDown() override
    {
        if (this->modalTimer_)
            KillTimer(AppTextTestAccess::Owner(*this->app_), this->modalTimer_);
        duringCapture = {};
        modalTick = {};
        this->state_.translationStopped = true;
        this->state_.stopped = true;
        this->app_.reset();
        test_text::state = nullptr;
        ShutdownSettings();
        ShutdownUiText();
        EXPECT_TRUE(JsonFileTestAccess::ReleaseFile("settings.user"));
        EXPECT_TRUE(JsonFileTestAccess::ReleaseFile("settings.default"));
        EXPECT_TRUE(JsonFileTestAccess::ReleaseFile("localization.ui_text"));
        std::error_code error;
        std::filesystem::remove_all(this->root_, error);
        EXPECT_FALSE(error);
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
        }
    }
    // 稳定资源 ID 经真实 App 消息过程打开窗口。入参：无。返回：借用 HWND。
    HWND Open()
    {
        SendMessageW(AppTextTestAccess::Owner(*this->app_), WM_COMMAND, ID_TRAY_TRANSLATION, 0);
        return FindTextWindow(AppTextTestAccess::Owner(*this->app_), GetUiText("translation.manual.title"));
    }
    // 通过真实按钮开始任务。入参：window/text。返回：无。
    void Translate(HWND window, const wchar_t* text)
    {
        SendMessageW(SourceEdit(window), EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(text));
        const std::wstring action = GetUiText("translation.action");
        const HWND button = FindWindowExW(window, nullptr, L"BUTTON", action.c_str());
        ASSERT_NE(button, nullptr);
        ASSERT_TRUE(IsWindowEnabled(button));
        SendMessageW(button, BM_CLICK, 0, 0);
    }
    // 发布带完整资格戳的成功快照，由真实App计时器入口接收。
    // 入参：network为来源在线事实。返回：无。
    void PublishSuccess(bool network)
    {
        std::shared_ptr<TranslationResult> result = std::make_shared<TranslationResult>();
        result->text = "translated";
        result->profileName = "fixture provider";
        result->network = network;
        result->requestId = this->state_.translation.requestId;
        result->sessionId = this->state_.request.sessionId;
        result->sourceRevision = this->state_.request.sourceRevision;
        result->configurationRevision = this->state_.request.configurationRevision;
        result->options = this->state_.request.options;
        this->state_.translation.result = std::move(result);
        this->state_.translation.busy = false;
        this->state_.translation.phase = TranslationPhase::Succeeded;
        ++this->state_.translation.revision;
        SendMessageW(AppTextTestAccess::Owner(*this->app_), WM_TIMER, StandaloneTextPollTimer, 0);
    }
    // 有界消费应用消息，不创建另一套产品消息处理。入参：无。返回：无。
    void Pump()
    {
        MSG message{};
        for (int index = 0; index < 256 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++index)
            if (message.message != WM_QUIT)
                AppTextTestAccess::Process(*this->app_, message);
    }
};

// 命令稳定且手动入口零 OCR；重复激活保留原生撤销。
// 入参：无。返回：断言结果。
TEST_F(AppTextIntegrationTest, tray_command_opens_manual_draft_and_repeated_activation_preserves_undo)
{
    const HWND window = this->Open();
    ASSERT_NE(window, nullptr);
    EXPECT_EQ(this->state_.creates, 0);
    EXPECT_EQ(this->state_.submissions, 0);
    SendMessageW(SourceEdit(window), EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"draft"));
    EXPECT_EQ(this->Open(), window);
    EXPECT_EQ(ReadText(SourceEdit(window)), L"draft");
    SendMessageW(SourceEdit(window), WM_UNDO, 0, 0);
    EXPECT_EQ(ReadText(SourceEdit(window)), L"");
    EXPECT_EQ(this->state_.translationSubmissions, 0U);
    EXPECT_EQ(this->state_.translationCreates, 1U);
}

// 所有业务忙门禁及截图会话都禁止从托盘新建窗口。
// 入参：无。返回：断言结果。
TEST_F(AppTextIntegrationTest, tray_command_respects_modal_capture_and_shutdown_admission)
{
    for (int index = 0; index < 9; ++index)
    {
        AppTextTestAccess::Gate(*this->app_, index, true);
        EXPECT_EQ(this->Open(), nullptr) << index;
        EXPECT_EQ(AppTextTestAccess::Manual(*this->app_), nullptr) << index;
        AppTextTestAccess::Gate(*this->app_, index, false);
    }
    AppTextTestAccess::Overlay(*this->app_, true);
    EXPECT_EQ(this->Open(), nullptr);
    AppTextTestAccess::Overlay(*this->app_, false);
    EXPECT_NE(this->Open(), nullptr);
}

// OCR 来源真实创建 OCR，独立窗与截图窗共存，截图结束保留独立草稿。
// 入参：无。返回：断言结果。
TEST_F(AppTextIntegrationTest, capture_session_is_independent_and_overlay_close_keeps_manual_draft)
{
    const HWND window = this->Open();
    ASSERT_NE(window, nullptr);
    this->Translate(window, L"manual draft");
    TextSession& capture = AppTextTestAccess::CaptureSession(*this->app_);
    SdrSelectionFrame frame{{0, 0, 2, 2}, std::vector<std::uint8_t>(16, 255)};
    capture.BeginOcr(frame, "best", "eng");
    EXPECT_EQ(this->state_.creates, 1);
    EXPECT_EQ(this->state_.submissions, 1);
    EXPECT_EQ(this->state_.translationCreates, 1U);
    AppTextTestAccess::Overlay(*this->app_, true);
    AppTextTestAccess::Overlay(*this->app_, false);
    this->Pump();
    EXPECT_TRUE(IsWindow(window));
    EXPECT_EQ(ReadText(SourceEdit(window)), L"manual draft");
    EXPECT_TRUE(this->state_.translation.busy);
    EXPECT_EQ(this->state_.translationCancellations, 0U);
}

// 冻结捕获前暂隐，失败清理后恢复，草稿与后台请求保持。
// 入参：无。返回：断言结果。
TEST_F(AppTextIntegrationTest, capture_hides_manual_window_before_freeze_and_failure_restores_it)
{
    const HWND window = this->Open();
    ASSERT_NE(window, nullptr);
    this->Translate(window, L"retained");
    ASSERT_TRUE(IsWindowVisible(window));
    duringCapture = [window]() { EXPECT_FALSE(IsWindowVisible(window)); };
    SendMessageW(AppTextTestAccess::Owner(*this->app_), WM_COMMAND, ID_TRAY_CAPTURE, 0);
    EXPECT_EQ(captureCalls, 1U);
    EXPECT_EQ(noticeCalls, 1U);
    EXPECT_TRUE(IsWindowVisible(window));
    EXPECT_EQ(ReadText(SourceEdit(window)), L"retained");
    EXPECT_TRUE(this->state_.translation.busy);
    EXPECT_EQ(this->state_.translationCancellations, 0U);
}

// 设置应用通知两个会话；切换界面语言不撤销正在运行的请求。
// 入参：无。返回：断言结果。
TEST_F(AppTextIntegrationTest, settings_and_localization_callbacks_reach_both_sessions_without_resubmission)
{
    const HWND manual = this->Open();
    ASSERT_NE(manual, nullptr);
    TextSession& capture = AppTextTestAccess::CaptureSession(*this->app_);
    capture.BeginText();
    const HWND other =
        FindTextWindow(AppTextTestAccess::Owner(*this->app_), GetUiText("translation.manual.title"), manual);
    ASSERT_NE(other, nullptr);
    this->Translate(manual, L"manual");
    SettingsWindowCallbacks callbacks = AppTextTestAccess::Callbacks(*this->app_);
    const HWND viewport = FindWindowExW(manual, nullptr, L"OpenST.WindowRendererPage", nullptr);
    const HWND languages = FindWindowExW(viewport, nullptr, L"COMBOBOX", nullptr);
    ASSERT_NE(languages, nullptr);
    const LRESULT selected = SendMessageW(languages, CB_GETCURSEL, 0, 0);
    wchar_t originalLabel[128]{};
    ASSERT_NE(
        SendMessageW(languages, CB_GETLBTEXT, static_cast<WPARAM>(selected), reinterpret_cast<LPARAM>(originalLabel)),
        CB_ERR);
    const unsigned before = this->state_.translationCancellations;
    ASSERT_TRUE(callbacks.languageApplied("ja-JP"));
    wchar_t localizedLabel[128]{};
    ASSERT_NE(
        SendMessageW(languages, CB_GETLBTEXT, static_cast<WPARAM>(selected), reinterpret_cast<LPARAM>(localizedLabel)),
        CB_ERR);
    EXPECT_NE(std::wstring(originalLabel), std::wstring(localizedLabel));
    EXPECT_EQ(SendMessageW(languages, CB_GETCURSEL, 0, 0), selected);
    EXPECT_EQ(this->state_.translationCancellations, before);
    EXPECT_EQ(ReadText(SourceEdit(manual)), L"manual");
    callbacks.translationSettingsApplied();
    EXPECT_GT(this->state_.translationCancellations, before);
    EXPECT_EQ(this->state_.translationSubmissions, 1U);
    this->state_.translation.busy = false;
    capture.Poll();
    AppTextTestAccess::Manual(*this->app_)->Poll();
    this->Translate(other, L"capture");
    const unsigned next = this->state_.translationCancellations;
    callbacks.translationSettingsApplied();
    EXPECT_GT(this->state_.translationCancellations, next);
    EXPECT_EQ(this->state_.translationSubmissions, 2U);
}

// 注册身份及时间核验后编辑键交给前台手动窗，禁用和 IME 时保留原选区。
// 入参：无。返回：断言结果。
TEST_F(AppTextIntegrationTest, registered_edit_hotkey_targets_manual_window_and_respects_focus_ime_pause)
{
    const HWND window = this->Open();
    ASSERT_NE(window, nullptr);
    const HWND edit = SourceEdit(window);
    SendMessageW(edit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"draft"));
    ASSERT_TRUE(AppTextTestAccess::Hotkey(*this->app_, this->hotkeys_));
    ASSERT_TRUE(ActivateTextTestWindow(window));
    SetFocus(edit);
    ASSERT_EQ(GetForegroundWindow(), window);
    ASSERT_EQ(GetFocus(), edit);
    SendMessageW(edit, EM_SETSEL, 1, 1);
    AppTextTestAccess::Dispatch(*this->app_, this->hotkeys_.id, 2);
    DWORD first{}, last{};
    SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&first), reinterpret_cast<LPARAM>(&last));
    EXPECT_EQ(first, 0U);
    EXPECT_EQ(last, 5U);
    SendMessageW(edit, EM_SETSEL, 2, 2);
    SendMessageW(edit, WM_IME_STARTCOMPOSITION, 0, 0);
    AppTextTestAccess::Dispatch(*this->app_, this->hotkeys_.id, 2);
    SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&first), reinterpret_cast<LPARAM>(&last));
    EXPECT_EQ(first, 2U);
    EXPECT_EQ(last, 2U);
    SendMessageW(edit, WM_IME_ENDCOMPOSITION, 0, 0);
    AppTextTestAccess::Gate(*this->app_, 1, true);
    EXPECT_FALSE(IsWindowEnabled(window));
    AppTextTestAccess::Dispatch(*this->app_, this->hotkeys_.id, 3);
    EXPECT_EQ(ReadText(edit), L"draft");
    EXPECT_EQ(captureCalls, 0U);
}

// 普通退出同时撤销两实例，真实后台屏障完成前不得报告释放。
// 入参：无。返回：断言结果。
TEST_F(AppTextIntegrationTest, ordinary_exit_waits_for_both_session_and_shared_translation_release)
{
    const HWND manual = this->Open();
    ASSERT_NE(manual, nullptr);
    this->Translate(manual, L"pending");
    TextSession& capture = AppTextTestAccess::CaptureSession(*this->app_);
    SdrSelectionFrame frame{{0, 0, 2, 2}, std::vector<std::uint8_t>(16, 255)};
    capture.BeginOcr(frame, "best", "eng");
    SendMessageW(AppTextTestAccess::Owner(*this->app_), WM_COMMAND, ID_TRAY_EXIT, 0);
    this->Pump();
    EXPECT_TRUE(AppTextTestAccess::Stopping(*this->app_));
    EXPECT_FALSE(AppTextTestAccess::Complete(*this->app_));
    EXPECT_FALSE(IsWindow(manual));
    EXPECT_TRUE(this->state_.stopping);
    EXPECT_GT(this->state_.translationShutdownRequests, 0U);
    this->state_.stopped = true;
    EXPECT_FALSE(AppTextTestAccess::Complete(*this->app_));
    this->state_.translationStopped = true;
    EXPECT_TRUE(AppTextTestAccess::Complete(*this->app_));
}

// 正式清理按钮停止两个后台；模态循环继续等待共享任务释放后退出。
// 入参：无。返回：断言结果。
TEST_F(AppTextIntegrationTest, cleanup_exit_waits_for_text_and_translation_then_finishes)
{
    const HWND manual = this->Open();
    ASSERT_NE(manual, nullptr);
    this->Translate(manual, L"pending");
    TextSession& capture = AppTextTestAccess::CaptureSession(*this->app_);
    SdrSelectionFrame frame{{0, 0, 2, 2}, std::vector<std::uint8_t>(16, 255)};
    capture.BeginOcr(frame, "best", "eng");
    unsigned ticks{};
    bool confirmed{};
    bool waited{};
    modalTick = [this, &ticks, &confirmed, &waited]()
    {
        ++ticks;
        const HWND dialog = AppTextTestAccess::Modal(*this->app_);
        if (ticks > 500)
        {
            ADD_FAILURE() << "Cleanup modal did not finish: ticks=" << ticks << " dialog=" << dialog
                          << " confirmed=" << confirmed << " waited=" << waited << " stopping=" << this->state_.stopping
                          << " shutdownRequests=" << this->state_.translationShutdownRequests;
            if (dialog)
                SendMessageW(dialog, WM_CLOSE, 0, 0);
            else
                PostQuitMessage(1);
            return;
        }
        if (!dialog)
            return;
        if (!confirmed)
        {
            const std::wstring label = GetUiText("cleanup.confirm");
            const HWND button = FindWindowExW(dialog, nullptr, L"BUTTON", label.c_str());
            if (!button)
                return;
            confirmed = true;
            SendMessageW(button, BM_CLICK, 0, 0);
            EXPECT_TRUE(AppTextTestAccess::Stopping(*this->app_));
            EXPECT_FALSE(AppTextTestAccess::Complete(*this->app_));
            return;
        }
        waited = true;
        EXPECT_TRUE(this->state_.stopping);
        EXPECT_GT(this->state_.translationShutdownRequests, 0U);
        this->state_.stopped = true;
        this->state_.translationStopped = true;
    };
    // 由正式宿主HWND关联计时器推进真实模态泵；ShowModal不分派hwnd为空的线程计时消息。
    // 入参：窗口为稳定消息宿主，197为本测试独立编号；回调参数无业务含义。返回：非零计时器编号。
    this->modalTimer_ = SetTimer(AppTextTestAccess::Owner(*this->app_), 197, 10,
                                 [](HWND, UINT, UINT_PTR, DWORD)
                                 {
                                     if (modalTick)
                                         modalTick();
                                 });
    ASSERT_NE(this->modalTimer_, 0U);
    SendMessageW(AppTextTestAccess::Owner(*this->app_), WM_COMMAND, ID_TRAY_CLEANUP, 0);
    EXPECT_GT(ticks, 0U);
    EXPECT_TRUE(confirmed);
    EXPECT_TRUE(waited);
    EXPECT_TRUE(AppTextTestAccess::Stopping(*this->app_));
    EXPECT_TRUE(AppTextTestAccess::Complete(*this->app_));
    EXPECT_FALSE(IsWindow(manual));
}
// 同一计时消息实际补收两实例；没有通知也能分别发布 OCR 与翻译。
// 入参：无。返回：断言结果。
TEST_F(AppTextIntegrationTest, one_session_timer_recovers_results_for_both_instances_without_wake_notification)
{
    const HWND manual = this->Open();
    ASSERT_NE(manual, nullptr);
    TextSession& capture = AppTextTestAccess::CaptureSession(*this->app_);
    SdrSelectionFrame frame{{0, 0, 2, 2}, std::vector<std::uint8_t>(16, 255)};
    capture.BeginOcr(frame, "best", "eng");
    const HWND ocr = FindTextWindow(AppTextTestAccess::Owner(*this->app_), GetUiText("translation.window.title"));
    ASSERT_NE(ocr, nullptr);
    this->Translate(manual, L"manual");
    this->state_.snapshot.phase = OcrPhase::Succeeded;
    this->state_.snapshot.busy = false;
    this->state_.snapshot.text = std::make_shared<const std::wstring>(L"recognized");
    ++this->state_.snapshot.revision;
    std::shared_ptr<TranslationResult> result = std::make_shared<TranslationResult>();
    result->text = "translated";
    result->profileName = "fixture";
    result->requestId = this->state_.translation.requestId;
    result->sessionId = this->state_.request.sessionId;
    result->sourceRevision = this->state_.request.sourceRevision;
    result->configurationRevision = this->state_.request.configurationRevision;
    result->options = this->state_.request.options;
    this->state_.translation.result = std::move(result);
    this->state_.translation.busy = false;
    this->state_.translation.phase = TranslationPhase::Succeeded;
    ++this->state_.translation.revision;
    SendMessageW(AppTextTestAccess::Owner(*this->app_), WM_TIMER, StandaloneTextPollTimer, 0);
    EXPECT_EQ(ReadText(SourceEdit(ocr)), L"recognized");
    const HWND viewport = GetParent(SourceEdit(manual));
    const HWND translated = FindWindowExW(viewport, SourceEdit(manual), L"EDIT", nullptr);
    EXPECT_EQ(ReadText(translated), L"translated");
    EXPECT_EQ(ReadText(SourceEdit(manual)), L"manual");
}

// 捕获同步调用中的重入命令也受准备门禁保护，关闭的窗口不会恢复。
// 入参：无。返回：断言结果。
TEST_F(AppTextIntegrationTest, capture_preparation_rejects_reentrant_open_after_manual_close)
{
    const HWND manual = this->Open();
    ASSERT_NE(manual, nullptr);
    duringCapture = [this, manual]()
    {
        EXPECT_FALSE(IsWindowVisible(manual));
        SendMessageW(manual, WM_CLOSE, 0, 0);
        SendMessageW(AppTextTestAccess::Owner(*this->app_), WM_COMMAND, ID_TRAY_TRANSLATION, 0);
        EXPECT_FALSE(AppTextTestAccess::Manual(*this->app_)->ActivateExisting());
    };
    SendMessageW(AppTextTestAccess::Owner(*this->app_), WM_COMMAND, ID_TRAY_CAPTURE, 0);
    this->Pump();
    EXPECT_EQ(captureCalls, 1U);
    EXPECT_FALSE(IsWindow(manual));
    EXPECT_EQ(FindTextWindow(AppTextTestAccess::Owner(*this->app_), GetUiText("translation.manual.title")), nullptr);
}
// 完成与取消共用的实际清理边界恢复旧窗，模态前台保持，退出不恢复。
// 入参：无。返回：断言结果。
TEST_F(AppTextIntegrationTest, overlay_cleanup_restores_manual_window_without_stealing_modal_focus_or_reviving_on_exit)
{
    const HWND manual = this->Open();
    ASSERT_NE(manual, nullptr);
    SendMessageW(SourceEdit(manual), EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"retained draft"));
    struct ModalWindow final
    {
        HWND window{};
        // 回收本用例创建的模态前台替身。入参：无。返回：无。
        ~ModalWindow()
        {
            if (this->window)
                DestroyWindow(this->window);
        }
    } modal;
    modal.window = CreateWindowExW(0, L"STATIC", L"App text modal owner", WS_POPUP | WS_VISIBLE, 0, 0, 80, 50,
                                   AppTextTestAccess::Owner(*this->app_), nullptr, GetModuleHandleW(nullptr), nullptr);
    ASSERT_NE(modal.window, nullptr);
    for (int boundary = 0; boundary < 2; ++boundary)
    {
        AppTextTestAccess::Overlay(*this->app_, true);
        AppTextTestAccess::Manual(*this->app_)->SetCaptureSuspended(true);
        ASSERT_FALSE(IsWindowVisible(manual));
        AppTextTestAccess::Gate(*this->app_, 0, true);
        ASSERT_TRUE(ActivateTextTestWindow(modal.window));
        SetFocus(modal.window);
        ASSERT_EQ(GetFocus(), modal.window);
        AppTextTestAccess::Overlay(*this->app_, false);
        EXPECT_TRUE(IsWindowVisible(manual)) << boundary;
        EXPECT_FALSE(IsWindowEnabled(manual));
        EXPECT_EQ(GetForegroundWindow(), modal.window);
        EXPECT_EQ(GetFocus(), modal.window);
        EXPECT_EQ(ReadText(SourceEdit(manual)), L"retained draft");
        AppTextTestAccess::Gate(*this->app_, 0, false);
    }
    AppTextTestAccess::Overlay(*this->app_, true);
    AppTextTestAccess::Manual(*this->app_)->SetCaptureSuspended(true);
    AppTextTestAccess::Gate(*this->app_, 8, true);
    AppTextTestAccess::Overlay(*this->app_, false);
    EXPECT_FALSE(IsWindowVisible(manual));
    EXPECT_EQ(GetForegroundWindow(), modal.window);
    EXPECT_EQ(ReadText(SourceEdit(manual)), L"retained draft");
}

// 来源只保存接口名称与在线事实，切换语言时本地/在线及完成状态重新解析。
// 入参：无。返回：断言结果。
TEST_F(AppTextIntegrationTest, successful_local_and_online_provenance_relocalizes_without_replacing_text)
{
    const HWND manual = this->Open();
    ASSERT_NE(manual, nullptr);
    this->Translate(manual, L"source draft");
    SettingsWindowCallbacks callbacks = AppTextTestAccess::Callbacks(*this->app_);
    for (const bool network : {false, true})
    {
        this->PublishSuccess(network);
        const std::uint64_t request = this->state_.translation.requestId;
        const std::uint64_t sourceRevision = this->state_.request.sourceRevision;
        const unsigned cancellations = this->state_.translationCancellations;
        const HWND translated = FindWindowExW(GetParent(SourceEdit(manual)), SourceEdit(manual), L"EDIT", nullptr);
        const std::wstring name =
            L"fixture provider · " + GetUiText(network ? "translation.online" : "translation.local");
        const std::wstring before = GetUiText("translation.origin", {{L"name", name}});
        EXPECT_TRUE(HasChildText(manual, before));
        const std::string target = CurrentUiLanguageCode() == "en-US" ? "ja-JP" : "en-US";
        ASSERT_TRUE(callbacks.languageApplied(target));
        const std::wstring localizedName =
            L"fixture provider · " + GetUiText(network ? "translation.online" : "translation.local");
        const std::wstring after = GetUiText("translation.origin", {{L"name", localizedName}});
        EXPECT_NE(before, after);
        EXPECT_TRUE(HasChildText(manual, after));
        EXPECT_TRUE(HasChildText(manual, GetUiText("translation.completed")));
        EXPECT_EQ(ReadText(SourceEdit(manual)), L"source draft");
        EXPECT_EQ(ReadText(translated), L"translated");
        EXPECT_EQ(this->state_.translation.requestId, request);
        EXPECT_EQ(this->state_.request.sourceRevision, sourceRevision);
        EXPECT_EQ(this->state_.translationCancellations, cancellations);
        if (!network)
        {
            const std::wstring action = GetUiText("translation.action");
            const HWND button = FindWindowExW(manual, nullptr, L"BUTTON", action.c_str());
            ASSERT_NE(button, nullptr);
            SendMessageW(button, BM_CLICK, 0, 0);
        }
    }
    EXPECT_EQ(this->state_.translationSubmissions, 2U);
}

// 运行与失败提示保留安全参数，界面语言刷新不更改请求资格或草稿。
// 入参：无。返回：断言结果。
TEST_F(AppTextIntegrationTest, running_and_failed_status_parameters_relocalize_without_cancelling_request)
{
    const HWND manual = this->Open();
    ASSERT_NE(manual, nullptr);
    this->Translate(manual, L"source draft");
    SettingsWindowCallbacks callbacks = AppTextTestAccess::Callbacks(*this->app_);
    const std::uint64_t request = this->state_.translation.requestId;
    const std::uint64_t sourceRevision = this->state_.request.sourceRevision;
    const std::uint64_t configurationRevision = this->state_.request.configurationRevision;
    const unsigned cancellations = this->state_.translationCancellations;
    this->state_.translation.currentProfileName = "safe provider";
    this->state_.translation.currentIndex = 2;
    this->state_.translation.totalInterfaces = 7;
    ++this->state_.translation.revision;
    SendMessageW(AppTextTestAccess::Owner(*this->app_), WM_TIMER, StandaloneTextPollTimer, 0);
    const std::wstring runningBefore =
        GetUiText("translation.attempting", {{L"name", L"safe provider"}, {L"index", L"2"}, {L"total", L"7"}});
    EXPECT_TRUE(HasChildText(manual, runningBefore));
    ASSERT_TRUE(callbacks.languageApplied("ja-JP"));
    const std::wstring runningAfter =
        GetUiText("translation.attempting", {{L"name", L"safe provider"}, {L"index", L"2"}, {L"total", L"7"}});
    EXPECT_NE(runningBefore, runningAfter);
    EXPECT_TRUE(HasChildText(manual, runningAfter));
    EXPECT_TRUE(this->state_.translation.busy);
    TranslationAttempt attempt;
    attempt.profileId = "safe-id";
    attempt.profileName = "safe provider";
    attempt.error = TranslationError::Unavailable;
    this->state_.translation.attempts = {attempt};
    this->state_.translation.unattempted = 3;
    this->state_.translation.error = TranslationError::Timeout;
    this->state_.translation.phase = TranslationPhase::Failed;
    this->state_.translation.busy = false;
    ++this->state_.translation.revision;
    SendMessageW(AppTextTestAccess::Owner(*this->app_), WM_TIMER, StandaloneTextPollTimer, 0);
    const std::wstring failedBefore = GetUiText(TranslationErrorTextKey(TranslationError::Timeout)) +
                                      L"\nsafe provider: " +
                                      GetUiText(TranslationErrorTextKey(TranslationError::Unavailable)) + L"\n" +
                                      GetUiText("translation.unattempted", {{L"count", L"3"}});
    EXPECT_TRUE(HasChildText(manual, failedBefore));
    ASSERT_TRUE(callbacks.languageApplied("en-US"));
    const std::wstring failedAfter = GetUiText(TranslationErrorTextKey(TranslationError::Timeout)) +
                                     L"\nsafe provider: " +
                                     GetUiText(TranslationErrorTextKey(TranslationError::Unavailable)) + L"\n" +
                                     GetUiText("translation.unattempted", {{L"count", L"3"}});
    EXPECT_NE(failedBefore, failedAfter);
    EXPECT_TRUE(HasChildText(manual, failedAfter));
    EXPECT_EQ(ReadText(SourceEdit(manual)), L"source draft");
    EXPECT_EQ(this->state_.translation.requestId, request);
    EXPECT_EQ(this->state_.request.sourceRevision, sourceRevision);
    EXPECT_EQ(this->state_.request.configurationRevision, configurationRevision);
    EXPECT_EQ(this->state_.translationCancellations, cancellations);
    EXPECT_EQ(this->state_.translationSubmissions, 1U);
}
} // namespace
} // namespace open_st
