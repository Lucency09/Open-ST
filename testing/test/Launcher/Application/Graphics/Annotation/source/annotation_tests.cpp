// 文件职责：验证独立标注对象的几何、样式、颜色输入及非法边界。
#include <annotation.h>
#include <gtest/gtest.h>
#include <limits>

namespace open_st
{
// 验证反向和负坐标形状有效，零面积及非有限值被拒绝。
// 入参：无。
// 返回：断言报告几何有效性。
TEST(AnnotationTest, signed_geometry_and_degenerate_boundaries)
{
    AnnotationObject object{1U, AnnotationKind::Rectangle, {-20.0, -40.0}, {-5.0, 7.0}};
    EXPECT_TRUE(IsValidAnnotation(object));
    object.extent.y = 0.0;
    EXPECT_FALSE(IsValidAnnotation(object));
    object.kind = AnnotationKind::Arrow;
    EXPECT_TRUE(IsValidAnnotation(object));
    object.extent.x = 0.0;
    EXPECT_FALSE(IsValidAnnotation(object));
    object.extent.x = std::numeric_limits<double>::infinity();
    EXPECT_FALSE(IsValidAnnotation(object));
    object.extent.x = 5.0;
    object.origin.y = std::numeric_limits<double>::quiet_NaN();
    EXPECT_FALSE(IsValidAnnotation(object));
}

// 验证透明度端点可用，越界样式和未知形状不能进入渲染。
// 入参：无。
// 返回：断言报告样式拒绝结果。
TEST(AnnotationTest, style_limits_and_unknown_kind)
{
    AnnotationObject object{1U, AnnotationKind::RoundedRectangle, {}, {2.0, 3.0}};
    EXPECT_TRUE(IsValidAnnotation(object));
    object.style.transparency = 100U;
    EXPECT_TRUE(IsValidAnnotation(object));
    object.style.transparency = 101U;
    EXPECT_FALSE(IsValidAnnotation(object));
    object.style.transparency = 0U;
    object.style.rgb = 0x1000000U;
    EXPECT_FALSE(IsValidAnnotation(object));
    object.style.rgb = 0U;
    object.style.lineWidth = 0.0;
    EXPECT_FALSE(IsValidAnnotation(object));
    object.style.lineWidth = 0.5;
    EXPECT_TRUE(IsValidAnnotation(object));
    object.style.lineWidth = 256.0;
    EXPECT_TRUE(IsValidAnnotation(object));
    object.style.lineWidth = 256.5;
    EXPECT_FALSE(IsValidAnnotation(object));
    object.style.lineWidth = std::numeric_limits<double>::quiet_NaN();
    EXPECT_FALSE(IsValidAnnotation(object));
    object.style.lineWidth = 3.0;
    object.cornerRadius = -1.0;
    EXPECT_FALSE(IsValidAnnotation(object));
    object.cornerRadius = 8.0;
    object.kind = static_cast<AnnotationKind>(99);
    EXPECT_FALSE(IsValidAnnotation(object));
}

// 验证严格颜色输入允许大小写，失败保留调用方旧值。
// 入参：无。
// 返回：断言报告文本到 RGB 的转换。
TEST(AnnotationTest, color_parse_is_strict_and_atomic)
{
    std::uint32_t rgb{0x123456U};
    EXPECT_TRUE(ParseAnnotationColor("#aBcD09", rgb));
    EXPECT_EQ(rgb, 0xabcd09U);
    for (const std::string_view text : {"", "abcdef", "#fff", "#12345g", " #123456", "#1234567"})
    {
        EXPECT_FALSE(ParseAnnotationColor(text, rgb));
        EXPECT_EQ(rgb, 0xabcd09U);
    }
}
} // namespace open_st
