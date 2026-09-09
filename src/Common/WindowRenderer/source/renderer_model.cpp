// 校验布局 JSON 的字段、尺寸和控件结构，并构建独立的布局树。

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
    // 解析窗口布局协议并构建具有自有存储的布局树。
    // 入参：document：完整布局 JSON，所有尺寸按 DIP 解释。
    // 返回：完整 Layout 值；协议、字段或节点无效时抛出 RendererResult，不返回部分树。
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

    // 中止无效布局的解析并携带可定位的结构化错误。
    // 入参：code：稳定错误码；path：出错字段的 JSON 路径；id：可选页面或控件 ID。
    // 返回：不返回；抛出包含 code、path、id 的 RendererResult。
    [[noreturn]] void Fail(std::string code, std::string path, std::string id = {}) const
    {
        throw RendererResult{std::move(code), std::move(path), std::move(id)};
    }
    // 检查布局对象只包含当前节点类型允许的属性。
    // 入参：object：待检查的 JSON 对象；allowed：允许的属性名称集合；path：对象的 JSON 路径。
    // 返回：无返回值；非对象或出现未知字段时抛出定位错误。
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
    // 读取布局对象中的必填属性。
    // 入参：object：待读取的 JSON 对象；key：必填字段名；path：对象的 JSON 路径。
    // 返回：字段值的借用 const JSON 引用；字段缺失时抛出定位错误。
    const nlohmann::json& Required(const nlohmann::json& object, const char* key, const std::string& path) const
    {
        if (!object.contains(key))
            this->Fail("missing_property", path + "/" + key);
        return object[key];
    }
    // 读取适合作为文本键或节点 ID 的有效 UTF-8 字符串。
    // 入参：object：含字段的 JSON 对象；key：字符串字段名；path：对象的 JSON 路径。
    // 返回：字符串值副本；缺失、类型错误、空值、超过 4096 字节、含空字符或非法 UTF-8 时抛出定位错误。
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
    // 校验并登记页面或控件的全局唯一 ID。
    // 入参：object：包含 id 的 JSON 节点；path：该节点的 JSON 路径。
    // 返回：已登记的 ID 副本；ID 无效、重复或总登记数量超过 2048 时抛出定位错误。
    std::string Id(const nlohmann::json& object, const std::string& path)
    {
        const std::string id = this->String(object, "id", path);
        if (++this->count_ > 2048)
            this->Fail("node_limit", path, id);
        if (!this->ids_.insert(id).second)
            this->Fail("duplicate_id", path + "/id", id);
        return id;
    }
    // 读取限定范围内的布局整数，避免尺寸转换溢出。
    // 入参：value：待转换的 JSON 数值；path：值的 JSON 路径；minimum：允许的最小整数。
    // 返回：范围 minimum 至 32767 内的整数；类型或范围不合法时抛出定位错误。
    int Integer(const nlohmann::json& value, const std::string& path, int minimum) const
    {
        if (!value.is_number_integer() || value < minimum || value > 32767)
            this->Fail("invalid_dimension", path);
        return value.get<int>();
    }
    // 读取可选的窗口客户区尺寸配置。
    // 入参：window：窗口配置 JSON；key：initialSize 或 minSize 字段名；width、height：输入输出 DIP 尺寸。
    // 返回：无返回值；字段缺失保留尺寸，存在时写入正整数宽高，格式无效抛出定位错误。
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
    // 递归构建列容器或已支持的叶控件节点。
    // 入参：source：节点 JSON；path：节点的 JSON 路径；depth：当前递归层级，从 1 开始。
    // 返回：自有 Node 值；超过 32 层、未知类型或属性无效时抛出定位错误。
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
        else if (type == "select" || type == "checkbox")
        {
            node.type = type == "select" ? NodeType::Select : NodeType::Checkbox;
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
    // 解析窗口底部 leading 或 trailing 区域的按钮列表。
    // 入参：footer：底部区域 JSON；key：待解析的区域名称；output：输出参数，依序追加自有按钮节点。
    // 返回：无返回值；区域缺失不追加，非数组或包含非按钮节点时抛出定位错误。
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
// 校验并解析 JSON 布局为可独立持有的窗口布局树。
// 入参：document：调用期间借用的完整布局 JSON；output：输出参数，成功时接收解析后的布局。
// 返回：错误码为空表示成功；失败提供错误码、JSON 路径及控件 ID，保留 output 原值。
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
