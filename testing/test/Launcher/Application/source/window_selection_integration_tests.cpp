// 用真实 App 选区编排和可控窗口记录验证候选准入、单击／拖动、取消与失捕，不执行真实输出。

#include "capture_command_gate.h"
#include "capture_overlay_session.h"
#include "capture_selection_input.h"
#include "window_selection_snapshot.h"
#include <app.h>
#include <array>
#include <capture_toolbar.h>
#include <gtest/gtest.h>

namespace open_st
{
struct AppSelectionTestAccess final
{
    // 创建隐藏消息窗口和两个固定候选，使用真实选区与命令门禁。
    // 入参：app 为测试独占应用。
    // 返回：所有前置准备成功为 true，不捕获桌面或创建输出器。
    static bool Initialize(App& app)
    {
        if (!app.CreateMessageWindow())
            return false;
        app.overlaySession_ = std::make_unique<CaptureOverlaySession>();
        app.selectionModel_ = std::make_unique<SelectionModel>();
        app.selectionModel_->SetBounds({0, 0, 300, 300});
        app.selectionInput_ = std::make_unique<CaptureSelectionInput>();
        app.windowSelection_ = std::make_unique<WindowSelectionSnapshot>();
        app.toolbarGate_ = std::make_unique<CaptureCommandGate>();
        std::array<WindowSelectionNode, 2> nodes{};
        nodes[0].id = 1;
        nodes[0].rectangle = {10, 10, 90, 90};
        nodes[1].id = 2;
        nodes[1].predecessor = 1;
        nodes[1].rectangle = {100, 100, 220, 220};
        const std::array<RectI, 1> outputs{{{0, 0, 300, 300}}};
        return app.windowSelection_->Build(nodes, outputs[0], outputs);
    }
    // 读取正式模型快照，和绘制候选比较其输出资格。
    // 入参：app 为测试应用。
    // 返回：模型快照值副本。
    static SelectionSnapshot Model(const App& app)
    {
        return app.selectionModel_->Snapshot();
    }
    // 读取正式绘制入口使用的视图。
    // 入参：app 为测试应用。
    // 返回：可含候选但不改变模型的快照。
    static SelectionSnapshot Drawing(const App& app)
    {
        return app.SelectionForDrawing();
    }
    // 模拟明确物理位置的鼠标移动。
    // 入参：app 为测试应用；point 为物理屏幕位置。
    // 返回：是否需要重绘。
    static bool Move(App& app, PointI point)
    {
        return app.UpdateSelectionInput(point);
    }
    // 模拟按下，容差由真实隐藏窗口 DPI 获取。
    // 入参：app 为测试应用；point 为按下位置。
    // 返回：交互是否被接受。
    static bool Down(App& app, PointI point)
    {
        return app.BeginSelectionInput(app.messageWindow_, point);
    }
    // 模拟抬起并更新真实命令代次。
    // 入参：app 为测试应用；point 为抬起位置。
    // 返回：无返回值。
    static void Up(App& app, PointI point)
    {
        app.EndSelectionInput(point);
        app.InvalidateToolbarCommands(true);
    }
    // 通过真实门禁尝试输出命令，仅排队而不执行系统输出。
    // 入参：app 为测试应用；command 为复制、保存或贴图。
    // 返回：命令排队成功为 true。
    static bool Post(App& app, CaptureToolbarCommand command)
    {
        return app.PostToolbarCommand(command, app.toolbarGate_->Token());
    }
    // 将取消或失捕消息送入真实遮罩过程。
    // 入参：app 为测试应用；message 为待处理消息。
    // 返回：无返回值。
    static void Message(App& app, UINT message)
    {
        (void)App::OverlayProc(app.messageWindow_, message, message == WM_KEYDOWN ? VK_ESCAPE : 0, 0);
    }
    // 判断是否仍保留按下／拖动交互。
    // 入参：app 为测试应用。
    // 返回：有进行中交互为 true。
    static bool Interacting(const App& app)
    {
        return app.HasSelectionInteraction();
    }
    // 查询截图会话是否仍存活。
    // 入参：app 为测试应用。
    // 返回：会话存在为 true。
    static bool Active(const App& app)
    {
        return app.overlaySession_ != nullptr;
    }
    // 清理智能记录模拟采集失败，验证手动路径仍能正常完成。
    // 入参：app 为测试应用。
    // 返回：无返回值。
    static void DisableCandidate(App& app)
    {
        app.windowSelection_->Clear();
    }
};
} // namespace open_st

