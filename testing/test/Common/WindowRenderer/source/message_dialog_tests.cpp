// 验证公共消息窗口的确认、取消、错误与活动借用恢复。
#include <gtest/gtest.h>
#include <message_dialog.h>
#include <stdexcept>
#include <window_renderer.h>

namespace
{
// 为消息窗口提供不依赖产品本地化的文本回调。
// 入参：无。
// 返回：有效的通用消息选项。
open_st::MessageDialogOptions Options()
{
    open_st::MessageDialogOptions options;
    // 提供测试标题。
    // 入参：无。
    // 返回：固定标题。
    options.title = []() { return L"Dialog test"; };
    // 提供测试正文。
    // 入参：无。
    // 返回：固定正文。
    options.message = []() { return L"Message"; };
    // 提供接受按钮文本。
    // 入参：无。
    // 返回：固定文本。
    options.acceptText = []() { return L"Accept"; };
    // 提供取消按钮文本。
    // 入参：无。
    // 返回：固定文本。
    options.cancelText = []() { return L"Cancel"; };
    return options;
}

// 通过原生控件文字找接受按钮并发送真实按钮消息。
// 入参：window：当前子窗口；忽略的 LPARAM 不携带业务状态。
// 返回：接受按钮已点击后停止枚举。
BOOL CALLBACK AcceptButton(HWND window, LPARAM)
{
    wchar_t text[64]{};
    GetWindowTextW(window, text, 64);
    if (std::wstring_view(text) != L"Accept")
        return TRUE;
    SendMessageW(window, BM_CLICK, 0, 0);
    return FALSE;
}

// 验证明确点击接受才返回 Accepted，退出清除活动借用。
// 入参：无。
// 返回：GoogleTest 断言结果。
TEST(MessageDialogTest, confirmation_accepts_explicit_button_and_releases_borrow)
{
    auto options = Options();
    options.confirmation = true;
    open_st::WindowRenderer* active{};
    options.activeRenderer = &active;
    MSG seed{};
    PeekMessageW(&seed, nullptr, 0, 0, PM_NOREMOVE);
    ASSERT_TRUE(PostThreadMessageW(GetCurrentThreadId(), WM_APP + 778, 0, 0));
    // 在真实模态循环中点击接受控件。
    // 入参：message：线程消息。
    // 返回：测试消息被消费时 true。
    options.processThreadMessage = [&active](MSG& message)
    {
        if (message.message != WM_APP + 778)
            return false;
        EXPECT_NE(active, nullptr);
        if (active != nullptr)
            EnumChildWindows(active->NativeHandle(), AcceptButton, 0);
        return true;
    };
    EXPECT_EQ(open_st::ShowMessageDialog(options), open_st::MessageDialogResult::Accepted);
    EXPECT_EQ(active, nullptr);
}

// 验证标题栏关闭属于取消，并在嵌套提示返回时恢复原活动窗口借用。
// 入参：无。
// 返回：GoogleTest 断言结果。
TEST(MessageDialogTest, system_close_cancels_and_restores_outer_borrow)
{
    auto options = Options();
    options.confirmation = true;
    open_st::WindowRenderer outer;
    open_st::WindowRenderer* active = &outer;
    options.activeRenderer = &active;
    MSG seed{};
    PeekMessageW(&seed, nullptr, 0, 0, PM_NOREMOVE);
    ASSERT_TRUE(PostThreadMessageW(GetCurrentThreadId(), WM_APP + 779, 0, 0));
    // 在模态消息边界触发系统关闭。
    // 入参：message：线程消息。
    // 返回：测试消息被消费时 true。
    options.processThreadMessage = [&active, &outer](MSG& message)
    {
        if (message.message != WM_APP + 779)
            return false;
        EXPECT_NE(active, &outer);
        if (active != nullptr)
            SendMessageW(active->NativeHandle(), WM_CLOSE, 0, 0);
        return true;
    };
    EXPECT_EQ(open_st::ShowMessageDialog(options), open_st::MessageDialogResult::Cancelled);
    EXPECT_EQ(active, &outer);
}

// 验证缺少回调和回调抛异常都返回 Failed，不留下悬空活动指针。
// 入参：无。
// 返回：GoogleTest 断言结果。
TEST(MessageDialogTest, invalid_or_throwing_text_fails_without_dangling_borrow)
{
    auto options = Options();
    open_st::WindowRenderer* active{};
    options.activeRenderer = &active;
    options.confirmation = true;
    options.cancelText = {};
    EXPECT_EQ(open_st::ShowMessageDialog(options), open_st::MessageDialogResult::Failed);
    EXPECT_EQ(active, nullptr);
    options.confirmation = false;
    // 模拟文本服务异常。
    // 入参：无。
    // 返回：不返回，异常由公共窗口边界捕获。
    options.title = []() -> std::wstring { throw std::runtime_error("test text failure"); };
    EXPECT_EQ(open_st::ShowMessageDialog(options), open_st::MessageDialogResult::Failed);
    EXPECT_EQ(active, nullptr);
}
} // namespace
