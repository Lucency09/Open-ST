// 文件职责：验证选区创建、移动、缩放、命中与取消状态机，包括负坐标和边界收敛。

#include <gtest/gtest.h>

#include <selection_model.h>

#include <array>
#include <cstddef>

namespace
{
// 逐边比较矩形实际值与预期值，精确定位选区或遮罩几何错误。
// 入参：actual：实际物理像素半开矩形；expected：预期物理像素半开矩形。
// 返回：无返回值；四条边界分别由 GoogleTest 断言报告是否一致。
void ExpectRectangle(const open_st::RectI& actual, const open_st::RectI& expected)
{
    EXPECT_EQ(actual.left, expected.left);
    EXPECT_EQ(actual.top, expected.top);
    EXPECT_EQ(actual.right, expected.right);
    EXPECT_EQ(actual.bottom, expected.bottom);
}

// 通过模型真实输入步骤建立稳定选区，作为移动和缩放测试的前置状态。
// 入参：selection：输入输出参数，待初始化的选区模型；bounds：允许选择的桌面物理像素边界；first、second：创建拖动的物理像素起点和终点。
// 返回：无返回值；模型边界和稳定选区被建立，并通过断言检查创建流程是否成功。
void CreateSelection(open_st::SelectionModel& selection, open_st::RectI bounds, open_st::PointI first,
                     open_st::PointI second)
{
    selection.SetBounds(bounds);
    ASSERT_TRUE(selection.Begin(first));
    ASSERT_TRUE(selection.End(second));
    ASSERT_EQ(selection.Phase(), open_st::SelectionPhase::Selected);
}

// 按指定控制点枚举查找模型计算出的中心位置，供后续鼠标交互测试使用。
// 入参：selection：已有选区的只读模型；wanted：需要定位的控制点枚举。
// 返回：匹配控制点的虚拟桌面物理像素中心；未找到时报告测试失败并返回零点。
open_st::PointI CenterFor(const open_st::SelectionModel& selection, open_st::SelectionHandle wanted)
{
    for (const open_st::SelectionHandlePosition& handle : selection.Handles())
    {
        if (handle.handle == wanted)
        {
            return handle.center;
        }
    }
    ADD_FAILURE() << "Requested selection handle was not found.";
    return {};
}
} // namespace

// 验证从四个方向拖动都产生同一个规范化半开矩形，避免反向拖动出现负宽高。
// 入参：无运行时形参；宏参数 SelectionModelTest 为测试套件，creates_a_normalized_rectangle_from_every_drag_direction 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
TEST(SelectionModelTest, creates_a_normalized_rectangle_from_every_drag_direction)
{
    constexpr std::array<open_st::PointI, 4> starts{
        open_st::PointI{10, 20},
        open_st::PointI{80, 20},
        open_st::PointI{80, 70},
        open_st::PointI{10, 70},
    };
    constexpr std::array<open_st::PointI, 4> ends{
        open_st::PointI{80, 70},
        open_st::PointI{10, 70},
        open_st::PointI{10, 20},
        open_st::PointI{80, 20},
    };

    for (std::size_t index = 0; index < starts.size(); ++index)
    {
        SCOPED_TRACE(index);
        open_st::SelectionModel selection;
        selection.SetBounds({0, 0, 100, 100});
        ASSERT_TRUE(selection.Begin(starts[index]));
        EXPECT_EQ(selection.Operation(), open_st::SelectionOperation::Creating);
        ASSERT_TRUE(selection.End(ends[index]));
        EXPECT_EQ(selection.Phase(), open_st::SelectionPhase::Selected);
        ExpectRectangle(selection.Snapshot().rectangle, {10, 20, 80, 70});
    }
}

// 验证创建点会限制在含负坐标的虚拟桌面内，并拒绝零面积的新选区。
// 入参：无运行时形参；宏参数 SelectionModelTest 为测试套件，clamps_creation_to_virtual_desktop_bounds_and_rejects_zero_area 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
TEST(SelectionModelTest, clamps_creation_to_virtual_desktop_bounds_and_rejects_zero_area)
{
    open_st::SelectionModel selection;
    selection.SetBounds({-100, -50, 100, 100});

    ASSERT_TRUE(selection.Begin({-80, 30}));
    ASSERT_TRUE(selection.End({150, -90}));
    ExpectRectangle(selection.Snapshot().rectangle, {-80, -50, 100, 30});

    selection.Reset();
    ASSERT_TRUE(selection.Begin({25, 35}));
    ASSERT_TRUE(selection.End({25, 35}));
    EXPECT_EQ(selection.Phase(), open_st::SelectionPhase::Unselected);
    EXPECT_FALSE(selection.HasSelection());
}

