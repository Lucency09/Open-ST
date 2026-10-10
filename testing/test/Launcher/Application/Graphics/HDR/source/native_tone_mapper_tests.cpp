// 文件职责：验证原生 HDR 色调映射的输出、行跨度、输入拒绝和重复调用，并识别图形环境限制。

#include <gtest/gtest.h>

#include <color_conversion.h>
#include <native_tone_mapper.h>
#include <native_tone_mapper_test_access.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <dxgi1_2.h>
#include <iostream>
#include <limits>
#include <psapi.h>
#include <vector>
#include <windows.h>
#include <wrl/client.h>

namespace
{
// 把给定灰阶亮度编码为 FP16 RGBA 图像，用于验证色调映射顺序与高光分离。
// 入参：levels：逐像素线性 scRGB 灰阶，1.0 对应 80 nit。
// 返回：紧凑 FP16 RGBA 字节缓冲区，每像素 RGB 等于对应灰阶且 alpha 为 1.0。
std::vector<std::uint8_t> MakeGrayPixels(std::span<const float> levels)
{
    std::vector<std::uint8_t> pixels(levels.size() * 8U);
    for (std::size_t index = 0U; index < levels.size(); ++index)
    {
        const std::uint16_t gray = open_st::EncodeFloat16(levels[index]);
        const std::array<std::uint16_t, 4> rgba{gray, gray, gray, 0x3C00U};
        std::memcpy(pixels.data() + index * 8U, rgba.data(), 8U);
    }
    return pixels;
}

// 仅在没有真实硬件适配器时跳过 GPU 集成测试，DXGI 调用失败仍按失败处理。
class NativeToneMapperIntegrationTest : public testing::Test
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
} // namespace

// 用双精度独立计算审核通过的曲线，不读取 shader 常量或实现输出。
// 入参：m 为曝光后最大线性分量，p 为固定设计范围。返回：映射最大分量。
double ReferenceShoulder(double m, double p)
{
    constexpr double A = .875, B = .995, K = .6;
    const double d = (B - A) / std::log(p);
    if (m <= K)
        return A * m;
    if (m < 1.0)
    {
        const double t = (m - K) / (1.0 - K);
        return A * m + (1.0 - K) * (A - d) * t * t * (1.0 - t);
    }
    if (m <= p)
        return A + (B - A) * std::log(m) / std::log(p);
    return B + (1.0 - B) * (-std::expm1(-(m - p) * d / (p * (1.0 - B))));
}

// 把独立线性参考编码为 SDR 字节。
// 入参：value 为线性光。返回：舍入的 sRGB 代码。
unsigned int ReferenceByte(double value)
{
    const double clamped = std::clamp(value, 0.0, 1.0);
    const double encoded = clamped <= .0031308 ? clamped * 12.92 : 1.055 * std::pow(clamped, 1.0 / 2.4) - .055;
    return static_cast<unsigned int>(std::lround(encoded * 255.0));
}

// 验证领域边界、缺失/非法参数明确失败并清空输出，不触发初始化或默认回退。
// 入参：无。返回：断言参数合同。
TEST(NativeToneMapperValidationTest, rejects_missing_or_invalid_white_and_highlight_ceiling)
{
    EXPECT_TRUE(open_st::IsHdrHighlightCeilingNits(1000));
    EXPECT_TRUE(open_st::IsHdrHighlightCeilingNits(10000));
    EXPECT_FALSE(open_st::IsHdrHighlightCeilingNits(999));
    EXPECT_FALSE(open_st::IsHdrHighlightCeilingNits(10001));
    EXPECT_FALSE(open_st::IsHdrHighlightCeilingNits(-1));
    const std::vector<std::uint8_t> pixels = MakeGrayPixels(std::array<float, 1>{1.0F});
    open_st::NativeToneMapper mapper;
    for (const float white :
         {0.0F, -1.0F, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN(),
          std::numeric_limits<float>::denorm_min(), std::numeric_limits<float>::min()})
    {
        std::wstring error;
        std::vector<std::uint8_t> output{255U};
        EXPECT_FALSE(mapper.Convert({1U, 1U, 8U, pixels, open_st::HdrPixelFormat::Rgba16FloatScRgb, white}, {4000U},
                                    output, error));
        EXPECT_TRUE(output.empty());
        EXPECT_FALSE(error.empty());
    }
    for (const unsigned int ceiling : {0U, 999U, 10001U, std::numeric_limits<unsigned int>::max()})
    {
        std::wstring error;
        std::vector<std::uint8_t> output{255U};
        EXPECT_FALSE(mapper.Convert({1U, 1U, 8U, pixels, open_st::HdrPixelFormat::Rgba16FloatScRgb, 80.0F}, {ceiling},
                                    output, error));
        EXPECT_TRUE(output.empty());
        EXPECT_FALSE(error.empty());
    }
}

