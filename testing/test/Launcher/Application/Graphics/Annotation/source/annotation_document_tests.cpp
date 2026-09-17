// 文件职责：验证标准库不可变文档操作的共享语义、原子失败和物理坐标平移。
#include <annotation_document.h>
#include <array>
#include <gtest/gtest.h>
#include <limits>

namespace open_st
{
// 验证追加保持旧快照和对象顺序，拒绝重复稳定 ID，但允许未提交的零 ID 草稿。
// 入参：无。
// 返回：不可变快照、稳定 ID 和容量估算断言。
TEST(AnnotationDocumentTest, append_keeps_original_and_rejects_duplicate_id)
{
    AnnotationObject first{1U, AnnotationKind::Rectangle, {-10.0, -20.0}, {30.0, 40.0}};
    AnnotationSnapshot original;
    ASSERT_TRUE(AppendAnnotation({}, first, original));
    ASSERT_EQ(original->size(), 1U);
    AnnotationObject second{2U, AnnotationKind::Arrow, {}, {20.0, 0.0}};
    AnnotationSnapshot result;
    ASSERT_TRUE(AppendAnnotation(original, second, result));
    EXPECT_NE(result, original);
    EXPECT_EQ(original->size(), 1U);
    ASSERT_EQ(result->size(), 2U);
    EXPECT_EQ(result->front().id, 1U);
    EXPECT_EQ(result->back().id, 2U);
    const AnnotationSnapshot saved = result;
    EXPECT_FALSE(AppendAnnotation(original, first, result));
    EXPECT_EQ(result, saved);
    second.id = 0U;
    ASSERT_TRUE(AppendAnnotation(original, second, result));
    EXPECT_EQ(result->back().id, 0U);
    EXPECT_EQ(FindAnnotation(result, 0U), nullptr);
    EXPECT_EQ(FindAnnotation(result, 1U)->origin.x, -10.0);
    EXPECT_EQ(FindAnnotation(result, 999U), nullptr);
    EXPECT_GE(AnnotationDocumentsStorageBytes(std::array{result}), result->size() * sizeof(AnnotationObject));
    EXPECT_EQ(AnnotationDocumentsStorageBytes({}), 0U);
}

// 验证单元素属性替换不改变几何、叠放和其他元素，同值直接共享原文档。
// 入参：无。
// 返回：原文档保留、同值共享和透明度端点断言。
TEST(AnnotationDocumentTest, replace_style_preserves_identity_geometry_and_noop_snapshot)
{
    const AnnotationObject first{1U, AnnotationKind::RoundedRectangle, {-10.0, 20.0}, {-30.0, 40.0}};
    const AnnotationObject second{2U, AnnotationKind::Arrow, {}, {50.0, 20.0}};
    AnnotationSnapshot original;
    ASSERT_TRUE(AppendAnnotation({}, first, original));
    ASSERT_TRUE(AppendAnnotation(original, second, original));
    AnnotationSnapshot result;
    ASSERT_TRUE(ReplaceAnnotationProperties(original, 1U, PropertiesOf(first), result));
    EXPECT_EQ(result, original);
    ASSERT_TRUE(ReplaceAnnotationProperties(original, 1U, {{0x123456U, 100U, 8.0}, {}, {}}, result));
    EXPECT_NE(result, original);
    const AnnotationObject& changed = result->front();
    EXPECT_EQ(changed.id, first.id);
    EXPECT_EQ(changed.kind, first.kind);
    EXPECT_DOUBLE_EQ(changed.origin.x, first.origin.x);
    EXPECT_DOUBLE_EQ(changed.origin.y, first.origin.y);
    EXPECT_DOUBLE_EQ(changed.extent.x, first.extent.x);
    EXPECT_DOUBLE_EQ(changed.extent.y, first.extent.y);
    EXPECT_DOUBLE_EQ(changed.cornerRadius, first.cornerRadius);
    EXPECT_EQ(changed.style.rgb, 0x123456U);
    EXPECT_EQ(changed.style.transparency, 100U);
    EXPECT_EQ(original->front().style.rgb, first.style.rgb);
    EXPECT_EQ(result->back().id, second.id);
    EXPECT_EQ(result->back().style.rgb, second.style.rgb);
    EXPECT_DOUBLE_EQ(result->back().extent.y, second.extent.y);
}

// 验证平移仅改变原点且基于原快照，零偏移和空文档不产生副本。
// 入参：无。
// 返回：负坐标、有符号终点及快照共享断言。
TEST(AnnotationDocumentTest, translate_changes_only_origins_and_reuses_zero_delta)
{
    const AnnotationObject first{1U, AnnotationKind::Rectangle, {-200.0, -100.0}, {-20.0, 30.0}};
    AnnotationSnapshot original;
    ASSERT_TRUE(AppendAnnotation({}, first, original));
    AnnotationSnapshot result;
    ASSERT_TRUE(TranslateAnnotations(original, {}, result));
    EXPECT_EQ(result, original);
    ASSERT_TRUE(TranslateAnnotations(original, {250.0, -50.0}, result));
    EXPECT_EQ(result->front().origin.x, 50.0);
    EXPECT_EQ(result->front().origin.y, -150.0);
    EXPECT_EQ(result->front().extent.x, -20.0);
    EXPECT_EQ(result->front().extent.y, 30.0);
    EXPECT_EQ(result->front().id, first.id);
    EXPECT_EQ(result->front().style.rgb, first.style.rgb);
    EXPECT_EQ(original->front().origin.x, -200.0);
    ASSERT_TRUE(TranslateAnnotations({}, {5.0, 5.0}, result));
    EXPECT_EQ(result, nullptr);
}

// 验证非法对象、未知 ID、样式和坐标溢出不会破坏调用方此前持有的结果。
// 入参：无。
// 返回：各失败入口的输出原子性断言。
TEST(AnnotationDocumentTest, invalid_edits_preserve_previous_result)
{
    AnnotationObject object{1U, AnnotationKind::Rectangle, {}, {20.0, 20.0}};
    AnnotationSnapshot original;
    ASSERT_TRUE(AppendAnnotation({}, object, original));
    AnnotationSnapshot result = original;
    object.extent.x = 0.0;
    EXPECT_FALSE(AppendAnnotation(original, object, result));
    EXPECT_EQ(result, original);
    EXPECT_FALSE(ReplaceAnnotationProperties(original, 99U, {}, result));
    EXPECT_EQ(result, original);
    EXPECT_FALSE(ReplaceAnnotationProperties(original, 1U, {{0U, 101U, 3.0}, {}, {}}, result));
    EXPECT_EQ(result, original);
    EXPECT_FALSE(TranslateAnnotations(original, {2147483647.0, 0.0}, result));
    EXPECT_EQ(result, original);
    EXPECT_FALSE(TranslateAnnotations(original, {std::numeric_limits<double>::infinity(), 0.0}, result));
    EXPECT_EQ(result, original);
    const AnnotationSnapshot invalid =
        std::make_shared<const std::vector<AnnotationObject>>(std::vector<AnnotationObject>{object});
    EXPECT_FALSE(TranslateAnnotations(invalid, {}, result));
    EXPECT_EQ(result, original);
}
} // namespace open_st
