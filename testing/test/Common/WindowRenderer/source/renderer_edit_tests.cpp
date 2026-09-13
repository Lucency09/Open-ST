// 验证通用文本、整数与滑条的即时草稿、非法中间态和无副作用刷新。

#include <window_renderer.h>

#include <commctrl.h>
#include <gtest/gtest.h>

namespace
{
// 构建包含文本及横向数字滑条的独立通用布局。
// 入参：无。
// 返回：独立布局文档，不包含业务设置键。
nlohmann::json EditDocument()
{
    return nlohmann::json::parse(R"({
      "schemaVersion":1,"window":{"titleKey":"title"},
      "content":{"type":"column","id":"content","children":[
        {"type":"edit","id":"text"},
        {"type":"row","id":"row","children":[
          {"type":"text","id":"label","textKey":"label","width":200},
          {"type":"integer","id":"number","minimum":1,"maximum":100,"slider":true}]}]},
      "footer":{"trailing":[{"type":"button","id":"confirm","textKey":"confirm"}]}
    })");
}

class RendererEditTest : public testing::Test
{
  protected:
    open_st::WindowRenderer renderer_;
    std::string text_{"valid"};
    std::optional<std::int64_t> number_{95};
    int textChanges_{};
    int numberChanges_{};
    int confirmations_{};
    bool rejectNumber_{};
    bool failRead_{};
    std::vector<open_st::RendererResult> errors_;

    // 绑定独立草稿并显示隐藏窗口，不发送真实桌面输入。
    // 入参：无。
    // 返回：无；初始化失败由断言报告。
    void SetUp() override
    {
        ASSERT_TRUE(this->renderer_.LoadLayout(EditDocument()));
        // 显示稳定文字键以隔离业务本地化。
        // 入参：key 为布局文字键。
        // 返回：对应测试文字。
        ASSERT_TRUE(
            this->renderer_.SetTextResolver([](std::string_view key) { return std::wstring(key.begin(), key.end()); }));
        ASSERT_TRUE(this->renderer_.BindString(
            "text",
            // 读取原始文本草稿，允许注入读取失败。
            // 入参：无。
            // 返回：原始草稿或读取错误。
            [this]() { return open_st::RendererStringResult{!this->failRead_, this->text_, L""}; },
            // 保留每次原始文本，同时用简单测试规则拒绝未完成内容。
            // 入参：value 为完整原始编辑文本。
            // 返回：仅 valid 被接受，其他输入携带字段错误。
            [this](std::string_view value)
            {
                this->text_ = value;
                ++this->textChanges_;
                return open_st::RendererChangeResult{value == "valid", value == "valid" ? L"" : L"Invalid text"};
            }));
        ASSERT_TRUE(this->renderer_.BindInteger(
            "number",
            // 返回数值草稿，无效输入期间不提供可冒充当前输入的旧数值。
            // 入参：无。
            // 返回：有效数值或明确的读取失败状态。
            [this]()
            {
                return open_st::RendererIntegerResult{!this->failRead_ && this->number_.has_value(),
                                                      this->number_.value_or(0),
                                                      this->number_ ? L"" : L"Invalid integer"};
            },
            // 记录合法整数或无效状态，并可拒绝业务范围内的数值。
            // 入参：value 为语法及布局范围允许的整数，否则为空。
            // 返回：测试宿主是否接受及字段错误。
            [this](std::optional<std::int64_t> value)
            {
                ++this->numberChanges_;
                if (this->rejectNumber_)
                    return open_st::RendererChangeResult{false, L"Rejected integer"};
                this->number_ = value;
                return open_st::RendererChangeResult{value.has_value(), value ? L"" : L"Invalid integer"};
            }));
        // 记录确认动作，验证普通编辑不会误触发提交。
        // 入参：无。
        // 返回：无。
        ASSERT_TRUE(this->renderer_.BindAction("confirm", [this]() { ++this->confirmations_; }));
        ASSERT_TRUE(this->renderer_.SetDefaultAction("confirm"));
        // 将窗口关闭交由渲染器延迟销毁。
        // 入参：无。
        // 返回：无。
        ASSERT_TRUE(this->renderer_.SetCloseHandler([this]() { (void)this->renderer_.RequestClose(); }));
        // 收集所有渲染器异常边界报告。
        // 入参：error 为结构化错误。
        // 返回：无。
        ASSERT_TRUE(this->renderer_.SetErrorHandler([this](const open_st::RendererResult& error)
                                                    { this->errors_.push_back(error); }));
        open_st::RendererWindowOptions options;
        options.showCommand = SW_HIDE;
        ASSERT_TRUE(this->renderer_.Show(options));
    }

