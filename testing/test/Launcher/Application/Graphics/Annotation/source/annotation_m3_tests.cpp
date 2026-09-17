// 文件职责：验证 M3 正文规范、类型专属属性、旧擦痕保留和共享正文计费。
#include <annotation_document.h>
#include <array>
#include <gtest/gtest.h>

namespace open_st
{
namespace
{
// 创建经过统一换行规范化的文字对象。
// 入参：id：稳定 ID；text：原始 UTF-16。
// 返回：默认字号 24 的对象，非法正文返回的对象仍由测试进一步验证。
AnnotationObject TextObject(std::uint64_t id, std::u16string_view text)
{
    std::shared_ptr<const std::u16string> normalized;
    EXPECT_TRUE(NormalizeAnnotationText(text, normalized));
    AnnotationObject object{id, AnnotationKind::Text, {-20.0, 30.0}};
    object.payload = AnnotationText{normalized, 24.0};
    return object;
}
} // namespace

// 验证换行统一为 LF 且保留空格、中日文和完整代理对，不因规范化改变正文语义。
// 入参：无。
// 返回：规范化结果、字体常量与空白判断断言。
TEST(AnnotationM3Test, text_normalizes_newlines_without_trimming)
{
    std::shared_ptr<const std::u16string> text;
    ASSERT_TRUE(NormalizeAnnotationText(u" 中\r\n日\rA\n\U0001F642 ", text));
    EXPECT_EQ(*text, u" 中\n日\nA\n\U0001F642 ");
    EXPECT_TRUE(IsNormalizedAnnotationText(*text));
    EXPECT_FALSE(IsNormalizedAnnotationText(u"A\rB"));
    EXPECT_TRUE(IsBlankAnnotationText(u" \t\n\u3000\u00A0"));
    EXPECT_FALSE(IsBlankAnnotationText(u"\u3000中"));
    EXPECT_EQ(ANNOTATION_FONT_FAMILY, u"Segoe UI");
}

// 验证原始输入先检查 8192 单元、代理项和 NUL，失败不覆盖此前共享正文。
// 入参：无。
// 返回：输入边界及失败原子性断言。
TEST(AnnotationM3Test, invalid_utf16_and_raw_limit_preserve_previous_text)
{
    std::shared_ptr<const std::u16string> text;
    ASSERT_TRUE(NormalizeAnnotationText(u"原文", text));
    const std::shared_ptr<const std::u16string> original = text;
    EXPECT_FALSE(NormalizeAnnotationText(std::u16string{0xD800U}, text));
    EXPECT_FALSE(NormalizeAnnotationText(std::u16string{0xDC00U}, text));
    EXPECT_FALSE(NormalizeAnnotationText(std::u16string{u'A', 0U, u'B'}, text));
    EXPECT_EQ(text, original);
    std::u16string oversized;
    for (std::size_t index = 0U; index < ANNOTATION_MAX_TEXT_UNITS / 2U + 1U; ++index)
        oversized += u"\r\n";
    EXPECT_FALSE(NormalizeAnnotationText(oversized, text));
    EXPECT_EQ(text, original);
    ASSERT_TRUE(NormalizeAnnotationText(std::u16string(ANNOTATION_MAX_TEXT_UNITS, u'A'), text));
    EXPECT_EQ(text->size(), ANNOTATION_MAX_TEXT_UNITS);
}

// 验证正文替换保留 ID、字号、位置和擦除引用，CRLF 等价输入不产生新文档。
// 入参：无。
// 返回：同值共享、旧擦痕和空白拒绝断言。
TEST(AnnotationM3Test, text_replace_retains_erasures_and_normalized_noop)
{
    AnnotationSnapshot document;
    ASSERT_TRUE(AppendAnnotation({}, TextObject(1U, u"甲\n乙"), document));
    const std::shared_ptr<const AnnotationEraseStroke> stroke = std::make_shared<const AnnotationEraseStroke>(
        AnnotationEraseStroke{{{-10.0, 35.0}}, 4.0, {-100.0, -100.0, 100.0, 100.0}});
    const std::array<std::uint64_t, 1U> ids{1U};
    ASSERT_TRUE(ApplyAnnotationErase(document, ids, stroke, document));
    const AnnotationSnapshot original = document;
    ASSERT_TRUE(ReplaceAnnotationText(original, 1U, u"甲\r\n乙", document));
    EXPECT_EQ(document, original);
    ASSERT_TRUE(ReplaceAnnotationText(original, 1U, u"新正文", document));
    EXPECT_EQ(document->front().erasures, original->front().erasures);
    EXPECT_EQ(document->front().origin.x, original->front().origin.x);
    EXPECT_EQ(document->front().id, 1U);
    EXPECT_EQ(std::get<AnnotationText>(document->front().payload).fontSize, 24.0);
    EXPECT_EQ(*std::get<AnnotationText>(document->front().payload).text, u"新正文");
    const AnnotationSnapshot changed = document;
    EXPECT_FALSE(ReplaceAnnotationText(original, 1U, u"\r\n \u3000", document));
    EXPECT_EQ(document, changed);
}

// 验证类型专属参数严格匹配，文字只改字号和样式，马赛克仅改档位并保持不透明。
// 入参：无。
// 返回：有效档位、非法混合参数及共享正文断言。
TEST(AnnotationM3Test, typed_properties_preserve_payload_and_reject_irrelevant_parameters)
{
    AnnotationSnapshot text;
    ASSERT_TRUE(AppendAnnotation({}, TextObject(1U, u"文字"), text));
    AnnotationProperties properties = PropertiesOf(text->front());
    const std::shared_ptr<const std::u16string> body = std::get<AnnotationText>(text->front().payload).text;
    properties.fontSize = 48.0;
    properties.style.transparency = 25U;
    ASSERT_TRUE(ReplaceAnnotationProperties(text, 1U, properties, text));
    EXPECT_EQ(std::get<AnnotationText>(text->front().payload).text, body);
    EXPECT_EQ(std::get<AnnotationText>(text->front().payload).fontSize, 48.0);
    properties.blockSize = 16U;
    EXPECT_FALSE(ReplaceAnnotationProperties(text, 1U, properties, text));
    AnnotationObject mosaic{2U, AnnotationKind::Mosaic, {}, {40.0, -20.0}};
    mosaic.payload = AnnotationMosaic{16U};
    AnnotationSnapshot document;
    ASSERT_TRUE(AppendAnnotation({}, mosaic, document));
    properties = PropertiesOf(mosaic);
    for (unsigned size : {4U, 8U, 16U, 32U})
    {
        properties.blockSize = size;
        ASSERT_TRUE(ReplaceAnnotationProperties(document, 2U, properties, document));
        EXPECT_EQ(std::get<AnnotationMosaic>(document->front().payload).blockSize, size);
    }
    properties.blockSize = 10U;
    EXPECT_FALSE(ReplaceAnnotationProperties(document, 2U, properties, document));
    properties.blockSize = 16U;
    properties.style.transparency = 50U;
    EXPECT_FALSE(ReplaceAnnotationProperties(document, 2U, properties, document));
}

// 验证文字空白和未规范化载荷不能进入文档，预览隐藏只生成视图而不删除原对象。
// 入参：无。
// 返回：文档校验和只读隐藏断言。
TEST(AnnotationM3Test, invalid_text_payload_and_hidden_editing_view)
{
    EXPECT_FALSE(IsValidAnnotation(TextObject(1U, u" \n")));
    AnnotationObject invalid = TextObject(1U, u"正文");
    std::get<AnnotationText>(invalid.payload).fontSize = 13.0;
    EXPECT_FALSE(IsValidAnnotation(invalid));
    invalid = TextObject(1U, u"正文");
    std::get<AnnotationText>(invalid.payload).text = std::make_shared<const std::u16string>(u"A\r\nB");
    EXPECT_FALSE(IsValidAnnotation(invalid));
    AnnotationSnapshot original;
    ASSERT_TRUE(AppendAnnotation({}, TextObject(1U, u"正文"), original));
    ASSERT_TRUE(AppendAnnotation(original, {2U, AnnotationKind::Line, {}, {20.0, 0.0}}, original));
    AnnotationSnapshot hidden;
    ASSERT_TRUE(HideAnnotationForPreview(original, 1U, hidden));
    ASSERT_EQ(hidden->size(), 1U);
    EXPECT_EQ(hidden->front().id, 2U);
    EXPECT_EQ(original->size(), 2U);
}

// 验证共享长正文在属性历史中按身份计费，字号变化不会重复计入整段字符串。
// 入参：无。
// 返回：深层字节数和多文档共享去重断言。
TEST(AnnotationM3Test, shared_utf16_body_is_counted_once_across_properties)
{
    AnnotationSnapshot original;
    ASSERT_TRUE(AppendAnnotation({}, TextObject(1U, std::u16string(8192U, u'中')), original));
    AnnotationProperties properties = PropertiesOf(original->front());
    properties.fontSize = 48.0;
    AnnotationSnapshot changed;
    ASSERT_TRUE(ReplaceAnnotationProperties(original, 1U, properties, changed));
    const std::array<AnnotationSnapshot, 2U> documents{original, changed};
    const std::size_t bytes = AnnotationDocumentsStorageBytes(documents);
    EXPECT_GT(AnnotationDocumentsStorageBytes(std::array{original}), 8192U * sizeof(char16_t));
    EXPECT_GT(bytes, AnnotationDocumentsStorageBytes(std::array{original}));
    EXPECT_LT(bytes, AnnotationDocumentsStorageBytes(std::array{original}) +
                         AnnotationDocumentsStorageBytes(std::array{changed}));
}
} // namespace open_st
