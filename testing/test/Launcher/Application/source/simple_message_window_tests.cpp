#include "simple_message_window.h"
#include <array>
#include <gtest/gtest.h>
#include <stdexcept>
#include <string>
#include <utility>
#include <window_renderer.h>

namespace open_st
{
namespace
{
struct TextSearch final
{
    std::wstring expected;
    HWND found{};
};

// 只检查当前测试创建的窗口子控件文字。
BOOL CALLBACK FindText(HWND control, LPARAM parameter)
{
    TextSearch& search = *reinterpret_cast<TextSearch*>(parameter);
    std::array<wchar_t, 256> text{};
    GetWindowTextW(control, text.data(), static_cast<int>(text.size()));
    if (search.expected == text.data())
    {
        search.found = control;
        return FALSE;
    }
    return TRUE;
}

// 从借用父窗读取控件，避免跨测试搜索其他窗口。
HWND ChildWithText(HWND parent, std::wstring text)
{
    TextSearch search{std::move(text), nullptr};
    EnumChildWindows(parent, FindText, reinterpret_cast<LPARAM>(&search));
    return search.found;
}

// 运行期刷新标题、正文和确认按钮，结束后清空宿主借用指针。
TEST(SimpleMessageWindowTest, language_refresh_updates_every_visible_text)
{
    MSG queued{};
    (void)PeekMessageW(&queued, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    ASSERT_TRUE(PostThreadMessageW(GetCurrentThreadId(), WM_APP + 105, 0, 0));
    bool chinese = false;
    bool visited = false;
    WindowRenderer* active = nullptr;
    const bool shown = TryShowSimpleMessageWindow(
        nullptr, nullptr, [&chinese]() { return chinese ? L"新标题" : L"Old title"; }, [&chinese]()
        { return chinese ? L"新正文" : L"Old body"; }, [&chinese]() { return chinese ? L"确定" : L"OK"; }, active,
        [&chinese, &visited, &active](MSG& message)
        {
            if (message.message != WM_APP + 105)
                return false;
            visited = true;
            EXPECT_NE(active, nullptr);
            if (active == nullptr)
            {
                PostQuitMessage(98);
                return true;
            }
            const HWND window = active->NativeHandle();
            EXPECT_NE(ChildWithText(window, L"Old body"), nullptr);
            EXPECT_NE(ChildWithText(window, L"OK"), nullptr);
            chinese = true;
            EXPECT_TRUE(active->RefreshTexts());
            std::array<wchar_t, 256> title{};
            GetWindowTextW(window, title.data(), static_cast<int>(title.size()));
            EXPECT_STREQ(title.data(), L"新标题");
            EXPECT_NE(ChildWithText(window, L"新正文"), nullptr);
            EXPECT_NE(ChildWithText(window, L"确定"), nullptr);
            EXPECT_EQ(ChildWithText(window, L"Old body"), nullptr);
            EXPECT_TRUE(active->RequestClose());
            return true;
        });
    EXPECT_TRUE(shown);
    EXPECT_TRUE(visited);
    EXPECT_EQ(active, nullptr);
}

// 文本回调异常导致创建失败时同样不留下悬空借用指针。
TEST(SimpleMessageWindowTest, text_failure_clears_borrowed_renderer)
{
    WindowRenderer* active = nullptr;
    EXPECT_FALSE(TryShowSimpleMessageWindow(
        nullptr, nullptr, []() -> std::wstring { throw std::runtime_error("text unavailable"); },
        []() { return L"Body"; }, []() { return L"OK"; }, active, {}));
    EXPECT_EQ(active, nullptr);
}
} // namespace
} // namespace open_st