// 验证使用光标真实可达的最右/最下像素创建选区时，会吸附到半开桌面边界并包含最后一行列。
// 入参：无运行时形参；宏参数 SelectionModelTest 为测试套件，creation_reaches_exclusive_right_and_bottom_bounds_from_both_directions
// 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
TEST(SelectionModelTest, creation_reaches_exclusive_right_and_bottom_bounds_from_both_directions)
{
    open_st::SelectionModel selection;
    selection.SetBounds({-100, -50, 100, 100});

    ASSERT_TRUE(selection.Begin({0, 0}));
    ASSERT_TRUE(selection.End({99, 99}));
    ExpectRectangle(selection.Snapshot().rectangle, {0, 0, 100, 100});

    selection.Reset();
    ASSERT_TRUE(selection.Begin({99, 99}));
    ASSERT_TRUE(selection.End({-100, -50}));
    ExpectRectangle(selection.Snapshot().rectangle, {-100, -50, 100, 100});
}

// 验证已有选区后，选区外按下不会因边界钳制而误命中控制点，也不会创建第二个选区。
// 入参：无运行时形参；宏参数 SelectionModelTest 为测试套件，ignores_input_outside_an_existing_selection 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
TEST(SelectionModelTest, ignores_input_outside_an_existing_selection)
{
    open_st::SelectionModel selection;
    CreateSelection(selection, {0, 0, 100, 100}, {20, 30}, {100, 100});

    EXPECT_FALSE(selection.Begin({500, 500}));
    EXPECT_EQ(selection.Phase(), open_st::SelectionPhase::Selected);
    EXPECT_EQ(selection.Operation(), open_st::SelectionOperation::None);
    ExpectRectangle(selection.Snapshot().rectangle, {20, 30, 100, 100});
}

// 验证从选区内部拖动会整体移动矩形，越界时保持宽高并把整体限制在桌面边界内。
// 入参：无运行时形参；宏参数 SelectionModelTest 为测试套件，moves_from_the_interior_without_changing_size 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
TEST(SelectionModelTest, moves_from_the_interior_without_changing_size)
{
    open_st::SelectionModel selection;
    CreateSelection(selection, {0, 0, 100, 100}, {20, 30}, {60, 70});

    ASSERT_TRUE(selection.Begin({30, 40}));
    EXPECT_EQ(selection.Operation(), open_st::SelectionOperation::Moving);
    ASSERT_TRUE(selection.Update({80, 90}));
    ExpectRectangle(selection.Snapshot().rectangle, {60, 60, 100, 100});
    ASSERT_TRUE(selection.End({80, 90}));
    EXPECT_EQ(selection.Snapshot().rectangle.Width(), 40);
    EXPECT_EQ(selection.Snapshot().rectangle.Height(), 40);
}

// 验证整体移动在含负坐标的桌面上分别钳制左、上、右、下边界，且各方向都不改变尺寸。
// 入参：无运行时形参；宏参数 SelectionModelTest 为测试套件，clamps_whole_selection_at_all_four_virtual_desktop_edges 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
TEST(SelectionModelTest, clamps_whole_selection_at_all_four_virtual_desktop_edges)
{
    struct MoveCase final
    {
        open_st::PointI target;
        open_st::RectI expected;
    };
    constexpr std::array<MoveCase, 4> cases{
        MoveCase{{-200, 10}, {-100, -10, -60, 30}},
        MoveCase{{0, -200}, {-20, -50, 20, -10}},
        MoveCase{{200, 10}, {60, -10, 100, 30}},
        MoveCase{{0, 200}, {-20, 60, 20, 100}},
    };

    for (const MoveCase& moveCase : cases)
    {
        open_st::SelectionModel selection;
        CreateSelection(selection, {-100, -50, 100, 100}, {-20, -10}, {20, 30});
        ASSERT_TRUE(selection.Begin({0, 10}));
        ASSERT_TRUE(selection.End(moveCase.target));
        ExpectRectangle(selection.Snapshot().rectangle, moveCase.expected);
        EXPECT_EQ(selection.Snapshot().rectangle.Width(), 40);
        EXPECT_EQ(selection.Snapshot().rectangle.Height(), 40);
    }
}

