// 文件职责：声明 SDR、scRGB、HDR10 与浮点编码之间的纯颜色转换函数。

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

// 将 binary16 原始位模式解码为单精度浮点，保留 HDR 值域。
// 入参：value：IEEE 754 binary16 的 16 位编码。
// 返回：对应 binary32 数值，包括符号、次正规数、无穷和 NaN。
[[nodiscard]] float DecodeFloat16(std::uint16_t value) noexcept;
// 将单精度浮点编码为半精度像素通道，供 FP16 图像存储。
// 入参：value：待编码的 binary32 数值。
// 返回：舍入后的 binary16 位模式，保留符号和特殊值，不将颜色限制在 0 至 1。
[[nodiscard]] std::uint16_t EncodeFloat16(float value) noexcept;
// 将 sRGB 编码颜色解码为线性通道，作为 SDR UI 的 HDR 合成基础。
// 入参：value：归一化 sRGB 编码通道。
// 返回：按 sRGB 分段传递函数计算的线性值，不附加参考白缩放。
[[nodiscard]] float SrgbToLinear(float value) noexcept;
// 解包 RGB10A2 像素并按指定颜色空间转换到统一线性 scRGB。
// 入参：pixel：打包的 RGB10A2 原始像素；colorSpace：RGB 通道所采用的 SDR、scRGB 或 HDR10 语义。
// 返回：线性 scRGB RGB 三通道，忽略 alpha；HDR10 经 PQ 和色域转换后以 80 nit 为 1.0。
[[nodiscard]] LinearScRgb DecodeRgb10A2ToScRgb(std::uint32_t pixel, Rgb10ColorSpace colorSpace) noexcept;
// 将 10 位 UNORM 通道量化为兼容的 8 位 SDR 通道。
// 入参：value：10 位 UNORM 通道整数。
// 返回：范围 0 至 255 的四舍五入量化值，输入超范围时先限制到 1023。
[[nodiscard]] std::uint8_t Unorm10ToByte(std::uint32_t value) noexcept;
} // namespace open_st
