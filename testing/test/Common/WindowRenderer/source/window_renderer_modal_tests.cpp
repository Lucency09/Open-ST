// 验证通用模态窗口的消息钩子、宿主状态恢复、关闭顺序与异常回收。

#include "window_renderer.h"
#include <commctrl.h>
#include <gtest/gtest.h>
#include <stdexcept>

namespace
{
// 构造用于通用模态窗口测试的无页签提示文档。
// 入参：无显式入参。
// 返回：只有正文和关闭按钮、没有页签的合法 JSON 布局文档。
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
    // 入参：无显式入参。
    // 返回：无返回值。
    void Prepare()
    {
        ASSERT_TRUE(this->renderer_.LoadLayout(PlainDocument()));
        ASSERT_TRUE(
            // 将测试文本键转为可显示文本，避免加载业务本地化资源。
            // 入参：key 为待查询的测试界面文本键。
            // 返回：将 key 的字符逐个扩展为 wchar_t 得到的测试文本。
            this->renderer_.SetTextResolver([](std::string_view key) { return std::wstring(key.begin(), key.end()); }));
        // 让关闭按钮请求结束当前模态窗口。
        // 入参：无显式入参。
        // 返回：无返回值。
        ASSERT_TRUE(this->renderer_.BindAction("close", [this]() { this->renderer_.RequestClose(); }));
        // 将系统关闭动作转交当前窗口的关闭流程。
        // 入参：无显式入参。
        // 返回：无返回值。
        ASSERT_TRUE(this->renderer_.SetCloseHandler([this]() { this->renderer_.RequestClose(); }));
        ASSERT_TRUE(this->renderer_.SetDefaultAction("close"));
    }
    // 先确保线程消息队列存在，再投递 hook 测试消息。
    // 入参：无显式入参。
    // 返回：无返回值。
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
    // 为模态窗口测试创建可观察启停状态的原生宿主窗口。
    // 入参：无显式入参。
    // 返回：无返回值。
    TestOwner()
    {
        this->window = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED, 0, 0, 100, 100, nullptr, nullptr,
                                       GetModuleHandleW(nullptr), nullptr);
    }
    // 断言提前退出时也销毁测试窗口。
    // 入参：无显式入参。
    // 返回：无返回值。
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
// 入参：window 为被观察模态窗口；message 为消息编号；wParam、lParam 为消息附加数据；subclassId 为子类标识；reference 为借用的销毁探针指针。
// 返回：DefSubclassProc 对该消息的处理结果。
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

// 验证正常关闭先恢复 owner 再销毁模态窗口；隐藏模态窗口不得改变其他窗口的前台状态。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
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
                                          // 接收测试消息后安装销毁探针并关闭，记录关闭前的前台窗口。
                                          // 入参：message 为模态循环当前取得的线程消息，按测试消息编号决定是否消费。
                                          // 返回：测试消息已消费时为 true；其他消息为 false，交由模态循环继续处理。
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
// 验证顶层 content 不创建页签，文本刷新不查询虚构的页面标题键。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
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
// 验证pages 与 content 必须恰好存在一种，拒绝同时声明或同时缺失。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(ModalRendererTest, content_and_pages_are_exclusive)
{
    nlohmann::json document = PlainDocument();
    document["pages"] = nlohmann::json::array();
    EXPECT_EQ(this->renderer_.LoadLayout(document).code, "invalid_content_mode");
    document.erase("pages");
    document.erase("content");
    EXPECT_EQ(this->renderer_.LoadLayout(document).code, "invalid_content_mode");
}
// 验证模态循环转发无 HWND 线程消息，关闭正常返回且只恢复原启用的 owner。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
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
                                                                     // 验证宿主在模态期间被禁用，再消费测试消息并请求关闭。
                                                                     // 入参：message 为模态循环当前取得的线程消息，按测试消息编号决定是否消费。
                                                                     // 返回：测试消息已消费时为 true；其他消息为 false，交由模态循环继续处理。
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
// 验证owner 进入前已禁用时，模态结束不可误启用。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
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
                                          // 消费测试消息并关闭模态窗口，供验证原本禁用的宿主保持禁用。
                                          // 入参：message 为模态循环当前取得的线程消息，按测试消息编号决定是否消费。
                                          // 返回：测试消息已消费时为 true；其他消息为 false，交由模态循环继续处理。
                                          [this](MSG& message)
                                          {
                                              if (message.message != WM_APP + 777)
                                                  return false;
                                              this->renderer_.RequestClose();
                                              return true;
                                          }));
    EXPECT_FALSE(IsWindowEnabled(owner.window));
}
// 验证嵌套消息循环遇到 WM_QUIT 必须关闭窗口并原码重投，供外层结束运行。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
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
// 验证宿主线程 hook 异常转换为结构化失败，恢复 owner 并回收模态窗口。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
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
                                  // 仅在测试消息中抛出异常，检查模态边界的失败转换与资源回收。
                                  // 入参：message 为模态循环当前取得的线程消息，按测试消息编号决定是否消费。
                                  // 返回：非测试消息返回 false；测试消息主动抛出异常。
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
// 验证创建阶段的文本回调失败同样恢复被临时禁用的 owner。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(ModalRendererTest, modal_creation_failure_restores_owner)
{
    ASSERT_TRUE(this->renderer_.LoadLayout(PlainDocument()));
    // 在文本查询时注入异常，验证创建失败仍恢复宿主。
    // 入参：未命名 std::string_view 为文本查询键，本故障注入回调不使用该值。
    // 返回：不正常返回；主动抛出测试异常，交由被测边界处理。
    ASSERT_TRUE(this->renderer_.SetTextResolver([](std::string_view) -> std::wstring
                                                { throw std::runtime_error("fake text failure"); }));
    // 提供无副作用的关闭按钮绑定，使故障集中在文本查询阶段。
    // 入参：无显式入参。
    // 返回：无返回值。
    ASSERT_TRUE(this->renderer_.BindAction("close", []() {}));
    // 提供无副作用的系统关闭回调，满足窗口创建所需绑定。
    // 入参：无显式入参。
    // 返回：无返回值。
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
