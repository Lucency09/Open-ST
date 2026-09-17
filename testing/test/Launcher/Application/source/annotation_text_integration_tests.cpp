// 文件职责：验证 App 原位文字与真实 RichEdit 的排队提交和寿命，不捕获桌面或访问用户剪贴板。
#include "annotation_interaction_controller.h"
#include "capture_annotation_state.h"
#include "capture_command_gate.h"
#include "capture_overlay_session.h"
#include "overlay_input_queue.h"
#include <app.h>
#include <array>
#include <commctrl.h>
#include <cwchar>
#include <gtest/gtest.h>
#include <hotkeys_test_access.h>
#include <inline_text_editor.h>
#include <inline_text_editor_test_access.h>
#include <richedit.h>

namespace open_st
{
struct AppAnnotationTextTestAccess final
{
    // 注入无系统注册的后端，并固定消息时间边界，保留真实注册身份校验。
    // 入参：app：应用；backend：活到应用之后的替身；chord：目标组合。
    // 返回：活动注册准备及提交成功为 true。
    static bool RegisterHotkey(App& app, HotkeyBackend& backend, HotkeyChord chord)
    {
        if (!app.hotkeys_)
            app.hotkeys_ = HotkeyManagerTestAccess::Create(app.messageWindow_, backend);
        if (!app.hotkeys_->Prepare(chord) || !app.hotkeys_->Finish(true))
            return false;
        app.hotkeyBoundary_ = 100U;
        return true;
    }

    // 把确定身份、组合和时间交给生产快捷键准入，不注册或合成全局键盘输入。
    // 入参：app：应用；id：通知身份；chord：完整组合；time：原通知时间。
    // 返回：无。
    static void Hotkey(App& app, int id, HotkeyChord chord, DWORD time = 101U)
    {
        app.DispatchHotkey(static_cast<WPARAM>(id),
                           MAKELPARAM(static_cast<WORD>(chord.modifiers), static_cast<WORD>(chord.key)), time);
    }

    // 设置本线程自有 RichEdit 的真实焦点，不要求改变用户当前前台窗口。
    // 入参：app：正在编辑文字的应用。
    // 返回：无。
    static void Focus(App& app)
    {
        SetFocus(Edit(app));
    }

    // 替换只读前台环境查询，准入和编辑动作仍执行公共组件生产逻辑。
    // 入参：app：正在编辑文字的应用；window：受控前台窗口。
    // 返回：无。
    static void Foreground(App& app, HWND window)
    {
        // 返回本例指定的前台环境，不执行系统激活或剪贴板操作。
        // 入参：无。
        // 返回：独立捕获的窗口句柄。
        InlineTextEditorTestAccess::SetForegroundQuery(*app.annotationInteraction_->editor_,
                                                       [window]() { return window; });
    }

    // 建立独立消息窗口及两个隐藏遮罩，只准备选区和标注状态，不建立冻结帧或渲染器。
    // 入参：app：独占应用；windows：接收遮罩句柄。
    // 返回：全部本地窗口和状态创建成功为 true。
    static bool Initialize(App& app, std::array<HWND, 2U>& windows)
    {
        if (app.messageWindow_ == nullptr && !app.CreateMessageWindow())
            return false;
        app.selectionModel_ = std::make_unique<SelectionModel>();
        app.selectionModel_->SetBounds({0, 0, 800, 600});
        if (!app.selectionModel_->SelectRectangle({100, 100, 500, 400}))
            return false;
        app.toolbarGate_ = std::make_unique<CaptureCommandGate>();
        app.annotation_ = std::make_unique<CaptureAnnotationState>();
        app.annotation_->SetTool(CaptureAnnotationTool::Text);
        app.overlaySession_ = std::make_unique<CaptureOverlaySession>();
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = App::OverlayProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = L"OpenST.AnnotationTextIntegrationTest";
        if (RegisterClassW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return false;
        app.overlayPreparing_ = true;
        for (std::size_t index = 0U; index < windows.size(); ++index)
        {
            windows[index] = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, windowClass.lpszClassName, L"",
                                             WS_POPUP, 100 + static_cast<int>(index) * 200, 100, 200, 300, nullptr,
                                             nullptr, windowClass.hInstance, &app);
            if (!windows[index])
            {
                app.overlayPreparing_ = false;
                return false;
            }
            app.overlaySession_->Add(windows[index]);
        }
        app.overlayPreparing_ = false;
        return !app.overlayInvalidated_;
    }

    // 经真实 App 入口开始新建或原位重编辑，不替换公共输入组件。
    // 入参：app：应用；point：新建落点；id：可选已有文字目标。
    // 返回：编辑组件和文字事务均已建立为 true。
    static bool Begin(App& app, PointI point = {120, 120}, std::uint64_t id = 0U)
    {
        app.BeginAnnotationText(point, id);
        return app.annotationInteraction_->TextActive() && app.annotationInteraction_->editor_->NativeHandle() &&
               app.annotation_ && app.annotation_->EditingText();
    }

    // 获取活动原位宿主窗口。
    // 入参：app：应用。
    // 返回：借用 HWND，无输入组件时为空。
    static HWND Host(const App& app)
    {
        return app.annotationInteraction_->editor_ ? app.annotationInteraction_->editor_->NativeHandle() : nullptr;
    }

