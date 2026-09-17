// 文件职责：验证公共 WindowRenderer 承载的标注样式表单，按实际控件类型与文字操作，不猜控件 ID。
#include "annotation_style_dialog.h"
#include <commctrl.h>
#include <cwchar>
#include <gtest/gtest.h>
#include <stdexcept>
#include <vector>

namespace open_st
{
namespace
{
// 读取原生控件当前显示文本，不借用系统内部字符串。
// 入参：window 为当前测试窗口或控件。
// 返回：独立文字副本。
std::wstring ControlText(HWND window)
{
    const int length = GetWindowTextLengthW(window);
    std::wstring text(static_cast<std::size_t>(length) + 1U, L'\0');
    const int copied = GetWindowTextW(window, text.data(), length + 1);
    text.resize(static_cast<std::size_t>(copied));
    return text;
}

// 查询原生控件类别，比较时忽略系统返回的大小写差异。
// 入参：window 为控件；expected 为目标类名。
// 返回：类别匹配为 true。
bool ControlClass(HWND window, const wchar_t* expected)
{
    wchar_t name[128]{};
    GetClassNameW(window, name, static_cast<int>(std::size(name)));
    return _wcsicmp(name, expected) == 0;
}

// 向控件真实父窗口发送包含源 HWND 的原生通知。
// 入参：control 为通知源；notification 为 EN_CHANGE、CBN_SELCHANGE 或 BN_CLICKED。
// 返回：无。
void NotifyControl(HWND control, WORD notification)
{
    ASSERT_NE(control, nullptr);
    SendMessageW(GetParent(control), WM_COMMAND, MAKEWPARAM(static_cast<WORD>(GetDlgCtrlID(control)), notification),
                 reinterpret_cast<LPARAM>(control));
}

// 递归收集窗口子控件，随后用原生类型和文字匹配语义。
// 入参：window 为当前子控件；context 为接收 HWND 的向量。
// 返回：TRUE 继续枚举。
BOOL CALLBACK CollectControls(HWND window, LPARAM context)
{
    std::vector<HWND>& controls = *reinterpret_cast<std::vector<HWND>*>(context);
    controls.push_back(window);
    return TRUE;
}

// 查找当前可见文本精确匹配的子控件，不依赖布局中的数字 ID。
// 入参：window 为表单；text 为隔离测试文字。
// 返回：匹配控件的借用 HWND，未找到为空。
HWND FindText(HWND window, std::wstring_view text)
{
    std::vector<HWND> controls;
    EnumChildWindows(window, CollectControls, reinterpret_cast<LPARAM>(&controls));
    for (HWND control : controls)
    {
        if (IsWindowVisible(control) && ControlText(control) == text)
            return control;
    }
    return nullptr;
}

struct StyleControls
{
    HWND color{};
    HWND transparency{};
    HWND slider{};
    HWND width{};
    HWND fontSize{};
    HWND choice{};
    HWND editText{};
    HWND confirm{};
    HWND cancel{};
    HWND blue{};
};

class AnnotationStyleDialogTest : public testing::Test
{
  protected:
    // 创建独占宿主和线程窗口钩子，测试动作等公共表单完成创建后才执行。
    // 入参：无。
    // 返回：无；平台窗口失败按真实失败报告。
    void SetUp() override
    {
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = AnnotationStyleDialogTest::ControllerProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = L"OpenST.AnnotationStyleTest";
        ASSERT_TRUE(RegisterClassW(&windowClass) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS);
        this->owner_ = CreateWindowExW(0, windowClass.lpszClassName, L"", WS_POPUP, 0, 0, 100, 100, nullptr, nullptr,
                                       windowClass.hInstance, this);
        ASSERT_NE(this->owner_, nullptr);
        AnnotationStyleDialogTest::active_ = this;
        this->hook_ = SetWindowsHookExW(WH_CBT, AnnotationStyleDialogTest::Hook, nullptr, GetCurrentThreadId());
        ASSERT_NE(this->hook_, nullptr);
        ASSERT_NE(SetTimer(this->owner_, 1U, 2500U, nullptr), 0U);
    }

    // 移除线程钩子和独占窗口，消费本例留下的线程消息。
    // 入参：无。
    // 返回：无，不访问用户其他窗口。
    void TearDown() override
    {
        if (this->hook_ != nullptr)
            UnhookWindowsHookEx(this->hook_);
        AnnotationStyleDialogTest::active_ = nullptr;
        if (IsWindow(this->owner_))
            DestroyWindow(this->owner_);
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
        }
    }

