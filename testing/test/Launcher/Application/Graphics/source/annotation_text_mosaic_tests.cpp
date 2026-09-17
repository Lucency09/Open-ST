// 验证 M3 文字真实字形以及冻结来源马赛克的网格、顺序、擦除与会话隔离。
#include "annotation_drawing.h"
#include <annotation_hit_test.h>
#include <annotation_mosaic_source.h>
#include <color_conversion.h>
#include <gtest/gtest.h>
#include <selection_output_renderer.h>

#include <algorithm>
#include <array>
#include <cstring>

namespace open_st
{
namespace
{
// 将测试图层固定成不可变文档。
// 入参：objects 为按绘制顺序排列的对象。
// 返回：共享文档。
AnnotationSnapshot Snapshot(std::vector<AnnotationObject> objects)
{
    return std::make_shared<const std::vector<AnnotationObject>>(std::move(objects));
}
// 创建无真实桌面依赖的常量 SDR 输出 plane。
// 入参：bounds 为物理范围；red、green、blue 为像素颜色。
// 返回：紧凑 BGRA plane。
CapturedOutputPlane Solid(RectI bounds, std::uint8_t red, std::uint8_t green = 0, std::uint8_t blue = 0)
{
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(bounds.Width()) * bounds.Height() * 4U);
    for (std::size_t index = 0; index < pixels.size(); index += 4)
    {
        pixels[index] = blue;
        pixels[index + 1] = green;
        pixels[index + 2] = red;
        pixels[index + 3] = 255;
    }
    return {bounds, CapturedPixelFormat::Bgra8Unorm, CapturedColorSpace::SdrGamma22P709, {}, std::move(pixels)};
}
// 创建确定性的二维渐变，方便独立计算各档网格平均值。
// 入参：无。
// 返回：64×32 的 SDR 冻结桌面。
FrozenDesktopFrame Gradient()
{
    std::vector<std::uint8_t> pixels(64U * 32U * 4U);
    for (unsigned y = 0; y < 32; ++y)
        for (unsigned x = 0; x < 64; ++x)
        {
            const std::size_t index = (y * 64U + x) * 4U;
            pixels[index] = 11;
            pixels[index + 1] = static_cast<std::uint8_t>(y * 8);
            pixels[index + 2] = static_cast<std::uint8_t>(x * 4);
            pixels[index + 3] = 255;
        }
    std::vector<CapturedOutputPlane> planes;
    planes.emplace_back(RectI{0, 0, 64, 32}, CapturedPixelFormat::Bgra8Unorm, CapturedColorSpace::SdrGamma22P709,
                        OutputColorMetadata{}, std::move(pixels));
    return {{0, 0, 64, 32}, std::move(planes)};
}
// 读取输出相对位置像素。
// 入参：frame 为有效输出；x、y 为图内坐标。
// 返回：BGRA 值。
std::array<std::uint8_t, 4> Pixel(const SdrSelectionFrame& frame, int x, int y)
{
    const std::size_t index = static_cast<std::size_t>(y) * frame.Stride() + static_cast<std::size_t>(x) * 4U;
    return {frame.Pixels()[index], frame.Pixels()[index + 1], frame.Pixels()[index + 2], frame.Pixels()[index + 3]};
}
// 创建使用指定块档位的矩形马赛克。
// 入参：origin、extent 为物理几何；block 为允许的网格档位。
// 返回：不透明马赛克对象。
AnnotationObject Mosaic(AnnotationPoint origin, AnnotationPoint extent, unsigned block = 8)
{
    AnnotationObject object{2, AnnotationKind::Mosaic, origin, extent};
    object.payload = AnnotationMosaic{block};
    return object;
}
// 创建固定字体及显式换行的文字对象。
// 入参：value 为已规范化正文；size 为物理字号。
// 返回：局部文字布局对象。
AnnotationObject Text(std::u16string value, double size = 24)
{
    AnnotationObject object{1, AnnotationKind::Text, {4, 4}, {}};
    object.payload = AnnotationText{std::make_shared<const std::u16string>(std::move(value)), size};
    return object;
}
// 用实际字形命中查询结果，避免拿文字外接框代替可见区域。
// 入参：snapshot 为文档；point 为物理坐标。
// 返回：命中稳定 ID。
std::uint64_t Hit(const AnnotationSnapshot& snapshot, AnnotationPoint point)
{
    std::uint64_t id{};
    std::wstring error;
    EXPECT_TRUE(HitTestAnnotations(snapshot, {0, 0, 256, 128}, point, 3, id, error)) << error;
    return id;
}
} // namespace