    // 按原生类名寻找 RichEdit，不依赖内部数字控件 ID。
    // 入参：app：应用。
    // 返回：借用编辑控件句柄。
    static HWND Edit(const App& app)
    {
        const HWND host = Host(app);
        if (!host)
            return nullptr;
        for (HWND child = GetWindow(host, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
        {
            wchar_t name[64]{};
            GetClassNameW(child, name, static_cast<int>(std::size(name)));
            if (_wcsicmp(name, MSFTEDIT_CLASS) == 0)
                return child;
        }
        return nullptr;
    }

    // 通过公共组件请求完成，实际业务收尾仍由 WM_APP+9 执行。
    // 入参：app：应用；accept：确认或取消。
    // 返回：公共输入组件的排队结果。
    static InlineTextRequest Request(App& app, bool accept)
    {
        if (!app.annotationInteraction_->TextActive())
            return InlineTextRequest::Rejected;
        return accept ? app.annotationInteraction_->RequestText(true) : app.annotationInteraction_->RequestText(false);
    }

    // 仅分派当前测试的文字完成消息，不误绘制没有 GPU 资源的隐藏遮罩。
    // 入参：app：应用。
    // 返回：确实分派了一条完成消息为 true。
    static bool Pump(App& app)
    {
        MSG message{};
        if (!PeekMessageW(&message, app.messageWindow_, WM_APP + 9U, WM_APP + 9U, PM_REMOVE))
            return false;
        DispatchMessageW(&message);
        return true;
    }

    // 将迟到的完成通知交给真实消息窗口，验证序号准入而不复制生产逻辑。
    // 入参：app：应用；serial：消息中的序号；accept：完成意图。
    // 返回：无。
    static void Dispatch(App& app, std::uint64_t serial, bool accept)
    {
        SendMessageW(app.messageWindow_, WM_APP + 9U, static_cast<WPARAM>(serial), accept ? 1 : 0);
    }

    // 经 App 点击适配入口传入确定的物理点，避免移动或读取全局真实鼠标。
    // 入参：app：应用；point：模拟遮罩收到的桌面点。
    // 返回：无。
    static void Click(App& app, PointI point)
    {
        app.HandleAnnotationTextClick(point);
    }

    // 读取唯一标注状态，用于断言历史及候选隔离。
    // 入参：app：应用。
    // 返回：借用状态引用。
    static CaptureAnnotationState& State(App& app)
    {
        return *app.annotation_;
    }

    // 读取实际批次绘制使用的文档，确认活动文字未重复显示。
    // 入参：app：应用。
    // 返回：共享只读快照。
    static AnnotationSnapshot Drawing(const App& app)
    {
        return app.AnnotationForDrawing();
    }

    // 读取当前选区，用于检查确认点击没有同时触发选区移动或绘图。
    // 入参：app：应用。
    // 返回：选区快照。
    static SelectionSnapshot Selection(const App& app)
    {
        return app.selectionModel_->Snapshot();
    }

    // 查询完成消息是否已排队。
    // 入参：app：应用。
    // 返回：有未处理请求为 true。
    static bool Pending(const App& app)
    {
        return app.annotationInteraction_->textPending_;
    }

    // 查询文字会话序号。
    // 入参：app：应用。
    // 返回：单调递增序号。
    static std::uint64_t Serial(const App& app)
    {
        return app.annotationInteraction_->textSerial_;
    }

    // 查询全部遮罩的共同忙状态。
    // 入参：app：应用。
    // 返回：编辑或完成阶段受保护为 true。
    static bool Busy(const App& app)
    {
        return app.CompletionBusy();
    }

    // 查询布局失效及延后清理状态。
    // 入参：app：应用。
    // 返回：布局已失效为 true。
    static bool Invalidated(const App& app)
    {
        return app.overlayInvalidated_;
    }

    // 查询截图资源是否仍在安全边界之前保留。
    // 入参：app：应用。
    // 返回：遮罩和标注状态仍存在为 true。
    static bool SessionAlive(const App& app)
    {
        return app.overlaySession_ && app.annotation_;
    }

    // 模拟资源准备或绘制栈尚未返回，允许测试确定性排队输入而不运行真实图形命令。
    // 入参：app：应用；preparing/rendering：对应受保护边界。
    // 返回：无。
    static void PointerBusy(App& app, bool preparing, bool rendering)
    {
        app.annotationPreparing_ = preparing;
        app.overlayRendering_ = rendering;
    }

    // 控制完成模态门禁，以验证原生鼠标消息不会误进入重放队列。
    // 入参：app：应用；busy：完成忙状态。
    // 返回：无。
    static void CompletionBusy(App& app, bool busy)
    {
        app.completionBusy_ = busy;
    }

    // 调用真实排队入口保存已知物理坐标，不读取或移动当前全局鼠标。
    // 入参：app：应用；window：自有遮罩；message/flags：事件；point：原始物理点。
    // 返回：无。
    static void Queue(App& app, HWND window, UINT message, WPARAM flags, PointI point)
    {
        app.QueueOverlayPointer(window, message, flags, point);
    }

    // 在测试模拟图形调用返回后使用生产入口逐点重放。
    // 入参：app：应用。
    // 返回：无。
    static void Drain(App& app)
    {
        app.DrainOverlayPointers();
    }

    // 查询队列长度，避免从文档结果反推是否发生输入合并。
    // 入参：app：应用。
    // 返回：尚未消费的事件数。
    static std::size_t Queued(const App& app)
    {
        return app.overlayInput_->samples_.size();
    }

    // 仅分派队列唤醒消息，检查旧会话迟到的唤醒无副作用。
    // 入参：app：应用。
    // 返回：确实分派一条消息为 true。
    static bool PumpPointerWake(App& app)
    {
        MSG message{};
        if (!PeekMessageW(&message, app.messageWindow_, WM_APP + 10U, WM_APP + 10U, PM_REMOVE))
            return false;
        DispatchMessageW(&message);
        return true;
    }

    // 经生产会话关闭入口回收隐藏遮罩并清理旧输入。
    // 入参：app：应用。
    // 返回：无。
    static void Close(App& app)
    {
        app.CloseOverlay();
    }

    // 在公共输入创建前的窄宿主通知中请求关闭，观察真实 App 是否延后回收借用状态。
    // 入参：app：应用；retained：回调内状态和忙状态仍有效时写入 true。
    // 返回：无；创建栈退出后经过生产安全边界完成关闭。
    static void BeginWithReentrantClose(App& app, bool& retained)
    {
        CaptureAnnotationState* const state = app.annotation_.get();
        AnnotationTextHost host;
        host.owner = app.overlaySession_->ActivationWindow();
        host.notificationWindow = app.messageWindow_;
        // 查询创建途中请求关闭后的真实会话状态，不缓存准入结果。
        // 入参：无。返回：原状态仍归宿主且未失效时 true。
        host.valid = [&app, state]() { return app.annotation_.get() == state && !app.overlayInvalidated_; };
        // 模拟同步窗口创建期间宿主请求结束截图，不能销毁当前事务借用。
        // 入参：无。返回：无。
        host.changed = [&app, state, &retained]()
        {
            app.CloseOverlay();
            retained = app.annotation_.get() == state && app.overlaySession_ && app.overlayInvalidated_ &&
                       app.CompletionBusy() && state->EditingText();
        };
        // 提供确定的窗口错误文字，不读取用户设置。
        // 入参：未使用的文字键。返回：本例固定文字。
        host.text = [](std::string_view) { return std::wstring{L"测试输入"}; };
        (void)app.annotationInteraction_->BeginText(*state, app.selectionModel_->Snapshot().rectangle, {120, 120}, 0U,
                                                    std::move(host));
        app.CompleteAnnotationTextBoundary();
    }

    // 在真实输入失败回调调用本地化查询时请求关闭，验证回调期间的窗口和状态保护。
    // 入参：app：活动文字应用；retained：回调内资源仍有效时写入 true。
    // 返回：无；新回调仅借用本例应用和断言结果。
    static void CloseOnTextError(App& app, bool& retained)
    {
        CaptureAnnotationState* const state = app.annotation_.get();
        const HWND window = app.annotationInteraction_->editor_->NativeHandle();
        // 使用同步错误适配触发真实关闭入口，不修改控制器状态标志。
        // 入参：未使用的文字键。返回：本例错误文字。
        app.annotationInteraction_->textHost_.text = [&app, state, window, &retained](std::string_view)
        {
            app.CloseOverlay();
            retained = app.annotation_.get() == state && app.overlaySession_ && app.overlayInvalidated_ &&
                       state->EditingText() && IsWindow(window);
            return std::wstring{L"测试关闭"};
        };
    }

    // 经生产消息边界完成被回调栈阻止的文字结束和失效会话清理。
    // 入参：app：应用。
    // 返回：无。
    static void DrainText(App& app)
    {
        app.DrainAnnotationText();
    }
};
} // namespace open_st

namespace
{
using namespace open_st;
using Access = AppAnnotationTextTestAccess;
constexpr RectI TEXT_CROP{100, 100, 500, 400};

class FakeBackend final : public HotkeyBackend
{
  public:
    int lastId{};

