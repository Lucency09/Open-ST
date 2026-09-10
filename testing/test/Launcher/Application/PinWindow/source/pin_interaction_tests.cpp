// 验证贴图滚轮缩放、透明度及物理坐标边界，不依赖真实窗口或图形设备。

#include "pin_interaction_model.h"
#include <gtest/gtest.h>
#include <limits>

namespace
{
// 验证一格放大保持光标下的图像位置，负屏幕坐标不改变物理像素语义。
// 入参：无运行时入参；测试宏参数为注册名称。
// 返回：无返回值；断言检查缩放比例和新矩形。
TEST(PinInteractionTest, zoom_keeps_cursor_anchor_on_negative_screen)
{
    open_st::PinInteractionState state;
    const RECT result = open_st::ZoomPin(state, {200, 100}, {-300, -100, -100, 0}, {-200, -50}, WHEEL_DELTA);
    EXPECT_NEAR(state.scale, 1.1, 0.000001);
    EXPECT_EQ(result.left, -310);
    EXPECT_EQ(result.top, -105);
    EXPECT_EQ(result.right, -90);
    EXPECT_EQ(result.bottom, 5);
}

// 验证高精度半格滚轮累积到相同缩放比例，不把小于一格的输入丢弃。
// 入参：无运行时入参；测试宏参数为注册名称。
// 返回：无返回值；比较两次半格与一次整格的状态。
TEST(PinInteractionTest, fractional_wheel_preserves_scale)
{
    open_st::PinInteractionState state;
    RECT result{0, 0, 1000, 500};
    result = open_st::ZoomPin(state, {1000, 500}, result, {0, 0}, WHEEL_DELTA / 2);
    result = open_st::ZoomPin(state, {1000, 500}, result, {0, 0}, WHEEL_DELTA / 2);
    EXPECT_NEAR(state.scale, 1.1, 0.000001);
    EXPECT_EQ(result.right, 1100);
    EXPECT_EQ(result.bottom, 550);
}

// 验证缩放下限和 GPU 尺寸上限共同生效，避免零尺寸与超大窗口。
// 入参：无运行时入参；测试宏参数为注册名称。
// 返回：无返回值；检查边界比例和正的输出尺寸。
TEST(PinInteractionTest, zoom_obeys_scale_and_resource_limits)
{
    open_st::PinInteractionState state;
    RECT result = open_st::ZoomPin(state, {8192, 4096}, {0, 0, 8192, 4096}, {0, 0}, 120000);
    EXPECT_DOUBLE_EQ(state.scale, 2.0);
    EXPECT_EQ(result.right, 16384);
    result = open_st::ZoomPin(state, {8192, 4096}, result, {0, 0}, -120000);
    EXPECT_DOUBLE_EQ(state.scale, 0.1);
    EXPECT_GT(result.right, result.left);
    EXPECT_GT(result.bottom, result.top);
}

// 验证无效原图尺寸不修改窗口和已有缩放状态。
// 入参：无运行时入参；测试宏参数为注册名称。
// 返回：无返回值；比较输入输出矩形及比例。
TEST(PinInteractionTest, invalid_image_size_does_not_change_zoom)
{
    open_st::PinInteractionState state{2.0, 0.5F};
    const RECT result = open_st::ZoomPin(state, {0, 10}, {2, 3, 12, 13}, {4, 5}, WHEEL_DELTA);
    EXPECT_DOUBLE_EQ(state.scale, 2.0);
    EXPECT_EQ(result.left, 2);
    EXPECT_EQ(result.right, 12);
}

// 验证双击重置只恢复原始物理尺寸，保留透明度和左上角位置。
// 入参：无运行时入参；测试宏参数为注册名称。
// 返回：无返回值；检查缩放、透明度及矩形。
TEST(PinInteractionTest, reset_preserves_opacity_and_origin)
{
    open_st::PinInteractionState state{2.0, 0.35F};
    const RECT result = open_st::ResetPinZoom(state, {200, 100}, {-20, 30, 380, 230});
    EXPECT_DOUBLE_EQ(state.scale, 1.0);
    EXPECT_FLOAT_EQ(state.opacity, 0.35F);
    EXPECT_EQ(result.left, -20);
    EXPECT_EQ(result.top, 30);
    EXPECT_EQ(result.right, 180);
    EXPECT_EQ(result.bottom, 130);
}

// 验证透明度以每格五个百分点连续调整，并严格限制在百分之十到一百之间。
// 入参：无运行时入参；测试宏参数为注册名称。
// 返回：无返回值；检查半格、下限和上限。
TEST(PinInteractionTest, opacity_supports_fractional_steps_and_clamping)
{
    open_st::PinInteractionState state;
    open_st::AdjustPinOpacity(state, -WHEEL_DELTA / 2);
    EXPECT_NEAR(state.opacity, 0.975F, 0.000001F);
    open_st::AdjustPinOpacity(state, -120000);
    EXPECT_FLOAT_EQ(state.opacity, 0.1F);
    open_st::AdjustPinOpacity(state, 120000);
    EXPECT_FLOAT_EQ(state.opacity, 1.0F);
    EXPECT_DOUBLE_EQ(state.scale, 1.0);
}

// 验证大位移在 LONG 坐标边界饱和，窗口宽高保持不变。
// 入参：无运行时入参；测试宏参数为注册名称。
// 返回：无返回值；检查平移后四边仍可表示。
TEST(PinInteractionTest, movement_preserves_extent_at_coordinate_limits)
{
    const RECT result = open_st::MovePinRectangle({-30, 20, 70, 70}, 9000000000LL, -9000000000LL);
    EXPECT_EQ(result.right, (std::numeric_limits<LONG>::max)());
    EXPECT_EQ(result.top, (std::numeric_limits<LONG>::min)());
    EXPECT_EQ(result.right - result.left, 100);
    EXPECT_EQ(result.bottom - result.top, 50);
}
} // namespace
