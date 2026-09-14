// 验证真实 App 重绘批次的快照、重入、延迟回收及退出；图形边界使用 fake，不显示窗口或输出图像。

#include "capture_overlay_session.h"

#include <app.h>
#include <array>
#include <gtest/gtest.h>
#include <selection_model.h>
#include <stdexcept>
#include <vector>

namespace open_st
{
struct AppOverlayTestAccess final
{
    // 创建三个隐藏遮罩并绑定真实窗口过程，不初始化 GPU 或用户设置。
    // 入参：app 为独占应用；windows 接收会话持有的借用句柄。
    // 返回：消息窗口、遮罩和初始选区全部建立成功为 true。
    static bool Initialize(App& app, std::array<HWND, 3>& windows)
    {
        if (!app.CreateMessageWindow())
        {
            return false;
        }
        app.overlaySession_ = std::make_unique<CaptureOverlaySession>();
        app.selectionModel_ = std::make_unique<SelectionModel>();
        app.selectionModel_->SetBounds({0, 0, 1200, 600});
        if (!app.selectionModel_->SelectRectangle({20, 30, 200, 160}))
        {
            return false;
        }
        WNDCLASSEXW windowClass{sizeof(windowClass)};
        windowClass.lpfnWndProc = App::OverlayProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = L"OpenST.AppOverlayRenderTest";
        if (RegisterClassExW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            return false;
        }
        app.overlayPreparing_ = true;
        for (HWND& window : windows)
        {
            window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, windowClass.lpszClassName, L"", WS_POPUP, 0,
                                     0, 200, 160, nullptr, nullptr, windowClass.hInstance, &app);
            if (window == nullptr)
            {
                app.overlayPreparing_ = false;
                return false;
            }
            app.overlaySession_->Add(window);
        }
        app.overlayPreparing_ = false;
        app.overlaySession_->Invalidate();
        return !app.overlayInvalidated_;
    }

    // 经真实应用批次入口执行可控同步图形调用。
    // 入参：app 为应用；window 为触发窗口；draw 为本次 fake 图形边界。
    // 返回：无；批次结束后可检查应用状态。
    static void Render(App& app, HWND window,
                       const std::function<bool(CaptureOverlayOutput&, const SelectionSnapshot&, std::wstring&)>& draw)
    {
        app.RenderOverlays(window, draw);
    }

    // 更新模型并为全部屏幕保留下一批绘制请求。
    // 入参：app 为应用；rectangle 为新的物理像素选区。
    // 返回：选区更新成功为 true。
    static bool ChangeSelection(App& app, RectI rectangle)
    {
        app.selectionModel_->Reset();
        const bool selected = app.selectionModel_->SelectRectangle(rectangle);
        app.overlaySession_->Invalidate();
        return selected;
    }

    // 在纯模型上准备移动中的选区，不设置系统鼠标位置或捕获。
    // 入参：app 为应用；origin、position 为物理像素拖动起止坐标。
    // 返回：成功进入拖动并实际更新矩形为 true。
    static bool BeginMove(App& app, PointI origin, PointI position)
    {
        if (!app.selectionModel_->Begin(origin) || !app.selectionModel_->Update(position))
        {
            return false;
        }
        app.overlaySession_->Invalidate();
        return true;
    }

    // 查询受保护会话，供回调检查对象是否提前释放。
    // 入参：app 为应用。
    // 返回：当前会话的借用指针。
    static CaptureOverlaySession* Session(const App& app)
    {
        return app.overlaySession_.get();
    }

    // 检查共享模型是否仍与会话一起存活。
    // 入参：app 为应用。
    // 返回：模型存在为 true。
    static bool HasModel(const App& app)
    {
        return app.selectionModel_ != nullptr;
    }

    // 查询当前批次是否处于禁止释放的同步调用阶段。
    // 入参：app 为应用。
    // 返回：批次正在执行为 true。
    static bool Rendering(const App& app)
    {
        return app.overlayRendering_;
    }

    // 查询实际完成命令准入，避免只断言某个内部布尔值。
    // 入参：app 为应用。
    // 返回：当前会话允许提交完成命令为 true。
    static bool CanSubmit(const App& app)
    {
        return app.CanSubmitToolbarCommand();
    }

    // 通过真实关闭入口验证批次内的延迟回收。
    // 入参：app 为应用。
    // 返回：无。
    static void Close(App& app)
    {
        app.CloseOverlay();
    }

    // 模拟完成流程已经持有的模态保护，不执行真实输出或弹窗。
    // 入参：app 为应用；busy 为期望忙状态。
    // 返回：无。
    static void SetCompletionBusy(App& app, bool busy)
    {
        app.completionBusy_ = busy;
    }

