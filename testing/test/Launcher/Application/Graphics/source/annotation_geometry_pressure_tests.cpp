// 有限测量密集单调笔迹与八条旧擦痕的几何成本，记录墙钟时间而不推算显示帧率。

#include <annotation_erasure.h>
#include <gtest/gtest.h>
#include <selection_output_renderer.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace open_st
{
namespace
{
using Clock = std::chrono::steady_clock;

// 生成固定边界内的平滑单调曲线，增加采样密度而不引入组合复杂度不可控的密集自交。
// 入参：count 为实际采样点数。
// 返回：桌面物理坐标点列，首末横坐标为 8 和 520。
std::vector<AnnotationPoint> DensePath(std::size_t count)
{
    std::vector<AnnotationPoint> points;
    points.reserve(count);
    for (std::size_t index = 0; index < count; ++index)
    {
        const double x = 8.0 + 512.0 * static_cast<double>(index) / static_cast<double>(count - 1);
        points.push_back({x, 32.0 + 8.0 * std::sin(x / 32.0)});
    }
    return points;
}

// 创建有限白色 SDR 画布，将测量集中在标注几何而非全屏像素分配。
// 入参：无。
// 返回：544×96 白色冻结图像，不捕获真实桌面。
FrozenDesktopFrame Desktop()
{
    std::vector<CapturedOutputPlane> planes;
    planes.emplace_back(RectI{0, 0, 544, 96}, CapturedPixelFormat::Bgra8Unorm, CapturedColorSpace::SdrGamma22P709,
                        OutputColorMetadata{}, std::vector<std::uint8_t>(544U * 96U * 4U, 255));
    return {{0, 0, 544, 96}, std::move(planes)};
}

// 查询完整输出的指定相对像素。
// 入参：frame 为有效输出；x、y 为画布内坐标。
// 返回：BGRA 四字节。
std::array<std::uint8_t, 4> Pixel(const SdrSelectionFrame& frame, int x, int y)
{
    const std::size_t offset = static_cast<std::size_t>(y) * frame.Stride() + static_cast<std::size_t>(x) * 4U;
    return {frame.Pixels()[offset], frame.Pixels()[offset + 1], frame.Pixels()[offset + 2], frame.Pixels()[offset + 3]};
}
} // namespace

// 先执行 32 点预检，再测量 4096/8192 点笔迹加八条旧擦痕，避免未验证简单几何就进入高密度运算。
// 入参：无；CTest 单用例总限时 45 秒，仅记录查询与实际输出墙钟时间，不设置 FPS 或机器性能通过线。
// 返回：有限图像、目标与快照完整性必须成立；超出剩余执行预算时停止扩大压力并报告。
TEST(AnnotationGeometryPressureTest, dense_smooth_paths_with_eight_prior_erasures)
{
    const Clock::time_point started = Clock::now();
    const FrozenDesktopFrame desktop = Desktop();
    SelectionOutputRenderer renderer;
    for (std::size_t count : {32U, 4096U, 8192U})
    {
        if (Clock::now() - started > std::chrono::seconds(30))
        {
            ADD_FAILURE() << "Preceding case consumed the bounded execution budget; larger path was not started.";
            return;
        }
        const auto points = std::make_shared<const std::vector<AnnotationPoint>>(DensePath(count));
        AnnotationObject pen{1, AnnotationKind::Pen, {}, {}};
        pen.style.lineWidth = 8;
        pen.payload = AnnotationStroke{points};
        std::vector<AnnotationEraseMask> masks;
        for (int index = 1; index <= 8; ++index)
        {
            const double x = static_cast<double>(index * 64);
            const auto stroke = std::make_shared<const AnnotationEraseStroke>(
                AnnotationEraseStroke{{{x, 0}, {x, 80}}, 4, {0, 0, 544, 96}});
            masks.push_back({stroke, {}});
        }
        pen.erasures = std::make_shared<const std::vector<AnnotationEraseMask>>(std::move(masks));
        // 新对象在第一条旧擦痕覆盖区域内，必须保持绿色，不继承旧对象的擦痕。
        AnnotationObject added{2, AnnotationKind::FilledRectangle, {62, 36}, {4, 6}};
        added.style.rgb = 0x00FF00;
        const AnnotationSnapshot snapshot =
            std::make_shared<const std::vector<AnnotationObject>>(std::vector<AnnotationObject>{pen, added});
        const AnnotationEraseStroke next{*points, 4, {0, 0, 544, 96}};
        std::printf("M2 geometry pressure: points=%zu old_masks=8 canvas=544x96 starting query\n", count);
        std::fflush(stdout);
        std::vector<std::uint64_t> ids;
        std::wstring error;
        const Clock::time_point queryStarted = Clock::now();
        ASSERT_TRUE(FindAnnotationEraseTargets(snapshot, next, ids, error)) << error;
        const double queryMs = std::chrono::duration<double, std::milli>(Clock::now() - queryStarted).count();
        EXPECT_EQ(ids, (std::vector<std::uint64_t>{1, 2}));
        const Clock::time_point hotQueryStarted = Clock::now();
        ASSERT_TRUE(FindAnnotationEraseTargets(snapshot, next, ids, error)) << error;
        const double hotQueryMs = std::chrono::duration<double, std::milli>(Clock::now() - hotQueryStarted).count();
        EXPECT_EQ(ids, (std::vector<std::uint64_t>{1, 2}));
        const AnnotationEraseStroke incremental{{(*points)[count / 4], (*points)[count / 4 + 1]}, 4, {0, 0, 544, 96}};
        const Clock::time_point incrementalStarted = Clock::now();
        ASSERT_TRUE(FindAnnotationEraseTargets(snapshot, incremental, ids, error)) << error;
        const double incrementalMs =
            std::chrono::duration<double, std::milli>(Clock::now() - incrementalStarted).count();
        EXPECT_EQ(ids, (std::vector<std::uint64_t>{1}));
        std::printf("M2 geometry pressure: points=%zu cold_query_ms=%.3f hot_query_ms=%.3f incremental_ms=%.3f "
                    "starting output\n",
                    count, queryMs, hotQueryMs, incrementalMs);
        std::fflush(stdout);
        SdrSelectionFrame output;
        const Clock::time_point outputStarted = Clock::now();
        ASSERT_TRUE(renderer.Render(desktop, {0, 0, 544, 96}, snapshot, output, error)) << error;
        const double outputMs = std::chrono::duration<double, std::milli>(Clock::now() - outputStarted).count();
        ASSERT_TRUE(output.IsValid());
        EXPECT_EQ(output.Pixels().size(), 544U * 96U * 4U);
        EXPECT_EQ(Pixel(output, 64, 39), (std::array<std::uint8_t, 4>{0, 255, 0, 255}));
        EXPECT_EQ(Pixel(output, 128, 26), (std::array<std::uint8_t, 4>{255, 255, 255, 255}));
        EXPECT_EQ(Pixel(output, 90, 34), (std::array<std::uint8_t, 4>{0, 0, 255, 255}));
        ASSERT_EQ(snapshot->size(), 2U);
        EXPECT_EQ(snapshot->front().id, 1U);
        EXPECT_EQ(snapshot->front().erasures->size(), 8U);
        EXPECT_EQ(std::get<AnnotationStroke>(snapshot->front().payload).points, points);
        EXPECT_EQ(points->size(), count);
        EXPECT_DOUBLE_EQ(points->front().x, 8);
        EXPECT_DOUBLE_EQ(points->back().x, 520);
        EXPECT_EQ(snapshot->back().id, 2U);
        EXPECT_EQ(snapshot->back().erasures, nullptr);
        EXPECT_EQ(snapshot->back().style.rgb, 0x00FF00U);
        SdrSelectionFrame secondOutput;
        const Clock::time_point secondStarted = Clock::now();
        ASSERT_TRUE(renderer.Render(desktop, {0, 0, 544, 96}, snapshot, secondOutput, error)) << error;
        const double secondOutputMs = std::chrono::duration<double, std::milli>(Clock::now() - secondStarted).count();
        EXPECT_EQ(std::vector<std::uint8_t>(secondOutput.Pixels().begin(), secondOutput.Pixels().end()),
                  std::vector<std::uint8_t>(output.Pixels().begin(), output.Pixels().end()));
        for (std::size_t step : {2U, 3U})
        {
            std::vector<AnnotationPoint> currentPoints;
            for (std::size_t index = 0; index < step; ++index)
                currentPoints.push_back((*points)[count / 4 + index]);
            const auto currentStroke = std::make_shared<const AnnotationEraseStroke>(
                AnnotationEraseStroke{std::move(currentPoints), 4, {0, 0, 544, 96}});
            std::vector<AnnotationEraseMask> currentMasks = *pen.erasures;
            currentMasks.push_back({currentStroke, {}});
            AnnotationObject previewPen = pen;
            previewPen.erasures = std::make_shared<const std::vector<AnnotationEraseMask>>(std::move(currentMasks));
            const AnnotationSnapshot preview =
                std::make_shared<const std::vector<AnnotationObject>>(std::vector<AnnotationObject>{previewPen, added});
            SdrSelectionFrame previewOutput;
            const Clock::time_point previewStarted = Clock::now();
            ASSERT_TRUE(renderer.Render(desktop, {0, 0, 544, 96}, preview, previewOutput, error)) << error;
            const double previewMs = std::chrono::duration<double, std::milli>(Clock::now() - previewStarted).count();
            const AnnotationPoint erased = (*points)[count / 4];
            EXPECT_EQ(Pixel(previewOutput, static_cast<int>(erased.x), static_cast<int>(erased.y)),
                      (std::array<std::uint8_t, 4>{255, 255, 255, 255}));
            EXPECT_EQ(Pixel(previewOutput, 64, 39), (std::array<std::uint8_t, 4>{0, 255, 0, 255}));
            EXPECT_EQ(snapshot->front().erasures, pen.erasures);
            RecordProperty("preview_" + std::to_string(count) + "_step" + std::to_string(step) + "_ms",
                           std::to_string(previewMs));
            std::printf("M2 geometry pressure: points=%zu changing_mask_points=%zu preview_output_ms=%.3f\n", count,
                        step, previewMs);
            std::fflush(stdout);
        }
        RecordProperty("query_" + std::to_string(count) + "_ms", std::to_string(queryMs));
        RecordProperty("hot_query_" + std::to_string(count) + "_ms", std::to_string(hotQueryMs));
        RecordProperty("incremental_query_" + std::to_string(count) + "_ms", std::to_string(incrementalMs));
        RecordProperty("output_" + std::to_string(count) + "_ms", std::to_string(outputMs));
        RecordProperty("second_output_" + std::to_string(count) + "_ms", std::to_string(secondOutputMs));
        std::printf("M2 geometry pressure: points=%zu cold_query_ms=%.3f hot_query_ms=%.3f incremental_ms=%.3f "
                    "first_output_ms=%.3f second_output_ms=%.3f\n",
                    count, queryMs, hotQueryMs, incrementalMs, outputMs, secondOutputMs);
        std::fflush(stdout);
    }
}
} // namespace open_st