// 在硬件/WARP 同一生产链对照独立数学参考，覆盖膝点、白锚、P上下和曝光，误差仅允许GPU FP16与BGRA8量化。
// 入参：无。返回：断言新基线、固定P和无选区峰值依赖。
TEST(NativeToneMapperProductionTest, matches_reference_on_hardware_and_warp_with_fixed_range)
{
    for (const bool warp : {false, true})
    {
        SCOPED_TRACE(warp);
        open_st::NativeToneMapper mapper;
        if (warp)
            ASSERT_TRUE(open_st::NativeToneMapperTestAccess::ForceWarp(mapper));
        std::wstring error;
        ASSERT_TRUE(mapper.Prepare(error)) << error;
        for (const float white : {80.0F, 203.0F, 260.0F, 2000.0F})
            for (const unsigned int ceiling : {1000U, 4000U, 10000U})
                for (const unsigned int exposure : {25U, 100U, 200U})
                {
                    SCOPED_TRACE(white);
                    SCOPED_TRACE(ceiling);
                    SCOPED_TRACE(exposure);
                    ASSERT_TRUE(mapper.SetBrightnessPercent(exposure));
                    const double p = std::max(2.0, static_cast<double>(ceiling) / white);
                    const std::array<double, 12> normalized{0.0, .1,    .599, .6,       .601, .9,
                                                            1.0, 1.001, 2.0,  p * .999, p,    p * 2.0};
                    std::vector<float> levels;
                    for (const double level : normalized)
                        levels.push_back(static_cast<float>(level * white / 80.0));
                    const std::vector<std::uint8_t> pixels = MakeGrayPixels(levels);
                    std::vector<std::uint8_t> output;
                    ASSERT_TRUE(mapper.Convert({12U, 1U, 96U, pixels, open_st::HdrPixelFormat::Rgba16FloatScRgb, white},
                                               {ceiling}, output, error))
                        << error;
                    for (std::size_t index = 0U; index < levels.size(); ++index)
                    {
                        const double decoded = open_st::DecodeFloat16(open_st::EncodeFloat16(levels[index]));
                        const unsigned int expected =
                            ReferenceByte(ReferenceShoulder(decoded * 80.0 / white * exposure / 100.0, p));
                        for (std::size_t channel = 0U; channel < 3U; ++channel)
                            EXPECT_NEAR(output[index * 4U + channel], expected, 1.0);
                        EXPECT_EQ(output[index * 4U + 3U], 255U);
                        std::vector<std::uint8_t> crop;
                        const std::span<const std::uint8_t> pixel(pixels.data() + index * 8U, 8U);
                        ASSERT_TRUE(
                            mapper.Convert({1U, 1U, 8U, pixel, open_st::HdrPixelFormat::Rgba16FloatScRgb, white},
                                           {ceiling}, crop, error))
                            << error;
                        EXPECT_TRUE(std::equal(crop.begin(), crop.end(),
                                               output.begin() + static_cast<std::ptrdiff_t>(index * 4U)));
                    }
                }
    }
}

