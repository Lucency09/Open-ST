// 验证通用业务提示窗口的运行期文本刷新、模态退出和异常回收。

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
// 入参：control 为当前枚举到的子控件句柄；parameter 为借用的查找条件及结果结构指针。
// 返回：找到目标控件时返回 FALSE 停止枚举；未匹配时返回 TRUE 继续枚举。
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
// 入参：parent 为限定查找范围的父窗口；text 为待匹配的控件文本。
// 返回：匹配控件或窗口的借用句柄；未找到时为 nullptr，调用方不取得销毁责任。
HWND ChildWithText(HWND parent, std::wstring text)
{
    TextSearch search{std::move(text), nullptr};
    EnumChildWindows(parent, FindText, reinterpret_cast<LPARAM>(&search));
    return search.found;
}

// 验证运行期刷新标题、正文和确认按钮，结束后清空宿主借用指针。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST(SimpleMessageWindowTest, language_refresh_updates_every_visible_text)
{
    MSG queued{};
    (void)PeekMessageW(&queued, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    ASSERT_TRUE(PostThreadMessageW(GetCurrentThreadId(), WM_APP + 105, 0, 0));
    bool chinese = false;
    bool visited = false;
    WindowRenderer* active = nullptr;
    const bool shown = TryShowSimpleMessageWindow(
        nullptr, nullptr,
        // 按模拟语言返回提示窗口标题。
        // 入参：无显式入参。
        // 返回：当前模拟语言对应的提示窗口标题。
        [&chinese]() { return chinese ? L"新标题" : L"Old title"; },
        // 按模拟语言返回提示窗口正文。
        // 入参：无显式入参。
        // 返回：当前模拟语言对应的提示窗口正文。
        [&chinese]() { return chinese ? L"新正文" : L"Old body"; },
        // 按模拟语言返回确认按钮文字。
        // 入参：无显式入参。
        // 返回：当前模拟语言对应的确认按钮文字。
        [&chinese]() { return chinese ? L"确定" : L"OK"; }, active,
        // 消费测试消息，切换语言并核验所有可见文本后关闭窗口。
        // 入参：message 为模态循环当前取得的线程消息，按测试消息编号决定是否消费。
        // 返回：测试消息已消费时为 true；其他消息为 false，交由模态循环继续处理。
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

// 验证置顶 owner 的提示保持在所属图像上方，模态关闭后恢复 owner 的可用状态。
// 入参：无运行入参；宏参数为测试注册名称。
// 返回：无返回值；通过真实 HWND 层级检查提示可达性，以消息驱动关闭而不等待真人。
TEST(SimpleMessageWindowTest, topmost_owner_keeps_message_above_image)
{
    const HWND owner = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"STATIC", L"Pin dialog owner", WS_POPUP, 60,
                                       60, 600, 400, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ASSERT_NE(owner, nullptr);
    struct OwnerScope final
    {
        HWND window;
        // 在断言或正常返回时销毁测试 owner。
        // 入参：无。
        // 返回：无返回值，句柄不再有效。
        ~OwnerScope()
        {
            DestroyWindow(this->window);
        }
    } ownerScope{owner};
    ShowWindow(owner, SW_SHOWNOACTIVATE);
    MSG queued{};
    (void)PeekMessageW(&queued, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    ASSERT_TRUE(PostThreadMessageW(GetCurrentThreadId(), WM_APP + 106, 0, 0));
    WindowRenderer* active = nullptr;
    bool visited = false;
    const bool shown = TryShowSimpleMessageWindow(
        owner, nullptr,
        // 提供隔离的测试标题。
        // 入参：无。
        // 返回：固定标题。
        []() { return L"Pinned image error"; },
        // 提供隔离的错误正文。
        // 入参：无。
        // 返回：固定正文。
        []() { return L"Test failure"; },
        // 提供确认按钮文字。
        // 入参：无。
        // 返回：固定按钮文字。
        []() { return L"OK"; }, active,
        // 在真实提示创建后核对所属关系和前后层级，再请求正常关闭。
        // 入参：message 为线程队列消息；借用 active、owner 和访问标记。
        // 返回：测试消息被消费时 true，其他消息交由正常模态循环。
        [&active, &visited, owner](MSG& message)
        {
            if (message.message != WM_APP + 106)
                return false;
            visited = true;
            EXPECT_NE(active, nullptr);
            if (!active)
            {
                PostQuitMessage(98);
                return true;
            }
            const HWND dialog = active->NativeHandle();
            EXPECT_EQ(GetWindow(dialog, GW_OWNER), owner);
            EXPECT_TRUE(IsWindowVisible(dialog));
            EXPECT_FALSE(IsWindowEnabled(owner));
            bool dialogBeforeOwner = false;
            for (HWND window = GetTopWindow(nullptr); window; window = GetWindow(window, GW_HWNDNEXT))
            {
                if (window == dialog)
                    dialogBeforeOwner = true;
                if (window == owner)
                    break;
            }
            EXPECT_TRUE(dialogBeforeOwner);
            EXPECT_TRUE(active->RequestClose());
            return true;
        });
    EXPECT_TRUE(shown);
    EXPECT_TRUE(visited);
    EXPECT_EQ(active, nullptr);
    EXPECT_TRUE(IsWindowEnabled(owner));
}

// 验证文本回调异常导致创建失败时同样不留下悬空借用指针。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST(SimpleMessageWindowTest, text_failure_clears_borrowed_renderer)
{
    WindowRenderer* active = nullptr;
    EXPECT_FALSE(TryShowSimpleMessageWindow(
        nullptr, nullptr,
        // 在标题查询中抛出异常，验证模态创建失败会解除宿主绑定。
        // 入参：无显式入参。
        // 返回：不正常返回；主动抛出测试异常，交由被测边界处理。
        []() -> std::wstring { throw std::runtime_error("text unavailable"); },
        // 提供固定正文，隔离标题查询故障。
        // 入参：无显式入参。
        // 返回：固定正文 Body。
        []() { return L"Body"; },
        // 提供固定确认文字，隔离标题查询故障。
        // 入参：无显式入参。
        // 返回：固定确认文字 OK。
        []() { return L"OK"; }, active, {}));
    EXPECT_EQ(active, nullptr);
}
} // namespace
} // namespace open_st
