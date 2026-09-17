// 验证真实 RichEdit 的输入、事务回调、裁剪和生命周期，不访问真实剪贴板。
#include <windows.h>

#include "inline_text_editor_test_access.h"
#include <commctrl.h>
#include <gtest/gtest.h>
#include <inline_text_editor.h>
#include <richedit.h>
#include <string>

namespace
{
// 拦截粘贴消息而不访问用户剪贴板。入参：系统 subclass 参数。返回：粘贴直接消费。
LRESULT CALLBACK ObservePaste(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data)
{
    if (message == WM_PASTE)
    {
        ++*reinterpret_cast<int*>(data);
        return 0;
    }
    if (message == WM_NCDESTROY)
        RemoveWindowSubclass(window, ObservePaste, 42);
    return DefSubclassProc(window, message, wParam, lParam);
}
class InlineTextEditorTest : public testing::Test
{
  protected:
    HWND owner{};
    open_st::InlineTextEditor editor;
    open_st::InlineTextOptions options;
    int changes{}, finishes{};
    bool permitChange{true}, permitFinish{true}, accepted{};
    std::wstring last;
    // 创建隔离宿主。入参：无。返回：无。
    void SetUp() override
    {
        owner = CreateWindowExW(0, L"STATIC", L"Inline editor test", WS_POPUP, 100, 100, 400, 300, nullptr, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
        ASSERT_NE(owner, nullptr);
        options.owner = owner;
        options.bounds = {110, 110, 410, 280};
        options.clip = {120, 120, 400, 260};
    }
    // 释放原生窗口。入参：无。返回：无。
    void TearDown() override
    {
        editor.Close();
        if (IsWindow(owner))
            DestroyWindow(owner);
    }
    // 打开组件并计数同步回调。入参：无。返回：接口结果。
    open_st::RendererResult Open()
    {
        return editor.Open(options, {// 保存观测值，拒绝不改控件内容。入参：text。返回：测试准入值。
                                     [this](std::wstring_view text)
                                     {
                                         ++changes;
                                         last = text;
                                         return permitChange;
                                     },
                                     // 模拟宿主已经排队收尾。入参：text、accept。返回：测试准入值。
                                     [this](std::wstring_view text, bool accept)
                                     {
                                         ++finishes;
                                         last = text;
                                         accepted = accept;
                                         return permitFinish;
                                     }});
    }
    // 获取原生编辑子窗。入参：无。返回：借用句柄。
    HWND Edit() const
    {
        return GetDlgItem(editor.NativeHandle(), 1);
    }
    // 读取控件正文。入参：无。返回：完整字符串。
    std::wstring Text() const
    {
        const int count = GetWindowTextLengthW(Edit());
        std::wstring text(static_cast<std::size_t>(count) + 1, L'\0');
        text.resize(static_cast<std::size_t>(GetWindowTextW(Edit(), text.data(), count + 1)));
        return text;
    }
};

// 初始文字不触发编辑回调，采用纯文本和多级撤销。入参：无。返回：测试断言。
TEST_F(InlineTextEditorTest, opens_plain_text_without_initial_callback)
{
    options.text = L"中文 日本語";
    ASSERT_TRUE(Open());
    EXPECT_EQ(Text(), options.text);
    EXPECT_EQ(changes, 0);
    const LRESULT mode = SendMessageW(Edit(), EM_GETTEXTMODE, 0, 0);
    EXPECT_NE(mode & TM_PLAINTEXT, 0);
    EXPECT_NE(mode & TM_MULTILEVELUNDO, 0);
}
// 裁剪不改变完整布局，跨 DPI 消息不放大物理边界。入参：无。返回：测试断言。
TEST_F(InlineTextEditorTest, clip_preserves_full_physical_bounds)
{
    ASSERT_TRUE(Open());
    RECT bounds{};
    ASSERT_TRUE(GetWindowRect(editor.NativeHandle(), &bounds));
    EXPECT_EQ(bounds.left, options.bounds.left);
    EXPECT_EQ(bounds.right, options.bounds.right);
    HRGN region = CreateRectRgn(0, 0, 0, 0);
    ASSERT_NE(GetWindowRgn(editor.NativeHandle(), region), ERROR);
    RECT clip{};
    GetRgnBox(region, &clip);
    DeleteObject(region);
    EXPECT_EQ(clip.left, options.clip.left - options.bounds.left);
    RECT proposed{0, 0, 900, 900};
    SendMessageW(editor.NativeHandle(), WM_DPICHANGED, MAKELONG(144, 144), reinterpret_cast<LPARAM>(&proposed));
    GetWindowRect(editor.NativeHandle(), &bounds);
    EXPECT_EQ(bounds.right, options.bounds.right);
}
// 确认只通知一次，重复请求不会重复提交。入参：无。返回：测试断言。
TEST_F(InlineTextEditorTest, accepts_finish_once)
{
    ASSERT_TRUE(Open());
    SendMessageW(Edit(), EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"hello"));
    EXPECT_EQ(editor.RequestCommit(), open_st::InlineTextRequest::Queued);
    EXPECT_EQ(editor.RequestCommit(), open_st::InlineTextRequest::Finished);
    EXPECT_EQ(finishes, 1);
    EXPECT_TRUE(accepted);
    EXPECT_EQ(last, L"hello");
}
// 业务拒绝时保留原文，取消不依赖预览成功。入参：无。返回：测试断言。
TEST_F(InlineTextEditorTest, rejected_change_keeps_text_and_allows_cancel)
{
    ASSERT_TRUE(Open());
    permitChange = false;
    SendMessageW(Edit(), EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"bad"));
    EXPECT_EQ(Text(), L"bad");
    EXPECT_EQ(editor.RequestCommit(), open_st::InlineTextRequest::Rejected);
    EXPECT_EQ(finishes, 0);
    EXPECT_EQ(editor.RequestCancel(), open_st::InlineTextRequest::Queued);
    EXPECT_FALSE(accepted);
}
// 组合输入阻止确认但程序取消允许退出。入参：无。返回：测试断言。
TEST_F(InlineTextEditorTest, composing_rejects_commit_but_programmatic_cancel_finishes)
{
    ASSERT_TRUE(Open());
    SendMessageW(Edit(), WM_IME_STARTCOMPOSITION, 0, 0);
    EXPECT_TRUE(editor.IsComposing());
    EXPECT_EQ(editor.RequestCommit(), open_st::InlineTextRequest::Rejected);
    EXPECT_EQ(editor.RequestCancel(), open_st::InlineTextRequest::Queued);
    EXPECT_FALSE(accepted);
}
// 单次超限替换全量拒绝，不能静默截断。入参：无。返回：测试断言。
TEST_F(InlineTextEditorTest, over_limit_replace_is_atomic)
{
    options.maxLength = 4;
    ASSERT_TRUE(Open());
    SendMessageW(Edit(), EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"abc"));
    SendMessageW(Edit(), EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"de"));
    EXPECT_EQ(Text(), L"abc");
    SendMessageW(Edit(), EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"d"));
    EXPECT_EQ(Text(), L"abcd");
}
// 使用 RichEdit 自带撤销重做，不引入业务历史。入参：无。返回：测试断言。
TEST_F(InlineTextEditorTest, native_undo_redo_preserves_text)
{
    ASSERT_TRUE(Open());
    SendMessageW(Edit(), EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"文字"));
    ASSERT_TRUE(SendMessageW(Edit(), EM_CANUNDO, 0, 0));
    ASSERT_TRUE(SendMessageW(Edit(), EM_UNDO, 0, 0));
    EXPECT_TRUE(Text().empty());
    ASSERT_TRUE(SendMessageW(Edit(), EM_REDO, 0, 0));
    EXPECT_EQ(Text(), L"文字");
}
// 排队后失败恢复保留同一编辑实例和原生撤销。入参：无。返回：测试断言。
TEST_F(InlineTextEditorTest, resume_retains_native_history_without_change_callback)
{
    ASSERT_TRUE(Open());
    const HWND original = Edit();
    SendMessageW(Edit(), EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"retry"));
    ASSERT_EQ(editor.RequestCommit(), open_st::InlineTextRequest::Queued);
    const int previousChanges = changes;
    editor.Resume();
    EXPECT_EQ(Edit(), original);
    EXPECT_EQ(changes, previousChanges);
    ASSERT_TRUE(SendMessageW(Edit(), EM_UNDO, 0, 0));
    EXPECT_TRUE(Text().empty());
    EXPECT_EQ(editor.RequestCommit(), open_st::InlineTextRequest::Queued);
    EXPECT_EQ(finishes, 2);
}
// 排队后程序替换与原生撤销同样冻结，恢复后保留原生历史。入参：无。返回：测试断言。
TEST_F(InlineTextEditorTest, queued_finish_freezes_native_mutations_until_resume)
{
    ASSERT_TRUE(Open());
    SendMessageW(Edit(), EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"fixed"));
    ASSERT_EQ(editor.RequestCommit(), open_st::InlineTextRequest::Queued);
    SendMessageW(Edit(), EM_UNDO, 0, 0);
    SendMessageW(Edit(), EM_REDO, 0, 0);
    SendMessageW(Edit(), WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L"wrong"));
    SendMessageW(Edit(), EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"wrong"));
    SendMessageW(Edit(), WM_CHAR, L'x', 0);
    EXPECT_EQ(Text(), L"fixed");
    editor.Resume();
    ASSERT_TRUE(SendMessageW(Edit(), EM_UNDO, 0, 0));
    EXPECT_TRUE(Text().empty());
}
// 模拟父子DPI通知后保持字号、完整矩形、正文、原生选区和撤销。入参：无。返回：测试断言。
TEST_F(InlineTextEditorTest, dpi_notifications_preserve_font_text_selection_and_undo)
{
    ASSERT_TRUE(Open());
    SendMessageW(Edit(), EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"physical"));
    CHARRANGE selected{1, 4};
    SendMessageW(Edit(), EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&selected));
    // RichEdit 不保证 WM_GETFONT 返回 HFONT；读取它实际应用的字号格式。
    CHARFORMAT2W beforeDescription{};
    beforeDescription.cbSize = sizeof(beforeDescription);
    SendMessageW(Edit(), EM_GETCHARFORMAT, SCF_DEFAULT, reinterpret_cast<LPARAM>(&beforeDescription));
    ASSERT_NE(beforeDescription.dwMask & CFM_SIZE, 0U);
    ASSERT_GT(beforeDescription.yHeight, 0);
    const HDC dc = GetDC(Edit());
    ASSERT_NE(dc, nullptr);
    const int dpiY = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(Edit(), dc);
    ASSERT_GT(dpiY, 0);
    EXPECT_EQ(MulDiv(beforeDescription.yHeight, dpiY, 1440), static_cast<int>(options.fontPixelHeight));
    RECT bounds{};
    GetWindowRect(editor.NativeHandle(), &bounds);
    RECT proposed{0, 0, 1200, 800};
    SendMessageW(Edit(), WM_DPICHANGED_BEFOREPARENT, 0, 0);
    SendMessageW(editor.NativeHandle(), WM_DPICHANGED, MAKELONG(192, 192), reinterpret_cast<LPARAM>(&proposed));
    SendMessageW(Edit(), WM_DPICHANGED_AFTERPARENT, 0, 0);
    CHARFORMAT2W afterDescription{};
    afterDescription.cbSize = sizeof(afterDescription);
    SendMessageW(Edit(), EM_GETCHARFORMAT, SCF_DEFAULT, reinterpret_cast<LPARAM>(&afterDescription));
    ASSERT_NE(afterDescription.dwMask & CFM_SIZE, 0U);
    EXPECT_EQ(beforeDescription.yHeight, afterDescription.yHeight);
    EXPECT_EQ(MulDiv(afterDescription.yHeight, dpiY, 1440), static_cast<int>(options.fontPixelHeight));
    RECT actual{};
    GetWindowRect(editor.NativeHandle(), &actual);
    EXPECT_EQ(actual.left, bounds.left);
    EXPECT_EQ(actual.right, bounds.right);
    CHARRANGE actualSelection{};
    SendMessageW(Edit(), EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&actualSelection));
    EXPECT_EQ(actualSelection.cpMin, 1);
    EXPECT_EQ(actualSelection.cpMax, 4);
    EXPECT_EQ(Text(), L"physical");
    ASSERT_TRUE(SendMessageW(Edit(), EM_UNDO, 0, 0));
    EXPECT_TRUE(Text().empty());
}
// LF 和 CRLF 输入均以 LF 通知宿主，原生选择范围不受双字符换行影响。入参：无。返回：测试断言。
TEST_F(InlineTextEditorTest, multiline_callbacks_use_lf_and_selection_limit_is_consistent)
{
    options.maxLength = 5;
    ASSERT_TRUE(Open());
    SendMessageW(Edit(), EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"a\r\nb"));
    EXPECT_EQ(last, L"a\nb");
    CHARRANGE selected{2, 3};
    SendMessageW(Edit(), EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&selected));
    SendMessageW(Edit(), EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"xyz"));
    EXPECT_EQ(last, L"a\nxyz");
}
// 析构或关闭 owner 不提交业务，关闭可重复调用。入参：无。返回：测试断言。
TEST_F(InlineTextEditorTest, owner_destruction_does_not_commit)
{
    ASSERT_TRUE(Open());
    DestroyWindow(owner);
    EXPECT_EQ(editor.NativeHandle(), nullptr);
    EXPECT_EQ(finishes, 0);
    editor.Close();
    editor.Close();
}
// 初值超过预算或含未配对代理项在创建窗口前被拒绝。入参：无。返回：测试断言。
TEST_F(InlineTextEditorTest, rejects_invalid_initial_text)
{
    options.text = std::wstring(1, static_cast<wchar_t>(0xd800));
    EXPECT_FALSE(Open());
    EXPECT_EQ(editor.NativeHandle(), nullptr);
    options.text = L"too long";
    options.maxLength = 2;
    EXPECT_FALSE(Open());
}
// 长行不因控件宽度自动生成显示行。入参：无。返回：测试断言。
TEST_F(InlineTextEditorTest, long_line_does_not_soft_wrap)
{
    options.text = std::wstring(160, L'A');
    ASSERT_TRUE(Open());
    EXPECT_EQ(SendMessageW(Edit(), EM_GETLINECOUNT, 0, 0), 1);
    const std::wstring twoLines = options.text + L"\r\n" + options.text;
    ASSERT_TRUE(SetWindowTextW(Edit(), twoLines.c_str()));
    EXPECT_EQ(SendMessageW(Edit(), EM_GETLINECOUNT, 0, 0), 2);
    RECT suggested{0, 0, 700, 500};
    SendMessageW(editor.NativeHandle(), WM_DPICHANGED, MAKELONG(144, 144), reinterpret_cast<LPARAM>(&suggested));
    EXPECT_EQ(SendMessageW(Edit(), EM_GETLINECOUNT, 0, 0), 2);
}
// 转发共用原生选择、撤销重做和粘贴路径。入参：无。返回：测试断言。
TEST_F(InlineTextEditorTest, forwarded_commands_use_native_edit_paths)
{
    ASSERT_TRUE(Open());
    open_st::InlineTextEditorTestAccess::SetForegroundQuery(editor,
                                                            // 确定性提供前台身份，避免后台测试抢占用户前台。
                                                            // 入参：无。返回：本测试输入宿主。
                                                            [this]() { return editor.NativeHandle(); });
    editor.Focus();
    ASSERT_EQ(GetFocus(), Edit());
    SendMessageW(Edit(), EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"commands"));
    ASSERT_TRUE(editor.InvokeEditCommand(open_st::InlineEditCommand::SelectAll));
    CHARRANGE selected{};
    SendMessageW(Edit(), EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&selected));
    EXPECT_EQ(selected.cpMin, 0);
    EXPECT_EQ(selected.cpMax, 8);
    ASSERT_TRUE(editor.InvokeEditCommand(open_st::InlineEditCommand::Undo));
    EXPECT_TRUE(Text().empty());
    ASSERT_TRUE(editor.InvokeEditCommand(open_st::InlineEditCommand::Redo));
    EXPECT_EQ(Text(), L"commands");
    int pastes{};
    ASSERT_TRUE(SetWindowSubclass(Edit(), ObservePaste, 42, reinterpret_cast<DWORD_PTR>(&pastes)));
    EXPECT_TRUE(editor.InvokeEditCommand(open_st::InlineEditCommand::Paste));
    EXPECT_EQ(pastes, 1);
    RemoveWindowSubclass(Edit(), ObservePaste, 42);
}
// 非焦点、IME组合和结束锁期间不分派编辑。入参：无。返回：测试断言。
TEST_F(InlineTextEditorTest, forwarded_commands_obey_focus_composition_and_finish_gates)
{
    ASSERT_TRUE(Open());
    bool foreground = true;
    open_st::InlineTextEditorTestAccess::SetForegroundQuery(editor,
                                                            // 同线程焦点保留时，外部前台仍必须拒绝编辑。
                                                            // 入参：无。返回：受控前台身份。
                                                            [this, &foreground]()
                                                            { return foreground ? editor.NativeHandle() : owner; });
    SendMessageW(Edit(), EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"safe"));
    foreground = false;
    EXPECT_FALSE(editor.InvokeEditCommand(open_st::InlineEditCommand::Undo));
    EXPECT_EQ(Text(), L"safe");
    foreground = true;
    SetFocus(owner);
    EXPECT_FALSE(editor.InvokeEditCommand(open_st::InlineEditCommand::Undo));
    EXPECT_EQ(Text(), L"safe");
    editor.Focus();
    ASSERT_EQ(GetFocus(), Edit());
    SendMessageW(Edit(), WM_IME_STARTCOMPOSITION, 0, 0);
    EXPECT_FALSE(editor.InvokeEditCommand(open_st::InlineEditCommand::SelectAll));
    SendMessageW(Edit(), WM_IME_ENDCOMPOSITION, 0, 0);
    ASSERT_EQ(editor.RequestCommit(), open_st::InlineTextRequest::Queued);
    EXPECT_FALSE(editor.InvokeEditCommand(open_st::InlineEditCommand::Undo));
    EXPECT_EQ(Text(), L"safe");
    editor.Resume();
    EXPECT_TRUE(editor.InvokeEditCommand(open_st::InlineEditCommand::Undo));
    EXPECT_TRUE(Text().empty());
}
} // namespace