namespace
{
class WindowSelectionIntegrationTest : public testing::Test
{
  protected:
    // 建立独立测试应用，不读取用户设置或操作剪贴板。
    // 入参：无。
    // 返回：无返回值；准备失败终止用例。
    void SetUp() override
    {
        this->app_ = std::make_unique<open_st::App>(GetModuleHandleW(nullptr));
        ASSERT_TRUE(open_st::AppSelectionTestAccess::Initialize(*this->app_));
    }
    // 释放会话和消息窗口，并移除本用例遗留的退出消息。
    // 入参：无。
    // 返回：无返回值。
    void TearDown() override
    {
        this->app_.reset();
        MSG message{};
        while (PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE))
        {
        }
    }
    // 核对正式选区半开边界，不借用生产比较函数。
    // 入参：expected 为期望物理矩形。
    // 返回：无返回值；各边分别报告差异。
    void ExpectRectangle(open_st::RectI expected)
    {
        const open_st::RectI actual = open_st::AppSelectionTestAccess::Model(*this->app_).rectangle;
        EXPECT_EQ(actual.left, expected.left);
        EXPECT_EQ(actual.top, expected.top);
        EXPECT_EQ(actual.right, expected.right);
        EXPECT_EQ(actual.bottom, expected.bottom);
    }
    std::unique_ptr<open_st::App> app_;
};
using Access = open_st::AppSelectionTestAccess;

// 验证绘制候选不授权复制、保存和贴图，桌面空白清除预选。
// 入参：无。
// 返回：无返回值；断言模型资格和实际命令门禁。
TEST_F(WindowSelectionIntegrationTest, candidate_is_drawing_only_and_cannot_submit)
{
    ASSERT_TRUE(Access::Move(*this->app_, {20, 20}));
    EXPECT_TRUE(Access::Drawing(*this->app_).hasSelection);
    EXPECT_FALSE(Access::Drawing(*this->app_).showHandles);
    EXPECT_FALSE(Access::Model(*this->app_).hasSelection);
    EXPECT_EQ(Access::Model(*this->app_).phase, open_st::SelectionPhase::Unselected);
    EXPECT_FALSE(Access::Post(*this->app_, open_st::CaptureToolbarCommand::Copy));
    EXPECT_FALSE(Access::Post(*this->app_, open_st::CaptureToolbarCommand::Save));
    EXPECT_FALSE(Access::Post(*this->app_, open_st::CaptureToolbarCommand::Pin));
    EXPECT_FALSE(Access::Move(*this->app_, {21, 21}));
    EXPECT_TRUE(Access::Move(*this->app_, {290, 290}));
    EXPECT_FALSE(Access::Drawing(*this->app_).hasSelection);
}

// 验证单击确认原窗口范围，失捕消息不撤销已完成的选择。
// 入参：无。
// 返回：无返回值；确认后才允许命令排队，重复请求仍被拒绝。
TEST_F(WindowSelectionIntegrationTest, click_confirms_before_capture_loss_and_allows_one_command)
{
    ASSERT_TRUE(Access::Down(*this->app_, {20, 20}));
    EXPECT_FALSE(Access::Post(*this->app_, open_st::CaptureToolbarCommand::Copy));
    Access::Up(*this->app_, {20, 20});
    Access::Message(*this->app_, WM_CAPTURECHANGED);
    this->ExpectRectangle({10, 10, 90, 90});
    EXPECT_TRUE(Access::Drawing(*this->app_).showHandles);
    EXPECT_TRUE(Access::Post(*this->app_, open_st::CaptureToolbarCommand::Copy));
    EXPECT_FALSE(Access::Post(*this->app_, open_st::CaptureToolbarCommand::Save));
}

