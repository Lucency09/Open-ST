// 根据选区相交面积与操作终点选择工具栏显示屏，支持负坐标桌面。

#include "capture_toolbar_monitor.h"

#include <algorithm>
#include <cstdint>

namespace open_st
{
// 为当前截图选区选择工具栏应显示的显示器。
// 入参：selection：选区物理像素矩形；endpoint：操作终点物理像素坐标；monitors：候选显示器矩形列表。
// 返回：优先返回包含终点且与选区相交的屏幕索引，否则取最大交集；无交集返回 monitors.size()。
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