    // 记录管理器分配的身份，不触碰系统全局快捷键。
    // 入参：id 为注册身份；error 输出成功码；其余参数由真实管理器校验。
    // 返回：总是成功。
    bool Register(HWND, int id, UINT, UINT, DWORD& error) noexcept override
    {
        this->lastId = id;
        error = ERROR_SUCCESS;
        return true;
    }

    // 完成模拟释放，保证测试退出不会调用系统注册接口。
    // 入参：error 输出成功码；其余参数为模拟注册。
    // 返回：总是成功。
    bool Unregister(HWND, int, DWORD& error) noexcept override
    {
        error = ERROR_SUCCESS;
        return true;
    }
};

struct PasteObserver final
{
    HWND edit{};
    unsigned count{};

    // 拦截并吞掉测试控件的粘贴消息，绝不读取或修改用户剪贴板。
    // 入参：标准子类回调参数；data 为仍存活的观察器。
    // 返回：粘贴返回零，其余交给原控件。
    static LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data)
    {
        if (message == WM_PASTE)
        {
            ++reinterpret_cast<PasteObserver*>(data)->count;
            return 0;
        }
        return DefSubclassProc(window, message, wParam, lParam);
    }

    // 安装只作用于本例 RichEdit 的观察器。
    // 入参：window 为自有控件。
    // 返回：子类安装成功时 true。
    bool Install(HWND window)
    {
        this->edit = window;
        return SetWindowSubclass(window, Procedure, 71U, reinterpret_cast<DWORD_PTR>(this)) != FALSE;
    }

    // 在测试断言提前返回时仍解除回调，避免借用栈状态悬空。
    // 入参：无。
    // 返回：无。
    ~PasteObserver()
    {
        if (IsWindow(this->edit))
            RemoveWindowSubclass(this->edit, Procedure, 71U);
    }
};

class AnnotationTextIntegrationTest : public testing::Test
{
  protected:
    // 创建两个自有隐藏遮罩及未显示输入组件的真实应用环境。
    // 入参：无。
    // 返回：无，初始化失败为真实测试失败。
    void SetUp() override
    {
        this->app_ = std::make_unique<App>(GetModuleHandleW(nullptr));
        ASSERT_TRUE(Access::Initialize(*this->app_, this->windows_));
    }

    // 经应用析构清理输入和遮罩，只移除本例剩余消息而不分派无渲染资源的绘制请求。
    // 入参：无。
    // 返回：无。
    void TearDown() override
    {
        if (this->app_)
        {
            Access::PointerBusy(*this->app_, false, false);
            Access::CompletionBusy(*this->app_, false);
        }
        this->app_.reset();
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
        }
    }

    // 通过真实 RichEdit 完整文本消息输入测试正文。
    // 入参：text：本例正文，不涉及剪贴板。
    // 返回：无，同时确认控件存在且消息成功。
    void SetText(const wchar_t* text)
    {
        const HWND edit = Access::Edit(*this->app_);
        ASSERT_NE(edit, nullptr);
        ASSERT_TRUE(SetWindowTextW(edit, text));
    }

    // 读取当前真实控件内容，供失败恢复及原生撤销检查。
    // 入参：无。
    // 返回：独立正文副本。
    std::wstring Text() const
    {
        const HWND edit = Access::Edit(*this->app_);
        const int length = GetWindowTextLengthW(edit);
        std::wstring text(static_cast<std::size_t>(length) + 1U, L'\0');
        text.resize(static_cast<std::size_t>(GetWindowTextW(edit, text.data(), length + 1)));
        return text;
    }

