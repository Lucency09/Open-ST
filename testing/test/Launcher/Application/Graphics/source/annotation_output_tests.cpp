// 文件职责：验证共享标注几何的透明度、裁剪、圆角和跨屏输出像素。
#include <array>
#include <gtest/gtest.h>
#include <limits>
#include <selection_output_renderer.h>

namespace open_st
{
namespace
{
// 创建带可辨识颜色和非透明度 X 字节的 SDR 冻结屏幕。
// 入参：bounds：物理像素矩形；blue：蓝通道区分不同屏幕。
// 返回：自有冻结 plane，所有像素的 X 都为 17。
CapturedOutputPlane Plane(RectI bounds, std::uint8_t blue = 40U)
{
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(bounds.Width()) * bounds.Height() * 4U);
    for (std::size_t index = 0U; index < pixels.size(); index += 4U)
    {
        pixels[index] = blue;
        pixels[index + 1U] = 80U;
        pixels[index + 2U] = 120U;
        pixels[index + 3U] = 17U;
    }
    return {bounds, CapturedPixelFormat::Bgra8Unorm, CapturedColorSpace::SdrGamma22P709, {}, std::move(pixels)};
}

// 读取输出中的一个相对物理像素。
// 入参：frame：有效输出；x、y：输出内坐标。
// 返回：B、G、R、X 四个字节。
std::array<std::uint8_t, 4U> Pixel(const SdrSelectionFrame& frame, int x, int y)
{
    const std::size_t offset = static_cast<std::size_t>(y) * frame.Stride() + static_cast<std::size_t>(x) * 4U;
    return {frame.Pixels()[offset], frame.Pixels()[offset + 1U], frame.Pixels()[offset + 2U],
            frame.Pixels()[offset + 3U]};
}

// 将单个对象固定为渲染期间不可修改的快照。
// 入参：object：待绘制对象。
// 返回：拥有独立向量的共享只读快照。
AnnotationSnapshot Snapshot(AnnotationObject object)
{
    return std::make_shared<const std::vector<AnnotationObject>>(std::vector<AnnotationObject>{object});
}
} // namespace

// 验证 0/50/100% 透明度，透明层外保留原 X，覆盖像素写入 X=255。
// 入参：无。
// 返回：像素断言报告 source-over 编码域计算结果。
TEST(AnnotationOutputTest, opacity_endpoints_and_uncovered_bytes)
{
    std::vector<CapturedOutputPlane> planes;
    planes.push_back(Plane({0, 0, 20, 20}));
    const FrozenDesktopFrame desktop({0, 0, 20, 20}, std::move(planes));
    SelectionOutputRenderer renderer;
    for (const unsigned transparency : {0U, 50U, 100U})
    {
        AnnotationObject object{1U, AnnotationKind::FilledRectangle, {4.0, 4.0}, {10.0, 10.0}};
        object.style.transparency = transparency;
        SdrSelectionFrame output;
        std::wstring error;
        ASSERT_TRUE(renderer.Render(desktop, {0, 0, 20, 20}, Snapshot(object), output, error));
        EXPECT_EQ(Pixel(output, 1, 1), (std::array<std::uint8_t, 4U>{40U, 80U, 120U, 17U}));
        const std::array<std::uint8_t, 4U> pixel = Pixel(output, 8, 8);
        if (transparency == 0U)
        {
            EXPECT_EQ(pixel, (std::array<std::uint8_t, 4U>{0U, 0U, 255U, 255U}));
        }
        else if (transparency == 100U)
        {
            EXPECT_EQ(pixel, (std::array<std::uint8_t, 4U>{40U, 80U, 120U, 17U}));
        }
        else
        {
            EXPECT_NEAR(pixel[0U], 20, 1);
            EXPECT_NEAR(pixel[1U], 40, 1);
            EXPECT_NEAR(pixel[2U], 188, 1);
            EXPECT_EQ(pixel[3U], 255U);
        }
    }
}