// 验证三格式彩色分量共享同一个最大分量肩部比例，并在硬件和 WARP 中保留负色域分量处理。
// 入参：无。返回：断言生产链与独立参考的逐通道结果。
TEST(NativeToneMapperProductionTest, preserves_color_ratios_for_three_formats_and_both_drivers)
{
    constexpr float WHITE = 203.0F;
    constexpr unsigned int CEILING = 4000U;
    for (const bool warp : {false, true})
    {
        open_st::NativeToneMapper mapper;
        if (warp)
            ASSERT_TRUE(open_st::NativeToneMapperTestAccess::ForceWarp(mapper));
        for (const open_st::HdrPixelFormat format :
             {open_st::HdrPixelFormat::Rgba16FloatScRgb, open_st::HdrPixelFormat::Rgb10A2ScRgb,
              open_st::HdrPixelFormat::Rgb10A2Hdr10})
        {
            SCOPED_TRACE(warp);
            SCOPED_TRACE(static_cast<int>(format));
            std::vector<std::uint8_t> pixels;
            std::array<float, 3> rgb{};
            if (format == open_st::HdrPixelFormat::Rgba16FloatScRgb)
            {
                const std::array<std::uint16_t, 4> half{open_st::EncodeFloat16(4.0F), open_st::EncodeFloat16(-.2F),
                                                        open_st::EncodeFloat16(1.0F), 0U};
                pixels.resize(8U);
                std::memcpy(pixels.data(), half.data(), 8U);
                for (std::size_t channel = 0U; channel < 3U; ++channel)
                    rgb[channel] = open_st::DecodeFloat16(half[channel]);
            }
            else
            {
                const std::uint32_t packed = 900U | (500U << 10U) | (100U << 20U);
                pixels.resize(4U);
                std::memcpy(pixels.data(), &packed, 4U);
                const open_st::LinearScRgb decoded = open_st::DecodeRgb10A2ToScRgb(
                    packed, format == open_st::HdrPixelFormat::Rgb10A2Hdr10 ? open_st::Rgb10ColorSpace::Hdr10
                                                                            : open_st::Rgb10ColorSpace::ScRgb);
                rgb = {open_st::DecodeFloat16(open_st::EncodeFloat16(decoded.red)),
                       open_st::DecodeFloat16(open_st::EncodeFloat16(decoded.green)),
                       open_st::DecodeFloat16(open_st::EncodeFloat16(decoded.blue))};
            }
            for (const unsigned int exposure : {25U, 100U, 200U})
            {
                ASSERT_TRUE(mapper.SetBrightnessPercent(exposure));
                std::wstring error;
                std::vector<std::uint8_t> output;
                ASSERT_TRUE(mapper.Convert({1U, 1U, pixels.size(), pixels, format, WHITE}, {CEILING}, output, error))
                    << error;
                const double scale = 80.0 / WHITE * exposure / 100.0;
                const double maximum = std::max({rgb[0], rgb[1], rgb[2]}) * scale;
                const double factor = ReferenceShoulder(maximum, static_cast<double>(CEILING) / WHITE) / maximum;
                for (std::size_t channel = 0U; channel < 3U; ++channel)
                    EXPECT_NEAR(output[2U - channel], ReferenceByte(rgb[channel] * scale * factor), 1.0);
                EXPECT_EQ(output[3U], 255U);
            }
        }
    }
}

// 验证零尺寸、未知格式、短 stride、短缓冲和溢出尺寸明确失败并清空旧输出。
// 入参：无运行时形参；宏参数 NativeToneMapperValidationTest 为测试套件，rejects_invalid_views_and_clears_output
// 为用例名。 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
TEST(NativeToneMapperValidationTest, rejects_invalid_views_and_clears_output)
{
    open_st::NativeToneMapper mapper;
    const std::array<float, 1> levels{1.0F};
    const std::vector<std::uint8_t> pixels = MakeGrayPixels(levels);
    const open_st::HdrImageView valid{1U, 1U, 8U, pixels, open_st::HdrPixelFormat::Rgba16FloatScRgb, 80.0F};
    std::array<open_st::HdrImageView, 6> invalid{valid, valid, valid, valid, valid, valid};
    invalid[0].width = 0U;
    invalid[1].height = 0U;
    invalid[2].stride = 7U;
    invalid[3].pixels = std::span<const std::uint8_t>(pixels).first(7U);
    invalid[4].format = static_cast<open_st::HdrPixelFormat>(99);
    invalid[5].width = std::numeric_limits<std::uint32_t>::max();
    invalid[5].height = std::numeric_limits<std::uint32_t>::max();
    for (const open_st::HdrImageView& view : invalid)
    {
        std::vector<std::uint8_t> output{1U, 2U, 3U};
        std::wstring error;
        EXPECT_FALSE(mapper.Convert(view, {4000U}, output, error));
        EXPECT_TRUE(output.empty());
        EXPECT_FALSE(error.empty());
    }
}

