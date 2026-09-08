#include "renderer_model.h"
#include <algorithm>
#include <initializer_list>
#include <set>

namespace open_st::renderer_detail
{
namespace
{
class Parser
{
  public:
    // 解析顶层协议，尺寸单位均为 DIP。
    Layout Parse(const nlohmann::json& document)
    {
        this->Keys(document, {"schemaVersion", "window", "pages", "content", "footer"}, "");
        if (!document.contains("schemaVersion") || !document["schemaVersion"].is_number_integer() ||
            document["schemaVersion"] != 1)
        {
            this->Fail("unsupported_version", "/schemaVersion");
        }
        Layout layout;
        const nlohmann::json& window = this->Required(document, "window", "");
        this->Keys(window, {"titleKey", "initialSize", "minSize", "resizable"}, "/window");
        layout.titleKey = this->String(window, "titleKey", "/window");
        this->Size(window, "initialSize", layout.width, layout.height);
        this->Size(window, "minSize", layout.minWidth, layout.minHeight);
        if (window.contains("resizable"))
        {
            if (!window["resizable"].is_boolean())
                this->Fail("invalid_type", "/window/resizable");
            layout.resizable = window["resizable"].get<bool>();
        }
        if (layout.minWidth > layout.width || layout.minHeight > layout.height)
            this->Fail("invalid_size", "/window/minSize");
        if (document.contains("pages") == document.contains("content"))
            this->Fail("invalid_content_mode", "/content");
        layout.showTabs = document.contains("pages");
        if (!layout.showTabs)
        {
            Page page;
            page.content = this->ParseNode(document["content"], "/content", 1);
            if (page.content.type != NodeType::Column)
                this->Fail("invalid_page_content", "/content", page.content.id);
            layout.pages.push_back(std::move(page));
        }
        else
        {
            const nlohmann::json& pages = this->Required(document, "pages", "");
            if (!pages.is_array() || pages.empty())
                this->Fail("invalid_pages", "/pages");
            for (std::size_t index = 0; index < pages.size(); ++index)
            {
                const std::string path = "/pages/" + std::to_string(index);
                const nlohmann::json& source = pages[index];
                this->Keys(source, {"id", "titleKey", "content"}, path);
                Page page;
                page.id = this->Id(source, path);
                page.titleKey = this->String(source, "titleKey", path);
                page.content = this->ParseNode(this->Required(source, "content", path), path + "/content", 1);
                if (page.content.type != NodeType::Column)
                    this->Fail("invalid_page_content", path + "/content", page.id);
                layout.pages.push_back(std::move(page));
            }
        }
        const nlohmann::json& footer = this->Required(document, "footer", "");
        this->Keys(footer, {"leading", "trailing"}, "/footer");
        this->Footer(footer, "leading", layout.leading);
        this->Footer(footer, "trailing", layout.trailing);
        return layout;
    }

  private:
    std::set<std::string> ids_;
    std::size_t count_ = 0;

