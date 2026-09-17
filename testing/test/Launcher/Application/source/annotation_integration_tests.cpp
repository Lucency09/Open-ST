// 验证 App 标注交互、选区共同历史及统一像素输出，不访问真实剪贴板或文件对话框。

#include "capture_annotation_state.h"
#include "capture_overlay_session.h"

#include <app.h>
#include <array>
#include <capture_toolbar.h>
#include <frozen_desktop_frame.h>
#include <gtest/gtest.h>
#include <objbase.h>
#include <selection_output_renderer.h>

namespace open_st
{
struct AppAnnotationTestAccess final
{
    // 创建隐藏消息窗口与空输出会话，并设置固定桌面范围和正式选区。
    // 入参：app 为独占测试应用。
    // 返回：初始化全部成功为 true，不创建遮罩或图形设备。
    static bool Initialize(App& app)
    {
        if (!app.CreateMessageWindow())
        {
            return false;
        }
        app.selectionModel_ = std::make_unique<SelectionModel>();
        app.selectionModel_->SetBounds({0, 0, 800, 600});
        app.annotation_ = std::make_unique<CaptureAnnotationState>();
        app.overlaySession_ = std::make_unique<CaptureOverlaySession>();
        return app.selectionModel_->SelectRectangle({20, 30, 220, 180});
    }

    // 借用当前标注文档状态，用于核验工具、草稿和提交历史。
    // 入参：app 为测试应用。
    // 返回：当前会话持有的状态引用。
    static CaptureAnnotationState& State(App& app)
    {
        return *app.annotation_;
    }

    // 查询正式截图矩形，避免把渲染候选误认为已提交裁剪。
    // 入参：app 为测试应用。
    // 返回：模型的独立快照。
    static SelectionSnapshot Selection(const App& app)
    {
        return app.selectionModel_->Snapshot();
    }

    // 经 App 真实工具命令入口切换当前工具。
    // 入参：app 为应用；command 为受测工具命令。
    // 返回：无。
    static void Tool(App& app, CaptureToolbarCommand command)
    {
        app.HandleAnnotationCommand(command);
    }

    // 经 App 输入入口按下，使用空句柄避免依赖桌面鼠标位置。
    // 入参：app 为应用；point 为虚拟桌面物理像素点。
    // 返回：手势被接受为 true。
    static bool Down(App& app, PointI point)
    {
        return app.BeginSelectionInput(nullptr, point);
    }

    // 经 App 推进绘图或选区手势。
    // 入参：app 为应用；point 为虚拟桌面物理像素点。
    // 返回：需要重绘为 true。
    static bool Move(App& app, PointI point)
    {
        return app.UpdateSelectionInput(point);
    }

    // 经 App 结束绘图或选区手势。
    // 入参：app 为应用；point 为虚拟桌面物理像素点。
    // 返回：无。
    static void Up(App& app, PointI point)
    {
        app.EndSelectionInput(point);
    }

    // 调用快捷键共享的实际撤销或重做入口。
    // 入参：app 为应用；redo 为重做方向。
    // 返回：无。
    static void Restore(App& app, bool redo)
    {
        app.RestoreAnnotationEdit(redo);
    }

    // 查询完成命令是否被活动绘制或无效选区阻止。
    // 入参：app 为应用。
    // 返回：准入为 true。
    static bool CanSubmit(const App& app)
    {
        return app.CanSubmitToolbarCommand();
    }

    // 调用 Esc 的分层取消入口。
    // 入参：app 为应用。
    // 返回：无。
    static void Cancel(App& app)
    {
        app.CancelSelectionOrClose();
    }

    // 把真实失捕消息交给遮罩过程，借用已绑定 App 的隐藏窗口。
    // 入参：app 为应用。
    // 返回：无；不实际抢占系统鼠标捕获。
    static void LoseCapture(App& app)
    {
        App::OverlayProc(app.messageWindow_, WM_CAPTURECHANGED, 0, 0);
    }