// 验证 FP16 各颜色通道的 NaN 和正负无穷不进入原生效果，失败后不返回旧像素。
// 入参：无运行时形参；宏参数 NativeToneMapperValidationTest 为测试套件，rejects_nonfinite_color_channels 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
TEST(NativeToneMapperValidationTest, rejects_nonfinite_color_channels)
{
    open_st::NativeToneMapper mapper;
    for (const std::uint16_t nonfinite : std::array<std::uint16_t, 3>{0x7C00U, 0xFC00U, 0x7E00U})
    {
        for (std::size_t channel = 0U; channel < 3U; ++channel)
        {
            std::array<std::uint16_t, 4> rgba{0x3C00U, 0x3C00U, 0x3C00U, 0x3C00U};
            rgba[channel] = nonfinite;
            std::vector<std::uint8_t> pixels(8U);
            std::memcpy(pixels.data(), rgba.data(), pixels.size());
            std::vector<std::uint8_t> output{255U};
            std::wstring error;
            EXPECT_FALSE(mapper.Convert({1U, 1U, 8U, pixels, open_st::HdrPixelFormat::Rgba16FloatScRgb, 80.0F}, {4000U},
                                        output, error));
            EXPECT_TRUE(output.empty());
            EXPECT_FALSE(error.empty());
        }
    }
}

// 验证黑位、灰阶单调、260 nit 桌面白和更亮高光；固定肩部新基线另由数值用例验证。
// 入参：无运行时形参；宏参数 NativeToneMapperIntegrationTest
// 为测试套件，preserves_black_gray_order_and_highlight_separation 为用例名。 返回：无返回值；断言向 GoogleTest
// 报告该用例通过或失败。
TEST_F(NativeToneMapperIntegrationTest, preserves_black_gray_order_and_highlight_separation)
{
    const std::array<float, 7> levels{0.0F, 0.18F, 1.0F, 2.0F, 3.25F, 5.0F, 12.5F};
    const std::vector<std::uint8_t> pixels = MakeGrayPixels(levels);
    open_st::NativeToneMapper mapper;
    std::wstring error;
    ASSERT_TRUE(mapper.Prepare(error)) << error;
    std::vector<std::uint8_t> output;
    ASSERT_TRUE(
        mapper.Convert({7U, 1U, 56U, pixels, open_st::HdrPixelFormat::Rgba16FloatScRgb, 80.0F}, {4000U}, output, error))
        << error;
    ASSERT_EQ(output.size(), 28U);
    EXPECT_LE(output[0], 2U);
    for (std::size_t index = 0U; index < levels.size(); ++index)
    {
        const std::size_t offset = index * 4U;
        EXPECT_NEAR(output[offset], output[offset + 1U], 2.0);
        EXPECT_NEAR(output[offset + 1U], output[offset + 2U], 2.0);
        EXPECT_EQ(output[offset + 3U], 255U);
        if (index != 0U)
        {
            EXPECT_GT(output[offset], output[offset - 4U]);
        }
    }
    EXPECT_GE(output[16U], 210U);
    EXPECT_LT(output[16U], 255U);
    EXPECT_GT(output[24U], output[20U]);
    mapper.ReleaseImageResources();
    mapper.ReleaseImageResources();
    std::vector<std::uint8_t> repeated;
    ASSERT_TRUE(mapper.Convert({7U, 1U, 56U, pixels, open_st::HdrPixelFormat::Rgba16FloatScRgb, 80.0F}, {4000U},
                               repeated, error))
        << error;
    EXPECT_EQ(repeated, output);
}

