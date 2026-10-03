// 使用真实公共子表单验证类型/模式切换、取消、密钥遮罩和结构化提交，不读取真实配置。
#include "translation_settings.h"
#include <array>
#include <chrono>
#include <commctrl.h>
#include <functional>
#include <gtest/gtest.h>
#include <limits>
namespace
{
struct Search
{
    std::wstring type, text;
    HWND found{};
};
// 按原生类和显示标签查找可见控件，不依赖动态数字ID。
// 入参：window为枚举窗口；data为搜索状态。
// 返回：找到后终止枚举。
BOOL CALLBACK FindControl(HWND window, LPARAM data)
{
    auto& search = *reinterpret_cast<Search*>(data);
    std::array<wchar_t, 256> type{}, text{};
    GetClassNameW(window, type.data(), static_cast<int>(type.size()));
    GetWindowTextW(window, text.data(), static_cast<int>(text.size()));
    if (IsWindowVisible(window) && _wcsicmp(type.data(), search.type.c_str()) == 0 && text.data() == search.text)
    {
        search.found = window;
        return FALSE;
    }
    return TRUE;
}
// 按完整标题查找显示控件。
// 入参：owner为窗口；type/text为原生类与标签。
// 返回：借用句柄或空。
HWND Control(HWND owner, const wchar_t* type, const wchar_t* text)
{
    Search search{type, text};
    EnumChildWindows(owner, FindControl, reinterpret_cast<LPARAM>(&search));
    return search.found;
}
// 按标签下方的最近同列输入定位字段，避免把隐藏种类字段当成当前输入。
// 入参：owner为表单，label/type为可见字段标签及原生类。
// 返回：对应输入句柄或空。
HWND Field(HWND owner, const wchar_t* label, const wchar_t* type)
{
    const HWND caption = Control(owner, L"STATIC", label);
    if (!caption)
        return nullptr;
    RECT labelRect{};
    GetWindowRect(caption, &labelRect);
    HWND result{};
    LONG distance = std::numeric_limits<LONG>::max();
    const HWND parent = GetParent(caption);
    for (HWND child = FindWindowExW(parent, nullptr, type, nullptr); child;
         child = FindWindowExW(parent, child, type, nullptr))
    {
        if (!IsWindowVisible(child))
            continue;
        RECT rect{};
        GetWindowRect(child, &rect);
        const LONG gap = rect.top - labelRect.bottom;
        if (rect.left == labelRect.left && gap >= -1 && gap < distance)
        {
            distance = gap;
            result = child;
        }
    }
    return result;
}
// 使用真实选项索引发送用户选择通知。
// 入参：combo为下拉框；value为显示值。
// 返回：无，断言选择存在。
void Select(HWND combo, const wchar_t* value)
{
    ASSERT_NE(combo, nullptr);
    const LRESULT index =
        SendMessageW(combo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(value));
    ASSERT_NE(index, CB_ERR);
    SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
    SendMessageW(GetParent(combo), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(combo), CBN_SELCHANGE),
                 reinterpret_cast<LPARAM>(combo));
}
// 读取原生编辑文字用于验证隐藏模式没有丢失草稿。
// 入参：window为输入框。
// 返回：显示文本。
std::wstring Value(HWND window)
{
    const int count = GetWindowTextLengthW(window);
    std::wstring value(static_cast<std::size_t>(count) + 1, L'\0');
    GetWindowTextW(window, value.data(), count + 1);
    value.resize(static_cast<std::size_t>(count));
    return value;
}
// 生成包含所有必需字段的无网络自定义样例。
// 入参：kind为普通测试或custom_http。
// 返回：固定身份的独立候选。
nlohmann::json Profile(std::string_view kind)
{
    using Json = nlohmann::json;
    Json profile{{"id", "stable"},
                 {"kind", kind},
                 {"name", "Original"},
                 {"enabled", false},
                 {"configuration", Json::object()},
                 {"secrets", {{"api_key", "original-secret"}}}};
    if (kind == "custom_http")
        profile["configuration"] = {{"method", "POST"},
                                    {"url", "https://example.invalid/translate"},
                                    {"headers", Json::object()},
                                    {"query", Json::object()},
                                    {"source_languages", {{"auto", "auto"}}},
                                    {"target_languages", {{"en", "en"}}},
                                    {"body_mode", "json"},
                                    {"body", {{"text", "{{text}}"}}},
                                    {"response", {{"mode", "json"}, {"text_pointer", ""}}}};
    return profile;
}
class TranslationProfileDialogTest : public testing::Test
{
  protected:
    HWND owner_{};
    UINT_PTR timer_{};
    bool visited_{}, timedOut_{};
    std::function<void(HWND)> action_;
    std::function<void(open_st::SettingsWindowCallbacks&)> configure_;
    std::chrono::steady_clock::time_point deadline_;
    static thread_local TranslationProfileDialogTest* current_;
    // 建立只属于本测试的宿主。
    // 入参：无。
    // 返回：无。
    void SetUp() override
    {
        this->owner_ = CreateWindowExW(0, L"STATIC", L"Profile test owner", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                                       GetModuleHandleW(nullptr), nullptr);
        ASSERT_NE(this->owner_, nullptr);
        current_ = this;
    }
    // 清除驱动与宿主。
    // 入参：无。
    // 返回：无。
    void TearDown() override
    {
        if (this->timer_)
            KillTimer(this->owner_, this->timer_);
        current_ = nullptr;
        DestroyWindow(this->owner_);
    }
    // 在本线程查找属于测试宿主的公共模态表单。
    // 入参：window为候选，data为输出句柄。
    // 返回：匹配后停止。
    static BOOL CALLBACK FindDialog(HWND window, LPARAM data)
    {
        wchar_t type[128]{};
        GetClassNameW(window, type, 128);
        if (GetWindow(window, GW_OWNER) == current_->owner_ && std::wstring_view(type) == L"OpenST.WindowRenderer")
        {
            *reinterpret_cast<HWND*>(data) = window;
            return FALSE;
        }
        return TRUE;
    }
    // 执行一次合成用户交互，超时以失败收尾避免挂起。
    // 入参：系统定时器参数忽略。
    // 返回：无。
    static void CALLBACK Drive(HWND, UINT, UINT_PTR, DWORD)
    {
        auto& self = *current_;
        HWND window{};
        EnumThreadWindows(GetCurrentThreadId(), FindDialog, reinterpret_cast<LPARAM>(&window));
        if (std::chrono::steady_clock::now() > self.deadline_)
        {
            self.timedOut_ = true;
            if (window)
                PostMessageW(window, WM_CLOSE, 0, 0);
            return;
        }
        if (!window || self.visited_)
            return;
        self.visited_ = true;
        self.action_(window);
    }
    // 运行真实表单，领域替身只发布值目录与结构有效性，不调用网络。
    // 入参：profile为父候选，creating控制新增类型选择。
    // 返回：明确确认状态。
    bool Run(nlohmann::json& profile, bool creating = false)
    {
        open_st::SettingsWindowCallbacks callbacks;
        callbacks.text = [](std::string_view key) { return std::wstring(key.begin(), key.end()); };
        callbacks.translationChoices = [](std::string_view key)
        {
            std::vector<std::string> values;
            if (key == "translation.kind")
                values = {"test", "custom_http"};
            else if (key == "custom.method")
                values = {"GET", "POST", "PUT", "PATCH"};
            else if (key == "custom.body_mode")
                values = {"none", "json", "form", "raw"};
            else if (key == "custom.response_mode")
                values = {"text", "json"};
            else if (key == "custom.extraction")
                values = {"single", "array"};
            else if (key == "custom.scalar_type")
                values = {"string", "number", "boolean", "null"};
            else if (key == "custom.error_category")
                values = {"authentication", "permission", "quota", "rate_limited", "input", "service", "context_limit"};
            else if (key == "translation.source_language")
                values = {"auto", "zh-CN", "en", "ja"};
            else
                values = {"zh-CN", "en", "ja"};
            std::vector<open_st::SettingsOption> result;
            for (const auto& value : values)
                result.push_back({value, std::wstring(value.begin(), value.end())});
            return result;
        };
        callbacks.createTranslationProfile = [](std::string_view kind)
        {
            auto profile = Profile(kind);
            profile["name"] = kind;
            return profile;
        };
        callbacks.translationProfileFields = [](std::string_view)
        { return std::vector<open_st::SettingsTranslationField>{{"api_key", true, false, {}}}; };
        callbacks.validateTranslationProfile = [](const nlohmann::json& candidate)
        { return candidate.at("configuration").is_object() ? L"" : L"invalid"; };
        if (this->configure_)
            this->configure_(callbacks);
        this->deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        this->timer_ = SetTimer(this->owner_, 901, 15, Drive);
        EXPECT_NE(this->timer_, 0U);
        const bool accepted = open_st::EditTranslationProfile(this->owner_, nullptr, profile, callbacks, creating,
                                                              "manual", "http://127.0.0.1:7897");
        EXPECT_TRUE(this->visited_);
        EXPECT_FALSE(this->timedOut_);
        return accepted;
    }
};
thread_local TranslationProfileDialogTest* TranslationProfileDialogTest::current_{};
// 确认和取消均通过真实密码控件，确认保持稳定身份，取消不改父条目。
// 入参：无。
// 返回：无。
TEST_F(TranslationProfileDialogTest, secret_edit_preserves_identity_and_cancel_preserves_parent)
{
    for (bool accept : {false, true})
    {
        this->visited_ = false;
        this->action_ = [accept](HWND window)
        {
            const HWND secret = Field(window, L"settings.translation.field.api_key", L"EDIT");
            ASSERT_NE(secret, nullptr);
            EXPECT_NE(GetWindowLongPtrW(secret, GWL_STYLE) & ES_PASSWORD, 0);
            EXPECT_NE(SendMessageW(secret, EM_GETPASSWORDCHAR, 0, 0), 0);
            EXPECT_TRUE(SetWindowTextW(secret, L"child-secret"));
            EXPECT_TRUE(SetWindowTextW(Field(window, L"settings.translation.profile_name", L"EDIT"), L"Changed name"));
            EXPECT_FALSE(IsWindowEnabled(Field(window, L"settings.translation.profile_type", L"COMBOBOX")));
            SendMessageW(Control(window, L"BUTTON", accept ? L"dialog.ok" : L"dialog.cancel"), BM_CLICK, 0, 0);
        };
        auto profile = Profile("test");
        const auto original = profile;
        EXPECT_EQ(this->Run(profile), accept);
        if (!accept)
            EXPECT_EQ(profile, original);
        else
        {
            EXPECT_EQ(profile["id"], "stable");
            EXPECT_EQ(profile["kind"], "test");
            EXPECT_EQ(profile["name"], "Changed name");
            EXPECT_EQ(profile["secrets"]["api_key"], "child-secret");
        }
    }
}
// 新增窗口切类型及正文模式只隐藏组，非法JSON切回后仍阻止提交，取消不发布任一种草稿。
// 入参：无。
// 返回：无。
TEST_F(TranslationProfileDialogTest, new_profile_type_and_body_mode_switches_preserve_drafts)
{
    this->action_ = [](HWND window)
    {
        const HWND kind = Field(window, L"settings.translation.profile_type", L"COMBOBOX");
        ASSERT_TRUE(IsWindowEnabled(kind));
        Select(kind, L"custom_http");
        const HWND name = Field(window, L"settings.translation.profile_name", L"EDIT");
        EXPECT_EQ(Value(name), L"custom_http");
        EXPECT_TRUE(SetWindowTextW(name, L"User selected name"));
        EXPECT_EQ(Field(window, L"settings.translation.field.api_key", L"EDIT"), nullptr);
        const HWND json = Field(window, L"settings.translation.custom.json_body", L"EDIT");
        ASSERT_NE(json, nullptr);
        SendMessageW(json, EM_SETSEL, 0, -1);
        SendMessageW(json, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"{unfinished"));
        EXPECT_FALSE(IsWindowEnabled(Control(window, L"BUTTON", L"dialog.ok")));
        const HWND mode = Field(window, L"settings.translation.custom.body_mode", L"COMBOBOX");
        Select(mode, L"none");
        EXPECT_TRUE(IsWindowEnabled(Control(window, L"BUTTON", L"dialog.ok")));
        EXPECT_FALSE(IsWindowVisible(json));
        Select(mode, L"json");
        EXPECT_EQ(Value(json), L"{unfinished");
        EXPECT_FALSE(IsWindowEnabled(Control(window, L"BUTTON", L"dialog.ok")));
        Select(kind, L"test");
        EXPECT_EQ(Value(name), L"User selected name");
        EXPECT_NE(Field(window, L"settings.translation.field.api_key", L"EDIT"), nullptr);
        EXPECT_FALSE(IsWindowVisible(json));
        EXPECT_TRUE(IsWindowEnabled(Control(window, L"BUTTON", L"dialog.ok")));
        Select(kind, L"custom_http");
        EXPECT_EQ(Value(json), L"{unfinished");
        EXPECT_FALSE(IsWindowEnabled(Control(window, L"BUTTON", L"dialog.ok")));
        SendMessageW(Control(window, L"BUTTON", L"dialog.cancel"), BM_CLICK, 0, 0);
    };
    auto profile = Profile("test");
    const auto before = profile;
    EXPECT_FALSE(this->Run(profile, true));
    EXPECT_EQ(profile, before);
}
// 自定义密钥只在列表提供固定遮罩，主表单不出现旧整块configuration编辑框。
// 入参：无。
// 返回：无。
TEST_F(TranslationProfileDialogTest, custom_secret_table_masks_values_and_confirm_keeps_native_configuration)
{
    this->action_ = [](HWND window)
    {
        const HWND table = Field(window, L"settings.translation.custom.secrets", WC_LISTVIEWW);
        ASSERT_NE(table, nullptr);
        ASSERT_EQ(ListView_GetItemCount(table), 1);
        const HWND viewport = FindWindowExW(window, nullptr, L"OpenST.WindowRendererPage", nullptr);
        ASSERT_NE(viewport, nullptr);
        EXPECT_EQ(FindWindowExW(window, viewport, L"OpenST.WindowRendererPage", nullptr), nullptr);
        EXPECT_EQ(FindWindowExW(window, nullptr, WC_TABCONTROLW, nullptr), nullptr);
        SCROLLINFO scroll{};
        scroll.cbSize = sizeof(scroll);
        scroll.fMask = SIF_ALL;
        ASSERT_TRUE(GetScrollInfo(viewport, SB_VERT, &scroll));
        EXPECT_GT(scroll.nMax, static_cast<int>(scroll.nPage));
        SendMessageW(viewport, WM_VSCROLL, SB_BOTTOM, 0);
        RECT viewRect{}, tableRect{};
        GetWindowRect(viewport, &viewRect);
        GetWindowRect(table, &tableRect);
        EXPECT_GE(tableRect.top, viewRect.top);
        EXPECT_LE(tableRect.bottom, viewRect.bottom);
        wchar_t value[256]{};
        ListView_GetItemText(table, 0, 1, value, 256);
        EXPECT_NE(std::wstring(value), L"original-secret");
        EXPECT_NE(std::wstring(value), L"");
        EXPECT_EQ(Control(window, L"STATIC", L"settings.translation.field.configuration"), nullptr);
        SendMessageW(Control(window, L"BUTTON", L"dialog.ok"), BM_CLICK, 0, 0);
    };
    auto profile = Profile("custom_http");
    const auto before = profile;
    EXPECT_TRUE(this->Run(profile));
    EXPECT_EQ(profile, before);
}

