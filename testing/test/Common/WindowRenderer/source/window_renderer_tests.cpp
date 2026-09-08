#include "window_renderer.h"
#include <commctrl.h>
#include <gtest/gtest.h>
#include <stdexcept>

namespace
{
// 创建两页和通用按钮，不引用 Settings、Common 或本地化模块。
nlohmann::json WindowDocument()
{
    return nlohmann::json::parse(R"({
      "schemaVersion":1,"window":{"titleKey":"title"},
      "pages":[
        {"id":"first","titleKey":"first","content":{"type":"column","id":"firstColumn","children":[
          {"type":"select","id":"choice","labelKey":"choice"}]}},
        {"id":"second","titleKey":"second","content":{"type":"column","id":"secondColumn","children":[
          {"type":"text","id":"info","textKey":"info"}]}}
      ],
      "footer":{"leading":[],"trailing":[
        {"type":"button","id":"confirm","textKey":"confirm"},
        {"type":"button","id":"cancel","textKey":"cancel"}]}
    })");
}
// 枚举查找原生下拉框，用真实父窗口路由通知。
BOOL CALLBACK FindCombo(HWND window, LPARAM context)
{
    wchar_t name[64]{};
    GetClassNameW(window, name, 64);
    if (wcscmp(name, L"ComboBox") == 0)
    {
        *reinterpret_cast<HWND*>(context) = window;
        return FALSE;
    }
    return TRUE;
}
// 清空当前线程已有消息，包含延迟关闭但不等待外部输入。
void PumpMessages()
{
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}
class RendererBindingTest : public testing::Test
{
  protected:
    open_st::WindowRenderer renderer_;
    std::string draft_ = "a";
    int changes_ = 0;
    int actions_ = 0;
    bool longText_ = false;
    bool throwChange_ = false;
    std::vector<open_st::RendererResult> errors_;
    // 注册可独立运行的草稿、选项和关闭动作。
    void Prepare(nlohmann::json document = WindowDocument())
    {
        ASSERT_TRUE(this->renderer_.LoadLayout(document));
        ASSERT_TRUE(this->renderer_.SetTextResolver(
            [this](std::string_view key)
            {
                return this->longText_ ? std::wstring(key == "longInfo" ? 2000 : 120, L'W')
                                       : std::wstring(key.begin(), key.end());
            }));
        ASSERT_TRUE(this->renderer_.BindString(
            "choice", [this]() { return open_st::RendererStringResult{true, this->draft_, {}}; },
            [this](std::string_view value)
            {
                if (this->throwChange_)
                    throw std::runtime_error("change failure");
                this->draft_ = value;
                ++this->changes_;
                return open_st::RendererChangeResult{};
            }));
        ASSERT_TRUE(this->renderer_.BindOptions(
            "choice", []() { return open_st::RendererOptionsResult{true, {{"a", L"A"}, {"b", L"B"}}, {}}; }));
        ASSERT_TRUE(this->renderer_.BindAction("confirm", [this]() { ++this->actions_; }));
        ASSERT_TRUE(this->renderer_.BindAction("cancel", [this]() { this->renderer_.RequestClose(); }));
        ASSERT_TRUE(this->renderer_.SetCloseHandler([this]() { this->renderer_.RequestClose(); }));
        ASSERT_TRUE(this->renderer_.SetDefaultAction("confirm"));
        ASSERT_TRUE(this->renderer_.SetErrorHandler([this](const open_st::RendererResult& error)
                                                    { this->errors_.push_back(error); }));
    }
    // 显示隐藏测试窗口，避免测试抢占前台。
    void Show()
    {
        open_st::RendererWindowOptions options;
        options.showCommand = SW_HIDE;
        ASSERT_TRUE(this->renderer_.Show(options));
        ASSERT_NE(this->renderer_.NativeHandle(), nullptr);
    }
    // 查找本实例控件，不依赖固定 Win32 数值 ID。
    HWND Combo()
    {
        HWND combo = nullptr;
        EnumChildWindows(this->renderer_.NativeHandle(), FindCombo, reinterpret_cast<LPARAM>(&combo));
        return combo;
    }
};
// 无布局或缺少窗口级与字段级回调时不可创建半成品窗口。
TEST_F(RendererBindingTest, requires_complete_bindings)
{
    EXPECT_FALSE(this->renderer_.ValidateBindings());
    ASSERT_TRUE(this->renderer_.LoadLayout(WindowDocument()));
    EXPECT_FALSE(this->renderer_.ValidateBindings());
    EXPECT_FALSE(this->renderer_.Show());
    EXPECT_EQ(this->renderer_.NativeHandle(), nullptr);
}
// 不存在 ID 和类型不匹配在注册时拒绝，而非延迟点击崩溃。
TEST_F(RendererBindingTest, rejects_unknown_and_wrong_type)
{
    ASSERT_TRUE(this->renderer_.LoadLayout(WindowDocument()));
    EXPECT_FALSE(this->renderer_.BindAction("missing", []() {}));
    EXPECT_FALSE(this->renderer_.BindAction("choice", []() {}));
    EXPECT_FALSE(this->renderer_.BindOptions("confirm", []() { return open_st::RendererOptionsResult{}; }));
}
// 同类型绑定不可覆盖，空函数也不能注册。
TEST_F(RendererBindingTest, rejects_duplicate_and_empty_callback)
{
    this->Prepare();
    EXPECT_FALSE(this->renderer_.BindAction("confirm", []() {}));
    EXPECT_FALSE(this->renderer_.BindOptions("choice", []() { return open_st::RendererOptionsResult{}; }));
    open_st::WindowRenderer other;
    ASSERT_TRUE(other.LoadLayout(WindowDocument()));
    EXPECT_FALSE(other.BindAction("confirm", {}));
    EXPECT_FALSE(other.BindString("choice", {}, {}));
}
// 一个下拉框的数据与选项属于不同槽，均注册后可以显示。
TEST_F(RendererBindingTest, accepts_distinct_binding_slots)
{
    this->Prepare();
    EXPECT_TRUE(this->renderer_.ValidateBindings());
    this->Show();
    EXPECT_EQ(this->renderer_.GetActivePageId(), "first");
    EXPECT_EQ(this->renderer_.GetControlPageId("choice"), "first");
    EXPECT_EQ(this->renderer_.GetControlPageId("confirm"), "");
}
// 显示回调抛异常时返回错误，异常不能越过窗口过程。
TEST_F(RendererBindingTest, catches_text_callback_exception)
{
    ASSERT_TRUE(this->renderer_.LoadLayout(WindowDocument()));
    ASSERT_TRUE(
        this->renderer_.SetTextResolver([](std::string_view) -> std::wstring { throw std::runtime_error("fake"); }));
    ASSERT_TRUE(this->renderer_.BindString(
        "choice", []() { return open_st::RendererStringResult{}; },
        [](std::string_view) { return open_st::RendererChangeResult{}; }));
    ASSERT_TRUE(this->renderer_.BindOptions("choice", []() { return open_st::RendererOptionsResult{}; }));
    ASSERT_TRUE(this->renderer_.BindAction("confirm", []() {}));
    ASSERT_TRUE(this->renderer_.BindAction("cancel", []() {}));
    ASSERT_TRUE(this->renderer_.SetCloseHandler([]() {}));
    ASSERT_TRUE(this->renderer_.SetDefaultAction("confirm"));
    EXPECT_FALSE(this->renderer_.Show());
    EXPECT_EQ(this->renderer_.NativeHandle(), nullptr);
}
// 程序刷新读取新的草稿但绝不触发修改回调。
TEST_F(RendererBindingTest, refresh_does_not_write_draft)
{
    this->Prepare();
    this->Show();
    this->draft_ = "b";
    ASSERT_TRUE(this->renderer_.RefreshValues());
    const HWND combo = this->Combo();
    ASSERT_NE(combo, nullptr);
    EXPECT_EQ(SendMessageW(combo, CB_GETCURSEL, 0, 0), 1);
    EXPECT_EQ(this->changes_, 0);
}
// 原生控件的真实父窗口接收通知后修改草稿一次。
TEST_F(RendererBindingTest, dispatches_selection_change)
{
    this->Prepare();
    this->Show();
    const HWND combo = this->Combo();
    ASSERT_NE(combo, nullptr);
    SendMessageW(combo, CB_SETCURSEL, 1, 0);
    SendMessageW(GetParent(combo), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(combo), CBN_SELCHANGE),
                 reinterpret_cast<LPARAM>(combo));
    EXPECT_EQ(this->draft_, "b");
    EXPECT_EQ(this->changes_, 1);
}
// 草稿不在选项中保持无选择，不能偷偷采用第一项。
TEST_F(RendererBindingTest, missing_option_preserves_draft)
{
    this->Prepare();
    this->draft_ = "missing";
    this->Show();
    const HWND combo = this->Combo();
    ASSERT_NE(combo, nullptr);
    EXPECT_EQ(SendMessageW(combo, CB_GETCURSEL, 0, 0), CB_ERR);
    EXPECT_EQ(this->draft_, "missing");
    EXPECT_EQ(this->changes_, 0);
}
// 关闭请求在当前分派结束后销毁窗口，不向线程投递退出。
TEST_F(RendererBindingTest, closes_after_message_dispatch)
{
    this->Prepare();
    this->Show();
    ASSERT_TRUE(this->renderer_.RequestClose());
    EXPECT_NE(this->renderer_.NativeHandle(), nullptr);
    PumpMessages();
    EXPECT_EQ(this->renderer_.NativeHandle(), nullptr);
}
// 忙状态解除后恢复宿主单项禁用状态，刷新不会解锁字段。
TEST_F(RendererBindingTest, restores_host_enabled_state_after_busy)
{
    this->Prepare();
    this->Show();
    ASSERT_TRUE(this->renderer_.SetEnabled("choice", false));
    ASSERT_TRUE(this->renderer_.SetBusy(true));
    ASSERT_TRUE(this->renderer_.SetBusy(false));
    const HWND combo = this->Combo();
    ASSERT_NE(combo, nullptr);
    EXPECT_FALSE(IsWindowEnabled(combo));
    EXPECT_EQ(this->changes_, 0);
}
// 原生页签通知切换页面身份，并隐藏上一页输入控件。
TEST_F(RendererBindingTest, switches_pages_with_native_notification)
{
    this->Prepare();
    this->Show();
    const HWND window = this->renderer_.NativeHandle();
    const HWND tabs = FindWindowExW(window, nullptr, WC_TABCONTROLW, nullptr);
    ASSERT_NE(tabs, nullptr);
    TabCtrl_SetCurSel(tabs, 1);
    NMHDR notification{tabs, static_cast<UINT_PTR>(GetDlgCtrlID(tabs)), TCN_SELCHANGE};
    SendMessageW(window, WM_NOTIFY, notification.idFrom, reinterpret_cast<LPARAM>(&notification));
    EXPECT_EQ(this->renderer_.GetActivePageId(), "second");
    EXPECT_EQ(GetWindowLongPtrW(this->Combo(), GWL_STYLE) & WS_VISIBLE, 0);
    TabCtrl_SetCurSel(tabs, 0);
    SendMessageW(window, WM_NOTIFY, notification.idFrom, reinterpret_cast<LPARAM>(&notification));
    EXPECT_EQ(this->renderer_.GetActivePageId(), "first");
    EXPECT_NE(GetWindowLongPtrW(this->Combo(), GWL_STYLE) & WS_VISIBLE, 0);
}
// 展开的下拉框优先消费 Enter/Esc，不得触发默认动作或关闭窗口。
TEST_F(RendererBindingTest, dropped_combo_keeps_enter_escape_local)
{
    this->Prepare();
    this->Show();
    const HWND window = this->renderer_.NativeHandle();
    ShowWindow(window, SW_SHOWNOACTIVATE);
    const HWND combo = this->Combo();
    ASSERT_NE(combo, nullptr);
    for (const WPARAM key : {static_cast<WPARAM>(VK_RETURN), static_cast<WPARAM>(VK_ESCAPE)})
    {
        SendMessageW(combo, CB_SHOWDROPDOWN, TRUE, 0);
        ASSERT_NE(SendMessageW(combo, CB_GETDROPPEDSTATE, 0, 0), 0);
        MSG message{};
        message.hwnd = combo;
        message.message = WM_KEYDOWN;
        message.wParam = key;
        EXPECT_FALSE(this->renderer_.ProcessDialogMessage(message));
        SendMessageW(combo, WM_KEYDOWN, key, 0);
        PumpMessages();
        EXPECT_EQ(this->actions_, 0);
        EXPECT_EQ(this->renderer_.NativeHandle(), window);
    }
}
// 较小客户区中的长译文按钮换行后必须完整位于窗口内部且互不覆盖。
TEST_F(RendererBindingTest, long_footer_fits_small_client)
{
    this->longText_ = true;
    this->Prepare();
    this->Show();
    const HWND window = this->renderer_.NativeHandle();
    RECT outer{0, 0, 480, 280};
    ASSERT_TRUE(AdjustWindowRectExForDpi(&outer, static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE)), FALSE,
                                         static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE)),
                                         GetDpiForWindow(window)));
    ASSERT_TRUE(SetWindowPos(window, nullptr, 0, 0, outer.right - outer.left, outer.bottom - outer.top,
                             SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE));
    RECT client{};
    GetClientRect(window, &client);
    std::vector<RECT> buttons;
    for (HWND button = FindWindowExW(window, nullptr, L"Button", nullptr); button != nullptr;
         button = FindWindowExW(window, button, L"Button", nullptr))
    {
        RECT rectangle{};
        GetWindowRect(button, &rectangle);
        MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&rectangle), 2);
        EXPECT_GE(rectangle.left, client.left);
        EXPECT_GE(rectangle.top, client.top);
        EXPECT_LE(rectangle.right, client.right);
        EXPECT_LE(rectangle.bottom, client.bottom);
        EXPECT_GT(rectangle.right, rectangle.left);
        for (const RECT& previous : buttons)
        {
            RECT intersection{};
            EXPECT_FALSE(IntersectRect(&intersection, &previous, &rectangle));
        }
        buttons.push_back(rectangle);
    }
    EXPECT_EQ(buttons.size(), 2U);
}
// DPI 重排只改变几何与字体，144/192 DPI 下保持当前草稿及选择。
TEST_F(RendererBindingTest, dpi_changes_preserve_selection)
{
    this->Prepare();
    this->draft_ = "b";
    this->Show();
    const HWND window = this->renderer_.NativeHandle();
    for (const UINT dpi : {144U, 192U})
    {
        RECT suggested{100, 100, 100 + MulDiv(600, static_cast<int>(dpi), 96),
                       100 + MulDiv(360, static_cast<int>(dpi), 96)};
        SendMessageW(window, WM_DPICHANGED, MAKEWPARAM(dpi, dpi), reinterpret_cast<LPARAM>(&suggested));
        ASSERT_TRUE(this->renderer_.RefreshTexts());
        EXPECT_EQ(SendMessageW(this->Combo(), CB_GETCURSEL, 0, 0), 1);
        EXPECT_EQ(this->draft_, "b");
        EXPECT_EQ(this->changes_, 0);
    }
}
// Tab 导航到长页面底部字段时应滚动，使获得焦点的输入框可见。
TEST_F(RendererBindingTest, tab_scrolls_focused_field_into_view)
{
    nlohmann::json document = WindowDocument();
    nlohmann::json& children = document["pages"][0]["content"]["children"];
    children.insert(children.begin(), nlohmann::json{{"type", "text"}, {"id", "longInfo"}, {"textKey", "longInfo"}});
    this->longText_ = true;
    this->Prepare(std::move(document));
    this->Show();
    const HWND window = this->renderer_.NativeHandle();
    ShowWindow(window, SW_SHOWNOACTIVATE);
    const HWND tabs = FindWindowExW(window, nullptr, WC_TABCONTROLW, nullptr);
    SetFocus(tabs);
    const HWND combo = this->Combo();
    ASSERT_NE(combo, nullptr);
    for (int step = 0; step < 10 && GetFocus() != combo; ++step)
    {
        MSG message{};
        message.hwnd = GetFocus();
        message.message = WM_KEYDOWN;
        message.wParam = VK_TAB;
        this->renderer_.ProcessDialogMessage(message);
    }
    ASSERT_EQ(GetFocus(), combo);
    RECT field{};
    RECT viewport{};
    GetWindowRect(combo, &field);
    GetClientRect(GetParent(combo), &viewport);
    MapWindowPoints(GetParent(combo), nullptr, reinterpret_cast<POINT*>(&viewport), 2);
    EXPECT_GE(field.top, viewport.top);
    EXPECT_LE(field.bottom, viewport.bottom);
    EXPECT_EQ(this->changes_, 0);
}
// 忙状态页面滚轮不得回传父窗口形成递归，并保持草稿不变。
TEST_F(RendererBindingTest, busy_page_wheel_returns_without_recursion)
{
    this->Prepare();
    this->Show();
    ASSERT_TRUE(this->renderer_.SetBusy(true));
    const HWND combo = this->Combo();
    ASSERT_NE(combo, nullptr);
    SendMessageW(GetParent(combo), WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), 0);
    EXPECT_EQ(this->draft_, "a");
    EXPECT_EQ(this->changes_, 0);
    EXPECT_TRUE(this->errors_.empty());
    EXPECT_NE(this->renderer_.NativeHandle(), nullptr);
    ASSERT_TRUE(this->renderer_.SetBusy(false));
}
// 修改回调异常时恢复已接受的草稿选择，并通知宿主错误与字段 ID。
TEST_F(RendererBindingTest, throwing_change_restores_value_and_reports_error)
{
    this->Prepare();
    this->Show();
    this->throwChange_ = true;
    const HWND combo = this->Combo();
    ASSERT_NE(combo, nullptr);
    SendMessageW(combo, CB_SETCURSEL, 1, 0);
    SendMessageW(GetParent(combo), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(combo), CBN_SELCHANGE),
                 reinterpret_cast<LPARAM>(combo));
    EXPECT_EQ(SendMessageW(combo, CB_GETCURSEL, 0, 0), 0);
    EXPECT_EQ(this->draft_, "a");
    EXPECT_EQ(this->changes_, 0);
    ASSERT_FALSE(this->errors_.empty());
    EXPECT_EQ(this->errors_.back().code, "callback_failed");
    EXPECT_EQ(this->errors_.back().id, "choice");
}
// 首次显示遇到草稿选项失效时通知宿主，不默默采用可用的第一项。
TEST_F(RendererBindingTest, initial_unavailable_value_reports_error)
{
    this->Prepare();
    this->draft_ = "unavailable";
    this->Show();
    ASSERT_FALSE(this->errors_.empty());
    EXPECT_EQ(this->errors_.back().code, "value_unavailable");
    EXPECT_EQ(this->errors_.back().id, "choice");
    EXPECT_EQ(this->draft_, "unavailable");
    EXPECT_EQ(this->changes_, 0);
}
// 通过原生样式识别复选框，避免依赖创建顺序或业务 ID。
BOOL CALLBACK FindCheckbox(HWND window, LPARAM context)
{
    wchar_t name[64]{};
    GetClassNameW(window, name, 64);
    if (wcscmp(name, L"Button") == 0 && (GetWindowLongPtrW(window, GWL_STYLE) & BS_TYPEMASK) == BS_AUTOCHECKBOX)
    {
        *reinterpret_cast<HWND*>(context) = window;
        return FALSE;
    }
    return TRUE;
}
class RendererBoolTest : public RendererBindingTest
{
  protected:
    bool value_ = true;
    bool reject_ = false;
    bool throw_ = false;
    bool readFailure_ = false;
    bool throwRead_ = false;
    // 在现有窗口加入通用布尔字段及其独立草稿。
    void PrepareBool()
    {
        nlohmann::json document = WindowDocument();
        document["pages"][0]["content"]["children"].push_back(
            {{"type", "checkbox"}, {"id", "checked"}, {"labelKey", "checkbox.label"}});
        this->Prepare(document);
        ASSERT_TRUE(this->renderer_.BindBool(
            "checked",
            [this]()
            {
                if (this->throwRead_)
                    throw std::runtime_error("bool read failed");
                return open_st::RendererBoolResult{!this->readFailure_, this->value_,
                                                   this->readFailure_ ? L"read failed" : L""};
            },
            [this](bool value)
            {
                if (this->throw_)
                    throw std::runtime_error("bool change failed");
                if (this->reject_)
                    return open_st::RendererChangeResult{false, L"rejected"};
                this->value_ = value;
                ++this->changes_;
                return open_st::RendererChangeResult{};
            }));
    }
    // 查找属于当前测试窗口的复选框。
    HWND Checkbox()
    {
        HWND checkbox{};
        EnumChildWindows(this->renderer_.NativeHandle(), FindCheckbox, reinterpret_cast<LPARAM>(&checkbox));
        return checkbox;
    }
};
// 布尔槽位不能重复、跨类型或缺失回调。
TEST_F(RendererBoolTest, validates_bool_binding_type_and_uniqueness)
{
    this->PrepareBool();
    const auto read = []() { return open_st::RendererBoolResult{}; };
    const auto change = [](bool) { return open_st::RendererChangeResult{}; };
    EXPECT_EQ(this->renderer_.BindBool("checked", read, change).code, "duplicate_binding");
    EXPECT_EQ(this->renderer_.BindBool("choice", read, change).code, "wrong_control_type");
    EXPECT_EQ(this->renderer_.BindBool("unknown", read, change).code, "unknown_id");
    EXPECT_EQ(this->renderer_.BindBool("checked", {}, change).code, "empty_callback");
    EXPECT_EQ(this->renderer_.BindOptions("checked", []() { return open_st::RendererOptionsResult{}; }).code,
              "wrong_control_type");
    EXPECT_TRUE(this->renderer_.ValidateBindings());
}
// 缺少布尔读取或修改绑定时，不创建半成品窗口。
TEST_F(RendererBoolTest, missing_bool_binding_prevents_window_creation)
{
    nlohmann::json document = WindowDocument();
    document["pages"][0]["content"]["children"].push_back(
        {{"type", "checkbox"}, {"id", "checked"}, {"labelKey", "checkbox.label"}});
    this->Prepare(document);
    EXPECT_EQ(this->renderer_.ValidateBindings().code, "field_binding_missing");
    EXPECT_FALSE(this->renderer_.Show());
    EXPECT_EQ(this->renderer_.NativeHandle(), nullptr);
}
// 读取失败保留已显示草稿，读取异常只返回结构错误且不修改业务。
TEST_F(RendererBoolTest, failed_read_keeps_previous_visual_value)
{
    this->PrepareBool();
    this->Show();
    const HWND checkbox = this->Checkbox();
    ASSERT_NE(checkbox, nullptr);
    this->value_ = false;
    this->readFailure_ = true;
    EXPECT_TRUE(this->renderer_.RefreshValues());
    EXPECT_EQ(SendMessageW(checkbox, BM_GETCHECK, 0, 0), BST_CHECKED);
    this->throwRead_ = true;
    EXPECT_EQ(this->renderer_.RefreshValues().code, "callback_failed");
    EXPECT_EQ(SendMessageW(checkbox, BM_GETCHECK, 0, 0), BST_CHECKED);
    EXPECT_EQ(this->changes_, 0);
}
// 原生点击修改草稿，程序刷新和文字刷新保留布尔值且不回写。
TEST_F(RendererBoolTest, click_updates_bool_and_refresh_is_read_only)
{
    this->PrepareBool();
    this->Show();
    const HWND checkbox = this->Checkbox();
    ASSERT_NE(checkbox, nullptr);
    EXPECT_EQ(SendMessageW(checkbox, BM_GETCHECK, 0, 0), BST_CHECKED);
    SendMessageW(checkbox, BM_CLICK, 0, 0);
    EXPECT_FALSE(this->value_);
    EXPECT_EQ(this->changes_, 1);
    this->value_ = true;
    ASSERT_TRUE(this->renderer_.RefreshValues());
    this->longText_ = true;
    ASSERT_TRUE(this->renderer_.RefreshTexts());
    EXPECT_EQ(SendMessageW(checkbox, BM_GETCHECK, 0, 0), BST_CHECKED);
    EXPECT_EQ(this->changes_, 1);
    EXPECT_TRUE(this->renderer_.SetFieldError("checked", L"field error"));
}
// 拒绝和异常均恢复视觉值，异常带控件 ID 汇报。
TEST_F(RendererBoolTest, rejected_or_throwing_change_restores_previous_check)
{
    this->PrepareBool();
    this->Show();
    const HWND checkbox = this->Checkbox();
    ASSERT_NE(checkbox, nullptr);
    this->reject_ = true;
    SendMessageW(checkbox, BM_CLICK, 0, 0);
    EXPECT_EQ(SendMessageW(checkbox, BM_GETCHECK, 0, 0), BST_CHECKED);
    this->throw_ = true;
    SendMessageW(checkbox, BM_CLICK, 0, 0);
    EXPECT_EQ(SendMessageW(checkbox, BM_GETCHECK, 0, 0), BST_CHECKED);
    EXPECT_EQ(this->changes_, 0);
    ASSERT_FALSE(this->errors_.empty());
    EXPECT_EQ(this->errors_.back().code, "callback_failed");
    EXPECT_EQ(this->errors_.back().id, "checked");
}
// 忙状态与单控件禁用均禁止通知修改业务草稿。
TEST_F(RendererBoolTest, disabled_and_busy_checkbox_do_not_dispatch)
{
    this->PrepareBool();
    this->Show();
    const HWND checkbox = this->Checkbox();
    ASSERT_NE(checkbox, nullptr);
    ASSERT_TRUE(this->renderer_.SetEnabled("checked", false));
    SendMessageW(GetParent(checkbox), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(checkbox), BN_CLICKED),
                 reinterpret_cast<LPARAM>(checkbox));
    EXPECT_EQ(this->changes_, 0);
    ASSERT_TRUE(this->renderer_.SetBusy(true));
    ASSERT_TRUE(this->renderer_.SetEnabled("checked", true));
    EXPECT_FALSE(IsWindowEnabled(checkbox));
    SendMessageW(GetParent(checkbox), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(checkbox), BN_CLICKED),
                 reinterpret_cast<LPARAM>(checkbox));
    EXPECT_EQ(this->changes_, 0);
    ASSERT_TRUE(this->renderer_.SetBusy(false));
    EXPECT_TRUE(IsWindowEnabled(checkbox));
}
} // namespace
