#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace open_st
{
// 同步借用顶向下的 SDR/sRGB BGRX 像素；第四字节不表示透明度。
struct SdrImageView
{
    std::uint32_t width{};
    std::uint32_t height{};
    std::size_t stride{};
    std::span<const std::uint8_t> pixels{};
};
} // namespace open_st
