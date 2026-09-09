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
    // 使用正式窗口过程，保证测试实际经过 PostMessage 与 HandleMessage。
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
    // 按钮与快捷键共用的真实投递入口。
    static bool Post(App& app, CaptureToolbarCommand command)
    {
        return app.PostToolbarCommand(command, app.toolbarGate_->Token());
    }
    // 单步分派该 App 的排队消息，调用真实 WindowProc/HandleMessage。
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
    // 观察真实准入状态，避免把无输出业务误记为导出验证。
    static bool Pending(const App& app)
    {
        return app.toolbarGate_->Pending();
    }
    // 取消命令若误作用于新会话会清除此对象。
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
    void SetUp() override
    {
        this->app_ = std::make_unique<App>(GetModuleHandleW(nullptr));
        ASSERT_TRUE(AppToolbarTestAccess::Initialize(*this->app_));
    }
    // App 销毁会投递 WM_QUIT，仅清理本用例遗留的线程退出通知。
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

// 原生消息队列中只能存在一个成功预订的命令，快捷键不能越过按钮的 pending。
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

// 旧取消消息已经进入原生队列后，新会话及其待处理命令仍必须完好。
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

// 模态取消后，之前排队的完成按键不会重新打开保存窗口。
TEST(CaptureCommandGateTest, input_before_modal_return_is_discarded)
{
    CaptureCommandGate gate;
    EXPECT_TRUE(gate.AcceptsInput(90));
    gate.SetInputBarrier(100);
    EXPECT_FALSE(gate.AcceptsInput(90));
    EXPECT_FALSE(gate.AcceptsInput(100));
    EXPECT_TRUE(gate.AcceptsInput(101));
}

// 系统计数约五十天回绕后，新的按键仍应被接受。
TEST(CaptureCommandGateTest, input_clock_wrap_preserves_newer_events)
{
    CaptureCommandGate gate;
    gate.SetInputBarrier(0xFFFFFFF0u);
    EXPECT_FALSE(gate.AcceptsInput(0xFFFFFFE0u));
    EXPECT_TRUE(gate.AcceptsInput(0x00000010u));
}

// 鼠标按钮先投递后，键盘命令及第二次点击不得再预订。
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

// 重新选区与新建截图均使旧消息失效，旧消息不能解锁新请求。
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

// 投递失败或 Esc 清空操作后允许重试；旧代次始终不能复用。
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

// 投递时可用但消费时进入模态忙状态的请求应丢弃，恢复后也不补执行。
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

// 命令类型不匹配不能窃取消费权。
TEST(CaptureCommandGateTest, different_command_does_not_unlock_pending_save)
{
    CaptureCommandGate gate;
    ASSERT_TRUE(gate.Reserve(gate.Token(), SAVE, true));
    EXPECT_FALSE(gate.Consume(gate.Token(), COPY, true));
    EXPECT_TRUE(gate.Pending());
    EXPECT_TRUE(gate.Consume(gate.Token(), SAVE, true));
}

// 小面积相交屏只要包含松开点就优先，避免跨屏选区始终吸附主屏。
TEST(CaptureToolbarMonitorTest, release_monitor_wins_over_largest_overlap)
{
    const std::array<RECT, 2> monitors{{{-1920, 0, 0, 1080}, {0, 0, 2560, 1440}}};
    EXPECT_EQ(SelectToolbarMonitor({-100, 100, 1800, 500}, {-50, 200}, monitors), 0u);
}

// 松开点处于屏幕空隙时，按相交面积选择而非最近光标屏幕。
TEST(CaptureToolbarMonitorTest, gap_falls_back_to_largest_intersection)
{
    const std::array<RECT, 2> monitors{{{-1920, 0, 0, 1080}, {300, -200, 2860, 1240}}};
    EXPECT_EQ(SelectToolbarMonitor({-100, 100, 1800, 500}, {150, 200}, monitors), 1u);
}

// 光标屏幕与选区完全不相交时不能作为工具栏目标。
TEST(CaptureToolbarMonitorTest, unrelated_monitor_does_not_override_selection)
{
    const std::array<RECT, 2> monitors{{{-1920, 0, 0, 1080}, {0, 0, 2560, 1440}}};
    EXPECT_EQ(SelectToolbarMonitor({-900, 100, -400, 500}, {100, 200}, monitors), 0u);
    EXPECT_EQ(SelectToolbarMonitor({4000, 100, 4500, 500}, {100, 200}, monitors), monitors.size());
}
} // namespace
} // namespace open_st
