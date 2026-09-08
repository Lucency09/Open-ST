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
// 解析并复制完整布局；失败保留输出，错误携带 JSON 路径和控件 ID。
RendererResult ParseLayout(const nlohmann::json& document, Layout& output);
} // namespace open_st::renderer_detail
