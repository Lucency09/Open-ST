// 文件职责：验证冻结选区的精确裁切、跨屏拼接、SDR 像素保真及 HDR 转换和失败恢复。

#include <array>
#include <color_conversion.h>
#include <cstring>
#include <dxgi1_2.h>
#include <gtest/gtest.h>
#include <limits>
#include <selection_output_renderer.h>
#include <utility>
#include <wrl/client.h>

namespace open_st
{
namespace
{
// 从预设 SDR 像素构造原生冻结 plane，供裁切和跨屏拼接测试使用。
// 入参：bounds：该 plane 在虚拟桌面的物理像素半开边界；pixels：转移所有权的紧凑 BGRA8 测试字节。
// 返回：标记为 SDR 颜色空间并拥有给定像素的 plane。
CapturedOutputPlane SdrPlane(RectI bounds, std::vector<std::uint8_t> pixels)
{
    return {bounds, CapturedPixelFormat::Bgra8Unorm, CapturedColorSpace::SdrGamma22P709, {}, std::move(pixels)};
}
// 构造非空旧选区输出，用于验证后续失败会清除旧结果。
// 入参：无。
// 返回：预填充像素的有效 SDR 选区帧，供失败回退测试作为初始输出。
SdrSelectionFrame PreviousOutput()
{
    return {{0, 0, 1, 1}, {1, 2, 3, 4}};
}
// 将 SDR 输出借用视图复制为独立字节数组，以便逐字节断言。
// 入参：frame：待检查的 SDR 选区输出帧。
// 返回：当前 frame 像素的自有字节副本。
std::vector<std::uint8_t> Bytes(const SdrSelectionFrame& frame)
{
    return {frame.Pixels().begin(), frame.Pixels().end()};
}

// 验证负桌面坐标下裁切行列偏移准确，SDR 的四个原始字节完整保留。
// 入参：无运行时形参；宏参数 SelectionOutputTest 为测试套件，negative_coordinates_preserve_sdr_bytes 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
TEST(SelectionOutputTest, negative_coordinates_preserve_sdr_bytes)
{
    std::vector<CapturedOutputPlane> planes;
    planes.push_back(
        SdrPlane({-3, -2, 0, 0}, {1, 2, 3, 0, 4, 5, 6, 1, 7, 8, 9, 2, 10, 11, 12, 3, 13, 14, 15, 4, 16, 17, 18, 5}));
    FrozenDesktopFrame desktop({-3, -2, 0, 0}, std::move(planes));
    SelectionOutputRenderer renderer;
    SdrSelectionFrame output;
    std::wstring error;
    ASSERT_TRUE(renderer.Render(desktop, {-2, -2, 0, 0}, output, error));
    EXPECT_EQ(output.Bounds().left, -2);
    EXPECT_EQ(output.Bounds().top, -2);
    EXPECT_EQ(output.Stride(), 8U);
    EXPECT_EQ(Bytes(output), (std::vector<std::uint8_t>{4, 5, 6, 1, 7, 8, 9, 2, 13, 14, 15, 4, 16, 17, 18, 5}));
}

// 验证跨屏拼接保留各屏像素，布局空洞固定为不透明黑色。
// 入参：无运行时形参；宏参数 SelectionOutputTest 为测试套件，cross_monitor_gap_is_opaque_black 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
TEST(SelectionOutputTest, cross_monitor_gap_is_opaque_black)
{
    std::vector<CapturedOutputPlane> planes;
    planes.push_back(SdrPlane({-1, 0, 0, 1}, {10, 20, 30, 40}));
    planes.push_back(SdrPlane({1, 0, 2, 1}, {50, 60, 70, 80}));
    FrozenDesktopFrame desktop({-1, 0, 2, 1}, std::move(planes));
    SelectionOutputRenderer renderer;
    SdrSelectionFrame output;
    std::wstring error;
    ASSERT_TRUE(renderer.Render(desktop, {-1, 0, 2, 1}, output, error));
    EXPECT_EQ(Bytes(output), (std::vector<std::uint8_t>{10, 20, 30, 40, 0, 0, 0, 255, 50, 60, 70, 80}));
}

// 验证空选区、越界、反向及有符号跨度溢出均失败，并清空旧输出。
// 入参：无运行时形参；宏参数 SelectionOutputTest 为测试套件，invalid_selection_clears_previous_output 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
TEST(SelectionOutputTest, invalid_selection_clears_previous_output)
{
    std::vector<CapturedOutputPlane> planes;
    planes.push_back(SdrPlane({0, 0, 1, 1}, {1, 2, 3, 4}));
    FrozenDesktopFrame desktop({0, 0, 1, 1}, std::move(planes));
    const std::array<RectI, 4> selections{
        {{0, 0, 0, 1},
         {-1, 0, 1, 1},
         {1, 0, 0, 1},
         {(std::numeric_limits<int>::min)(), 0, (std::numeric_limits<int>::max)(), 1}}};
    SelectionOutputRenderer renderer;
    for (const RectI selection : selections)
    {
        SdrSelectionFrame output = PreviousOutput();
        std::wstring error;
        EXPECT_FALSE(renderer.Render(desktop, selection, output, error));
        EXPECT_FALSE(output.IsValid());
        EXPECT_TRUE(output.Pixels().empty());
        EXPECT_FALSE(error.empty());
    }
}

// 验证只拒绝与选区相交的重叠输出，远处重叠不会阻止无歧义裁切。
// 入参：无运行时形参；宏参数 SelectionOutputTest 为测试套件，overlapping_outputs_rejected_only_inside_selection 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
TEST(SelectionOutputTest, overlapping_outputs_rejected_only_inside_selection)
{
    std::vector<CapturedOutputPlane> planes;
    planes.push_back(SdrPlane({0, 0, 2, 1}, {1, 2, 3, 4, 5, 6, 7, 8}));
    planes.push_back(SdrPlane({1, 0, 2, 1}, {9, 10, 11, 12}));
    FrozenDesktopFrame desktop({0, 0, 2, 1}, std::move(planes));
    SelectionOutputRenderer renderer;
    SdrSelectionFrame output = PreviousOutput();
    std::wstring error;
    EXPECT_FALSE(renderer.Render(desktop, {0, 0, 2, 1}, output, error));
    EXPECT_TRUE(output.Pixels().empty());
    ASSERT_TRUE(renderer.Render(desktop, {0, 0, 1, 1}, output, error));
    EXPECT_EQ(Bytes(output), (std::vector<std::uint8_t>{1, 2, 3, 4}));
}

// 验证 SDR RGB10 按通道四舍五入量化为 BGRX，忽略两位 alpha 且无需 HDR 设备。
// 入参：无运行时形参；宏参数 SelectionOutputTest 为测试套件，sdr_rgb10_quantizes_channels 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
TEST(SelectionOutputTest, sdr_rgb10_quantizes_channels)
{
    const std::array<std::uint32_t, 2> packed{1023U | (512U << 10U), (1023U << 20U) | (3U << 30U)};
    std::vector<std::uint8_t> pixels(sizeof(packed));
    std::memcpy(pixels.data(), packed.data(), sizeof(packed));
    std::vector<CapturedOutputPlane> planes;
    planes.emplace_back(RectI{0, 0, 2, 1}, CapturedPixelFormat::Rgb10A2Unorm, CapturedColorSpace::SdrGamma22P709,
                        OutputColorMetadata{}, std::move(pixels));
    FrozenDesktopFrame desktop({0, 0, 2, 1}, std::move(planes));
    SelectionOutputRenderer renderer;
    SdrSelectionFrame output;
    std::wstring error;
    ASSERT_TRUE(renderer.Render(desktop, {0, 0, 2, 1}, output, error));
    EXPECT_EQ(Bytes(output), (std::vector<std::uint8_t>{0, 128, 255, 255, 255, 0, 0, 255}));
}

// 验证第一屏成功后遇到未知颜色空间仍整次失败，不发布部分拼接结果。
// 入参：无运行时形参；宏参数 SelectionOutputTest 为测试套件，unknown_color_discards_partial_output 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
TEST(SelectionOutputTest, unknown_color_discards_partial_output)
{
    std::vector<CapturedOutputPlane> planes;
    planes.push_back(SdrPlane({0, 0, 1, 1}, {1, 2, 3, 4}));
    planes.emplace_back(RectI{1, 0, 2, 1}, CapturedPixelFormat::Rgb10A2Unorm, CapturedColorSpace::Unknown,
                        OutputColorMetadata{}, std::vector<std::uint8_t>(4, 0));
    FrozenDesktopFrame desktop({0, 0, 2, 1}, std::move(planes));
    ASSERT_TRUE(desktop.IsValid());
    SelectionOutputRenderer renderer;
    SdrSelectionFrame output = PreviousOutput();
    std::wstring error;
    EXPECT_FALSE(renderer.Render(desktop, {0, 0, 2, 1}, output, error));
    EXPECT_TRUE(output.Pixels().empty());
    EXPECT_FALSE(error.empty());
}

// 验证 FP16 非有限原生像素失败，不保留上一次或部分 SDR 输出。
// 入参：无运行时形参；宏参数 SelectionOutputTest 为测试套件，non_finite_hdr_discards_output 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
TEST(SelectionOutputTest, non_finite_hdr_discards_output)
{
    const std::array<std::uint16_t, 4> half{0x7C00U, 0x0000U, 0x0000U, 0x3C00U};
    std::vector<std::uint8_t> pixels(sizeof(half));
    std::memcpy(pixels.data(), half.data(), sizeof(half));
    std::vector<CapturedOutputPlane> planes;
    planes.emplace_back(RectI{0, 0, 1, 1}, CapturedPixelFormat::Rgba16FloatScRgb, CapturedColorSpace::ScRgb,
                        OutputColorMetadata{}, std::move(pixels));
    FrozenDesktopFrame desktop({0, 0, 1, 1}, std::move(planes));
    SelectionOutputRenderer renderer;
    SdrSelectionFrame output = PreviousOutput();
    std::wstring error;
    EXPECT_FALSE(renderer.Render(desktop, {0, 0, 1, 1}, output, error));
    EXPECT_TRUE(output.Pixels().empty());
    EXPECT_FALSE(error.empty());
}
// 通过独立 DXGI 枚举确认硬件前提，不能把被测转换器初始化失败当作跳过条件。
class SelectionOutputIntegrationTest : public ::testing::Test
{
  protected:
    // 在每个图形集成用例运行前验证独立于被测函数的 Windows 图形平台前提。
    // 入参：无。
    // 返回：无返回值；平台前提不满足时标记跳过，平台查询错误按断言报告失败。
    void SetUp() override
    {
        Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
        ASSERT_TRUE(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf()))));
        for (UINT index = 0U;; ++index)
        {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            const HRESULT result = factory->EnumAdapters1(index, adapter.GetAddressOf());
            if (result == DXGI_ERROR_NOT_FOUND)
            {
                GTEST_SKIP() << "No hardware graphics adapter is available.";
            }
            ASSERT_TRUE(SUCCEEDED(result));
            DXGI_ADAPTER_DESC1 description{};
            ASSERT_TRUE(SUCCEEDED(adapter->GetDesc1(&description)));
            if ((description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0U)
            {
                return;
            }
        }
    }
};

