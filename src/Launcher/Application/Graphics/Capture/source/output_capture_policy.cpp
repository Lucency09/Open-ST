#include "output_capture_policy.h"

#include <array>

namespace open_st
{
// 返回进程期稳定的高色深优先协商表，调用方只借用其只读存储。
std::span<const DXGI_FORMAT> PreferredDuplicationFormats() noexcept
{
    static constexpr std::array formats{
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        DXGI_FORMAT_R10G10B10A2_UNORM,
        DXGI_FORMAT_B8G8R8A8_UNORM,
    };
    return formats;
}

// 把接口查询和高色深会话结果收敛为严格的兼容降级判定。
bool ShouldUseLegacyDuplication(HRESULT output5QueryResult, HRESULT duplicateOutput1Result) noexcept
{
    return output5QueryResult == E_NOINTERFACE ||
           (SUCCEEDED(output5QueryResult) && duplicateOutput1Result == DXGI_ERROR_UNSUPPORTED);
}

// 把 Windows 常见 SDR、scRGB 和 HDR10 输出颜色空间映射到冻结模型。
CapturedColorSpace MapCapturedColorSpace(DXGI_COLOR_SPACE_TYPE colorSpace) noexcept
{
    switch (colorSpace)
    {
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:
        return CapturedColorSpace::SdrGamma22P709;
    case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
        return CapturedColorSpace::ScRgb;
    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
        return CapturedColorSpace::Hdr10;
    default:
        return CapturedColorSpace::Unknown;
    }
}

// 接受当前冻结模型能够逐字节保存的三个 DXGI 像素格式。
bool TryMapCapturedPixelFormat(DXGI_FORMAT format, CapturedPixelFormat& mappedFormat) noexcept
{
    switch (format)
    {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        mappedFormat = CapturedPixelFormat::Bgra8Unorm;
        return true;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        mappedFormat = CapturedPixelFormat::Rgb10A2Unorm;
        return true;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        mappedFormat = CapturedPixelFormat::Rgba16FloatScRgb;
        return true;
    default:
        return false;
    }
}

// 固定 BGRA8 为 sRGB、FP16 为 scRGB；RGB10A2 依赖显示输出报告的颜色空间。
CapturedColorSpace ResolveCapturedPixelColorSpace(CapturedPixelFormat format,
                                                   CapturedColorSpace displayColorSpace) noexcept
{
    switch (format)
    {
    case CapturedPixelFormat::Bgra8Unorm:
        return CapturedColorSpace::SdrGamma22P709;
    case CapturedPixelFormat::Rgba16FloatScRgb:
        return CapturedColorSpace::ScRgb;
    case CapturedPixelFormat::Rgb10A2Unorm:
        return displayColorSpace;
    default:
        return CapturedColorSpace::Unknown;
    }
}

// 接受 Desktop Duplication 定义的四种有效桌面表面方向。
bool TryMapCapturedRotation(DXGI_MODE_ROTATION rotation, CapturedSurfaceRotation& mappedRotation) noexcept
{
    switch (rotation)
    {
    case DXGI_MODE_ROTATION_UNSPECIFIED:
    case DXGI_MODE_ROTATION_IDENTITY:
        mappedRotation = CapturedSurfaceRotation::Identity;
        return true;
    case DXGI_MODE_ROTATION_ROTATE90:
        mappedRotation = CapturedSurfaceRotation::Rotate90;
        return true;
    case DXGI_MODE_ROTATION_ROTATE180:
        mappedRotation = CapturedSurfaceRotation::Rotate180;
        return true;
    case DXGI_MODE_ROTATION_ROTATE270:
        mappedRotation = CapturedSurfaceRotation::Rotate270;
        return true;
    default:
        return false;
    }
}

// 区分“使用旧接口”与“已发生 HDR→SDR”，避免普通 SDR 兼容路径被错误标记。
bool WasSystemConvertedToSdr(CapturedPixelFormat format, CapturedColorSpace displayColorSpace) noexcept
{
    return format == CapturedPixelFormat::Bgra8Unorm &&
           (displayColorSpace == CapturedColorSpace::ScRgb ||
            displayColorSpace == CapturedColorSpace::Hdr10);
}
} // namespace open_st