// 验证四档网格的颜色来自完整未标注块均值，而非对象边界内的局部均值。
// 入参：无。
// 返回：各档位与两块的独立数学期望一致。
TEST(AnnotationMosaicTest, four_block_sizes_use_full_frozen_grid_averages)
{
    const FrozenDesktopFrame desktop = Gradient();
    AnnotationMosaicSource source(desktop);
    SelectionOutputRenderer renderer;
    for (unsigned block : {4U, 8U, 16U, 32U})
    {
        const AnnotationSnapshot snapshot = Snapshot({Mosaic({0, 0}, {64, 32}, block)});
        SdrSelectionFrame output;
        std::wstring error;
        ASSERT_TRUE(source.Prepare(snapshot, {0, 0, 64, 32}, error)) << error;
        ASSERT_TRUE(renderer.Render(desktop, {0, 0, 64, 32}, snapshot, output, error, &source)) << error;
        EXPECT_EQ(Pixel(output, 1, 1),
                  (std::array<std::uint8_t, 4>{11, static_cast<std::uint8_t>(4U * (block - 1U)),
                                               static_cast<std::uint8_t>(2U * (block - 1U)), 255}));
        EXPECT_EQ(Pixel(output, static_cast<int>(block + 1), 1)[2], 2U * (3U * block - 1U));
    }
}

// 验证同一网格块跨两个输出时先拼 SDR 再求平均，负坐标及选区裁边不移动格点。
// 入参：无。
// 返回：仅显示块的一部分仍保持整块均值。
TEST(AnnotationMosaicTest, cross_output_block_and_crop_keep_desktop_anchored_grid)
{
    std::vector<CapturedOutputPlane> planes;
    planes.push_back(Solid({-3, -2, 0, 2}, 10));
    planes.push_back(Solid({0, -2, 5, 2}, 210));
    const FrozenDesktopFrame desktop({-3, -2, 5, 2}, std::move(planes));
    AnnotationMosaicSource source(desktop);
    SelectionOutputRenderer renderer;
    SdrSelectionFrame output;
    std::wstring error;
    const AnnotationSnapshot snapshot = Snapshot({Mosaic({-3, -2}, {8, 4}, 4)});
    ASSERT_TRUE(renderer.Render(desktop, {-1, -1, 3, 1}, snapshot, output, error, &source)) << error;
    EXPECT_EQ(Pixel(output, 0, 0)[2], 60U);
    EXPECT_EQ(Pixel(output, 1, 0)[2], 60U);
    EXPECT_EQ(Pixel(output, 2, 0)[2], 210U);
    EXPECT_EQ(Pixel(output, 3, 0)[2], 210U);
}

// 验证桌面空洞以正式输出约定的不透明黑参与整块平均，不跳过缺失输出面积。
// 入参：无。
// 返回：半块红色加半块空洞的均值为一半红色。
TEST(AnnotationMosaicTest, desktop_gap_counts_as_black_in_block_average)
{
    std::vector<CapturedOutputPlane> planes;
    planes.push_back(Solid({-4, 0, -2, 4}, 200));
    planes.push_back(Solid({0, 0, 4, 4}, 100));
    const FrozenDesktopFrame desktop({-4, 0, 4, 4}, std::move(planes));
    SelectionOutputRenderer renderer;
    SdrSelectionFrame output;
    std::wstring error;
    ASSERT_TRUE(renderer.Render(desktop, {-4, 0, 4, 4}, Snapshot({Mosaic({-4, 0}, {8, 4}, 4)}), output, error));
    EXPECT_EQ(Pixel(output, 0, 0)[2], 100U);
    EXPECT_EQ(Pixel(output, 3, 0)[2], 100U);
}

