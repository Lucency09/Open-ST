#include <gtest/gtest.h>

#include <color_conversion.h>
#include <desktop_preview.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace
{
// 把 RGBA16F 通道序列打包为一行原生像素，支持灰阶及超范围颜色测试。
std::vector<std::uint8_t> MakeHalfPixels(const std::vector<std::uint16_t>& channels)
{
    std::vector<std::uint8_t> pixels(channels.size() * sizeof(std::uint16_t));
    std::memcpy(pixels.data(), channels.data(), pixels.size());
    return pixels;
}

// 把 RGB10A2 位模式写成原生四字节像素。
std::vector<std::uint8_t> MakeRgb10Pixel(std::uint32_t packed)
{
    std::vector<std::uint8_t> pixels(sizeof(packed));
    std::memcpy(pixels.data(), &packed, sizeof(packed));
    return pixels;
}

// 从呈现像素中解码指定 FP16 通道，避免未对齐的指针类型转换。
float ReadHalfChannel(const open_st::OutputPreviewFrame& preview, std::size_t channel)
{
    std::uint16_t half{};
    std::memcpy(&half, preview.pixels.data() + channel * sizeof(half), sizeof(half));
    return open_st::DecodeFloat16(half);
}

// 创建具有明确 SDR 白值的 HDR 输出元数据，峰值与参考白故意不同。
open_st::OutputColorMetadata HdrMetadata(float whiteNits = 160.0F)
{
    open_st::OutputColorMetadata metadata{};
    metadata.displayColorSpace = open_st::CapturedColorSpace::Hdr10;
    metadata.maximumLuminance = 1000.0F;
    metadata.hasSdrWhiteLevel = true;
    metadata.sdrWhiteLevelNits = whiteNits;
    metadata.targetId = 42U;
    return metadata;
}
} // namespace

// 验证 SDR BGRA 保留所有像素字节与负坐标，不因 HDR 预览改造改变普通截图亮度。
TEST(DesktopPreviewTest, preserves_sdr_bgra_bytes_and_output_coordinates)
{
    const std::vector<std::uint8_t> original{10U, 20U, 30U, 0U, 40U, 50U, 60U, 255U};
    open_st::CapturedOutputPlane plane({-2, -1, 0, 0}, open_st::CapturedPixelFormat::Bgra8Unorm,
                                       open_st::CapturedColorSpace::SdrGamma22P709, {}, original);
    open_st::OutputPreviewFrame preview;
    std::wstring errorMessage;
    ASSERT_TRUE(open_st::BuildOutputPreview(plane, preview, errorMessage));
    EXPECT_EQ(preview.pixels, original);
    EXPECT_EQ(preview.bounds.left, -2);
    EXPECT_EQ(preview.bounds.top, -1);
    EXPECT_EQ(preview.stride, 8U);
    EXPECT_EQ(preview.pixelFormat, open_st::CapturedPixelFormat::Bgra8Unorm);
    EXPECT_FLOAT_EQ(preview.uiWhiteScale, 1.0F);
    EXPECT_FALSE(preview.compatibilityMode);
    EXPECT_TRUE(errorMessage.empty());
}

