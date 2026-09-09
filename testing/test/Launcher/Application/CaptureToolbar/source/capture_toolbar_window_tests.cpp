// 验证截图工具栏真实窗口的创建、状态更新、命令回调与资源回收。

#include <capture_toolbar.h>
#include <gtest/gtest.h>

#include <array>
#include <string>

namespace open_st
{
namespace
{
struct TextSearch
{
    const wchar_t* text;
    HWND found{};
};

// 通过可访问名称查询按钮，不依赖内部窗口 ID。
// 入参：window 为枚举到的子窗口；parameter 为借用的名称查找条件和结果结构指针。
// 返回：找到目标控件时返回 FALSE 停止枚举；未匹配时返回 TRUE 继续枚举。
BOOL CALLBACK FindNamedChild(HWND window, LPARAM parameter)
{
    TextSearch& search = *reinterpret_cast<TextSearch*>(parameter);
    std::array<wchar_t, 256> text{};
    GetWindowTextW(window, text.data(), static_cast<int>(text.size()));
    if (std::wstring_view(text.data()) == search.text)
    {
        search.found = window;
        return FALSE;
    }
    return TRUE;
}

class CaptureToolbarWindowTest : public ::testing::Test
{
  protected:
    // 创建工具栏测试的隐藏宿主及隔离业务状态。
    // 入参：无显式入参。
    // 返回：无返回值。
    void SetUp() override
    {
        this->owner_ = CreateWindowExW(0, L"STATIC", L"Toolbar test owner", WS_POPUP, 0, 0, 200, 100, nullptr, nullptr,
                                       GetModuleHandleW(nullptr), nullptr);
        ASSERT_NE(this->owner_, nullptr);
    }
    // 先关工具栏再销毁 owner；重复 Close 必须安全。
    // 入参：无显式入参。
    // 返回：无返回值。
    void TearDown() override
    {
        this->toolbar_.Close();
        if (IsWindow(this->owner_))
        {
            DestroyWindow(this->owner_);
        }
    }
    // 使用生产顺序与分组，文本由测试局部语言状态提供。
    // 入参：buttons 为本次工具栏的命令、图标、文本键和分组描述；默认包含取消、保存、复制。
    // 返回：真实工具栏创建结果，包含成功标志及失败时的诊断信息。
    ToolbarResult Create(std::vector<ToolbarButtonSpec> buttons = {
                             {CaptureToolbarCommand::Cancel, ToolbarIcon::Cancel, "cancel", 0},
                             {CaptureToolbarCommand::Save, ToolbarIcon::Save, "save", 1},
                             {CaptureToolbarCommand::Copy, ToolbarIcon::Copy, "copy", 1}})
    {
        return this->toolbar_.Create(
            GetModuleHandleW(nullptr), this->owner_, std::move(buttons),
            // 按模拟语言返回工具栏标题和按钮提示，供运行期文本刷新断言使用。
            // 入参：key 为待查询的测试界面文本键。
            // 返回：模拟语言下的取消、保存、复制按钮提示或工具栏标题。
            [this](std::string_view key) -> std::wstring
            {
                if (key == "cancel")
                {
                    return this->chinese_ ? L"取消截图" : L"Cancel";
                }
                if (key == "save")
                {
                    return this->chinese_ ? L"保存" : L"Save";
                }
                if (key == "copy")
                {
                    return this->chinese_ ? L"复制" : L"Copy";
                }
                return this->chinese_ ? L"截图工具栏" : L"Capture toolbar";
            },
            // 记录工具栏命令和代次，并按测试状态模拟回调结果。
            // 入参：command 为工具栏提交的命令；token 为该命令对应的截图/选区代次。
            // 返回：accept_，表示本用例是否接受记录下来的命令。
            [this](CaptureToolbarCommand command, std::uint64_t token)
            {
                ++this->calls_;
                this->lastCommand_ = command;
                this->lastToken_ = token;
                return this->accept_;
            });
    }
    // 查询已创建工具栏的子控件，避免访问其他窗口。
    // 入参：name 为要匹配的工具栏按钮文字。
    // 返回：匹配控件或窗口的借用句柄；未找到时为 nullptr，调用方不取得销毁责任。
    HWND Button(const wchar_t* name)
    {
        TextSearch search{name};
        EnumChildWindows(this->toolbar_.NativeHandle(), FindNamedChild, reinterpret_cast<LPARAM>(&search));
        return search.found;
    }
    // 短暂以不激活模式显示，测试退出立即关闭。
    // 入参：无显式入参。
    // 返回：无返回值。
    void Show()
    {
        ASSERT_TRUE(this->toolbar_.UpdatePlacement({100, 100, 400, 300}, {0, 0, 1920, 1080}, 96).success);
        ASSERT_TRUE(this->toolbar_.Show(42).success);
    }
    CaptureToolbar toolbar_;
    HWND owner_{};
    bool chinese_{};
    bool accept_{true};
    int calls_{};
    CaptureToolbarCommand lastCommand_{CaptureToolbarCommand::Cancel};
    std::uint64_t lastToken_{};
};

// 验证创建入口必须拒绝重复命令 ID，并清理部分创建资源。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(CaptureToolbarWindowTest, rejects_duplicate_commands)
{
    EXPECT_FALSE(this->Create({{CaptureToolbarCommand::Copy, ToolbarIcon::Copy, "copy", 0},
                               {CaptureToolbarCommand::Copy, ToolbarIcon::Save, "save", 1}})
                     .success);
    EXPECT_FALSE(IsWindow(this->toolbar_.NativeHandle()));
}

// 验证窗口及按钮的可访问名称随文本刷新更新。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(CaptureToolbarWindowTest, refreshes_button_names_and_title)
{
    ASSERT_TRUE(this->Create().success);
    ASSERT_NE(this->Button(L"Copy"), nullptr);
    this->chinese_ = true;
    ASSERT_TRUE(this->toolbar_.RefreshTexts().success);
    EXPECT_NE(this->Button(L"复制"), nullptr);
    EXPECT_EQ(this->Button(L"Copy"), nullptr);
    std::array<wchar_t, 256> title{};
    GetWindowTextW(this->toolbar_.NativeHandle(), title.data(), static_cast<int>(title.size()));
    EXPECT_STREQ(title.data(), L"截图工具栏");
}
// 验证原生点击只通知一次，新代次才解除旧请求锁。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(CaptureToolbarWindowTest, click_pending_blocks_duplicates_and_new_token_recovers)
{
    ASSERT_TRUE(this->Create().success);
    this->Show();
    const HWND copy = this->Button(L"Copy");
    ASSERT_NE(copy, nullptr);
    SendMessageW(copy, BM_CLICK, 0, 0);
    SendMessageW(copy, BM_CLICK, 0, 0);
    EXPECT_EQ(this->calls_, 1);
    EXPECT_EQ(this->lastCommand_, CaptureToolbarCommand::Copy);
    EXPECT_EQ(this->lastToken_, 42u);
    ASSERT_TRUE(this->toolbar_.Show(43).success);
    SendMessageW(copy, BM_CLICK, 0, 0);
    EXPECT_EQ(this->calls_, 2);
    EXPECT_EQ(this->lastToken_, 43u);
}

// 验证投递失败不能永久禁用按钮，下一次用户操作仍可提交。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(CaptureToolbarWindowTest, failed_post_allows_retry)
{
    ASSERT_TRUE(this->Create().success);
    this->Show();
    const HWND save = this->Button(L"Save");
    ASSERT_NE(save, nullptr);
    this->accept_ = false;
    SendMessageW(save, BM_CLICK, 0, 0);
    this->accept_ = true;
    SendMessageW(save, BM_CLICK, 0, 0);
    EXPECT_EQ(this->calls_, 2);
    EXPECT_EQ(this->lastCommand_, CaptureToolbarCommand::Save);
}

// 验证顶层及子按钮都不激活，点击后不会主动把键盘焦点转给工具栏。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(CaptureToolbarWindowTest, button_and_popup_refuse_activation)
{
    ASSERT_TRUE(this->Create().success);
    const HWND before = GetFocus();
    this->Show();
    const HWND copy = this->Button(L"Copy");
    ASSERT_NE(copy, nullptr);
    EXPECT_EQ(SendMessageW(this->toolbar_.NativeHandle(), WM_MOUSEACTIVATE, reinterpret_cast<WPARAM>(this->owner_),
                           MAKELPARAM(HTCLIENT, WM_LBUTTONDOWN)),
              MA_NOACTIVATE);
    EXPECT_EQ(SendMessageW(copy, WM_MOUSEACTIVATE, reinterpret_cast<WPARAM>(this->owner_),
                           MAKELPARAM(HTCLIENT, WM_LBUTTONDOWN)),
              MA_NOACTIVATE);
    SendMessageW(copy, BM_CLICK, 0, 0);
    EXPECT_EQ(GetFocus(), before);
}

// 验证隐藏后不销毁窗口，owner 销毁后重复关闭不会留下原生资源。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(CaptureToolbarWindowTest, hide_and_owner_destruction_are_safe)
{
    ASSERT_TRUE(this->Create().success);
    this->Show();
    const HWND popup = this->toolbar_.NativeHandle();
    this->toolbar_.Hide();
    EXPECT_TRUE(IsWindow(popup));
    EXPECT_FALSE(IsWindowVisible(popup));
    ASSERT_TRUE(DestroyWindow(this->owner_));
    this->owner_ = nullptr;
    EXPECT_FALSE(IsWindow(popup));
    this->toolbar_.Close();
    this->toolbar_.Close();
    EXPECT_FALSE(IsWindow(this->toolbar_.NativeHandle()));
}
} // namespace
} // namespace open_st
