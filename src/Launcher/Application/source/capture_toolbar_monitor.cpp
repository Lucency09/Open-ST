#include "capture_toolbar_monitor.h"

#include <algorithm>
#include <cstdint>

namespace open_st
{
// 所有坐标均为物理像素；使用宽整数计算相交面积以支持跨屏和负坐标。
std::size_t SelectToolbarMonitor(RECT selection, POINT endpoint, std::span<const RECT> monitors) noexcept
{
    std::size_t best = monitors.size();
    std::int64_t bestArea = 0;
    for (std::size_t index = 0; index < monitors.size(); ++index)
    {
        const RECT& monitor = monitors[index];
        const std::int64_t width = static_cast<std::int64_t>(std::min(selection.right, monitor.right)) -
                                   std::max(selection.left, monitor.left);
        const std::int64_t height = static_cast<std::int64_t>(std::min(selection.bottom, monitor.bottom)) -
                                    std::max(selection.top, monitor.top);
        if (width <= 0 || height <= 0)
        {
            continue;
        }
        if (endpoint.x >= monitor.left && endpoint.x < monitor.right && endpoint.y >= monitor.top &&
            endpoint.y < monitor.bottom)
        {
            return index;
        }
        const std::int64_t area = width * height;
        if (area > bestArea)
        {
            bestArea = area;
            best = index;
        }
    }
    return best;
}
} // namespace open_st
