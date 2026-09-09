// 文件职责：从冻结桌面裁切并拼接选区，对 HDR 执行 SDR 转换并保留 SDR 原始像素。

#include <algorithm>
#include <climits>
#include <cstring>
#include <exception>
#include <limits>
#include <selection_output_renderer.h>

namespace
{
// 计算两个半开矩形的重叠区域，限定逐屏选区裁切范围。
// 入参：a、b：虚拟桌面物理像素半开矩形。
// 返回：两矩形的交集；不相交时结果为可由 IsEmpty 判断的空矩形。
open_st::RectI Intersection(open_st::RectI a, open_st::RectI b) noexcept
{
    return {std::max(a.left, b.left), std::max(a.top, b.top), std::min(a.right, b.right), std::min(a.bottom, b.bottom)};
}
// 将 SDR RGB10A2 通道量化为最终 8 位输出通道。
// 入参：component：10 位 UNORM 通道值，范围 0 至 1023。
// 返回：四舍五入后的 0 至 255 通道值。
std::uint8_t Quantize(std::uint32_t component) noexcept
{
    return static_cast<std::uint8_t>((component * 255U + 511U) / 1023U);
}
} // namespace
namespace open_st
{
// 建立并预热 HDR 色调映射资源，避免第一次选区输出承担全部初始化成本。
// 入参：errorMessage：输出参数，失败时接收初始化或预热原因。
// 返回：设备及微小图像预热成功时为 true；受控回退后仍失败或分配异常时为 false。
bool SelectionOutputRenderer::Prepare(std::wstring& errorMessage)
{
    return this->toneMapper_.Prepare(errorMessage);
}
// 释放本次图像转换使用的大块资源，同时保留可复用转换设备。
// 入参：无。
// 返回：无返回值；效果图不再引用单张图像，图像缓存被释放，可再次执行转换。
void SelectionOutputRenderer::ReleaseImageResources() noexcept
{
    this->toneMapper_.ReleaseImageResources();
}
// 从冻结桌面裁切并拼接指定选区，将 HDR 区域转换为 SDR 供最终输出。
// 入参：desktop：只读原生冻结桌面；selection：虚拟桌面物理像素半开选区；output：输出参数，接收完整 SDR 图像；errorMessage：输出参数，接收转换或几何错误原因。
// 返回：整张选区完成时为 true；失败时为 false 且清空 output，不发布部分图像；显示器之间的空洞填不透明黑色。
bool SelectionOutputRenderer::Render(const FrozenDesktopFrame& desktop, RectI selection, SdrSelectionFrame& output,
                                     std::wstring& errorMessage)
try
{
    output = {};
    errorMessage.clear();
    const std::int64_t width = static_cast<std::int64_t>(selection.right) - selection.left;
    const std::int64_t height = static_cast<std::int64_t>(selection.bottom) - selection.top;
    const RectI bounds = desktop.Bounds();
    if (!desktop.IsValid() || width <= 0 || height <= 0 || width > INT_MAX || height > INT_MAX ||
        selection.left < bounds.left || selection.top < bounds.top || selection.right > bounds.right ||
        selection.bottom > bounds.bottom)
    {
        errorMessage = L"冻结帧或选区边界无效。";
        return false;
    }
    const std::size_t stride = static_cast<std::size_t>(width) * 4U;
    if (static_cast<std::size_t>(height) > std::numeric_limits<std::size_t>::max() / stride)
    {
        errorMessage = L"选区尺寸溢出。";
        return false;
    }
    const std::span<const CapturedOutputPlane> planes = desktop.Outputs();
    for (std::size_t i = 0; i < planes.size(); ++i)
    {
        for (std::size_t j = i + 1; j < planes.size(); ++j)
        {
            if (!Intersection(Intersection(planes[i].Bounds(), planes[j].Bounds()), selection).IsEmpty())
            {
                errorMessage = L"选区包含有歧义的重叠显示输出。";
                return false;
            }
        }
    }
    std::vector<std::uint8_t> pixels(stride * static_cast<std::size_t>(height), 0U);
    for (std::size_t alpha = 3; alpha < pixels.size(); alpha += 4)
    {
        pixels[alpha] = 255U;
    }
    for (const CapturedOutputPlane& plane : planes)
    {
        const RectI part = Intersection(selection, plane.Bounds());
        if (part.IsEmpty())
        {
            continue;
        }
        const std::size_t sourceOffset =
            static_cast<std::size_t>(part.top - plane.Bounds().top) * plane.Stride() +
            static_cast<std::size_t>(part.left - plane.Bounds().left) * CapturedBytesPerPixel(plane.Format());
        const std::span<const std::uint8_t> source = plane.Pixels().subspan(sourceOffset);
        std::vector<std::uint8_t> converted;
        const bool sdr = plane.PixelColorSpace() == CapturedColorSpace::SdrGamma22P709;
        const bool copyBgra =
            plane.Format() == CapturedPixelFormat::Bgra8Unorm && (sdr || plane.ColorMetadata().systemConvertedToSdr);
        const bool quantize = plane.Format() == CapturedPixelFormat::Rgb10A2Unorm && sdr;
        if (!copyBgra && !quantize)
        {
            HdrPixelFormat format{};
            if (plane.Format() == CapturedPixelFormat::Rgba16FloatScRgb &&
                plane.PixelColorSpace() == CapturedColorSpace::ScRgb)
            {
                format = HdrPixelFormat::Rgba16FloatScRgb;
            }
            else if (plane.Format() == CapturedPixelFormat::Rgb10A2Unorm &&
                     plane.PixelColorSpace() == CapturedColorSpace::Hdr10)
            {
                format = HdrPixelFormat::Rgb10A2Hdr10;
            }
            else if (plane.Format() == CapturedPixelFormat::Rgb10A2Unorm &&
                     plane.PixelColorSpace() == CapturedColorSpace::ScRgb)
            {
                format = HdrPixelFormat::Rgb10A2ScRgb;
            }
            else
            {
                errorMessage = L"不支持的输出像素格式与颜色空间组合。";
                return false;
            }
            if (!this->toneMapper_.Convert({static_cast<std::uint32_t>(part.Width()),
                                            static_cast<std::uint32_t>(part.Height()), plane.Stride(), source, format},
                                           converted, errorMessage))
            {
                return false;
            }
        }
        for (int y = 0; y < part.Height(); ++y)
        {
            std::uint8_t* destination = pixels.data() +
                                        static_cast<std::size_t>(part.top - selection.top + y) * stride +
                                        static_cast<std::size_t>(part.left - selection.left) * 4U;
            const std::uint8_t* row = source.data() + static_cast<std::size_t>(y) * plane.Stride();
            if (copyBgra)
            {
                std::memcpy(destination, row, static_cast<std::size_t>(part.Width()) * 4U);
            }
            else if (quantize)
            {
                for (int x = 0; x < part.Width(); ++x)
                {
                    std::uint32_t packed{};
                    std::memcpy(&packed, row + static_cast<std::size_t>(x) * 4U, 4U);
                    destination[static_cast<std::size_t>(x) * 4U] = Quantize((packed >> 20U) & 1023U);
                    destination[static_cast<std::size_t>(x) * 4U + 1] = Quantize((packed >> 10U) & 1023U);
                    destination[static_cast<std::size_t>(x) * 4U + 2] = Quantize(packed & 1023U);
                    destination[static_cast<std::size_t>(x) * 4U + 3] = 255U;
                }
            }
            else
            {
                std::memcpy(destination,
                            converted.data() +
                                static_cast<std::size_t>(y) * static_cast<std::size_t>(part.Width()) * 4U,
                            static_cast<std::size_t>(part.Width()) * 4U);
            }
        }
    }
    output = SdrSelectionFrame(selection, std::move(pixels));
    return output.IsValid();
}
catch (const std::exception&)
{
    output = {};
    this->ReleaseImageResources();
    errorMessage = L"无法分配选区图像资源。";
    return false;
}
} // namespace open_st