// 验证超过阈值后从按下点框选，穿过另一窗口不会切换为该窗口矩形。
// 入参：无。
// 返回：无返回值；断言自由框选边界。
TEST_F(WindowSelectionIntegrationTest, drag_across_another_candidate_keeps_original_anchor)
{
    ASSERT_TRUE(Access::Down(*this->app_, {20, 20}));
    ASSERT_TRUE(Access::Move(*this->app_, {150, 150}));
    Access::Up(*this->app_, {180, 180});
    this->ExpectRectangle({20, 20, 180, 180});
}

// 验证抬起位置也启动自由框选，并且返回原点的拖动不误确认整窗。
// 入参：无。
// 返回：无返回值；覆盖遗漏移动消息和零面积创建回退。
TEST_F(WindowSelectionIntegrationTest, release_without_move_and_return_to_origin_do_not_select_window)
{
    ASSERT_TRUE(Access::Down(*this->app_, {20, 20}));
    Access::Up(*this->app_, {70, 60});
    this->ExpectRectangle({20, 20, 70, 60});
    Access::Message(*this->app_, WM_KEYDOWN);
    ASSERT_TRUE(Access::Down(*this->app_, {20, 20}));
    ASSERT_TRUE(Access::Move(*this->app_, {70, 60}));
    Access::Up(*this->app_, {20, 20});
    EXPECT_FALSE(Access::Model(*this->app_).hasSelection);
    EXPECT_TRUE(Access::Drawing(*this->app_).hasSelection);
}

// 验证待判定失捕和取消消息都清状态；Esc 先取消操作，再退出无正式选区的会话。
// 入参：无。
// 返回：无返回值；不允许取消后的抬起提交旧候选。
TEST_F(WindowSelectionIntegrationTest, pending_capture_loss_cancelmode_and_escape_clear_context)
{
    for (const UINT message : {WM_CAPTURECHANGED, WM_CANCELMODE, WM_KEYDOWN})
    {
        ASSERT_TRUE(Access::Down(*this->app_, {20, 20}));
        Access::Message(*this->app_, message);
        EXPECT_FALSE(Access::Interacting(*this->app_));
        EXPECT_TRUE(Access::Active(*this->app_));
        Access::Up(*this->app_, {20, 20});
        EXPECT_FALSE(Access::Model(*this->app_).hasSelection);
    }
    Access::Message(*this->app_, WM_KEYDOWN);
    EXPECT_FALSE(Access::Active(*this->app_));
}

// 验证智能查询不可用时仍保留一像素手动框选，并且已选区外点击不重选。
// 入参：无。
// 返回：无返回值；只验证新功能对既有交互的兼容。
TEST_F(WindowSelectionIntegrationTest, unavailable_snapshot_keeps_small_manual_selection_and_outside_rejection)
{
    Access::DisableCandidate(*this->app_);
    ASSERT_TRUE(Access::Down(*this->app_, {20, 20}));
    Access::Up(*this->app_, {21, 21});
    this->ExpectRectangle({20, 20, 21, 21});
    EXPECT_FALSE(Access::Down(*this->app_, {200, 200}));
    this->ExpectRectangle({20, 20, 21, 21});
}

// 验证已确认窗口清除后可重新预选，关闭布局失效会话释放全部新增上下文。
// 入参：无。
// 返回：无返回值；正式窗口范围不随后续悬停替换。
TEST_F(WindowSelectionIntegrationTest, confirmed_selection_stays_fixed_until_cleared_and_display_change_closes)
{
    ASSERT_TRUE(Access::Down(*this->app_, {20, 20}));
    Access::Up(*this->app_, {20, 20});
    EXPECT_FALSE(Access::Move(*this->app_, {150, 150}));
    this->ExpectRectangle({10, 10, 90, 90});
    Access::Message(*this->app_, WM_KEYDOWN);
    (void)Access::Move(*this->app_, {150, 150});
    EXPECT_EQ(Access::Drawing(*this->app_).rectangle.left, 100);
    EXPECT_FALSE(Access::Model(*this->app_).hasSelection);
    ASSERT_TRUE(Access::Down(*this->app_, {150, 150}));
    Access::Message(*this->app_, WM_DISPLAYCHANGE);
    EXPECT_FALSE(Access::Active(*this->app_));
    EXPECT_FALSE(Access::Interacting(*this->app_));
    EXPECT_FALSE(Access::Drawing(*this->app_).hasSelection);
}
} // namespace
