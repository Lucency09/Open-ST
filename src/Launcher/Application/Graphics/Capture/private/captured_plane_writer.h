#pragma once

#include <frozen_desktop_frame.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace open_st
{
// 标识 Desktop Duplication 返回的未旋转表面应如何映射到桌面方向。
enum class CapturedSurfaceRotation
{
    Identity,
    Rotate90,
    Rotate180,
    Rotate270
};

// 只借用一次 Map 调用暴露的表面内存，调用结束前必须复制完成。
struct MappedCaptureSurface final
{
    const std::uint8_t* pixels{};
    std::size_t rowPitch{};
    int width{};
    int height{};
    CapturedPixelFormat format{CapturedPixelFormat::Bgra8Unorm};
};

// 把带驱动 RowPitch 的未旋转表面复制为桌面方向紧凑 plane；失败时 output 保持无效。
[[nodiscard]] bool BuildCapturedOutputPlane(const MappedCaptureSurface& surface, RectI desktopBounds,
                                            CapturedSurfaceRotation rotation,
                                            CapturedColorSpace pixelColorSpace,
                                            OutputColorMetadata metadata, CapturedOutputPlane& output,
                                            std::wstring& errorMessage);
} // namespace open_st