// 验证选区内部单击但没有产生位移时，矩形及最终状态保持不变。
// 入参：无运行时形参；宏参数 SelectionModelTest 为测试套件，an_interior_click_without_motion_has_no_effect 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
TEST(SelectionModelTest, an_interior_click_without_motion_has_no_effect)
{
    open_st::SelectionModel selection;
    CreateSelection(selection, {0, 0, 100, 100}, {20, 30}, {80, 90});

    ASSERT_TRUE(selection.Begin({50, 60}));
    EXPECT_FALSE(selection.Update({50, 60}));
    ASSERT_TRUE(selection.End({50, 60}));

    ExpectRectangle(selection.Snapshot().rectangle, {20, 30, 80, 90});
}

// 验证选区包含判断采用半开矩形：左上边界属于内部，右下边界及其外侧不属于内部。
// 入参：无运行时形参；宏参数 SelectionModelTest 为测试套件，contains_uses_half_open_rectangle_edges 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
TEST(SelectionModelTest, contains_uses_half_open_rectangle_edges)
{
    open_st::SelectionModel selection;
    CreateSelection(selection, {0, 0, 100, 100}, {20, 30}, {80, 90});

    EXPECT_TRUE(selection.Contains({20, 30}));
    EXPECT_TRUE(selection.Contains({79, 89}));
    EXPECT_FALSE(selection.Contains({80, 30}));
    EXPECT_FALSE(selection.Contains({20, 90}));
    EXPECT_FALSE(selection.Contains({19, 30}));
    EXPECT_FALSE(selection.Contains({20, 29}));
}

// 验证八个控制点的位置和 12×12 半开命中区，命中区正边界外的像素必须排除。
// 入参：无运行时形参；宏参数 SelectionModelTest 为测试套件，exposes_and_hit_tests_all_eight_handles 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
TEST(SelectionModelTest, exposes_and_hit_tests_all_eight_handles)
{
    open_st::SelectionModel selection;
    CreateSelection(selection, {0, 0, 100, 100}, {10, 20}, {90, 80});

    constexpr std::array<open_st::SelectionHandlePosition, 8> expected{
        open_st::SelectionHandlePosition{open_st::SelectionHandle::TopLeft, {10, 20}},
        open_st::SelectionHandlePosition{open_st::SelectionHandle::Top, {50, 20}},
        open_st::SelectionHandlePosition{open_st::SelectionHandle::TopRight, {90, 20}},
        open_st::SelectionHandlePosition{open_st::SelectionHandle::Right, {90, 50}},
        open_st::SelectionHandlePosition{open_st::SelectionHandle::BottomRight, {90, 80}},
        open_st::SelectionHandlePosition{open_st::SelectionHandle::Bottom, {50, 80}},
        open_st::SelectionHandlePosition{open_st::SelectionHandle::BottomLeft, {10, 80}},
        open_st::SelectionHandlePosition{open_st::SelectionHandle::Left, {10, 50}},
    };

    const std::array<open_st::SelectionHandlePosition, 8> handles = selection.Handles();
    for (std::size_t index = 0; index < handles.size(); ++index)
    {
        EXPECT_EQ(handles[index].handle, expected[index].handle);
        EXPECT_EQ(handles[index].center.x, expected[index].center.x);
        EXPECT_EQ(handles[index].center.y, expected[index].center.y);
        EXPECT_EQ(selection.HitTestHandle(handles[index].center), handles[index].handle);
    }
    EXPECT_EQ(selection.HitTestHandle({16, 20}), open_st::SelectionHandle::None);
    EXPECT_EQ(selection.HitTestHandle({4, 20}), open_st::SelectionHandle::TopLeft);
}