    // 查询忙期间的失败是否被记录为待回收状态。
    // 入参：app 为应用。
    // 返回：会话已失效为 true。
    static bool Invalidated(const App& app)
    {
        return app.overlayInvalidated_;
    }

    // 从真实消息窗口发出退出请求，覆盖窗口过程重入路径。
    // 入参：app 为应用。
    // 返回：无；WM_QUIT 应当等批次返回后才可取得。
    static void RequestExit(App& app)
    {
        SendMessageW(app.messageWindow_, WM_CLOSE, 0, 0);
    }
};
} // namespace open_st

namespace
{
using Access = open_st::AppOverlayTestAccess;
using Draw = std::function<bool(open_st::CaptureOverlayOutput&, const open_st::SelectionSnapshot&, std::wstring&)>;

class AppOverlayRenderTest : public testing::Test
{
  protected:
    // 创建每例独占的隐藏窗口会话，失败属于测试失败而非跳过。
    // 入参：无。
    // 返回：无；初始化失败终止用例。
    void SetUp() override
    {
        this->app_ = std::make_unique<open_st::App>(GetModuleHandleW(nullptr));
        ASSERT_TRUE(Access::Initialize(*this->app_, this->windows_));
    }

    // 释放全部测试窗口并清空本线程退出消息，防止影响随后用例。
    // 入参：无。
    // 返回：无。
    void TearDown() override
    {
        Access::SetCompletionBusy(*this->app_, false);
        this->app_.reset();
        for (HWND window : this->windows_)
        {
            EXPECT_FALSE(IsWindow(window));
        }
        MSG message{};
        while (PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE))
        {
        }
    }

