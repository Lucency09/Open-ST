#include "toolbar_layout.h"

#include <array>
#include <gtest/gtest.h>

namespace open_st::toolbar_detail
{
namespace
{
// 创建真实首版按钮顺序，验证布局使用稳定命令而非假定连续索引。
std::array<ToolbarButtonSpec, 3> Buttons()
{
    return {{{CaptureToolbarCommand::Cancel, ToolbarIcon::Cancel, "capture.toolbar.cancel", 0},
             {CaptureToolbarCommand::Save, ToolbarIcon::Save, "capture.toolbar.save", 1},
             {CaptureToolbarCommand::Copy, ToolbarIcon::Copy, "capture.toolbar.copy", 1}}};
}

// 返回全部可用状态，个别测试再模拟业务隐藏、禁用和选中。
std::array<ToolbarButtonState, 3> States()
{
    return {{{CaptureToolbarCommand::Cancel, true, true, false},
             {CaptureToolbarCommand::Save, true, true, false},
             {CaptureToolbarCommand::Copy, true, true, false}}};
}

// 描述必须有有效且唯一的命令、有效图标和非空文本键。
TEST(ToolbarGeometryTest, validates_stable_identity_and_required_description)
{
    std::array<ToolbarButtonSpec, 3> specs = Buttons();
    EXPECT_TRUE(ValidateButtons(specs).success);
    EXPECT_FALSE(ValidateButtons({}).success);
    specs[1].command = specs[0].command;
    EXPECT_FALSE(ValidateButtons(specs).success);
    specs = Buttons();
    specs[0].command = static_cast<CaptureToolbarCommand>(999);
    EXPECT_FALSE(ValidateButtons(specs).success);
    specs = Buttons();
    specs[0].icon = static_cast<ToolbarIcon>(999);
    EXPECT_FALSE(ValidateButtons(specs).success);
    specs = Buttons();
    specs[0].tooltipKey.clear();
    EXPECT_FALSE(ValidateButtons(specs).success);
}

// 下方默认位置须靠右对齐且留出选区控制点距离，首版有一个有效分隔线。
TEST(ToolbarGeometryTest, places_below_selection_with_control_handle_clearance)
{
    ToolbarLayout layout;
    ASSERT_TRUE(BuildLayout(Buttons(), States(), {100, 100, 500, 300}, {0, 0, 1920, 1080}, 96, layout).success);
    EXPECT_EQ(layout.bounds.right, 500);
    EXPECT_GE(layout.bounds.top, 312);
    EXPECT_EQ(layout.bounds.bottom - layout.bounds.top, 34);
    ASSERT_EQ(layout.separators.size(), 1u);
    ASSERT_EQ(layout.buttons.size(), 3u);
    EXPECT_LT(layout.buttons[0].right, layout.buttons[1].left);
    EXPECT_EQ(layout.buttons[1].right, layout.buttons[2].left);
}

// 底部没有空间时转到选区上方，而不是压住底部控制点。
TEST(ToolbarGeometryTest, bottom_edge_moves_toolbar_above_selection)
{
    ToolbarLayout layout;
    ASSERT_TRUE(BuildLayout(Buttons(), States(), {100, 800, 500, 1070}, {0, 0, 1920, 1080}, 96, layout).success);
    EXPECT_LE(layout.bounds.bottom, 788);
    EXPECT_GE(layout.bounds.top, 0);
}

// 负坐标屏幕和满屏选区仍将全部按钮约束在指定工作区。
TEST(ToolbarGeometryTest, full_screen_negative_origin_stays_inside_work_area)
{
    ToolbarLayout layout;
    const RECT work{-1920, -1080, 0, -40};
    ASSERT_TRUE(BuildLayout(Buttons(), States(), {-1920, -1080, 0, 0}, work, 144, layout).success);
    EXPECT_GE(layout.bounds.left, work.left);
    EXPECT_GE(layout.bounds.top, work.top);
    EXPECT_LE(layout.bounds.right, work.right);
    EXPECT_LE(layout.bounds.bottom, work.bottom);
}

// 很窄且偏左的选区不能把工具栏挤出目标屏幕。
TEST(ToolbarGeometryTest, narrow_selection_clamps_horizontal_position)
{
    ToolbarLayout layout;
    ASSERT_TRUE(BuildLayout(Buttons(), States(), {0, 100, 1, 200}, {0, 0, 1920, 1080}, 96, layout).success);
    EXPECT_EQ(layout.bounds.left, 0);
    EXPECT_GT(layout.bounds.right, 1);
}

// 隐藏整组会收缩宽度并移除悬空分隔线，禁用和选中状态不改变按钮几何。
TEST(ToolbarGeometryTest, hidden_group_collapses_without_extra_separator)
{
    ToolbarLayout full;
    ToolbarLayout hidden;
    std::array<ToolbarButtonState, 3> states = States();
    ASSERT_TRUE(BuildLayout(Buttons(), states, {100, 100, 500, 300}, {0, 0, 1920, 1080}, 96, full).success);
    states[0].visible = false;
    states[1].enabled = false;
    states[2].checked = true;
    ASSERT_TRUE(BuildLayout(Buttons(), states, {100, 100, 500, 300}, {0, 0, 1920, 1080}, 96, hidden).success);
    EXPECT_TRUE(hidden.separators.empty());
    EXPECT_TRUE(IsRectEmpty(&hidden.buttons[0]));
    EXPECT_LT(hidden.bounds.right - hidden.bounds.left, full.bounds.right - full.bounds.left);
    EXPECT_EQ(hidden.buttons[1].right - hidden.buttons[1].left, full.buttons[1].right - full.buttons[1].left);
}

// 中间按钮隐藏后，同组关系仍依据相邻可见项，不能产生两个分隔线。
TEST(ToolbarGeometryTest, hidden_middle_button_preserves_one_group_boundary)
{
    ToolbarLayout layout;
    std::array<ToolbarButtonState, 3> states = States();
    states[1].visible = false;
    ASSERT_TRUE(BuildLayout(Buttons(), states, {100, 100, 500, 300}, {0, 0, 1920, 1080}, 96, layout).success);
    EXPECT_TRUE(IsRectEmpty(&layout.buttons[1]));
    EXPECT_EQ(layout.separators.size(), 1u);
}

// 100%、150%、200% 缩放应保持物理尺寸与 DIP 一致，按钮位于工具栏内部。
TEST(ToolbarGeometryTest, scales_buttons_for_common_monitor_dpi)
{
    for (UINT dpi : {96u, 144u, 192u})
    {
        ToolbarLayout layout;
        ASSERT_TRUE(BuildLayout(Buttons(), States(), {100, 100, 900, 300}, {0, 0, 2560, 1440}, dpi, layout).success);
        EXPECT_EQ(layout.bounds.bottom - layout.bounds.top, MulDiv(34, static_cast<int>(dpi), 96));
        for (const RECT& button : layout.buttons)
        {
            EXPECT_EQ(button.right - button.left, MulDiv(30, static_cast<int>(dpi), 96));
            EXPECT_GE(button.left, 0);
            EXPECT_LE(button.right, layout.bounds.right - layout.bounds.left);
        }
    }
}

// 无效输入和无法容纳的工作区返回错误，不发布半成品位置覆盖旧布局。
TEST(ToolbarGeometryTest, invalid_or_too_small_geometry_keeps_previous_layout)
{
    ToolbarLayout layout;
    layout.bounds = {1, 2, 3, 4};
    EXPECT_FALSE(BuildLayout(Buttons(), States(), {0, 0, 0, 100}, {0, 0, 1920, 1080}, 96, layout).success);
    EXPECT_FALSE(BuildLayout(Buttons(), States(), {0, 0, 100, 100}, {0, 0, 1, 1}, 96, layout).success);
    EXPECT_FALSE(BuildLayout(Buttons(), States(), {0, 0, 100, 100}, {0, 0, 1920, 1080}, 0, layout).success);
    EXPECT_FALSE(BuildLayout(Buttons(), {}, {0, 0, 100, 100}, {0, 0, 1920, 1080}, 96, layout).success);
    EXPECT_EQ(layout.bounds.left, 1);
    EXPECT_EQ(layout.bounds.bottom, 4);
}
} // namespace
} // namespace open_st::toolbar_detail
