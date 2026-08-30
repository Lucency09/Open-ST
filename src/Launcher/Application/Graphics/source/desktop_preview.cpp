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
// 判断当前显示输出是否明确报告 HDR，未知显示状态不猜测 SDR 白亮度。
bool IsHdrOutput(const open_st::OutputColorMetadata& metadata) noexcept
{
    return metadata.displayColorSpace == open_st::CapturedColorSpace::Hdr10 ||
           metadata.displayColorSpace == open_st::CapturedColorSpace::ScRgb;
}

// 把线性颜色写入不透明 FP16 像素，保留负色域分量与超出 1.0 的高光。
void WriteLinearPixel(open_st::LinearScRgb color, std::uint8_t* target) noexcept
{
    const std::uint16_t channels[4]{open_st::EncodeFloat16(color.red), open_st::EncodeFloat16(color.green),
                                    open_st::EncodeFloat16(color.blue), 0x3C00U};
    std::memcpy(target, channels, sizeof(channels));
}

// 原样保留 FP16 RGB 位模式；桌面 alpha 不参与最终不透明覆盖窗口合成。
void CopyHalfPixel(const std::uint8_t* source, std::uint8_t* target) noexcept
{
    std::memcpy(target, source, 6U);
    constexpr std::uint16_t OPAQUE_ALPHA = 0x3C00U;
    std::memcpy(target + 6U, &OPAQUE_ALPHA, sizeof(OPAQUE_ALPHA));
}

// 按单屏呈现策略转换像素；仅 HDR 上的 SDR 兼容内容需要缩放 SDR 白，不缩放原生 HDR 数据。
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
// 为每个输出独立准备 SDR 或 scRGB 呈现缓冲，避免跨屏统一色调映射改变原始 HDR 亮度。
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