    // 查询会话是否仍存在，区分清空选区和关闭截图两个取消层级。
    // 入参：app 为应用。
    // 返回：会话存在为 true。
    static bool Active(const App& app)
    {
        return app.overlaySession_ != nullptr;
    }

    // 清空模型准备首次创建流程，但保留工具状态以验证自动选择工具。
    // 入参：app 为应用。
    // 返回：无。
    static void ClearSelection(App& app)
    {
        app.selectionModel_->Reset();
    }

    // 注入小型纯 SDR 冻结帧及真实输出器，不捕获真实桌面。
    // 入参：app 为应用。
    // 返回：冻结帧有效为 true。
    static bool PrepareOutput(App& app)
    {
        std::vector<CapturedOutputPlane> planes;
        planes.emplace_back(RectI{0, 0, 800, 600}, CapturedPixelFormat::Bgra8Unorm, CapturedColorSpace::SdrGamma22P709,
                            OutputColorMetadata{}, std::vector<std::uint8_t>(800U * 600U * 4U, 255));
        app.frozenDesktopFrame_ = std::make_unique<FrozenDesktopFrame>(RectI{0, 0, 800, 600}, std::move(planes));
        app.outputRenderer_ = std::make_unique<SelectionOutputRenderer>();
        return app.frozenDesktopFrame_->IsValid();
    }

    // 经复制、保存和贴图共享的最终合成入口输出内存图像。
    // 入参：app 为应用；snapshot 为固定文档；frame 与 error 接收结果。
    // 返回：完整生成成功为 true。
    static bool Generate(App& app, const AnnotationSnapshot& snapshot, SdrSelectionFrame& frame, std::wstring& error)
    {
        return app.GenerateAnnotatedSelection(app.selectionModel_->Snapshot().rectangle, snapshot, frame, error);
    }
};
} // namespace open_st

namespace
{
using Access = open_st::AppAnnotationTestAccess;
using namespace open_st;

// 比较四条选区边界，直接检查可观察几何而不调用生产比较函数。
// 入参：actual 为实际选区；expected 为预期物理像素矩形。
// 返回：无；每条边独立报告。
void ExpectRectangle(RectI actual, RectI expected)
{
    EXPECT_EQ(actual.left, expected.left);
    EXPECT_EQ(actual.top, expected.top);
    EXPECT_EQ(actual.right, expected.right);
    EXPECT_EQ(actual.bottom, expected.bottom);
}

class AnnotationIntegrationTest : public testing::Test
{
  protected:
    // 创建独立应用与内存标注会话，拒绝依赖设置或真实桌面内容。
    // 入参：无。
    // 返回：无；初始化失败终止用例。
    void SetUp() override
    {
        this->app_ = std::make_unique<App>(GetModuleHandleW(nullptr));
        ASSERT_TRUE(Access::Initialize(*this->app_));
    }

    // 释放应用所有权并清除本用例的线程退出消息。
    // 入参：无。
    // 返回：无。
    void TearDown() override
    {
        this->app_.reset();
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
        }
    }

    // 完成一笔矩形标注，为后续选区及历史测试建立真实提交。
    // 入参：无。
    // 返回：无；准备失败终止当前辅助函数并记录失败。
    void DrawRectangle()
    {
        Access::Tool(*this->app_, CaptureToolbarCommand::RectangleTool);
        ASSERT_TRUE(Access::Down(*this->app_, {60, 60}));
        ASSERT_TRUE(Access::Move(*this->app_, {100, 100}));
        Access::Up(*this->app_, {100, 100});
        ASSERT_NE(Access::State(*this->app_).Committed(), nullptr);
    }

    std::unique_ptr<App> app_;
};

