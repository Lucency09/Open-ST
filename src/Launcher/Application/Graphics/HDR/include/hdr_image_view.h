// 文件职责：定义 HDR 模块自有的借用图像视图和输入格式，明确行跨度及 scRGB 亮度单位。

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace open_st
{
// HDR 自有输入格式，避免颜色处理模块依赖 Capture 的帧模型。
enum class HdrPixelFormat
{
    Rgba16FloatScRgb,
    Rgb10A2ScRgb,
    Rgb10A2Hdr10,
};

// 同步借用图像；stride 以字节计，scRGB 的 1.0 为 80 nit，转换器不保留 pixels。
struct HdrImageView final
{
    std::uint32_t width{};
    std::uint32_t height{};
    std::size_t stride{};
    std::span<const std::uint8_t> pixels;
    HdrPixelFormat format{HdrPixelFormat::Rgba16FloatScRgb};
};
} // namespace open_st
