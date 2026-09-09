// 文件职责：将各输出原生冻结像素转换为窗口预览，处理 HDR 显示格式和 SDR UI 参考白。

#include <desktop_preview.h>

#include <color_conversion.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <span>
#include <utility>

namespace
{
// 判断预览目标是否报告 HDR10 或 scRGB 显示颜色空间。
// 入参：metadata：捕获输出保存的显示颜色元数据。
// 返回：显示颜色空间为 HDR10 或 scRGB 时为 true，其余为 false。
bool IsHdrOutput(const open_st::OutputColorMetadata& metadata) noexcept
{
    return metadata.displayColorSpace == open_st::CapturedColorSpace::Hdr10 ||
           metadata.displayColorSpace == open_st::CapturedColorSpace::ScRgb;
}

// 将线性 scRGB 三通道编码为不透明 FP16 像素供 HDR 预览上传。
// 入参：color：线性 scRGB RGB，1.0 对应 80 nit；target：输出参数，指向至少 8 字节目标像素。
// 返回：无返回值；目标写入四个 binary16 通道，alpha 固定为 1.0。
void WriteLinearPixel(open_st::LinearScRgb color, std::uint8_t* target) noexcept
{
    const std::uint16_t channels[4]{open_st::EncodeFloat16(color.red), open_st::EncodeFloat16(color.green),
                                    open_st::EncodeFloat16(color.blue), 0x3C00U};
    std::memcpy(target, channels, sizeof(channels));
}

// 复制原生 FP16 RGB 位模式并统一桌面预览的不透明 alpha。
// 入参：source：借用的 8 字节 FP16 源像素；target：输出参数，指向至少 8 字节目标像素。
// 返回：无返回值；RGB 原位复制，目标 alpha 写为 binary16 的 1.0。
void CopyHalfPixel(const std::uint8_t* source, std::uint8_t* target) noexcept
{
    std::memcpy(target, source, 6U);
    constexpr std::uint16_t OPAQUE_ALPHA = 0x3C00U;
    std::memcpy(target + 6U, &OPAQUE_ALPHA, sizeof(OPAQUE_ALPHA));
}

// 按原生 plane 格式和预览目标转换一个像素，统一 SDR/HDR 预览语义。
// 入参：plane：原生输出格式和颜色信息；source：借用的当前源像素；preview：目标预览格式和 UI 白比例；target：输出参数，指向足够容纳目标像素的内存。
// 返回：无返回值；目标写入匹配 preview 格式的像素，必要时解码 HDR10 或应用一次 SDR 白缩放。
void ConvertPixel(const open_st::CapturedOutputPlane& plane, const std::uint8_t* source,
                  const open_st::OutputPreviewFrame& preview, std::uint8_t* target) noexcept
{
    if (plane.Format() == open_st::CapturedPixelFormat::Rgba16FloatScRgb)
    {
        CopyHalfPixel(source, target);
        return;
    }
    if (plane.Format() == open_st::CapturedPixelFormat::Bgra8Unorm)
    {
        if (preview.pixelFormat == open_st::CapturedPixelFormat::Bgra8Unorm)
        {
            std::memcpy(target, source, 4U);
        }
        else
        {
            WriteLinearPixel({open_st::SrgbToLinear(static_cast<float>(source[2]) / 255.0F) * preview.uiWhiteScale,
                              open_st::SrgbToLinear(static_cast<float>(source[1]) / 255.0F) * preview.uiWhiteScale,
                              open_st::SrgbToLinear(static_cast<float>(source[0]) / 255.0F) * preview.uiWhiteScale},
                             target);
        }
        return;
    }

    std::uint32_t packed{};
    std::memcpy(&packed, source, sizeof(packed));
    if (preview.pixelFormat == open_st::CapturedPixelFormat::Bgra8Unorm)
    {
        target[2] = open_st::Unorm10ToByte(packed & 0x03FFU);
        target[1] = open_st::Unorm10ToByte((packed >> 10U) & 0x03FFU);
        target[0] = open_st::Unorm10ToByte((packed >> 20U) & 0x03FFU);
        target[3] = 255U;
        return;
    }
    const open_st::Rgb10ColorSpace colorSpace =
        plane.PixelColorSpace() == open_st::CapturedColorSpace::Hdr10   ? open_st::Rgb10ColorSpace::Hdr10
        : plane.PixelColorSpace() == open_st::CapturedColorSpace::ScRgb ? open_st::Rgb10ColorSpace::ScRgb
                                                                        : open_st::Rgb10ColorSpace::SdrGamma22P709;
    open_st::LinearScRgb color = open_st::DecodeRgb10A2ToScRgb(packed, colorSpace);
    if (plane.PixelColorSpace() == open_st::CapturedColorSpace::SdrGamma22P709)
    {
        color.red *= preview.uiWhiteScale;
        color.green *= preview.uiWhiteScale;
        color.blue *= preview.uiWhiteScale;
    }
    WriteLinearPixel(color, target);
}
} // namespace

