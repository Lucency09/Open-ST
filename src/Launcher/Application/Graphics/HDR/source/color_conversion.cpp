// 文件职责：实现传递函数、色域矩阵及浮点编码转换，为预览和 HDR 色调映射提供数值基础。

#include <color_conversion.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>

namespace
{
// 解码 SDR Gamma 2.2 通道，供 RGB10 兼容颜色转换使用。
// 入参：value：归一化 Gamma 2.2 编码通道。
// 返回：先限制在 0 至 1 后按 2.2 次幂转换的线性通道。
float Gamma22P709ToLinear(float value) noexcept
{
    return std::pow(std::clamp(value, 0.0F, 1.0F), 2.2F);
}

// 按 PQ 传递函数把 HDR10 编码通道还原为绝对亮度。
// 入参：value：归一化 PQ 编码通道。
// 返回：对应的绝对亮度，单位 nit。
float PqToNits(float value) noexcept
{
    constexpr float M1 = 2610.0F / 16384.0F;
    constexpr float M2 = 2523.0F / 32.0F;
    constexpr float C1 = 3424.0F / 4096.0F;
    constexpr float C2 = 2413.0F / 128.0F;
    constexpr float C3 = 2392.0F / 128.0F;
    const float powered = std::pow(std::clamp(value, 0.0F, 1.0F), 1.0F / M2);
    const float numerator = std::max(powered - C1, 0.0F);
    const float denominator = std::max(C2 - C3 * powered, 1.0e-6F);
    return 10000.0F * std::pow(numerator / denominator, 1.0F / M1);
}

// 把 BT.2020 绝对亮度 RGB 转换为线性 scRGB，统一 HDR10 预览和映射输入。
// 入参：red、green、blue：BT.2020 各通道的绝对亮度，单位 nit。
// 返回：转换后的线性 scRGB RGB；1.0 对应 80 nit，保留负色域分量和超白值。
open_st::LinearScRgb Bt2020NitsToScRgb(float red, float green, float blue) noexcept
{
    constexpr float SDR_WHITE_NITS = 80.0F;
    return {
        (1.660491F * red - 0.587641F * green - 0.072850F * blue) / SDR_WHITE_NITS,
        (-0.124550F * red + 1.132900F * green - 0.008350F * blue) / SDR_WHITE_NITS,
        (-0.018151F * red - 0.100579F * green + 1.118730F * blue) / SDR_WHITE_NITS,
    };
}

} // namespace

