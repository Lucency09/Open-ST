#include <algorithm>
#include <climits>
#include <cstring>
#include <exception>
#include <limits>
#include <selection_output_renderer.h>

namespace
{
// 计算半开矩形交集；不相交时产生空矩形。
open_st::RectI Intersection(open_st::RectI a, open_st::RectI b) noexcept
{
    return {std::max(a.left, b.left), std::max(a.top, b.top), std::min(a.right, b.right), std::min(a.bottom, b.bottom)};
}
// 采用整数四舍五入将 SDR 10 位编码值映射为 8 位，不执行 HDR 映射。
std::uint8_t Quantize(std::uint32_t component) noexcept
{
    return static_cast<std::uint8_t>((component * 255U + 511U) / 1023U);
}
} // namespace
namespace open_st
{
// 预热与正式转换使用同一个实例和线程。
bool SelectionOutputRenderer::Prepare(std::wstring& errorMessage)
{
    return this->toneMapper_.Prepare(errorMessage);
}
// 显式释放 GPU 中间图像。
void SelectionOutputRenderer::ReleaseImageResources() noexcept
{
    this->toneMapper_.ReleaseImageResources();
}
// 验证几何和颜色语义后，先在局部缓冲区完成整张图像，再一次性发布。
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
