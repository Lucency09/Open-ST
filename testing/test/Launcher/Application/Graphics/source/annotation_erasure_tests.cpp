// 验证 M2 笔迹、直线、椭圆及对象局部擦除的共同可见几何，不读取桌面或写入文件。

#include <annotation_erasure.h>
#include <annotation_hit_test.h>
#include <gtest/gtest.h>
#include <selection_output_renderer.h>

#include <array>

namespace open_st
{
namespace
{
// 固定图层顺序形成只读对象快照。
// 入参：objects 为按从底到顶排列的对象。
// 返回：独立共享文档。
AnnotationSnapshot Snapshot(std::vector<AnnotationObject> objects)
{
    return std::make_shared<const std::vector<AnnotationObject>>(std::move(objects));
}

// 创建独立白色 SDR 冻结帧，输出器仍执行生产合成流程。
// 入参：bounds 为桌面物理边界。
// 返回：有效且不引用真实桌面的冻结帧。
FrozenDesktopFrame WhiteDesktop(RectI bounds = {0, 0, 64, 64})
{
    std::vector<CapturedOutputPlane> planes;
    planes.emplace_back(
        bounds, CapturedPixelFormat::Bgra8Unorm, CapturedColorSpace::SdrGamma22P709, OutputColorMetadata{},
        std::vector<std::uint8_t>(static_cast<std::size_t>(bounds.Width()) * bounds.Height() * 4U, 255));
    return {bounds, std::move(planes)};
}

// 从输出内相对坐标读取 BGRA 字节。
// 入参：frame 为成功输出；x、y 为相对物理像素。
// 返回：该像素四字节。
std::array<std::uint8_t, 4> Pixel(const SdrSelectionFrame& frame, int x, int y)
{
    const std::size_t offset = static_cast<std::size_t>(y) * frame.Stride() + static_cast<std::size_t>(x) * 4U;
    return {frame.Pixels()[offset], frame.Pixels()[offset + 1], frame.Pixels()[offset + 2], frame.Pixels()[offset + 3]};
}

// 使用公开几何命中接口检查可见像素区域及辅助容差。
// 入参：objects 为快照；point 为桌面点；tolerance 为辅助物理距离。
// 返回：命中 ID；接口失败同时报告用例失败。
std::uint64_t Hit(const AnnotationSnapshot& objects, AnnotationPoint point, float tolerance = 0)
{
    std::uint64_t id{};
    std::wstring error;
    EXPECT_TRUE(HitTestAnnotations(objects, {-64, -64, 128, 128}, point, tolerance, id, error)) << error;
    return id;
}

// 构造一笔桌面坐标擦痕，并以负对象原点存成跟随对象平移的局部 mask。
// 入参：object 为目标；points 为桌面笔迹；radius 为半径；clip 为手势开始裁剪。
// 返回：修改后的独立对象，已有 mask 保留。
AnnotationObject Erased(AnnotationObject object, std::vector<AnnotationPoint> points, double radius,
                        AnnotationRect clip = {0, 0, 64, 64})
{
    const auto stroke =
        std::make_shared<const AnnotationEraseStroke>(AnnotationEraseStroke{std::move(points), radius, clip});
    std::vector<AnnotationEraseMask> masks = object.erasures ? *object.erasures : std::vector<AnnotationEraseMask>{};
    masks.push_back({stroke, {-object.origin.x, -object.origin.y}});
    object.erasures = std::make_shared<const std::vector<AnnotationEraseMask>>(std::move(masks));
    return object;
}

// 创建局部点列笔迹，几何坐标与实际宽度独立于窗口 DPI。
// 入参：points 为局部采样点；width 为物理线宽。
// 返回：红色有效笔迹对象。
AnnotationObject Pen(std::vector<AnnotationPoint> points, double width = 8)
{
    AnnotationObject object{1, AnnotationKind::Pen, {10, 20}, {}};
    object.style.lineWidth = width;
    object.payload = AnnotationStroke{std::make_shared<const std::vector<AnnotationPoint>>(std::move(points))};
    return object;
}
} // namespace

// 验证直线实际圆帽与命中共用几何，单点自由笔迹产生一个圆而不是消失。
// 入参：无。
// 返回：像素与圆形边界命中断言。
TEST(AnnotationErasureTest, line_round_caps_and_single_point_pen_share_draw_and_hit_geometry)
{
    AnnotationObject line{1, AnnotationKind::Line, {10, 20}, {40, 0}};
    line.style.lineWidth = 6;
    EXPECT_EQ(Hit(Snapshot({line}), {8, 20}), 1U);
    EXPECT_EQ(Hit(Snapshot({line}), {6, 20}), 0U);
    SelectionOutputRenderer renderer;
    SdrSelectionFrame frame;
    std::wstring error;
    const FrozenDesktopFrame desktop = WhiteDesktop();
    ASSERT_TRUE(renderer.Render(desktop, {0, 0, 64, 64}, Snapshot({line}), frame, error)) << error;
    EXPECT_EQ(Pixel(frame, 8, 20), (std::array<std::uint8_t, 4>{0, 0, 255, 255}));
    const AnnotationObject point = Pen({{0, 0}}, 8);
    EXPECT_EQ(Hit(Snapshot({point}), {10, 20}), 1U);
    EXPECT_EQ(Hit(Snapshot({point}), {10, 25}), 0U);
    ASSERT_TRUE(renderer.Render(desktop, {0, 0, 64, 64}, Snapshot({point}), frame, error)) << error;
    EXPECT_EQ(Pixel(frame, 10, 20), (std::array<std::uint8_t, 4>{0, 0, 255, 255}));
    EXPECT_EQ(Pixel(frame, 10, 26), (std::array<std::uint8_t, 4>{255, 255, 255, 255}));
}

// 验证同一笔反复覆盖或自交仍只应用一次透明度，接角与圆帽不叠加变深。
// 入参：无。
// 返回：重复线段内部保持单次 50% source-over。
TEST(AnnotationErasureTest, repeated_pen_segments_do_not_accumulate_alpha)
{
    AnnotationObject object = Pen({{0, 0}, {30, 0}, {0, 0}, {30, 0}}, 8);
    object.style.transparency = 50;
    SelectionOutputRenderer renderer;
    SdrSelectionFrame frame;
    std::wstring error;
    const FrozenDesktopFrame desktop = WhiteDesktop();
    ASSERT_TRUE(renderer.Render(desktop, {0, 0, 64, 64}, Snapshot({object}), frame, error)) << error;
    for (int x : {15, 25, 35})
    {
        EXPECT_NEAR(Pixel(frame, x, 20)[0], 127, 1);
        EXPECT_NEAR(Pixel(frame, x, 20)[1], 127, 1);
        EXPECT_EQ(Pixel(frame, x, 20)[2], 255U);
    }
}

// 验证反向椭圆只绘制轮廓，真实中心空白不被填充或容差错误命中。
// 入参：无。
// 返回：椭圆边界和内部像素及命中断言。
TEST(AnnotationErasureTest, reverse_ellipse_is_outline_with_empty_center)
{
    AnnotationObject object{1, AnnotationKind::Ellipse, {50, 50}, {-40, -40}};
    object.style.lineWidth = 4;
    const AnnotationSnapshot snapshot = Snapshot({object});
    EXPECT_EQ(Hit(snapshot, {30, 10}), 1U);
    EXPECT_EQ(Hit(snapshot, {30, 30}, 3), 0U);
    EXPECT_EQ(Hit(snapshot, {10, 10}, 3), 0U);
    SelectionOutputRenderer renderer;
    SdrSelectionFrame frame;
    std::wstring error;
    const FrozenDesktopFrame desktop = WhiteDesktop();
    ASSERT_TRUE(renderer.Render(desktop, {0, 0, 64, 64}, snapshot, frame, error)) << error;
    EXPECT_EQ(Pixel(frame, 30, 30), (std::array<std::uint8_t, 4>{255, 255, 255, 255}));
    EXPECT_LT(Pixel(frame, 30, 10)[0], 10U);
}

// 验证擦孔在精确与辅助容差命中中均为空，擦痕边缘邻近的孔内点不能选中原线条。
// 入参：无。
// 返回：擦孔输出露出底图，保留段仍可见可选。
TEST(AnnotationErasureTest, erased_hole_is_not_reintroduced_by_hit_tolerance)
{
    AnnotationObject object{1, AnnotationKind::Line, {10, 30}, {40, 0}};
    object.style.lineWidth = 10;
    object = Erased(object, {{30, 30}}, 4);
    const AnnotationSnapshot snapshot = Snapshot({object});
    EXPECT_EQ(Hit(snapshot, {30, 30}, 3), 0U);
    EXPECT_EQ(Hit(snapshot, {32.5, 30}, 3), 0U);
    EXPECT_EQ(Hit(snapshot, {37, 30}), 1U);
    SelectionOutputRenderer renderer;
    SdrSelectionFrame frame;
    std::wstring error;
    const FrozenDesktopFrame desktop = WhiteDesktop();
    ASSERT_TRUE(renderer.Render(desktop, {0, 0, 64, 64}, snapshot, frame, error)) << error;
    EXPECT_EQ(Pixel(frame, 30, 30), (std::array<std::uint8_t, 4>{255, 255, 255, 255}));
    EXPECT_EQ(Pixel(frame, 40, 30), (std::array<std::uint8_t, 4>{0, 0, 255, 255}));
}

// 验证擦除只选择实际有面积交集的对象，不能用轮廓外接框把空心内部判为目标。
// 入参：无。
// 返回：空擦除返回空 ID，遮挡关系不排除下层实际相交对象。
TEST(AnnotationErasureTest, targets_use_visible_geometry_and_include_occluded_objects)
{
    AnnotationObject outline{1, AnnotationKind::Rectangle, {10, 10}, {40, 40}};
    AnnotationObject filled{2, AnnotationKind::FilledRectangle, {20, 20}, {20, 20}};
    AnnotationObject upper = filled;
    upper.id = 3;
    AnnotationObject invisible = filled;
    invisible.id = 4;
    invisible.style.transparency = 100;
    AnnotationObject draft = filled;
    draft.id = 0;
    const AnnotationEraseStroke stroke{{{30, 30}}, 4, {0, 0, 64, 64}};
    std::vector<std::uint64_t> ids;
    std::wstring error;
    ASSERT_TRUE(FindAnnotationEraseTargets(Snapshot({outline}), stroke, ids, error)) << error;
    EXPECT_TRUE(ids.empty());
    ASSERT_TRUE(FindAnnotationEraseTargets(Snapshot({outline, filled, upper, invisible, draft}), stroke, ids, error));
    EXPECT_EQ(ids, (std::vector<std::uint64_t>{2, 3}));
}

// 验证旧擦痕已清除的区域不再产生新目标，重复或更细的空擦除不形成实际编辑。
// 入参：无。
// 返回：重复擦除和位于旧孔内的路径均返回空 ID。
TEST(AnnotationErasureTest, repeated_erasure_does_not_target_already_empty_pixels)
{
    AnnotationObject object{1, AnnotationKind::FilledRectangle, {10, 10}, {40, 40}};
    object = Erased(object, {{30, 30}}, 8);
    std::vector<std::uint64_t> ids;
    std::wstring error;
    ASSERT_TRUE(FindAnnotationEraseTargets(Snapshot({object}), {{{30, 30}}, 8, {0, 0, 64, 64}}, ids, error));
    EXPECT_TRUE(ids.empty());
    ASSERT_TRUE(FindAnnotationEraseTargets(Snapshot({object}), {{{29, 30}, {31, 30}}, 4, {0, 0, 64, 64}}, ids, error));
    EXPECT_TRUE(ids.empty());
}

// 验证擦痕只作用于手势开始时的选区，随后扩大 crop 不会额外擦掉原边界之外的对象。
// 入参：无。
// 返回：擦除圆的选区外部分仍保留原图形及命中资格。
TEST(AnnotationErasureTest, mask_clip_is_fixed_and_does_not_erase_outside_original_crop)
{
    AnnotationObject object{1, AnnotationKind::FilledRectangle, {10, 10}, {40, 40}};
    object = Erased(object, {{20, 30}}, 8, {0, 0, 20, 64});
    const AnnotationSnapshot snapshot = Snapshot({object});
    EXPECT_EQ(Hit(snapshot, {17, 30}), 0U);
    EXPECT_EQ(Hit(snapshot, {23, 30}), 1U);
    SelectionOutputRenderer renderer;
    SdrSelectionFrame frame;
    std::wstring error;
    const FrozenDesktopFrame desktop = WhiteDesktop();
    ASSERT_TRUE(renderer.Render(desktop, {0, 0, 64, 64}, snapshot, frame, error));
    EXPECT_EQ(Pixel(frame, 17, 30), (std::array<std::uint8_t, 4>{255, 255, 255, 255}));
    EXPECT_EQ(Pixel(frame, 23, 30), (std::array<std::uint8_t, 4>{0, 0, 255, 255}));
}

// 验证对象移动时局部擦孔与其固定手势裁剪一起平移，不重新解释旧桌面坐标。
// 入参：无。
// 返回：新位置保留孔与可见区域，旧快照保持不变。
TEST(AnnotationErasureTest, local_mask_moves_with_object_origin)
{
    AnnotationObject object{1, AnnotationKind::FilledRectangle, {5, 5}, {30, 30}};
    object = Erased(object, {{20, 20}}, 4);
    const AnnotationSnapshot original = Snapshot({object});
    object.origin.x += 20;
    object.origin.y += 10;
    const AnnotationSnapshot moved = Snapshot({object});
    EXPECT_EQ(Hit(original, {20, 20}), 0U);
    EXPECT_EQ(Hit(moved, {40, 30}), 0U);
    EXPECT_EQ(Hit(moved, {30, 20}), 1U);
    SelectionOutputRenderer renderer;
    SdrSelectionFrame frame;
    std::wstring error;
    const FrozenDesktopFrame desktop = WhiteDesktop();
    ASSERT_TRUE(renderer.Render(desktop, {0, 0, 64, 64}, moved, frame, error));
    EXPECT_EQ(Pixel(frame, 40, 30), (std::array<std::uint8_t, 4>{255, 255, 255, 255}));
    EXPECT_EQ(Pixel(frame, 30, 20), (std::array<std::uint8_t, 4>{0, 0, 255, 255}));
}

// 验证擦痕跨越负坐标输出拼缝时，分屏原点不改变可见区域及合成结果。
// 入参：无。
// 返回：两侧单独输出逐像素等同于完整区域输出。
TEST(AnnotationErasureTest, erased_geometry_is_consistent_across_output_origins)
{
    AnnotationObject object{1, AnnotationKind::FilledRectangle, {-24, 8}, {48, 40}};
    object = Erased(object, {{-12, 30}, {12, 30}}, 4, {-32, 0, 32, 64});
    const AnnotationSnapshot snapshot = Snapshot({object});
    const FrozenDesktopFrame desktop = WhiteDesktop({-32, 0, 32, 64});
    SelectionOutputRenderer renderer;
    SdrSelectionFrame whole;
    std::wstring error;
    ASSERT_TRUE(renderer.Render(desktop, {-32, 0, 32, 64}, snapshot, whole, error));
    for (int left : {-32, 0})
    {
        SdrSelectionFrame part;
        ASSERT_TRUE(renderer.Render(desktop, {left, 0, left + 32, 64}, snapshot, part, error));
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 32; ++x)
                EXPECT_EQ(Pixel(part, x, y), Pixel(whole, left + 32 + x, y));
    }
}