    // 从真实控件类别和初始化文字识别业务字段，之后直接持有有效 HWND。
    // 入参：无。
    // 返回：无，缺少必需控件立即报告测试失败。
    void Locate()
    {
        this->controls_ = {};
        std::vector<HWND> controls;
        EnumChildWindows(this->dialog_, CollectControls, reinterpret_cast<LPARAM>(&controls));
        for (HWND control : controls)
        {
            if (ControlClass(control, L"EDIT"))
            {
                const std::wstring text = ControlText(control);
                // 公共 Integer 接受非法中间输入，当前与普通 Edit 一样没有 ES_NUMBER。
                if (!text.empty() && text.front() == L'#')
                    this->controls_.color = control;
                else
                    this->controls_.transparency = control;
            }
            else if (ControlClass(control, TRACKBAR_CLASSW))
                this->controls_.slider = control;
            else if (ControlClass(control, L"COMBOBOX"))
            {
                if (this->choiceMode_)
                    this->controls_.choice = control;
                else if (SendMessageW(control, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
                                      reinterpret_cast<LPARAM>(L"48")) >= 0 &&
                         SendMessageW(control, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
                                      reinterpret_cast<LPARAM>(L"12")) >= 0)
                    this->controls_.fontSize = control;
                else if (SendMessageW(control, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
                                      reinterpret_cast<LPARAM>(L"1")) >= 0 &&
                         SendMessageW(control, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
                                      reinterpret_cast<LPARAM>(L"5")) >= 0)
                    this->controls_.width = control;
            }
            else if (ControlClass(control, L"BUTTON"))
            {
                const std::wstring text = ControlText(control);
                if (text == L"annotation.style.ok")
                    this->controls_.confirm = control;
                else if (text == L"annotation.style.cancel")
                    this->controls_.cancel = control;
                else if (text == L"#0066FF")
                    this->controls_.blue = control;
                else if (text == L"annotation.text.edit")
                    this->controls_.editText = control;
            }
        }
        if (this->choiceMode_)
        {
            ASSERT_NE(this->controls_.choice, nullptr);
            ASSERT_NE(this->controls_.confirm, nullptr);
            ASSERT_NE(this->controls_.cancel, nullptr);
            return;
        }
        ASSERT_NE(this->controls_.color, nullptr);
        ASSERT_NE(this->controls_.transparency, nullptr);
        ASSERT_NE(this->controls_.slider, nullptr);
        ASSERT_NE(this->controls_.confirm, nullptr);
        ASSERT_NE(this->controls_.cancel, nullptr);
        ASSERT_NE(this->controls_.blue, nullptr);
        if (this->textMode_)
        {
            ASSERT_NE(this->controls_.fontSize, nullptr);
            EXPECT_EQ(this->controls_.width, nullptr);
        }
    }

    // 显示真实业务表单，使用文本键本身作为隔离文字，不读取用户配置。
    // 入参：value 为待编辑样式；allowWidth 控制线宽；preview 为同步预览检查。
    // 返回：业务结果，并检查窗口销毁和正常错误输出。
    AnnotationStyleResult Show(AnnotationStyle& value, bool allowWidth = true,
                               const AnnotationStylePreview& preview = {})
    {
        this->choiceMode_ = false;
        this->textMode_ = false;
        this->controls_ = {};
        this->dialog_ = nullptr;
        this->invoked_ = false;
        std::wstring error;
        const AnnotationStyleResult result = ShowAnnotationStyleDialog(
            this->owner_, value, allowWidth,
            // 使用可辨识的文本键，控件查找依据展示语义而非数字 ID。
            // 入参：key 为本地化键。
            // 返回：该键的宽字符副本。
            [](std::string_view key) { return std::wstring(key.begin(), key.end()); }, error, preview);
        EXPECT_TRUE(error.empty()) << error;
        EXPECT_FALSE(IsWindow(this->dialog_));
        EXPECT_TRUE(this->invoked_);
        return result;
    }

    // 显示文字专属样式表单，复用隔离文本和真实窗口定位。
    // 入参：style：颜色透明度；textStyle：字号和正文编辑许可；preview：完整文字样式预览。
    // 返回：业务结果，确认前保持调用方原值。
    AnnotationStyleResult ShowText(AnnotationStyle& style, AnnotationTextStyle& textStyle,
                                   const AnnotationTextStylePreview& preview = {})
    {
        this->choiceMode_ = false;
        this->textMode_ = true;
        this->controls_ = {};
        this->dialog_ = nullptr;
        this->invoked_ = false;
        std::wstring error;
        const AnnotationStyleResult result = ShowAnnotationTextStyleDialog(
            this->owner_, style, textStyle,
            // 文本键作为实际展示文字，方便按语义定位控件。
            // 入参：key：本地化键。
            // 返回：宽字符键名。
            [](std::string_view key) { return std::wstring(key.begin(), key.end()); }, error, preview);
        EXPECT_TRUE(error.empty()) << error;
        EXPECT_FALSE(IsWindow(this->dialog_));
        EXPECT_TRUE(this->invoked_);
        return result;
    }

    // 显示马赛克档位表单，验证原生下拉框与同步预览回调。
    // 入参：value：块大小；preview：候选准备结果。
    // 返回：确认、取消或失败。
    AnnotationStyleResult ShowMosaic(unsigned& value, const std::function<bool(unsigned)>& preview)
    {
        this->choiceMode_ = true;
        this->textMode_ = false;
        this->controls_ = {};
        this->dialog_ = nullptr;
        this->invoked_ = false;
        constexpr unsigned sizes[]{4U, 8U, 16U, 32U};
        std::wstring error;
        const AnnotationStyleResult result = ShowAnnotationChoiceDialog(
            this->owner_, value, sizes, "annotation.mosaic.title", "annotation.mosaic.size",
            // 使用键名而非依赖用户语言文件。
            // 入参：key：本地化键。
            // 返回：宽字符键名。
            [](std::string_view key) { return std::wstring(key.begin(), key.end()); }, error, preview);
        EXPECT_TRUE(error.empty()) << error;
        EXPECT_FALSE(IsWindow(this->dialog_));
        EXPECT_TRUE(this->invoked_);
        return result;
    }

