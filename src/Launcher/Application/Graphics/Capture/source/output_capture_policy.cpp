// 文件职责：实现捕获格式优先级、受控兼容回退及像素颜色空间和旋转映射策略。

#include "output_capture_policy.h"

#include <array>

namespace open_st
{
// 提供 Desktop Duplication 的原生格式协商优先级。
// 入参：无。
// 返回：静态格式数组的只读视图，高色深格式位于兼容 BGRA8 之前。
std::span<const DXGI_FORMAT> PreferredDuplicationFormats() noexcept
{
    static constexpr std::array formats{
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        DXGI_FORMAT_R10G10B10A2_UNORM,
        DXGI_FORMAT_B8G8R8A8_UNORM,
    };
    return formats;
}

// 判断高色深捕获失败是否符合已批准的旧接口回退条件。
// 入参：output5QueryResult：查询 IDXGIOutput5 的 HRESULT；duplicateOutput1Result：DuplicateOutput1 的 HRESULT。
// 返回：接口不存在或高色深复制明确不支持时为 true，其他结果为 false。
bool ShouldUseLegacyDuplication(HRESULT output5QueryResult, HRESULT duplicateOutput1Result) noexcept
{
    return output5QueryResult == E_NOINTERFACE ||
           (SUCCEEDED(output5QueryResult) && duplicateOutput1Result == DXGI_ERROR_UNSUPPORTED);
}

// 将 DXGI 输出颜色空间转换为冻结帧使用的稳定枚举。
// 入参：colorSpace：DXGI 输出报告的颜色空间。
// 返回：对应的 SDR、scRGB 或 HDR10 枚举；不支持的值返回 Unknown。
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

// 将桌面复制实际返回的 DXGI 格式映射为原生帧格式。
// 入参：format：DXGI 像素格式；mappedFormat：输出参数，成功时写入对应捕获格式。
// 返回：支持 BGRA8、RGB10A2 或 FP16 时为 true；未知格式为 false 且不改 mappedFormat。
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

// 结合像素格式与显示颜色空间确定冻结像素的实际解码语义。
// 入参：format：桌面复制取得的像素格式；displayColorSpace：显示输出报告的颜色空间。
// 返回：BGRA8 对应 SDR，FP16 对应 scRGB，RGB10A2 沿用显示颜色空间；未知格式返回 Unknown。
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

// 把 DXGI 旋转枚举转换为纯像素复制使用的旋转枚举。
// 入参：rotation：DXGI 报告的表面旋转；mappedRotation：输出参数，成功时写入对应旋转方式。
// 返回：旋转受支持时为 true；未知值为 false 且不修改 mappedRotation。
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

// 识别 HDR 显示输出是否实际只取得系统已转换的 SDR 数据。
// 入参：format：实际捕获格式；displayColorSpace：显示输出报告的颜色空间。
// 返回：HDR10 或 scRGB 显示输出实际返回 BGRA8 时为 true，其余为 false。
bool WasSystemConvertedToSdr(CapturedPixelFormat format, CapturedColorSpace displayColorSpace) noexcept
{
    return format == CapturedPixelFormat::Bgra8Unorm &&
           (displayColorSpace == CapturedColorSpace::ScRgb ||
            displayColorSpace == CapturedColorSpace::Hdr10);
}
} // namespace open_st