// 验证负 scRGB 色域分量可处理且 alpha 被统一为不透明，不把合法负值作为输入错误。
// 入参：无运行时形参；宏参数 NativeToneMapperIntegrationTest 为测试套件，accepts_negative_gamut_components 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
TEST_F(NativeToneMapperIntegrationTest, accepts_negative_gamut_components)
{
    const std::array<std::uint16_t, 4> rgba{open_st::EncodeFloat16(3.0F), open_st::EncodeFloat16(-0.2F),
                                            open_st::EncodeFloat16(0.5F), 0x0000U};
    std::vector<std::uint8_t> pixels(8U);
    std::memcpy(pixels.data(), rgba.data(), pixels.size());
    open_st::NativeToneMapper mapper;
    std::wstring error;
    std::vector<std::uint8_t> output;
    ASSERT_TRUE(
        mapper.Convert({1U, 1U, 8U, pixels, open_st::HdrPixelFormat::Rgba16FloatScRgb, 80.0F}, {4000U}, output, error))
        << error;
    ASSERT_EQ(output.size(), 4U);
    EXPECT_EQ(output[3U], 255U);
    EXPECT_GT(output[2U], output[1U]);
}

// 验证带行尾填充的 FP16 输入不把 padding 当像素，并在不同尺寸间复用同一转换器。
// 入参：无运行时形参；宏参数 NativeToneMapperIntegrationTest 为测试套件，respects_padded_stride_and_changes_dimensions
// 为用例名。 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
TEST_F(NativeToneMapperIntegrationTest, respects_padded_stride_and_changes_dimensions)
{
    const std::array<float, 4> levels{0.0F, 1.0F, 3.25F, 12.5F};
    const std::vector<std::uint8_t> tight = MakeGrayPixels(levels);
    std::vector<std::uint8_t> padded(40U, 0xFFU);
    std::memcpy(padded.data(), tight.data(), 16U);
    std::memcpy(padded.data() + 24U, tight.data() + 16U, 16U);
    open_st::NativeToneMapper mapper;
    std::wstring error;
    std::vector<std::uint8_t> expected;
    std::vector<std::uint8_t> actual;
    ASSERT_TRUE(mapper.Convert({4U, 1U, 32U, tight, open_st::HdrPixelFormat::Rgba16FloatScRgb, 80.0F}, {4000U},
                               expected, error))
        << error;
    ASSERT_TRUE(
        mapper.Convert({2U, 2U, 24U, padded, open_st::HdrPixelFormat::Rgba16FloatScRgb, 80.0F}, {4000U}, actual, error))
        << error;
    EXPECT_EQ(actual, expected);
}

// 记录同一进程 4K 首次与热运行的真实耗时及显式释放前后内存，不把硬件性能设为固定门槛。
// 入参：无运行时形参；宏参数 NativeToneMapperIntegrationTest
// 为测试套件，reports_4k_conversion_timing_and_resource_release 为用例名。 返回：无返回值；断言向 GoogleTest
// 报告该用例通过或失败。
TEST_F(NativeToneMapperIntegrationTest, reports_4k_conversion_timing_and_resource_release)
{
    constexpr std::uint32_t WIDTH = 3840U;
    constexpr std::uint32_t HEIGHT = 2160U;
    const std::array<std::uint16_t, 4> rgba{0x4280U, 0x4280U, 0x4280U, 0x3C00U};
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(WIDTH) * HEIGHT * 8U);
    for (std::size_t offset = 0U; offset < pixels.size(); offset += 8U)
    {
        std::memcpy(pixels.data() + offset, rgba.data(), 8U);
    }
    PROCESS_MEMORY_COUNTERS_EX before{};
    before.cb = sizeof(before);
    ASSERT_NE(K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&before),
                                      sizeof(before)),
              FALSE);
    open_st::NativeToneMapper mapper;
    std::vector<std::uint8_t> output;
    std::wstring error;
    const open_st::HdrImageView view{
        WIDTH, HEIGHT, static_cast<std::size_t>(WIDTH) * 8U, pixels, open_st::HdrPixelFormat::Rgba16FloatScRgb, 80.0F};
    std::array<double, 4> milliseconds{};
    for (std::size_t iteration = 0U; iteration < milliseconds.size(); ++iteration)
    {
        const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        ASSERT_TRUE(mapper.Convert(view, {4000U}, output, error)) << error;
        milliseconds[iteration] =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        ASSERT_EQ(output.size(), static_cast<std::size_t>(WIDTH) * HEIGHT * 4U);
    }
    PROCESS_MEMORY_COUNTERS_EX converted{};
    converted.cb = sizeof(converted);
    ASSERT_NE(K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&converted),
                                      sizeof(converted)),
              FALSE);
    mapper.ReleaseImageResources();
    std::vector<std::uint8_t>().swap(output);
    std::vector<std::uint8_t>().swap(pixels);
    PROCESS_MEMORY_COUNTERS_EX released{};
    released.cb = sizeof(released);
    ASSERT_NE(K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&released),
                                      sizeof(released)),
              FALSE);
    std::cout << "HDR 4K conversion ms: first=" << milliseconds[0] << ", warm=" << milliseconds[1] << ','
              << milliseconds[2] << ',' << milliseconds[3] << "; working set bytes: before=" << before.WorkingSetSize
              << ", converted=" << converted.WorkingSetSize << ", released=" << released.WorkingSetSize
              << "; private bytes: before=" << before.PrivateUsage << ", converted=" << converted.PrivateUsage
              << ", released=" << released.PrivateUsage << '\n';
}

