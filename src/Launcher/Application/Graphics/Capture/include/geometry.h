// 文件职责：定义虚拟桌面物理像素矩形和矩形并集接口，统一捕获几何的半开边界语义。

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

    // 查询矩形或图像的水平物理像素尺寸。
    // 入参：无。
    // 返回：right 减 left 的有符号宽度，空或反向边界可能为零或负数。
    [[nodiscard]] constexpr int Width() const noexcept
    {
        return this->right - this->left;
    }

    // 查询矩形或图像的垂直物理像素尺寸。
    // 入参：无。
    // 返回：bottom 减 top 的有符号高度，空或反向边界可能为零或负数。
    [[nodiscard]] constexpr int Height() const noexcept
    {
        return this->bottom - this->top;
    }

    // 判断半开矩形是否具有正面积。
    // 入参：无。
    // 返回：宽度或高度不大于零时为 true，否则为 false。
    [[nodiscard]] constexpr bool IsEmpty() const noexcept
    {
        return this->right <= this->left || this->bottom <= this->top;
    }
};

// 合并显示矩形，求出容纳所有有效矩形的最小虚拟桌面边界。
// 入参：rectangles：虚拟桌面物理像素半开矩形集合，允许负坐标和空矩形。
// 返回：非空矩形的最小外接矩形；输入为空或全部矩形为空时返回零矩形。
[[nodiscard]] RectI UnionRectangles(std::span<const RectI> rectangles) noexcept;
} // namespace open_st
