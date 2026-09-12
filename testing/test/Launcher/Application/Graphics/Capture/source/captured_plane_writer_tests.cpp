// 文件职责：验证原生映射表面的旋转、行填充移除、逐字节保真和无效输入拒绝。

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
// 构造含驱动行填充且每像素带唯一编号的表面，用于验证旋转复制。
// 入参：无。
// 返回：3×2 BGRA 测试缓冲区，每行 14 字节，行末填充为 0xEE，像素首字节为编号。
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

// 逐像素比对目标 plane 中的编号，验证旋转映射没有错行错列。
// 入参：plane：待检查的冻结 plane；expected：按目标行序排列的预期像素编号。
// 返回：无返回值；通过 GoogleTest 断言报告 plane 有效性、尺寸和每个像素编号是否符合预期。
void ExpectPixelIds(const open_st::CapturedOutputPlane& plane, std::span<const std::uint8_t> expected)
{
    ASSERT_TRUE(plane.IsValid());
    ASSERT_EQ(static_cast<std::size_t>(plane.Width() * plane.Height()), expected.size());
    for (std::size_t index = 0U; index < expected.size(); ++index)
    {
        EXPECT_EQ(plane.Pixels()[index * 4U], expected[index]);
    }
}

// 调用实际 plane 构建逻辑生成指定旋转的带编号测试图像。
// 入参：rotation：要求验证的表面旋转；bounds：目标桌面方向物理像素边界。
// 返回：生成的自有冻结 plane；同时通过断言报告构建失败或非空诊断。
open_st::CapturedOutputPlane BuildBgraPlane(open_st::CapturedSurfaceRotation rotation, open_st::RectI bounds)
{
    const std::vector<std::uint8_t> source = MakePaddedBgraSurface();
    const open_st::MappedCaptureSurface surface{source.data(), 14U, 3, 2, open_st::CapturedPixelFormat::Bgra8Unorm};
    open_st::CapturedOutputPlane plane;
    std::wstring errorMessage;
    EXPECT_TRUE(open_st::BuildCapturedOutputPlane(surface, bounds, rotation,
                                                  open_st::CapturedColorSpace::SdrGamma22P709, {}, plane, errorMessage))
        << "Unexpected plane build failure";
    EXPECT_TRUE(errorMessage.empty());
    return plane;
}
} // namespace

// 验证全部格式和旋转在多行非对齐 RowPitch 下保留每个通道位模式，包括 FP16 特殊值和原 alpha。
// 入参：无运行入参；使用固定 3×2 像素排列和独立的预期索引表。
// 返回：无返回值；逐字节断言输出紧凑且不含行填充。
TEST(CapturedPlaneWriterTest, preserves_all_pixel_bytes_for_each_format_and_rotation)
{
    constexpr std::array<open_st::CapturedSurfaceRotation, 4> rotations{
        open_st::CapturedSurfaceRotation::Identity, open_st::CapturedSurfaceRotation::Rotate90,
        open_st::CapturedSurfaceRotation::Rotate180, open_st::CapturedSurfaceRotation::Rotate270};
    constexpr std::array<std::array<std::size_t, 6>, 4> orders{
        {{0, 1, 2, 3, 4, 5}, {3, 0, 4, 1, 5, 2}, {5, 4, 3, 2, 1, 0}, {2, 5, 1, 4, 0, 3}}};
    for (const open_st::CapturedPixelFormat format :
         {open_st::CapturedPixelFormat::Bgra8Unorm, open_st::CapturedPixelFormat::Rgb10A2Unorm,
          open_st::CapturedPixelFormat::Rgba16FloatScRgb})
    {
        const std::size_t pixelBytes = open_st::CapturedBytesPerPixel(format);
        const std::size_t rowPitch = 3U * pixelBytes + 5U;
        std::vector<std::uint8_t> source(rowPitch * 2U, 0xEEU);
        for (std::size_t index = 0; index < 6U; ++index)
            for (std::size_t byte = 0; byte < pixelBytes; ++byte)
                source[(index / 3U) * rowPitch + (index % 3U) * pixelBytes + byte] =
                    static_cast<std::uint8_t>(index * 31U + byte * 17U);
        const open_st::MappedCaptureSurface surface{source.data(), rowPitch, 3, 2, format};
        for (std::size_t rotation = 0; rotation < rotations.size(); ++rotation)
        {
            const bool quarterTurn = rotation == 1U || rotation == 3U;
            const open_st::RectI bounds{-5, -7, quarterTurn ? -3 : -2, quarterTurn ? -4 : -5};
            const open_st::CapturedColorSpace colorSpace = format == open_st::CapturedPixelFormat::Rgba16FloatScRgb
                                                               ? open_st::CapturedColorSpace::ScRgb
                                                               : open_st::CapturedColorSpace::SdrGamma22P709;
            open_st::CapturedOutputPlane plane;
            std::wstring error;
            ASSERT_TRUE(
                open_st::BuildCapturedOutputPlane(surface, bounds, rotations[rotation], colorSpace, {}, plane, error));
            ASSERT_EQ(plane.Pixels().size(), 6U * pixelBytes);
            for (std::size_t target = 0; target < 6U; ++target)
            {
                const std::size_t sourceIndex = orders[rotation][target];
                for (std::size_t byte = 0; byte < pixelBytes; ++byte)
                    EXPECT_EQ(plane.Pixels()[target * pixelBytes + byte],
                              source[(sourceIndex / 3U) * rowPitch + (sourceIndex % 3U) * pixelBytes + byte]);
            }
        }
    }
}