namespace open_st
{
// 将 binary16 原始位模式解码为单精度浮点，保留 HDR 值域。
// 入参：value：IEEE 754 binary16 的 16 位编码。
// 返回：对应 binary32 数值，包括符号、次正规数、无穷和 NaN。
float DecodeFloat16(std::uint16_t value) noexcept
{
    const std::uint32_t sign = static_cast<std::uint32_t>(value & 0x8000U) << 16U;
    int exponent = static_cast<int>((value >> 10U) & 0x1FU);
    std::uint32_t mantissa = value & 0x03FFU;
    std::uint32_t bits{};

    if (exponent == 0)
    {
        if (mantissa == 0U)
        {
            bits = sign;
        }
        else
        {
            exponent = 1;
            while ((mantissa & 0x0400U) == 0U)
            {
                mantissa <<= 1U;
                --exponent;
            }
            mantissa &= 0x03FFU;
            bits = sign | (static_cast<std::uint32_t>(exponent + 112) << 23U) | (mantissa << 13U);
        }
    }
    else if (exponent == 31)
    {
        bits = sign | 0x7F800000U | (mantissa << 13U);
    }
    else
    {
        bits = sign | (static_cast<std::uint32_t>(exponent + 112) << 23U) | (mantissa << 13U);
    }
    return std::bit_cast<float>(bits);
}

// 将单精度浮点编码为半精度像素通道，供 FP16 图像存储。
// 入参：value：待编码的 binary32 数值。
// 返回：舍入后的 binary16 位模式，保留符号和特殊值，不将颜色限制在 0 至 1。
std::uint16_t EncodeFloat16(float value) noexcept
{
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
    const std::uint32_t sign = (bits >> 16U) & 0x8000U;
    const std::uint32_t rawExponent = (bits >> 23U) & 0xFFU;
    std::uint32_t mantissa = bits & 0x007FFFFFU;
    if (rawExponent == 255U)
    {
        return static_cast<std::uint16_t>(sign | (mantissa == 0U ? 0x7C00U : 0x7E00U));
    }
    const int exponent = static_cast<int>(rawExponent) - 112;
    if (exponent >= 31)
    {
        return static_cast<std::uint16_t>(sign | 0x7C00U);
    }
    if (exponent <= 0)
    {
        if (exponent < -10)
        {
            return static_cast<std::uint16_t>(sign);
        }
        mantissa |= 0x00800000U;
        const std::uint32_t shift = static_cast<std::uint32_t>(14 - exponent);
        const std::uint32_t rounded = (mantissa + ((1U << (shift - 1U)) - 1U) + ((mantissa >> shift) & 1U)) >> shift;
        return static_cast<std::uint16_t>(sign | rounded);
    }
    const std::uint32_t roundedMantissa = (mantissa + 0xFFFU + ((mantissa >> 13U) & 1U)) >> 13U;
    return static_cast<std::uint16_t>(sign | ((static_cast<std::uint32_t>(exponent) << 10U) + roundedMantissa));
}

// 将 sRGB 编码颜色解码为线性通道，作为 SDR UI 的 HDR 合成基础。
// 入参：value：归一化 sRGB 编码通道。
// 返回：按 sRGB 分段传递函数计算的线性值，不附加参考白缩放。
float SrgbToLinear(float value) noexcept
{
    const float clamped = std::clamp(value, 0.0F, 1.0F);
    return clamped <= 0.04045F ? clamped / 12.92F : std::pow((clamped + 0.055F) / 1.055F, 2.4F);
}

// 解包 RGB10A2 像素并按指定颜色空间转换到统一线性 scRGB。
// 入参：pixel：打包的 RGB10A2 原始像素；colorSpace：RGB 通道所采用的 SDR、scRGB 或 HDR10 语义。
// 返回：线性 scRGB RGB 三通道，忽略 alpha；HDR10 经 PQ 和色域转换后以 80 nit 为 1.0。
LinearScRgb DecodeRgb10A2ToScRgb(std::uint32_t pixel, Rgb10ColorSpace colorSpace) noexcept
{
    constexpr float UNORM10_MAXIMUM = 1023.0F;
    const float red = static_cast<float>(pixel & 0x03FFU) / UNORM10_MAXIMUM;
    const float green = static_cast<float>((pixel >> 10U) & 0x03FFU) / UNORM10_MAXIMUM;
    const float blue = static_cast<float>((pixel >> 20U) & 0x03FFU) / UNORM10_MAXIMUM;
    if (colorSpace == Rgb10ColorSpace::Hdr10)
    {
        return Bt2020NitsToScRgb(PqToNits(red), PqToNits(green), PqToNits(blue));
    }
    if (colorSpace == Rgb10ColorSpace::ScRgb)
    {
        return {red, green, blue};
    }
    return {Gamma22P709ToLinear(red), Gamma22P709ToLinear(green), Gamma22P709ToLinear(blue)};
}

// 将 10 位 UNORM 通道量化为兼容的 8 位 SDR 通道。
// 入参：value：10 位 UNORM 通道整数。
// 返回：范围 0 至 255 的四舍五入量化值，输入超范围时先限制到 1023。
std::uint8_t Unorm10ToByte(std::uint32_t value) noexcept
{
    constexpr std::uint32_t UNORM10_MAXIMUM = 1023U;
    constexpr std::uint32_t BYTE_MAXIMUM = 255U;
    const std::uint32_t clamped = std::min(value, UNORM10_MAXIMUM);
    return static_cast<std::uint8_t>((clamped * BYTE_MAXIMUM + UNORM10_MAXIMUM / 2U) / UNORM10_MAXIMUM);
}
} // namespace open_st