namespace open_st
{
// 从原生冻结 plane 生成独立单屏预览，保留 HDR 像素亮度语义。
// 入参：plane：只读原生冻结输出；preview：输出参数，接收自有预览像素、格式及颜色信息；errorMessage：输出参数，接收失败原因。
// 返回：预览完整生成时为 true；原生帧或元数据无效时为 false，preview 清空并写入诊断。
bool BuildOutputPreview(const CapturedOutputPlane& plane, OutputPreviewFrame& preview, std::wstring& errorMessage)
{
    preview = {};
    errorMessage.clear();
    if (!plane.IsValid())
    {
        errorMessage = L"冻结显示输出无效，无法生成单屏覆盖预览。";
        return false;
    }
    const OutputColorMetadata& metadata = plane.ColorMetadata();
    const bool hdrOutput = IsHdrOutput(metadata);
    if (hdrOutput && (!metadata.hasSdrWhiteLevel || !std::isfinite(metadata.sdrWhiteLevelNits) ||
                      metadata.sdrWhiteLevelNits <= 0.0F))
    {
        errorMessage = L"HDR 显示输出缺少有效的 SDR 白亮度，无法准确设置覆盖界面颜色。";
        return false;
    }
    if (plane.Format() == CapturedPixelFormat::Rgb10A2Unorm &&
        plane.PixelColorSpace() == CapturedColorSpace::Unknown)
    {
        errorMessage = L"RGB10A2 冻结像素颜色空间未知，无法安全确定预览转换方式。";
        return false;
    }

    try
    {
        OutputPreviewFrame candidate;
        candidate.bounds = plane.Bounds();
        candidate.colorMetadata = metadata;
        candidate.uiWhiteScale = hdrOutput ? metadata.sdrWhiteLevelNits / 80.0F : 1.0F;
        const bool sourceSdr = plane.Format() == CapturedPixelFormat::Bgra8Unorm ||
                               plane.PixelColorSpace() == CapturedColorSpace::SdrGamma22P709;
        candidate.compatibilityMode = hdrOutput && sourceSdr;
        candidate.pixelFormat = hdrOutput || !sourceSdr ? CapturedPixelFormat::Rgba16FloatScRgb
                                                        : CapturedPixelFormat::Bgra8Unorm;
        const std::size_t targetBytesPerPixel = CapturedBytesPerPixel(candidate.pixelFormat);
        const std::size_t stride = static_cast<std::size_t>(plane.Width()) * targetBytesPerPixel;
        const std::size_t height = static_cast<std::size_t>(plane.Height());
        if (stride > std::numeric_limits<std::uint32_t>::max() ||
            height > std::numeric_limits<std::size_t>::max() / stride ||
            !std::isfinite(candidate.uiWhiteScale))
        {
            errorMessage = L"单屏覆盖预览尺寸或 SDR 白亮度超出可表示范围。";
            return false;
        }
        candidate.stride = static_cast<std::uint32_t>(stride);
        candidate.pixels.resize(stride * height);
        const std::span<const std::uint8_t> sourcePixels = plane.Pixels();
        const std::size_t sourceBytesPerPixel = CapturedBytesPerPixel(plane.Format());
        for (int y = 0; y < plane.Height(); ++y)
        {
            for (int x = 0; x < plane.Width(); ++x)
            {
                const std::uint8_t* source = sourcePixels.data() + static_cast<std::size_t>(y) * plane.Stride() +
                                             static_cast<std::size_t>(x) * sourceBytesPerPixel;
                std::uint8_t* target = candidate.pixels.data() + static_cast<std::size_t>(y) * stride +
                                       static_cast<std::size_t>(x) * targetBytesPerPixel;
                ConvertPixel(plane, source, candidate, target);
            }
        }
        preview = std::move(candidate);
        return true;
    }
    catch (const std::exception&)
    {
        errorMessage = L"无法为单屏覆盖窗口分配原生呈现缓冲区。";
        return false;
    }
}
} // namespace open_st
