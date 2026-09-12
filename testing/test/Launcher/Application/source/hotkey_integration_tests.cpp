// 验证 App 的快捷键暂停时间边界、局部命令共用队列以及 Esc 分层取消。

#include "capture_command_gate.h"
#include "capture_overlay_session.h"
#include <app.h>
#include <array>
#include <capture_toolbar.h>
#include <gtest/gtest.h>
#include <selection_model.h>

namespace open_st
{
struct AppHotkeyTestAccess
{
    // 建立隐藏消息窗口和不含冻结图像的稳定选区，不捕获或导出真实桌面。
    // 入参：app 为独立测试实例。
    // 返回：窗口与选区初始化成功为 true。
    static bool Initialize(App& app)
    {
        if (!app.CreateMessageWindow())
            return false;
        app.toolbarGate_ = std::make_unique<CaptureCommandGate>();
        app.overlaySession_ = std::make_unique<CaptureOverlaySession>();
        app.selectionModel_ = std::make_unique<SelectionModel>();
        app.selectionModel_->SetBounds({0, 0, 100, 100});
        (void)app.selectionModel_->Begin({10, 10});
        (void)app.selectionModel_->End({80, 80});
        return true;
    }
    // 切换录入或设置忙状态并调用产品门禁更新。
    // 入参：app 为实例；recording、busy 为新状态。
    // 返回：更新后的跨线程截图门禁值。
    static std::uint64_t Pause(App& app, bool recording, bool busy)
    {
        app.hotkeyRecording_ = recording;
        app.settingsBusy_ = busy;
        app.UpdateCaptureGate();
        return app.captureGate_.load();
    }
    // 设置受控旧边界，避免自动测试等待时钟前进。
    // 入参：app 为实例；time 为测试毫秒边界。
    // 返回：无。
    static void SetBoundary(App& app, DWORD time)
    {
        app.hotkeyBoundary_ = time;
    }
    // 查询产品更新后的全局消息边界。
    // 入参：app 为实例。
    // 返回：DWORD 毫秒边界。
    static DWORD Boundary(const App& app)
    {
        return app.hotkeyBoundary_;
    }
    // 查询局部输入是否晚于产品门禁建立的边界。
    // 入参：app 为实例；time 为键盘消息时间。
    // 返回：局部门禁接受输入为 true。
    static bool Accepts(const App& app, DWORD time)
    {
        return app.toolbarGate_->AcceptsInput(time);
    }
    // 经正式覆盖窗口过程处理局部按键，不向桌面注入按键。
    // 入参：app 为实例；key 为虚拟键；data 为原始重复等标志。
    // 返回：无。
    static void Key(App& app, UINT key, LPARAM data = 0)
    {
        (void)App::OverlayProc(app.messageWindow_, WM_KEYDOWN, key, data);
    }
    // 查询共用工具栏命令是否已被预订。
    // 入参：app 为实例。
    // 返回：存在待处理命令为 true。
    static bool Pending(const App& app)
    {
        return app.toolbarGate_->Pending();
    }
    // 从真实消息队列取走局部键生成的命令，但不执行导出业务。
    // 入参：app 为实例；message 输出取得的工具栏消息。
    // 返回：队列存在一条对应消息时为 true。
    static bool Take(App& app, MSG& message)
    {
        return PeekMessageW(&message, app.messageWindow_, WM_APP + 4, WM_APP + 4, PM_REMOVE) != FALSE;
    }
    // 为下一组局部按键保留同一选区并清空待处理预订。
    // 入参：app 为实例。
    // 返回：无。
    static void ResetPending(App& app)
    {
        app.toolbarGate_->Invalidate();
    }
    // 查询选区阶段及会话，以验证 Esc 清选区后才关闭。
    // 入参：app 为实例。
    // 返回：存在选区为 2、仅存在会话为 1、无会话为 0。
    static int SelectionLevel(const App& app)
    {
        if (!app.overlaySession_)
            return 0;
        return app.selectionModel_ && app.selectionModel_->HasSelection() ? 2 : 1;
    }
};

namespace
{
class AppHotkeyIntegrationTest : public ::testing::Test
{
  protected:
    std::unique_ptr<App> app_;
    std::array<BYTE, 256> originalKeys_{};
    bool keyboardSaved_{};
    // 保存本线程键态并建立隔离 App。
    // 入参：无。
    // 返回：无。
    void SetUp() override
    {
        this->keyboardSaved_ = GetKeyboardState(this->originalKeys_.data()) != FALSE;
        ASSERT_TRUE(this->keyboardSaved_);
        std::array<BYTE, 256> keys{};
        ASSERT_TRUE(SetKeyboardState(keys.data()));
        this->app_ = std::make_unique<App>(GetModuleHandleW(nullptr));
        ASSERT_TRUE(AppHotkeyTestAccess::Initialize(*this->app_));
    }
    // 恢复线程键态并清除该 App 析构发出的退出消息。
    // 入参：无。
    // 返回：无。
    void TearDown() override
    {
        if (this->keyboardSaved_)
            (void)SetKeyboardState(this->originalKeys_.data());
        this->app_.reset();
        MSG message{};
        while (PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE))
        {
        }
    }
};

// 验证录入和设置忙状态均更新暂停代次，并同步截断局部旧按键。
// 入参：无。
// 返回：无；通过断言记录状态、代次及严格时间边界。
TEST_F(AppHotkeyIntegrationTest, recording_and_busy_transitions_reset_message_boundaries)
{
    AppHotkeyTestAccess::SetBoundary(*this->app_, GetTickCount() - 1000);
    const DWORD previous = AppHotkeyTestAccess::Boundary(*this->app_);
    const std::uint64_t paused = AppHotkeyTestAccess::Pause(*this->app_, true, false);
    EXPECT_EQ(paused & 1U, 1U);
    const DWORD recordingBoundary = AppHotkeyTestAccess::Boundary(*this->app_);
    EXPECT_GT(static_cast<LONG>(recordingBoundary - previous), 0);
    EXPECT_FALSE(AppHotkeyTestAccess::Accepts(*this->app_, previous));
    EXPECT_FALSE(AppHotkeyTestAccess::Accepts(*this->app_, recordingBoundary));
    const std::uint64_t busy = AppHotkeyTestAccess::Pause(*this->app_, false, true);
    EXPECT_EQ(busy & 1U, 1U);
    EXPECT_GT(busy & ~std::uint64_t{1}, paused & ~std::uint64_t{1});
    const std::uint64_t resumed = AppHotkeyTestAccess::Pause(*this->app_, false, false);
    EXPECT_EQ(resumed & 1U, 0U);
    EXPECT_GT(resumed, busy);
    EXPECT_FALSE(AppHotkeyTestAccess::Accepts(*this->app_, recordingBoundary));
    EXPECT_TRUE(AppHotkeyTestAccess::Accepts(*this->app_, GetTickCount() + 1));
}

// 验证 Enter、Ctrl+C 和 Ctrl+S 经覆盖过程进入同一个工具栏 pending 队列。
// 入参：无。
// 返回：无；断言命令类别、重复过滤及每组只产生一条消息。
TEST_F(AppHotkeyIntegrationTest, local_copy_save_share_toolbar_pending_and_ignore_repeats)
{
    const std::array<UINT, 3> keys{VK_RETURN, 'C', 'S'};
    for (const UINT key : keys)
    {
        std::array<BYTE, 256> state{};
        if (key != VK_RETURN)
            state[VK_CONTROL] = 0x80;
        ASSERT_TRUE(SetKeyboardState(state.data()));
        AppHotkeyTestAccess::ResetPending(*this->app_);
        AppHotkeyTestAccess::Key(*this->app_, key, static_cast<LPARAM>(1) << 30);
        EXPECT_FALSE(AppHotkeyTestAccess::Pending(*this->app_));
        AppHotkeyTestAccess::Key(*this->app_, key);
        ASSERT_TRUE(AppHotkeyTestAccess::Pending(*this->app_));
        AppHotkeyTestAccess::Key(*this->app_, key);
        MSG message{};
        ASSERT_TRUE(AppHotkeyTestAccess::Take(*this->app_, message));
        EXPECT_EQ(message.wParam,
                  static_cast<WPARAM>(key == 'S' ? CaptureToolbarCommand::Save : CaptureToolbarCommand::Copy));
        EXPECT_FALSE(AppHotkeyTestAccess::Take(*this->app_, message));
        EXPECT_EQ(AppHotkeyTestAccess::SelectionLevel(*this->app_), 2);
    }
}

// 验证 Esc 保留原有分层取消，先清选区并撤销 pending，第二次再关闭会话。
// 入参：无。
// 返回：无；断言阶段变化和旧命令失效。
TEST_F(AppHotkeyIntegrationTest, escape_clears_selection_before_closing_session)
{
    AppHotkeyTestAccess::Key(*this->app_, VK_RETURN);
    ASSERT_TRUE(AppHotkeyTestAccess::Pending(*this->app_));
    AppHotkeyTestAccess::Key(*this->app_, VK_ESCAPE);
    EXPECT_EQ(AppHotkeyTestAccess::SelectionLevel(*this->app_), 1);
    EXPECT_FALSE(AppHotkeyTestAccess::Pending(*this->app_));
    AppHotkeyTestAccess::Key(*this->app_, VK_ESCAPE);
    EXPECT_EQ(AppHotkeyTestAccess::SelectionLevel(*this->app_), 0);
}
} // namespace
} // namespace open_st
