// 声明工具栏按钮校验和布局计算，隔离窗口呈现与纯几何规则。

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

// 根据可见按钮和选区计算工具栏窗口、按钮及分隔线布局。
// 入参：specs：有序按钮描述；states：一一对应的状态；selection、workArea：虚拟桌面物理像素矩形；dpi：目标屏 DPI；layout：输出布局。
// 返回：成功时 success 为 true 并发布 layout；非法输入或工作区不足时 false 并附诊断，保留原布局。
ToolbarResult BuildLayout(std::span<const ToolbarButtonSpec> specs, std::span<const ToolbarButtonState> states,
                          RECT selection, RECT workArea, UINT dpi, ToolbarLayout& layout);
// 验证工具栏按钮描述能否用于创建窗口。
// 入参：specs：有序按钮描述，要求命令 ID 唯一且图标和文本键有效。
// 返回：全部合法时 success 为 true；非法或重复项目时 false 并附诊断。
ToolbarResult ValidateButtons(std::span<const ToolbarButtonSpec> specs);
} // namespace open_st::toolbar_detail
