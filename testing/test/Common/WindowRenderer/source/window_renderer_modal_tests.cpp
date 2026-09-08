#include "window_renderer.h"
#include <commctrl.h>
#include <gtest/gtest.h>
#include <stdexcept>

namespace
{
// 无页签的通用提示文档，业务内容由文本回调提供。
nlohmann::json PlainDocument()
{
    return nlohmann::json::parse(R"({"schemaVersion":1,"window":{"titleKey":"title"},
        "content":{"type":"column","id":"body","children":[{"type":"text","id":"message","textKey":"message"}]},
        "footer":{"trailing":[{"type":"button","id":"close","textKey":"close"}]}})");
}
class ModalRendererTest : public testing::Test
{
  protected:
    open_st::WindowRenderer renderer_;
    // 提供独立回调，关闭行为不依赖任何业务模块。
    void Prepare()
    {
        ASSERT_TRUE(this->renderer_.LoadLayout(PlainDocument()));
        ASSERT_TRUE(
            this->renderer_.SetTextResolver([](std::string_view key) { return std::wstring(key.begin(), key.end()); }));
        ASSERT_TRUE(this->renderer_.BindAction("close", [this]() { this->renderer_.RequestClose(); }));
        ASSERT_TRUE(this->renderer_.SetCloseHandler([this]() { this->renderer_.RequestClose(); }));
        ASSERT_TRUE(this->renderer_.SetDefaultAction("close"));
    }
    // 先确保线程消息队列存在，再投递 hook 测试消息。
    void PostHookMessage()
    {
        MSG message{};
        PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
        ASSERT_TRUE(PostThreadMessageW(GetCurrentThreadId(), WM_APP + 777, 0, 0));
    }
};
struct TestOwner
{
    HWND window{};
    // 系统 STATIC 类提供无需注册的测试 owner。
    TestOwner()
    {
        this->window = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED, 0, 0, 100, 100, nullptr, nullptr,
                                       GetModuleHandleW(nullptr), nullptr);
    }
    // 断言提前退出时也销毁测试窗口。
    ~TestOwner()
    {
        if (this->window != nullptr)
            DestroyWindow(this->window);
    }
};
struct CloseOrderProbe
{
    HWND owner{};
    bool destroyed{};
    bool ownerEnabledAtDestroy{};
};

// 在实际 WM_DESTROY 时检查 owner，避免只验证返回后的状态而漏掉关闭顺序。
LRESULT CALLBACK ObserveModalDestroy(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR subclassId,
                                     DWORD_PTR reference)
{
    CloseOrderProbe* probe = reinterpret_cast<CloseOrderProbe*>(reference);
    if (message == WM_DESTROY)
    {
        probe->destroyed = true;
        probe->ownerEnabledAtDestroy = IsWindowEnabled(probe->owner) != FALSE;
    }
    if (message == WM_NCDESTROY)
        RemoveWindowSubclass(window, ObserveModalDestroy, subclassId);
    return DefSubclassProc(window, message, wParam, lParam);
}

