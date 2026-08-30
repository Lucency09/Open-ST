#include "outside_mask_layout.h"

#include <algorithm>

namespace
{
// 计算两个半开整数矩形的交集；不相交时返回空矩形。
open_st::RectI IntersectRectangles(open_st::RectI first, open_st::RectI second) noexcept
{
    return {std::max(first.left, second.left), std::max(first.top, second.top),
            std::min(first.right, second.right), std::min(first.bottom, second.bottom)};
}

// 只把非空遮罩条带追加到固定容量布局，避免绘制无效矩形。
void AppendIfNotEmpty(open_st::OutsideMaskLayout& layout, open_st::RectI rectangle) noexcept
{
    if (!rectangle.IsEmpty() && layout.count < layout.rectangles.size())
    {
        layout.rectangles[layout.count] = rectangle;
        ++layout.count;
    }
}
} // namespace

namespace open_st
{
// 将帧减去有效选区交集，按上、下、左、右顺序生成互不重叠的遮罩条带。
OutsideMaskLayout BuildOutsideMaskLayout(RectI frameBounds, bool hasSelection,
                                         RectI selectionRectangle) noexcept
{
    OutsideMaskLayout layout{};
    if (frameBounds.IsEmpty())
    {
        return layout;
    }

    const RectI clippedSelection = IntersectRectangles(frameBounds, selectionRectangle);
    if (!hasSelection || clippedSelection.IsEmpty())
    {
        AppendIfNotEmpty(layout, frameBounds);
        return layout;
    }

    AppendIfNotEmpty(layout,
                     {frameBounds.left, frameBounds.top, frameBounds.right, clippedSelection.top});
    AppendIfNotEmpty(layout,
                     {frameBounds.left, clippedSelection.bottom, frameBounds.right, frameBounds.bottom});
    AppendIfNotEmpty(layout,
                     {frameBounds.left, clippedSelection.top, clippedSelection.left, clippedSelection.bottom});
    AppendIfNotEmpty(layout,
                     {clippedSelection.right, clippedSelection.top, frameBounds.right, clippedSelection.bottom});
    return layout;
}
} // namespace open_st
