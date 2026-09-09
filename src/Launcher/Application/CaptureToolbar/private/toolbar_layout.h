#pragma once
#include <capture_toolbar.h>

namespace open_st::toolbar_detail
{
struct ToolbarLayout
{
    RECT bounds{};
    std::vector<RECT> buttons;
    std::vector<int> separators;
};

// 返回物理像素几何；隐藏项保留空矩形，不产生分隔线。
ToolbarResult BuildLayout(std::span<const ToolbarButtonSpec> specs, std::span<const ToolbarButtonState> states,
                          RECT selection, RECT workArea, UINT dpi, ToolbarLayout& layout);
// 校验已实现命令、图标及唯一 ID，不创建任何窗口。
ToolbarResult ValidateButtons(std::span<const ToolbarButtonSpec> specs);
} // namespace open_st::toolbar_detail