// 验证全部几何及笔迹工具经 App 提交正确种类，手势期间阻止完成，结束后保留所选工具。
// 入参：无。
// 返回：无；每笔只增加一个对象且共享选区不被改变。
TEST_F(AnnotationIntegrationTest, drawing_tools_commit_without_changing_tool_or_crop)
{
    const std::array<CaptureToolbarCommand, 7> commands{CaptureToolbarCommand::RectangleTool,
                                                        CaptureToolbarCommand::ArrowTool,
                                                        CaptureToolbarCommand::FilledRectangleTool,
                                                        CaptureToolbarCommand::RoundedRectangleTool,
                                                        CaptureToolbarCommand::PenTool,
                                                        CaptureToolbarCommand::LineTool,
                                                        CaptureToolbarCommand::EllipseTool};
    const std::array<CaptureAnnotationTool, 7> tools{
        CaptureAnnotationTool::Rectangle,        CaptureAnnotationTool::Arrow, CaptureAnnotationTool::FilledRectangle,
        CaptureAnnotationTool::RoundedRectangle, CaptureAnnotationTool::Pen,   CaptureAnnotationTool::Line,
        CaptureAnnotationTool::Ellipse};
    const std::array<AnnotationKind, 7> kinds{
        AnnotationKind::Rectangle,        AnnotationKind::Arrow, AnnotationKind::FilledRectangle,
        AnnotationKind::RoundedRectangle, AnnotationKind::Pen,   AnnotationKind::Line,
        AnnotationKind::Ellipse};
    for (std::size_t index = 0; index < commands.size(); ++index)
    {
        Access::Tool(*this->app_, commands[index]);
        ASSERT_TRUE(Access::Down(*this->app_, {60, 60}));
        EXPECT_FALSE(Access::CanSubmit(*this->app_));
        EXPECT_TRUE(Access::Move(*this->app_, {100, 100}));
        Access::Up(*this->app_, {110, 110});
        const AnnotationSnapshot snapshot = Access::State(*this->app_).Committed();
        ASSERT_NE(snapshot, nullptr);
        ASSERT_EQ(snapshot->size(), index + 1);
        EXPECT_EQ(snapshot->back().kind, kinds[index]);
        EXPECT_NE(snapshot->back().id, 0U);
        EXPECT_EQ(Access::State(*this->app_).Tool(), tools[index]);
        EXPECT_TRUE(Access::CanSubmit(*this->app_));
        ExpectRectangle(Access::Selection(*this->app_).rectangle, {20, 30, 220, 180});
    }
}

// 验证选择工具按在已有标注上仍移动截图区域，预览平移以手势起点为基线而非累计偏移。
// 入参：无。
// 返回：无；标注随 crop 平移，大小及身份保持。
TEST_F(AnnotationIntegrationTest, select_over_annotation_moves_crop_and_objects_together)
{
    this->DrawRectangle();
    const AnnotationSnapshot original = Access::State(*this->app_).Committed();
    Access::Tool(*this->app_, CaptureToolbarCommand::SelectTool);
    ASSERT_TRUE(Access::Down(*this->app_, {70, 70}));
    ASSERT_TRUE(Access::Move(*this->app_, {90, 80}));
    ASSERT_TRUE(Access::Move(*this->app_, {100, 90}));
    EXPECT_DOUBLE_EQ(Access::State(*this->app_).Preview()->front().origin.x, 90);
    EXPECT_DOUBLE_EQ(original->front().origin.x, 60);
    EXPECT_FALSE(Access::CanSubmit(*this->app_));
    Access::Up(*this->app_, {100, 90});
    const AnnotationObject& object = Access::State(*this->app_).Committed()->front();
    ExpectRectangle(Access::Selection(*this->app_).rectangle, {50, 50, 250, 200});
    EXPECT_DOUBLE_EQ(object.origin.x, 90);
    EXPECT_DOUBLE_EQ(object.origin.y, 80);
    EXPECT_DOUBLE_EQ(object.extent.x, 40);
    EXPECT_EQ(object.id, original->front().id);
    EXPECT_TRUE(Access::CanSubmit(*this->app_));
}

