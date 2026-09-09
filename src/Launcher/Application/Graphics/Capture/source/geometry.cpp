// 文件职责：实现虚拟桌面物理像素矩形并集，忽略空矩形并保留负坐标。

#include <geometry.h>

#include <algorithm>

namespace open_st
{
// 合并显示矩形，求出容纳所有有效矩形的最小虚拟桌面边界。
// 入参：rectangles：虚拟桌面物理像素半开矩形集合，允许负坐标和空矩形。
// 返回：非空矩形的最小外接矩形；输入为空或全部矩形为空时返回零矩形。
RectI UnionRectangles(std::span<const RectI> rectangles) noexcept
{
    RectI result{};
    bool hasRectangle = false;

    for (const RectI rectangle : rectangles)
    {
        if (rectangle.IsEmpty())
        {
            continue;
        }

        if (!hasRectangle)
        {
            result = rectangle;
            hasRectangle = true;
            continue;
        }

        result.left = std::min(result.left, rectangle.left);
        result.top = std::min(result.top, rectangle.top);
        result.right = std::max(result.right, rectangle.right);
        result.bottom = std::max(result.bottom, rectangle.bottom);
    }

    return result;
}
} // namespace open_st
