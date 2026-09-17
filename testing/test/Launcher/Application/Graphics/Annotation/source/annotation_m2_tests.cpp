// 文件职责：验证 M2 路径载荷、局部擦除引用及按共享身份去重的深层预算。
#include <annotation_document.h>
#include <array>
#include <gtest/gtest.h>
#include <limits>

namespace open_st
{
namespace
{
// 构造固定桌面裁剪中的可复用擦除轨迹。
// 入参：无。
// 返回：一条包含两个采样点的不可变轨迹。
std::shared_ptr<const AnnotationEraseStroke> EraseStroke()
{
    return std::make_shared<const AnnotationEraseStroke>(
        AnnotationEraseStroke{{{5.0, 10.0}, {20.0, 10.0}}, 4.0, {-100.0, -100.0, 100.0, 100.0}});
}
} // namespace

// 验证单点画笔有效、直线零长度无效、椭圆允许反向拖动且拒绝零面积。
// 入参：无。
// 返回：种类、载荷和几何边界断言。
TEST(AnnotationM2Test, pen_dot_line_and_reverse_ellipse_validation)
{
    AnnotationObject object{1U, AnnotationKind::Pen, {-20.0, -10.0}};
    EXPECT_FALSE(IsValidAnnotation(object));
    object.payload = AnnotationStroke{
        std::make_shared<const std::vector<AnnotationPoint>>(std::vector<AnnotationPoint>{{0.0, 0.0}})};
    EXPECT_TRUE(IsValidAnnotation(object));
    object.kind = AnnotationKind::Line;
    EXPECT_FALSE(IsValidAnnotation(object));
    object.payload = std::monostate{};
    EXPECT_FALSE(IsValidAnnotation(object));
    object.extent = {0.0, 20.0};
    EXPECT_TRUE(IsValidAnnotation(object));
    object.kind = AnnotationKind::Ellipse;
    EXPECT_FALSE(IsValidAnnotation(object));
    object.extent = {-20.0, -30.0};
    EXPECT_TRUE(IsValidAnnotation(object));
}

// 验证路径数量上限、非有限点及空擦除路径均明确拒绝。
// 入参：无。
// 返回：动态载荷与擦除坐标校验断言。
TEST(AnnotationM2Test, invalid_path_points_radius_and_clip_are_rejected)
{
    AnnotationObject object{1U, AnnotationKind::Pen};
    object.payload = AnnotationStroke{
        std::make_shared<const std::vector<AnnotationPoint>>(ANNOTATION_MAX_PATH_POINTS, AnnotationPoint{})};
    EXPECT_TRUE(IsValidAnnotation(object));
    object.payload = AnnotationStroke{
        std::make_shared<const std::vector<AnnotationPoint>>(ANNOTATION_MAX_PATH_POINTS + 1U, AnnotationPoint{})};
    EXPECT_FALSE(IsValidAnnotation(object));
    object.payload = AnnotationStroke{std::make_shared<const std::vector<AnnotationPoint>>(
        std::vector<AnnotationPoint>{{std::numeric_limits<double>::quiet_NaN(), 0.0}})};
    EXPECT_FALSE(IsValidAnnotation(object));
    AnnotationEraseStroke stroke = *EraseStroke();
    EXPECT_TRUE(IsValidAnnotationEraseStroke(stroke));
    stroke.radius = 0.0;
    EXPECT_FALSE(IsValidAnnotationEraseStroke(stroke));
    stroke.radius = 4.0;
    stroke.clip.right = stroke.clip.left;
    EXPECT_FALSE(IsValidAnnotationEraseStroke(stroke));
    stroke = *EraseStroke();
    stroke.points.clear();
    EXPECT_FALSE(IsValidAnnotationEraseStroke(stroke));
}

// 验证多对象共用一次擦除路径，各自记录局部偏移，旧文档和后来新建对象不受影响。
// 入参：无。
// 返回：共享引用、局部坐标、对象顺序和原文档不可变断言。
TEST(AnnotationM2Test, erasure_shares_stroke_with_object_local_offsets)
{
    AnnotationSnapshot original;
    ASSERT_TRUE(AppendAnnotation({}, {1U, AnnotationKind::FilledRectangle, {-10.0, -20.0}, {40.0, 40.0}}, original));
    ASSERT_TRUE(AppendAnnotation(original, {2U, AnnotationKind::FilledRectangle, {0.0, 0.0}, {40.0, 40.0}}, original));
    const std::shared_ptr<const AnnotationEraseStroke> stroke = EraseStroke();
    const std::array<std::uint64_t, 2U> ids{1U, 2U};
    AnnotationSnapshot erased;
    ASSERT_TRUE(ApplyAnnotationErase(original, ids, stroke, erased));
    ASSERT_NE(erased->front().erasures, nullptr);
    ASSERT_NE(erased->back().erasures, nullptr);
    EXPECT_EQ(erased->front().erasures->front().stroke, stroke);
    EXPECT_EQ(erased->back().erasures->front().stroke, stroke);
    EXPECT_EQ(erased->front().erasures->front().offset.x, 10.0);
    EXPECT_EQ(erased->front().erasures->front().offset.y, 20.0);
    EXPECT_EQ(original->front().erasures, nullptr);
    AnnotationSnapshot appended;
    ASSERT_TRUE(AppendAnnotation(erased, {3U, AnnotationKind::Line, {}, {40.0, 0.0}}, appended));
    EXPECT_EQ(appended->back().erasures, nullptr);
    EXPECT_EQ(appended->front().erasures, erased->front().erasures);
    AnnotationSnapshot translated;
    ASSERT_TRUE(TranslateAnnotations(erased, {50.0, 60.0}, translated));
    EXPECT_EQ(translated->front().erasures, erased->front().erasures);
    const AnnotationEraseMask& mask = translated->front().erasures->front();
    EXPECT_EQ(mask.stroke->points.front().x + mask.offset.x + translated->front().origin.x, 55.0);
    EXPECT_EQ(mask.stroke->points.front().y + mask.offset.y + translated->front().origin.y, 70.0);
}

// 验证空目标和等值擦除不生成新文档，未知 ID 或非法路径失败时保留原输出。
// 入参：无。
// 返回：幂等与失败原子性断言。
TEST(AnnotationM2Test, empty_identical_and_invalid_erasure_are_atomic)
{
    AnnotationSnapshot original;
    ASSERT_TRUE(AppendAnnotation({}, {1U, AnnotationKind::FilledRectangle, {}, {40.0, 40.0}}, original));
    const std::shared_ptr<const AnnotationEraseStroke> stroke = EraseStroke();
    AnnotationSnapshot erased;
    ASSERT_TRUE(ApplyAnnotationErase(original, {}, stroke, erased));
    EXPECT_EQ(erased, original);
    const std::array<std::uint64_t, 2U> repeated{1U, 1U};
    ASSERT_TRUE(ApplyAnnotationErase(original, repeated, stroke, erased));
    EXPECT_EQ(erased->front().erasures->size(), 1U);
    const AnnotationSnapshot expected = erased;
    ASSERT_TRUE(ApplyAnnotationErase(erased, repeated, EraseStroke(), erased));
    EXPECT_EQ(erased, expected);
    const std::array<std::uint64_t, 1U> unknown{99U};
    EXPECT_FALSE(ApplyAnnotationErase(original, unknown, stroke, erased));
    EXPECT_EQ(erased, expected);
    EXPECT_FALSE(ApplyAnnotationErase(original, repeated, {}, erased));
    EXPECT_EQ(erased, expected);
}

// 验证路径及擦除共享块跨对象和跨历史只计一次，改变样式不会重复收取大路径预算。
// 入参：无。
// 返回：深层存储增长、共享去重和活动擦除计费断言。
TEST(AnnotationM2Test, deep_budget_deduplicates_history_paths_and_erasure_references)
{
    AnnotationObject object{1U, AnnotationKind::Pen};
    object.payload = AnnotationStroke{
        std::make_shared<const std::vector<AnnotationPoint>>(ANNOTATION_MAX_PATH_POINTS, AnnotationPoint{})};
    AnnotationSnapshot original;
    ASSERT_TRUE(AppendAnnotation({}, object, original));
    const std::shared_ptr<const AnnotationEraseStroke> stroke = EraseStroke();
    const std::array<std::uint64_t, 1U> ids{1U};
    ASSERT_TRUE(ApplyAnnotationErase(original, ids, stroke, original));
    AnnotationSnapshot changed;
    ASSERT_TRUE(ReplaceAnnotationProperties(original, 1U, {{0U, 25U, 5.0}, {}, {}}, changed));
    const std::array<AnnotationSnapshot, 2U> repeated{original, original};
    EXPECT_EQ(AnnotationDocumentsStorageBytes(repeated), AnnotationDocumentsStorageBytes(std::array{original}));
    const std::array<AnnotationSnapshot, 2U> histories{original, changed};
    const std::size_t combined = AnnotationDocumentsStorageBytes(histories);
    EXPECT_GT(combined, AnnotationDocumentsStorageBytes(std::array{original}));
    EXPECT_LT(combined, AnnotationDocumentsStorageBytes(std::array{original}) +
                            AnnotationDocumentsStorageBytes(std::array{changed}));
    EXPECT_GT(AnnotationDocumentsStorageBytes(std::array{original}),
              ANNOTATION_MAX_PATH_POINTS * sizeof(AnnotationPoint));
    EXPECT_EQ(AnnotationDocumentsStorageBytes(histories, stroke), combined);
    EXPECT_GT(AnnotationDocumentsStorageBytes({}, stroke), 0U);
}

// 验证深层预算使用实际容量，不能靠少量有效点隐藏预分配的大缓冲区。
// 入参：无。
// 返回：32 MiB 以上载荷被完整计费的断言。
TEST(AnnotationM2Test, reserved_dynamic_capacity_is_not_hidden_from_budget)
{
    std::vector<AnnotationPoint> points;
    points.reserve(32U * 1024U * 1024U / sizeof(AnnotationPoint) + 1U);
    points.push_back({});
    AnnotationObject object{1U, AnnotationKind::Pen};
    object.payload = AnnotationStroke{std::make_shared<const std::vector<AnnotationPoint>>(std::move(points))};
    AnnotationSnapshot document;
    ASSERT_TRUE(AppendAnnotation({}, object, document));
    EXPECT_GT(AnnotationDocumentsStorageBytes(std::array{document}), 32U * 1024U * 1024U);
}
} // namespace open_st