// 验证超采样预算明确失败并清除旧结果，不能悄悄画未擦版本或留下半成品目标列表。
// 入参：无。
// 返回：路径超预算的输出、命中与擦除查询均拒绝。
TEST(AnnotationErasureTest, oversized_paths_fail_without_unerased_fallback)
{
    AnnotationObject object = Pen(std::vector<AnnotationPoint>(ANNOTATION_MAX_PATH_POINTS + 1, {0, 0}));
    const AnnotationSnapshot snapshot = Snapshot({object});
    SelectionOutputRenderer renderer;
    SdrSelectionFrame output({0, 0, 1, 1}, {1, 2, 3, 4});
    std::wstring error;
    const FrozenDesktopFrame desktop = WhiteDesktop();
    EXPECT_FALSE(renderer.Render(desktop, {0, 0, 64, 64}, snapshot, output, error));
    EXPECT_FALSE(output.IsValid());
    std::uint64_t id = 99;
    EXPECT_FALSE(HitTestAnnotations(snapshot, {0, 0, 64, 64}, {10, 20}, 3, id, error));
    EXPECT_EQ(id, 0U);
    AnnotationEraseStroke stroke{
        std::vector<AnnotationPoint>(ANNOTATION_MAX_PATH_POINTS + 1, {10, 20}), 4, {0, 0, 64, 64}};
    std::vector<std::uint64_t> ids{99};
    EXPECT_FALSE(FindAnnotationEraseTargets(snapshot, stroke, ids, error));
    EXPECT_TRUE(ids.empty());
}

