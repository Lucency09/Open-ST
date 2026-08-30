#pragma once

#include <geometry.h>

#include <array>
#include <cstddef>

namespace open_st
{
// 选区外遮罩最多由上、下、左、右四个互不重叠的虚拟桌面矩形组成。
struct OutsideMaskLayout final
{
    std::array<RectI, 4> rectangles{};
    std::size_t count{};
};

// 在虚拟桌面物理坐标中计算选区外遮罩；无有效选区时返回完整帧矩形。
[[nodiscard]] OutsideMaskLayout BuildOutsideMaskLayout(RectI frameBounds, bool hasSelection,
                                                       RectI selectionRectangle) noexcept;
} // namespace open_st
