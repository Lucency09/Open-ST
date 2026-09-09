// 文件职责：按冻结帧与选区边界计算互不重叠的外部变暗矩形，处理空选区和裁切边界。

#include "outside_mask_layout.h"

#include <algorithm>

namespace
{
// 计算选区与帧边界重叠部分以生成外部遮罩。
// 入参：first、second：虚拟桌面物理像素半开矩形。
// 返回：两矩形的交集；无交集时返回空矩形。
open_st::RectI IntersectRectangles(open_st::RectI first, open_st::RectI second) noexcept
{
    return {std::max(first.left, second.left), std::max(first.top, second.top),
            std::min(first.right, second.right), std::min(first.bottom, second.bottom)};
}

// 将有效遮罩矩形追加到固定容量布局中，跳过零面积条带。
// 入参：layout：输入输出参数，保存遮罩矩形及数量；rectangle：待追加的物理像素半开矩形。
// 返回：无返回值；矩形非空时写入布局并递增数量，空矩形不改变布局。
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
// 将冻结帧中选区外区域拆成互不重叠的变暗矩形。
// 入参：frameBounds：冻结帧物理像素边界；hasSelection：是否存在有效选区；selectionRectangle：选区的物理像素半开矩形。
// 返回：最多四个外部遮罩矩形；无有效交集时覆盖全帧，选区覆盖全帧时布局为空。
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