// 验证缓存复用旧擦痕前缀时仍应用新擦痕，样式宽度和点列所有者改变后不复用错误几何。
// 入参：无；连续保留多个不可变版本模拟真实绘制和历史切换。
// 返回：旧孔、新孔及改变后的可见区域分别保持正确。
TEST(AnnotationErasureTest, cached_prefix_preserves_new_masks_and_invalidates_changed_geometry)
{
    const AnnotationObject base = Pen({{0, 0}, {40, 0}}, 8);
    const AnnotationSnapshot baseline = Snapshot({base});
    EXPECT_EQ(Hit(baseline, {20, 20}), 1U);
    const AnnotationObject first = Erased(base, {{20, 20}}, 4);
    const AnnotationSnapshot firstSnapshot = Snapshot({first});
    EXPECT_EQ(Hit(firstSnapshot, {20, 20}), 0U);
    EXPECT_EQ(Hit(firstSnapshot, {35, 20}), 1U);
    AnnotationObject second = Erased(first, {{35, 20}}, 4);
    const AnnotationSnapshot secondSnapshot = Snapshot({second});
    EXPECT_EQ(Hit(secondSnapshot, {20, 20}), 0U);
    EXPECT_EQ(Hit(secondSnapshot, {35, 20}), 0U);
    EXPECT_EQ(Hit(secondSnapshot, {45, 26}), 0U);
    second.style.lineWidth = 16;
    EXPECT_EQ(Hit(Snapshot({second}), {45, 26}), 1U);
    second.payload = AnnotationStroke{
        std::make_shared<const std::vector<AnnotationPoint>>(std::vector<AnnotationPoint>{{0, 0}, {0, 30}})};
    EXPECT_EQ(Hit(Snapshot({second}), {10, 40}), 1U);
    EXPECT_EQ(Hit(Snapshot({second}), {45, 20}), 0U);
    EXPECT_EQ(Hit(firstSnapshot, {35, 20}), 1U);
}

// 验证缓存只弱持有不可变文档数据，不延长已淘汰点列和擦痕向量的生命周期。
// 入参：无；先实际命中以确保几何已经进入缓存。
// 返回：所有宿主快照释放后共享数据的弱引用立即过期。
TEST(AnnotationErasureTest, geometry_cache_does_not_retain_document_payload_or_mask_owners)
{
    std::weak_ptr<const std::vector<AnnotationPoint>> points;
    std::weak_ptr<const std::vector<AnnotationEraseMask>> masks;
    {
        const AnnotationObject object = Erased(Pen({{0, 0}, {40, 0}}), {{20, 20}}, 4);
        points = std::get<AnnotationStroke>(object.payload).points;
        masks = object.erasures;
        const AnnotationSnapshot snapshot = Snapshot({object});
        EXPECT_EQ(Hit(snapshot, {40, 20}), 1U);
        EXPECT_FALSE(points.expired());
        EXPECT_FALSE(masks.expired());
    }
    EXPECT_TRUE(points.expired());
    EXPECT_TRUE(masks.expired());
}
} // namespace open_st