// 验证 FP16 原生 RGB 位模式逐位保留，灰阶、负分量及高光不经过 ACES 或 SDR 白重复缩放。
TEST(DesktopPreviewTest, preserves_native_half_rgb_bits_without_tone_mapping_or_white_scaling)
{
    const std::vector<std::uint16_t> channels{
        0x0000U, 0x0000U, 0x0000U, 0U, 0x3400U, 0x3400U, 0x3400U, 0U,
        0x3800U, 0x3800U, 0x3800U, 0U, 0x3C00U, 0x3C00U, 0x3C00U, 0U,
        0xBC00U, 0x4000U, 0x4400U, 0U};
    const std::vector<std::uint8_t> original = MakeHalfPixels(channels);
    open_st::CapturedOutputPlane plane({-5, 0, 0, 1}, open_st::CapturedPixelFormat::Rgba16FloatScRgb,
                                       open_st::CapturedColorSpace::ScRgb, HdrMetadata(), original);
    open_st::OutputPreviewFrame preview;
    std::wstring errorMessage;
    ASSERT_TRUE(open_st::BuildOutputPreview(plane, preview, errorMessage));
    ASSERT_EQ(preview.pixels.size(), original.size());
    for (std::size_t pixel = 0U; pixel < 5U; ++pixel)
    {
        EXPECT_EQ(std::memcmp(preview.pixels.data() + pixel * 8U, original.data() + pixel * 8U, 6U), 0);
        EXPECT_FLOAT_EQ(ReadHalfChannel(preview, pixel * 4U + 3U), 1.0F);
    }
    EXPECT_FLOAT_EQ(ReadHalfChannel(preview, 16U), -1.0F);
    EXPECT_FLOAT_EQ(ReadHalfChannel(preview, 17U), 2.0F);
    EXPECT_FLOAT_EQ(ReadHalfChannel(preview, 18U), 4.0F);
    EXPECT_EQ(preview.pixelFormat, open_st::CapturedPixelFormat::Rgba16FloatScRgb);
    EXPECT_FLOAT_EQ(preview.uiWhiteScale, 2.0F);
    EXPECT_EQ(preview.colorMetadata.targetId, 42U);
    EXPECT_FALSE(preview.compatibilityMode);
    EXPECT_EQ(std::vector<std::uint8_t>(plane.Pixels().begin(), plane.Pixels().end()), original);
}

// 验证 HDR10 PQ/BT.2020 红色进入 FP16 时保留负色域分量与高光，且不乘 SDR 白比例。
TEST(DesktopPreviewTest, converts_hdr10_to_extended_linear_half_without_clipping)
{
    open_st::CapturedOutputPlane plane({0, 0, 1, 1}, open_st::CapturedPixelFormat::Rgb10A2Unorm,
                                       open_st::CapturedColorSpace::Hdr10, HdrMetadata(240.0F),
                                       MakeRgb10Pixel(0x000003FFU));
    open_st::OutputPreviewFrame preview;
    std::wstring errorMessage;
    ASSERT_TRUE(open_st::BuildOutputPreview(plane, preview, errorMessage));
    ASSERT_EQ(preview.pixels.size(), 8U);
    EXPECT_NEAR(ReadHalfChannel(preview, 0U), 207.56F, 0.2F);
    EXPECT_LT(ReadHalfChannel(preview, 1U), 0.0F);
    EXPECT_LT(ReadHalfChannel(preview, 2U), 0.0F);
    EXPECT_FLOAT_EQ(ReadHalfChannel(preview, 3U), 1.0F);
    EXPECT_FLOAT_EQ(preview.uiWhiteScale, 3.0F);
    EXPECT_FALSE(preview.compatibilityMode);
}

// 验证 HDR 屏幕的 BGRA 兼容数据只进行一次 sRGB 解码及 SDR 白缩放，并明确标记降级。
TEST(DesktopPreviewTest, marks_hdr_bgra_compatibility_and_scales_sdr_white_once)
{
    open_st::CapturedOutputPlane plane({0, 0, 1, 1}, open_st::CapturedPixelFormat::Bgra8Unorm,
                                       open_st::CapturedColorSpace::SdrGamma22P709, HdrMetadata(),
                                       {0U, 128U, 255U, 0U});
    open_st::OutputPreviewFrame preview;
    std::wstring errorMessage;
    ASSERT_TRUE(open_st::BuildOutputPreview(plane, preview, errorMessage));
    EXPECT_TRUE(preview.compatibilityMode);
    EXPECT_EQ(preview.pixelFormat, open_st::CapturedPixelFormat::Rgba16FloatScRgb);
    EXPECT_FLOAT_EQ(ReadHalfChannel(preview, 0U), 2.0F);
    EXPECT_NEAR(ReadHalfChannel(preview, 1U), 0.43172F, 0.001F);
    EXPECT_FLOAT_EQ(ReadHalfChannel(preview, 2U), 0.0F);
}

