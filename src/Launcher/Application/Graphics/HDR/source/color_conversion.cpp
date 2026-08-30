#include <color_conversion.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>

namespace
{
// 把 DXGI G22/P709 非线性通道按 2.2 gamma 解码为 BT.709 线性强度。
float Gamma22P709ToLinear(float value) noexcept
{
    return std::pow(std::clamp(value, 0.0F, 1.0F), 2.2F);
}

// 把 ST.2084 PQ 码值解码为绝对亮度 nit。
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

// 把线性 BT.2020 绝对亮度转换为线性 BT.709/scRGB，并以 80 nit 归一。
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
// 按 IEEE 754 规则展开 binary16 的符号、指数和尾数位。
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

// 采用最近偶数舍入编码 binary16，原生 FP16 的直接复制不经过本函数。
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

// 按 IEC sRGB 分段传递函数解码界面颜色，保留黑色附近的线性段。
float SrgbToLinear(float value) noexcept
{
    const float clamped = std::clamp(value, 0.0F, 1.0F);
    return clamped <= 0.04045F ? clamped / 12.92F : std::pow((clamped + 0.055F) / 1.055F, 2.4F);
}

// 按 RGB10A2 的 R/G/B 位域解码；HDR10 先执行 PQ 和 BT.2020→BT.709，其他格式按 P709 解释。
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

// 把 0–1023 的 10 位 UNORM 值四舍五入到 0–255。
std::uint8_t Unorm10ToByte(std::uint32_t value) noexcept
{
    constexpr std::uint32_t UNORM10_MAXIMUM = 1023U;
    constexpr std::uint32_t BYTE_MAXIMUM = 255U;
    const std::uint32_t clamped = std::min(value, UNORM10_MAXIMUM);
    return static_cast<std::uint8_t>((clamped * BYTE_MAXIMUM + UNORM10_MAXIMUM / 2U) / UNORM10_MAXIMUM);
}
} // namespace open_st
