#include <gtest/gtest.h>

#include "captured_plane_writer.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace
{
// 构造带两字节驱动行填充的 3×2 BGRA 表面，每个像素首字节保存唯一编号。
std::vector<std::uint8_t> MakePaddedBgraSurface()
{
    constexpr std::size_t rowPitch = 14U;
    std::vector<std::uint8_t> pixels(rowPitch * 2U, 0xEEU);
    for (std::size_t index = 0U; index < 6U; ++index)
    {
        const std::size_t row = index / 3U;
        const std::size_t column = index % 3U;
        pixels[row * rowPitch + column * 4U] = static_cast<std::uint8_t>(index + 1U);
    }
    return pixels;
}

// 按目标行列验证 plane 的像素编号矩阵，确保旋转映射没有错行或错列。
void ExpectPixelIds(const open_st::CapturedOutputPlane& plane, std::span<const std::uint8_t> expected)
{
    ASSERT_TRUE(plane.IsValid());
    ASSERT_EQ(static_cast<std::size_t>(plane.Width() * plane.Height()), expected.size());
    for (std::size_t index = 0U; index < expected.size(); ++index)
    {
        EXPECT_EQ(plane.Pixels()[index * 4U], expected[index]);
    }
}

// 构造指定旋转的 plane 并断言复制成功，减少四向矩阵测试重复代码。
open_st::CapturedOutputPlane BuildBgraPlane(open_st::CapturedSurfaceRotation rotation, open_st::RectI bounds)
{
    const std::vector<std::uint8_t> source = MakePaddedBgraSurface();
    const open_st::MappedCaptureSurface surface{source.data(), 14U, 3, 2,
                                                open_st::CapturedPixelFormat::Bgra8Unorm};
    open_st::CapturedOutputPlane plane;
    std::wstring errorMessage;
    EXPECT_TRUE(open_st::BuildCapturedOutputPlane(surface, bounds, rotation,
                                                  open_st::CapturedColorSpace::SdrGamma22P709, {},
                                                  plane, errorMessage))
        << "Unexpected plane build failure";
    EXPECT_TRUE(errorMessage.empty());
    return plane;
}
} // namespace

// 验证 identity、90、180、270 度旋转都生成桌面方向紧凑像素，且丢弃 RowPitch padding。
TEST(CapturedPlaneWriterTest, normalizes_all_rotations_and_discards_row_padding)
{
    const open_st::CapturedOutputPlane identity =
        BuildBgraPlane(open_st::CapturedSurfaceRotation::Identity, {-3, 5, 0, 7});
    constexpr std::array<std::uint8_t, 6> identityExpected{1U, 2U, 3U, 4U, 5U, 6U};
    ExpectPixelIds(identity, identityExpected);
    EXPECT_EQ(identity.Stride(), 12U);

    const open_st::CapturedOutputPlane rotate90 =
        BuildBgraPlane(open_st::CapturedSurfaceRotation::Rotate90, {-2, 5, 0, 8});
    constexpr std::array<std::uint8_t, 6> rotate90Expected{4U, 1U, 5U, 2U, 6U, 3U};
    ExpectPixelIds(rotate90, rotate90Expected);

    const open_st::CapturedOutputPlane rotate180 =
        BuildBgraPlane(open_st::CapturedSurfaceRotation::Rotate180, {-3, 5, 0, 7});
    constexpr std::array<std::uint8_t, 6> rotate180Expected{6U, 5U, 4U, 3U, 2U, 1U};
    ExpectPixelIds(rotate180, rotate180Expected);

    const open_st::CapturedOutputPlane rotate270 =
        BuildBgraPlane(open_st::CapturedSurfaceRotation::Rotate270, {-2, 5, 0, 8});
    constexpr std::array<std::uint8_t, 6> rotate270Expected{3U, 6U, 2U, 5U, 1U, 4U};
    ExpectPixelIds(rotate270, rotate270Expected);
}

// 验证 FP16 原始八字节像素逐字节保留，且无效 RowPitch 不发布部分 plane。
TEST(CapturedPlaneWriterTest, preserves_fp16_bits_and_rejects_short_row_pitch)
{
    constexpr std::array<std::uint8_t, 10> source{1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 0xEEU, 0xEEU};
    open_st::MappedCaptureSurface surface{source.data(), source.size(), 1, 1,
                                          open_st::CapturedPixelFormat::Rgba16FloatScRgb};
    open_st::CapturedOutputPlane plane;
    std::wstring errorMessage;
    ASSERT_TRUE(open_st::BuildCapturedOutputPlane(surface, {10, 20, 11, 21},
                                                  open_st::CapturedSurfaceRotation::Identity,
                                                  open_st::CapturedColorSpace::ScRgb, {}, plane, errorMessage));
    EXPECT_EQ(std::vector<std::uint8_t>(plane.Pixels().begin(), plane.Pixels().end()),
              (std::vector<std::uint8_t>{1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U}));

    surface.rowPitch = 7U;
    EXPECT_FALSE(open_st::BuildCapturedOutputPlane(surface, {10, 20, 11, 21},
                                                   open_st::CapturedSurfaceRotation::Identity,
                                                   open_st::CapturedColorSpace::ScRgb, {}, plane, errorMessage));
    EXPECT_FALSE(plane.IsValid());
    EXPECT_FALSE(errorMessage.empty());

    surface.rowPitch = source.size();
    surface.pixels = nullptr;
    EXPECT_FALSE(open_st::BuildCapturedOutputPlane(surface, {10, 20, 11, 21},
                                                   open_st::CapturedSurfaceRotation::Identity,
                                                   open_st::CapturedColorSpace::ScRgb, {}, plane, errorMessage));
    EXPECT_FALSE(plane.IsValid());
}

