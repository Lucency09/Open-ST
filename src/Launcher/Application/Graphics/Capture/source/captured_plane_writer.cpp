#include "captured_plane_writer.h"

#include <cstring>
#include <exception>
#include <limits>
#include <utility>
#include <vector>

namespace
{
// 验证源表面尺寸与旋转后的桌面边界尺寸一致。
bool DimensionsMatch(const open_st::MappedCaptureSurface& surface, open_st::RectI bounds,
                     open_st::CapturedSurfaceRotation rotation) noexcept
{
    if (surface.width <= 0 || surface.height <= 0 || bounds.IsEmpty())
    {
        return false;
    }

    if (rotation == open_st::CapturedSurfaceRotation::Rotate90 ||
        rotation == open_st::CapturedSurfaceRotation::Rotate270)
    {
        return surface.width == bounds.Height() && surface.height == bounds.Width();
    }
    return surface.width == bounds.Width() && surface.height == bounds.Height();
}

// 将桌面方向目标像素坐标反算为未旋转源表面坐标。
bool MapTargetToSource(int targetX, int targetY, int sourceWidth, int sourceHeight,
                       open_st::CapturedSurfaceRotation rotation, int& sourceX, int& sourceY) noexcept
{
    switch (rotation)
    {
    case open_st::CapturedSurfaceRotation::Identity:
        sourceX = targetX;
        sourceY = targetY;
        break;
    case open_st::CapturedSurfaceRotation::Rotate90:
        sourceX = targetY;
        sourceY = sourceHeight - 1 - targetX;
        break;
    case open_st::CapturedSurfaceRotation::Rotate180:
        sourceX = sourceWidth - 1 - targetX;
        sourceY = sourceHeight - 1 - targetY;
        break;
    case open_st::CapturedSurfaceRotation::Rotate270:
        sourceX = sourceWidth - 1 - targetY;
        sourceY = targetX;
        break;
    default:
        return false;
    }

    return sourceX >= 0 && sourceY >= 0 && sourceX < sourceWidth && sourceY < sourceHeight;
}
} // namespace

namespace open_st
{
// 复制有效像素并丢弃驱动行填充，输出始终使用桌面方向的紧凑顶向下布局。
bool BuildCapturedOutputPlane(const MappedCaptureSurface& surface, RectI desktopBounds,
                              CapturedSurfaceRotation rotation, CapturedColorSpace pixelColorSpace,
                              OutputColorMetadata metadata, CapturedOutputPlane& output,
                              std::wstring& errorMessage)
{
    output = {};
    errorMessage.clear();
    const std::size_t bytesPerPixel = CapturedBytesPerPixel(surface.format);
    if (surface.pixels == nullptr || bytesPerPixel == 0U || !DimensionsMatch(surface, desktopBounds, rotation))
    {
        errorMessage = L"捕获表面尺寸、格式或内存地址无效。";
        return false;
    }

    const std::size_t sourceWidth = static_cast<std::size_t>(surface.width);
    if (sourceWidth > std::numeric_limits<std::size_t>::max() / bytesPerPixel ||
        surface.rowPitch < sourceWidth * bytesPerPixel)
    {
        errorMessage = L"捕获表面的 RowPitch 小于有效像素行。";
        return false;
    }

    const std::size_t targetWidth = static_cast<std::size_t>(desktopBounds.Width());
    const std::size_t targetHeight = static_cast<std::size_t>(desktopBounds.Height());
    if (targetWidth > std::numeric_limits<std::size_t>::max() / bytesPerPixel ||
        targetHeight > std::numeric_limits<std::size_t>::max() / (targetWidth * bytesPerPixel))
    {
        errorMessage = L"冻结显示输出像素缓冲区大小溢出。";
        return false;
    }

    try
    {
        const std::size_t targetStride = targetWidth * bytesPerPixel;
        std::vector<std::uint8_t> pixels(targetStride * targetHeight);
        for (int targetY = 0; targetY < desktopBounds.Height(); ++targetY)
        {
            for (int targetX = 0; targetX < desktopBounds.Width(); ++targetX)
            {
                int sourceX{};
                int sourceY{};
                if (!MapTargetToSource(targetX, targetY, surface.width, surface.height, rotation, sourceX,
                                       sourceY))
                {
                    errorMessage = L"旋转后的显示像素超出捕获表面范围。";
                    return false;
                }

                const std::uint8_t* sourcePixel =
                    surface.pixels + static_cast<std::size_t>(sourceY) * surface.rowPitch +
                    static_cast<std::size_t>(sourceX) * bytesPerPixel;
                std::uint8_t* targetPixel = pixels.data() + static_cast<std::size_t>(targetY) * targetStride +
                                            static_cast<std::size_t>(targetX) * bytesPerPixel;
                std::memcpy(targetPixel, sourcePixel, bytesPerPixel);
            }
        }

        CapturedOutputPlane candidate(desktopBounds, surface.format, pixelColorSpace, metadata, std::move(pixels));
        if (!candidate.IsValid())
        {
            errorMessage = L"冻结显示输出没有形成有效的紧凑像素 plane。";
            return false;
        }
        output = std::move(candidate);
        return true;
    }
    catch (const std::exception&)
    {
        errorMessage = L"无法为冻结显示输出分配原生像素缓冲区。";
        return false;
    }
}
} // namespace open_st