    // 在回调捕获成员销毁之前关闭窗口。
    // 入参：无。
    // 返回：无。
    void TearDown() override
    {
        DestroyWindow(this->renderer_.NativeHandle());
    }
    // 返回测试页面视口。
    // 入参：无。
    // 返回：借用的原生视口句柄。
    HWND Viewport()
    {
        return FindWindowExW(this->renderer_.NativeHandle(), nullptr, L"OpenST.WindowRendererPage", nullptr);
    }
    // 根据布局中叶节点顺序定位原生编辑框。
    // 入参：number 表示数字框，否则取普通文本框。
    // 返回：借用的控件句柄。
    HWND Edit(bool number)
    {
        return GetDlgItem(this->Viewport(), number ? 102 : 100);
    }
    // 定位整数控件的原生滑条。
    // 入参：无。
    // 返回：借用的滑条句柄。
    HWND Slider()
    {
        return FindWindowExW(this->Viewport(), nullptr, TRACKBAR_CLASSW, nullptr);
    }
    // 读取可见原始编辑文本供断言使用。
    // 入参：number 表示数字框，否则取普通文本框。
    // 返回：文本副本。
    std::wstring Value(bool number)
    {
        wchar_t text[256]{};
        GetWindowTextW(this->Edit(number), text, 256);
        return text;
    }
};

// 验证输入拒绝保留原始文本，初始化及文本刷新不产生伪变更。
// 入参：无。
// 返回：无；断言宿主原始草稿、可见值及通知次数。
TEST_F(RendererEditTest, text_changes_are_immediate_and_rejection_keeps_original_input)
{
    EXPECT_EQ(this->textChanges_, 0);
    EXPECT_EQ(this->numberChanges_, 0);
    ASSERT_TRUE(SetWindowTextW(this->Edit(false), L"unfinished"));
    EXPECT_EQ(this->text_, "unfinished");
    EXPECT_EQ(this->Value(false), L"unfinished");
    EXPECT_EQ(this->textChanges_, 1);
    ASSERT_TRUE(this->renderer_.RefreshTexts());
    EXPECT_EQ(this->Value(false), L"unfinished");
    EXPECT_EQ(this->textChanges_, 1);
    EXPECT_TRUE(this->renderer_.SetFieldError("text", L"field error"));
    EXPECT_EQ(this->confirmations_, 0);
}
// 验证空、越界、浮点及非数字均通知无效状态，滑条与刷新不会掩盖原始输入。
// 入参：无。
// 返回：无；断言每次编辑通知、原始文本和最后合法滑条位置。
TEST_F(RendererEditTest, invalid_integer_notifies_null_and_keeps_original_text_and_slider)
{
    const std::vector<std::wstring> invalid{L"",    L"-",  L"-1", L"1.5", L"abc", L"101", L"0", L"9223372036854775808",
                                            L" 95", L"95 "};
    for (const std::wstring& value : invalid)
    {
        const int before = this->numberChanges_;
        ASSERT_TRUE(SetWindowTextW(this->Edit(true), value.c_str()));
        EXPECT_EQ(this->numberChanges_, before + 1);
        EXPECT_FALSE(this->number_.has_value());
        EXPECT_EQ(this->Value(true), value);
        EXPECT_EQ(SendMessageW(this->Slider(), TBM_GETPOS, 0, 0), 95);
        ASSERT_TRUE(this->renderer_.RefreshTexts());
        ASSERT_TRUE(this->renderer_.RefreshValues());
        EXPECT_EQ(this->Value(true), value);
        EXPECT_EQ(this->numberChanges_, before + 1);
    }
    EXPECT_TRUE(this->renderer_.SetFieldError("number", L"number error"));
    EXPECT_EQ(this->confirmations_, 0);
}

// 验证合法整数同步滑条，程序移动不通知，用户滑条操作才替换非法文本。
// 入参：无。
// 返回：无；断言双向同步及每次用户变化仅通知一次。
TEST_F(RendererEditTest, slider_and_integer_synchronize_only_for_explicit_user_changes)
{
    ASSERT_TRUE(SetWindowTextW(this->Edit(true), L"64"));
    EXPECT_EQ(this->number_, 64);
    EXPECT_EQ(SendMessageW(this->Slider(), TBM_GETPOS, 0, 0), 64);
    ASSERT_TRUE(SetWindowTextW(this->Edit(true), L"unfinished"));
    EXPECT_FALSE(this->number_);
    const int before = this->numberChanges_;
    SendMessageW(this->Slider(), TBM_SETPOS, TRUE, 80);
    EXPECT_EQ(this->numberChanges_, before);
    EXPECT_EQ(this->Value(true), L"unfinished");
    SendMessageW(this->Viewport(), WM_HSCROLL, TB_THUMBTRACK, reinterpret_cast<LPARAM>(this->Slider()));
    EXPECT_EQ(this->number_, 80);
    EXPECT_EQ(this->Value(true), L"80");
    EXPECT_EQ(this->numberChanges_, before + 1);
    SendMessageW(this->Viewport(), WM_HSCROLL, TB_ENDTRACK, reinterpret_cast<LPARAM>(this->Slider()));
    EXPECT_EQ(this->numberChanges_, before + 1);
}