// 验证移动后的马赛克从新位置读取冻结底图，图层顺序改变覆盖效果但不会改变来源颜色。
// 入参：无。
// 返回：位移不搬运旧色块，后绘制对象严格覆盖前对象。
TEST(AnnotationMosaicTest, moving_resamples_frozen_position_and_respects_object_order)
{
    const FrozenDesktopFrame desktop = Gradient();
    AnnotationMosaicSource source(desktop);
    SelectionOutputRenderer renderer;
    AnnotationObject mosaic = Mosaic({0, 0}, {16, 16});
    SdrSelectionFrame output;
    std::wstring error;
    ASSERT_TRUE(renderer.Render(desktop, {0, 0, 64, 32}, Snapshot({mosaic}), output, error, &source));
    EXPECT_EQ(Pixel(output, 3, 3)[2], 14U);
    mosaic.origin.x = 32;
    ASSERT_TRUE(renderer.Render(desktop, {0, 0, 64, 32}, Snapshot({mosaic}), output, error, &source));
    EXPECT_EQ(Pixel(output, 35, 3)[2], 142U);
    AnnotationObject fill{1, AnnotationKind::FilledRectangle, {0, 0}, {64, 32}};
    ASSERT_TRUE(renderer.Render(desktop, {0, 0, 64, 32}, Snapshot({fill, mosaic}), output, error, &source));
    EXPECT_EQ(Pixel(output, 35, 3)[2], 142U);
    ASSERT_TRUE(renderer.Render(desktop, {0, 0, 64, 32}, Snapshot({mosaic, fill}), output, error, &source));
    EXPECT_EQ(Pixel(output, 35, 3), (std::array<std::uint8_t, 4>{0, 0, 255, 255}));
}

// 验证马赛克对象支持局部擦除，擦孔露出原冻结像素并不再命中。
// 入参：无。
// 返回：孔内为原渐变，孔外仍为块均值。
TEST(AnnotationMosaicTest, erasures_reveal_underlying_pixels_and_remove_hit_region)
{
    const FrozenDesktopFrame desktop = Gradient();
    AnnotationObject mosaic = Mosaic({0, 0}, {32, 32});
    const auto stroke =
        std::make_shared<const AnnotationEraseStroke>(AnnotationEraseStroke{{{8, 8}}, 4, {0, 0, 64, 32}});
    mosaic.erasures =
        std::make_shared<const std::vector<AnnotationEraseMask>>(std::vector<AnnotationEraseMask>{{stroke, {}}});
    const AnnotationSnapshot snapshot = Snapshot({mosaic});
    SelectionOutputRenderer renderer;
    SdrSelectionFrame output;
    std::wstring error;
    ASSERT_TRUE(renderer.Render(desktop, {0, 0, 64, 32}, snapshot, output, error));
    EXPECT_EQ(Pixel(output, 8, 8), (std::array<std::uint8_t, 4>{11, 64, 32, 255}));
    EXPECT_EQ(Pixel(output, 2, 2), (std::array<std::uint8_t, 4>{11, 28, 14, 255}));
    EXPECT_EQ(Hit(snapshot, {8, 8}), 0U);
    EXPECT_EQ(Hit(snapshot, {2, 2}), 2U);
}

