#include <gtest/gtest.h>

#include "outside_mask_layout.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace
{
// 逐边比较实际矩形与预期矩形，使布局顺序或边界偏差能被直接定位。
void ExpectRectangle(const open_st::RectI& actual, const open_st::RectI& expected)
{
    EXPECT_EQ(actual.left, expected.left);
    EXPECT_EQ(actual.top, expected.top);
    EXPECT_EQ(actual.right, expected.right);
    EXPECT_EQ(actual.bottom, expected.bottom);
}

// 计算半开矩形覆盖的物理像素面积；空矩形面积为零。
std::int64_t RectangleArea(open_st::RectI rectangle) noexcept
{
    return rectangle.IsEmpty() ? 0 : static_cast<std::int64_t>(rectangle.Width()) * rectangle.Height();
}

// 计算两个半开矩形的交叠面积，用于证明遮罩条带不会重复压暗角落。
std::int64_t IntersectionArea(open_st::RectI first, open_st::RectI second) noexcept
{
    const int width = std::min(first.right, second.right) - std::max(first.left, second.left);
    const int height = std::min(first.bottom, second.bottom) - std::max(first.top, second.top);
    return width <= 0 || height <= 0 ? 0 : static_cast<std::int64_t>(width) * height;
}

// 汇总布局面积并逐对检查条带交集，验证遮罩既完整又没有重复区域。
void ExpectAreaAndNoOverlap(const open_st::OutsideMaskLayout& layout, std::int64_t expectedArea)
{
    std::int64_t actualArea = 0;
    for (std::size_t first = 0U; first < layout.count; ++first)
    {
        actualArea += RectangleArea(layout.rectangles[first]);
        for (std::size_t second = first + 1U; second < layout.count; ++second)
        {
            EXPECT_EQ(IntersectionArea(layout.rectangles[first], layout.rectangles[second]), 0);
        }
    }
    EXPECT_EQ(actualArea, expectedArea);
}
} // namespace

// 验证无选区或零面积选区时遮罩覆盖完整帧，而空帧不产生任何绘制矩形。
TEST(OutsideMaskLayoutTest, covers_the_full_frame_without_a_valid_selection)
{
    const open_st::OutsideMaskLayout noSelection =
        open_st::BuildOutsideMaskLayout({0, 0, 100, 80}, false, {});
    ASSERT_EQ(noSelection.count, 1U);
    ExpectRectangle(noSelection.rectangles[0], {0, 0, 100, 80});

    const open_st::OutsideMaskLayout zeroArea =
        open_st::BuildOutsideMaskLayout({0, 0, 100, 80}, true, {20, 30, 20, 50});
    ASSERT_EQ(zeroArea.count, 1U);
    ExpectRectangle(zeroArea.rectangles[0], {0, 0, 100, 80});

    EXPECT_EQ(open_st::BuildOutsideMaskLayout({}, false, {}).count, 0U);
}

// 验证居中选区严格拆成上、下、左、右四条，且总面积等于帧面积减选区面积。
TEST(OutsideMaskLayoutTest, splits_a_center_selection_into_four_non_overlapping_strips)
{
    const open_st::OutsideMaskLayout layout =
        open_st::BuildOutsideMaskLayout({0, 0, 100, 80}, true, {20, 10, 70, 60});

    ASSERT_EQ(layout.count, 4U);
    ExpectRectangle(layout.rectangles[0], {0, 0, 100, 10});
    ExpectRectangle(layout.rectangles[1], {0, 60, 100, 80});
    ExpectRectangle(layout.rectangles[2], {0, 10, 20, 60});
    ExpectRectangle(layout.rectangles[3], {70, 10, 100, 60});
    ExpectAreaAndNoOverlap(layout, 5'500);
}

// 验证全帧选区不产生遮罩条带，确保冻结帧全部保持原色。
TEST(OutsideMaskLayoutTest, emits_no_mask_for_a_full_frame_selection)
{
    const open_st::OutsideMaskLayout layout =
        open_st::BuildOutsideMaskLayout({-100, -50, 100, 80}, true, {-100, -50, 100, 80});

    EXPECT_EQ(layout.count, 0U);
}

// 验证选区贴住边或角时会跳过空条带，不产生零面积绘制命令。
TEST(OutsideMaskLayoutTest, omits_empty_strips_when_selection_touches_edges_and_corners)
{
    struct EdgeCase final
    {
        open_st::RectI selection;
        std::size_t expectedCount;
    };
    constexpr std::array<EdgeCase, 3> cases{
        EdgeCase{{0, 20, 40, 80}, 2U},
        EdgeCase{{0, 0, 40, 40}, 2U},
        EdgeCase{{60, 40, 100, 80}, 2U},
    };

    for (const EdgeCase& edgeCase : cases)
    {
        const open_st::OutsideMaskLayout layout =
            open_st::BuildOutsideMaskLayout({0, 0, 100, 80}, true, edgeCase.selection);
        EXPECT_EQ(layout.count, edgeCase.expectedCount);
        ExpectAreaAndNoOverlap(layout, 8'000 - RectangleArea(edgeCase.selection));
    }
}

// 验证负坐标多屏帧中的 1×1 右下选区遵守 exclusive 边界且只留下一个原色像素。
TEST(OutsideMaskLayoutTest, preserves_one_pixel_at_exclusive_right_and_bottom_bounds)
{
    const open_st::RectI frame{-100, -50, 100, 100};
    const open_st::OutsideMaskLayout layout =
        open_st::BuildOutsideMaskLayout(frame, true, {99, 99, 100, 100});

    ASSERT_EQ(layout.count, 2U);
    ExpectAreaAndNoOverlap(layout, RectangleArea(frame) - 1);
}

// 验证部分越界选区先裁到帧内再拆分，避免生成帧外坐标或漏画遮罩。
TEST(OutsideMaskLayoutTest, clips_a_partially_outside_selection_to_the_frame)
{
    const open_st::OutsideMaskLayout layout =
        open_st::BuildOutsideMaskLayout({0, 0, 100, 80}, true, {-30, -20, 50, 40});

    ASSERT_EQ(layout.count, 2U);
    ExpectRectangle(layout.rectangles[0], {0, 40, 100, 80});
    ExpectRectangle(layout.rectangles[1], {50, 0, 100, 40});
    ExpectAreaAndNoOverlap(layout, 6'000);
}

// 验证完全位于帧外的防御性快照不会开洞，而是继续绘制完整遮罩。
TEST(OutsideMaskLayoutTest, covers_the_full_frame_for_a_completely_outside_selection)
{
    const open_st::OutsideMaskLayout layout =
        open_st::BuildOutsideMaskLayout({0, 0, 100, 80}, true, {120, 100, 180, 160});

    ASSERT_EQ(layout.count, 1U);
    ExpectRectangle(layout.rectangles[0], {0, 0, 100, 80});
}
