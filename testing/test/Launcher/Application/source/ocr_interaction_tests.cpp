// 使用真实结果窗口与会话编排、可控 OCR 边界验证输入归属和迟到结果，不识别或写真实剪贴板。
#include "ocr_result_window.h"
#include "ocr_session.h"
#include <clipboard_writer.h>
#include <gtest/gtest.h>
#include <ocr_client.h>
#include <sdr_selection_frame.h>
#include <ui_text.h>
#include <vector>

namespace
{
struct OcrBoundaryState
{
    open_st::OcrSnapshot snapshot;
    open_st::OcrOptions options;
    int creates{};
    int destroys{};
    int submissions{};
    int cancellations{};
    int copies{};
    bool stopping{};
    bool stopped{};
    bool copySucceeds{true};
    std::wstring copied;
};
OcrBoundaryState* boundary{};
} // namespace

namespace open_st
{
// 独立链接替身只拥有空实现，真实后台线程由 OCR 模块专项覆盖。
struct OcrClient::Impl
{
};
// 建立可控任务边界，不加载模型或启动后台线程。
// 入参：root 和 notify 为真实协调器传入的资源路径及通知。
// 返回：未提交状态。
OcrClient::OcrClient(std::filesystem::path root, std::function<void(std::uint64_t)> notify)
    : impl_(std::make_unique<Impl>())
{
    (void)root;
    (void)notify;
    ++boundary->creates;
}
// 记录客户端释放，检验关闭窗口不会提前析构工作所有者。
// 入参：无。
// 返回：无。
OcrClient::~OcrClient()
{
    ++boundary->destroys;
}
// 模拟单任务准入，故意保持任务运行至测试明确发布结果。
// 入参：image/options 为真实会话参数；requestId/error 为输出。
// 返回：忙或停止时拒绝，其他情况接受。
bool OcrClient::Submit(OcrImageView image, const OcrOptions& options, std::uint64_t& requestId,
                       OcrError& error) noexcept
{
    requestId = 0;
    error = OcrError::None;
    ++boundary->submissions;
    if (boundary->snapshot.busy || boundary->stopping)
    {
        error = boundary->stopping ? OcrError::Unavailable : OcrError::Busy;
        return false;
    }
    if (image.width != 2 || image.height != 2 || image.stride != 8 || image.pixels.size() != 16)
    {
        error = OcrError::InvalidInput;
        return false;
    }
    boundary->options = options;
    ++boundary->snapshot.requestId;
    ++boundary->snapshot.revision;
    boundary->snapshot.phase = OcrPhase::Preparing;
    boundary->snapshot.busy = true;
    boundary->snapshot.text.reset();
    requestId = boundary->snapshot.requestId;
    return true;
}
// 保持 busy，模拟取消不能立即中断的模型初始化。
// 入参：无。
// 返回：无，只撤销可发布正文。
void OcrClient::Cancel() noexcept
{
    ++boundary->cancellations;
    boundary->snapshot.phase = OcrPhase::Cancelled;
    boundary->snapshot.text.reset();
    ++boundary->snapshot.revision;
}
// 请求退出但不虚构后台已完成。
// 入参：无。
// 返回：无。
void OcrClient::RequestShutdown() noexcept
{
    boundary->stopping = true;
    this->Cancel();
}
// 返回测试明确控制的释放状态。
// 入参：无。
// 返回：后台释放完成时为 true。
bool OcrClient::ShutdownComplete() const noexcept
{
    return boundary->stopped;
}
// 提供不可变正文及阶段快照，供真实会话执行代次检查。
// 入参：无。
// 返回：当前测试快照。
OcrSnapshot OcrClient::Snapshot() const
{
    return boundary->snapshot;
}
// 把文本键直接用作稳定测试文案，不读取真实用户资源。
// 入参：key 为文本键；arguments 未使用。
// 返回：文本键的宽字符形式。
std::wstring GetUiText(std::string_view key, std::initializer_list<UiTextArgument> arguments)
{
    (void)arguments;
    return std::wstring(key.begin(), key.end());
}
// 替换系统剪贴板边界，仅记录结果窗口传入的当前草稿。
// 入参：owner 为结果 HWND；text 为当前 UTF-16；error 接收测试错误。
// 返回：测试指定的发布结果。
bool CopyTextToClipboard(HWND owner, std::wstring_view text, std::wstring& error)
{
    EXPECT_TRUE(IsWindow(owner));
    ++boundary->copies;
    boundary->copied = text;
    error = boundary->copySucceeds ? L"" : L"Injected clipboard failure";
    return boundary->copySucceeds;
}
} // namespace open_st