// 验证合成 SDR/HDR 跨屏裁切：SDR 含 X 字节原样保留，HDR 与相同非紧凑 ROI 单独转换逐字节一致。
// 入参：无运行时形参；宏参数 SelectionOutputIntegrationTest 为测试套件，mixed_sdr_hdr_matches_standalone_roi 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
TEST_F(SelectionOutputIntegrationTest, mixed_sdr_hdr_matches_standalone_roi)
{
    const std::vector<std::uint8_t> sdr{1,  2,  3,  0,  4,  5,  6,  17,  7,  8,  9,  31,
                                        10, 11, 12, 63, 13, 14, 15, 127, 16, 17, 18, 191};
    const std::array<float, 9> levels{0.0F, 0.25F, 9.0F, 1.0F, 3.25F, 9.0F, 5.0F, 12.5F, 9.0F};
    std::vector<std::uint8_t> hdr(levels.size() * 8U);
    for (std::size_t index = 0U; index < levels.size(); ++index)
    {
        const std::uint16_t gray = EncodeFloat16(levels[index]);
        const std::array<std::uint16_t, 4> rgba{gray, gray, gray, 0x3C00U};
        std::memcpy(hdr.data() + index * 8U, rgba.data(), sizeof(rgba));
    }
    NativeToneMapper referenceMapper;
    std::vector<std::uint8_t> expectedHdr;
    std::wstring error;
    ASSERT_TRUE(referenceMapper.Convert(
        {2U, 2U, 24U, std::span<const std::uint8_t>(hdr).subspan(24U), HdrPixelFormat::Rgba16FloatScRgb}, expectedHdr,
        error))
        << error;
    ASSERT_EQ(expectedHdr.size(), 16U);
    std::vector<CapturedOutputPlane> planes;
    planes.push_back(SdrPlane({-2, -1, 0, 2}, sdr));
    planes.emplace_back(RectI{0, -1, 3, 2}, CapturedPixelFormat::Rgba16FloatScRgb, CapturedColorSpace::ScRgb,
                        OutputColorMetadata{}, std::move(hdr));
    FrozenDesktopFrame desktop({-2, -1, 3, 2}, std::move(planes));
    ASSERT_TRUE(desktop.IsValid());
    SelectionOutputRenderer renderer;
    ASSERT_TRUE(renderer.Prepare(error)) << error;
    SdrSelectionFrame output;
    ASSERT_TRUE(renderer.Render(desktop, {-1, 0, 2, 2}, output, error)) << error;
    ASSERT_TRUE(output.IsValid());
    EXPECT_EQ(output.Bounds().left, -1);
    EXPECT_EQ(output.Bounds().top, 0);
    EXPECT_EQ(output.Stride(), 12U);
    std::vector<std::uint8_t> expected(24U);
    for (std::size_t row = 0U; row < 2U; ++row)
    {
        std::memcpy(expected.data() + row * 12U, sdr.data() + (row + 1U) * 8U + 4U, 4U);
        std::memcpy(expected.data() + row * 12U + 4U, expectedHdr.data() + row * 8U, 8U);
    }
    EXPECT_EQ(Bytes(output), expected);
}
} // namespace
} // namespace open_st
