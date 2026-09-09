// 文件职责：验证原生 plane 的格式、缓冲区大小及冻结桌面的范围包含和有效性约束。

#include <gtest/gtest.h>

#include <frozen_desktop_frame.h>

#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

// 验证冻结 plane 的格式字节数、紧凑布局、元数据和原始字节保持不变。
// 入参：无运行时形参；宏参数 FrozenDesktopFrameTest 为测试套件，owns_valid_tightly_packed_native_planes 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
TEST(FrozenDesktopFrameTest, owns_valid_tightly_packed_native_planes)
{
    open_st::OutputColorMetadata metadata{};
    metadata.displayColorSpace = open_st::CapturedColorSpace::Hdr10;
    metadata.bitsPerColor = 10U;
    metadata.maximumLuminance = 1000.0F;
    const std::vector<std::uint8_t> expected{1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};
    open_st::CapturedOutputPlane plane({-2, 4, 0, 5}, open_st::CapturedPixelFormat::Bgra8Unorm,
                                       open_st::CapturedColorSpace::SdrGamma22P709, metadata,
                                       std::vector<std::uint8_t>(expected));

    ASSERT_TRUE(plane.IsValid());
    EXPECT_EQ(plane.Width(), 2);
    EXPECT_EQ(plane.Height(), 1);
    EXPECT_EQ(plane.Stride(), 8U);
    EXPECT_EQ(plane.Format(), open_st::CapturedPixelFormat::Bgra8Unorm);
    EXPECT_EQ(plane.PixelColorSpace(), open_st::CapturedColorSpace::SdrGamma22P709);
    EXPECT_EQ(plane.ColorMetadata().bitsPerColor, 10U);
    EXPECT_EQ(std::vector<std::uint8_t>(plane.Pixels().begin(), plane.Pixels().end()), expected);
}

// 验证三种受支持格式的字节数，以及错误缓冲区大小不会形成有效 plane。
// 入参：无运行时形参；宏参数 FrozenDesktopFrameTest 为测试套件，rejects_inconsistent_plane_storage 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
TEST(FrozenDesktopFrameTest, rejects_inconsistent_plane_storage)
{
    EXPECT_EQ(open_st::CapturedBytesPerPixel(open_st::CapturedPixelFormat::Bgra8Unorm), 4U);
    EXPECT_EQ(open_st::CapturedBytesPerPixel(open_st::CapturedPixelFormat::Rgb10A2Unorm), 4U);
    EXPECT_EQ(open_st::CapturedBytesPerPixel(open_st::CapturedPixelFormat::Rgba16FloatScRgb), 8U);

    open_st::CapturedOutputPlane shortPlane({0, 0, 2, 1}, open_st::CapturedPixelFormat::Bgra8Unorm,
                                            open_st::CapturedColorSpace::SdrGamma22P709, {},
                                            {1U, 2U, 3U, 4U});
    open_st::CapturedOutputPlane emptyBounds({}, open_st::CapturedPixelFormat::Rgba16FloatScRgb,
                                             open_st::CapturedColorSpace::ScRgb, {}, {});
    open_st::CapturedOutputPlane contradictoryColorSpace(
        {0, 0, 1, 1}, open_st::CapturedPixelFormat::Rgba16FloatScRgb,
        open_st::CapturedColorSpace::SdrGamma22P709, {}, std::vector<std::uint8_t>(8U, 0U));
    EXPECT_FALSE(shortPlane.IsValid());
    EXPECT_FALSE(emptyBounds.IsValid());
    EXPECT_FALSE(contradictoryColorSpace.IsValid());
}

// 验证冻结桌面支持负坐标和不同格式，并拒绝越过虚拟桌面边界的输出。
// 入参：无运行时形参；宏参数 FrozenDesktopFrameTest 为测试套件，validates_output_containment_and_clear 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
TEST(FrozenDesktopFrameTest, validates_output_containment_and_clear)
{
    std::vector<open_st::CapturedOutputPlane> outputs;
    outputs.emplace_back(open_st::RectI{-2, 0, 0, 1}, open_st::CapturedPixelFormat::Bgra8Unorm,
                         open_st::CapturedColorSpace::SdrGamma22P709, open_st::OutputColorMetadata{},
                         std::vector<std::uint8_t>(8U, 1U));
    outputs.emplace_back(open_st::RectI{0, 0, 1, 1}, open_st::CapturedPixelFormat::Rgba16FloatScRgb,
                         open_st::CapturedColorSpace::ScRgb, open_st::OutputColorMetadata{},
                         std::vector<std::uint8_t>(8U, 2U));
    open_st::FrozenDesktopFrame frame({-2, 0, 2, 1}, std::move(outputs));

    ASSERT_TRUE(frame.IsValid());
    EXPECT_EQ(frame.Outputs().size(), 2U);
    frame.Clear();
    EXPECT_FALSE(frame.IsValid());
    EXPECT_TRUE(frame.Outputs().empty());

    std::vector<open_st::CapturedOutputPlane> outside;
    outside.emplace_back(open_st::RectI{0, 0, 3, 1}, open_st::CapturedPixelFormat::Bgra8Unorm,
                         open_st::CapturedColorSpace::SdrGamma22P709, open_st::OutputColorMetadata{},
                         std::vector<std::uint8_t>(12U, 1U));
    EXPECT_FALSE(open_st::FrozenDesktopFrame({0, 0, 2, 1}, std::move(outside)).IsValid());
}

// 验证高分辨率冻结对象只能移动不能复制，固定其大缓冲区所有权契约。
// 入参：无运行时形参；宏参数 FrozenDesktopFrameTest 为测试套件，is_move_only 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
TEST(FrozenDesktopFrameTest, is_move_only)
{
    static_assert(!std::is_copy_constructible_v<open_st::CapturedOutputPlane>);
    static_assert(!std::is_copy_assignable_v<open_st::CapturedOutputPlane>);
    static_assert(std::is_nothrow_move_constructible_v<open_st::CapturedOutputPlane>);
    static_assert(!std::is_copy_constructible_v<open_st::FrozenDesktopFrame>);
    static_assert(!std::is_copy_assignable_v<open_st::FrozenDesktopFrame>);
    static_assert(std::is_nothrow_move_constructible_v<open_st::FrozenDesktopFrame>);
    SUCCEED();
}
