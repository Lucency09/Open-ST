#include "renderer_model.h"
#include <gtest/gtest.h>
#include <limits>

namespace
{
using open_st::renderer_detail::Layout;
using open_st::renderer_detail::ParseLayout;
// 创建无业务依赖的完整布局，用于逐项修改协议边界。
nlohmann::json Document()
{
    return nlohmann::json::parse(R"({
        "schemaVersion":1,
        "window":{"titleKey":"title"},
        "pages":[{"id":"page","titleKey":"page.title","content":{
            "type":"column","id":"column","children":[
                {"type":"select","id":"choice","labelKey":"label","width":"fill"}
            ]}}],
        "footer":{"leading":[],"trailing":[{"type":"button","id":"confirm","textKey":"confirm"}]}
    })");
}
// 验证布局不借用输入 JSON，输入销毁后仍保留控件与文字键。
TEST(RendererParserTest, copies_document)
{
    Layout layout;
    {
        const nlohmann::json document = Document();
        ASSERT_TRUE(ParseLayout(document, layout));
    }
    EXPECT_EQ(layout.pages[0].content.children[0].id, "choice");
    EXPECT_EQ(layout.trailing[0].textKey, "confirm");
}
// 页面与底部按钮共享 ID 空间，重复时附带定位并保留旧输出。
TEST(RendererParserTest, rejects_duplicate_ids_transactionally)
{
    Layout layout;
    ASSERT_TRUE(ParseLayout(Document(), layout));
    nlohmann::json document = Document();
    document["footer"]["trailing"][0]["id"] = "page";
    const open_st::RendererResult result = ParseLayout(document, layout);
    EXPECT_EQ(result.code, "duplicate_id");
    EXPECT_EQ(result.id, "page");
    EXPECT_EQ(result.path, "/footer/trailing/0/id");
    EXPECT_EQ(layout.trailing[0].id, "confirm");
}
// 未知属性、节点类别及协议版本不能被默认忽略。
TEST(RendererParserTest, rejects_unknown_protocol_fields)
{
    Layout layout;
    nlohmann::json document = Document();
    document["schemaVersion"] = 2;
    EXPECT_EQ(ParseLayout(document, layout).code, "unsupported_version");
    document = Document();
    document["window"]["titelKey"] = "typo";
    EXPECT_EQ(ParseLayout(document, layout).code, "unknown_property");
    document = Document();
    document["pages"][0]["content"]["children"][0]["type"] = "script";
    EXPECT_EQ(ParseLayout(document, layout).code, "unknown_node");
}
// 负数、布尔、过大无符号值及浮点尺寸均不能进入布局计算。
TEST(RendererParserTest, rejects_invalid_dimensions)
{
    const std::vector<nlohmann::json> invalid = {-1, true, 0, 1.5, std::numeric_limits<std::uint64_t>::max()};
    for (const nlohmann::json& value : invalid)
    {
        Layout layout;
        nlohmann::json document = Document();
        document["pages"][0]["content"]["children"][0]["width"] = value;
        EXPECT_EQ(ParseLayout(document, layout).code, "invalid_dimension");
    }
}
// 递归布局深度超过上限必须在创建窗口之前拒绝。
TEST(RendererParserTest, limits_depth)
{
    nlohmann::json node = {{"type", "text"}, {"id", "leaf"}, {"textKey", "text"}};
    for (int depth = 0; depth < 33; ++depth)
        node = {
            {"type", "column"}, {"id", "nested" + std::to_string(depth)}, {"children", nlohmann::json::array({node})}};
    nlohmann::json document = Document();
    document["pages"][0]["content"] = std::move(node);
    Layout layout;
    EXPECT_EQ(ParseLayout(document, layout).code, "depth_limit");
}
// 过大节点总量在协议解析阶段拒绝，包含页面及容器计数。
TEST(RendererParserTest, limits_node_count)
{
    nlohmann::json document = Document();
    nlohmann::json& children = document["pages"][0]["content"]["children"];
    for (int index = 0; index < 2048; ++index)
        children.push_back({{"type", "text"}, {"id", "text" + std::to_string(index)}, {"textKey", "text"}});
    Layout layout;
    EXPECT_EQ(ParseLayout(document, layout).code, "node_limit");
}
// 不允许空页面、非容器页内容及底部交互类型混用。
TEST(RendererParserTest, rejects_invalid_structure)
{
    Layout layout;
    nlohmann::json document = Document();
    document["pages"] = nlohmann::json::array();
    EXPECT_EQ(ParseLayout(document, layout).code, "invalid_pages");
    document = Document();
    document["footer"]["trailing"][0] = {{"type", "text"}, {"id", "text"}, {"textKey", "text"}};
    EXPECT_EQ(ParseLayout(document, layout).code, "invalid_footer_node");
}
} // namespace