// 验证圆角保留角外底图，小矩形把圆角半径夹到短边的一半。
// 入参：无。
// 返回：几何覆盖与夹限的像素断言。
TEST(AnnotationOutputTest, rounded_corners_and_small_radius_clamp)
{
    std::vector<CapturedOutputPlane> planes;
    planes.push_back(Plane({0, 0, 32, 32}));
    const FrozenDesktopFrame desktop({0, 0, 32, 32}, std::move(planes));
    SelectionOutputRenderer renderer;
    AnnotationObject object{1U, AnnotationKind::RoundedRectangle, {4.0, 4.0}, {24.0, 24.0}};
    SdrSelectionFrame output;
    std::wstring error;
    ASSERT_TRUE(renderer.Render(desktop, {0, 0, 32, 32}, Snapshot(object), output, error));
    EXPECT_EQ(Pixel(output, 4, 4), (std::array<std::uint8_t, 4U>{40U, 80U, 120U, 17U}));
    EXPECT_EQ(Pixel(output, 16, 16), (std::array<std::uint8_t, 4U>{0U, 0U, 255U, 255U}));
    object.extent = {6.0, 4.0};
    ASSERT_TRUE(renderer.Render(desktop, {0, 0, 32, 32}, Snapshot(object), output, error));
    SdrSelectionFrame explicitRadius;
    object.cornerRadius = 2.0;
    ASSERT_TRUE(renderer.Render(desktop, {0, 0, 32, 32}, Snapshot(object), explicitRadius, error));
    EXPECT_EQ(std::vector<std::uint8_t>(output.Pixels().begin(), output.Pixels().end()),
              std::vector<std::uint8_t>(explicitRadius.Pixels().begin(), explicitRadius.Pixels().end()));
}

// 验证跨两屏的反向矩形连续合成，负桌面坐标和正式选区裁剪不引入拼缝。
// 入参：无。
// 返回：选区内覆盖与两侧未覆盖像素断言。
TEST(AnnotationOutputTest, negative_cross_screen_reverse_shape_and_clip)
{
    std::vector<CapturedOutputPlane> planes;
    planes.push_back(Plane({-20, -10, 0, 10}, 40U));
    planes.push_back(Plane({0, -10, 20, 10}, 60U));
    const FrozenDesktopFrame desktop({-20, -10, 20, 10}, std::move(planes));
    SelectionOutputRenderer renderer;
    AnnotationObject object{1U, AnnotationKind::FilledRectangle, {12.0, 12.0}, {-24.0, -24.0}};
    SdrSelectionFrame output;
    std::wstring error;
    ASSERT_TRUE(renderer.Render(desktop, {-15, -8, 15, 8}, Snapshot(object), output, error));
    EXPECT_EQ(output.Bounds().left, -15);
    EXPECT_EQ(Pixel(output, 14, 0), (std::array<std::uint8_t, 4U>{0U, 0U, 255U, 255U}));
    EXPECT_EQ(Pixel(output, 15, 15), (std::array<std::uint8_t, 4U>{0U, 0U, 255U, 255U}));
    EXPECT_EQ(Pixel(output, 0, 5), (std::array<std::uint8_t, 4U>{40U, 80U, 120U, 17U}));
    EXPECT_EQ(Pixel(output, 29, 5), (std::array<std::uint8_t, 4U>{60U, 80U, 120U, 17U}));
}

// 验证箭头肩部与杆身只合成一次透明度，不出现交叠加深。
// 入参：无。
// 返回：箭头各内部区域具有相同像素的断言。
TEST(AnnotationOutputTest, arrow_shaft_and_head_share_single_opacity)
{
    std::vector<CapturedOutputPlane> planes;
    planes.push_back(Plane({0, 0, 64, 32}));
    const FrozenDesktopFrame desktop({0, 0, 64, 32}, std::move(planes));
    AnnotationObject object{1U, AnnotationKind::Arrow, {4.0, 16.0}, {52.0, 0.0}};
    object.style.lineWidth = 4.0;
    object.style.transparency = 50U;
    SelectionOutputRenderer renderer;
    SdrSelectionFrame output;
    std::wstring error;
    ASSERT_TRUE(renderer.Render(desktop, {0, 0, 64, 32}, Snapshot(object), output, error));
    EXPECT_EQ(Pixel(output, 20, 16), Pixel(output, 40, 16));
    EXPECT_EQ(Pixel(output, 20, 16), Pixel(output, 45, 16));
    EXPECT_NEAR(Pixel(output, 40, 16)[2U], 188, 1);
}

