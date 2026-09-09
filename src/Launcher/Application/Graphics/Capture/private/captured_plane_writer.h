// 文件职责：声明借用映射表面到自有冻结 plane 的转换接口，隔离旋转、行填充与像素格式。

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

// 去除映射表面的行填充并校正旋转，生成独立持有像素的冻结 plane。
// 入参：surface：借用的驱动映射表面；desktopBounds：输出物理像素半开边界；rotation：表面旋转方式；pixelColorSpace：原生像素颜色空间；metadata：显示颜色元数据；output
// ：输出参数，接收冻结 plane；errorMessage：输出参数，接收失败诊断。
// 返回：完整复制并通过合法性检查时为 true；输入无效、旋转越界或分配失败为 false，output 保持无效且写入诊断。
[[nodiscard]] bool BuildCapturedOutputPlane(const MappedCaptureSurface& surface, RectI desktopBounds,
                                            CapturedSurfaceRotation rotation,
                                            CapturedColorSpace pixelColorSpace,
                                            OutputColorMetadata metadata, CapturedOutputPlane& output,
                                            std::wstring& errorMessage);
} // namespace open_st