// 验证极小选区的控制点命中区域重叠时，等距情况下角控制点优先于边控制点。
// 入参：无运行时形参；宏参数 SelectionModelTest 为测试套件，corner_wins_a_tied_hit_test_on_a_tiny_selection 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
TEST(SelectionModelTest, corner_wins_a_tied_hit_test_on_a_tiny_selection)
{
    open_st::SelectionModel selection;
    CreateSelection(selection, {0, 0, 100, 100}, {10, 10}, {14, 14});

    EXPECT_EQ(selection.HitTestHandle({11, 11}), open_st::SelectionHandle::TopLeft);
}

// 验证八个控制点只修改各自负责的边，并把越界目标限制在桌面矩形内。
// 入参：无运行时形参；宏参数 SelectionModelTest 为测试套件，every_handle_resizes_its_owned_edges 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
TEST(SelectionModelTest, every_handle_resizes_its_owned_edges)
{
    struct ResizeCase final
    {
        open_st::SelectionHandle handle;
        open_st::PointI target;
        open_st::RectI expected;
    };
    constexpr std::array<ResizeCase, 8> cases{
        ResizeCase{open_st::SelectionHandle::TopLeft, {10, 15}, {10, 15, 80, 90}},
        ResizeCase{open_st::SelectionHandle::Top, {50, 15}, {20, 15, 80, 90}},
        ResizeCase{open_st::SelectionHandle::TopRight, {95, 15}, {20, 15, 95, 90}},
        ResizeCase{open_st::SelectionHandle::Right, {95, 60}, {20, 30, 95, 90}},
        ResizeCase{open_st::SelectionHandle::BottomRight, {95, 105}, {20, 30, 95, 100}},
        ResizeCase{open_st::SelectionHandle::Bottom, {50, 105}, {20, 30, 80, 100}},
        ResizeCase{open_st::SelectionHandle::BottomLeft, {10, 105}, {10, 30, 80, 100}},
        ResizeCase{open_st::SelectionHandle::Left, {10, 60}, {10, 30, 80, 90}},
    };

    for (const ResizeCase& resizeCase : cases)
    {
        SCOPED_TRACE(static_cast<int>(resizeCase.handle));
        open_st::SelectionModel selection;
        CreateSelection(selection, {0, 0, 100, 100}, {20, 30}, {80, 90});
        ASSERT_TRUE(selection.Begin(CenterFor(selection, resizeCase.handle)));
        EXPECT_EQ(selection.Operation(), open_st::SelectionOperation::Resizing);
        ASSERT_TRUE(selection.End(resizeCase.target));
        ExpectRectangle(selection.Snapshot().rectangle, resizeCase.expected);
    }
}

// 验证右侧和底部控制点拖到光标可达的最后一个像素时，选区仍扩展到 exclusive 桌面边界。
// 入参：无运行时形参；宏参数 SelectionModelTest 为测试套件，resizing_reaches_exclusive_right_and_bottom_bounds 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
TEST(SelectionModelTest, resizing_reaches_exclusive_right_and_bottom_bounds)
{
    open_st::SelectionModel selection;
    CreateSelection(selection, {0, 0, 100, 100}, {10, 10}, {50, 50});

    ASSERT_TRUE(selection.Begin(CenterFor(selection, open_st::SelectionHandle::Right)));
    ASSERT_TRUE(selection.End({99, 30}));
    ExpectRectangle(selection.Snapshot().rectangle, {10, 10, 100, 50});

    ASSERT_TRUE(selection.Begin(CenterFor(selection, open_st::SelectionHandle::Bottom)));
    ASSERT_TRUE(selection.End({50, 99}));
    ExpectRectangle(selection.Snapshot().rectangle, {10, 10, 100, 100});
}

// 验证调整边跨越固定对边后矩形仍然规范化，并把活动控制点翻转到对应方向。
// 入参：无运行时形参；宏参数 SelectionModelTest 为测试套件，crossing_opposite_edges_flips_the_active_handle 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
TEST(SelectionModelTest, crossing_opposite_edges_flips_the_active_handle)
{
    open_st::SelectionModel selection;
    CreateSelection(selection, {0, 0, 120, 120}, {20, 30}, {80, 90});

    ASSERT_TRUE(selection.Begin(CenterFor(selection, open_st::SelectionHandle::TopLeft)));
    ASSERT_TRUE(selection.Update({95, 100}));

    ExpectRectangle(selection.Snapshot().rectangle, {80, 90, 95, 100});
    EXPECT_EQ(selection.ActiveHandle(), open_st::SelectionHandle::BottomRight);
    ASSERT_TRUE(selection.End({95, 100}));
}