// 验证选择边界缩小仅裁剪，不缩放、删除或改写被裁出范围的原对象。
// 入参：无。
// 返回：无；重新扩大边界后保留同一对象数据。
TEST_F(AnnotationIntegrationTest, resize_changes_only_crop_and_keeps_clipped_objects)
{
    this->DrawRectangle();
    const AnnotationSnapshot original = Access::State(*this->app_).Committed();
    Access::Tool(*this->app_, CaptureToolbarCommand::SelectTool);
    ASSERT_TRUE(Access::Down(*this->app_, {220, 180}));
    Access::Up(*this->app_, {50, 50});
    ExpectRectangle(Access::Selection(*this->app_).rectangle, {20, 30, 50, 50});
    EXPECT_EQ(Access::State(*this->app_).Committed(), original);
    ASSERT_TRUE(Access::Down(*this->app_, {50, 50}));
    Access::Up(*this->app_, {220, 180});
    ExpectRectangle(Access::Selection(*this->app_).rectangle, {20, 30, 220, 180});
    EXPECT_EQ(Access::State(*this->app_).Committed(), original);
}

// 验证对象创建与选区平移共享撤销栈，重做恢复同一对象 ID 和对应裁剪。
// 入参：无。
// 返回：无；新编辑分支正确丢弃旧 redo。
TEST_F(AnnotationIntegrationTest, undo_redo_restore_objects_and_crop_as_one_edit)
{
    this->DrawRectangle();
    const std::uint64_t id = Access::State(*this->app_).Committed()->front().id;
    Access::Tool(*this->app_, CaptureToolbarCommand::SelectTool);
    ASSERT_TRUE(Access::Down(*this->app_, {80, 80}));
    Access::Up(*this->app_, {100, 100});
    Access::Restore(*this->app_, false);
    ExpectRectangle(Access::Selection(*this->app_).rectangle, {20, 30, 220, 180});
    EXPECT_DOUBLE_EQ(Access::State(*this->app_).Committed()->front().origin.x, 60);
    Access::Restore(*this->app_, false);
    EXPECT_EQ(Access::State(*this->app_).Committed(), nullptr);
    Access::Restore(*this->app_, true);
    ASSERT_NE(Access::State(*this->app_).Committed(), nullptr);
    EXPECT_EQ(Access::State(*this->app_).Committed()->front().id, id);
    Access::Restore(*this->app_, true);
    ExpectRectangle(Access::Selection(*this->app_).rectangle, {40, 50, 240, 200});
    EXPECT_DOUBLE_EQ(Access::State(*this->app_).Committed()->front().origin.x, 80);
    Access::Restore(*this->app_, false);
    this->DrawRectangle();
    EXPECT_FALSE(Access::State(*this->app_).CanRedo());
}

// 验证 Ctrl+Y 在活动绘制和选区拖动期间无效，Ctrl+Z 只取消草稿而不消费已有历史。
// 入参：无。
// 返回：无；两类活动事务均恢复原文档与 crop，下一次撤销才撤回已提交标注。
TEST_F(AnnotationIntegrationTest, active_undo_cancels_only_and_active_redo_is_ignored)
{
    this->DrawRectangle();
    const AnnotationSnapshot original = Access::State(*this->app_).Committed();
    ASSERT_TRUE(Access::Down(*this->app_, {120, 70}));
    ASSERT_TRUE(Access::Move(*this->app_, {170, 100}));
    Access::Restore(*this->app_, true);
    EXPECT_TRUE(Access::State(*this->app_).Drawing());
    Access::Restore(*this->app_, false);
    EXPECT_FALSE(Access::State(*this->app_).Active());
    EXPECT_EQ(Access::State(*this->app_).Committed(), original);
    Access::Tool(*this->app_, CaptureToolbarCommand::SelectTool);
    ASSERT_TRUE(Access::Down(*this->app_, {80, 80}));
    ASSERT_TRUE(Access::Move(*this->app_, {140, 100}));
    Access::Restore(*this->app_, true);
    EXPECT_EQ(Access::Selection(*this->app_).phase, SelectionPhase::Dragging);
    Access::Restore(*this->app_, false);
    ExpectRectangle(Access::Selection(*this->app_).rectangle, {20, 30, 220, 180});
    EXPECT_EQ(Access::State(*this->app_).Committed(), original);
    EXPECT_TRUE(Access::CanSubmit(*this->app_));
    Access::Restore(*this->app_, false);
    EXPECT_EQ(Access::State(*this->app_).Committed(), nullptr);
}