// 验证 HDR 区域沿用现有 SDR 转换，再和同块 SDR 区域一起求平均，避免跨屏块分裂或重复色调映射。
// 入参：无；只使用小型人工冻结原生 FP16 像素。
// 返回：马赛克颜色等于独立取得的正式无标注 SDR 块均值。
TEST(AnnotationMosaicTest, hdr_and_sdr_share_one_converted_block_average)
{
    std::vector<std::uint8_t> bytes(2U * 4U * 8U);
    const std::array<std::uint16_t, 4> color{EncodeFloat16(2.0F), EncodeFloat16(0.5F), EncodeFloat16(0.25F),
                                             EncodeFloat16(1.0F)};
    for (std::size_t index = 0; index < bytes.size(); index += 8)
        std::memcpy(bytes.data() + index, color.data(), 8);
    std::vector<CapturedOutputPlane> planes;
    planes.emplace_back(RectI{0, 0, 2, 4}, CapturedPixelFormat::Rgba16FloatScRgb, CapturedColorSpace::ScRgb,
                        OutputColorMetadata{}, std::move(bytes));
    planes.push_back(Solid({2, 0, 8, 4}, 20, 60, 100));
    const FrozenDesktopFrame desktop({0, 0, 8, 4}, std::move(planes));
    SelectionOutputRenderer renderer;
    SdrSelectionFrame base;
    std::wstring error;
    ASSERT_TRUE(renderer.Render(desktop, {0, 0, 8, 4}, base, error)) << error;
    std::array<unsigned, 3> sum{};
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            for (std::size_t channel = 0; channel < 3; ++channel)
                sum[channel] += Pixel(base, x, y)[channel];
    SdrSelectionFrame output;
    ASSERT_TRUE(renderer.Render(desktop, {0, 0, 8, 4}, Snapshot({Mosaic({0, 0}, {8, 4}, 4)}), output, error));
    for (std::size_t channel = 0; channel < 3; ++channel)
        EXPECT_EQ(Pixel(output, 1, 1)[channel], (sum[channel] + 8U) / 16U);
}

// 验证超预算候选在转换前拒绝，旧准备结果仍可用，来源也不能被另一冻结会话冒用。
// 入参：无；大桌面仅包含一个像素 plane，其余为空洞，不分配整屏缓冲。
// 返回：预算和会话错误不发布不完整图像。
TEST(AnnotationMosaicTest, failed_preparation_preserves_previous_candidate_and_rejects_other_session)
{
    std::vector<CapturedOutputPlane> planes;
    planes.push_back(Solid({0, 0, 1, 1}, 200));
    const FrozenDesktopFrame desktop({0, 0, 20000, 20000}, std::move(planes));
    AnnotationMosaicSource source(desktop);
    const AnnotationSnapshot valid = Snapshot({Mosaic({0, 0}, {4, 4}, 4)});
    std::wstring error;
    ASSERT_TRUE(source.Prepare(valid, {0, 0, 4, 4}, error));
    EXPECT_FALSE(source.Prepare(Snapshot({Mosaic({0, 0}, {20000, 20000}, 4)}), {0, 0, 20000, 20000}, error));
    SelectionOutputRenderer renderer;
    SdrSelectionFrame output;
    ASSERT_TRUE(renderer.Render(desktop, {0, 0, 4, 4}, valid, output, error, &source));
    EXPECT_EQ(Pixel(output, 0, 0)[2], 13U);
    const FrozenDesktopFrame other = Gradient();
    EXPECT_FALSE(renderer.Render(other, {0, 0, 4, 4}, valid, output, error, &source));
    EXPECT_FALSE(output.IsValid());
}

// 验证低层绘制入口无法从当前预览或标注层猜测马赛克来源，缺少来源时明确失败。
// 入参：无；直接调用共享透明合成入口而不使用会自动建立来源的高层输出器。
// 返回：输出保持为空且具有错误诊断。
TEST(AnnotationMosaicTest, low_level_drawing_requires_frozen_source)
{
    const AnnotationSnapshot snapshot = Snapshot({Mosaic({0, 0}, {8, 8}, 4)});
    const std::vector<std::uint8_t> base(8U * 8U * 4U, 255);
    std::vector<std::uint8_t> pixels;
    std::wstring error;
    EXPECT_FALSE(CompositeAnnotations({0, 0, 8, 8}, snapshot, base, pixels, error));
    EXPECT_TRUE(pixels.empty());
    EXPECT_FALSE(error.empty());
}