// 验证控制点恰好收拢到固定对边时，不接受零面积结果并恢复调整前的选区。
// 入参：无运行时形参；宏参数 SelectionModelTest 为测试套件，zero_area_resize_restores_the_previous_selection 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
TEST(SelectionModelTest, zero_area_resize_restores_the_previous_selection)
{
    open_st::SelectionModel selection;
    CreateSelection(selection, {0, 0, 100, 100}, {20, 30}, {80, 90});

    ASSERT_TRUE(selection.Begin(CenterFor(selection, open_st::SelectionHandle::Left)));
    ASSERT_TRUE(selection.End({80, 60}));

    EXPECT_EQ(selection.Phase(), open_st::SelectionPhase::Selected);
    ExpectRectangle(selection.Snapshot().rectangle, {20, 30, 80, 90});
}

// 验证取消创建会清空状态，而取消移动或缩放都会恢复操作开始前的矩形。
// 入参：无运行时形参；宏参数 SelectionModelTest 为测试套件，cancelling_each_interaction_restores_the_expected_state 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
TEST(SelectionModelTest, cancelling_each_interaction_restores_the_expected_state)
{
    open_st::SelectionModel selection;
    selection.SetBounds({0, 0, 100, 100});
    ASSERT_TRUE(selection.Begin({10, 10}));
    ASSERT_TRUE(selection.Update({40, 40}));
    ASSERT_TRUE(selection.CancelInteraction());
    EXPECT_EQ(selection.Phase(), open_st::SelectionPhase::Unselected);

    CreateSelection(selection, {0, 0, 100, 100}, {20, 30}, {80, 90});
    ASSERT_TRUE(selection.Begin({50, 60}));
    ASSERT_TRUE(selection.Update({60, 70}));
    ASSERT_TRUE(selection.CancelInteraction());
    ExpectRectangle(selection.Snapshot().rectangle, {20, 30, 80, 90});

    ASSERT_TRUE(selection.Begin(CenterFor(selection, open_st::SelectionHandle::BottomRight)));
    ASSERT_TRUE(selection.Update({95, 100}));
    ASSERT_TRUE(selection.CancelInteraction());
    ExpectRectangle(selection.Snapshot().rectangle, {20, 30, 80, 90});
}

// 验证渲染快照完全按值保存，后续移动模型不会反向改变已经取得的旧快照。
// 入参：无运行时形参；宏参数 SelectionModelTest 为测试套件，snapshot_is_an_immutable_value_copy 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
TEST(SelectionModelTest, snapshot_is_an_immutable_value_copy)
{
    open_st::SelectionModel selection;
    CreateSelection(selection, {0, 0, 100, 100}, {20, 30}, {80, 90});
    const open_st::SelectionSnapshot snapshot = selection.Snapshot();

    ASSERT_TRUE(selection.Begin({50, 60}));
    ASSERT_TRUE(selection.End({60, 70}));

    ExpectRectangle(snapshot.rectangle, {20, 30, 80, 90});
    EXPECT_TRUE(snapshot.hasSelection);
    EXPECT_TRUE(snapshot.showHandles);
}

// 验证边界缩小时优先保留原选区尺寸，仅在新边界容纳不下时收缩到完整边界。
// 入参：无运行时形参；宏参数 SelectionModelTest 为测试套件，changed_bounds_preserve_size_when_possible 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
TEST(SelectionModelTest, changed_bounds_preserve_size_when_possible)
{
    open_st::SelectionModel selection;
    CreateSelection(selection, {0, 0, 100, 100}, {40, 50}, {90, 95});

    selection.SetBounds({0, 0, 70, 80});

    ExpectRectangle(selection.Snapshot().rectangle, {20, 35, 70, 80});
    EXPECT_EQ(selection.Phase(), open_st::SelectionPhase::Selected);

    selection.SetBounds({-10, -5, 20, 15});

    ExpectRectangle(selection.Snapshot().rectangle, {-10, -5, 20, 15});
    EXPECT_EQ(selection.Phase(), open_st::SelectionPhase::Selected);
}