    // 有界分派真实线程消息，避免队列中较早消息遮住按需产生的 WM_QUIT。
    // 入参：timeout 为最长等待毫秒数；quit 接收实际取得的退出消息。
    // 返回：限时内消费到 WM_QUIT 为 true，未产生则为 false。
    bool WaitForQuit(DWORD timeout, MSG& quit)
    {
        const ULONGLONG deadline = GetTickCount64() + timeout;
        do
        {
            MSG message{};
            if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                if (message.message == WM_QUIT)
                {
                    quit = message;
                    return true;
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            else
            {
                MsgWaitForMultipleObjectsEx(0, nullptr, 1, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            }
        } while (GetTickCount64() < deadline);
        return false;
    }

    std::unique_ptr<open_st::App> app_;
    std::array<HWND, 3> windows_{};
};

// 验证第一屏绘制期间修改模型后，三个输出仍使用旧快照，下一批才消费新选区。
// 入参：无；用隐藏窗口和 fake 图形边界构造同步重入。
// 返回：无；两个批次的矩形及输出顺序须符合预期。
TEST_F(AppOverlayRenderTest, shares_snapshot_until_next_batch)
{
    std::vector<open_st::SelectionSnapshot> snapshots;
    std::vector<HWND> drawn;
    const Draw draw =
        // 记录本屏快照，并在首次调用时模拟新的选区输入。
        // 入参：output 为输出；snapshot 为本批快照；未命名文本接收错误。
        // 返回：始终模拟成功。
        [this, &snapshots, &drawn](open_st::CaptureOverlayOutput& output, const open_st::SelectionSnapshot& snapshot,
                                   std::wstring&)
    {
        snapshots.push_back(snapshot);
        drawn.push_back(output.window);
        if (snapshots.size() == 1)
        {
            EXPECT_TRUE(Access::ChangeSelection(*this->app_, {400, 60, 580, 190}));
        }
        return true;
    };
    Access::Render(*this->app_, this->windows_[1], draw);
    ASSERT_EQ(snapshots.size(), 3U);
    for (std::size_t index = 0; index < 3; ++index)
    {
        EXPECT_EQ(drawn[index], this->windows_[index]);
        EXPECT_EQ(snapshots[index].rectangle.left, 20);
        EXPECT_EQ(snapshots[index].rectangle.top, 30);
        EXPECT_EQ(snapshots[index].rectangle.right, 200);
        EXPECT_EQ(snapshots[index].rectangle.bottom, 160);
    }
    Access::Render(*this->app_, this->windows_[2], draw);
    ASSERT_EQ(snapshots.size(), 6U);
    for (std::size_t index = 3; index < 6; ++index)
    {
        EXPECT_EQ(snapshots[index].rectangle.left, 400);
        EXPECT_EQ(snapshots[index].rectangle.top, 60);
        EXPECT_EQ(snapshots[index].rectangle.right, 580);
        EXPECT_EQ(snapshots[index].rectangle.bottom, 190);
    }
}

// 验证真实 WM_PAINT 同步重入不进入默认 GPU 绘制路径，同时保留被重入窗口的下一批请求。
// 入参：无。
// 返回：无；首批三个输出，下一批仅绘制保留请求的第二窗和显式触发窗。
TEST_F(AppOverlayRenderTest, reentrant_paint_preserves_request_without_nested_drawing)
{
    int count{};
    const Draw draw =
        // 首屏内部发送第二屏绘制消息，返回前确认没有发生嵌套 fake 调用。
        // 入参：未命名参数为输出、快照与错误。
        // 返回：始终成功。
        [this, &count](open_st::CaptureOverlayOutput&, const open_st::SelectionSnapshot&, std::wstring&)
    {
        ++count;
        if (count == 1)
        {
            SendMessageW(this->windows_[1], WM_PAINT, 0, 0);
            EXPECT_EQ(count, 1);
            EXPECT_TRUE(Access::Rendering(*this->app_));
        }
        return true;
    };
    Access::Render(*this->app_, this->windows_[0], draw);
    ASSERT_EQ(count, 3);
    std::vector<HWND> nextBatch;
    Access::Render(*this->app_, this->windows_[0],
                   // 记录后续批次的输出，验证第二窗请求没有被首批尾部清除。
                   // 入参：output 为当前输出；其他参数为快照与错误。
                   // 返回：始终成功。
                   [&nextBatch](open_st::CaptureOverlayOutput& output, const open_st::SelectionSnapshot&, std::wstring&)
                   {
                       nextBatch.push_back(output.window);
                       return true;
                   });
    EXPECT_EQ(nextBatch, (std::vector<HWND>{this->windows_[0], this->windows_[1]}));
    EXPECT_FALSE(Access::Rendering(*this->app_));
}

enum class Invalidation
{
    Close,
    WindowClose,
    Size,
    Display,
    Destroy,
};

class AppOverlayInvalidationTest : public AppOverlayRenderTest, public testing::WithParamInterface<Invalidation>
{
};

// 验证同步绘制期间的关闭、尺寸、显示变化与外部销毁均延迟释放会话，且停止后续输出。
// 入参：测试参数指定失效来源。
// 返回：无；回调内对象仍存活，最外层返回后全部会话资源释放。
TEST_P(AppOverlayInvalidationTest, releases_session_only_after_draw_returns)
{
    open_st::CaptureOverlaySession* const session = Access::Session(*this->app_);
    open_st::CaptureOverlayOutput* const secondOutput = session->Find(this->windows_[1]);
    ASSERT_NE(secondOutput, nullptr);
    int count{};
    Access::Render(*this->app_, this->windows_[0],
                   // 在首屏模拟同步失效并借用第二屏记录核验生命周期。
                   // 入参：未命名参数为当前输出、快照和错误。
                   // 返回：图形调用自身模拟成功，终止由应用失效标记决定。
                   [this, session, secondOutput, &count](open_st::CaptureOverlayOutput&,
                                                         const open_st::SelectionSnapshot&, std::wstring&)
                   {
                       ++count;
                       switch (this->GetParam())
                       {
                       case Invalidation::Close:
                           Access::Close(*this->app_);
                           break;
                       case Invalidation::WindowClose:
                           SendMessageW(this->windows_[1], WM_CLOSE, 0, 0);
                           break;
                       case Invalidation::Size:
                           SendMessageW(this->windows_[1], WM_SIZE, SIZE_RESTORED, MAKELPARAM(240, 160));
                           break;
                       case Invalidation::Display:
                           SendMessageW(this->windows_[1], WM_DISPLAYCHANGE, 32, MAKELPARAM(1920, 1080));
                           break;
                       case Invalidation::Destroy:
                           EXPECT_TRUE(DestroyWindow(this->windows_[1]));
                           EXPECT_EQ(secondOutput->window, nullptr);
                           break;
                       }
                       EXPECT_EQ(Access::Session(*this->app_), session);
                       EXPECT_TRUE(Access::HasModel(*this->app_));
                       EXPECT_TRUE(Access::Rendering(*this->app_));
                       EXPECT_TRUE(Access::Invalidated(*this->app_));
                       if (this->GetParam() != Invalidation::Destroy)
                       {
                           EXPECT_EQ(secondOutput->window, this->windows_[1]);
                           EXPECT_TRUE(IsWindow(secondOutput->window));
                       }
                       return true;
                   });
    EXPECT_EQ(count, 1);
    EXPECT_EQ(Access::Session(*this->app_), nullptr);
    EXPECT_FALSE(Access::HasModel(*this->app_));
    EXPECT_FALSE(Access::Rendering(*this->app_));
    for (HWND window : this->windows_)
    {
        EXPECT_FALSE(IsWindow(window));
    }
}

INSTANTIATE_TEST_SUITE_P(InvalidationPaths, AppOverlayInvalidationTest,
                         testing::Values(Invalidation::Close, Invalidation::WindowClose, Invalidation::Size,
                                         Invalidation::Display, Invalidation::Destroy));

// 验证完成忙期间 fake 绘制失败不弹真实提示、不提前释放，忙结束后统一关闭。
// 入参：无。
// 返回：无；错误标记保留且命令禁止提交，最终会话和模型均释放。
TEST_F(AppOverlayRenderTest, draw_failure_defers_cleanup_while_completion_is_busy)
{
    Access::SetCompletionBusy(*this->app_, true);
    int count{};
    Access::Render(*this->app_, this->windows_[0],
                   // 模拟首屏图形提交失败，使 App 走真实失败收尾分支。
                   // 入参：error 接收固定测试错误；其余参数为输出及快照。
                   // 返回：false 表示图形边界失败。
                   [&count](open_st::CaptureOverlayOutput&, const open_st::SelectionSnapshot&, std::wstring& error)
                   {
                       ++count;
                       error = L"测试模拟绘制失败";
                       return false;
                   });
    EXPECT_EQ(count, 1);
    EXPECT_NE(Access::Session(*this->app_), nullptr);
    EXPECT_TRUE(Access::HasModel(*this->app_));
    EXPECT_TRUE(Access::Invalidated(*this->app_));
    EXPECT_FALSE(Access::Rendering(*this->app_));
    EXPECT_FALSE(Access::CanSubmit(*this->app_));
    Access::SetCompletionBusy(*this->app_, false);
    Access::Close(*this->app_);
    EXPECT_EQ(Access::Session(*this->app_), nullptr);
    EXPECT_FALSE(Access::HasModel(*this->app_));
}

// 验证完成忙期间图形边界抛异常仍恢复批次准入，并保留失效会话直到模态保护解除。
// 入参：无。
// 返回：无；异常不得越过 App，后续屏幕不绘制，最终可安全回收。
TEST_F(AppOverlayRenderTest, draw_exception_releases_render_guard_but_preserves_busy_session)
{
    open_st::CaptureOverlaySession* const session = Access::Session(*this->app_);
    Access::SetCompletionBusy(*this->app_, true);
    int count{};
    const Draw draw =
        // 模拟同步图形边界在首屏抛出异常，验证会话和应用两层收尾。
        // 入参：未命名参数为输出、快照与错误。
        // 返回：无正常返回；抛出测试专用异常。
        [&count](open_st::CaptureOverlayOutput&, const open_st::SelectionSnapshot&, std::wstring&) -> bool
    {
        ++count;
        throw std::runtime_error("Test draw exception.");
    };
    EXPECT_NO_THROW(Access::Render(*this->app_, this->windows_[0], draw));
    EXPECT_EQ(count, 1);
    EXPECT_EQ(Access::Session(*this->app_), session);
    EXPECT_TRUE(Access::HasModel(*this->app_));
    EXPECT_TRUE(Access::Invalidated(*this->app_));
    EXPECT_FALSE(Access::Rendering(*this->app_));
    EXPECT_FALSE(Access::CanSubmit(*this->app_));
    for (HWND window : this->windows_)
    {
        EXPECT_TRUE(IsWindow(window));
    }
    Access::SetCompletionBusy(*this->app_, false);
    Access::Close(*this->app_);
    EXPECT_EQ(Access::Session(*this->app_), nullptr);
    EXPECT_FALSE(Access::HasModel(*this->app_));
    for (HWND window : this->windows_)
    {
        EXPECT_FALSE(IsWindow(window));
    }
}

// 验证真实失捕消息在首屏同步重入后恢复拖动前选区，但本批三屏仍使用失捕前快照。
// 入参：无；只在模型中准备拖动，不移动系统鼠标。
// 返回：无；下一批三屏统一显示恢复后的稳定选区。
TEST_F(AppOverlayRenderTest, capture_changed_updates_only_the_next_batch_snapshot)
{
    ASSERT_TRUE(Access::BeginMove(*this->app_, {90, 90}, {390, 120}));
    std::vector<open_st::SelectionSnapshot> snapshots;
    const Draw draw =
        // 记录所有输出的快照，并在首屏发送真实窗口失捕消息。
        // 入参：snapshot 为本批不可变状态；未命名参数为输出和错误。
        // 返回：模拟绘制成功。
        [this, &snapshots](open_st::CaptureOverlayOutput&, const open_st::SelectionSnapshot& snapshot, std::wstring&)
    {
        snapshots.push_back(snapshot);
        if (snapshots.size() == 1)
        {
            SendMessageW(this->windows_[0], WM_CAPTURECHANGED, 0, 0);
        }
        return true;
    };
    Access::Render(*this->app_, this->windows_[0], draw);
    ASSERT_EQ(snapshots.size(), 3U);
    EXPECT_TRUE(Access::CanSubmit(*this->app_));
    for (std::size_t index = 0; index < 3; ++index)
    {
        EXPECT_EQ(snapshots[index].phase, open_st::SelectionPhase::Dragging);
        EXPECT_EQ(snapshots[index].operation, open_st::SelectionOperation::Moving);
        EXPECT_EQ(snapshots[index].rectangle.left, 320);
        EXPECT_EQ(snapshots[index].rectangle.top, 60);
        EXPECT_EQ(snapshots[index].rectangle.right, 500);
        EXPECT_EQ(snapshots[index].rectangle.bottom, 190);
    }
    Access::Render(*this->app_, this->windows_[2], draw);
    ASSERT_EQ(snapshots.size(), 6U);
    for (std::size_t index = 3; index < 6; ++index)
    {
        EXPECT_EQ(snapshots[index].phase, open_st::SelectionPhase::Selected);
        EXPECT_EQ(snapshots[index].operation, open_st::SelectionOperation::None);
        EXPECT_EQ(snapshots[index].rectangle.left, 20);
        EXPECT_EQ(snapshots[index].rectangle.top, 30);
        EXPECT_EQ(snapshots[index].rectangle.right, 200);
        EXPECT_EQ(snapshots[index].rectangle.bottom, 160);
    }
}

// 验证实际完成命令准入在批次期间关闭，成功返回后恢复稳定选区的提交资格。
// 入参：无。
// 返回：无；三个绘制回调均观察到门禁关闭，结束后重新开放。
TEST_F(AppOverlayRenderTest, completion_gate_recovers_after_successful_batch)
{
    ASSERT_TRUE(Access::CanSubmit(*this->app_));
    int count{};
    Access::Render(*this->app_, this->windows_[2],
                   // 在实际同步绘制边界检查完成入口，而非只检查内部状态字段。
                   // 入参：未命名参数为输出、快照与错误。
                   // 返回：始终成功。
                   [this, &count](open_st::CaptureOverlayOutput&, const open_st::SelectionSnapshot&, std::wstring&)
                   {
                       ++count;
                       EXPECT_FALSE(Access::CanSubmit(*this->app_));
                       return true;
                   });
    EXPECT_EQ(count, 3);
    EXPECT_TRUE(Access::CanSubmit(*this->app_));
    EXPECT_FALSE(Access::Rendering(*this->app_));
}

// 验证消息窗口的退出请求在同步绘制内部不投递 WM_QUIT，返回后才清理会话并推进退出。
// 入参：无。
// 返回：无；只有首屏执行，外层返回后可取得退出消息且所有遮罩已销毁。
TEST_F(AppOverlayRenderTest, exit_waits_for_outer_batch_before_posting_quit)
{
    int count{};
    Access::Render(*this->app_, this->windows_[0],
                   // 在首屏内发送真实退出请求，检查消息和受保护资源。
                   // 入参：未命名参数为输出、快照与错误。
                   // 返回：模拟成功，由退出状态中止批次。
                   [this, &count](open_st::CaptureOverlayOutput&, const open_st::SelectionSnapshot&, std::wstring&)
                   {
                       ++count;
                       Access::RequestExit(*this->app_);
                       MSG message{};
                       EXPECT_FALSE(this->WaitForQuit(20, message));
                       EXPECT_NE(Access::Session(*this->app_), nullptr);
                       EXPECT_TRUE(Access::HasModel(*this->app_));
                       EXPECT_TRUE(Access::Rendering(*this->app_));
                       return true;
                   });
    EXPECT_EQ(count, 1);
    EXPECT_EQ(Access::Session(*this->app_), nullptr);
    EXPECT_FALSE(Access::HasModel(*this->app_));
    EXPECT_FALSE(Access::Rendering(*this->app_));
    EXPECT_FALSE(Access::CanSubmit(*this->app_));
    MSG message{};
    EXPECT_TRUE(this->WaitForQuit(500, message));
    EXPECT_EQ(message.message, static_cast<UINT>(WM_QUIT));
    for (HWND window : this->windows_)
    {
        EXPECT_FALSE(IsWindow(window));
    }
}
} // namespace
