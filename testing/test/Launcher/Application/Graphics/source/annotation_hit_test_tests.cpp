// 文件职责：验证标注属性入口的真实几何、精确优先、叠放和选区内点击容差。
#include <annotation_hit_test.h>
#include <gtest/gtest.h>
#include <limits>

namespace open_st
{
namespace
{
// 固定测试对象顺序为不可变快照，后面的元素位于上层。
// 入参：objects：待移入的对象列表。
// 返回：拥有元素的只读共享快照。
AnnotationSnapshot HitSnapshot(std::vector<AnnotationObject> objects)
{
    return std::make_shared<const std::vector<AnnotationObject>>(std::move(objects));
}

// 查询默认选区中的元素并确认成功路径会清除旧诊断。
// 入参：snapshot：只读元素；point：桌面物理点；tolerance：物理容差；selection：正式选区。
// 返回：命中 ID，未命中为零；接口错误同时报告测试失败。
std::uint64_t Hit(const AnnotationSnapshot& snapshot, AnnotationPoint point, float tolerance = 0.0F,
                  RectI selection = {-100, -100, 100, 100})
{
    std::uint64_t objectId = 999U;
    std::wstring error = L"旧诊断";
    EXPECT_TRUE(HitTestAnnotations(snapshot, selection, point, tolerance, objectId, error)) << error;
    EXPECT_TRUE(error.empty());
    return objectId;
}
} // namespace

// 验证空心矩形内部和远处空白不命中，正反向矩形使用相同实际描边。
// 入参：无。
// 返回：边框、内部及反向几何的 ID 断言。
TEST(AnnotationHitTest, outline_rejects_interior_and_handles_reverse_extent)
{
    AnnotationObject object{1U, AnnotationKind::Rectangle, {10.0, 10.0}, {40.0, 30.0}};
    object.style.lineWidth = 2.0;
    const AnnotationSnapshot snapshot = HitSnapshot({object});
    EXPECT_EQ(Hit(snapshot, {10.0, 25.0}), 1U);
    EXPECT_EQ(Hit(snapshot, {30.0, 25.0}), 0U);
    EXPECT_EQ(Hit(snapshot, {0.0, 0.0}, 3.0F), 0U);
    object.origin = {50.0, 40.0};
    object.extent = {-40.0, -30.0};
    EXPECT_EQ(Hit(HitSnapshot({object}), {10.0, 25.0}), 1U);
    EXPECT_EQ(Hit(HitSnapshot({object}), {30.0, 25.0}), 0U);
}

// 验证圆角外的空白和箭头外接框空白不会误命中，箭头杆身与头部都可点击。
// 入参：无。
// 返回：真实轮廓命中 ID 的断言。
TEST(AnnotationHitTest, rounded_and_arrow_use_actual_geometry)
{
    AnnotationObject rounded{1U, AnnotationKind::RoundedRectangle, {10.0, 10.0}, {30.0, 30.0}};
    EXPECT_EQ(Hit(HitSnapshot({rounded}), {10.1, 10.1}, 8.0F), 0U);
    EXPECT_EQ(Hit(HitSnapshot({rounded}), {20.0, 20.0}), 1U);
    AnnotationObject arrow{2U, AnnotationKind::Arrow, {0.0, 0.0}, {60.0, 0.0}};
    arrow.style.lineWidth = 4.0;
    const AnnotationSnapshot arrows = HitSnapshot({arrow});
    EXPECT_EQ(Hit(arrows, {20.0, 0.0}), 2U);
    EXPECT_EQ(Hit(arrows, {48.0, 3.0}), 2U);
    EXPECT_EQ(Hit(arrows, {20.0, 6.0}), 0U);
    EXPECT_EQ(Hit(arrows, {58.0, 6.0}), 0U);
}

// 验证圆角半径在小矩形上夹到半短边，矩形填充不扩大点击范围。
// 入参：无。
// 返回：小圆角和填充无容差的边界断言。
TEST(AnnotationHitTest, small_round_radius_clamp_and_fill_without_tolerance)
{
    AnnotationObject object{1U, AnnotationKind::RoundedRectangle, {}, {6.0, 4.0}};
    EXPECT_EQ(Hit(HitSnapshot({object}), {3.0, 2.0}), 1U);
    EXPECT_EQ(Hit(HitSnapshot({object}), {0.1, 0.1}, 10.0F), 0U);
    object.kind = AnnotationKind::FilledRectangle;
    EXPECT_EQ(Hit(HitSnapshot({object}), {0.1, 0.1}), 1U);
    EXPECT_EQ(Hit(HitSnapshot({object}), {-0.5, 2.0}, 10.0F), 0U);
}

// 验证最上层半透明元素优先，完全透明元素和零 ID 草稿不会截获点击。
// 入参：无。
// 返回：元素叠放、透明度和草稿跳过断言。
TEST(AnnotationHitTest, layer_order_transparency_and_draft_exclusion)
{
    AnnotationObject lower{1U, AnnotationKind::FilledRectangle, {}, {30.0, 30.0}};
    AnnotationObject upper = lower;
    upper.id = 2U;
    upper.style.transparency = 50U;
    EXPECT_EQ(Hit(HitSnapshot({lower, upper}), {10.0, 10.0}), 2U);
    upper.style.transparency = 100U;
    EXPECT_EQ(Hit(HitSnapshot({lower, upper}), {10.0, 10.0}), 1U);
    lower.style.transparency = 100U;
    EXPECT_EQ(Hit(HitSnapshot({lower, upper}), {10.0, 10.0}), 0U);
    lower.style.transparency = 0U;
    upper.id = 0U;
    upper.style.transparency = 0U;
    EXPECT_EQ(Hit(HitSnapshot({lower, upper}), {10.0, 10.0}), 1U);
}

// 验证任何下层精确覆盖都优先于上层点击容差，两轮分别按叠放逆序。
// 入参：无。
// 返回：精确优先与容差层次的 ID 断言。
TEST(AnnotationHitTest, exact_hit_precedes_upper_tolerance)
{
    AnnotationObject lower{1U, AnnotationKind::FilledRectangle, {5.0, 5.0}, {20.0, 20.0}};
    AnnotationObject upper{2U, AnnotationKind::Rectangle, {10.0, 10.0}, {40.0, 40.0}};
    upper.style.lineWidth = 2.0;
    EXPECT_EQ(Hit(HitSnapshot({lower, upper}), {8.0, 20.0}, 3.0F), 1U);
    EXPECT_EQ(Hit(HitSnapshot({upper}), {8.0, 20.0}, 3.0F), 2U);
    AnnotationObject newest = upper;
    newest.id = 3U;
    EXPECT_EQ(Hit(HitSnapshot({upper, newest}), {8.0, 20.0}, 3.0F), 3U);
}

// 验证 DPI 换算后的不同物理容差作用于实际轮廓，不将容差当作几何展平误差。
// 入参：无。
// 返回：零容差、小容差与高 DPI 容差下的命中结果断言。
TEST(AnnotationHitTest, physical_tolerance_changes_stroke_and_arrow_reach)
{
    AnnotationObject rectangle{1U, AnnotationKind::Rectangle, {}, {40.0, 40.0}};
    rectangle.style.lineWidth = 2.0;
    const AnnotationSnapshot rectangles = HitSnapshot({rectangle});
    EXPECT_EQ(Hit(rectangles, {-5.0, 20.0}, 0.0F), 0U);
    EXPECT_EQ(Hit(rectangles, {-5.0, 20.0}, 3.0F), 0U);
    EXPECT_EQ(Hit(rectangles, {-5.0, 20.0}, 6.0F), 1U);
    AnnotationObject arrow{2U, AnnotationKind::Arrow, {}, {60.0, 0.0}};
    arrow.style.lineWidth = 4.0;
    EXPECT_EQ(Hit(HitSnapshot({arrow}), {20.0, 5.0}, 0.0F), 0U);
    EXPECT_EQ(Hit(HitSnapshot({arrow}), {20.0, 5.0}, 4.0F), 2U);
    EXPECT_EQ(Hit(HitSnapshot({arrow}), {62.0, 0.0}, 3.0F), 2U);
    EXPECT_EQ(Hit(HitSnapshot({arrow}), {64.0, 0.0}, 3.0F), 0U);
}

// 验证负坐标与半开选区边界，区外点和完全被裁掉的笔迹均不能借容差命中。
// 入参：无。
// 返回：桌面坐标变换、正式裁剪和不可见边缘的断言。
TEST(AnnotationHitTest, negative_coordinates_and_selection_clipping)
{
    AnnotationObject fill{1U, AnnotationKind::FilledRectangle, {-40.0, -20.0}, {80.0, 40.0}};
    const RectI selection{-20, -10, 20, 10};
    EXPECT_EQ(Hit(HitSnapshot({fill}), {-20.0, 0.0}, 3.0F, selection), 1U);
    EXPECT_EQ(Hit(HitSnapshot({fill}), {19.9, 0.0}, 3.0F, selection), 1U);
    EXPECT_EQ(Hit(HitSnapshot({fill}), {20.0, 0.0}, 3.0F, selection), 0U);
    EXPECT_EQ(Hit(HitSnapshot({fill}), {0.0, 10.0}, 3.0F, selection), 0U);
    AnnotationObject outside{2U, AnnotationKind::Arrow, {-40.0, -12.0}, {80.0, 0.0}};
    outside.style.lineWidth = 2.0;
    // 箭头头部位于裁剪区右侧，杆身也完全在选区上方。
    EXPECT_EQ(Hit(HitSnapshot({outside}), {0.0, -9.0}, 8.0F, selection), 0U);
    AnnotationObject outline{3U, AnnotationKind::Rectangle, {-40.0, -20.0}, {80.0, 40.0}};
    outline.style.lineWidth = 2.0;
    EXPECT_EQ(Hit(HitSnapshot({outline}), {0.0, -9.0}, 20.0F, selection), 0U);
}

// 验证空快照正常未命中，非法输入清零旧 ID 且报告诊断，不返回部分查询结果。
// 入参：无。
// 返回：空输入兼容性和错误原子性的断言。
TEST(AnnotationHitTest, invalid_inputs_clear_output_and_empty_is_no_hit)
{
    EXPECT_EQ(Hit({}, {}), 0U);
    EXPECT_EQ(Hit(HitSnapshot({}), {}), 0U);
    AnnotationObject object{1U, AnnotationKind::FilledRectangle, {}, {20.0, 20.0}};
    const AnnotationSnapshot snapshot = HitSnapshot({object});
    const float nan = std::numeric_limits<float>::quiet_NaN();
    for (const float tolerance : {-1.0F, nan, std::numeric_limits<float>::infinity()})
    {
        std::uint64_t id = 99U;
        std::wstring error;
        EXPECT_FALSE(HitTestAnnotations(snapshot, {0, 0, 50, 50}, {10.0, 10.0}, tolerance, id, error));
        EXPECT_EQ(id, 0U);
        EXPECT_FALSE(error.empty());
    }
    std::uint64_t id = 99U;
    std::wstring error;
    EXPECT_FALSE(HitTestAnnotations(snapshot, {}, {}, 3.0F, id, error));
    EXPECT_EQ(id, 0U);
    id = 99U;
    EXPECT_FALSE(HitTestAnnotations(snapshot, {0, 0, 50, 50}, {nan, 0.0}, 3.0F, id, error));
    EXPECT_EQ(id, 0U);
    object.style.transparency = 101U;
    id = 99U;
    EXPECT_FALSE(HitTestAnnotations(HitSnapshot({object}), {0, 0, 50, 50}, {10.0, 10.0}, 3.0F, id, error));
    EXPECT_EQ(id, 0U);
    EXPECT_FALSE(error.empty());
}
} // namespace open_st
