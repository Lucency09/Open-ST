#pragma once

#include <cstdint>

namespace open_st
{
// RGB10 解码自有色彩空间，包含预览所需的 SDR 兼容格式，不依赖捕获模块枚举。
enum class Rgb10ColorSpace
{
    SdrGamma22P709,
    ScRgb,
    Hdr10,
};

// 保存线性 scRGB 的三个颜色通道；1.0 表示 80 nit 参考白。
struct LinearScRgb final
{
    float red{};
    float green{};
    float blue{};
};

// 把 IEEE 754 binary16 位模式解码为 binary32，完整处理次正规数和非有限值。
[[nodiscard]] float DecodeFloat16(std::uint16_t value) noexcept;
// 把 binary32 舍入为 binary16，保留符号、次正规数和非有限值，不钳制 HDR 颜色范围。
[[nodiscard]] std::uint16_t EncodeFloat16(float value) noexcept;
// 把归一化 sRGB 界面或 SDR 兼容颜色解码为线性通道，不执行亮度缩放。
[[nodiscard]] float SrgbToLinear(float value) noexcept;
// 把 RGB10A2 像素按自有颜色空间解码为线性 scRGB；alpha 不参与不透明桌面输出。
[[nodiscard]] LinearScRgb DecodeRgb10A2ToScRgb(std::uint32_t pixel, Rgb10ColorSpace colorSpace) noexcept;
// 把 10 位 UNORM 通道直接量化为 8 位 SDR 兼容通道。
[[nodiscard]] std::uint8_t Unorm10ToByte(std::uint32_t value) noexcept;
} // namespace open_st