    // 使用结构化错误中止当前候选树，不输出用户数据。
    [[noreturn]] void Fail(std::string code, std::string path, std::string id = {}) const
    {
        throw RendererResult{std::move(code), std::move(path), std::move(id)};
    }
    // 拒绝未知属性，避免拼写错误被静默忽略。
    void Keys(const nlohmann::json& object, std::initializer_list<std::string_view> allowed,
              const std::string& path) const
    {
        if (!object.is_object())
            this->Fail("invalid_type", path);
        for (auto iterator = object.begin(); iterator != object.end(); ++iterator)
        {
            if (std::find(allowed.begin(), allowed.end(), iterator.key()) == allowed.end())
                this->Fail("unknown_property", path + "/" + iterator.key());
        }
    }
    // 获取必需字段并在缺失时返回定位错误。
    const nlohmann::json& Required(const nlohmann::json& object, const char* key, const std::string& path) const
    {
        if (!object.contains(key))
            this->Fail("missing_property", path + "/" + key);
        return object[key];
    }
    // 文本键和 ID 必须是非空且不含空字符的 UTF-8 字符串。
    std::string String(const nlohmann::json& object, const char* key, const std::string& path) const
    {
        const nlohmann::json& value = this->Required(object, key, path);
        if (!value.is_string())
            this->Fail("invalid_type", path + "/" + key);
        const std::string text = value.get<std::string>();
        if (text.empty() || text.size() > 4096 || text.find('\0') != std::string::npos ||
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr,
                                0) == 0)
            this->Fail("invalid_string", path + "/" + key);
        return text;
    }
    // 全局登记页面和控件 ID，并限制布局节点总数。
    std::string Id(const nlohmann::json& object, const std::string& path)
    {
        const std::string id = this->String(object, "id", path);
        if (++this->count_ > 2048)
            this->Fail("node_limit", path, id);
        if (!this->ids_.insert(id).second)
            this->Fail("duplicate_id", path + "/id", id);
        return id;
    }
    // 有界整数先检查范围再转换，避免溢出。
    int Integer(const nlohmann::json& value, const std::string& path, int minimum) const
    {
        if (!value.is_number_integer() || value < minimum || value > 32767)
            this->Fail("invalid_dimension", path);
        return value.get<int>();
    }
    // 读取可选的客户区宽高。
    void Size(const nlohmann::json& window, const char* key, int& width, int& height) const
    {
        if (!window.contains(key))
            return;
        const nlohmann::json& pair = window[key];
        const std::string path = std::string("/window/") + key;
        if (!pair.is_array() || pair.size() != 2)
            this->Fail("invalid_size", path);
        width = this->Integer(pair[0], path + "/0", 1);
        height = this->Integer(pair[1], path + "/1", 1);
    }
    // 解析已支持节点，递归深度在进入子树前检查。
    Node ParseNode(const nlohmann::json& source, const std::string& path, int depth)
    {
        if (depth > 32)
            this->Fail("depth_limit", path);
        if (!source.is_object())
            this->Fail("invalid_type", path);
        const std::string type = this->String(source, "type", path);
        Node node;
        node.id = this->Id(source, path);
        if (type == "column")
        {
            node.type = NodeType::Column;
            this->Keys(source, {"type", "id", "padding", "gap", "width", "children"}, path);
            if (source.contains("padding"))
                node.padding = this->Integer(source["padding"], path + "/padding", 0);
            if (source.contains("gap"))
                node.gap = this->Integer(source["gap"], path + "/gap", 0);
            const nlohmann::json& children = this->Required(source, "children", path);
            if (!children.is_array())
                this->Fail("invalid_type", path + "/children", node.id);
            for (std::size_t index = 0; index < children.size(); ++index)
                node.children.push_back(
                    this->ParseNode(children[index], path + "/children/" + std::to_string(index), depth + 1));
        }
        else if (type == "select")
        {
            node.type = NodeType::Select;
            this->Keys(source, {"type", "id", "labelKey", "width"}, path);
            node.textKey = this->String(source, "labelKey", path);
        }
        else if (type == "text" || type == "button")
        {
            node.type = type == "text" ? NodeType::Text : NodeType::Button;
            this->Keys(source, {"type", "id", "textKey", "width"}, path);
            node.textKey = this->String(source, "textKey", path);
        }
        else
            this->Fail("unknown_node", path + "/type", node.id);
        if (source.contains("width"))
        {
            const nlohmann::json& width = source["width"];
            if (width == "fill")
                node.width = -1;
            else if (width == "auto")
                node.width = 0;
            else
                node.width = this->Integer(width, path + "/width", 1);
        }
        return node;
    }
    // 底部区域只允许按钮，布局位置由 leading/trailing 决定。
    void Footer(const nlohmann::json& footer, const char* key, std::vector<Node>& output)
    {
        if (!footer.contains(key))
            return;
        const nlohmann::json& source = footer[key];
        const std::string path = std::string("/footer/") + key;
        if (!source.is_array())
            this->Fail("invalid_type", path);
        for (std::size_t index = 0; index < source.size(); ++index)
        {
            Node node = this->ParseNode(source[index], path + "/" + std::to_string(index), 1);
            if (node.type != NodeType::Button)
                this->Fail("invalid_footer_node", path, node.id);
            output.push_back(std::move(node));
        }
    }
};
} // namespace
// 仅在完整解析成功后替换输出树，不保留文档引用。
RendererResult ParseLayout(const nlohmann::json& document, Layout& output)
{
    try
    {
        Parser parser;
        Layout candidate = parser.Parse(document);
        output = std::move(candidate);
        return {};
    }
    catch (const RendererResult& result)
    {
        return result;
    }
    catch (...)
    {
        return {"parse_failure", "", ""};
    }
}
} // namespace open_st::renderer_detail