// 正常关闭先恢复 owner 再销毁模态窗口；隐藏模态窗口不得改变其他窗口的前台状态。
TEST_F(ModalRendererTest, modal_restores_owner_before_destroy_without_stealing_foreground)
{
    this->Prepare();
    TestOwner owner;
    ASSERT_NE(owner.window, nullptr);
    CloseOrderProbe probe{owner.window};
    this->PostHookMessage();
    open_st::RendererWindowOptions options;
    options.owner = owner.window;
    options.showCommand = SW_HIDE;
    HWND foregroundBeforeClose{};
    EXPECT_TRUE(this->renderer_.ShowModal(options,
                                          [this, &probe, &foregroundBeforeClose](MSG& message)
                                          {
                                              if (message.message != WM_APP + 777)
                                                  return false;
                                              EXPECT_TRUE(SetWindowSubclass(this->renderer_.NativeHandle(),
                                                                            ObserveModalDestroy, 1,
                                                                            reinterpret_cast<DWORD_PTR>(&probe)));
                                              foregroundBeforeClose = GetForegroundWindow();
                                              EXPECT_TRUE(this->renderer_.RequestClose());
                                              return true;
                                          }));
    EXPECT_TRUE(probe.destroyed);
    EXPECT_TRUE(probe.ownerEnabledAtDestroy);
    EXPECT_EQ(GetForegroundWindow(), foregroundBeforeClose);
}
// 顶层 content 不创建页签，文本刷新不查询虚构的页面标题键。
TEST_F(ModalRendererTest, plain_content_has_no_tabs)
{
    this->Prepare();
    open_st::RendererWindowOptions options;
    options.showCommand = SW_HIDE;
    ASSERT_TRUE(this->renderer_.Show(options));
    EXPECT_EQ(FindWindowExW(this->renderer_.NativeHandle(), nullptr, WC_TABCONTROLW, nullptr), nullptr);
    EXPECT_TRUE(this->renderer_.RefreshTexts());
    EXPECT_EQ(this->renderer_.GetActivePageId(), "");
}
// pages 与 content 必须恰好存在一种，拒绝同时声明或同时缺失。
TEST_F(ModalRendererTest, content_and_pages_are_exclusive)
{
    nlohmann::json document = PlainDocument();
    document["pages"] = nlohmann::json::array();
    EXPECT_EQ(this->renderer_.LoadLayout(document).code, "invalid_content_mode");
    document.erase("pages");
    document.erase("content");
    EXPECT_EQ(this->renderer_.LoadLayout(document).code, "invalid_content_mode");
}
// 模态循环转发无 HWND 线程消息，关闭正常返回且只恢复原启用的 owner。
TEST_F(ModalRendererTest, modal_close_restores_owner)
{
    this->Prepare();
    TestOwner owner;
    ASSERT_NE(owner.window, nullptr);
    this->PostHookMessage();
    open_st::RendererWindowOptions options;
    options.owner = owner.window;
    options.showCommand = SW_HIDE;
    bool handled = false;
    const open_st::RendererResult result = this->renderer_.ShowModal(options,
                                                                     [this, &owner, &handled](MSG& message)
                                                                     {
                                                                         if (message.message != WM_APP + 777)
                                                                             return false;
                                                                         handled = true;
                                                                         EXPECT_EQ(message.hwnd, nullptr);
                                                                         EXPECT_FALSE(IsWindowEnabled(owner.window));
                                                                         EXPECT_TRUE(this->renderer_.RequestClose());
                                                                         return true;
                                                                     });
    EXPECT_TRUE(result);
    EXPECT_TRUE(handled);
    EXPECT_TRUE(IsWindowEnabled(owner.window));
    EXPECT_EQ(this->renderer_.NativeHandle(), nullptr);
}
// owner 进入前已禁用时，模态结束不可误启用。
TEST_F(ModalRendererTest, modal_preserves_disabled_owner)
{
    this->Prepare();
    TestOwner owner;
    ASSERT_NE(owner.window, nullptr);
    EnableWindow(owner.window, FALSE);
    this->PostHookMessage();
    open_st::RendererWindowOptions options;
    options.owner = owner.window;
    options.showCommand = SW_HIDE;
    EXPECT_TRUE(this->renderer_.ShowModal(options,
                                          [this](MSG& message)
                                          {
                                              if (message.message != WM_APP + 777)
                                                  return false;
                                              this->renderer_.RequestClose();
                                              return true;
                                          }));
    EXPECT_FALSE(IsWindowEnabled(owner.window));
}
// 嵌套消息循环遇到 WM_QUIT 必须关闭窗口并原码重投，供外层结束运行。
TEST_F(ModalRendererTest, modal_reposts_quit_code)
{
    this->Prepare();
    TestOwner owner;
    ASSERT_NE(owner.window, nullptr);
    PostQuitMessage(73);
    open_st::RendererWindowOptions options;
    options.owner = owner.window;
    options.showCommand = SW_HIDE;
    EXPECT_TRUE(this->renderer_.ShowModal(options));
    EXPECT_EQ(this->renderer_.NativeHandle(), nullptr);
    EXPECT_TRUE(IsWindowEnabled(owner.window));
    MSG message{};
    ASSERT_TRUE(PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE));
    EXPECT_EQ(message.message, WM_QUIT);
    EXPECT_EQ(message.wParam, 73U);
}
// 宿主线程 hook 异常转换为结构化失败，恢复 owner 并回收模态窗口。
TEST_F(ModalRendererTest, modal_hook_exception_restores_owner)
{
    this->Prepare();
    TestOwner owner;
    ASSERT_NE(owner.window, nullptr);
    this->PostHookMessage();
    open_st::RendererWindowOptions options;
    options.owner = owner.window;
    options.showCommand = SW_HIDE;
    const open_st::RendererResult result =
        this->renderer_.ShowModal(options,
                                  [](MSG& message) -> bool
                                  {
                                      if (message.message == WM_APP + 777)
                                          throw std::runtime_error("fake hook failure");
                                      return false;
                                  });
    EXPECT_EQ(result.code, "callback_failed");
    EXPECT_TRUE(IsWindowEnabled(owner.window));
    EXPECT_EQ(this->renderer_.NativeHandle(), nullptr);
}
// 创建阶段的文本回调失败同样恢复被临时禁用的 owner。
TEST_F(ModalRendererTest, modal_creation_failure_restores_owner)
{
    ASSERT_TRUE(this->renderer_.LoadLayout(PlainDocument()));
    ASSERT_TRUE(this->renderer_.SetTextResolver([](std::string_view) -> std::wstring
                                                { throw std::runtime_error("fake text failure"); }));
    ASSERT_TRUE(this->renderer_.BindAction("close", []() {}));
    ASSERT_TRUE(this->renderer_.SetCloseHandler([]() {}));
    ASSERT_TRUE(this->renderer_.SetDefaultAction("close"));
    TestOwner owner;
    ASSERT_NE(owner.window, nullptr);
    open_st::RendererWindowOptions options;
    options.owner = owner.window;
    options.showCommand = SW_HIDE;
    EXPECT_FALSE(this->renderer_.ShowModal(options));
    EXPECT_TRUE(IsWindowEnabled(owner.window));
    EXPECT_EQ(this->renderer_.NativeHandle(), nullptr);
}
} // namespace
