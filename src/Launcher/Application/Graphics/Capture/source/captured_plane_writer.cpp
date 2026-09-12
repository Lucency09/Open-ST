// 文件职责：实现映射表面的旋转归一化与像素搬运，无旋转时按行复制，移除驱动填充并验证边界。

#include "captured_plane_writer.h"

#include <cstring>
#include <exception>
#include <limits>
#include <utility>
#include <vector>

namespace
{
// 验证旋转后的捕获表面尺寸能否精确对应桌面输出边界。
// 入参：surface：未旋转映射表面；bounds：桌面方向物理像素半开边界；rotation：源表面旋转方式。
// 返回：尺寸均为正且旋转后的宽高与边界匹配时为 true，否则为 false。
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

// 以编译期确定的旋转搬运完整像素，避免在每个像素中重复分派旋转。
// 入参：surface：已验证尺寸及行跨度的源表面；bounds：匹配旋转后的边界；bytesPerPixel：像素字节数；target：足量紧凑目标内存。
// 返回：无返回值；只复制原始位模式，不解码通道或复制行填充。
template <open_st::CapturedSurfaceRotation ROTATION>
void CopyRotatedPixels(const open_st::MappedCaptureSurface& surface, open_st::RectI bounds, std::size_t bytesPerPixel,
                       std::uint8_t* target) noexcept
{
    const std::size_t targetStride = static_cast<std::size_t>(bounds.Width()) * bytesPerPixel;
    for (int targetY = 0; targetY < bounds.Height(); ++targetY)
    {
        for (int targetX = 0; targetX < bounds.Width(); ++targetX)
        {
            int sourceX{};
            int sourceY{};
            if constexpr (ROTATION == open_st::CapturedSurfaceRotation::Rotate90)
            {
                sourceX = targetY;
                sourceY = surface.height - 1 - targetX;
            }
            else if constexpr (ROTATION == open_st::CapturedSurfaceRotation::Rotate180)
            {
                sourceX = surface.width - 1 - targetX;
                sourceY = surface.height - 1 - targetY;
            }
            else
            {
                static_assert(ROTATION == open_st::CapturedSurfaceRotation::Rotate270);
                sourceX = surface.width - 1 - targetY;
                sourceY = targetX;
            }
            std::memcpy(target + static_cast<std::size_t>(targetY) * targetStride +
                            static_cast<std::size_t>(targetX) * bytesPerPixel,
                        surface.pixels + static_cast<std::size_t>(sourceY) * surface.rowPitch +
                            static_cast<std::size_t>(sourceX) * bytesPerPixel,
                        bytesPerPixel);
        }
    }
}
} // namespace

namespace open_st
{
// 去除映射表面的行填充并校正旋转，生成独立持有像素的冻结 plane。
// 入参：surface：借用的驱动映射表面；desktopBounds：输出物理像素半开边界；rotation：表面旋转方式；pixelColorSpace：原生像素颜色空间；metadata：显示颜色元数据；output
// ：输出参数，接收冻结 plane；errorMessage：输出参数，接收失败诊断。
// 返回：完整复制并通过合法性检查时为 true；输入无效、旋转越界或分配失败为 false，output 保持无效且写入诊断。
bool BuildCapturedOutputPlane(const MappedCaptureSurface& surface, RectI desktopBounds,
                              CapturedSurfaceRotation rotation, CapturedColorSpace pixelColorSpace,
                              OutputColorMetadata metadata, CapturedOutputPlane& output, std::wstring& errorMessage)
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
        // 尺寸已与旋转匹配；每个分支都只访问有效源像素，未知枚举在发布候选前拒绝。
        switch (rotation)
        {
        case CapturedSurfaceRotation::Identity:
            for (std::size_t targetY = 0; targetY < targetHeight; ++targetY)
            {
                std::memcpy(pixels.data() + targetY * targetStride, surface.pixels + targetY * surface.rowPitch,
                            targetStride);
            }
            break;
        case CapturedSurfaceRotation::Rotate90:
            CopyRotatedPixels<CapturedSurfaceRotation::Rotate90>(surface, desktopBounds, bytesPerPixel, pixels.data());
            break;
        case CapturedSurfaceRotation::Rotate180:
            CopyRotatedPixels<CapturedSurfaceRotation::Rotate180>(surface, desktopBounds, bytesPerPixel, pixels.data());
            break;
        case CapturedSurfaceRotation::Rotate270:
            CopyRotatedPixels<CapturedSurfaceRotation::Rotate270>(surface, desktopBounds, bytesPerPixel, pixels.data());
            break;
        default:
            errorMessage = L"旋转后的显示像素超出捕获表面范围。";
            return false;
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