    FakeBackend hotkeyBackend_;
    std::unique_ptr<App> app_;
    std::array<HWND, 2U> windows_{};
};

// 验证有效注册的 Ctrl+V 只到达持焦点的原位控件，不开始截图或改动标注文档。
// 入参：无，粘贴消息被自有子类吞掉，不访问用户剪贴板。
// 返回：消息计数、输入及事务保持断言。
TEST_F(AnnotationTextIntegrationTest, registered_paste_hotkey_reaches_only_active_inline_editor)
{
    ASSERT_TRUE(Access::Begin(*this->app_));
    this->SetText(L"粘贴路由");
    const HWND host = Access::Host(*this->app_);
    const HWND edit = Access::Edit(*this->app_);
    Access::Foreground(*this->app_, host);
    Access::Focus(*this->app_);
    ASSERT_EQ(GetFocus(), edit);
    PasteObserver observer;
    ASSERT_TRUE(observer.Install(edit));
    ASSERT_TRUE(Access::RegisterHotkey(*this->app_, this->hotkeyBackend_, {MOD_CONTROL, 'V'}));
    Access::Hotkey(*this->app_, this->hotkeyBackend_.lastId, {MOD_CONTROL, 'V'});
    EXPECT_EQ(observer.count, 1U);
    EXPECT_EQ(this->Text(), L"粘贴路由");
    EXPECT_EQ(Access::Host(*this->app_), host);
    EXPECT_TRUE(Access::State(*this->app_).EditingText());
    EXPECT_EQ(Access::State(*this->app_).Committed(), nullptr);
    EXPECT_FALSE(Access::Pending(*this->app_));
}

// 验证身份、时间、完整组合和控件状态均在转发前校验。
// 入参：无，所有消息只指向本例窗口，粘贴被吞掉。
// 返回：旧身份、旧时间、错组合、失焦、组合输入和完成排队均不转发断言。
TEST_F(AnnotationTextIntegrationTest, paste_hotkey_rejects_stale_notifications_and_ineligible_editor)
{
    ASSERT_TRUE(Access::Begin(*this->app_));
    this->SetText(L"受保护文字");
    const HWND edit = Access::Edit(*this->app_);
    Access::Foreground(*this->app_, Access::Host(*this->app_));
    Access::Focus(*this->app_);
    ASSERT_EQ(GetFocus(), edit);
    PasteObserver observer;
    ASSERT_TRUE(observer.Install(edit));
    ASSERT_TRUE(Access::RegisterHotkey(*this->app_, this->hotkeyBackend_, {MOD_CONTROL, 'A'}));
    const int retired = this->hotkeyBackend_.lastId;
    ASSERT_TRUE(Access::RegisterHotkey(*this->app_, this->hotkeyBackend_, {MOD_CONTROL, 'V'}));
    const int current = this->hotkeyBackend_.lastId;
    ASSERT_NE(retired, current);
    Access::Hotkey(*this->app_, retired, {MOD_CONTROL, 'V'});
    Access::Hotkey(*this->app_, current, {MOD_CONTROL, 'V'}, 99U);
    Access::Hotkey(*this->app_, current, {MOD_CONTROL, 'V'}, 100U);
    Access::Hotkey(*this->app_, current, {MOD_CONTROL | MOD_SHIFT, 'V'});
    Access::Hotkey(*this->app_, current, {MOD_CONTROL, 'C'});
    EXPECT_EQ(observer.count, 0U);
    SetFocus(this->windows_[1]);
    ASSERT_NE(GetFocus(), edit);
    Access::Hotkey(*this->app_, current, {MOD_CONTROL, 'V'});
    EXPECT_EQ(observer.count, 0U);
    Access::Focus(*this->app_);
    ASSERT_EQ(GetFocus(), edit);
    Access::Foreground(*this->app_, this->windows_[1]);
    Access::Hotkey(*this->app_, current, {MOD_CONTROL, 'V'});
    EXPECT_EQ(observer.count, 0U);
    EXPECT_EQ(GetFocus(), edit);
    Access::Foreground(*this->app_, Access::Host(*this->app_));
    SendMessageW(edit, WM_IME_STARTCOMPOSITION, 0, 0);
    Access::Hotkey(*this->app_, current, {MOD_CONTROL, 'V'});
    EXPECT_EQ(observer.count, 0U);
    SendMessageW(edit, WM_IME_ENDCOMPOSITION, 0, 0);
    Access::Hotkey(*this->app_, current, {MOD_CONTROL, 'V'});
    EXPECT_EQ(observer.count, 1U);
    ASSERT_EQ(Access::Request(*this->app_, true), InlineTextRequest::Queued);
    Access::Hotkey(*this->app_, current, {MOD_CONTROL, 'V'});
    EXPECT_EQ(observer.count, 1U);
    EXPECT_EQ(this->Text(), L"受保护文字");
    ASSERT_TRUE(Access::Pump(*this->app_));
}

// 验证 Ctrl+A 使用原生全选，额外修饰键即便是有效全局注册也不变成文字命令。
// 入参：无，不合成物理键盘输入。
// 返回：全选范围、精确组合及事务保持断言。
TEST_F(AnnotationTextIntegrationTest, select_all_hotkey_requires_exact_control_combination)
{
    ASSERT_TRUE(Access::Begin(*this->app_));
    this->SetText(L"Select 全部");
    const HWND edit = Access::Edit(*this->app_);
    Access::Foreground(*this->app_, Access::Host(*this->app_));
    Access::Focus(*this->app_);
    ASSERT_EQ(GetFocus(), edit);
    SendMessageW(edit, EM_SETSEL, 2U, 2);
    ASSERT_TRUE(Access::RegisterHotkey(*this->app_, this->hotkeyBackend_, {MOD_CONTROL | MOD_ALT, 'A'}));
    Access::Hotkey(*this->app_, this->hotkeyBackend_.lastId, {MOD_CONTROL | MOD_ALT, 'A'});
    CHARRANGE selection{};
    SendMessageW(edit, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&selection));
    EXPECT_EQ(selection.cpMin, 2);
    EXPECT_EQ(selection.cpMax, 2);
    ASSERT_TRUE(Access::RegisterHotkey(*this->app_, this->hotkeyBackend_, {MOD_CONTROL, 'A'}));
    Access::Hotkey(*this->app_, this->hotkeyBackend_.lastId, {MOD_CONTROL, 'A'});
    SendMessageW(edit, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&selection));
    EXPECT_EQ(selection.cpMin, 0);
    EXPECT_EQ(selection.cpMax, GetWindowTextLengthW(edit));
    EXPECT_FALSE(Access::Pending(*this->app_));
    EXPECT_EQ(Access::State(*this->app_).Committed(), nullptr);
}

// 验证真实输入回调只更新文字草稿，完成消息分派后生成唯一 Text 对象和一条历史。
// 入参：无。
// 返回：忙状态、异步发布、LF 正文和单步撤销重做断言。
TEST_F(AnnotationTextIntegrationTest, queued_commit_creates_text_in_one_history_entry)
{
    ASSERT_TRUE(Access::Begin(*this->app_));
    const HWND host = Access::Host(*this->app_);
    EXPECT_TRUE(Access::Busy(*this->app_));
    this->SetText(L"中文\r\n日本語");
    EXPECT_EQ(Access::State(*this->app_).Committed(), nullptr);
    EXPECT_EQ(Access::Drawing(*this->app_), nullptr);
    ASSERT_EQ(Access::Request(*this->app_, true), InlineTextRequest::Queued);
    EXPECT_TRUE(IsWindow(host));
    EXPECT_TRUE(Access::Pending(*this->app_));
    EXPECT_EQ(Access::State(*this->app_).Committed(), nullptr);
    ASSERT_TRUE(Access::Pump(*this->app_));
    EXPECT_FALSE(IsWindow(host));
    EXPECT_FALSE(Access::Busy(*this->app_));
    const AnnotationSnapshot committed = Access::State(*this->app_).Committed();
    ASSERT_NE(committed, nullptr);
    ASSERT_EQ(committed->size(), 1U);
    EXPECT_EQ(committed->front().kind, AnnotationKind::Text);
    EXPECT_EQ(*std::get<AnnotationText>(committed->front().payload).text, u"中文\n日本語");
    EXPECT_NE(committed->front().id, 0U);
    RectI restored;
    ASSERT_TRUE(Access::State(*this->app_).Restore(false, TEXT_CROP, restored));
    EXPECT_EQ(Access::State(*this->app_).Committed(), nullptr);
    EXPECT_FALSE(Access::State(*this->app_).CanUndo());
    ASSERT_TRUE(Access::State(*this->app_).Restore(true, TEXT_CROP, restored));
    EXPECT_EQ(Access::State(*this->app_).Committed(), committed);
}