// 验证 RGB10A2 解码后与等价 FP16 输入逐字节一致；黑位质量由独立 1000 nit 灰阶测试约束。
// 入参：无运行时形参；宏参数 NativeToneMapperIntegrationTest 为测试套件，converts_rgb10_scrgb_and_hdr10 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
TEST_F(NativeToneMapperIntegrationTest, converts_rgb10_scrgb_and_hdr10)
{
    const std::array<std::uint32_t, 2> packed{0U, 0x3FFFFFFFU};
    std::vector<std::uint8_t> pixels(sizeof(packed));
    std::memcpy(pixels.data(), packed.data(), pixels.size());
    open_st::NativeToneMapper mapper;
    for (const open_st::HdrPixelFormat format :
         {open_st::HdrPixelFormat::Rgb10A2ScRgb, open_st::HdrPixelFormat::Rgb10A2Hdr10})
    {
        SCOPED_TRACE(static_cast<int>(format));
        std::wstring error;
        std::vector<std::uint8_t> output;
        ASSERT_TRUE(mapper.Convert({2U, 1U, 8U, pixels, format, 80.0F}, {4000U}, output, error)) << error;
        ASSERT_EQ(output.size(), 8U);
        const std::array<float, 2> levels{0.0F, format == open_st::HdrPixelFormat::Rgb10A2ScRgb ? 1.0F : 125.0F};
        const std::vector<std::uint8_t> referencePixels = MakeGrayPixels(levels);
        std::vector<std::uint8_t> reference;
        ASSERT_TRUE(mapper.Convert({2U, 1U, 16U, referencePixels, open_st::HdrPixelFormat::Rgba16FloatScRgb, 80.0F},
                                   {4000U}, reference, error))
            << error;
        std::cout << "RGB10 format=" << static_cast<int>(format)
                  << ", black B=" << static_cast<unsigned int>(output[0U])
                  << ", FP16 equivalent black B=" << static_cast<unsigned int>(reference[0U]) << '\n';
        std::cout << "Synthetic black/white BGRA8=";
        for (const std::uint8_t channel : output)
        {
            std::cout << static_cast<unsigned int>(channel) << ' ';
        }
        std::cout << '\n';
        // HDR10 码值 1023 为 10000 nit；三格式最终进入相同肩部效果。
        EXPECT_EQ(output, reference);
        EXPECT_GT(output[4U], output[0U]);
        EXPECT_NEAR(output[4U], output[5U], 2.0);
        EXPECT_NEAR(output[5U], output[6U], 2.0);
        EXPECT_EQ(output[3U], 255U);
        EXPECT_EQ(output[7U], 255U);
    }
}

