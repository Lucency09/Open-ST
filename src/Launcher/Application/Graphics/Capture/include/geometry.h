#pragma once

#include <span>

namespace open_st
{
// 使用物理像素表示的半开矩形：[left, right) × [top, bottom)。
// 采用半开区间可以让 Width/Height 直接相减，并避免相邻屏幕边界重复一个像素。
// 原点是 Windows 虚拟桌面左上角，可能为负数；坐标单位是物理像素，而非 DPI 缩放后的逻辑像素。
struct RectI final
{
    int left{};   // 左边界，包含
    int top{};    // 上边界，包含
    int right{};  // 右边界，不包含
    int bottom{}; // 下边界，不包含

    // 返回半开矩形的水平物理像素数。
    [[nodiscard]] constexpr int Width() const noexcept
    {
        return this->right - this->left;
    }

    // 返回半开矩形的垂直物理像素数。
    [[nodiscard]] constexpr int Height() const noexcept
    {
        return this->bottom - this->top;
    }

    // 判断矩形是否为空：宽度或高度为零或负数时认为空。
    [[nodiscard]] constexpr bool IsEmpty() const noexcept
    {
        return this->right <= this->left || this->bottom <= this->top;
    }
};

// 计算一组矩形的最小外接矩形。空输入或全部为空时返回零矩形。
[[nodiscard]] RectI UnionRectangles(std::span<const RectI> rectangles) noexcept;
} // namespace open_st