    // 设置滑条位置，并向真实父窗口投递带源 HWND 的滑动通知。
    // 入参：value 为透明度百分比。
    // 返回：无。
    void Slide(unsigned value)
    {
        SendMessageW(this->controls_.slider, TBM_SETPOS, TRUE, value);
        SendMessageW(GetParent(this->controls_.slider), WM_HSCROLL, TB_THUMBPOSITION,
                     reinterpret_cast<LPARAM>(this->controls_.slider));
    }

    // 按显示文字选择线宽，不依赖选项排列位置。
    // 入参：text 为线宽文字。
    // 返回：无，找不到选项为测试失败。
    void Width(const wchar_t* text)
    {
        this->SelectCombo(this->choiceMode_ ? this->controls_.choice : this->controls_.width, text);
    }

    // 按实际显示文字选择任意已识别下拉控件，不依赖布局 ID 或排列下标。
    // 入参：control：目标控件；text：选项文字。
    // 返回：无，缺少控件或选项为测试失败。
    void SelectCombo(HWND control, const wchar_t* text)
    {
        ASSERT_NE(control, nullptr);
        const LRESULT index =
            SendMessageW(control, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(text));
        ASSERT_GE(index, 0);
        SendMessageW(control, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
        NotifyControl(control, CBN_SELCHANGE);
    }

    // 只识别当前宿主创建的公共 Renderer 窗口，并排队在初始化后执行测试。
    // 入参：code、wParam、lParam 为线程 CBT 钩子参数。
    // 返回：保留后续系统钩子处理。
    static LRESULT CALLBACK Hook(int code, WPARAM wParam, LPARAM lParam)
    {
        AnnotationStyleDialogTest* self = AnnotationStyleDialogTest::active_;
        if (code == HCBT_CREATEWND && self != nullptr)
        {
            const HWND window = reinterpret_cast<HWND>(wParam);
            const CBT_CREATEWNDW& creation = *reinterpret_cast<const CBT_CREATEWNDW*>(lParam);
            if (ControlClass(window, L"OpenST.WindowRenderer") && creation.lpcs->hwndParent == self->owner_)
            {
                self->dialog_ = window;
                PostMessageW(self->owner_, WM_APP + 1U, 0, 0);
            }
        }
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    // 执行排队的控件动作，超时仅作为自动测试失败退出保护。
    // 入参：window、message、wParam、lParam 为本地控制窗口消息。
    // 返回：已处理返回零，其余保持默认处理。
    static LRESULT CALLBACK ControllerProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        AnnotationStyleDialogTest* self =
            reinterpret_cast<AnnotationStyleDialogTest*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            self = static_cast<AnnotationStyleDialogTest*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self != nullptr && message == WM_APP + 1U)
        {
            self->invoked_ = true;
            self->Locate();
            if (!testing::Test::HasFatalFailure())
                self->action_();
            else
                SendMessageW(self->dialog_, WM_CLOSE, 0, 0);
            return 0;
        }
        if (self != nullptr && message == WM_TIMER)
        {
            ADD_FAILURE() << "Annotation style dialog test timed out";
            if (IsWindow(self->dialog_))
                SendMessageW(self->dialog_, WM_CLOSE, 0, 0);
            else
                PostQuitMessage(1);
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    inline static thread_local AnnotationStyleDialogTest* active_{};
    HWND owner_{};
    HWND dialog_{};
    HHOOK hook_{};
    bool invoked_{};
    bool choiceMode_{};
    bool textMode_{};
    StyleControls controls_;
    std::function<void()> action_;
};

// 验证预设色、自定义文本、透明滑条及线宽通过公共控件提交，宿主在模态后恢复启用。
// 入参：无。
// 返回：字段值与宿主启用状态断言。
TEST_F(AnnotationStyleDialogTest, native_controls_commit_and_restore_owner)
{
    // 经实际父子控件通知完成全部属性设置。
    // 入参：无，捕获本例控件。
    // 返回：无。
    this->action_ = [this]()
    {
        EXPECT_FALSE(IsWindowEnabled(this->owner_));
        NotifyControl(this->controls_.blue, BN_CLICKED);
        EXPECT_EQ(ControlText(this->controls_.color), L"#0066FF");
        SetWindowTextW(this->controls_.color, L"#12aB34");
        this->Slide(75U);
        this->Width(L"8");
        EXPECT_EQ(ControlText(this->controls_.transparency), L"75");
        EXPECT_TRUE(IsWindowEnabled(this->controls_.confirm));
        NotifyControl(this->controls_.confirm, BN_CLICKED);
    };
    AnnotationStyle value;
    EXPECT_EQ(this->Show(value), AnnotationStyleResult::Accepted);
    EXPECT_EQ(value.rgb, 0x12AB34U);
    EXPECT_EQ(value.transparency, 75U);
    EXPECT_EQ(value.lineWidth, 8.0);
    EXPECT_TRUE(IsWindowEnabled(this->owner_));
}

// 验证填充属性没有线宽控件，非法文字不被覆盖且禁止确认，取消不启用原本禁用的宿主。
// 入参：无。
// 返回：原始输入、取消原值与宿主状态断言。
TEST_F(AnnotationStyleDialogTest, hidden_width_invalid_text_and_cancel_preserve_values)
{
    EnableWindow(this->owner_, FALSE);
    // 输入不完整颜色及越界透明度，检查确认门禁后取消。
    // 入参：无。
    // 返回：无。
    this->action_ = [this]()
    {
        EXPECT_EQ(this->controls_.width, nullptr);
        SetWindowTextW(this->controls_.color, L"#123");
        SetWindowTextW(this->controls_.transparency, L"101");
        EXPECT_FALSE(IsWindowEnabled(this->controls_.confirm));
        NotifyControl(this->controls_.confirm, BN_CLICKED);
        EXPECT_TRUE(IsWindow(this->dialog_));
        EXPECT_EQ(ControlText(this->controls_.color), L"#123");
        EXPECT_EQ(ControlText(this->controls_.transparency), L"101");
        NotifyControl(this->controls_.cancel, BN_CLICKED);
    };
    AnnotationStyle value{0xABCDEFU, 25U, 5.0};
    EXPECT_EQ(this->Show(value, false), AnnotationStyleResult::Cancelled);
    EXPECT_EQ(value.rgb, 0xABCDEFU);
    EXPECT_EQ(value.transparency, 25U);
    EXPECT_EQ(value.lineWidth, 5.0);
    EXPECT_FALSE(IsWindowEnabled(this->owner_));
}

// 验证预设色只刷新颜色字段，不覆盖另一字段尚未完成的透明度文字。
// 入参：无。
// 返回：原文保留与完整输入恢复后才可确认的断言。
TEST_F(AnnotationStyleDialogTest, preset_preserves_unfinished_transparency)
{
    // 清空透明度后选预设色，随后补全透明度再确认。
    // 入参：无。
    // 返回：无。
    this->action_ = [this]()
    {
        SetWindowTextW(this->controls_.transparency, L"");
        NotifyControl(this->controls_.blue, BN_CLICKED);
        EXPECT_EQ(ControlText(this->controls_.color), L"#0066FF");
        EXPECT_EQ(ControlText(this->controls_.transparency), L"");
        EXPECT_FALSE(IsWindowEnabled(this->controls_.confirm));
        SetWindowTextW(this->controls_.transparency, L"45");
        EXPECT_TRUE(IsWindowEnabled(this->controls_.confirm));
        NotifyControl(this->controls_.confirm, BN_CLICKED);
    };
    AnnotationStyle value;
    EXPECT_EQ(this->Show(value), AnnotationStyleResult::Accepted);
    EXPECT_EQ(value.rgb, 0x0066FFU);
    EXPECT_EQ(value.transparency, 45U);
}

// 验证初始化不预览，全部有效变化各触发一次，同值通知与确认不重复发布。
// 入参：无。
// 返回：预览次数及最终属性断言。
TEST_F(AnnotationStyleDialogTest, valid_preview_changes_are_deduplicated)
{
    std::vector<AnnotationStyle> previews;
    // 复制预览候选，不保留同步借用引用。
    // 入参：candidate 为有效样式。
    // 返回：true 表示发布成功。
    const AnnotationStylePreview preview = [&previews](const AnnotationStyle& candidate)
    {
        previews.push_back(candidate);
        return true;
    };
    // 修改预设、滑条、线宽，并重复触发同值通知。
    // 入参：无。
    // 返回：无。
    this->action_ = [this, &previews]()
    {
        EXPECT_TRUE(previews.empty());
        NotifyControl(this->controls_.blue, BN_CLICKED);
        EXPECT_EQ(previews.size(), 1U);
        NotifyControl(this->controls_.color, EN_CHANGE);
        EXPECT_EQ(previews.size(), 1U);
        this->Slide(75U);
        EXPECT_EQ(previews.size(), 2U);
        this->Width(L"8");
        EXPECT_EQ(previews.size(), 3U);
        NotifyControl(this->controls_.confirm, BN_CLICKED);
        EXPECT_EQ(previews.size(), 3U);
    };
    AnnotationStyle value;
    EXPECT_EQ(this->Show(value, true, preview), AnnotationStyleResult::Accepted);
    ASSERT_EQ(previews.size(), 3U);
    EXPECT_EQ(previews.back().rgb, value.rgb);
    EXPECT_EQ(previews.back().transparency, value.transparency);
    EXPECT_EQ(previews.back().lineWidth, value.lineWidth);
}

// 验证非法中间输入保留上次成功预览，恢复同一颜色去重，取消不提交或延长回调存活期。
// 入参：无。
// 返回：预览次数与取消原子性断言。
TEST_F(AnnotationStyleDialogTest, invalid_input_keeps_last_preview_and_cancel_stops_callbacks)
{
    unsigned calls{};
    // 记录有效候选的发布次数。
    // 入参：candidate 为当前候选。
    // 返回：true。
    const AnnotationStylePreview preview = [&calls](const AnnotationStyle& candidate)
    {
        ++calls;
        EXPECT_EQ(candidate.rgb, 0x12AB34U);
        return true;
    };
    // 有效色、非法文本、同值恢复与取消按顺序执行。
    // 入参：无。
    // 返回：无。
    this->action_ = [this, &calls]()
    {
        SetWindowTextW(this->controls_.color, L"#12AB34");
        EXPECT_EQ(calls, 1U);
        SetWindowTextW(this->controls_.color, L"#12");
        EXPECT_EQ(calls, 1U);
        EXPECT_FALSE(IsWindowEnabled(this->controls_.confirm));
        SetWindowTextW(this->controls_.color, L"#12ab34");
        EXPECT_EQ(calls, 1U);
        EXPECT_TRUE(IsWindowEnabled(this->controls_.confirm));
        NotifyControl(this->controls_.cancel, BN_CLICKED);
    };
    AnnotationStyle value{0xABCDEFU, 25U, 5.0};
    EXPECT_EQ(this->Show(value, true, preview), AnnotationStyleResult::Cancelled);
    EXPECT_EQ(value.rgb, 0xABCDEFU);
    EXPECT_EQ(value.transparency, 25U);
    EXPECT_EQ(value.lineWidth, 5.0);
    EXPECT_FALSE(IsWindow(this->controls_.color));
    EXPECT_EQ(calls, 1U);
}

// 验证预览失败保留原显示、禁用确认并显示专用错误，同值有效输入可以重试。
// 入参：无。
// 返回：预览失败、恢复与提交值断言。
TEST_F(AnnotationStyleDialogTest, failed_preview_blocks_confirmation_and_input_retries)
{
    unsigned calls{};
    bool succeed{};
    AnnotationStyle visible;
    // 模拟宿主原子发布，失败不修改可见样式。
    // 入参：candidate 为有效候选。
    // 返回：succeed 决定是否发布。
    const AnnotationStylePreview preview = [&calls, &succeed, &visible](const AnnotationStyle& candidate)
    {
        ++calls;
        if (succeed)
            visible = candidate;
        return succeed;
    };
    // 先验证错误，再通过有效输入通知重试并确认。
    // 入参：无。
    // 返回：无。
    this->action_ = [this, &calls, &succeed, &visible]()
    {
        SetWindowTextW(this->controls_.color, L"#123456");
        EXPECT_EQ(calls, 1U);
        EXPECT_EQ(visible.rgb, 0xFF0000U);
        EXPECT_FALSE(IsWindowEnabled(this->controls_.confirm));
        EXPECT_NE(FindText(this->dialog_, L"annotation.style.preview_failed"), nullptr);
        NotifyControl(this->controls_.confirm, BN_CLICKED);
        EXPECT_EQ(calls, 1U);
        succeed = true;
        NotifyControl(this->controls_.color, EN_CHANGE);
        EXPECT_EQ(calls, 2U);
        EXPECT_TRUE(IsWindowEnabled(this->controls_.confirm));
        NotifyControl(this->controls_.confirm, BN_CLICKED);
        EXPECT_EQ(calls, 2U);
    };
    AnnotationStyle value;
    EXPECT_EQ(this->Show(value, true, preview), AnnotationStyleResult::Accepted);
    EXPECT_EQ(value.rgb, 0x123456U);
    EXPECT_EQ(visible.rgb, value.rgb);
}

// 验证预览异常被业务捕获，后续新输入可恢复确认而非穿过窗口过程。
// 入参：无。
// 返回：异常提示、恢复次数和最终属性断言。
TEST_F(AnnotationStyleDialogTest, preview_exception_is_contained_and_new_input_recovers)
{
    unsigned calls{};
    // 第一候选抛出异常，下一候选成功。
    // 入参：candidate 为有效样式。
    // 返回：恢复后为 true，第一次抛测试异常。
    const AnnotationStylePreview preview = [&calls](const AnnotationStyle& candidate)
    {
        ++calls;
        if (calls == 1U)
            throw std::runtime_error("preview failure");
        EXPECT_EQ(candidate.rgb, 0x654321U);
        return true;
    };
    // 在异常后继续输入不同颜色并完成确认。
    // 入参：无。
    // 返回：无。
    this->action_ = [this, &calls]()
    {
        SetWindowTextW(this->controls_.color, L"#123456");
        EXPECT_FALSE(IsWindowEnabled(this->controls_.confirm));
        EXPECT_NE(FindText(this->dialog_, L"annotation.style.preview_failed"), nullptr);
        SetWindowTextW(this->controls_.color, L"#654321");
        EXPECT_EQ(calls, 2U);
        EXPECT_TRUE(IsWindowEnabled(this->controls_.confirm));
        EXPECT_EQ(FindText(this->dialog_, L"annotation.style.preview_failed"), nullptr);
        NotifyControl(this->controls_.confirm, BN_CLICKED);
    };
    AnnotationStyle value;
    EXPECT_EQ(this->Show(value, true, preview), AnnotationStyleResult::Accepted);
    EXPECT_EQ(value.rgb, 0x654321U);
}

// 验证成功 A 后失败 B，再改回 A 时不能因同值去重跳过恢复预览，成功恢复后才允许确认。
// 入参：无。
// 返回：失败恢复回调次数与确认门禁断言。
TEST_F(AnnotationStyleDialogTest, returning_to_last_success_after_failure_retries_preview)
{
    unsigned calls{};
    bool recover{};
    AnnotationStyle visible;
    // 首次发布 A，拒绝 B 和未恢复状态的 A，恢复允许后再次发布 A。
    // 入参：candidate 为有效候选。
    // 返回：是否已原子发布预览。
    const AnnotationStylePreview preview = [&calls, &recover, &visible](const AnnotationStyle& candidate)
    {
        ++calls;
        const bool accepted = calls == 1U || recover;
        if (accepted)
            visible = candidate;
        return accepted;
    };
    // 先造成失败，再检查回到 A 仍要求宿主确认预览恢复。
    // 入参：无。
    // 返回：无。
    this->action_ = [this, &calls, &recover, &visible]()
    {
        SetWindowTextW(this->controls_.color, L"#123456");
        EXPECT_EQ(calls, 1U);
        EXPECT_EQ(visible.rgb, 0x123456U);
        SetWindowTextW(this->controls_.color, L"#654321");
        EXPECT_EQ(calls, 2U);
        EXPECT_FALSE(IsWindowEnabled(this->controls_.confirm));
        EXPECT_EQ(visible.rgb, 0x123456U);
        SetWindowTextW(this->controls_.color, L"#123456");
        EXPECT_EQ(calls, 3U);
        EXPECT_FALSE(IsWindowEnabled(this->controls_.confirm));
        recover = true;
        NotifyControl(this->controls_.color, EN_CHANGE);
        EXPECT_EQ(calls, 4U);
        EXPECT_TRUE(IsWindowEnabled(this->controls_.confirm));
        NotifyControl(this->controls_.confirm, BN_CLICKED);
        EXPECT_EQ(calls, 4U);
    };
    AnnotationStyle value;
    EXPECT_EQ(this->Show(value, true, preview), AnnotationStyleResult::Accepted);
    EXPECT_EQ(value.rgb, 0x123456U);
    EXPECT_EQ(value.rgb, visible.rgb);
}

// 验证未编辑的初始样式可以直接确认，不产生不必要的同步预览。
// 入参：无。
// 返回：零回调次数和原样式保持断言。
TEST_F(AnnotationStyleDialogTest, unchanged_initial_style_does_not_preview)
{
    unsigned calls{};
    // 记录不应发生的冗余预览。
    // 入参：未命名参数为样式候选。
    // 返回：true。
    const AnnotationStylePreview preview = [&calls](const AnnotationStyle&)
    {
        ++calls;
        return true;
    };
    // 发送同值通知后直接确认。
    // 入参：无。
    // 返回：无。
    this->action_ = [this, &calls]()
    {
        EXPECT_EQ(calls, 0U);
        NotifyControl(this->controls_.color, EN_CHANGE);
        NotifyControl(this->controls_.confirm, BN_CLICKED);
        EXPECT_EQ(calls, 0U);
    };
    AnnotationStyle value{0x123456U, 25U, 5.0};
    EXPECT_EQ(this->Show(value, false, preview), AnnotationStyleResult::Accepted);
    EXPECT_EQ(value.rgb, 0x123456U);
    EXPECT_EQ(value.lineWidth, 5.0);
}

// 验证 Esc 经公共模态按键路由取消表单，标题栏关闭同样保持入口值。
// 入参：无。
// 返回：两种关闭方式的取消结果和原值断言。
TEST_F(AnnotationStyleDialogTest, escape_and_window_close_cancel_without_committing)
{
    for (bool escape : {true, false})
    {
        // 修改草稿后模拟 Esc 消息或标题栏关闭。
        // 入参：无，escape 决定退出路径。
        // 返回：无。
        this->action_ = [this, escape]()
        {
            SetWindowTextW(this->controls_.color, L"#123456");
            if (escape)
                PostMessageW(this->controls_.color, WM_KEYDOWN, VK_ESCAPE, 0);
            else
                SendMessageW(this->dialog_, WM_CLOSE, 0, 0);
        };
        AnnotationStyle value;
        EXPECT_EQ(this->Show(value), AnnotationStyleResult::Cancelled);
        EXPECT_EQ(value.rgb, 0xFF0000U);
    }
}

// 验证公共模态循环消费退出消息后原样转发，且退出不提交样式并恢复宿主。
// 入参：无。
// 返回：退出码、取消结果和宿主恢复断言。
TEST_F(AnnotationStyleDialogTest, quit_is_forwarded_after_modal_cleanup)
{
    // 在表单内部请求当前线程退出。
    // 入参：无。
    // 返回：无。
    this->action_ = []() { PostQuitMessage(37); };
    AnnotationStyle value{0x123456U, 25U, 5.0};
    EXPECT_EQ(this->Show(value), AnnotationStyleResult::Cancelled);
    EXPECT_EQ(value.rgb, 0x123456U);
    EXPECT_TRUE(IsWindowEnabled(this->owner_));
    MSG message{};
    bool quit{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    {
        if (message.message == WM_QUIT)
        {
            quit = true;
            break;
        }
        DispatchMessageW(&message);
    }
    EXPECT_TRUE(quit);
    EXPECT_EQ(message.wParam, 37U);
}

// 验证预览回调内关闭不能被外层确认覆盖，原始样式仍未提交。
// 入参：无。
// 返回：重入关闭的取消结果及回调次数断言。
TEST_F(AnnotationStyleDialogTest, close_inside_preview_remains_cancelled)
{
    unsigned calls{};
    // 预览期间请求关闭并返回成功，模拟会话要求结束属性表单。
    // 入参：未命名参数为有效候选。
    // 返回：true，取消意图仍须保留。
    const AnnotationStylePreview preview = [this, &calls](const AnnotationStyle&)
    {
        ++calls;
        SendMessageW(this->dialog_, WM_CLOSE, 0, 0);
        return true;
    };
    // 输入触发关闭后再发送确认，确认不得覆盖关闭结果。
    // 入参：无。
    // 返回：无。
    this->action_ = [this]()
    {
        SetWindowTextW(this->controls_.color, L"#123456");
        NotifyControl(this->controls_.confirm, BN_CLICKED);
    };
    AnnotationStyle value;
    EXPECT_EQ(this->Show(value, true, preview), AnnotationStyleResult::Cancelled);
    EXPECT_EQ(value.rgb, 0xFF0000U);
    EXPECT_EQ(calls, 1U);
}
// 验证橡皮直径的公共离散表单只在确认时发布允许档位。
// 入参：无。
// 返回：无；取消保留原值，不需要伪造标注样式字段。
TEST_F(AnnotationStyleDialogTest, discrete_brush_diameter_accepts_and_cancels)
{
    this->choiceMode_ = true;
    constexpr unsigned sizes[]{8, 16, 32, 64};
    for (bool accept : {false, true})
    {
        unsigned value = 16;
        this->action_ = [this, accept]()
        {
            this->Width(L"32");
            NotifyControl(accept ? this->controls_.confirm : this->controls_.cancel, BN_CLICKED);
            NotifyControl(accept ? this->controls_.cancel : this->controls_.confirm, BN_CLICKED);
        };
        std::wstring error;
        const AnnotationStyleResult result = ShowAnnotationChoiceDialog(
            this->owner_, value, sizes, "annotation.eraser.title", "annotation.eraser.diameter",
            // 用键名观察真实控件，不读取用户设置。
            // 入参：key为文本键。返回：隔离文字。
            [](std::string_view key) { return std::wstring(key.begin(), key.end()); }, error);
        EXPECT_EQ(result, accept ? AnnotationStyleResult::Accepted : AnnotationStyleResult::Cancelled);
        EXPECT_EQ(value, accept ? 32U : 16U);
        EXPECT_TRUE(error.empty());
    }
}
// 验证文字字号实时预览与颜色透明度共用完整候选，取消保留入口值且不展示线宽。
// 入参：无。
// 返回：字号回调、控件类型与取消原子性断言。
TEST_F(AnnotationStyleDialogTest, text_font_previews_and_cancel_preserves_original_properties)
{
    unsigned calls{};
    unsigned displayedFont{};
    // 记录完整文字属性预览中的字号。
    // 入参：style：样式；fontSize：物理字号。
    // 返回：true。
    const AnnotationTextStylePreview preview = [&calls, &displayedFont](const AnnotationStyle& style, unsigned fontSize)
    {
        ++calls;
        displayedFont = fontSize;
        EXPECT_EQ(style.rgb, 0x123456U);
        return true;
    };
    // 选择新字号后重复通知，确认不重复预览，再取消。
    // 入参：无。
    // 返回：无。
    this->action_ = [this, &calls, &displayedFont]()
    {
        EXPECT_EQ(this->controls_.width, nullptr);
        EXPECT_EQ(this->controls_.editText, nullptr);
        EXPECT_EQ(calls, 0U);
        this->SelectCombo(this->controls_.fontSize, L"48");
        EXPECT_EQ(calls, 1U);
        EXPECT_EQ(displayedFont, 48U);
        NotifyControl(this->controls_.fontSize, CBN_SELCHANGE);
        EXPECT_EQ(calls, 1U);
        NotifyControl(this->controls_.cancel, BN_CLICKED);
    };
    AnnotationStyle style{0x123456U, 25U, 5.0};
    AnnotationTextStyle textStyle{24U, false, false};
    EXPECT_EQ(this->ShowText(style, textStyle, preview), AnnotationStyleResult::Cancelled);
    EXPECT_EQ(textStyle.fontSize, 24U);
    EXPECT_FALSE(textStyle.editRequested);
    EXPECT_EQ(style.rgb, 0x123456U);
    EXPECT_EQ(style.lineWidth, 5.0);
}

// 验证编辑正文按钮先确认完整属性并发布编辑意图，不重复已有成功预览。
// 入参：无。
// 返回：确认值、编辑意图与回调去重断言。
TEST_F(AnnotationStyleDialogTest, text_edit_intent_accepts_properties_without_duplicate_preview)
{
    unsigned calls{};
    // 检查字体变化和颜色变化都携带完整属性。
    // 入参：style：有效样式；fontSize：候选字号。
    // 返回：true。
    const AnnotationTextStylePreview preview = [&calls](const AnnotationStyle& style, unsigned fontSize)
    {
        ++calls;
        EXPECT_EQ(fontSize, 32U);
        EXPECT_EQ(style.transparency, 25U);
        return true;
    };
    // 预览后点击正文编辑，只请求下一步原位编辑而不在表单中创建正文控件。
    // 入参：无。
    // 返回：无。
    this->action_ = [this, &calls]()
    {
        ASSERT_NE(this->controls_.editText, nullptr);
        this->SelectCombo(this->controls_.fontSize, L"32");
        SetWindowTextW(this->controls_.color, L"#654321");
        EXPECT_EQ(calls, 2U);
        NotifyControl(this->controls_.editText, BN_CLICKED);
        EXPECT_EQ(calls, 2U);
    };
    AnnotationStyle style{0x123456U, 25U, 5.0};
    AnnotationTextStyle textStyle{24U, true, false};
    EXPECT_EQ(this->ShowText(style, textStyle, preview), AnnotationStyleResult::Accepted);
    EXPECT_EQ(style.rgb, 0x654321U);
    EXPECT_EQ(textStyle.fontSize, 32U);
    EXPECT_TRUE(textStyle.editRequested);
    EXPECT_EQ(calls, 2U);
}

// 验证文字预览失败同时禁止确认和编辑正文，恢复有效输入后可普通确认而不发正文意图。
// 入参：无。
// 返回：双按钮门禁、失败恢复和普通确认意图断言。
TEST_F(AnnotationStyleDialogTest, text_preview_failure_blocks_edit_intent_until_retry)
{
    bool ready{};
    unsigned calls{};
    // 用宿主失败模拟字号预览不能发布。
    // 入参：未命名样式与字号为有效候选。
    // 返回：ready。
    const AnnotationTextStylePreview preview = [&ready, &calls](const AnnotationStyle&, unsigned)
    {
        ++calls;
        return ready;
    };
    // 失败后两入口都不得关闭表单，重复输入恢复后普通确认。
    // 入参：无。
    // 返回：无。
    this->action_ = [this, &ready, &calls]()
    {
        this->SelectCombo(this->controls_.fontSize, L"48");
        EXPECT_FALSE(IsWindowEnabled(this->controls_.confirm));
        EXPECT_FALSE(IsWindowEnabled(this->controls_.editText));
        NotifyControl(this->controls_.editText, BN_CLICKED);
        EXPECT_EQ(calls, 1U);
        ready = true;
        NotifyControl(this->controls_.fontSize, CBN_SELCHANGE);
        EXPECT_TRUE(IsWindowEnabled(this->controls_.confirm));
        EXPECT_TRUE(IsWindowEnabled(this->controls_.editText));
        NotifyControl(this->controls_.confirm, BN_CLICKED);
        EXPECT_EQ(calls, 2U);
    };
    AnnotationStyle style;
    AnnotationTextStyle textStyle{24U, true, false};
    EXPECT_EQ(this->ShowText(style, textStyle, preview), AnnotationStyleResult::Accepted);
    EXPECT_EQ(textStyle.fontSize, 48U);
    EXPECT_FALSE(textStyle.editRequested);
}

// 验证马赛克档位失败不允许确认，回到旧成功值也必须重新预览，恢复后一次确认。
// 入参：无。
// 返回：档位失败保护、重试次数和最终值断言。
TEST_F(AnnotationStyleDialogTest, mosaic_choice_preview_failure_requires_successful_retry)
{
    unsigned calls{};
    unsigned displayed{16U};
    bool ready{};
    // 首次8成功，32失败，回8仍失败，明确恢复后才发布。
    // 入参：size：候选块大小。
    // 返回：首次或 ready 时成功。
    const std::function<bool(unsigned)> preview = [&calls, &displayed, &ready](unsigned size)
    {
        ++calls;
        if (calls == 1U || ready)
        {
            displayed = size;
            return true;
        }
        return false;
    };
    // 通过实际选项文字选择候选，并断言失败不覆盖上次可见档位。
    // 入参：无。
    // 返回：无。
    this->action_ = [this, &calls, &displayed, &ready]()
    {
        this->Width(L"8");
        EXPECT_EQ(calls, 1U);
        this->Width(L"32");
        EXPECT_EQ(displayed, 8U);
        EXPECT_FALSE(IsWindowEnabled(this->controls_.confirm));
        EXPECT_NE(FindText(this->dialog_, L"annotation.style.preview_failed"), nullptr);
        NotifyControl(this->controls_.confirm, BN_CLICKED);
        EXPECT_EQ(calls, 2U);
        this->Width(L"8");
        EXPECT_EQ(calls, 3U);
        EXPECT_FALSE(IsWindowEnabled(this->controls_.confirm));
        ready = true;
        NotifyControl(this->controls_.choice, CBN_SELCHANGE);
        EXPECT_EQ(calls, 4U);
        EXPECT_TRUE(IsWindowEnabled(this->controls_.confirm));
        NotifyControl(this->controls_.confirm, BN_CLICKED);
        EXPECT_EQ(calls, 4U);
    };
    unsigned value{16U};
    EXPECT_EQ(this->ShowMosaic(value, preview), AnnotationStyleResult::Accepted);
    EXPECT_EQ(value, 8U);
}

// 验证马赛克已经成功预览后取消仍不修改入口块大小。
// 入参：无。
// 返回：取消原子性与成功预览次数断言。
TEST_F(AnnotationStyleDialogTest, mosaic_choice_preview_cancel_keeps_original_value)
{
    unsigned calls{};
    // 记录有效档位预览。
    // 入参：size：候选块大小。
    // 返回：true。
    const std::function<bool(unsigned)> preview = [&calls](unsigned size)
    {
        ++calls;
        EXPECT_EQ(size, 4U);
        return true;
    };
    // 预览细档位，再取消本次属性窗口。
    // 入参：无。
    // 返回：无。
    this->action_ = [this]()
    {
        this->Width(L"4");
        NotifyControl(this->controls_.cancel, BN_CLICKED);
    };
    unsigned value{16U};
    EXPECT_EQ(this->ShowMosaic(value, preview), AnnotationStyleResult::Cancelled);
    EXPECT_EQ(value, 16U);
    EXPECT_EQ(calls, 1U);
}
} // namespace
} // namespace open_st
