// 文件职责：将各输出原生冻结像素转换为窗口预览，处理 HDR 显示格式和 SDR UI 参考白。

#include <desktop_preview.h>

#include <color_conversion.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
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

// 按已确定的像素尺寸遍历源和目标，将格式分派保留在整帧转换入口。
// 入参：plane：有效原生帧；preview：已分配目标；convert：同步像素转换器，不保存借用指针。
// 返回：无返回值；每个源像素恰好转换一次，模板字节数与调用分支的格式对应。
template <std::size_t SOURCE_BYTES, std::size_t TARGET_BYTES, typename Converter>
void ConvertPixels(const open_st::CapturedOutputPlane& plane, open_st::OutputPreviewFrame& preview,
                   Converter convert) noexcept
{
    const std::uint8_t* sourcePixels = plane.Pixels().data();
    const int width = plane.Width();
    const int height = plane.Height();
    for (int y = 0; y < height; ++y)
    {
        const std::uint8_t* source = sourcePixels + static_cast<std::size_t>(y) * plane.Stride();
        std::uint8_t* target = preview.pixels.data() + static_cast<std::size_t>(y) * preview.stride;
        for (int x = 0; x < width; ++x)
        {
            convert(source, target);
            source += SOURCE_BYTES;
            target += TARGET_BYTES;
        }
    }
}

// 为整帧选择唯一的格式转换路径，原格式 BGRA 直接逐行复制。
// 入参：plane：已校验的只读原生帧；preview：已确定格式、白比例并分配足量缓冲的目标。
// 返回：无返回值；保留原有颜色转换、RGB 位模式与显示 alpha 规则。
void ConvertPlanePixels(const open_st::CapturedOutputPlane& plane, open_st::OutputPreviewFrame& preview) noexcept
{
    if (plane.Format() == open_st::CapturedPixelFormat::Rgba16FloatScRgb)
    {
        ConvertPixels<8U, 8U>(plane, preview, CopyHalfPixel);
    }
    else if (plane.Format() == open_st::CapturedPixelFormat::Bgra8Unorm &&
             preview.pixelFormat == open_st::CapturedPixelFormat::Bgra8Unorm)
    {
        const std::uint8_t* source = plane.Pixels().data();
        for (int y = 0; y < plane.Height(); ++y)
        {
            std::memcpy(preview.pixels.data() + static_cast<std::size_t>(y) * preview.stride,
                        source + static_cast<std::size_t>(y) * plane.Stride(), preview.stride);
        }
    }
    else if (plane.Format() == open_st::CapturedPixelFormat::Bgra8Unorm)
    {
        // 将兼容 SDR 字节解码为线性 FP16，并仅应用一次输出 SDR 白比例。
        // 入参：source：四字节 BGRA；target：八字节目标像素。
        // 返回：无返回值，写入不透明 FP16 RGBA。
        ConvertPixels<4U, 8U>(
            plane, preview,
            [&preview](const std::uint8_t* source, std::uint8_t* target)
            {
                WriteLinearPixel({open_st::SrgbToLinear(static_cast<float>(source[2]) / 255.0F) * preview.uiWhiteScale,
                                  open_st::SrgbToLinear(static_cast<float>(source[1]) / 255.0F) * preview.uiWhiteScale,
                                  open_st::SrgbToLinear(static_cast<float>(source[0]) / 255.0F) * preview.uiWhiteScale},
                                 target);
            });
    }
    else if (preview.pixelFormat == open_st::CapturedPixelFormat::Bgra8Unorm)
    {
        // 将 SDR RGB10 通道量化为不透明 BGRA，舍入由共享纯颜色函数定义。
        // 入参：source：四字节 RGB10A2；target：四字节 BGRA 目标。
        // 返回：无返回值，忽略原两位 alpha。
        ConvertPixels<4U, 4U>(plane, preview,
                              [](const std::uint8_t* source, std::uint8_t* target)
                              {
                                  std::uint32_t packed{};
                                  std::memcpy(&packed, source, sizeof(packed));
                                  target[2] = open_st::Unorm10ToByte(packed & 0x03FFU);
                                  target[1] = open_st::Unorm10ToByte((packed >> 10U) & 0x03FFU);
                                  target[0] = open_st::Unorm10ToByte((packed >> 20U) & 0x03FFU);
                                  target[3] = 255U;
                              });
    }
    else
    {
        const open_st::Rgb10ColorSpace colorSpace =
            plane.PixelColorSpace() == open_st::CapturedColorSpace::Hdr10   ? open_st::Rgb10ColorSpace::Hdr10
            : plane.PixelColorSpace() == open_st::CapturedColorSpace::ScRgb ? open_st::Rgb10ColorSpace::ScRgb
                                                                            : open_st::Rgb10ColorSpace::SdrGamma22P709;
        const bool scaleSdr = plane.PixelColorSpace() == open_st::CapturedColorSpace::SdrGamma22P709;
        // 解码已确定颜色空间的 RGB10，只有 SDR 兼容像素应用白比例。
        // 入参：source：四字节 RGB10A2；target：八字节 FP16 目标。
        // 返回：无返回值，保留线性负分量和高光，不执行色调映射。
        ConvertPixels<4U, 8U>(plane, preview,
                              [colorSpace, scaleSdr, &preview](const std::uint8_t* source, std::uint8_t* target)
                              {
                                  std::uint32_t packed{};
                                  std::memcpy(&packed, source, sizeof(packed));
                                  open_st::LinearScRgb color = open_st::DecodeRgb10A2ToScRgb(packed, colorSpace);
                                  if (scaleSdr)
                                  {
                                      color.red *= preview.uiWhiteScale;
                                      color.green *= preview.uiWhiteScale;
                                      color.blue *= preview.uiWhiteScale;
                                  }
                                  WriteLinearPixel(color, target);
                              });
    }
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
    if (plane.Format() == CapturedPixelFormat::Rgb10A2Unorm && plane.PixelColorSpace() == CapturedColorSpace::Unknown)
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
        candidate.pixelFormat =
            hdrOutput || !sourceSdr ? CapturedPixelFormat::Rgba16FloatScRgb : CapturedPixelFormat::Bgra8Unorm;
        const std::size_t targetBytesPerPixel = CapturedBytesPerPixel(candidate.pixelFormat);
        const std::size_t stride = static_cast<std::size_t>(plane.Width()) * targetBytesPerPixel;
        const std::size_t height = static_cast<std::size_t>(plane.Height());
        if (stride > std::numeric_limits<std::uint32_t>::max() ||
            height > std::numeric_limits<std::size_t>::max() / stride || !std::isfinite(candidate.uiWhiteScale))
        {
            errorMessage = L"单屏覆盖预览尺寸或 SDR 白亮度超出可表示范围。";
            return false;
        }
        candidate.stride = static_cast<std::uint32_t>(stride);
        candidate.pixels.resize(stride * height);
        ConvertPlanePixels(plane, candidate);
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