// 验证 identity、90、180、270 度旋转都生成桌面方向紧凑像素，且丢弃 RowPitch padding。
// 入参：无运行时形参；宏参数 CapturedPlaneWriterTest 为测试套件，normalizes_all_rotations_and_discards_row_padding
// 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
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
// 入参：无运行时形参；宏参数 CapturedPlaneWriterTest 为测试套件，preserves_fp16_bits_and_rejects_short_row_pitch
// 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
TEST(CapturedPlaneWriterTest, preserves_fp16_bits_and_rejects_short_row_pitch)
{
    constexpr std::array<std::uint8_t, 10> source{1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 0xEEU, 0xEEU};
    open_st::MappedCaptureSurface surface{source.data(), source.size(), 1, 1,
                                          open_st::CapturedPixelFormat::Rgba16FloatScRgb};
    open_st::CapturedOutputPlane plane;
    std::wstring errorMessage;
    ASSERT_TRUE(open_st::BuildCapturedOutputPlane(surface, {10, 20, 11, 21}, open_st::CapturedSurfaceRotation::Identity,
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
// 入参：无运行时形参；宏参数 CapturedPlaneWriterTest 为测试套件，preserves_rgb10_bits_while_discarding_padding
// 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
TEST(CapturedPlaneWriterTest, preserves_rgb10_bits_while_discarding_padding)
{
    constexpr std::array<std::uint8_t, 12> source{1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 0xEEU, 0xEEU, 0xEEU, 0xEEU};
    const open_st::MappedCaptureSurface surface{source.data(), source.size(), 2, 1,
                                                open_st::CapturedPixelFormat::Rgb10A2Unorm};
    open_st::CapturedOutputPlane plane;
    std::wstring errorMessage;
    ASSERT_TRUE(open_st::BuildCapturedOutputPlane(surface, {0, 0, 2, 1}, open_st::CapturedSurfaceRotation::Identity,
                                                  open_st::CapturedColorSpace::Hdr10, {}, plane, errorMessage));
    EXPECT_EQ(std::vector<std::uint8_t>(plane.Pixels().begin(), plane.Pixels().end()),
              (std::vector<std::uint8_t>{1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U}));
}

// 验证 FP16 的 8 Bpp 偏移在 90 度旋转时仍按完整像素搬运，不发生四字节错位。
// 入参：无运行时形参；宏参数 CapturedPlaneWriterTest 为测试套件，rotates_fp16_pixels_using_eight_byte_offsets
// 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
TEST(CapturedPlaneWriterTest, rotates_fp16_pixels_using_eight_byte_offsets)
{
    constexpr std::size_t rowPitch = 26U;
    std::vector<std::uint8_t> source(rowPitch * 2U, 0xEEU);
    for (std::size_t index = 0U; index < 6U; ++index)
    {
        source[(index / 3U) * rowPitch + (index % 3U) * 8U] = static_cast<std::uint8_t>(index + 1U);
    }
    const open_st::MappedCaptureSurface surface{source.data(), rowPitch, 3, 2,
                                                open_st::CapturedPixelFormat::Rgba16FloatScRgb};
    open_st::CapturedOutputPlane plane;
    std::wstring errorMessage;
    ASSERT_TRUE(open_st::BuildCapturedOutputPlane(surface, {0, 0, 2, 3}, open_st::CapturedSurfaceRotation::Rotate90,
                                                  open_st::CapturedColorSpace::ScRgb, {}, plane, errorMessage));
    constexpr std::array<std::uint8_t, 6> expected{4U, 1U, 5U, 2U, 6U, 3U};
    for (std::size_t index = 0U; index < expected.size(); ++index)
    {
        EXPECT_EQ(plane.Pixels()[index * 8U], expected[index]);
    }
}

// 验证尺寸与旋转不匹配、未知旋转和未知格式均清空旧 output 并返回诊断。
// 入参：无运行时形参；宏参数 CapturedPlaneWriterTest
// 为测试套件，rejects_invalid_dimensions_rotation_and_format_atomically 为用例名。
// 返回：无返回值；断言向 GoogleTest
// 报告该用例通过或失败。
TEST(CapturedPlaneWriterTest, rejects_invalid_dimensions_rotation_and_format_atomically)
{
    const std::vector<std::uint8_t> source = MakePaddedBgraSurface();
    open_st::MappedCaptureSurface surface{source.data(), 14U, 3, 2, open_st::CapturedPixelFormat::Bgra8Unorm};
    open_st::CapturedOutputPlane plane = BuildBgraPlane(open_st::CapturedSurfaceRotation::Identity, {0, 0, 3, 2});
    std::wstring errorMessage;
    EXPECT_FALSE(open_st::BuildCapturedOutputPlane(surface, {0, 0, 2, 2}, open_st::CapturedSurfaceRotation::Identity,
                                                   open_st::CapturedColorSpace::SdrGamma22P709, {}, plane,
                                                   errorMessage));
    EXPECT_FALSE(plane.IsValid());

    EXPECT_FALSE(
        open_st::BuildCapturedOutputPlane(surface, {0, 0, 3, 2}, static_cast<open_st::CapturedSurfaceRotation>(99),
                                          open_st::CapturedColorSpace::SdrGamma22P709, {}, plane, errorMessage));
    EXPECT_FALSE(plane.IsValid());

    surface.format = static_cast<open_st::CapturedPixelFormat>(99);
    EXPECT_FALSE(open_st::BuildCapturedOutputPlane(surface, {0, 0, 3, 2}, open_st::CapturedSurfaceRotation::Identity,
                                                   open_st::CapturedColorSpace::Unknown, {}, plane, errorMessage));
    EXPECT_FALSE(plane.IsValid());
    EXPECT_FALSE(errorMessage.empty());
}