// 验证 RGB10A2 的四字节位模式在去除 RowPitch padding 后保持逐字节一致。
TEST(CapturedPlaneWriterTest, preserves_rgb10_bits_while_discarding_padding)
{
    constexpr std::array<std::uint8_t, 12> source{
        1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 0xEEU, 0xEEU, 0xEEU, 0xEEU};
    const open_st::MappedCaptureSurface surface{source.data(), source.size(), 2, 1,
                                                open_st::CapturedPixelFormat::Rgb10A2Unorm};
    open_st::CapturedOutputPlane plane;
    std::wstring errorMessage;
    ASSERT_TRUE(open_st::BuildCapturedOutputPlane(surface, {0, 0, 2, 1},
                                                  open_st::CapturedSurfaceRotation::Identity,
                                                  open_st::CapturedColorSpace::Hdr10, {}, plane,
                                                  errorMessage));
    EXPECT_EQ(std::vector<std::uint8_t>(plane.Pixels().begin(), plane.Pixels().end()),
              (std::vector<std::uint8_t>{1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U}));
}

// 验证 FP16 的 8 Bpp 偏移在 90 度旋转时仍按完整像素搬运，不发生四字节错位。
TEST(CapturedPlaneWriterTest, rotates_fp16_pixels_using_eight_byte_offsets)
{
    constexpr std::size_t rowPitch = 26U;
    std::vector<std::uint8_t> source(rowPitch * 2U, 0xEEU);
    for (std::size_t index = 0U; index < 6U; ++index)
    {
        source[(index / 3U) * rowPitch + (index % 3U) * 8U] =
            static_cast<std::uint8_t>(index + 1U);
    }
    const open_st::MappedCaptureSurface surface{source.data(), rowPitch, 3, 2,
                                                open_st::CapturedPixelFormat::Rgba16FloatScRgb};
    open_st::CapturedOutputPlane plane;
    std::wstring errorMessage;
    ASSERT_TRUE(open_st::BuildCapturedOutputPlane(surface, {0, 0, 2, 3},
                                                  open_st::CapturedSurfaceRotation::Rotate90,
                                                  open_st::CapturedColorSpace::ScRgb, {}, plane,
                                                  errorMessage));
    constexpr std::array<std::uint8_t, 6> expected{4U, 1U, 5U, 2U, 6U, 3U};
    for (std::size_t index = 0U; index < expected.size(); ++index)
    {
        EXPECT_EQ(plane.Pixels()[index * 8U], expected[index]);
    }
}

// 验证尺寸与旋转不匹配、未知旋转和未知格式均清空旧 output 并返回诊断。
TEST(CapturedPlaneWriterTest, rejects_invalid_dimensions_rotation_and_format_atomically)
{
    const std::vector<std::uint8_t> source = MakePaddedBgraSurface();
    open_st::MappedCaptureSurface surface{source.data(), 14U, 3, 2,
                                          open_st::CapturedPixelFormat::Bgra8Unorm};
    open_st::CapturedOutputPlane plane =
        BuildBgraPlane(open_st::CapturedSurfaceRotation::Identity, {0, 0, 3, 2});
    std::wstring errorMessage;
    EXPECT_FALSE(open_st::BuildCapturedOutputPlane(surface, {0, 0, 2, 2},
                                                   open_st::CapturedSurfaceRotation::Identity,
                                                   open_st::CapturedColorSpace::SdrGamma22P709, {}, plane,
                                                   errorMessage));
    EXPECT_FALSE(plane.IsValid());

    EXPECT_FALSE(open_st::BuildCapturedOutputPlane(
        surface, {0, 0, 3, 2}, static_cast<open_st::CapturedSurfaceRotation>(99),
        open_st::CapturedColorSpace::SdrGamma22P709, {}, plane, errorMessage));
    EXPECT_FALSE(plane.IsValid());

    surface.format = static_cast<open_st::CapturedPixelFormat>(99);
    EXPECT_FALSE(open_st::BuildCapturedOutputPlane(surface, {0, 0, 3, 2},
                                                   open_st::CapturedSurfaceRotation::Identity,
                                                   open_st::CapturedColorSpace::Unknown, {}, plane,
                                                   errorMessage));
    EXPECT_FALSE(plane.IsValid());
    EXPECT_FALSE(errorMessage.empty());
}