// 验证重编辑期间绘制快照排除原文字，取消通过排队收尾恢复同一原快照。
// 入参：无。
// 返回：防双绘、取消原子性和修订保持断言。
TEST_F(AnnotationTextIntegrationTest, editing_hides_target_and_cancel_restores_original)
{
    ASSERT_TRUE(Access::Begin(*this->app_));
    this->SetText(L"原文");
    ASSERT_EQ(Access::Request(*this->app_, true), InlineTextRequest::Queued);
    ASSERT_TRUE(Access::Pump(*this->app_));
    const AnnotationSnapshot original = Access::State(*this->app_).Committed();
    const std::uint64_t revision = Access::State(*this->app_).Revision();
    ASSERT_TRUE(Access::Begin(*this->app_, {}, original->front().id));
    ASSERT_NE(Access::Drawing(*this->app_), nullptr);
    EXPECT_TRUE(Access::Drawing(*this->app_)->empty());
    EXPECT_EQ(this->Text(), L"原文");
    this->SetText(L"不提交的修改");
    ASSERT_EQ(Access::Request(*this->app_, false), InlineTextRequest::Queued);
    ASSERT_TRUE(Access::Pump(*this->app_));
    EXPECT_EQ(Access::State(*this->app_).Committed(), original);
    EXPECT_EQ(Access::Drawing(*this->app_), original);
    EXPECT_EQ(Access::State(*this->app_).Revision(), revision);
}

// 验证空白新建结束不增加对象，已有文字改为空白拒绝确认且保留同一输入控件。
// 入参：无。
// 返回：新建与已有正文不同的空白契约断言。
TEST_F(AnnotationTextIntegrationTest, blank_new_text_is_noop_but_existing_blank_stays_editable)
{
    ASSERT_TRUE(Access::Begin(*this->app_));
    this->SetText(L" \r\n\t");
    ASSERT_EQ(Access::Request(*this->app_, true), InlineTextRequest::Queued);
    ASSERT_TRUE(Access::Pump(*this->app_));
    EXPECT_EQ(Access::State(*this->app_).Committed(), nullptr);
    EXPECT_FALSE(Access::State(*this->app_).CanUndo());
    ASSERT_TRUE(Access::Begin(*this->app_));
    this->SetText(L"保留");
    ASSERT_EQ(Access::Request(*this->app_, true), InlineTextRequest::Queued);
    ASSERT_TRUE(Access::Pump(*this->app_));
    const AnnotationSnapshot original = Access::State(*this->app_).Committed();
    ASSERT_TRUE(Access::Begin(*this->app_, {}, original->front().id));
    const HWND edit = Access::Edit(*this->app_);
    this->SetText(L" \r\n");
    EXPECT_EQ(Access::Request(*this->app_, true), InlineTextRequest::Rejected);
    EXPECT_FALSE(Access::Pending(*this->app_));
    EXPECT_FALSE(Access::Pump(*this->app_));
    EXPECT_EQ(Access::Edit(*this->app_), edit);
    EXPECT_TRUE(Access::State(*this->app_).EditingText());
    EXPECT_EQ(Access::State(*this->app_).Committed(), original);
    ASSERT_EQ(Access::Request(*this->app_, false), InlineTextRequest::Queued);
    ASSERT_TRUE(Access::Pump(*this->app_));
}

// 验证另一遮罩区域内点击只结束当前文字，不额外创建笔迹；选区外点击保持编辑。
// 入参：无，物理点击直接进入生产适配函数，不读取或移动全局鼠标。
// 返回：点击吞掉、裁剪保持及唯一对象断言。
TEST_F(AnnotationTextIntegrationTest, other_overlay_click_commits_once_and_outside_click_keeps_editor)
{
    ASSERT_TRUE(Access::Begin(*this->app_));
    this->SetText(L"跨窗确认");
    Access::Click(*this->app_, {510, 350});
    EXPECT_FALSE(Access::Pending(*this->app_));
    EXPECT_NE(Access::Host(*this->app_), nullptr);
    EXPECT_EQ(SendMessageW(this->windows_[1], WM_MOUSEACTIVATE, 0, MAKELPARAM(HTCLIENT, WM_LBUTTONDOWN)),
              MA_NOACTIVATE);
    Access::Click(*this->app_, {450, 350});
    EXPECT_TRUE(Access::Pending(*this->app_));
    ASSERT_TRUE(Access::Pump(*this->app_));
    ASSERT_EQ(Access::State(*this->app_).Committed()->size(), 1U);
    EXPECT_FALSE(Access::State(*this->app_).Active());
    EXPECT_EQ(Access::State(*this->app_).Tool(), CaptureAnnotationTool::Text);
    const SelectionSnapshot selection = Access::Selection(*this->app_);
    EXPECT_EQ(selection.rectangle.left, TEXT_CROP.left);
    EXPECT_EQ(selection.rectangle.top, TEXT_CROP.top);
    EXPECT_EQ(selection.rectangle.right, TEXT_CROP.right);
    EXPECT_EQ(selection.rectangle.bottom, TEXT_CROP.bottom);
}

