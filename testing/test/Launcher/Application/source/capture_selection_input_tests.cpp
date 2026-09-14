// 文件职责：验证窗口单击候选的锁定、容差边界、消息合并及取消，不依赖原生鼠标和显示器。

#include <gtest/gtest.h>

#include "capture_selection_input.h"

#include <array>
#include <limits>

namespace
{
// 比较锁定候选的四条边界，避免候选在移动期间被改写而未被发现。
// 入参：actual：实际候选；expected：预期半开物理像素矩形。
// 返回：无返回值；边界差异由断言报告。
void ExpectRectangle(open_st::RectI actual, open_st::RectI expected)
{
    EXPECT_EQ(actual.left, expected.left);
    EXPECT_EQ(actual.top, expected.top);
    EXPECT_EQ(actual.right, expected.right);
    EXPECT_EQ(actual.bottom, expected.bottom);
}
} // namespace

// 验证候选锁定后允许微动，拒绝重复按下替换，并只在抬起时发出一次采用结果。
// 入参：无。
// 返回：无返回值；通过断言验证候选值、副本隔离和单次采用。
TEST(CaptureSelectionInputTest, click_locks_candidate_until_release)
{
    open_st::CaptureSelectionInput input;
    open_st::RectI candidate{-100, -50, 200, 100};
    ASSERT_TRUE(input.Begin({0, 0}, candidate, 6, 8));
    candidate = {10, 20, 30, 40};
    EXPECT_FALSE(input.Begin({10, 20}, candidate, 2, 2));
    EXPECT_FALSE(input.Move({2, 3}));
    EXPECT_TRUE(input.Pending());
    ExpectRectangle(input.Candidate(), {-100, -50, 200, 100});
    const open_st::CaptureSelectionFinish result = input.Finish({-3, -4});
    EXPECT_EQ(result.action, open_st::CaptureSelectionAction::SelectCandidate);
    ExpectRectangle(result.rectangle, {-100, -50, 200, 100});
    EXPECT_EQ(result.origin.x, 0);
    EXPECT_EQ(result.origin.y, 0);
    EXPECT_FALSE(input.Pending());
    EXPECT_TRUE(input.Candidate().IsEmpty());
    EXPECT_EQ(input.Finish({0, 0}).action, open_st::CaptureSelectionAction::None);
}

// 验证偶数容差的左上包含、右下排除，水平和垂直任一方向越界都立即进入拖动。
// 入参：无。
// 返回：无返回值；分别断言四条边内外的判定结果。
TEST(CaptureSelectionInputTest, even_tolerance_observes_each_half_open_edge)
{
    constexpr std::array<open_st::PointI, 4> inside{{{98, 200}, {101, 200}, {100, 197}, {100, 202}}};
    constexpr std::array<open_st::PointI, 4> outside{{{97, 200}, {102, 200}, {100, 196}, {100, 203}}};
    for (open_st::PointI point : inside)
    {
        open_st::CaptureSelectionInput input;
        ASSERT_TRUE(input.Begin({100, 200}, {50, 150, 150, 250}, 4, 6));
        EXPECT_FALSE(input.Move(point));
        EXPECT_TRUE(input.Pending());
    }
    for (open_st::PointI point : outside)
    {
        open_st::CaptureSelectionInput input;
        ASSERT_TRUE(input.Begin({100, 200}, {50, 150, 150, 250}, 4, 6));
        EXPECT_TRUE(input.Move(point));
        EXPECT_FALSE(input.Pending());
    }
}

// 验证奇数容差保留完整尺寸，单像素容差仍允许原地单击但不允许任意移动。
// 入参：无。
// 返回：无返回值；断言奇数右边界和最小有效容差。
TEST(CaptureSelectionInputTest, odd_and_single_pixel_tolerances_preserve_full_size)
{
    open_st::CaptureSelectionInput input;
    ASSERT_TRUE(input.Begin({-10, -20}, {-30, -40, 0, 0}, 5, 3));
    EXPECT_FALSE(input.Move({-12, -21}));
    EXPECT_FALSE(input.Move({-8, -19}));
    EXPECT_TRUE(input.Move({-7, -20}));
    input.Cancel();
    ASSERT_TRUE(input.Begin({0, 0}, {-5, -5, 5, 5}, 1, 1));
    EXPECT_EQ(input.Finish({0, 0}).action, open_st::CaptureSelectionAction::SelectCandidate);
    ASSERT_TRUE(input.Begin({0, 0}, {-5, -5, 5, 5}, 1, 1));
    EXPECT_TRUE(input.Move({0, -1}));
}