// 验证边界外按下、完全透明、零面积不产生历史；反向及越界终点保持原始几何以供统一裁剪。
// 入参：无。
// 返回：无；仅有效反向手势提交一个对象。
TEST_F(AnnotationIntegrationTest, rejects_empty_or_invisible_draws_and_preserves_off_crop_endpoints)
{
    Access::Tool(*this->app_, CaptureToolbarCommand::FilledRectangleTool);
    EXPECT_FALSE(Access::Down(*this->app_, {19, 60}));
    EXPECT_FALSE(Access::Down(*this->app_, {220, 60}));
    ASSERT_TRUE(Access::State(*this->app_).SetStyle({0, 100, 3}));
    EXPECT_FALSE(Access::Down(*this->app_, {100, 100}));
    EXPECT_FALSE(Access::State(*this->app_).CanUndo());
    ASSERT_TRUE(Access::State(*this->app_).SetStyle({0, 0, 3}));
    ASSERT_TRUE(Access::Down(*this->app_, {100, 100}));
    Access::Up(*this->app_, {100, 100});
    EXPECT_FALSE(Access::State(*this->app_).CanUndo());
    EXPECT_EQ(Access::State(*this->app_).Committed(), nullptr);
    ASSERT_TRUE(Access::Down(*this->app_, {100, 100}));
    Access::Up(*this->app_, {-100, -100});
    const AnnotationSnapshot snapshot = Access::State(*this->app_).Committed();
    ASSERT_NE(snapshot, nullptr);
    ASSERT_EQ(snapshot->size(), 1U);
    EXPECT_DOUBLE_EQ(snapshot->front().origin.x, 100);
    EXPECT_DOUBLE_EQ(snapshot->front().extent.x, -200);
    EXPECT_DOUBLE_EQ(snapshot->front().extent.y, -200);
    Access::Tool(*this->app_, CaptureToolbarCommand::ArrowTool);
    ASSERT_TRUE(Access::Down(*this->app_, {60, 60}));
    Access::Up(*this->app_, {400, 250});
    const AnnotationObject& arrow = Access::State(*this->app_).Committed()->back();
    EXPECT_EQ(arrow.kind, AnnotationKind::Arrow);
    EXPECT_DOUBLE_EQ(arrow.extent.x, 340);
    EXPECT_DOUBLE_EQ(arrow.extent.y, 190);
}

// 验证样式在按下时固定，活动期间不能改样式或工具，提交后新默认值不改旧对象。
// 入参：无。
// 返回：无；线条与填充默认值互相独立。
TEST_F(AnnotationIntegrationTest, gesture_style_is_fixed_and_defaults_do_not_restyle_old_objects)
{
    Access::Tool(*this->app_, CaptureToolbarCommand::RectangleTool);
    ASSERT_TRUE(Access::State(*this->app_).SetStyle({0x123456, 25, 5}));
    ASSERT_TRUE(Access::Down(*this->app_, {60, 60}));
    EXPECT_FALSE(Access::State(*this->app_).SetStyle({0xABCDEF, 50, 8}));
    Access::Tool(*this->app_, CaptureToolbarCommand::ArrowTool);
    Access::Up(*this->app_, {100, 100});
    EXPECT_EQ(Access::State(*this->app_).Tool(), CaptureAnnotationTool::Rectangle);
    ASSERT_TRUE(Access::State(*this->app_).SetStyle({0xABCDEF, 50, 8}));
    const AnnotationObject& object = Access::State(*this->app_).Committed()->front();
    EXPECT_EQ(object.style.rgb, 0x123456U);
    EXPECT_EQ(object.style.transparency, 25U);
    EXPECT_DOUBLE_EQ(object.style.lineWidth, 5);
    Access::Tool(*this->app_, CaptureToolbarCommand::FilledRectangleTool);
    EXPECT_EQ(Access::State(*this->app_).Style().rgb, 0U);
    ASSERT_TRUE(Access::State(*this->app_).SetStyle({0x111111, 10, 3}));
    Access::Tool(*this->app_, CaptureToolbarCommand::RoundedRectangleTool);
    EXPECT_EQ(Access::State(*this->app_).Style().rgb, 0x111111U);
    Access::Tool(*this->app_, CaptureToolbarCommand::ArrowTool);
    EXPECT_EQ(Access::State(*this->app_).Style().rgb, 0xABCDEFU);
}

