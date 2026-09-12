// 文件职责：按显式开关测量 4K 像素搬运，比较逐像素参考与当前捕获、预览路径，不设置性能通过阈值。

#include "captured_plane_writer.h"
#include <desktop_preview.h>
#include <gtest/gtest.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <vector>

#ifdef NDEBUG
namespace
{
// 以逐像素复制构造 identity 紧凑缓冲，作为无旋转复制优化的对照。
// 入参：surface：有效且有完整借用内存的源表面。
// 返回：紧凑原始字节；包括分配和清零，不包含产品输入校验及元数据构造。
std::vector<std::uint8_t> ReferenceCapture(const open_st::MappedCaptureSurface& surface)
{
    const std::size_t pixelBytes = open_st::CapturedBytesPerPixel(surface.format);
    const std::size_t stride = static_cast<std::size_t>(surface.width) * pixelBytes;
    std::vector<std::uint8_t> output(stride * static_cast<std::size_t>(surface.height));
    for (int y = 0; y < surface.height; ++y)
        for (int x = 0; x < surface.width; ++x)
            std::memcpy(output.data() + static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * pixelBytes,
                        surface.pixels + static_cast<std::size_t>(y) * surface.rowPitch +
                            static_cast<std::size_t>(x) * pixelBytes,
                        pixelBytes);
    return output;
}

// 以原有逐像素格式分派复制 SDR BGRA 或原生 FP16，作为预览遍历的对照。
// 入参：plane：有效的 BGRA SDR 或原生 scRGB FP16 帧。
// 返回：预览字节；FP16 只修改目标 alpha，包括分配清零，不含元数据校验。
std::vector<std::uint8_t> ReferencePreview(const open_st::CapturedOutputPlane& plane)
{
    const std::size_t pixelBytes = open_st::CapturedBytesPerPixel(plane.Format());
    std::vector<std::uint8_t> output(plane.Pixels().size());
    const std::uint8_t* source = plane.Pixels().data();
    for (int y = 0; y < plane.Height(); ++y)
    {
        for (int x = 0; x < plane.Width(); ++x)
        {
            const std::size_t offset =
                static_cast<std::size_t>(y) * plane.Stride() + static_cast<std::size_t>(x) * pixelBytes;
            if (plane.Format() == open_st::CapturedPixelFormat::Rgba16FloatScRgb)
            {
                std::memcpy(output.data() + offset, source + offset, 6U);
                constexpr std::uint16_t ALPHA = 0x3C00U;
                std::memcpy(output.data() + offset + 6U, &ALPHA, sizeof(ALPHA));
            }
            else
                std::memcpy(output.data() + offset, source + offset, 4U);
        }
    }
    return output;
}

// 同步计时一次内存工作，不将后续结果验证计入耗时。
// 入参：work：本次测量的同步操作，输出由调用方保存。
// 返回：steady_clock 测得的毫秒数。
template <typename Work> double Measure(Work work)
{
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    work();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

// 输出去掉预热后样本的中位耗时，避免把瞬时抖动当作确定收益。
// 入参：name：测量路径名称；samples：非空毫秒样本，按值传入供排序。
// 返回：无返回值，只写测试标准输出，不创建报告文件。
void Report(const char* name, std::vector<double> samples)
{
    std::sort(samples.begin(), samples.end());
    std::cout << name << " median_ms=" << samples[samples.size() / 2U] << '\n';
}
} // namespace
#endif

// 测量多行带 padding 的 4K BGRA/FP16 捕获与预览，默认跳过且仅接受 Release 显式运行。
// 入参：无运行入参；OPEN_ST_PIXEL_COPY_BENCHMARKS=1 启用，数据全部在内存构造。
// 返回：无返回值；验证两种路径的像素一致，输出中位耗时，不断言机器相关的加速比例。
TEST(PixelCopyBenchmark, compares_4k_capture_and_preview)
{
#ifndef NDEBUG
    GTEST_SKIP() << "Pixel copy measurements require a Release build.";
#else
    wchar_t enabled[2]{};
    if (GetEnvironmentVariableW(L"OPEN_ST_PIXEL_COPY_BENCHMARKS", enabled, 2U) != 1U || enabled[0] != L'1')
        GTEST_SKIP() << "Set OPEN_ST_PIXEL_COPY_BENCHMARKS=1 to run memory-only measurements.";
    constexpr int WIDTH = 3840;
    constexpr int HEIGHT = 2160;
    for (const open_st::CapturedPixelFormat format :
         {open_st::CapturedPixelFormat::Bgra8Unorm, open_st::CapturedPixelFormat::Rgba16FloatScRgb})
    {
        const std::size_t pixelBytes = open_st::CapturedBytesPerPixel(format);
        const std::size_t pitch = static_cast<std::size_t>(WIDTH) * pixelBytes + 32U;
        std::vector<std::uint8_t> source(pitch * HEIGHT);
        for (std::size_t index = 0; index < source.size(); ++index)
            source[index] = static_cast<std::uint8_t>(index * 31U + 7U);
        const open_st::MappedCaptureSurface surface{source.data(), pitch, WIDTH, HEIGHT, format};
        const open_st::CapturedColorSpace colorSpace = format == open_st::CapturedPixelFormat::Bgra8Unorm
                                                           ? open_st::CapturedColorSpace::SdrGamma22P709
                                                           : open_st::CapturedColorSpace::ScRgb;
        std::vector<double> captureReference;
        std::vector<double> captureCurrent;
        std::vector<double> previewReference;
        std::vector<double> previewCurrent;
        for (int run = 0; run < 10; ++run)
        {
            std::vector<std::uint8_t> reference;
            open_st::CapturedOutputPlane plane;
            open_st::OutputPreviewFrame preview;
            std::wstring error;
            // 测量包含分配的逐像素对照；借用表面在整个基准期间有效。
            // 入参：无；捕获本轮输出和输入表面。
            // 返回：无返回值，写入 reference。
            const double captureBaselineMs = Measure([&]() { reference = ReferenceCapture(surface); });
            bool captured = false;
            // 测量完整产品 plane 构造，包含校验、分配和复制。
            // 入参：无；捕获本轮输入及输出。
            // 返回：无返回值，记录构造结果及 plane。
            const double captureMs = Measure(
                [&]()
                {
                    captured = open_st::BuildCapturedOutputPlane(surface, {0, 0, WIDTH, HEIGHT},
                                                                 open_st::CapturedSurfaceRotation::Identity, colorSpace,
                                                                 {}, plane, error);
                });
            ASSERT_TRUE(captured);
            ASSERT_TRUE(std::equal(reference.begin(), reference.end(), plane.Pixels().begin(), plane.Pixels().end()));
            reference.clear();
            reference.shrink_to_fit();
            // 测量保留原有格式分派的逐像素预览对照。
            // 入参：无；捕获已构造 plane 和本轮参考输出。
            // 返回：无返回值，写入 reference。
            const double previewBaselineMs = Measure([&]() { reference = ReferencePreview(plane); });
            bool converted = false;
            // 测量完整产品预览路径，包含校验、分配和格式处理。
            // 入参：无；捕获 plane 和本轮预览输出。
            // 返回：无返回值，记录预览构造结果。
            const double previewMs = Measure([&]() { converted = open_st::BuildOutputPreview(plane, preview, error); });
            ASSERT_TRUE(converted);
            ASSERT_EQ(reference, preview.pixels);
            if (run != 0)
            {
                captureReference.push_back(captureBaselineMs);
                captureCurrent.push_back(captureMs);
                previewReference.push_back(previewBaselineMs);
                previewCurrent.push_back(previewMs);
            }
        }
        std::cout << "format=" << (pixelBytes == 4U ? "BGRA8" : "FP16") << " size=3840x2160 samples=9\n";
        Report("capture_pixel_reference", captureReference);
        Report("capture_current_full_build", captureCurrent);
        Report("preview_pixel_reference", previewReference);
        Report("preview_current_full_build", previewCurrent);
    }
#endif
}