// 验证无标注和空快照逐字节兼容，非法对象清空旧输出而不返回部分底图。
// 入参：无。
// 返回：兼容路径和失败原子性断言。
TEST(AnnotationOutputTest, empty_snapshot_compatibility_and_invalid_atomic_failure)
{
    std::vector<CapturedOutputPlane> planes;
    planes.push_back(Plane({0, 0, 10, 10}));
    const FrozenDesktopFrame desktop({0, 0, 10, 10}, std::move(planes));
    SelectionOutputRenderer renderer;
    SdrSelectionFrame original;
    SdrSelectionFrame output;
    std::wstring error;
    ASSERT_TRUE(renderer.Render(desktop, {0, 0, 10, 10}, original, error));
    for (const AnnotationSnapshot& snapshot :
         {AnnotationSnapshot{}, std::make_shared<const std::vector<AnnotationObject>>()})
    {
        ASSERT_TRUE(renderer.Render(desktop, {0, 0, 10, 10}, snapshot, output, error));
        EXPECT_EQ(std::vector<std::uint8_t>(original.Pixels().begin(), original.Pixels().end()),
                  std::vector<std::uint8_t>(output.Pixels().begin(), output.Pixels().end()));
    }
    AnnotationObject invalid{1U, AnnotationKind::FilledRectangle, {}, {5.0, 5.0}};
    invalid.style.transparency = 101U;
    EXPECT_FALSE(renderer.Render(desktop, {0, 0, 10, 10}, Snapshot(invalid), output, error));
    EXPECT_FALSE(output.IsValid());
    EXPECT_FALSE(error.empty());
}

// 验证空心矩形内部保留底图，后创建半透明对象按顺序覆盖前面的对象。
// 入参：无。
// 返回：边框、空心内部和叠放顺序的像素断言。
TEST(AnnotationOutputTest, outline_interior_and_object_order)
{
    std::vector<CapturedOutputPlane> planes;
    planes.push_back(Plane({0, 0, 32, 32}));
    const FrozenDesktopFrame desktop({0, 0, 32, 32}, std::move(planes));
    SelectionOutputRenderer renderer;
    AnnotationObject outline{1U, AnnotationKind::Rectangle, {4.0, 4.0}, {24.0, 24.0}};
    outline.style.lineWidth = 2.0;
    SdrSelectionFrame output;
    std::wstring error;
    ASSERT_TRUE(renderer.Render(desktop, {0, 0, 32, 32}, Snapshot(outline), output, error));
    EXPECT_EQ(Pixel(output, 16, 16), (std::array<std::uint8_t, 4U>{40U, 80U, 120U, 17U}));
    EXPECT_EQ(Pixel(output, 4, 16), (std::array<std::uint8_t, 4U>{0U, 0U, 255U, 255U}));
    AnnotationObject red{2U, AnnotationKind::FilledRectangle, {8.0, 8.0}, {16.0, 16.0}};
    AnnotationObject blue = red;
    blue.id = 3U;
    blue.style.rgb = 0x0000ffU;
    blue.style.transparency = 50U;
    const AnnotationSnapshot annotations =
        std::make_shared<const std::vector<AnnotationObject>>(std::vector<AnnotationObject>{red, blue});
    ASSERT_TRUE(renderer.Render(desktop, {0, 0, 32, 32}, annotations, output, error));
    EXPECT_NEAR(Pixel(output, 16, 16)[0U], 128, 1);
    EXPECT_EQ(Pixel(output, 16, 16)[1U], 0U);
    EXPECT_NEAR(Pixel(output, 16, 16)[2U], 127, 1);
}
} // namespace open_st