// 验证最早历史超过 128 步后被淘汰，最近 128 次编辑仍可完整撤销和重做。
// 入参：无。
// 返回：无；历史淘汰不删除当前文档中的早期对象。
TEST(AnnotationStateTest, retains_latest_128_edits_without_deleting_older_objects)
{
    CaptureAnnotationState state;
    state.SetTool(CaptureAnnotationTool::Rectangle);
    const RectI crop{0, 0, 100, 100};
    for (int index = 0; index < 129; ++index)
    {
        ASSERT_TRUE(state.BeginDraw({10, 10}, crop));
        ASSERT_EQ(state.EndDraw({20, 20}), AnnotationCommitResult::Committed);
    }
    ASSERT_EQ(state.Committed()->size(), 129U);
    RectI restored{};
    for (int index = 0; index < 128; ++index)
    {
        ASSERT_TRUE(state.Restore(false, crop, restored));
        ExpectRectangle(restored, crop);
    }
    EXPECT_FALSE(state.CanUndo());
    ASSERT_NE(state.Committed(), nullptr);
    EXPECT_EQ(state.Committed()->size(), 1U);
    for (int index = 0; index < 128; ++index)
    {
        ASSERT_TRUE(state.Restore(true, crop, restored));
    }
    EXPECT_EQ(state.Committed()->size(), 129U);
    EXPECT_FALSE(state.CanRedo());
}

// 验证达到 1024 对象上限后新增失败不破坏文档及尚可重做的选区编辑。
// 入参：无。
// 返回：无；重做仍能恢复原位移和对象 ID。
TEST(AnnotationStateTest, object_limit_failure_preserves_document_and_redo_branch)
{
    CaptureAnnotationState state;
    state.SetTool(CaptureAnnotationTool::Rectangle);
    const RectI crop{0, 0, 100, 100};
    const RectI moved{10, 10, 110, 110};
    for (int index = 0; index < 1024; ++index)
    {
        ASSERT_TRUE(state.BeginDraw({10, 10}, crop));
        ASSERT_EQ(state.EndDraw({20, 20}), AnnotationCommitResult::Committed);
    }
    const AnnotationSnapshot original = state.Committed();
    state.BeginCrop(crop);
    ASSERT_TRUE(state.UpdateCrop(moved, true));
    ASSERT_EQ(state.EndCrop(moved), AnnotationCommitResult::Committed);
    RectI restored{};
    ASSERT_TRUE(state.Restore(false, moved, restored));
    ASSERT_TRUE(state.CanRedo());
    ASSERT_TRUE(state.BeginDraw({30, 30}, crop));
    EXPECT_EQ(state.EndDraw({40, 40}), AnnotationCommitResult::Failed);
    EXPECT_FALSE(state.Active());
    EXPECT_EQ(state.Committed(), original);
    EXPECT_EQ(state.Preview(), original);
    ASSERT_TRUE(state.CanRedo());
    ASSERT_TRUE(state.Restore(true, crop, restored));
    ExpectRectangle(restored, moved);
    EXPECT_EQ(state.Committed()->size(), 1024U);
    EXPECT_EQ(state.Committed()->front().id, original->front().id);
    EXPECT_DOUBLE_EQ(state.Committed()->front().origin.x, 20);
}

