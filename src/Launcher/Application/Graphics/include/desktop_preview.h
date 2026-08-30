#pragma once

#include <frozen_desktop_frame.h>

#include <cstdint>
#include <string>
#include <vector>

namespace open_st
{
// 拥有单屏覆盖窗口的紧凑呈现像素；边界采用虚拟桌面物理像素，不作为保存或剪贴板的数据源。
struct OutputPreviewFrame final
{
    RectI bounds{};
    OutputColorMetadata colorMetadata{};
    CapturedPixelFormat pixelFormat{CapturedPixelFormat::Bgra8Unorm};
    std::vector<std::uint8_t> pixels{};
    std::uint32_t stride{};
    float uiWhiteScale{1.0F}; // FP16 界面颜色的 SDR 白相对于 80 nit scRGB 参考白的比例。
    bool compatibilityMode{}; // HDR 输出只取得 SDR 数据时置位，不能视为原生 HDR 还原成功。
};

// 从只读原生 plane 生成单屏呈现数据；FP16 不色调映射，失败时清空 preview 并返回诊断原因。
[[nodiscard]] bool BuildOutputPreview(const CapturedOutputPlane& plane, OutputPreviewFrame& preview,
                                      std::wstring& errorMessage);
} // namespace open_st
