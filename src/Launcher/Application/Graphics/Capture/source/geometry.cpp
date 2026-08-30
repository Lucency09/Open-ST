#include <geometry.h>

#include <algorithm>

namespace open_st
{
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