// 验证 Esc 顺序为退出绘图工具、清空选区及文档历史、关闭空选区会话。
// 入参：无。
// 返回：无；切换到选择工具不会误删标注。
TEST_F(AnnotationIntegrationTest, escape_selects_tool_then_clears_document_then_closes)
{
    this->DrawRectangle();
    Access::Cancel(*this->app_);
    EXPECT_EQ(Access::State(*this->app_).Tool(), CaptureAnnotationTool::Select);
    EXPECT_NE(Access::State(*this->app_).Committed(), nullptr);
    EXPECT_TRUE(Access::CanSubmit(*this->app_));
    Access::Cancel(*this->app_);
    EXPECT_TRUE(Access::Active(*this->app_));
    EXPECT_EQ(Access::Selection(*this->app_).phase, SelectionPhase::Unselected);
    EXPECT_EQ(Access::State(*this->app_).Committed(), nullptr);
    EXPECT_FALSE(Access::State(*this->app_).CanUndo());
    EXPECT_FALSE(Access::State(*this->app_).CanRedo());
    EXPECT_FALSE(Access::CanSubmit(*this->app_));
    Access::Cancel(*this->app_);
    EXPECT_FALSE(Access::Active(*this->app_));
}

// 验证真实遮罩失捕分派回滚绘制和选区移动的草稿，保留此前已提交对象及历史。
// 入参：无。
// 返回：无；两种操作取消后完成准入恢复。
TEST_F(AnnotationIntegrationTest, capture_loss_rolls_back_draw_and_crop_without_consuming_history)
{
    this->DrawRectangle();
    const AnnotationSnapshot original = Access::State(*this->app_).Committed();
    ASSERT_TRUE(Access::Down(*this->app_, {120, 70}));
    ASSERT_TRUE(Access::Move(*this->app_, {160, 100}));
    Access::LoseCapture(*this->app_);
    EXPECT_FALSE(Access::State(*this->app_).Active());
    EXPECT_EQ(Access::State(*this->app_).Preview(), original);
    Access::Tool(*this->app_, CaptureToolbarCommand::SelectTool);
    ASSERT_TRUE(Access::Down(*this->app_, {80, 80}));
    ASSERT_TRUE(Access::Move(*this->app_, {100, 100}));
    Access::LoseCapture(*this->app_);
    ExpectRectangle(Access::Selection(*this->app_).rectangle, {20, 30, 220, 180});
    EXPECT_EQ(Access::State(*this->app_).Preview(), original);
    EXPECT_TRUE(Access::State(*this->app_).CanUndo());
    EXPECT_TRUE(Access::CanSubmit(*this->app_));
}

// 验证首次框选完成自动恢复选择工具，未确认区域不会因先前工具状态而画标注。
// 入参：无。
// 返回：无；只形成正式选区，不产生标注历史。
TEST_F(AnnotationIntegrationTest, initial_selection_automatically_switches_to_select_tool)
{
    Access::Tool(*this->app_, CaptureToolbarCommand::ArrowTool);
    Access::ClearSelection(*this->app_);
    ASSERT_TRUE(Access::Down(*this->app_, {30, 40}));
    Access::Up(*this->app_, {200, 160});
    ExpectRectangle(Access::Selection(*this->app_).rectangle, {30, 40, 200, 160});
    EXPECT_EQ(Access::State(*this->app_).Tool(), CaptureAnnotationTool::Select);
    EXPECT_EQ(Access::State(*this->app_).Committed(), nullptr);
    EXPECT_FALSE(Access::State(*this->app_).CanUndo());
    EXPECT_TRUE(Access::CanSubmit(*this->app_));
}

