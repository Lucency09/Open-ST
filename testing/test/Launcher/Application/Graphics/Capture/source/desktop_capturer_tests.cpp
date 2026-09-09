// 文件职责：在可捕获的桌面环境中验证冻结帧结构、颜色元数据及原生像素，并区分环境限制。

#include <gtest/gtest.h>

#include <desktop_capturer.h>
#include <frozen_desktop_frame.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <string>
#include <windows.h>

namespace
{
// 将 Windows 宽字符错误转成 UTF-8，以便在捕获测试失败信息中输出。
// 入参：text：待转换的宽字符字符串。
// 返回：对应 UTF-8 字符串；空输入或转换失败时返回空字符串。
std::string WideToUtf8(const std::wstring& text)
{
    if (text.empty())
    {
        return {};
    }

    const int size =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0)
    {
        return "Unable to convert native error message to UTF-8.";
    }

    std::string converted(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), converted.data(), size, nullptr,
                        nullptr);
    return converted;
}

// 检查真实捕获测试所需的 Windows 显示和输入桌面环境。
// 入参：skipReason：输出参数，环境不可用时写入可跳过原因。
// 返回：存在显示输出且可访问输入桌面时为 true；否则为 false 并提供跳过原因。
bool IsDesktopCapturePlatformAvailable(std::string& skipReason)
{
    if (GetSystemMetrics(SM_CMONITORS) <= 0)
    {
        skipReason = "No active display monitor is available.";
        return false;
    }

    HDESK inputDesktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (inputDesktop == nullptr)
    {
        skipReason = "No interactive input desktop is available.";
        return false;
    }

    CloseDesktop(inputDesktop);
    return true;
}

// 按实际像素格式检测冻结 plane 是否包含非零 RGB，辅助发现全黑假成功。
// 入参：plane：待检查的只读原生捕获 plane。
// 返回：任一像素包含非零 RGB 通道时为 true；未发现颜色或格式未支持时为 false。
bool PlaneContainsColor(const open_st::CapturedOutputPlane& plane)
{
    const std::span<const std::uint8_t> pixels = plane.Pixels();
    const std::size_t bytesPerPixel = open_st::CapturedBytesPerPixel(plane.Format());
    for (std::size_t offset = 0U; offset + bytesPerPixel <= pixels.size(); offset += bytesPerPixel)
    {
        if (plane.Format() == open_st::CapturedPixelFormat::Bgra8Unorm &&
            (pixels[offset] != 0U || pixels[offset + 1U] != 0U || pixels[offset + 2U] != 0U))
        {
            return true;
        }
        if (plane.Format() == open_st::CapturedPixelFormat::Rgb10A2Unorm)
        {
            std::uint32_t packed{};
            std::memcpy(&packed, pixels.data() + offset, sizeof(packed));
            if ((packed & 0x3FFFFFFFU) != 0U)
            {
                return true;
            }
        }
        if (plane.Format() == open_st::CapturedPixelFormat::Rgba16FloatScRgb)
        {
            std::uint16_t channels[3]{};
            std::memcpy(channels, pixels.data() + offset, sizeof(channels));
            if ((channels[0] & 0x7FFFU) != 0U || (channels[1] & 0x7FFFU) != 0U ||
                (channels[2] & 0x7FFFU) != 0U)
            {
                return true;
            }
        }
    }
    return false;
}
} // namespace

// 在具备显示器和交互式桌面的 Windows 会话中执行一次真实的虚拟桌面捕获；验证冻结桌面和每个原生 plane 有效；HDR 输出若降级 BGRA8，必须显式记录系统 SDR
// 转换；无显示器或无交互式桌面时属于平台条件不满足，测试会明确标记为 Skipped，而不是失败。
// 入参：无运行时形参；宏参数 DesktopCapturerIntegrationTest 为测试套件，captures_virtual_desktop 为用例名。
// 返回：无返回值；断言向 GoogleTest 报告该用例通过或失败，平台前提不满足时可标记跳过。
TEST(DesktopCapturerIntegrationTest, captures_virtual_desktop)
{
    std::string skipReason;
    if (!IsDesktopCapturePlatformAvailable(skipReason))
    {
        GTEST_SKIP() << skipReason;
    }

    // 与产品入口使用相同 DPI 模式，确保虚拟桌面尺寸和负坐标验证具有相同语义。
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    open_st::DesktopCapturer capturer;
    open_st::FrozenDesktopFrame frame;
    std::wstring errorMessage;

    const bool captured = capturer.Capture(frame, errorMessage);
    ASSERT_TRUE(captured) << "Desktop capture failed: " << WideToUtf8(errorMessage);
    ASSERT_TRUE(frame.IsValid());
    ASSERT_FALSE(frame.Outputs().empty());

    bool containsColor = false;
    for (const open_st::CapturedOutputPlane& output : frame.Outputs())
    {
        ASSERT_TRUE(output.IsValid());
        EXPECT_GT(output.Width(), 0);
        EXPECT_GT(output.Height(), 0);
        EXPECT_EQ(output.Stride(), static_cast<std::size_t>(output.Width()) *
                                           open_st::CapturedBytesPerPixel(output.Format()));
        containsColor = containsColor || PlaneContainsColor(output);
        const bool isExtendedDisplay =
            output.ColorMetadata().displayColorSpace == open_st::CapturedColorSpace::Hdr10 ||
            output.ColorMetadata().displayColorSpace == open_st::CapturedColorSpace::ScRgb;
        if (output.Format() == open_st::CapturedPixelFormat::Bgra8Unorm)
        {
            EXPECT_EQ(output.PixelColorSpace(), open_st::CapturedColorSpace::SdrGamma22P709);
            EXPECT_EQ(output.ColorMetadata().systemConvertedToSdr, isExtendedDisplay);
        }
        else
        {
            EXPECT_FALSE(output.ColorMetadata().systemConvertedToSdr);
            EXPECT_FALSE(output.ColorMetadata().usedLegacyDuplication);
        }
    }

    std::cout << "Captured " << frame.Outputs().size() << " native output plane(s) in virtual bounds "
              << frame.Bounds().Width() << 'x' << frame.Bounds().Height() << std::endl;
    if (!containsColor)
    {
        std::cout << "Captured native planes are fully black; this is legal desktop content and is not treated as "
                     "an API failure."
                  << std::endl;
    }
    EXPECT_TRUE(errorMessage.empty());
}
