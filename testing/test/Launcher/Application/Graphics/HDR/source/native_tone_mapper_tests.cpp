// 文件职责：验证原生 HDR 色调映射的输出、行跨度、输入拒绝和重复调用，并识别图形环境限制。

#include <gtest/gtest.h>

#include <color_conversion.h>
#include <native_tone_mapper.h>

#include <algorithm>
#include <array>
#include <chrono>
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

// 验证零尺寸、未知格式、短 stride、短缓冲和溢出尺寸明确失败并清空旧输出。
// 入参：无运行时形参；宏参数 NativeToneMapperValidationTest 为测试套件，rejects_invalid_views_and_clears_output 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
TEST(NativeToneMapperValidationTest, rejects_invalid_views_and_clears_output)
{
    open_st::NativeToneMapper mapper;
    const std::array<float, 1> levels{1.0F};
    const std::vector<std::uint8_t> pixels = MakeGrayPixels(levels);
    const open_st::HdrImageView valid{1U, 1U, 8U, pixels};
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
        EXPECT_FALSE(mapper.Convert(view, output, error));
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
            EXPECT_FALSE(mapper.Convert({1U, 1U, 8U, pixels}, output, error));
            EXPECT_TRUE(output.empty());
            EXPECT_FALSE(error.empty());
        }
    }
}

// 验证黑位、灰阶单调、260 nit 桌面白和更亮高光；只约束可用范围，不锁定驱动曲线。
// 入参：无运行时形参；宏参数 NativeToneMapperIntegrationTest 为测试套件，preserves_black_gray_order_and_highlight_separation 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
TEST_F(NativeToneMapperIntegrationTest, preserves_black_gray_order_and_highlight_separation)
{
    const std::array<float, 7> levels{0.0F, 0.18F, 1.0F, 2.0F, 3.25F, 5.0F, 12.5F};
    const std::vector<std::uint8_t> pixels = MakeGrayPixels(levels);
    open_st::NativeToneMapper mapper;
    std::wstring error;
    ASSERT_TRUE(mapper.Prepare(error)) << error;
    std::vector<std::uint8_t> output;
    ASSERT_TRUE(mapper.Convert({7U, 1U, 56U, pixels}, output, error)) << error;
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
    ASSERT_TRUE(mapper.Convert({7U, 1U, 56U, pixels}, repeated, error)) << error;
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
    ASSERT_TRUE(mapper.Convert({1U, 1U, 8U, pixels}, output, error)) << error;
    ASSERT_EQ(output.size(), 4U);
    EXPECT_EQ(output[3U], 255U);
    EXPECT_GT(output[2U], output[1U]);
}

// 验证带行尾填充的 FP16 输入不把 padding 当像素，并在不同尺寸间复用同一转换器。
// 入参：无运行时形参；宏参数 NativeToneMapperIntegrationTest 为测试套件，respects_padded_stride_and_changes_dimensions 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
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
    ASSERT_TRUE(mapper.Convert({4U, 1U, 32U, tight}, expected, error)) << error;
    ASSERT_TRUE(mapper.Convert({2U, 2U, 24U, padded}, actual, error)) << error;
    EXPECT_EQ(actual, expected);
}

// 记录同一进程 4K 首次与热运行的真实耗时及显式释放前后内存，不把硬件性能设为固定门槛。
// 入参：无运行时形参；宏参数 NativeToneMapperIntegrationTest 为测试套件，reports_4k_conversion_timing_and_resource_release 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
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
    const open_st::HdrImageView view{WIDTH, HEIGHT, static_cast<std::size_t>(WIDTH) * 8U, pixels};
    std::array<double, 4> milliseconds{};
    for (std::size_t iteration = 0U; iteration < milliseconds.size(); ++iteration)
    {
        const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        ASSERT_TRUE(mapper.Convert(view, output, error)) << error;
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
        ASSERT_TRUE(mapper.Convert({2U, 1U, 8U, pixels, format}, output, error)) << error;
        ASSERT_EQ(output.size(), 8U);
        const std::array<float, 2> levels{0.0F, format == open_st::HdrPixelFormat::Rgb10A2ScRgb ? 1.0F : 125.0F};
        const std::vector<std::uint8_t> referencePixels = MakeGrayPixels(levels);
        std::vector<std::uint8_t> reference;
        ASSERT_TRUE(mapper.Convert({2U, 1U, 16U, referencePixels}, reference, error)) << error;
        std::cout << "RGB10 format=" << static_cast<int>(format)
                  << ", black B=" << static_cast<unsigned int>(output[0U])
                  << ", FP16 equivalent black B=" << static_cast<unsigned int>(reference[0U]) << '\n';
        std::cout << "Synthetic black/white BGRA8=";
        for (const std::uint8_t channel : output)
        {
            std::cout << static_cast<unsigned int>(channel) << ' ';
        }
        std::cout << '\n';
        // HDR10 码值 1023 为 10000 nit；原生链在该峰值下可能产生近黑偏置，不能归咎于格式解码。
        EXPECT_EQ(output, reference);
        EXPECT_GT(output[4U], output[0U]);
        EXPECT_NEAR(output[4U], output[5U], 2.0);
        EXPECT_NEAR(output[5U], output[6U], 2.0);
        EXPECT_EQ(output[3U], 255U);
        EXPECT_EQ(output[7U], 255U);
    }
}
