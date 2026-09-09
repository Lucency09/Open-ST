// 校验工具栏按钮描述并计算随 DPI 缩放的分组布局及工作区内位置。

#include "toolbar_layout.h"

#include <algorithm>
#include <limits>

namespace open_st::toolbar_detail
{
// 验证工具栏按钮描述能否用于创建窗口。
// 入参：specs：有序按钮描述，要求命令 ID 唯一且图标和文本键有效。
// 返回：全部合法时 success 为 true；非法或重复项目时 false 并附诊断。
ToolbarResult ValidateButtons(std::span<const ToolbarButtonSpec> specs)
{
    if (specs.empty())
    {
        return {false, L"工具栏按钮列表为空。"};
    }
    for (std::size_t index = 0; index < specs.size(); ++index)
    {
        const ToolbarButtonSpec& spec = specs[index];
        if ((spec.command != CaptureToolbarCommand::Cancel && spec.command != CaptureToolbarCommand::Save &&
             spec.command != CaptureToolbarCommand::Copy) ||
            (spec.icon != ToolbarIcon::Cancel && spec.icon != ToolbarIcon::Save && spec.icon != ToolbarIcon::Copy) ||
            spec.tooltipKey.empty())
        {
            return {false, L"工具栏包含无效命令、图标或文本键。"};
        }
        for (std::size_t earlier = 0; earlier < index; ++earlier)
        {
            if (specs[earlier].command == spec.command)
            {
                return {false, L"工具栏命令 ID 重复。"};
            }
        }
    }
    return {true, {}};
}

// 根据可见按钮和选区计算工具栏窗口、按钮及分隔线布局。
// 入参：specs：有序按钮描述；states：一一对应的状态；selection、workArea：虚拟桌面物理像素矩形；dpi：目标屏 DPI；layout：输出布局。
// 返回：成功时 success 为 true 并发布 layout；非法输入或工作区不足时 false 并附诊断，保留原布局。
ToolbarResult BuildLayout(std::span<const ToolbarButtonSpec> specs, std::span<const ToolbarButtonState> states,
                          RECT selection, RECT workArea, UINT dpi, ToolbarLayout& layout)
{
    if (states.size() != specs.size() || selection.left >= selection.right || selection.top >= selection.bottom ||
        workArea.left >= workArea.right || workArea.top >= workArea.bottom || dpi < 48 || dpi > 768)
    {
        return {false, L"工具栏位置或 DPI 无效。"};
    }
    ToolbarLayout next;
    next.buttons.resize(specs.size());
    const int padding = MulDiv(3, static_cast<int>(dpi), 96);
    const int buttonWidth = MulDiv(30, static_cast<int>(dpi), 96);
    const int height = MulDiv(34, static_cast<int>(dpi), 96);
    const int divider = MulDiv(7, static_cast<int>(dpi), 96);
    int width = padding;
    bool previous = false;
    std::uint32_t previousGroup = 0;
    for (std::size_t index = 0; index < specs.size(); ++index)
    {
        if (!states[index].visible)
        {
            continue;
        }
        if (previous && previousGroup != specs[index].group)
        {
            next.separators.push_back(width + divider / 2);
            width += divider;
        }
        next.buttons[index] = {width, padding, width + buttonWidth, height - padding};
        width += buttonWidth;
        previousGroup = specs[index].group;
        previous = true;
    }
    width += padding;
    const std::int64_t workWidth = static_cast<std::int64_t>(workArea.right) - workArea.left;
    const std::int64_t workHeight = static_cast<std::int64_t>(workArea.bottom) - workArea.top;
    if (width > workWidth || height > workHeight)
    {
        return {false, L"工作区不足以容纳工具栏。"};
    }
    // 控制点命中半径约 6 像素，额外留出 6 DIP 间隔。
    const int gap = 6 + MulDiv(6, static_cast<int>(dpi), 96);
    const std::int64_t x = std::clamp<std::int64_t>(static_cast<std::int64_t>(selection.right) - width, workArea.left,
                                                    static_cast<std::int64_t>(workArea.right) - width);
    std::int64_t y = static_cast<std::int64_t>(selection.bottom) + gap;
    if (y + height > workArea.bottom)
    {
        y = static_cast<std::int64_t>(selection.top) - gap - height;
    }
    y = std::clamp<std::int64_t>(y, workArea.top, static_cast<std::int64_t>(workArea.bottom) - height);
    next.bounds = {static_cast<LONG>(x), static_cast<LONG>(y), static_cast<LONG>(x + width),
                   static_cast<LONG>(y + height)};
    layout = std::move(next);
    return {true, {}};
}
} // namespace open_st::toolbar_detail
