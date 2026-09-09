// 文件职责：验证颜色传递函数、HDR10 色域转换与浮点编解码的数值和边界行为。

#include <gtest/gtest.h>

#include <color_conversion.h>

#include <cmath>
#include <cstdint>
#include <limits>

// 验证 binary16 的零、次正规数、普通数和非有限位模式被正确展开。
// 入参：无运行时形参；宏参数 HdrToneMapperTest 为测试套件，decodes_binary16_special_and_finite_values 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
TEST(HdrToneMapperTest, decodes_binary16_special_and_finite_values)
{
    EXPECT_FLOAT_EQ(open_st::DecodeFloat16(0x0000U), 0.0F);
    EXPECT_TRUE(std::signbit(open_st::DecodeFloat16(0x8000U)));
    EXPECT_FLOAT_EQ(open_st::DecodeFloat16(0x3800U), 0.5F);
    EXPECT_FLOAT_EQ(open_st::DecodeFloat16(0x3C00U), 1.0F);
    EXPECT_FLOAT_EQ(open_st::DecodeFloat16(0x4000U), 2.0F);
    EXPECT_GT(open_st::DecodeFloat16(0x0001U), 0.0F);
    EXPECT_TRUE(std::isinf(open_st::DecodeFloat16(0x7C00U)));
    EXPECT_TRUE(std::isnan(open_st::DecodeFloat16(0x7E00U)));
}

// 验证全部有限 binary16 值经 binary32 编码往返不损失符号、次正规数、负值和高光。
// 入参：无运行时形参；宏参数 HdrToneMapperTest 为测试套件，round_trips_all_finite_half_values_without_clipping 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
TEST(HdrToneMapperTest, round_trips_all_finite_half_values_without_clipping)
{
    for (std::uint32_t bits = 0U; bits <= 0xFFFFU; ++bits)
    {
        if ((bits & 0x7C00U) == 0x7C00U)
        {
            continue;
        }
        const std::uint16_t half = static_cast<std::uint16_t>(bits);
        EXPECT_EQ(open_st::EncodeFloat16(open_st::DecodeFloat16(half)), half);
    }
}

// 验证编码采用最近偶数舍入，并明确处理 binary16 的上溢和非有限值。
// 入参：无运行时形参；宏参数 HdrToneMapperTest 为测试套件，encodes_half_rounding_and_special_values 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
TEST(HdrToneMapperTest, encodes_half_rounding_and_special_values)
{
    EXPECT_EQ(open_st::EncodeFloat16(1.00048828125F), 0x3C00U);
    EXPECT_EQ(open_st::EncodeFloat16(1.00146484375F), 0x3C02U);
    EXPECT_EQ(open_st::EncodeFloat16(65504.0F), 0x7BFFU);
    EXPECT_EQ(open_st::EncodeFloat16(65536.0F), 0x7C00U);
    EXPECT_EQ(open_st::EncodeFloat16(-std::numeric_limits<float>::infinity()), 0xFC00U);
    EXPECT_TRUE(std::isnan(open_st::DecodeFloat16(open_st::EncodeFloat16(std::numeric_limits<float>::quiet_NaN()))));
}

// 验证 sRGB 界面颜色采用分段解码而不是 gamma2.2 近似，函数自身不乘 SDR 白比例。
// 入参：无运行时形参；宏参数 HdrToneMapperTest 为测试套件，decodes_srgb_ui_color_without_white_scaling 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
TEST(HdrToneMapperTest, decodes_srgb_ui_color_without_white_scaling)
{
    EXPECT_FLOAT_EQ(open_st::SrgbToLinear(0.0F), 0.0F);
    EXPECT_FLOAT_EQ(open_st::SrgbToLinear(1.0F), 1.0F);
    EXPECT_NEAR(open_st::SrgbToLinear(0.5F), 0.214041F, 0.000001F);
    EXPECT_NEAR(open_st::SrgbToLinear(0.04F), 0.04F / 12.92F, 0.000001F);
}

// 验证 RGB10A2 的通道位域顺序和 10 位到 8 位量化边界。
// 入参：无运行时形参；宏参数 HdrToneMapperTest 为测试套件，decodes_rgb10_channel_order 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
TEST(HdrToneMapperTest, decodes_rgb10_channel_order)
{
    constexpr std::uint32_t redMaximum = 0x000003FFU;
    constexpr std::uint32_t greenMaximum = 0x000FFC00U;
    constexpr std::uint32_t blueMaximum = 0x3FF00000U;
    const open_st::LinearScRgb red =
        open_st::DecodeRgb10A2ToScRgb(redMaximum, open_st::Rgb10ColorSpace::SdrGamma22P709);
    const open_st::LinearScRgb green =
        open_st::DecodeRgb10A2ToScRgb(greenMaximum, open_st::Rgb10ColorSpace::SdrGamma22P709);
    const open_st::LinearScRgb blue =
        open_st::DecodeRgb10A2ToScRgb(blueMaximum, open_st::Rgb10ColorSpace::SdrGamma22P709);
    EXPECT_FLOAT_EQ(red.red, 1.0F);
    EXPECT_FLOAT_EQ(red.green, 0.0F);
    EXPECT_FLOAT_EQ(green.green, 1.0F);
    EXPECT_FLOAT_EQ(blue.blue, 1.0F);
    EXPECT_EQ(open_st::Unorm10ToByte(0U), 0U);
    EXPECT_EQ(open_st::Unorm10ToByte(1023U), 255U);
}

// 验证 HDR10 的 PQ 峰值转换为高于 SDR 参考白的中性 scRGB，不执行色调映射或提前截断。
// 入参：无运行时形参；宏参数 HdrToneMapperTest 为测试套件，decodes_hdr10_peak_white_to_extended_sc_rgb 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
TEST(HdrToneMapperTest, decodes_hdr10_peak_white_to_extended_sc_rgb)
{
    constexpr std::uint32_t rgbMaximum = 0x3FFFFFFFU;
    const open_st::LinearScRgb white = open_st::DecodeRgb10A2ToScRgb(rgbMaximum, open_st::Rgb10ColorSpace::Hdr10);
    EXPECT_GT(white.red, 100.0F);
    EXPECT_GT(white.green, 100.0F);
    EXPECT_GT(white.blue, 100.0F);
    EXPECT_NEAR(white.red, white.green, 0.01F);
    EXPECT_NEAR(white.green, white.blue, 0.01F);
}