// 验证默认值字节保真、亮度单调以及非法参数不会偷偷换档。
// 入参：无。返回：断言真实D2D输出及参数回退。
TEST_F(NativeToneMapperIntegrationTest, brightness_preserves_default_and_scales_monotonically)
{
    open_st::NativeToneMapper mapper;
    const std::array<float, 4> levels{0.15F, 0.5F, 1.0F, 3.25F};
    const std::vector<std::uint8_t> pixels = MakeGrayPixels(levels);
    const open_st::HdrImageView view{4U, 1U, 32U, pixels, open_st::HdrPixelFormat::Rgba16FloatScRgb, 80.0F};
    std::wstring error;
    std::vector<std::uint8_t> original, neutral, dark, bright, invalid;
    ASSERT_TRUE(mapper.Convert(view, {4000U}, original, error)) << error;
    ASSERT_TRUE(mapper.SetBrightnessPercent(100U));
    ASSERT_TRUE(mapper.Convert(view, {4000U}, neutral, error)) << error;
    EXPECT_EQ(original, neutral);
    ASSERT_TRUE(mapper.SetBrightnessPercent(25U));
    ASSERT_TRUE(mapper.Convert(view, {4000U}, dark, error)) << error;
    ASSERT_TRUE(mapper.SetBrightnessPercent(200U));
    ASSERT_TRUE(mapper.Convert(view, {4000U}, bright, error)) << error;
    EXPECT_FALSE(mapper.SetBrightnessPercent(24U));
    EXPECT_FALSE(mapper.SetBrightnessPercent(201U));
    ASSERT_TRUE(mapper.Convert(view, {4000U}, invalid, error)) << error;
    EXPECT_EQ(invalid, bright);
    for (std::size_t i = 0; i < levels.size(); ++i)
    {
        EXPECT_LT(dark[i * 4U], neutral[i * 4U]);
        EXPECT_GE(bright[i * 4U], neutral[i * 4U]);
    }
}

// 验证已知SDR灰阶构造的HDR边缘没有空间重采样，并输出相对边缘权重用于画质诊断。
// 入参：无。返回：断言仅验证尺寸、邻域独立性及端点；输出指标不冒充文字画质已经通过。
TEST_F(NativeToneMapperIntegrationTest, known_sdr_edges_have_no_spatial_resampling)
{
    const std::array<unsigned int, 9> reference{0U, 32U, 64U, 96U, 128U, 160U, 192U, 224U, 255U};
    open_st::NativeToneMapper mapper;
    for (const float whiteNits : {80.0F, 203.0F, 260.0F})
    {
        std::vector<float> levels;
        for (const unsigned int sample : reference)
            levels.push_back(open_st::SrgbToLinear(static_cast<float>(sample) / 255.0F) * whiteNits / 80.0F);
        levels.push_back(12.5F); // 同一图内保留1000nit高光，模拟SDR文字与HDR内容共存。
        for (std::size_t index = reference.size(); index > 0U; --index)
            levels.push_back(levels[index - 1U]);
        levels.push_back(12.5F);
        const std::vector<std::uint8_t> pixels = MakeGrayPixels(levels);
        for (const unsigned int brightness : {50U, 100U, 150U})
        {
            ASSERT_TRUE(mapper.SetBrightnessPercent(brightness));
            std::wstring error;
            std::vector<std::uint8_t> line, grid;
            ASSERT_TRUE(mapper.Convert({20U, 1U, 160U, pixels, open_st::HdrPixelFormat::Rgba16FloatScRgb, whiteNits},
                                       {4000U}, line, error))
                << error;
            ASSERT_TRUE(mapper.Convert({10U, 2U, 80U, pixels, open_st::HdrPixelFormat::Rgba16FloatScRgb, whiteNits},
                                       {4000U}, grid, error))
                << error;
            ASSERT_EQ(grid.size(), 80U);
            EXPECT_EQ(line, grid); // 相同像素改换行与相邻顺序，不得产生插值或改变灰阶。
            EXPECT_LE(grid[0U], 4U);
            EXPECT_GT(grid[32U], grid[16U]);
            const float foreground = open_st::SrgbToLinear(static_cast<float>(grid[0U]) / 255.0F);
            const float background = open_st::SrgbToLinear(static_cast<float>(grid[32U]) / 255.0F);
            ASSERT_GT(background, foreground);
            for (std::size_t index = 0U; index < reference.size(); ++index)
            {
                const unsigned int output = grid[index * 4U];
                const float mapped = open_st::SrgbToLinear(static_cast<float>(output) / 255.0F);
                const float expectedWeight =
                    1.0F - open_st::SrgbToLinear(static_cast<float>(reference[index]) / 255.0F);
                const float actualWeight = (background - mapped) / (background - foreground);
                std::cout << "HDR_EDGE white=" << whiteNits << " brightness=" << brightness
                          << " reference=" << reference[index] << " output=" << output
                          << " expected_weight=" << expectedWeight << " mapped_weight=" << actualWeight << '\n';
            }
        }
    }
}
