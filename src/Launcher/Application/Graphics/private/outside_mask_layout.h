// 文件职责：声明选区外变暗区域的纯几何布局，保证选区内部不被遮罩覆盖。

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

// 将冻结帧中选区外区域拆成互不重叠的变暗矩形。
// 入参：frameBounds：冻结帧物理像素边界；hasSelection：是否存在有效选区；selectionRectangle：选区的物理像素半开矩形。
// 返回：最多四个外部遮罩矩形；无有效交集时覆盖全帧，选区覆盖全帧时布局为空。
[[nodiscard]] OutsideMaskLayout BuildOutsideMaskLayout(RectI frameBounds, bool hasSelection,
                                                       RectI selectionRectangle) noexcept;
} // namespace open_st
