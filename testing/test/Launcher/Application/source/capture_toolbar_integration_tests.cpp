// 验证工具栏与截图快捷键共享的命令门禁、会话代次及显示器选择逻辑。

#include "capture_command_gate.h"
#include "capture_overlay_session.h"
#include "capture_toolbar_monitor.h"

#include <app.h>
#include <array>
#include <capture_toolbar.h>
#include <gtest/gtest.h>
#include <selection_model.h>

namespace open_st
{
// 只建立稳定选区与隐藏消息窗口，不捕获桌面、不创建输出器、不写文件或剪贴板。
struct AppToolbarTestAccess
{
    // 准备通过正式 App 消息过程验证命令排队行为的隐藏窗口和稳定选区。
    // 入参：app 为本用例持有的 App 实例。
    // 返回：隐藏消息窗口创建成功且选区已准备时为 true；创建失败为 false。
    static bool Initialize(App& app)
    {
        if (!app.CreateMessageWindow())
        {
            return false;
        }
        NewSelection(app);
        return true;
    }
    // 用新的无输出选区模拟会话切换，保留代次闸门。
    // 入参：app 为本用例持有的 App 实例。
    // 返回：无返回值。
    static void NewSelection(App& app)
    {
        app.CloseOverlay();
        if (app.toolbarGate_ == nullptr)
        {
            app.toolbarGate_ = std::make_unique<CaptureCommandGate>();
        }
        app.toolbarGate_->Invalidate();
        app.overlaySession_ = std::make_unique<CaptureOverlaySession>();
        app.selectionModel_ = std::make_unique<SelectionModel>();
        app.selectionModel_->SetBounds({0, 0, 100, 100});
        (void)app.selectionModel_->Begin({10, 10});
        (void)app.selectionModel_->End({80, 80});
    }
    // 从测试触发按钮与快捷键共用的真实工具栏命令投递入口。
    // 入参：app 为本用例持有的 App 实例；command 为要通过真实入口投递的工具栏命令。
    // 返回：真实 App 接受并投递命令时为 true；门禁拒绝或投递失败为 false。
    static bool Post(App& app, CaptureToolbarCommand command)
    {
        return app.PostToolbarCommand(command, app.toolbarGate_->Token());
    }
    // 单步分派该 App 的排队消息，调用真实 WindowProc/HandleMessage。
    // 入参：app 为本用例持有的 App 实例。
    // 返回：取得并分派一条工具栏消息时为 true；队列无该消息时为 false。
    static bool DispatchNext(App& app)
    {
        MSG message{};
        if (!PeekMessageW(&message, app.messageWindow_, WM_APP + 4, WM_APP + 4, PM_REMOVE))
        {
            return false;
        }
        DispatchMessageW(&message);
        return true;
    }
    // 查询测试 App 是否仍有待消费的工具栏请求。
    // 入参：app 为本用例持有的 App 实例。
    // 返回：命令门禁存在待处理请求时为 true，否则为 false。
    static bool Pending(const App& app)
    {
        return app.toolbarGate_->Pending();
    }
    // 查询测试 App 是否仍持有截图会话，用于发现旧取消消息误伤新会话。
    // 入参：app 为本用例持有的 App 实例。
    // 返回：App 仍持有截图会话时为 true，否则为 false。
    static bool HasSession(const App& app)
    {
        return app.overlaySession_ != nullptr;
    }
};

namespace
{
constexpr std::uint32_t COPY = 3;
constexpr std::uint32_t SAVE = 2;

class CaptureToolbarQueueTest : public ::testing::Test
{
  protected:
    // 启动真正的隐藏 App 消息窗口，全部截图业务为无输出状态。
    // 入参：无显式入参。
    // 返回：无返回值。
    void SetUp() override
    {
        this->app_ = std::make_unique<App>(GetModuleHandleW(nullptr));
        ASSERT_TRUE(AppToolbarTestAccess::Initialize(*this->app_));
    }
    // App 销毁会投递 WM_QUIT，仅清理本用例遗留的线程退出通知。
    // 入参：无显式入参。
    // 返回：无返回值。
    void TearDown() override
    {
        this->app_.reset();
        MSG message{};
        while (PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE))
        {
        }
    }
    std::unique_ptr<App> app_;
};

// 验证原生消息队列中只能存在一个成功预订的命令，快捷键不能越过按钮的 pending。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(CaptureToolbarQueueTest, actual_app_posts_and_dispatches_only_one_command)
{
    ASSERT_TRUE(AppToolbarTestAccess::Post(*this->app_, CaptureToolbarCommand::Save));
    EXPECT_FALSE(AppToolbarTestAccess::Post(*this->app_, CaptureToolbarCommand::Copy));
    EXPECT_TRUE(AppToolbarTestAccess::Pending(*this->app_));
    EXPECT_TRUE(AppToolbarTestAccess::DispatchNext(*this->app_));
    EXPECT_FALSE(AppToolbarTestAccess::Pending(*this->app_));
    EXPECT_FALSE(AppToolbarTestAccess::DispatchNext(*this->app_));
    EXPECT_TRUE(AppToolbarTestAccess::HasSession(*this->app_));
}

// 验证旧取消消息已经进入原生队列后，新会话及其待处理命令仍必须完好。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(CaptureToolbarQueueTest, queued_old_cancel_cannot_close_replacement_session)
{
    ASSERT_TRUE(AppToolbarTestAccess::Post(*this->app_, CaptureToolbarCommand::Cancel));
    AppToolbarTestAccess::NewSelection(*this->app_);
    ASSERT_TRUE(AppToolbarTestAccess::Post(*this->app_, CaptureToolbarCommand::Copy));
    ASSERT_TRUE(AppToolbarTestAccess::DispatchNext(*this->app_));
    EXPECT_TRUE(AppToolbarTestAccess::HasSession(*this->app_));
    EXPECT_TRUE(AppToolbarTestAccess::Pending(*this->app_));
    EXPECT_TRUE(AppToolbarTestAccess::DispatchNext(*this->app_));
    EXPECT_FALSE(AppToolbarTestAccess::Pending(*this->app_));
}

// 验证模态取消后，之前排队的完成按键不会重新打开保存窗口。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST(CaptureCommandGateTest, input_before_modal_return_is_discarded)
{
    CaptureCommandGate gate;
    EXPECT_TRUE(gate.AcceptsInput(90));
    gate.SetInputBarrier(100);
    EXPECT_FALSE(gate.AcceptsInput(90));
    EXPECT_FALSE(gate.AcceptsInput(100));
    EXPECT_TRUE(gate.AcceptsInput(101));
}

// 验证系统计数约五十天回绕后，新的按键仍应被接受。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST(CaptureCommandGateTest, input_clock_wrap_preserves_newer_events)
{
    CaptureCommandGate gate;
    gate.SetInputBarrier(0xFFFFFFF0u);
    EXPECT_FALSE(gate.AcceptsInput(0xFFFFFFE0u));
    EXPECT_TRUE(gate.AcceptsInput(0x00000010u));
}

// 验证鼠标按钮先投递后，键盘命令及第二次点击不得再预订。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST(CaptureCommandGateTest, button_and_shortcut_share_one_pending_request)
{
    CaptureCommandGate gate;
    const std::uint64_t token = gate.Token();
    ASSERT_TRUE(gate.Reserve(token, SAVE, true));
    EXPECT_FALSE(gate.Reserve(token, SAVE, true));
    EXPECT_FALSE(gate.Reserve(token, COPY, true));
    EXPECT_TRUE(gate.Consume(token, SAVE, true));
    EXPECT_FALSE(gate.Consume(token, SAVE, true));
}

// 验证重新选区与新建截图均使旧消息失效，旧消息不能解锁新请求。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST(CaptureCommandGateTest, obsolete_selection_or_session_cannot_consume_new_request)
{
    CaptureCommandGate gate;
    const std::uint64_t previous = gate.Token();
    ASSERT_TRUE(gate.Reserve(previous, COPY, true));
    gate.Invalidate();
    const std::uint64_t current = gate.Token();
    ASSERT_TRUE(gate.Reserve(current, SAVE, true));
    EXPECT_FALSE(gate.Consume(previous, COPY, true));
    EXPECT_TRUE(gate.Pending());
    EXPECT_TRUE(gate.Consume(current, SAVE, true));
}

// 验证投递失败或 Esc 清空操作后允许重试；旧代次始终不能复用。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST(CaptureCommandGateTest, cancelled_or_failed_post_allows_only_new_token)
{
    CaptureCommandGate gate;
    const std::uint64_t previous = gate.Token();
    ASSERT_TRUE(gate.Reserve(previous, SAVE, true));
    gate.Invalidate();
    EXPECT_FALSE(gate.Pending());
    EXPECT_FALSE(gate.Reserve(previous, SAVE, true));
    EXPECT_TRUE(gate.Reserve(gate.Token(), SAVE, true));
}

// 验证投递时可用但消费时进入模态忙状态的请求应丢弃，恢复后也不补执行。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST(CaptureCommandGateTest, modal_busy_rejects_pending_and_does_not_replay_after_cancel)
{
    CaptureCommandGate gate;
    const std::uint64_t previous = gate.Token();
    EXPECT_FALSE(gate.Reserve(previous, SAVE, false));
    ASSERT_TRUE(gate.Reserve(previous, SAVE, true));
    EXPECT_FALSE(gate.Consume(previous, SAVE, false));
    EXPECT_FALSE(gate.Consume(previous, SAVE, true));
    EXPECT_TRUE(gate.Reserve(gate.Token(), SAVE, true));
}

// 验证命令类型不匹配不能窃取消费权。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST(CaptureCommandGateTest, different_command_does_not_unlock_pending_save)
{
    CaptureCommandGate gate;
    ASSERT_TRUE(gate.Reserve(gate.Token(), SAVE, true));
    EXPECT_FALSE(gate.Consume(gate.Token(), COPY, true));
    EXPECT_TRUE(gate.Pending());
    EXPECT_TRUE(gate.Consume(gate.Token(), SAVE, true));
}

// 验证小面积相交屏只要包含松开点就优先，避免跨屏选区始终吸附主屏。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST(CaptureToolbarMonitorTest, release_monitor_wins_over_largest_overlap)
{
    const std::array<RECT, 2> monitors{{{-1920, 0, 0, 1080}, {0, 0, 2560, 1440}}};
    EXPECT_EQ(SelectToolbarMonitor({-100, 100, 1800, 500}, {-50, 200}, monitors), 0u);
}

// 验证松开点处于屏幕空隙时，按相交面积选择而非最近光标屏幕。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST(CaptureToolbarMonitorTest, gap_falls_back_to_largest_intersection)
{
    const std::array<RECT, 2> monitors{{{-1920, 0, 0, 1080}, {300, -200, 2860, 1240}}};
    EXPECT_EQ(SelectToolbarMonitor({-100, 100, 1800, 500}, {150, 200}, monitors), 1u);
}

// 验证光标屏幕与选区完全不相交时不能作为工具栏目标。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST(CaptureToolbarMonitorTest, unrelated_monitor_does_not_override_selection)
{
    const std::array<RECT, 2> monitors{{{-1920, 0, 0, 1080}, {0, 0, 2560, 1440}}};
    EXPECT_EQ(SelectToolbarMonitor({-900, 100, -400, 500}, {100, 200}, monitors), 0u);
    EXPECT_EQ(SelectToolbarMonitor({4000, 100, 4500, 500}, {100, 200}, monitors), monitors.size());
}
} // namespace
} // namespace open_st