// 验证宿主拒绝合法数字不会回退编辑内容，滑条保留上次接受位置。
// 入参：无。
// 返回：无；断言原始输入、宿主草稿和滑条不相互冒充。
TEST_F(RendererEditTest, rejected_integer_keeps_raw_text_and_previous_accepted_slider)
{
    this->rejectNumber_ = true;
    ASSERT_TRUE(SetWindowTextW(this->Edit(true), L"60"));
    EXPECT_EQ(this->Value(true), L"60");
    EXPECT_EQ(this->number_, 95);
    EXPECT_EQ(SendMessageW(this->Slider(), TBM_GETPOS, 0, 0), 95);
    SendMessageW(this->Slider(), TBM_SETPOS, TRUE, 30);
    SendMessageW(this->Viewport(), WM_HSCROLL, TB_THUMBTRACK, reinterpret_cast<LPARAM>(this->Slider()));
    EXPECT_EQ(this->Value(true), L"30");
    EXPECT_EQ(this->number_, 95);
    EXPECT_EQ(SendMessageW(this->Slider(), TBM_GETPOS, 0, 0), 95);
}

// 验证读取失败保留输入，明确刷新合法宿主值时同步两个控件且不产生变更。
// 入参：无。
// 返回：无；断言失败保护和只读刷新通知次数。
TEST_F(RendererEditTest, read_failure_preserves_input_and_explicit_refresh_does_not_notify)
{
    ASSERT_TRUE(SetWindowTextW(this->Edit(true), L"broken"));
    this->failRead_ = true;
    this->number_ = 22;
    this->text_ = "replacement";
    const int changes = this->numberChanges_;
    ASSERT_TRUE(this->renderer_.RefreshValues());
    EXPECT_EQ(this->Value(true), L"broken");
    EXPECT_EQ(this->Value(false), L"valid");
    this->failRead_ = false;
    ASSERT_TRUE(this->renderer_.RefreshValues());
    EXPECT_EQ(this->Value(true), L"22");
    EXPECT_EQ(this->Value(false), L"replacement");
    EXPECT_EQ(SendMessageW(this->Slider(), TBM_GETPOS, 0, 0), 22);
    EXPECT_EQ(this->numberChanges_, changes);
    EXPECT_EQ(this->textChanges_, 0);
}

// 验证忙及字段禁用同时禁用滑条与输入框，并忽略伪造的用户通知。
// 入参：无。
// 返回：无；断言原生启用状态和无业务回调。
TEST_F(RendererEditTest, busy_and_disabled_fields_ignore_edit_and_slider_notifications)
{
    ASSERT_TRUE(this->renderer_.SetBusy(true));
    EXPECT_FALSE(IsWindowEnabled(this->Edit(true)));
    EXPECT_FALSE(IsWindowEnabled(this->Slider()));
    SendMessageW(this->Viewport(), WM_COMMAND, MAKEWPARAM(102, EN_CHANGE), reinterpret_cast<LPARAM>(this->Edit(true)));
    SendMessageW(this->Viewport(), WM_HSCROLL, TB_THUMBTRACK, reinterpret_cast<LPARAM>(this->Slider()));
    EXPECT_EQ(this->numberChanges_, 0);
    ASSERT_TRUE(this->renderer_.SetEnabled("number", false));
    ASSERT_TRUE(this->renderer_.SetBusy(false));
    EXPECT_FALSE(IsWindowEnabled(this->Edit(true)));
    EXPECT_FALSE(IsWindowEnabled(this->Slider()));
    SendMessageW(this->Viewport(), WM_HSCROLL, TB_THUMBTRACK, reinterpret_cast<LPARAM>(this->Slider()));
    EXPECT_EQ(this->numberChanges_, 0);
    ASSERT_TRUE(this->renderer_.SetEnabled("number", true));
    EXPECT_TRUE(IsWindowEnabled(this->Edit(true)));
    EXPECT_TRUE(IsWindowEnabled(this->Slider()));
}

// 验证行内滑条与数字框共用同一高度，窗口变窄时均不超出视口。
// 入参：无。
// 返回：无；断言原生控件矩形边界。
TEST_F(RendererEditTest, integer_slider_and_edit_fit_row_in_normal_and_narrow_windows)
{
    RECT slider{};
    RECT edit{};
    RECT viewport{};
    GetWindowRect(this->Slider(), &slider);
    GetWindowRect(this->Edit(true), &edit);
    EXPECT_EQ(slider.top, edit.top);
    EXPECT_LE(slider.right, edit.left);
    const UINT dpi = GetDpiForWindow(this->renderer_.NativeHandle());
    EXPECT_EQ(edit.right - edit.left, MulDiv(64, static_cast<int>(dpi), 96));
    SetWindowPos(this->renderer_.NativeHandle(), nullptr, 0, 0, 180, 400, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    GetWindowRect(this->Slider(), &slider);
    GetWindowRect(this->Edit(true), &edit);
    GetWindowRect(this->Viewport(), &viewport);
    EXPECT_GE(slider.left, viewport.left);
    EXPECT_LE(slider.right, viewport.right);
    EXPECT_GE(edit.left, slider.right);
    EXPECT_LE(edit.right, viewport.right);
}
} // namespace