// 验证输入法组合期间另一遮罩不会抢激活，点击与确认均不排队，组合结束后可正常确认。
// 入参：无，只向自有 RichEdit 发送组合生命周期消息。
// 返回：组合保护及结束后恢复准入断言。
TEST_F(AnnotationTextIntegrationTest, composition_blocks_cross_overlay_confirmation_until_end)
{
    ASSERT_TRUE(Access::Begin(*this->app_));
    this->SetText(L"组合保护");
    const HWND edit = Access::Edit(*this->app_);
    SendMessageW(edit, WM_IME_STARTCOMPOSITION, 0, 0);
    EXPECT_EQ(SendMessageW(this->windows_[1], WM_MOUSEACTIVATE, 0, MAKELPARAM(HTCLIENT, WM_LBUTTONDOWN)),
              MA_NOACTIVATE);
    EXPECT_EQ(Access::Request(*this->app_, true), InlineTextRequest::Rejected);
    Access::Click(*this->app_, {450, 350});
    EXPECT_FALSE(Access::Pending(*this->app_));
    EXPECT_FALSE(Access::Pump(*this->app_));
    EXPECT_EQ(Access::Edit(*this->app_), edit);
    SendMessageW(edit, WM_IME_ENDCOMPOSITION, 0, 0);
    ASSERT_EQ(Access::Request(*this->app_, true), InlineTextRequest::Queued);
    ASSERT_TRUE(Access::Pump(*this->app_));
    EXPECT_EQ(Access::State(*this->app_).Committed()->size(), 1U);
}

// 验证旧序号通知不能取消或完成新输入，也不消费新编辑已经排队的确认。
// 入参：无。
// 返回：序号隔离和当前正文提交断言。
TEST_F(AnnotationTextIntegrationTest, stale_serial_messages_do_not_touch_new_editor)
{
    ASSERT_TRUE(Access::Begin(*this->app_));
    const std::uint64_t oldSerial = Access::Serial(*this->app_);
    ASSERT_EQ(Access::Request(*this->app_, false), InlineTextRequest::Queued);
    ASSERT_TRUE(Access::Pump(*this->app_));
    ASSERT_TRUE(Access::Begin(*this->app_));
    const HWND current = Access::Host(*this->app_);
    EXPECT_GT(Access::Serial(*this->app_), oldSerial);
    this->SetText(L"新会话文字");
    ASSERT_EQ(Access::Request(*this->app_, true), InlineTextRequest::Queued);
    Access::Dispatch(*this->app_, oldSerial, false);
    Access::Dispatch(*this->app_, oldSerial, true);
    EXPECT_EQ(Access::Host(*this->app_), current);
    EXPECT_TRUE(Access::Pending(*this->app_));
    ASSERT_TRUE(Access::Pump(*this->app_));
    EXPECT_EQ(*std::get<AnnotationText>(Access::State(*this->app_).Committed()->front().payload).text, u"新会话文字");
}

// 验证同一 App 关闭并重建截图后序号仍递增，旧排队确认不能消费新截图文字请求。
// 入参：无；仅重建本例隐藏窗口，不进行桌面捕获。
// 返回：跨截图序号、旧消息隔离、控件存活和新正文唯一提交断言。
TEST_F(AnnotationTextIntegrationTest, closed_capture_text_request_cannot_finish_next_capture_editor)
{
    ASSERT_TRUE(Access::Begin(*this->app_));
    this->SetText(L"旧截图正文");
    const std::uint64_t retired = Access::Serial(*this->app_);
    ASSERT_EQ(Access::Request(*this->app_, true), InlineTextRequest::Queued);
    Access::Close(*this->app_);
    ASSERT_FALSE(Access::SessionAlive(*this->app_));
    ASSERT_TRUE(Access::Initialize(*this->app_, this->windows_));
    ASSERT_TRUE(Access::Begin(*this->app_));
    ASSERT_GT(Access::Serial(*this->app_), retired);
    this->SetText(L"新截图正文");
    const HWND current = Access::Host(*this->app_);
    ASSERT_EQ(Access::Request(*this->app_, true), InlineTextRequest::Queued);
    ASSERT_TRUE(Access::Pump(*this->app_));
    Access::Dispatch(*this->app_, retired, false);
    EXPECT_EQ(Access::Host(*this->app_), current);
    EXPECT_TRUE(IsWindow(current));
    EXPECT_TRUE(Access::Pending(*this->app_));
    EXPECT_TRUE(Access::State(*this->app_).EditingText());
    EXPECT_EQ(Access::State(*this->app_).Committed(), nullptr);
    ASSERT_TRUE(Access::Pump(*this->app_));
    const AnnotationSnapshot document = Access::State(*this->app_).Committed();
    ASSERT_NE(document, nullptr);
    ASSERT_EQ(document->size(), 1U);
    EXPECT_EQ(*std::get<AnnotationText>(document->front().payload).text, u"新截图正文");
}

// 验证控件创建通知中关闭截图时仍保留事务借用，创建返回后才能释放窗口和状态。
// 入参：无；通过控制器窄宿主回调触发真实 App 关闭入口。
// 返回：同步创建期间存活及安全边界后全部回收断言。
TEST_F(AnnotationTextIntegrationTest, close_during_text_opening_preserves_state_until_boundary)
{
    bool retained{};
    Access::BeginWithReentrantClose(*this->app_, retained);
    EXPECT_TRUE(retained);
    EXPECT_FALSE(Access::SessionAlive(*this->app_));
    EXPECT_FALSE(Access::Busy(*this->app_));
    EXPECT_FALSE(IsWindow(this->windows_[0]));
    EXPECT_FALSE(IsWindow(this->windows_[1]));
}

// 验证已有文字清空后确认失败的回调内请求关闭，不提前销毁 RichEdit 或唯一文字事务。
// 入参：无；真实控件输入和确认触发业务错误适配，不手工设置分派保护标志。
// 返回：回调内资源保持及返回后取消旧事务、销毁会话断言。
TEST_F(AnnotationTextIntegrationTest, close_during_text_callback_defers_state_and_window_destruction)
{
    ASSERT_TRUE(Access::Begin(*this->app_));
    this->SetText(L"已有文字");
    ASSERT_EQ(Access::Request(*this->app_, true), InlineTextRequest::Queued);
    ASSERT_TRUE(Access::Pump(*this->app_));
    const std::uint64_t id = Access::State(*this->app_).Committed()->front().id;
    ASSERT_TRUE(Access::Begin(*this->app_, {120, 120}, id));
    this->SetText(L"");
    const HWND host = Access::Host(*this->app_);
    bool retained{};
    Access::CloseOnTextError(*this->app_, retained);
    EXPECT_EQ(Access::Request(*this->app_, true), InlineTextRequest::Rejected);
    EXPECT_TRUE(retained);
    EXPECT_TRUE(Access::SessionAlive(*this->app_));
    EXPECT_TRUE(IsWindow(host));
    Access::DrainText(*this->app_);
    EXPECT_FALSE(Access::SessionAlive(*this->app_));
    EXPECT_FALSE(IsWindow(host));
    EXPECT_FALSE(Access::Busy(*this->app_));
}

