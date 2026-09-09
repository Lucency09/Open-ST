// 文件职责：验证原生格式优先级、兼容回退条件以及颜色空间和旋转映射策略。

#include <gtest/gtest.h>

#include "output_capture_policy.h"

// 验证高色深格式协商顺序固定为 FP16、RGB10A2、兼容 BGRA8。
// 入参：无运行时形参；宏参数 OutputCapturePolicyTest 为测试套件，prefers_native_high_color_formats_before_bgra8 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
TEST(OutputCapturePolicyTest, prefers_native_high_color_formats_before_bgra8)
{
    const std::span<const DXGI_FORMAT> formats = open_st::PreferredDuplicationFormats();
    ASSERT_EQ(formats.size(), 3U);
    EXPECT_EQ(formats[0], DXGI_FORMAT_R16G16B16A16_FLOAT);
    EXPECT_EQ(formats[1], DXGI_FORMAT_R10G10B10A2_UNORM);
    EXPECT_EQ(formats[2], DXGI_FORMAT_B8G8R8A8_UNORM);
}

// 验证只有 Output5 接口缺失或 DuplicateOutput1 明确不支持时允许进入旧复制路径。
// 入参：无运行时形参；宏参数 OutputCapturePolicyTest 为测试套件，permits_legacy_fallback_only_for_two_approved_results 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
TEST(OutputCapturePolicyTest, permits_legacy_fallback_only_for_two_approved_results)
{
    EXPECT_TRUE(open_st::ShouldUseLegacyDuplication(E_NOINTERFACE, E_FAIL));
    EXPECT_TRUE(open_st::ShouldUseLegacyDuplication(S_OK, DXGI_ERROR_UNSUPPORTED));
    EXPECT_FALSE(open_st::ShouldUseLegacyDuplication(E_ACCESSDENIED, E_FAIL));
    EXPECT_FALSE(open_st::ShouldUseLegacyDuplication(S_OK, DXGI_ERROR_NOT_CURRENTLY_AVAILABLE));
    EXPECT_FALSE(open_st::ShouldUseLegacyDuplication(S_OK, DXGI_ERROR_SESSION_DISCONNECTED));
    EXPECT_FALSE(open_st::ShouldUseLegacyDuplication(S_OK, DXGI_ERROR_DEVICE_REMOVED));
}

// 验证 DXGI 格式和颜色空间映射保持精确语义，并拒绝未支持的返回格式。
// 入参：无运行时形参；宏参数 OutputCapturePolicyTest 为测试套件，maps_supported_formats_and_color_spaces 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
TEST(OutputCapturePolicyTest, maps_supported_formats_and_color_spaces)
{
    open_st::CapturedPixelFormat format{};
    EXPECT_TRUE(open_st::TryMapCapturedPixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, format));
    EXPECT_EQ(format, open_st::CapturedPixelFormat::Bgra8Unorm);
    EXPECT_TRUE(open_st::TryMapCapturedPixelFormat(DXGI_FORMAT_R10G10B10A2_UNORM, format));
    EXPECT_EQ(format, open_st::CapturedPixelFormat::Rgb10A2Unorm);
    EXPECT_TRUE(open_st::TryMapCapturedPixelFormat(DXGI_FORMAT_R16G16B16A16_FLOAT, format));
    EXPECT_EQ(format, open_st::CapturedPixelFormat::Rgba16FloatScRgb);
    EXPECT_FALSE(open_st::TryMapCapturedPixelFormat(DXGI_FORMAT_R8G8B8A8_UNORM, format));

    EXPECT_EQ(open_st::MapCapturedColorSpace(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709),
              open_st::CapturedColorSpace::SdrGamma22P709);
    EXPECT_EQ(open_st::MapCapturedColorSpace(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709),
              open_st::CapturedColorSpace::ScRgb);
    EXPECT_EQ(open_st::MapCapturedColorSpace(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020),
              open_st::CapturedColorSpace::Hdr10);
    EXPECT_EQ(open_st::MapCapturedColorSpace(DXGI_COLOR_SPACE_CUSTOM),
              open_st::CapturedColorSpace::Unknown);
}

// 验证 plane 像素颜色空间、旋转映射和系统 SDR 转换标记互不混淆。
// 入参：无运行时形参；宏参数 OutputCapturePolicyTest 为测试套件，resolves_plane_semantics_without_conflating_legacy_usage 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败。
TEST(OutputCapturePolicyTest, resolves_plane_semantics_without_conflating_legacy_usage)
{
    EXPECT_EQ(open_st::ResolveCapturedPixelColorSpace(open_st::CapturedPixelFormat::Bgra8Unorm,
                                                      open_st::CapturedColorSpace::Hdr10),
              open_st::CapturedColorSpace::SdrGamma22P709);
    EXPECT_EQ(open_st::ResolveCapturedPixelColorSpace(open_st::CapturedPixelFormat::Rgba16FloatScRgb,
                                                      open_st::CapturedColorSpace::Hdr10),
              open_st::CapturedColorSpace::ScRgb);
    EXPECT_EQ(open_st::ResolveCapturedPixelColorSpace(open_st::CapturedPixelFormat::Rgb10A2Unorm,
                                                      open_st::CapturedColorSpace::Hdr10),
              open_st::CapturedColorSpace::Hdr10);
    EXPECT_FALSE(open_st::WasSystemConvertedToSdr(open_st::CapturedPixelFormat::Bgra8Unorm,
                                                  open_st::CapturedColorSpace::SdrGamma22P709));
    EXPECT_TRUE(open_st::WasSystemConvertedToSdr(open_st::CapturedPixelFormat::Bgra8Unorm,
                                                 open_st::CapturedColorSpace::Hdr10));
    EXPECT_FALSE(open_st::WasSystemConvertedToSdr(open_st::CapturedPixelFormat::Rgba16FloatScRgb,
                                                  open_st::CapturedColorSpace::Hdr10));

    open_st::CapturedSurfaceRotation rotation{};
    EXPECT_TRUE(open_st::TryMapCapturedRotation(DXGI_MODE_ROTATION_UNSPECIFIED, rotation));
    EXPECT_EQ(rotation, open_st::CapturedSurfaceRotation::Identity);
    EXPECT_TRUE(open_st::TryMapCapturedRotation(DXGI_MODE_ROTATION_ROTATE270, rotation));
    EXPECT_EQ(rotation, open_st::CapturedSurfaceRotation::Rotate270);
    EXPECT_FALSE(open_st::TryMapCapturedRotation(static_cast<DXGI_MODE_ROTATION>(99), rotation));
}