// 验证中日文和显式换行真实输出，命中只接受实际字形，擦除后字形对应位置变为空洞。
// 入参：无；扫描实际完全覆盖像素，避免假定特定系统字体的单个字形坐标。
// 返回：文字产生可见轮廓，空白与擦孔不命中。
TEST(AnnotationTextTest, multilingual_glyph_output_hit_and_local_erase_share_geometry)
{
    std::vector<CapturedOutputPlane> planes;
    planes.push_back(Solid({0, 0, 128, 96}, 255, 255, 255));
    const FrozenDesktopFrame desktop({0, 0, 128, 96}, std::move(planes));
    AnnotationObject object = Text(u"中文\n日本語", 24);
    AnnotationSnapshot snapshot = Snapshot({object});
    SelectionOutputRenderer renderer;
    SdrSelectionFrame output;
    std::wstring error;
    ASSERT_TRUE(renderer.Render(desktop, {0, 0, 128, 96}, snapshot, output, error)) << error;
    int inkX = -1;
    int inkY = -1;
    bool secondLine{};
    for (int y = 0; y < 80; ++y)
        for (int x = 0; x < 110; ++x)
            if (Pixel(output, x, y)[0] < 32)
            {
                if (inkX < 0)
                {
                    inkX = x;
                    inkY = y;
                }
                secondLine = secondLine || y > 34;
            }
    ASSERT_GE(inkX, 0);
    EXPECT_TRUE(secondLine);
    EXPECT_EQ(Hit(snapshot, {inkX + 0.5, inkY + 0.5}), 1U);
    EXPECT_EQ(Hit(snapshot, {120, 80}), 0U);
    const auto stroke = std::make_shared<const AnnotationEraseStroke>(
        AnnotationEraseStroke{{{inkX + 0.5, inkY + 0.5}}, 4, {0, 0, 128, 96}});
    object.erasures =
        std::make_shared<const std::vector<AnnotationEraseMask>>(std::vector<AnnotationEraseMask>{{stroke, {-4, -4}}});
    snapshot = Snapshot({object});
    ASSERT_TRUE(renderer.Render(desktop, {0, 0, 128, 96}, snapshot, output, error));
    EXPECT_EQ(Pixel(output, inkX, inkY), (std::array<std::uint8_t, 4>{255, 255, 255, 255}));
    EXPECT_EQ(Hit(snapshot, {inkX + 0.5, inkY + 0.5}), 0U);
}

// 验证空白字符不变成可选矩形，长行按选区裁剪而不自动软换行。
// 入参：无。
// 返回：两字之间空白不命中，长行不在下一行生成墨迹。
TEST(AnnotationTextTest, spaces_are_not_hits_and_long_lines_do_not_soft_wrap)
{
    std::vector<CapturedOutputPlane> planes;
    planes.push_back(Solid({0, 0, 128, 96}, 255, 255, 255));
    const FrozenDesktopFrame desktop({0, 0, 128, 96}, std::move(planes));
    SelectionOutputRenderer renderer;
    SdrSelectionFrame output;
    std::wstring error;
    const AnnotationSnapshot spaced = Snapshot({Text(u"I     I", 32)});
    ASSERT_TRUE(renderer.Render(desktop, {0, 0, 128, 96}, spaced, output, error));
    int first = 128;
    int last = -1;
    for (int y = 0; y < 48; ++y)
        for (int x = 0; x < 128; ++x)
            if (Pixel(output, x, y)[0] < 64)
            {
                first = std::min(first, x);
                last = std::max(last, x);
            }
    ASSERT_GT(last, first);
    const int middle = (first + last) / 2;
    for (int y = 4; y < 44; ++y)
        EXPECT_EQ(Hit(spaced, {middle + 0.5, y + 0.5}), 0U);
    ASSERT_TRUE(renderer.Render(desktop, {0, 0, 128, 96}, Snapshot({Text(u"IIIIIIIIIIIIIIIIIIIIIIIIIIII", 32)}), output,
                                error));
    for (int x = 0; x < 128; ++x)
        EXPECT_EQ(Pixel(output, x, 65), (std::array<std::uint8_t, 4>{255, 255, 255, 255}));
}
} // namespace open_st