// 验证确认已经排队时另一屏显示失效仍延后到安全消息边界取消，不能提交过期截图。
// 入参：无。
// 返回：同步栈存活和排队后资源回收断言。
TEST_F(AnnotationTextIntegrationTest, display_change_defers_cleanup_and_cancels_queued_commit)
{
    ASSERT_TRUE(Access::Begin(*this->app_));
    this->SetText(L"不得发布失效截图");
    const HWND host = Access::Host(*this->app_);
    ASSERT_EQ(Access::Request(*this->app_, true), InlineTextRequest::Queued);
    SendMessageW(this->windows_[1], WM_DISPLAYCHANGE, 0, 0);
    EXPECT_TRUE(Access::Invalidated(*this->app_));
    EXPECT_TRUE(Access::SessionAlive(*this->app_));
    EXPECT_TRUE(IsWindow(host));
    EXPECT_EQ(Access::State(*this->app_).Committed(), nullptr);
    ASSERT_TRUE(Access::Pump(*this->app_));
    EXPECT_FALSE(Access::SessionAlive(*this->app_));
    EXPECT_FALSE(IsWindow(host));
    EXPECT_FALSE(IsWindow(this->windows_[0]));
    EXPECT_FALSE(IsWindow(this->windows_[1]));
}

// 验证候选准备在排队提交时失败，App Resume 同一个 RichEdit 并保留正文与原生撤销记录。
// 入参：无，来源准备失败由同步钩子确定性注入，不伪造内存故障或调用真实捕获。
// 返回：同控件恢复、原生撤销重做和后续成功提交断言。
TEST_F(AnnotationTextIntegrationTest, preparation_failure_resumes_same_control_with_native_history)
{
    CaptureAnnotationState& state = Access::State(*this->app_);
    state.SetTool(CaptureAnnotationTool::Mosaic);
    ASSERT_TRUE(state.BeginDraw({300, 150}, TEXT_CROP));
    ASSERT_EQ(state.EndDraw({400, 250}), AnnotationCommitResult::Committed);
    const AnnotationSnapshot baseline = state.Committed();
    bool ready = true;
    // 准备成功开窗，后续完成前将准入切为失败，模拟来源暂时不可用。
    // 入参：未命名参数为候选和对应裁剪。
    // 返回：ready。
    ASSERT_TRUE(state.SetPreviewPreparation([&ready](const AnnotationSnapshot&, RectI) { return ready; }));
    state.SetTool(CaptureAnnotationTool::Text);
    ASSERT_TRUE(Access::Begin(*this->app_));
    const HWND edit = Access::Edit(*this->app_);
    SendMessageW(edit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"重试正文"));
    ASSERT_TRUE(SendMessageW(edit, EM_CANUNDO, 0, 0));
    ready = false;
    ASSERT_EQ(Access::Request(*this->app_, true), InlineTextRequest::Queued);
    ASSERT_TRUE(Access::Pump(*this->app_));
    EXPECT_EQ(Access::Edit(*this->app_), edit);
    EXPECT_FALSE(Access::Pending(*this->app_));
    EXPECT_TRUE(Access::Busy(*this->app_));
    EXPECT_TRUE(state.EditingText());
    EXPECT_EQ(state.Committed(), baseline);
    EXPECT_EQ(this->Text(), L"重试正文");
    EXPECT_EQ(GetWindowLongPtrW(edit, GWL_STYLE) & ES_READONLY, 0);
    ASSERT_TRUE(SendMessageW(edit, EM_UNDO, 0, 0));
    EXPECT_TRUE(this->Text().empty());
    ASSERT_TRUE(SendMessageW(edit, EM_REDO, 0, 0));
    EXPECT_EQ(this->Text(), L"重试正文");
    ready = true;
    ASSERT_EQ(Access::Request(*this->app_, true), InlineTextRequest::Queued);
    ASSERT_TRUE(Access::Pump(*this->app_));
    EXPECT_FALSE(IsWindow(edit));
    ASSERT_EQ(state.Committed()->size(), 2U);
    EXPECT_EQ(state.Committed()->front().kind, AnnotationKind::Mosaic);
    EXPECT_EQ(state.Committed()->back().kind, AnnotationKind::Text);
}
// 验证准备期间排队的每个物理点都保留，第一笔正常释放捕获不会清掉后面的第二笔。
// 入参：无，不读取真实鼠标位置。
// 返回：两笔路径逐点坐标、队列清理和每笔一次历史断言。
TEST_F(AnnotationTextIntegrationTest, pointer_queue_preserves_pen_points_and_following_gesture)
{
    Access::State(*this->app_).SetTool(CaptureAnnotationTool::Pen);
    Access::PointerBusy(*this->app_, true, false);
    const std::array<PointI, 4U> first{{{120, 130}, {140, 160}, {135, 175}, {160, 150}}};
    Access::Queue(*this->app_, this->windows_[0], WM_LBUTTONDOWN, MK_LBUTTON, first[0]);
    Access::Queue(*this->app_, this->windows_[0], WM_MOUSEMOVE, MK_LBUTTON, first[1]);
    Access::Queue(*this->app_, this->windows_[0], WM_MOUSEMOVE, MK_LBUTTON, first[2]);
    Access::Queue(*this->app_, this->windows_[0], WM_LBUTTONUP, 0, first[3]);
    Access::Queue(*this->app_, this->windows_[1], WM_LBUTTONDOWN, MK_LBUTTON, {320, 180});
    Access::Queue(*this->app_, this->windows_[1], WM_MOUSEMOVE, MK_LBUTTON, {330, 190});
    Access::Queue(*this->app_, this->windows_[1], WM_LBUTTONUP, 0, {340, 195});
    EXPECT_EQ(Access::Queued(*this->app_), 7U);
    Access::Drain(*this->app_);
    EXPECT_EQ(Access::Queued(*this->app_), 7U);
    EXPECT_EQ(Access::State(*this->app_).Committed(), nullptr);
    Access::PointerBusy(*this->app_, false, false);
    Access::Drain(*this->app_);
    EXPECT_EQ(Access::Queued(*this->app_), 0U);
    const AnnotationSnapshot document = Access::State(*this->app_).Committed();
    ASSERT_NE(document, nullptr);
    ASSERT_EQ(document->size(), 2U);
    const AnnotationObject& object = document->front();
    ASSERT_EQ(object.kind, AnnotationKind::Pen);
    const std::shared_ptr<const std::vector<AnnotationPoint>> points =
        std::get<AnnotationStroke>(object.payload).points;
    ASSERT_EQ(points->size(), first.size());
    for (std::size_t index = 0U; index < first.size(); ++index)
    {
        EXPECT_EQ(object.origin.x + (*points)[index].x, first[index].x);
        EXPECT_EQ(object.origin.y + (*points)[index].y, first[index].y);
    }
    const AnnotationObject& second = document->back();
    EXPECT_EQ(second.origin.x, 320.0);
    EXPECT_EQ(second.origin.y, 180.0);
    EXPECT_EQ(std::get<AnnotationStroke>(second.payload).points->size(), 3U);
    EXPECT_FALSE(Access::State(*this->app_).Active());
    RectI restored;
    ASSERT_TRUE(Access::State(*this->app_).Restore(false, TEXT_CROP, restored));
    ASSERT_EQ(Access::State(*this->app_).Committed()->size(), 1U);
    EXPECT_EQ(Access::State(*this->app_).Committed()->front().id, object.id);
    ASSERT_TRUE(Access::State(*this->app_).Restore(false, TEXT_CROP, restored));
    EXPECT_EQ(Access::State(*this->app_).Committed(), nullptr);
}

