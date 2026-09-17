// 文件职责：验证统一标注状态中的属性预览、历史提交、拒绝及取消恢复，不创建窗口。
#include "capture_annotation_state.h"
#include <annotation_document.h>
#include <annotation_erasure.h>
#include <array>
#include <gtest/gtest.h>
#include <limits>

namespace open_st
{
namespace
{
constexpr RectI PREVIEW_CROP{0, 0, 300, 300};

// 建立两个已提交对象，再撤销第二对象以准备可恢复的 redo 分支。
// 入参：state：独占测试状态。
// 返回：准备成功为 true，失败由测试断言报告。
bool PreparePreviewState(CaptureAnnotationState& state)
{
    state.SetTool(CaptureAnnotationTool::Rectangle);
    if (!state.BeginDraw({20, 20}, PREVIEW_CROP) || state.EndDraw({100, 100}) != AnnotationCommitResult::Committed ||
        !state.BeginDraw({120, 120}, PREVIEW_CROP) || state.EndDraw({180, 180}) != AnnotationCommitResult::Committed)
    {
        return false;
    }
    RectI restored;
    return state.Restore(false, PREVIEW_CROP, restored);
}
} // namespace

// 验证工具默认线宽比文档要求更严格，同时允许界面预设以外的合法工具值。
// 入参：无。
// 返回：共用基础校验、工具下限以及失败保留默认值的断言。
TEST(CaptureAnnotationStateTest, tool_style_preserves_own_width_range)
{
    CaptureAnnotationState state;
    ASSERT_TRUE(state.SetStyle({0x123456U, 100U, 4.0}));
    EXPECT_FALSE(state.SetStyle({0U, 0U, 0.5}));
    EXPECT_FALSE(state.SetStyle({0x1000000U, 0U, 4.0}));
    EXPECT_FALSE(state.SetStyle({0U, 101U, 4.0}));
    EXPECT_FALSE(state.SetStyle({0U, 0U, 257.0}));
    EXPECT_EQ(state.Style().rgb, 0x123456U);
    EXPECT_EQ(state.Style().transparency, 100U);
    EXPECT_DOUBLE_EQ(state.Style().lineWidth, 4.0);
    EXPECT_TRUE(state.SetStyle({0U, 0U, 1.0}));
    EXPECT_TRUE(state.SetStyle({0U, 0U, 256.0}));
}

// 验证属性预览共用 Preview 且保持 committed/revision，取消后 redo 仍可恢复第二对象。
// 入参：无。
// 返回：预览隔离、活动状态和取消恢复断言。
TEST(CaptureAnnotationStateTest, style_preview_is_unified_and_cancel_preserves_redo)
{
    CaptureAnnotationState state;
    ASSERT_TRUE(PreparePreviewState(state));
    const AnnotationSnapshot original = state.Committed();
    const std::uint64_t revision = state.Revision();
    ASSERT_TRUE(state.BeginObjectPreview(original->front().id, revision));
    EXPECT_TRUE(state.Active());
    EXPECT_TRUE(state.PreviewingObject());
    EXPECT_FALSE(state.Drawing());
    EXPECT_FALSE(state.CanUndo());
    EXPECT_FALSE(state.CanRedo());
    ASSERT_TRUE(state.UpdateObjectPreview({{0x123456U, 35U, 8.0}, {}, {}}));
    const AnnotationSnapshot firstPreview = state.Preview();
    EXPECT_EQ(firstPreview->front().style.rgb, 0x123456U);
    ASSERT_TRUE(state.UpdateObjectPreview({{0x654321U, 100U, 2.0}, {}, {}}));
    EXPECT_EQ(state.Preview()->front().style.transparency, 100U);
    EXPECT_EQ(firstPreview->front().style.rgb, 0x123456U);
    EXPECT_EQ(state.Committed(), original);
    EXPECT_EQ(state.Revision(), revision);
    EXPECT_EQ(state.EndObjectPreview(PREVIEW_CROP, false), AnnotationCommitResult::Unchanged);
    EXPECT_FALSE(state.Active());
    EXPECT_FALSE(state.PreviewingObject());
    EXPECT_EQ(state.Preview(), original);
    EXPECT_TRUE(state.CanRedo());
    RectI restored;
    ASSERT_TRUE(state.Restore(true, PREVIEW_CROP, restored));
    EXPECT_EQ(state.Committed()->size(), 2U);
}

// 验证连续预览后确认只提交一次历史，撤销恢复同一原快照，默认样式不变化。
// 入参：无。
// 返回：单步历史、对象几何和当前默认样式断言。
TEST(CaptureAnnotationStateTest, accepted_style_preview_is_one_edit)
{
    CaptureAnnotationState state;
    ASSERT_TRUE(PreparePreviewState(state));
    const AnnotationSnapshot original = state.Committed();
    const AnnotationStyle defaults = state.Style();
    const std::uint64_t revision = state.Revision();
    ASSERT_TRUE(state.BeginObjectPreview(original->front().id, revision));
    ASSERT_TRUE(state.UpdateObjectPreview({{0x123456U, 35U, 8.0}, {}, {}}));
    ASSERT_TRUE(state.UpdateObjectPreview({{0x654321U, 75U, 2.0}, {}, {}}));
    const AnnotationSnapshot lastPreview = state.Preview();
    EXPECT_EQ(state.EndObjectPreview(PREVIEW_CROP, true), AnnotationCommitResult::Committed);
    EXPECT_EQ(state.Committed(), lastPreview);
    EXPECT_EQ(state.Preview(), lastPreview);
    EXPECT_EQ(state.Revision(), revision + 1U);
    EXPECT_FALSE(state.Active());
    EXPECT_FALSE(state.CanRedo());
    EXPECT_EQ(state.Style().rgb, defaults.rgb);
    EXPECT_EQ(state.Style().transparency, defaults.transparency);
    EXPECT_EQ(state.Style().lineWidth, defaults.lineWidth);
    EXPECT_EQ(lastPreview->front().origin.x, original->front().origin.x);
    EXPECT_EQ(lastPreview->front().extent.x, original->front().extent.x);
    RectI restored;
    ASSERT_TRUE(state.Restore(false, PREVIEW_CROP, restored));
    EXPECT_EQ(state.Committed(), original);
    ASSERT_TRUE(state.Restore(true, PREVIEW_CROP, restored));
    EXPECT_EQ(state.Committed(), lastPreview);
}

// 验证改回原样式共享基线，不创建空历史或清除先前 redo。
// 入参：无。
// 返回：同值确认的修订和历史保持断言。
TEST(CaptureAnnotationStateTest, reverted_style_preview_is_unchanged)
{
    CaptureAnnotationState state;
    ASSERT_TRUE(PreparePreviewState(state));
    const AnnotationSnapshot original = state.Committed();
    const std::uint64_t revision = state.Revision();
    ASSERT_TRUE(state.BeginObjectPreview(original->front().id, revision));
    ASSERT_TRUE(state.UpdateObjectPreview({{0x123456U, 35U, 8.0}, {}, {}}));
    ASSERT_TRUE(state.UpdateObjectPreview(PropertiesOf(original->front())));
    EXPECT_EQ(state.Preview(), original);
    EXPECT_EQ(state.EndObjectPreview(PREVIEW_CROP, true), AnnotationCommitResult::Unchanged);
    EXPECT_EQ(state.Revision(), revision);
    EXPECT_TRUE(state.CanRedo());
    EXPECT_EQ(state.Committed(), original);
}

// 验证失败预览保留上次有效显示但禁止确认，随后有效输入可恢复，空裁剪提交也原子回滚。
// 入参：无。
// 返回：失败候选、恢复输入和错误结束的历史保持断言。
TEST(CaptureAnnotationStateTest, failed_preview_keeps_last_frame_and_can_recover)
{
    CaptureAnnotationState state;
    ASSERT_TRUE(PreparePreviewState(state));
    const AnnotationSnapshot original = state.Committed();
    const std::uint64_t revision = state.Revision();
    ASSERT_TRUE(state.BeginObjectPreview(original->front().id, revision));
    ASSERT_TRUE(state.UpdateObjectPreview({{0x123456U, 35U, 8.0}, {}, {}}));
    const AnnotationSnapshot lastPreview = state.Preview();
    EXPECT_FALSE(state.UpdateObjectPreview({{0U, 101U, 3.0}, {}, {}}));
    EXPECT_EQ(state.Preview(), lastPreview);
    EXPECT_EQ(state.Committed(), original);
    EXPECT_EQ(state.EndObjectPreview(PREVIEW_CROP, true), AnnotationCommitResult::Failed);
    EXPECT_FALSE(state.Active());
    EXPECT_EQ(state.Preview(), original);
    EXPECT_TRUE(state.CanRedo());
    ASSERT_TRUE(state.BeginObjectPreview(original->front().id, revision));
    EXPECT_FALSE(state.UpdateObjectPreview({{0U, 101U, 3.0}, {}, {}}));
    ASSERT_TRUE(state.UpdateObjectPreview({{0x123456U, 35U, 8.0}, {}, {}}));
    EXPECT_EQ(state.EndObjectPreview({}, true), AnnotationCommitResult::Failed);
    EXPECT_EQ(state.Committed(), original);
    EXPECT_EQ(state.Revision(), revision);
    EXPECT_TRUE(state.CanRedo());
}

// 验证零 ID、过期修订和活动事务被拒绝，旧属性调用不能破坏当前预览或绘制草稿。
// 入参：无。
// 返回：事务互斥和拒绝不改变状态的断言。
TEST(CaptureAnnotationStateTest, stale_requests_and_other_edits_do_not_replace_transaction)
{
    CaptureAnnotationState state;
    ASSERT_TRUE(PreparePreviewState(state));
    const AnnotationSnapshot original = state.Committed();
    const std::uint64_t id = original->front().id;
    const std::uint64_t revision = state.Revision();
    EXPECT_FALSE(state.BeginObjectPreview(0U, revision));
    EXPECT_FALSE(state.BeginObjectPreview(id, revision - 1U));
    EXPECT_FALSE(state.BeginObjectPreview(999U, revision));
    ASSERT_TRUE(state.BeginObjectPreview(id, revision));
    ASSERT_TRUE(state.UpdateObjectPreview({{0x123456U, 35U, 8.0}, {}, {}}));
    const AnnotationSnapshot preview = state.Preview();
    EXPECT_FALSE(state.BeginObjectPreview(id, revision));
    EXPECT_FALSE(state.BeginDraw({50, 50}, PREVIEW_CROP));
    EXPECT_FALSE(state.UpdateCrop(PREVIEW_CROP, true));
    EXPECT_FALSE(state.BeginObjectPreview(id, revision, PREVIEW_CROP));
    EXPECT_EQ(state.Preview(), preview);
    EXPECT_TRUE(state.PreviewingObject());
    state.Cancel();
    ASSERT_TRUE(state.BeginDraw({50, 50}, PREVIEW_CROP));
    EXPECT_FALSE(state.BeginObjectPreview(id, revision));
    EXPECT_EQ(state.EndObjectPreview(PREVIEW_CROP, true), AnnotationCommitResult::Failed);
    EXPECT_TRUE(state.Drawing());
    state.Cancel();
    EXPECT_EQ(state.Committed(), original);
}

// 验证 Clear 和 Cancel 均清理属性事务，旧更新或确认不能重新发布被回收的文档。
// 入参：无。
// 返回：预览生命周期、旧调用拒绝和清理结果断言。
TEST(CaptureAnnotationStateTest, clear_invalidates_property_preview_and_late_updates)
{
    CaptureAnnotationState state;
    ASSERT_TRUE(PreparePreviewState(state));
    const AnnotationSnapshot original = state.Committed();
    const std::uint64_t revision = state.Revision();
    ASSERT_TRUE(state.BeginObjectPreview(original->front().id, revision));
    ASSERT_TRUE(state.UpdateObjectPreview({{0x123456U, 35U, 8.0}, {}, {}}));
    state.Clear();
    EXPECT_FALSE(state.Active());
    EXPECT_FALSE(state.PreviewingObject());
    EXPECT_EQ(state.Preview(), nullptr);
    EXPECT_EQ(state.Committed(), nullptr);
    EXPECT_FALSE(state.UpdateObjectPreview({{}, {}, {}}));
    EXPECT_EQ(state.EndObjectPreview(PREVIEW_CROP, true), AnnotationCommitResult::Failed);
    EXPECT_FALSE(state.BeginObjectPreview(original->front().id, revision));
    EXPECT_FALSE(state.CanUndo());
    EXPECT_FALSE(state.CanRedo());
}
// 验证画笔单击生成一个点，重复采样合并，已发布路径保持不可变且一笔只需一次撤销。
// 入参：无。
// 返回：路径载荷、重复点及统一历史断言。
TEST(CaptureAnnotationStateTest, pen_dot_and_path_are_immutable_single_edits)
{
    CaptureAnnotationState state;
    state.SetTool(CaptureAnnotationTool::Pen);
    ASSERT_TRUE(state.BeginDraw({20, 20}, PREVIEW_CROP));
    EXPECT_EQ(state.EndDraw({20, 20}), AnnotationCommitResult::Committed);
    const AnnotationSnapshot dot = state.Committed();
    ASSERT_EQ(dot->size(), 1U);
    EXPECT_EQ(std::get<AnnotationStroke>(dot->front().payload).points->size(), 1U);
    ASSERT_TRUE(state.BeginDraw({40, 40}, PREVIEW_CROP));
    ASSERT_TRUE(state.UpdateDraw({50, 50}));
    const AnnotationSnapshot first = state.Preview();
    ASSERT_TRUE(state.UpdateDraw({50, 50}));
    EXPECT_EQ(std::get<AnnotationStroke>(state.Preview()->back().payload).points->size(), 2U);
    EXPECT_EQ(state.EndDraw({60, 45}), AnnotationCommitResult::Committed);
    EXPECT_EQ(std::get<AnnotationStroke>(state.Committed()->back().payload).points->size(), 3U);
    EXPECT_EQ(std::get<AnnotationStroke>(first->back().payload).points->size(), 2U);
    RectI restored;
    ASSERT_TRUE(state.Restore(false, PREVIEW_CROP, restored));
    EXPECT_EQ(state.Committed(), dot);
    ASSERT_TRUE(state.Restore(true, PREVIEW_CROP, restored));
    EXPECT_EQ(state.Committed()->size(), 2U);
}

// 验证直线和椭圆使用现有两点手势，拒绝退化几何，反向拖动保留有符号终点。
// 入参：无。
// 返回：新形状种类、坐标及透明新建拒绝断言。
TEST(CaptureAnnotationStateTest, line_and_ellipse_follow_existing_gesture_contract)
{
    CaptureAnnotationState state;
    state.SetTool(CaptureAnnotationTool::Line);
    ASSERT_TRUE(state.BeginDraw({50, 50}, PREVIEW_CROP));
    EXPECT_EQ(state.EndDraw({50, 50}), AnnotationCommitResult::Unchanged);
    ASSERT_TRUE(state.BeginDraw({50, 50}, PREVIEW_CROP));
    EXPECT_EQ(state.EndDraw({20, 50}), AnnotationCommitResult::Committed);
    EXPECT_EQ(state.Committed()->back().kind, AnnotationKind::Line);
    EXPECT_EQ(state.Committed()->back().extent.x, -30.0);
    state.SetTool(CaptureAnnotationTool::Ellipse);
    ASSERT_TRUE(state.BeginDraw({80, 80}, PREVIEW_CROP));
    EXPECT_EQ(state.EndDraw({40, 30}), AnnotationCommitResult::Committed);
    EXPECT_EQ(state.Committed()->back().kind, AnnotationKind::Ellipse);
    EXPECT_EQ(state.Committed()->back().extent.y, -50.0);
    ASSERT_TRUE(state.SetStyle({0x123456U, 100U, 3.0}));
    EXPECT_FALSE(state.BeginDraw({20, 20}, PREVIEW_CROP));
}

// 验证一次擦除作用于所有叠放对象，每次移动从基线更新同一轨迹，一次撤销恢复整笔。
// 入参：无。
// 返回：共享轨迹、每对象单条掩码及原子历史断言。
TEST(CaptureAnnotationStateTest, eraser_sweeps_all_layers_in_one_baseline_transaction)
{
    CaptureAnnotationState state;
    state.SetTool(CaptureAnnotationTool::FilledRectangle);
    ASSERT_TRUE(state.BeginDraw({20, 20}, PREVIEW_CROP));
    ASSERT_EQ(state.EndDraw({120, 120}), AnnotationCommitResult::Committed);
    ASSERT_TRUE(state.BeginDraw({40, 40}, PREVIEW_CROP));
    ASSERT_EQ(state.EndDraw({140, 140}), AnnotationCommitResult::Committed);
    const AnnotationSnapshot baseline = state.Committed();
    state.SetTool(CaptureAnnotationTool::Eraser);
    EXPECT_EQ(state.EraserDiameter(), 16U);
    ASSERT_TRUE(state.SetEraserDiameter(32U));
    ASSERT_TRUE(state.BeginDraw({60, 60}, PREVIEW_CROP));
    EXPECT_TRUE(state.Drawing());
    EXPECT_FALSE(state.SetEraserDiameter(64U));
    ASSERT_TRUE(state.UpdateDraw({65, 65}));
    ASSERT_TRUE(state.UpdateDraw({70, 70}));
    EXPECT_EQ(state.Committed(), baseline);
    const AnnotationSnapshot preview = state.Preview();
    ASSERT_EQ(preview->size(), 2U);
    ASSERT_NE(preview->front().erasures, nullptr);
    ASSERT_NE(preview->back().erasures, nullptr);
    EXPECT_EQ(preview->front().erasures->size(), 1U);
    EXPECT_EQ(preview->back().erasures->size(), 1U);
    EXPECT_EQ(preview->front().erasures->front().stroke, preview->back().erasures->front().stroke);
    EXPECT_EQ(preview->front().erasures->front().stroke->points.size(), 3U);
    EXPECT_EQ(preview->front().erasures->front().stroke->radius, 16.0);
    EXPECT_EQ(state.EndDraw({70, 70}), AnnotationCommitResult::Committed);
    EXPECT_FALSE(state.Drawing());
    RectI restored;
    ASSERT_TRUE(state.Restore(false, PREVIEW_CROP, restored));
    EXPECT_EQ(state.Committed(), baseline);
    ASSERT_TRUE(state.Restore(true, PREVIEW_CROP, restored));
    EXPECT_EQ(state.Committed()->front().erasures->size(), 1U);
}

// 验证空擦不改变历史，取消有效擦除恢复全部对象，橡皮不受绘图透明度限制。
// 入参：无。
// 返回：空擦、取消、独立大小和 redo 保留断言。
TEST(CaptureAnnotationStateTest, empty_and_cancelled_erase_preserve_redo_and_defaults)
{
    CaptureAnnotationState state;
    ASSERT_TRUE(PreparePreviewState(state));
    const AnnotationSnapshot original = state.Committed();
    const std::uint64_t revision = state.Revision();
    state.SetTool(CaptureAnnotationTool::Eraser);
    ASSERT_TRUE(state.SetStyle({0U, 100U, 3.0}));
    EXPECT_FALSE(state.SetEraserDiameter(12U));
    EXPECT_EQ(state.EraserDiameter(), 16U);
    ASSERT_TRUE(state.BeginDraw({250, 250}, PREVIEW_CROP));
    EXPECT_EQ(state.EndDraw({260, 260}), AnnotationCommitResult::Unchanged);
    EXPECT_TRUE(state.CanRedo());
    EXPECT_EQ(state.Revision(), revision);
    ASSERT_TRUE(state.BeginDraw({20, 50}, PREVIEW_CROP));
    ASSERT_NE(state.Preview()->front().erasures, nullptr);
    state.Cancel();
    EXPECT_EQ(state.Preview(), original);
    EXPECT_EQ(state.Committed(), original);
    EXPECT_TRUE(state.CanRedo());
    EXPECT_EQ(state.Revision(), revision);
}

// 验证擦痕随对象平移且裁剪记录随之移动；后续新对象不继承旧擦痕。
// 入参：无。
// 返回：局部引用、平移与新建隔离断言。
TEST(CaptureAnnotationStateTest, erased_content_moves_with_crop_and_new_objects_remain_clean)
{
    CaptureAnnotationState state;
    state.SetTool(CaptureAnnotationTool::FilledRectangle);
    ASSERT_TRUE(state.BeginDraw({20, 20}, PREVIEW_CROP));
    ASSERT_EQ(state.EndDraw({120, 120}), AnnotationCommitResult::Committed);
    state.SetTool(CaptureAnnotationTool::Eraser);
    ASSERT_TRUE(state.BeginDraw({60, 60}, PREVIEW_CROP));
    ASSERT_EQ(state.EndDraw({60, 60}), AnnotationCommitResult::Committed);
    const AnnotationSnapshot erased = state.Committed();
    state.BeginCrop(PREVIEW_CROP);
    const RectI moved{50, 60, 350, 360};
    ASSERT_TRUE(state.UpdateCrop(moved, true));
    ASSERT_EQ(state.EndCrop(moved), AnnotationCommitResult::Committed);
    const AnnotationObject& shifted = state.Committed()->front();
    EXPECT_EQ(shifted.erasures, erased->front().erasures);
    const AnnotationEraseMask& mask = shifted.erasures->front();
    EXPECT_EQ(mask.stroke->points.front().x + mask.offset.x + shifted.origin.x, 110.0);
    EXPECT_EQ(mask.stroke->points.front().y + mask.offset.y + shifted.origin.y, 120.0);
    state.SetTool(CaptureAnnotationTool::Pen);
    ASSERT_TRUE(state.BeginDraw({110, 120}, moved));
    ASSERT_EQ(state.EndDraw({110, 120}), AnnotationCommitResult::Committed);
    EXPECT_EQ(state.Committed()->back().erasures, nullptr);
}

// 验证 8192 点上限拒绝新采样，整笔失败恢复开始文档与已有 redo，不静默截短笔迹。
// 入参：无。
// 返回：采样上限、不可变预览及失败回滚断言。
TEST(CaptureAnnotationStateTest, pen_point_budget_failure_rolls_back_entire_gesture)
{
    CaptureAnnotationState state;
    ASSERT_TRUE(PreparePreviewState(state));
    const AnnotationSnapshot original = state.Committed();
    state.SetTool(CaptureAnnotationTool::Pen);
    ASSERT_TRUE(state.BeginDraw({20, 20}, PREVIEW_CROP));
    for (std::size_t index = 1U; index < ANNOTATION_MAX_PATH_POINTS; ++index)
        ASSERT_TRUE(state.UpdateDraw({20 + static_cast<int>(index), 20}));
    const AnnotationSnapshot maximum = state.Preview();
    EXPECT_EQ(std::get<AnnotationStroke>(maximum->back().payload).points->size(), ANNOTATION_MAX_PATH_POINTS);
    EXPECT_FALSE(state.UpdateDraw({9000, 20}));
    EXPECT_EQ(state.Preview(), maximum);
    EXPECT_EQ(state.EndDraw({9000, 20}), AnnotationCommitResult::Failed);
    EXPECT_FALSE(state.Active());
    EXPECT_EQ(state.Committed(), original);
    EXPECT_EQ(state.Preview(), original);
    EXPECT_TRUE(state.CanRedo());
}
// 验证增量线段查询与整条轨迹参考查询作用于同一基线时得到相同目标和完整擦痕。
// 入参：无。
// 返回：跨出选区、回原点、旧擦痕及叠放下层的等价断言。
TEST(CaptureAnnotationStateTest, incremental_erase_matches_full_trace_on_fixed_baseline)
{
    CaptureAnnotationState state;
    state.SetTool(CaptureAnnotationTool::FilledRectangle);
    ASSERT_TRUE(state.BeginDraw({20, 20}, PREVIEW_CROP));
    ASSERT_EQ(state.EndDraw({120, 120}), AnnotationCommitResult::Committed);
    ASSERT_TRUE(state.BeginDraw({40, 40}, PREVIEW_CROP));
    ASSERT_EQ(state.EndDraw({140, 140}), AnnotationCommitResult::Committed);
    state.SetTool(CaptureAnnotationTool::Eraser);
    ASSERT_TRUE(state.BeginDraw({80, 80}, PREVIEW_CROP));
    ASSERT_EQ(state.EndDraw({80, 80}), AnnotationCommitResult::Committed);
    const AnnotationSnapshot baseline = state.Committed();
    const std::array<PointI, 7U> samples{{{60, 60}, {60, 60}, {320, 60}, {320, -20}, {60, -20}, {60, 60}, {260, 260}}};
    ASSERT_TRUE(state.BeginDraw(samples.front(), PREVIEW_CROP));
    const AnnotationSnapshot firstPreview = state.Preview();
    ASSERT_TRUE(state.UpdateDraw(samples[1U]));
    EXPECT_EQ(state.Preview(), firstPreview);
    for (std::size_t index = 2U; index < samples.size(); ++index)
        ASSERT_TRUE(state.UpdateDraw(samples[index]));
    const AnnotationSnapshot lastPreview = state.Preview();
    ASSERT_EQ(state.EndDraw(samples.back()), AnnotationCommitResult::Committed);
    EXPECT_EQ(state.Committed(), lastPreview);

    AnnotationEraseStroke reference;
    reference.radius = 8.0;
    reference.clip = {0.0, 0.0, 300.0, 300.0};
    for (const PointI point : samples)
    {
        if (reference.points.empty() || reference.points.back().x != point.x || reference.points.back().y != point.y)
            reference.points.push_back({static_cast<double>(point.x), static_cast<double>(point.y)});
    }
    std::vector<std::uint64_t> targets;
    std::wstring error;
    ASSERT_TRUE(FindAnnotationEraseTargets(baseline, reference, targets, error)) << error;
    ASSERT_EQ(targets.size(), 2U);
    AnnotationSnapshot expected;
    ASSERT_TRUE(
        ApplyAnnotationErase(baseline, targets, std::make_shared<const AnnotationEraseStroke>(reference), expected));
    ASSERT_EQ(state.Committed()->size(), expected->size());
    for (std::size_t index = 0U; index < expected->size(); ++index)
    {
        const AnnotationObject& actualObject = (*state.Committed())[index];
        const AnnotationObject& expectedObject = (*expected)[index];
        EXPECT_EQ(actualObject.id, expectedObject.id);
        ASSERT_NE(actualObject.erasures, nullptr);
        ASSERT_EQ(actualObject.erasures->size(), expectedObject.erasures->size());
        const AnnotationEraseMask& actualMask = actualObject.erasures->back();
        const AnnotationEraseMask& expectedMask = expectedObject.erasures->back();
        EXPECT_EQ(actualMask.offset.x, expectedMask.offset.x);
        EXPECT_EQ(actualMask.offset.y, expectedMask.offset.y);
        EXPECT_EQ(actualMask.stroke->radius, expectedMask.stroke->radius);
        EXPECT_EQ(actualMask.stroke->clip.left, expectedMask.stroke->clip.left);
        EXPECT_EQ(actualMask.stroke->clip.right, expectedMask.stroke->clip.right);
        EXPECT_EQ(actualMask.stroke->clip.top, expectedMask.stroke->clip.top);
        EXPECT_EQ(actualMask.stroke->clip.bottom, expectedMask.stroke->clip.bottom);
        ASSERT_EQ(actualMask.stroke->points.size(), expectedMask.stroke->points.size());
        for (std::size_t sample = 0U; sample < actualMask.stroke->points.size(); ++sample)
        {
            EXPECT_EQ(actualMask.stroke->points[sample].x, expectedMask.stroke->points[sample].x);
            EXPECT_EQ(actualMask.stroke->points[sample].y, expectedMask.stroke->points[sample].y);
        }
    }
}

// 验证取消和提交均清理累计命中 ID，下一次空擦不能给旧目标添加无关掩码。
// 入参：无。
// 返回：跨事务隔离、空擦复用文档及同点复用预览断言。
TEST(CaptureAnnotationStateTest, incremental_erase_targets_do_not_survive_cancel_or_commit)
{
    CaptureAnnotationState state;
    state.SetTool(CaptureAnnotationTool::FilledRectangle);
    ASSERT_TRUE(state.BeginDraw({20, 20}, PREVIEW_CROP));
    ASSERT_EQ(state.EndDraw({100, 100}), AnnotationCommitResult::Committed);
    state.SetTool(CaptureAnnotationTool::Eraser);
    ASSERT_TRUE(state.BeginDraw({50, 50}, PREVIEW_CROP));
    const AnnotationSnapshot hitPreview = state.Preview();
    ASSERT_TRUE(state.UpdateDraw({50, 50}));
    EXPECT_EQ(state.Preview(), hitPreview);
    state.Cancel();
    const AnnotationSnapshot original = state.Committed();
    ASSERT_TRUE(state.BeginDraw({250, 250}, PREVIEW_CROP));
    EXPECT_EQ(state.EndDraw({260, 260}), AnnotationCommitResult::Unchanged);
    EXPECT_EQ(state.Committed(), original);
    ASSERT_TRUE(state.BeginDraw({50, 50}, PREVIEW_CROP));
    ASSERT_EQ(state.EndDraw({50, 50}), AnnotationCommitResult::Committed);
    const AnnotationSnapshot erased = state.Committed();
    ASSERT_TRUE(state.BeginDraw({250, 250}, PREVIEW_CROP));
    EXPECT_EQ(state.EndDraw({260, 260}), AnnotationCommitResult::Unchanged);
    EXPECT_EQ(state.Committed(), erased);
    EXPECT_EQ(erased->front().erasures->size(), 1U);
}

// 验证增量查询失败立即回滚整笔并清理目标缓存，后续新手势不携带旧命中。
// 入参：无。
// 返回：非法桌面坐标失败、原文档和 redo 恢复及下一笔空擦断言。
TEST(CaptureAnnotationStateTest, incremental_erase_failure_clears_transaction_targets)
{
    CaptureAnnotationState state;
    ASSERT_TRUE(PreparePreviewState(state));
    const AnnotationSnapshot original = state.Committed();
    state.SetTool(CaptureAnnotationTool::Eraser);
    ASSERT_TRUE(state.BeginDraw({20, 50}, PREVIEW_CROP));
    ASSERT_NE(state.Preview()->front().erasures, nullptr);
    EXPECT_FALSE(state.UpdateDraw({std::numeric_limits<int>::min(), 50}));
    EXPECT_FALSE(state.Active());
    EXPECT_FALSE(state.Drawing());
    EXPECT_EQ(state.Committed(), original);
    EXPECT_EQ(state.Preview(), original);
    EXPECT_TRUE(state.CanRedo());
    ASSERT_TRUE(state.BeginDraw({250, 250}, PREVIEW_CROP));
    EXPECT_EQ(state.EndDraw({260, 260}), AnnotationCommitResult::Unchanged);
    EXPECT_EQ(state.Committed(), original);
    EXPECT_TRUE(state.CanRedo());
}
// 验证新文字统一换行、编辑期间不双绘，确认后一次历史且空白新建不提交。
// 入参：无。
// 返回：正文、预览隔离、一次撤销和空白语义断言。
TEST(CaptureAnnotationStateTest, text_creation_normalizes_and_commits_once)
{
    CaptureAnnotationState state;
    state.SetTool(CaptureAnnotationTool::Text);
    EXPECT_EQ(state.TextFontSize(), 24U);
    EXPECT_FALSE(state.SetTextFontSize(13U));
    ASSERT_TRUE(state.SetTextFontSize(32U));
    ASSERT_TRUE(state.BeginText({20, 30}, PREVIEW_CROP));
    EXPECT_TRUE(state.Active());
    EXPECT_TRUE(state.EditingText());
    EXPECT_FALSE(state.Drawing());
    EXPECT_FALSE(state.CanCommitText());
    ASSERT_TRUE(state.UpdateText(u"第一行\r\n第二行\r三"));
    EXPECT_TRUE(state.CanCommitText());
    EXPECT_EQ(state.Committed(), nullptr);
    EXPECT_EQ(state.Preview(), nullptr);
    EXPECT_EQ(*std::get<AnnotationText>(state.TextDraft()->payload).text, u"第一行\n第二行\n三");
    ASSERT_EQ(state.EndText(true), AnnotationCommitResult::Committed);
    ASSERT_EQ(state.Committed()->size(), 1U);
    EXPECT_EQ(std::get<AnnotationText>(state.Committed()->front().payload).fontSize, 32.0);
    const AnnotationSnapshot saved = state.Committed();
    RectI restored;
    ASSERT_TRUE(state.Restore(false, PREVIEW_CROP, restored));
    EXPECT_EQ(state.Committed(), nullptr);
    ASSERT_TRUE(state.Restore(true, PREVIEW_CROP, restored));
    EXPECT_EQ(state.Committed(), saved);
    ASSERT_TRUE(state.BeginText({30, 30}, PREVIEW_CROP));
    ASSERT_TRUE(state.UpdateText(u" \r\n\u3000"));
    EXPECT_EQ(state.EndText(true), AnnotationCommitResult::Unchanged);
    EXPECT_EQ(state.Committed(), saved);
}

// 验证已有文字编辑隐藏原对象，CRLF 同值不增历史，空白候选拒绝而保留继续编辑。
// 入参：无。
// 返回：目标隐藏、同值、空白拒绝与取消恢复断言。
TEST(CaptureAnnotationStateTest, existing_text_blank_is_rejected_without_deleting)
{
    CaptureAnnotationState state;
    state.SetTool(CaptureAnnotationTool::Text);
    ASSERT_TRUE(state.BeginText({20, 30}, PREVIEW_CROP));
    ASSERT_TRUE(state.UpdateText(u"甲\n乙"));
    ASSERT_EQ(state.EndText(true), AnnotationCommitResult::Committed);
    const AnnotationSnapshot original = state.Committed();
    const std::uint64_t revision = state.Revision();
    ASSERT_TRUE(state.BeginTextEdit(original->front().id, revision, PREVIEW_CROP));
    ASSERT_NE(state.Preview(), nullptr);
    EXPECT_TRUE(state.Preview()->empty());
    ASSERT_TRUE(state.UpdateText(u"甲\r\n乙"));
    EXPECT_EQ(state.EndText(true), AnnotationCommitResult::Unchanged);
    EXPECT_EQ(state.Committed(), original);
    EXPECT_EQ(state.Revision(), revision);
    ASSERT_TRUE(state.BeginTextEdit(original->front().id, revision, PREVIEW_CROP));
    ASSERT_TRUE(state.UpdateText(u"\t \r\n"));
    EXPECT_FALSE(state.CanCommitText());
    EXPECT_EQ(state.EndText(true), AnnotationCommitResult::Failed);
    EXPECT_TRUE(state.EditingText());
    EXPECT_EQ(state.Committed(), original);
    ASSERT_TRUE(state.UpdateText(u"恢复后的正文"));
    EXPECT_EQ(state.EndText(false), AnnotationCommitResult::Unchanged);
    EXPECT_EQ(state.Preview(), original);
}

// 验证非法和超限正文保留上次草稿但不能提交旧候选，后续有效输入可恢复并保持事务互斥。
// 入参：无。
// 返回：UTF-16 失败、旧修订及提交恢复断言。
TEST(CaptureAnnotationStateTest, text_invalid_input_blocks_old_candidate_and_recovers)
{
    CaptureAnnotationState state;
    state.SetTool(CaptureAnnotationTool::Text);
    ASSERT_TRUE(state.BeginText({20, 30}, PREVIEW_CROP));
    ASSERT_TRUE(state.UpdateText(u"有效"));
    EXPECT_FALSE(state.UpdateText(std::u16string(8193U, u'A')));
    EXPECT_FALSE(state.CanCommitText());
    EXPECT_EQ(*std::get<AnnotationText>(state.TextDraft()->payload).text, u"有效");
    EXPECT_EQ(state.EndText(true), AnnotationCommitResult::Failed);
    EXPECT_TRUE(state.EditingText());
    EXPECT_FALSE(state.BeginDraw({50, 50}, PREVIEW_CROP));
    ASSERT_TRUE(state.UpdateText(u"有效\r\n恢复"));
    ASSERT_EQ(state.EndText(true), AnnotationCommitResult::Committed);
    const AnnotationSnapshot original = state.Committed();
    EXPECT_FALSE(state.BeginTextEdit(original->front().id, state.Revision() - 1U, PREVIEW_CROP));
    ASSERT_TRUE(state.BeginTextEdit(original->front().id, state.Revision(), PREVIEW_CROP));
    state.Clear();
    EXPECT_FALSE(state.UpdateText(u"旧消息"));
    EXPECT_FALSE(state.EditingText());
    EXPECT_EQ(state.EndText(true), AnnotationCommitResult::Failed);
}

// 验证文字字号参数复用通用预览，颜色修改保留当前字号且确认仍是一条历史。
// 入参：无。
// 返回：完整参数更新、正文共享、撤销恢复断言。
TEST(CaptureAnnotationStateTest, object_style_and_font_parameters_share_one_transaction)
{
    CaptureAnnotationState state;
    state.SetTool(CaptureAnnotationTool::Text);
    ASSERT_TRUE(state.BeginText({20, 30}, PREVIEW_CROP));
    ASSERT_TRUE(state.UpdateText(u"共享正文"));
    ASSERT_EQ(state.EndText(true), AnnotationCommitResult::Committed);
    const AnnotationSnapshot original = state.Committed();
    ASSERT_TRUE(state.BeginObjectPreview(original->front().id, state.Revision(), PREVIEW_CROP));
    AnnotationProperties properties = PropertiesOf(original->front());
    properties.fontSize = 48.0;
    ASSERT_TRUE(state.UpdateObjectPreview(properties));
    properties.style = {0x123456U, 50U, 3.0};
    ASSERT_TRUE(state.UpdateObjectPreview(properties));
    EXPECT_EQ(std::get<AnnotationText>(state.Preview()->front().payload).fontSize, 48.0);
    EXPECT_EQ(std::get<AnnotationText>(state.Preview()->front().payload).text,
              std::get<AnnotationText>(original->front().payload).text);
    ASSERT_EQ(state.EndObjectPreview(PREVIEW_CROP, true), AnnotationCommitResult::Committed);
    RectI restored;
    ASSERT_TRUE(state.Restore(false, PREVIEW_CROP, restored));
    EXPECT_EQ(state.Committed(), original);
}

// 验证马赛克强制不透明且支持四档默认值，准备失败不发布或提交旧候选，恢复可继续。
// 入参：无。
// 返回：准备回调、真实目标裁剪、失败与重试断言。
TEST(CaptureAnnotationStateTest, mosaic_preparation_failure_keeps_previous_preview_until_retry)
{
    CaptureAnnotationState state;
    bool ready = true;
    unsigned calls{};
    // 模拟来源准备，断言回调得到真实手势裁剪与马赛克候选。
    // 入参：candidate：拟发布文档；selection：目标裁剪。
    // 返回：ready 控制成功或失败。
    ASSERT_TRUE(state.SetPreviewPreparation(
        [&ready, &calls](const AnnotationSnapshot& candidate, RectI selection)
        {
            ++calls;
            EXPECT_EQ(selection.left, 0);
            EXPECT_EQ(selection.right, 300);
            EXPECT_EQ(candidate->back().kind, AnnotationKind::Mosaic);
            return ready;
        }));
    state.SetTool(CaptureAnnotationTool::Mosaic);
    ASSERT_TRUE(state.SetStyle({0U, 100U, 3.0}));
    EXPECT_EQ(state.MosaicBlockSize(), 16U);
    EXPECT_FALSE(state.SetMosaicBlockSize(10U));
    ASSERT_TRUE(state.SetMosaicBlockSize(32U));
    ASSERT_TRUE(state.BeginDraw({20, 30}, PREVIEW_CROP));
    ASSERT_TRUE(state.UpdateDraw({80, 90}));
    const AnnotationSnapshot good = state.Preview();
    EXPECT_EQ(good->back().style.transparency, 0U);
    EXPECT_EQ(std::get<AnnotationMosaic>(good->back().payload).blockSize, 32U);
    ready = false;
    EXPECT_FALSE(state.UpdateDraw({120, 130}));
    EXPECT_EQ(state.Preview(), good);
    EXPECT_EQ(state.EndDraw({120, 130}), AnnotationCommitResult::Failed);
    EXPECT_TRUE(state.Drawing());
    EXPECT_EQ(state.Committed(), nullptr);
    ready = true;
    ASSERT_TRUE(state.UpdateDraw({100, 110}));
    ASSERT_EQ(state.EndDraw({100, 110}), AnnotationCommitResult::Committed);
    EXPECT_EQ(state.Committed()->back().extent.x, 80.0);
    EXPECT_GT(calls, 0U);
}

// 验证参数和选区候选准备失败保持旧预览，撤销准备使用恢复目标而非当前裁剪。
// 入参：无。
// 返回：属性失败、平移失败和历史目标裁剪断言。
TEST(CaptureAnnotationStateTest, mosaic_parameters_crop_and_restore_prepare_their_target_selection)
{
    CaptureAnnotationState state;
    state.SetTool(CaptureAnnotationTool::Mosaic);
    ASSERT_TRUE(state.BeginDraw({20, 30}, PREVIEW_CROP));
    ASSERT_EQ(state.EndDraw({80, 90}), AnnotationCommitResult::Committed);
    const AnnotationSnapshot original = state.Committed();
    bool ready = true;
    RectI prepared{};
    // 记录每次准备的正式目标裁剪，以校验移动和撤销路径。
    // 入参：未命名参数为候选；selection 为目标裁剪。
    // 返回：ready。
    ASSERT_TRUE(state.SetPreviewPreparation(
        [&ready, &prepared](const AnnotationSnapshot&, RectI selection)
        {
            prepared = selection;
            return ready;
        }));
    ASSERT_TRUE(state.BeginObjectPreview(original->front().id, state.Revision(), PREVIEW_CROP));
    AnnotationProperties properties = PropertiesOf(original->front());
    properties.blockSize = 4U;
    ready = false;
    EXPECT_FALSE(state.UpdateObjectPreview(properties));
    EXPECT_EQ(state.Preview(), original);
    EXPECT_EQ(state.EndObjectPreview(PREVIEW_CROP, true), AnnotationCommitResult::Failed);
    EXPECT_EQ(state.Committed(), original);
    ready = true;
    const RectI moved{50, 60, 350, 360};
    state.BeginCrop(PREVIEW_CROP);
    ASSERT_TRUE(state.UpdateCrop(moved, true));
    EXPECT_EQ(prepared.left, 50);
    ASSERT_EQ(state.EndCrop(moved), AnnotationCommitResult::Committed);
    RectI restored;
    ASSERT_TRUE(state.Restore(false, moved, restored));
    EXPECT_EQ(prepared.left, PREVIEW_CROP.left);
    EXPECT_EQ(restored.left, PREVIEW_CROP.left);
    EXPECT_EQ(state.Committed(), original);
    state.BeginCrop(PREVIEW_CROP);
    ready = false;
    EXPECT_FALSE(state.UpdateCrop(moved, true));
    EXPECT_EQ(state.EndCrop(moved), AnnotationCommitResult::Failed);
    EXPECT_EQ(state.Committed(), original);
    EXPECT_TRUE(state.CanRedo());
}

// 验证没有马赛克的既有形状和文字路径不调用来源准备钩子。
// 入参：无。
// 返回：普通编辑在失败钩子下仍可提交的断言。
TEST(CaptureAnnotationStateTest, source_preparation_is_skipped_for_non_mosaic_documents)
{
    CaptureAnnotationState state;
    unsigned calls{};
    // 若被错误调用则返回失败，以证明非马赛克路径无准备依赖。
    // 入参：未命名参数为候选文档和目标裁剪。
    // 返回：false。
    ASSERT_TRUE(state.SetPreviewPreparation(
        [&calls](const AnnotationSnapshot&, RectI)
        {
            ++calls;
            return false;
        }));
    state.SetTool(CaptureAnnotationTool::Line);
    ASSERT_TRUE(state.BeginDraw({20, 20}, PREVIEW_CROP));
    ASSERT_EQ(state.EndDraw({50, 50}), AnnotationCommitResult::Committed);
    state.SetTool(CaptureAnnotationTool::Text);
    ASSERT_TRUE(state.BeginText({20, 20}, PREVIEW_CROP));
    ASSERT_TRUE(state.UpdateText(u"不调用来源准备"));
    ASSERT_EQ(state.EndText(true), AnnotationCommitResult::Committed);
    EXPECT_EQ(calls, 0U);
}
} // namespace open_st