// 验证鼠标消息合并导致没有 MOVE 时，抬起越界仍从按下起点启动自由框选。
// 入参：无。
// 返回：无返回值；断言抬起补判与原点保留。
TEST(CaptureSelectionInputTest, release_without_move_can_start_drag)
{
    open_st::CaptureSelectionInput input;
    ASSERT_TRUE(input.Begin({-500, 100}, {-600, 0, -200, 300}, 8, 8));
    const open_st::CaptureSelectionFinish result = input.Finish({400, -200});
    EXPECT_EQ(result.action, open_st::CaptureSelectionAction::BeginDrag);
    EXPECT_EQ(result.origin.x, -500);
    EXPECT_EQ(result.origin.y, 100);
    EXPECT_FALSE(input.Pending());
}

// 验证首次拖出只发一次创建通知，返回起点和抬起都不能重新变成窗口单击。
// 入参：无。
// 返回：无返回值；断言拖动不可逆及创建起点仍可读取。
TEST(CaptureSelectionInputTest, drag_never_reverts_to_click_when_pointer_returns)
{
    open_st::CaptureSelectionInput input;
    ASSERT_TRUE(input.Begin({20, 30}, {0, 0, 100, 100}, 4, 4));
    EXPECT_TRUE(input.Move({22, 30}));
    EXPECT_EQ(input.Origin().x, 20);
    EXPECT_EQ(input.Origin().y, 30);
    EXPECT_FALSE(input.Move({60, 90}));
    EXPECT_FALSE(input.Move({20, 30}));
    EXPECT_EQ(input.Finish({20, 30}).action, open_st::CaptureSelectionAction::None);
}

// 验证取消可以重复调用，失捕后的残留抬起不会采用旧候选，下一次按下仍可正常处理。
// 入参：无。
// 返回：无返回值；断言取消隔离旧消息及复用状态。
TEST(CaptureSelectionInputTest, cancel_discards_pending_and_allows_next_interaction)
{
    open_st::CaptureSelectionInput input;
    ASSERT_TRUE(input.Begin({10, 10}, {0, 0, 100, 100}, 4, 4));
    input.Cancel();
    input.Cancel();
    EXPECT_FALSE(input.Pending());
    EXPECT_FALSE(input.Move({1000, 1000}));
    EXPECT_EQ(input.Finish({10, 10}).action, open_st::CaptureSelectionAction::None);
    EXPECT_TRUE(input.Candidate().IsEmpty());
    ASSERT_TRUE(input.Begin({10, 10}, {5, 5, 15, 15}, 4, 4));
    EXPECT_EQ(input.Finish({10, 10}).action, open_st::CaptureSelectionAction::SelectCandidate);
}

// 验证无候选和非法系统容差不会建立等待状态，留给 App 直接进入手动框选。
// 入参：无。
// 返回：无返回值；断言每一种无效输入都没有后续采用结果。
TEST(CaptureSelectionInputTest, invalid_candidate_and_tolerances_are_rejected)
{
    open_st::CaptureSelectionInput input;
    EXPECT_FALSE(input.Begin({0, 0}, {}, 4, 4));
    EXPECT_FALSE(input.Begin({0, 0}, {10, 0, 5, 10}, 4, 4));
    EXPECT_FALSE(input.Begin({0, 0}, {0, 0, 10, 10}, 0, 4));
    EXPECT_FALSE(input.Begin({0, 0}, {0, 0, 10, 10}, 4, -1));
    EXPECT_FALSE(input.Pending());
    EXPECT_EQ(input.Finish({0, 0}).action, open_st::CaptureSelectionAction::None);
}

// 验证起点接近整数极值时容差计算不会溢出，跨整个坐标域的移动仍判为拖动。
// 入参：无。
// 返回：无返回值；断言极值内单击和极值间移动。
TEST(CaptureSelectionInputTest, extreme_coordinates_do_not_overflow_tolerance)
{
    constexpr int LOW = std::numeric_limits<int>::min();
    constexpr int HIGH = std::numeric_limits<int>::max();
    open_st::CaptureSelectionInput input;
    ASSERT_TRUE(input.Begin({LOW, HIGH}, {LOW, HIGH - 10, LOW + 10, HIGH}, HIGH, HIGH));
    EXPECT_FALSE(input.Move({LOW + 1, HIGH - 1}));
    EXPECT_TRUE(input.Move({HIGH, LOW}));
    input.Cancel();
    ASSERT_TRUE(input.Begin({HIGH, LOW}, {HIGH - 10, LOW, HIGH, LOW + 10}, 4, 4));
    EXPECT_EQ(input.Finish({HIGH, LOW}).action, open_st::CaptureSelectionAction::SelectCandidate);
}