namespace
{
struct WindowSearch
{
    HWND owner{};
    HWND found{};
};
// 按本测试独有 owner 查找实际 Renderer，避免误操作其他桌面窗口。
// 入参：window 为当前线程窗口；data 指向查找状态。
// 返回：找到后停止枚举。
BOOL CALLBACK FindResult(HWND window, LPARAM data)
{
    auto& search = *reinterpret_cast<WindowSearch*>(data);
    wchar_t type[64]{};
    GetClassNameW(window, type, 64);
    if (GetWindow(window, GW_OWNER) == search.owner && std::wstring_view(type) == L"OpenST.WindowRenderer")
    {
        search.found = window;
        return FALSE;
    }
    return TRUE;
}

class OcrInteractionTest : public testing::Test
{
  protected:
    OcrBoundaryState state_;
    HWND owner_{};
    std::unique_ptr<open_st::OcrSession> session_;
    open_st::SdrSelectionFrame frame_{{0, 0, 2, 2}, std::vector<std::uint8_t>(16, 255)};

    // 创建隐藏稳定宿主及真实协调器。
    // 入参：无。
    // 返回：无；失败由断言记录。
    void SetUp() override
    {
        boundary = &this->state_;
        this->owner_ = CreateWindowExW(0, L"STATIC", L"OCR integration owner", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                                       GetModuleHandleW(nullptr), nullptr);
        ASSERT_NE(this->owner_, nullptr);
        this->session_ = std::make_unique<open_st::OcrSession>(this->owner_, nullptr);
    }
    // 先停止边界并关闭会话，随后销毁稳定 HWND。
    // 入参：无。
    // 返回：无；不留下窗口、计时器或后台任务。
    void TearDown() override
    {
        this->state_.snapshot.busy = false;
        this->state_.stopped = true;
        this->session_->Shutdown();
        this->Pump();
        this->session_.reset();
        DestroyWindow(this->owner_);
        boundary = nullptr;
    }
    // 查找本会话窗口，窗口仍以真实公共组件创建。
    // 入参：无。
    // 返回：借用句柄或空。
    HWND Window() const
    {
        WindowSearch search{this->owner_};
        EnumThreadWindows(GetCurrentThreadId(), FindResult, reinterpret_cast<LPARAM>(&search));
        return search.found;
    }
    // 定位结果编辑框，不依赖生成控件编号。
    // 入参：无。
    // 返回：借用编辑框 HWND。
    HWND Edit() const
    {
        const HWND viewport = FindWindowExW(this->Window(), nullptr, L"OpenST.WindowRendererPage", nullptr);
        return FindWindowExW(viewport, nullptr, L"EDIT", nullptr);
    }
    // 查找确定测试文字的底部按钮或状态。
    // 入参：type 为原生类；text 为稳定文案。
    // 返回：根窗口直属控件。
    HWND Control(const wchar_t* type, const wchar_t* text) const
    {
        return FindWindowExW(this->Window(), nullptr, type, text);
    }
    // 读取实际可见编辑文本。
    // 入参：无。
    // 返回：独立 UTF-16 文本。
    std::wstring Value() const
    {
        std::wstring text(static_cast<std::size_t>(GetWindowTextLengthW(this->Edit())) + 1, L'\0');
        text.resize(static_cast<std::size_t>(GetWindowTextW(this->Edit(), text.data(), static_cast<int>(text.size()))));
        return text;
    }
    // 有界泵送原生消息，包括延迟关闭；不等待识别线程。
    // 入参：无。
    // 返回：无。
    void Pump()
    {
        MSG message{};
        for (int index = 0; index < 256 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++index)
        {
            if (message.message != WM_QUIT && !this->session_->Process(message))
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
    }
    // 开始并立即隐藏真实结果窗口，避免让自动化窗口停留桌面。
    // 入参：无。
    // 返回：无；窗口不能创建时为测试失败。
    void Begin()
    {
        ASSERT_NO_THROW(this->session_->Begin(this->frame_, "best", "eng"));
        ASSERT_NE(this->Window(), nullptr);
        ShowWindow(this->Window(), SW_HIDE);
    }
    // 发布一次成功快照，真实协调器负责填写草稿。
    // 入参：text 为模拟识别结果。
    // 返回：无。
    void Succeed(std::wstring text)
    {
        this->state_.snapshot.phase = open_st::OcrPhase::Succeeded;
        this->state_.snapshot.busy = false;
        this->state_.snapshot.text = std::make_shared<const std::wstring>(std::move(text));
        ++this->state_.snapshot.revision;
        this->session_->Poll();
    }
    // 模拟可撤销的用户文本编辑，触发真实 EN_CHANGE。
    // 入参：text 为替换文本。
    // 返回：无。
    void Replace(const wchar_t* text)
    {
        SendMessageW(this->Edit(), EM_SETSEL, 0, -1);
        SendMessageW(this->Edit(), EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(text));
    }
};

// 验证公共表单创建、固定参数传递以及同一成功快照不会覆盖修改后的草稿。
// 入参：无。
// 返回：无；使用实际 Renderer 和 EN_CHANGE。
TEST_F(OcrInteractionTest, completed_snapshot_is_consumed_once_and_copy_uses_edited_draft)
{
    this->Begin();
    ASSERT_NE(this->Window(), nullptr);
    EXPECT_FALSE(IsWindowEnabled(this->Edit()));
    EXPECT_EQ(this->state_.options.model, "best");
    EXPECT_EQ(this->state_.options.language, "eng");
    this->Succeed(L"原文\nsecond");
    EXPECT_EQ(this->Value(), L"原文\r\nsecond");
    EXPECT_TRUE(IsWindowEnabled(this->Edit()));
    this->Replace(L"编辑\r\nchanged");
    this->session_->Poll();
    this->session_->Poll();
    EXPECT_EQ(this->Value(), L"编辑\r\nchanged");
    SendMessageW(this->Control(L"BUTTON", L"ocr.copy_all"), BM_CLICK, 0, 0);
    EXPECT_EQ(this->state_.copies, 1);
    EXPECT_EQ(this->state_.copied, L"编辑\r\nchanged");
    SendMessageW(this->Edit(), WM_UNDO, 0, 0);
    EXPECT_EQ(this->Value(), L"原文\r\nsecond");
}

// 验证剪贴板失败不关闭结果窗口、不清空正文，也不破坏撤销。
// 入参：无。
// 返回：无；剪贴板边界由替身接收。
TEST_F(OcrInteractionTest, copy_failure_preserves_window_draft_and_undo)
{
    this->Begin();
    ASSERT_NE(this->Window(), nullptr);
    this->Succeed(L"original");
    this->Replace(L"changed");
    this->state_.copySucceeds = false;
    SendMessageW(this->Control(L"BUTTON", L"ocr.copy_all"), BM_CLICK, 0, 0);
    EXPECT_EQ(this->state_.copies, 1);
    EXPECT_EQ(this->Value(), L"changed");
    EXPECT_NE(this->Control(L"STATIC", L"ocr.copy_failed"), nullptr);
    SendMessageW(this->Edit(), WM_UNDO, 0, 0);
    EXPECT_EQ(this->Value(), L"original");
}

// 验证取消只关闭当前窗口，不能销毁不可中断的任务所有者，也不能发布迟到正文。
// 入参：无。
// 返回：无；直到明确完成前，重新提交必须拒绝忙状态。
TEST_F(OcrInteractionTest, close_keeps_client_alive_and_busy_reopen_does_not_accept_late_result)
{
    this->Begin();
    ASSERT_NE(this->Window(), nullptr);
    SendMessageW(this->Window(), WM_CLOSE, 0, 0);
    this->Pump();
    EXPECT_EQ(this->Window(), nullptr);
    EXPECT_GT(this->state_.cancellations, 0);
    EXPECT_EQ(this->state_.destroys, 0);
    EXPECT_TRUE(this->state_.snapshot.busy);
    this->Begin();
    ASSERT_NE(this->Window(), nullptr);
    EXPECT_EQ(this->state_.creates, 1);
    EXPECT_NE(this->Control(L"STATIC", L"ocr.busy"), nullptr);
    this->Succeed(L"late text");
    EXPECT_EQ(this->Value(), L"");
    EXPECT_FALSE(IsWindowEnabled(this->Edit()));
}

// 验证会话结束撤销发布，退出释放状态不由窗口有无推测。
// 入参：无。
// 返回：无；迟到阶段不能恢复已经结束的窗口。
TEST_F(OcrInteractionTest, end_capture_and_shutdown_wait_for_explicit_background_completion)
{
    this->Begin();
    ASSERT_NE(this->Window(), nullptr);
    this->session_->EndCapture();
    this->Pump();
    EXPECT_EQ(this->Window(), nullptr);
    this->Succeed(L"obsolete");
    EXPECT_EQ(this->Window(), nullptr);
    this->session_->Shutdown();
    EXPECT_TRUE(this->state_.stopping);
    EXPECT_FALSE(this->session_->ShutdownComplete());
    this->state_.stopped = true;
    EXPECT_TRUE(this->session_->ShutdownComplete());
}

// 验证结果编辑 Enter 归多行输入，后台注册热键不能偷偷复制或修改正文。
// 入参：无。
// 返回：无；不发送真实桌面按键。
TEST_F(OcrInteractionTest, multiline_enter_and_background_registered_hotkeys_keep_input_ownership)
{
    this->Begin();
    ASSERT_NE(this->Window(), nullptr);
    this->Succeed(L"text");
    SetFocus(this->Edit());
    ASSERT_EQ(GetFocus(), this->Edit());
    MSG message{};
    message.hwnd = this->Edit();
    message.message = WM_KEYDOWN;
    message.wParam = VK_RETURN;
    EXPECT_FALSE(this->session_->Process(message));
    SendMessageW(this->Edit(), WM_CHAR, VK_RETURN, 0);
    EXPECT_NE(this->Value().find(L"\r\n"), std::wstring::npos);
    EXPECT_EQ(this->state_.cancellations, 0);
    // 隐藏窗口也可能被 SetFocus 激活；先恢复可见再隐藏，确保系统真正执行停用转换。
    ShowWindow(this->Window(), SW_SHOWNOACTIVATE);
    ShowWindow(this->Window(), SW_HIDE);
    this->Pump();
    ASSERT_NE(GetForegroundWindow(), this->Window());
    EXPECT_FALSE(this->session_->RegisteredHotkey(MOD_CONTROL, 'C'));
    EXPECT_EQ(this->state_.copies, 0);
    EXPECT_NE(this->Window(), nullptr);
}

// 验证模态暂停仅禁用结果窗口输入，完成通知不能提前解除暂停，恢复后保留编辑撤销。
// 入参：无。
// 返回：无；不写真实剪贴板或取消识别任务。
TEST_F(OcrInteractionTest, modal_pause_survives_completion_and_preserves_draft_undo)
{
    this->Begin();
    ASSERT_NE(this->Window(), nullptr);
    this->session_->Pause(true);
    EXPECT_FALSE(IsWindowEnabled(this->Window()));
    this->Succeed(L"original");
    EXPECT_FALSE(IsWindowEnabled(this->Window()));
    EXPECT_EQ(this->Value(), L"original");
    EXPECT_EQ(this->state_.cancellations, 0);
    this->session_->Pause(false);
    EXPECT_TRUE(IsWindowEnabled(this->Window()));
    this->Replace(L"changed");
    this->session_->Pause(true);
    (void)this->session_->RegisteredHotkey(MOD_CONTROL, 'C');
    EXPECT_EQ(this->state_.copies, 0);
    this->session_->Pause(false);
    EXPECT_EQ(this->Value(), L"changed");
    SendMessageW(this->Edit(), WM_UNDO, 0, 0);
    EXPECT_EQ(this->Value(), L"original");
}

// 验证关闭空闲结果窗后停止轮询，不因缓存客户端工作线程而永久产生定时消息。
// 入参：无。
// 返回：无；窗口关闭不释放仍归应用持有的客户端。
TEST_F(OcrInteractionTest, closing_completed_result_stops_poll_timer_without_destroying_client)
{
    this->Begin();
    ASSERT_NE(this->Window(), nullptr);
    this->Succeed(L"finished");
    EXPECT_FALSE(this->session_->ShutdownComplete());
    SendMessageW(this->Window(), WM_CLOSE, 0, 0);
    this->Pump();
    this->session_->Poll();
    EXPECT_EQ(this->Window(), nullptr);
    EXPECT_EQ(this->state_.destroys, 0);
    EXPECT_EQ(KillTimer(this->owner_, open_st::OcrPollTimer), FALSE);
}
} // namespace