// 验证取消屏障丢弃此前待重放手势，但保留屏障后的新按下；完成忙状态不排队原生鼠标消息。
// 入参：无。
// 返回：取消后新手势的唯一几何、旧手势不复活及完成门禁断言。
TEST_F(AnnotationTextIntegrationTest, pointer_cancel_barrier_keeps_later_down_and_modal_input_is_ignored)
{
    Access::State(*this->app_).SetTool(CaptureAnnotationTool::Rectangle);
    Access::PointerBusy(*this->app_, false, true);
    Access::Queue(*this->app_, this->windows_[0], WM_LBUTTONDOWN, MK_LBUTTON, {120, 120});
    Access::Queue(*this->app_, this->windows_[0], WM_MOUSEMOVE, MK_LBUTTON, {180, 180});
    SendMessageW(this->windows_[1], WM_CANCELMODE, 0, 0);
    EXPECT_EQ(Access::Queued(*this->app_), 1U);
    Access::Queue(*this->app_, this->windows_[1], WM_LBUTTONDOWN, MK_LBUTTON, {330, 200});
    Access::Queue(*this->app_, this->windows_[1], WM_MOUSEMOVE, MK_LBUTTON, {370, 250});
    Access::Queue(*this->app_, this->windows_[1], WM_LBUTTONUP, 0, {380, 260});
    EXPECT_EQ(Access::Queued(*this->app_), 4U);
    Access::PointerBusy(*this->app_, false, false);
    Access::Drain(*this->app_);
    const AnnotationSnapshot document = Access::State(*this->app_).Committed();
    ASSERT_NE(document, nullptr);
    ASSERT_EQ(document->size(), 1U);
    EXPECT_EQ(document->front().origin.x, 330.0);
    EXPECT_EQ(document->front().origin.y, 200.0);
    EXPECT_EQ(document->front().extent.x, 50.0);
    EXPECT_EQ(document->front().extent.y, 60.0);
    EXPECT_EQ(Access::Queued(*this->app_), 0U);
    Access::PointerBusy(*this->app_, true, false);
    Access::CompletionBusy(*this->app_, true);
    SendMessageW(this->windows_[0], WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(20, 20));
    SendMessageW(this->windows_[0], WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(40, 40));
    SendMessageW(this->windows_[0], WM_LBUTTONUP, 0, MAKELPARAM(60, 60));
    EXPECT_EQ(Access::Queued(*this->app_), 0U);
    Access::CompletionBusy(*this->app_, false);
    Access::PointerBusy(*this->app_, false, false);
    Access::Drain(*this->app_);
    EXPECT_EQ(Access::State(*this->app_).Committed(), document);
    EXPECT_FALSE(Access::State(*this->app_).Active());
}

// 验证关闭会话清空旧窗口输入，迟到的队列唤醒不能将旧手势画进新会话。
// 入参：无。
// 返回：关闭清理、同一 App 新会话隔离及新输入仍可用断言。
TEST_F(AnnotationTextIntegrationTest, pointer_queue_is_cleared_before_next_capture_session)
{
    Access::State(*this->app_).SetTool(CaptureAnnotationTool::Rectangle);
    Access::PointerBusy(*this->app_, true, false);
    Access::Queue(*this->app_, this->windows_[0], WM_LBUTTONDOWN, MK_LBUTTON, {120, 120});
    Access::Queue(*this->app_, this->windows_[0], WM_MOUSEMOVE, MK_LBUTTON, {190, 190});
    Access::Queue(*this->app_, this->windows_[0], WM_LBUTTONUP, 0, {200, 200});
    ASSERT_EQ(Access::Queued(*this->app_), 3U);
    Access::PointerBusy(*this->app_, false, false);
    Access::Close(*this->app_);
    EXPECT_EQ(Access::Queued(*this->app_), 0U);
    EXPECT_FALSE(IsWindow(this->windows_[0]));
    EXPECT_FALSE(IsWindow(this->windows_[1]));
    ASSERT_TRUE(Access::Initialize(*this->app_, this->windows_));
    Access::State(*this->app_).SetTool(CaptureAnnotationTool::Rectangle);
    ASSERT_TRUE(Access::PumpPointerWake(*this->app_));
    EXPECT_EQ(Access::State(*this->app_).Committed(), nullptr);
    EXPECT_FALSE(Access::State(*this->app_).Active());
    Access::PointerBusy(*this->app_, false, true);
    Access::Queue(*this->app_, this->windows_[1], WM_LBUTTONDOWN, MK_LBUTTON, {320, 140});
    Access::Queue(*this->app_, this->windows_[1], WM_LBUTTONUP, 0, {360, 200});
    Access::PointerBusy(*this->app_, false, false);
    Access::Drain(*this->app_);
    const AnnotationSnapshot document = Access::State(*this->app_).Committed();
    ASSERT_NE(document, nullptr);
    ASSERT_EQ(document->size(), 1U);
    EXPECT_EQ(document->front().origin.x, 320.0);
    EXPECT_EQ(document->front().origin.y, 140.0);
}
} // namespace