// 验证 SDR RGB10 的正确通道量化，以及 SDR 屏幕上 FP16 数据不依赖 HDR 白值查询。
TEST(DesktopPreviewTest, supports_sdr_rgb10_and_half_outputs_independently)
{
    open_st::CapturedOutputPlane rgb10({0, 0, 1, 1}, open_st::CapturedPixelFormat::Rgb10A2Unorm,
                                       open_st::CapturedColorSpace::SdrGamma22P709, {},
                                       MakeRgb10Pixel(0x000FFC00U));
    open_st::CapturedOutputPlane half({1, 0, 2, 1}, open_st::CapturedPixelFormat::Rgba16FloatScRgb,
                                      open_st::CapturedColorSpace::ScRgb, {},
                                      MakeHalfPixels({0x3800U, 0x3800U, 0x3800U, 0x3C00U}));
    open_st::OutputPreviewFrame rgb10Preview;
    open_st::OutputPreviewFrame halfPreview;
    std::wstring errorMessage;
    ASSERT_TRUE(open_st::BuildOutputPreview(rgb10, rgb10Preview, errorMessage));
    ASSERT_TRUE(open_st::BuildOutputPreview(half, halfPreview, errorMessage));
    EXPECT_EQ(rgb10Preview.pixels, (std::vector<std::uint8_t>{0U, 255U, 0U, 255U}));
    EXPECT_EQ(halfPreview.pixelFormat, open_st::CapturedPixelFormat::Rgba16FloatScRgb);
    EXPECT_FLOAT_EQ(ReadHalfChannel(halfPreview, 0U), 0.5F);
    EXPECT_FLOAT_EQ(halfPreview.uiWhiteScale, 1.0F);
}

// 验证 HDR 白值缺失、零、负数和非有限值均显式失败，旧结果不会泄漏到后续会话。
TEST(DesktopPreviewTest, rejects_missing_or_invalid_hdr_white_metadata)
{
    const std::array<float, 4U> invalidValues{0.0F, -80.0F, std::numeric_limits<float>::infinity(),
                                            std::numeric_limits<float>::quiet_NaN()};
    for (const float whiteNits : invalidValues)
    {
        open_st::CapturedOutputPlane plane({0, 0, 1, 1}, open_st::CapturedPixelFormat::Rgba16FloatScRgb,
                                           open_st::CapturedColorSpace::ScRgb, HdrMetadata(whiteNits),
                                           MakeHalfPixels({0x3C00U, 0U, 0U, 0x3C00U}));
        open_st::OutputPreviewFrame preview;
        preview.pixels = {1U};
        std::wstring errorMessage;
        EXPECT_FALSE(open_st::BuildOutputPreview(plane, preview, errorMessage));
        EXPECT_TRUE(preview.pixels.empty());
        EXPECT_FALSE(errorMessage.empty());
    }
    open_st::OutputColorMetadata metadata = HdrMetadata();
    metadata.hasSdrWhiteLevel = false;
    open_st::CapturedOutputPlane plane({0, 0, 1, 1}, open_st::CapturedPixelFormat::Bgra8Unorm,
                                       open_st::CapturedColorSpace::SdrGamma22P709, metadata, {0U, 0U, 0U, 255U});
    open_st::OutputPreviewFrame preview;
    std::wstring errorMessage;
    EXPECT_FALSE(open_st::BuildOutputPreview(plane, preview, errorMessage));
}

// 验证未知 RGB10 颜色空间不被猜测为 HDR10/SDR，且无效 plane 明确清空输出。
TEST(DesktopPreviewTest, rejects_unknown_rgb10_and_invalid_planes)
{
    open_st::CapturedOutputPlane plane({0, 0, 1, 1}, open_st::CapturedPixelFormat::Rgb10A2Unorm,
                                       open_st::CapturedColorSpace::Unknown, {}, MakeRgb10Pixel(0U));
    open_st::OutputPreviewFrame preview;
    std::wstring errorMessage;
    EXPECT_FALSE(open_st::BuildOutputPreview(plane, preview, errorMessage));
    EXPECT_FALSE(errorMessage.empty());
    preview.pixels = {1U};
    EXPECT_FALSE(open_st::BuildOutputPreview({}, preview, errorMessage));
    EXPECT_TRUE(preview.pixels.empty());
    EXPECT_EQ(preview.stride, 0U);
}
