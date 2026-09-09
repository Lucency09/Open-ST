// 文件职责：定义各显示输出的预览帧和生成接口，区分 SDR 与 HDR 预览的像素和参考白语义。

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

// 从原生冻结 plane 生成独立单屏预览，保留 HDR 像素亮度语义。
// 入参：plane：只读原生冻结输出；preview：输出参数，接收自有预览像素、格式及颜色信息；errorMessage：输出参数，接收失败原因。
// 返回：预览完整生成时为 true；原生帧或元数据无效时为 false，preview 清空并写入诊断。
[[nodiscard]] bool BuildOutputPreview(const CapturedOutputPlane& plane, OutputPreviewFrame& preview,
                                      std::wstring& errorMessage);
} // namespace open_st