// 当前草稿和代理仅交付单次测试，未启用状态及父事务保持不变。
// 入参：无。返回：无。
TEST_F(TranslationProfileDialogTest, connection_test_uses_unsaved_disabled_draft_and_cancels_owned_request)
{
    int submitted = 0, cancelled = 0;
    this->configure_ = [&](open_st::SettingsWindowCallbacks& callbacks)
    {
        callbacks.submitTranslationTest =
            [&](const nlohmann::json& candidate, std::string_view mode, std::string_view address)
        {
            ++submitted;
            EXPECT_EQ(candidate["name"], "Unsaved");
            EXPECT_EQ(candidate["secrets"]["api_key"], "draft-secret");
            EXPECT_FALSE(candidate["enabled"].get<bool>());
            EXPECT_EQ(mode, "manual");
            EXPECT_EQ(address, "http://127.0.0.1:7897");
            return open_st::SettingsTranslationTestStatus{91, true, L"working"};
        };
        callbacks.translationTestStatus = [](std::uint64_t id)
        { return open_st::SettingsTranslationTestStatus{id, true, L"working"}; };
        callbacks.cancelTranslationTest = [&](std::uint64_t id)
        {
            EXPECT_EQ(id, 91U);
            ++cancelled;
        };
    };
    this->action_ = [](HWND window)
    {
        SetWindowTextW(Field(window, L"settings.translation.profile_name", L"EDIT"), L"Unsaved");
        SetWindowTextW(Field(window, L"settings.translation.field.api_key", L"EDIT"), L"draft-secret");
        const HWND test = Control(window, L"BUTTON", L"translation.test.start");
        ASSERT_NE(test, nullptr);
        SendMessageW(test, BM_CLICK, 0, 0);
        const HWND output = Field(window, L"translation.test.output", L"EDIT");
        ASSERT_NE(output, nullptr);
        EXPECT_NE(GetWindowLongPtrW(output, GWL_STYLE) & ES_READONLY, 0);
        EXPECT_EQ(Value(output), L"working");
        SetWindowTextW(Field(window, L"settings.translation.profile_name", L"EDIT"), L"Changed after test");
        EXPECT_NE(Value(output).find(L"translation.test.previous"), std::wstring::npos);
        EXPECT_NE(Control(window, L"BUTTON", L"translation.test.cancel"), nullptr);
        PostMessageW(window, WM_CLOSE, 0, 0);
    };
    auto profile = Profile("test");
    const auto original = profile;
    EXPECT_FALSE(this->Run(profile));
    EXPECT_EQ(submitted, 1);
    EXPECT_EQ(cancelled, 1);
    EXPECT_EQ(profile, original);
}
// 宿主消息在编辑期间继续分派；结束状态必须属于当前请求才刷新。
// 入参：无。返回：无。
TEST_F(TranslationProfileDialogTest, connection_result_is_polled_after_host_dispatch)
{
    constexpr UINT wake = WM_APP + 611;
    bool delivered = false;
    HWND dialog{};
    this->configure_ = [&](open_st::SettingsWindowCallbacks& callbacks)
    {
        callbacks.submitTranslationTest = [](const nlohmann::json&, std::string_view, std::string_view)
        { return open_st::SettingsTranslationTestStatus{72, true, L"pending"}; };
        callbacks.cancelTranslationTest = [](std::uint64_t id) { EXPECT_EQ(id, 72U); };
        callbacks.translationTestStatus = [&](std::uint64_t id)
        {
            return open_st::SettingsTranslationTestStatus{id, !delivered, delivered ? L"HTTP 200 / 54001" : L"pending"};
        };
        callbacks.processThreadMessage = [&](MSG& message)
        {
            if (message.message != wake)
                return false;
            if (!delivered)
            {
                delivered = true;
                PostThreadMessageW(GetCurrentThreadId(), wake, 0, 0);
            }
            else
            {
                EXPECT_EQ(Value(Field(dialog, L"translation.test.output", L"EDIT")), L"HTTP 200 / 54001");
                EXPECT_NE(Control(dialog, L"BUTTON", L"translation.test.start"), nullptr);
                PostMessageW(dialog, WM_CLOSE, 0, 0);
            }
            return true;
        };
    };
    this->action_ = [&](HWND window)
    {
        dialog = window;
        SendMessageW(Control(window, L"BUTTON", L"translation.test.start"), BM_CLICK, 0, 0);
        PostThreadMessageW(GetCurrentThreadId(), wake, 0, 0);
    };
    auto profile = Profile("test");
    EXPECT_FALSE(this->Run(profile));
    EXPECT_TRUE(delivered);
}
} // namespace
