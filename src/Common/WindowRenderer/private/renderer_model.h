// 定义通用表单的私有布局树及严格 JSON 布局解析入口。

#pragma once
#include "window_renderer.h"
namespace open_st::renderer_detail
{
enum class NodeType
{
    Column,
    Text,
    Select,
    Checkbox,
    Button
};
struct Node
{
    NodeType type = NodeType::Column;
    std::string id;
    std::string textKey;
    int padding = 0;
    int gap = 12;
    int width = -1;
    std::vector<Node> children;
};
struct Page
{
    std::string id;
    std::string titleKey;
    Node content;
};
struct Layout
{
    std::string titleKey;
    int width = 600;
    int height = 360;
    int minWidth = 480;
    int minHeight = 280;
    bool resizable = true;
    bool showTabs = true;
    std::vector<Page> pages;
    std::vector<Node> leading;
    std::vector<Node> trailing;
};
// 校验并解析 JSON 布局为可独立持有的窗口布局树。
// 入参：document：调用期间借用的完整布局 JSON；output：输出参数，成功时接收解析后的布局。
// 返回：错误码为空表示成功；失败提供错误码、JSON 路径及控件 ID，保留 output 原值。
RendererResult ParseLayout(const nlohmann::json& document, Layout& output);
} // namespace open_st::renderer_detail
