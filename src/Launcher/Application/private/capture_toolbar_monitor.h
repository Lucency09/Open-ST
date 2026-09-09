// 声明工具栏选屏算法，输入采用虚拟桌面物理像素坐标。

#pragma once

#include <cstddef>
#include <span>
#include <windows.h>

namespace open_st
{
// 为当前截图选区选择工具栏应显示的显示器。
// 入参：selection：选区物理像素矩形；endpoint：操作终点物理像素坐标；monitors：候选显示器矩形列表。
// 返回：优先返回包含终点且与选区相交的屏幕索引，否则取最大交集；无交集返回 monitors.size()。
[[nodiscard]] std::size_t SelectToolbarMonitor(RECT selection, POINT endpoint, std::span<const RECT> monitors) noexcept;
} // namespace open_st
