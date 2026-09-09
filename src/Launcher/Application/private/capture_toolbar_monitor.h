#pragma once

#include <cstddef>
#include <span>
#include <windows.h>

namespace open_st
{
// 优先选区操作终点所在且相交的屏幕，否则选相交面积最大的屏幕；无交集返回 monitors.size()。
[[nodiscard]] std::size_t SelectToolbarMonitor(RECT selection, POINT endpoint, std::span<const RECT> monitors) noexcept;
} // namespace open_st