// 验证三个完成操作共享的 App 输出入口扁平化固定标注快照，原冻结底图保持白色。
// 入参：无；只生成内存 SDR 图像，不保存或写剪贴板。
// 返回：无；覆盖像素为红色，未覆盖像素保持原色，旧快照不被后续编辑影响。
TEST_F(AnnotationIntegrationTest, shared_output_entry_flattens_fixed_annotation_snapshot)
{
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ASSERT_TRUE(SUCCEEDED(initialized) || initialized == RPC_E_CHANGED_MODE);
    struct ComGuard
    {
        bool initialized{};
        // 仅配对当前用例成功取得的 COM 初始化计数。
        // 入参：无。
        // 返回：无。
        ~ComGuard()
        {
            if (this->initialized)
            {
                CoUninitialize();
            }
        }
    } guard{SUCCEEDED(initialized)};
    ASSERT_TRUE(Access::PrepareOutput(*this->app_));
    Access::Tool(*this->app_, CaptureToolbarCommand::FilledRectangleTool);
    ASSERT_TRUE(Access::State(*this->app_).SetStyle({0xFF0000, 0, 3}));
    ASSERT_TRUE(Access::Down(*this->app_, {60, 60}));
    Access::Up(*this->app_, {100, 100});
    const AnnotationSnapshot fixed = Access::State(*this->app_).Committed();
    Access::Restore(*this->app_, false);
    ASSERT_EQ(Access::State(*this->app_).Committed(), nullptr);
    SdrSelectionFrame output;
    std::wstring error;
    ASSERT_TRUE(Access::Generate(*this->app_, fixed, output, error));
    ASSERT_TRUE(output.IsValid());
    const std::size_t redPixel = 50U * output.Stride() + 50U * 4U;
    ASSERT_GT(output.Pixels().size(), redPixel + 3);
    EXPECT_EQ(output.Pixels()[redPixel], 0U);
    EXPECT_EQ(output.Pixels()[redPixel + 1], 0U);
    EXPECT_EQ(output.Pixels()[redPixel + 2], 255U);
    EXPECT_EQ(output.Pixels()[redPixel + 3], 255U);
    EXPECT_EQ(output.Pixels()[0], 255U);
    ASSERT_TRUE(Access::Generate(*this->app_, {}, output, error));
    EXPECT_EQ(output.Pixels()[redPixel], 255U);
    EXPECT_EQ(output.Pixels()[redPixel + 1], 255U);
    EXPECT_EQ(output.Pixels()[redPixel + 2], 255U);
}
// 验证橡皮擦经真实 App 手势改变统一文档，选区保持且一次撤销恢复全部擦痕。
// 入参：无。
// 返回：无；活动擦除仍禁止完成截图，取消不产生额外历史。
TEST_F(AnnotationIntegrationTest, eraser_gesture_uses_shared_history_and_crop)
{
    Access::Tool(*this->app_, CaptureToolbarCommand::FilledRectangleTool);
    ASSERT_TRUE(Access::Down(*this->app_, {60, 60}));
    Access::Up(*this->app_, {180, 140});
    const AnnotationSnapshot original = Access::State(*this->app_).Committed();
    ASSERT_NE(original, nullptr);
    Access::Tool(*this->app_, CaptureToolbarCommand::EraserTool);
    ASSERT_TRUE(Access::Down(*this->app_, {90, 100}));
    EXPECT_FALSE(Access::CanSubmit(*this->app_));
    ASSERT_TRUE(Access::Move(*this->app_, {150, 100}));
    Access::Up(*this->app_, {150, 100});
    const AnnotationSnapshot erased = Access::State(*this->app_).Committed();
    ASSERT_NE(erased, original);
    ASSERT_NE(erased->front().erasures, nullptr);
    EXPECT_EQ(erased->front().erasures->size(), 1U);
    ExpectRectangle(Access::Selection(*this->app_).rectangle, {20, 30, 220, 180});
    Access::Restore(*this->app_, false);
    EXPECT_EQ(Access::State(*this->app_).Committed(), original);
    Access::Restore(*this->app_, true);
    EXPECT_EQ(Access::State(*this->app_).Committed(), erased);
}
} // namespace
