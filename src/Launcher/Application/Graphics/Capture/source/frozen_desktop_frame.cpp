#include <frozen_desktop_frame.h>

#include <cstdint>
#include <limits>
#include <utility>

namespace
{
// 在不执行有符号减法溢出的前提下计算有效矩形维度。
bool TryGetDimensions(open_st::RectI bounds, std::size_t& width, std::size_t& height) noexcept
{
    if (bounds.IsEmpty())
    {
        return false;
    }

    const std::int64_t wideWidth =
        static_cast<std::int64_t>(bounds.right) - static_cast<std::int64_t>(bounds.left);
    const std::int64_t wideHeight =
        static_cast<std::int64_t>(bounds.bottom) - static_cast<std::int64_t>(bounds.top);
    if (wideWidth <= 0 || wideHeight <= 0)
    {
        return false;
    }

    width = static_cast<std::size_t>(wideWidth);
    height = static_cast<std::size_t>(wideHeight);
    return true;
}

// 判断 child 半开矩形是否完整位于 parent 半开矩形内。
bool ContainsRectangle(open_st::RectI parent, open_st::RectI child) noexcept
{
    return !parent.IsEmpty() && !child.IsEmpty() && child.left >= parent.left && child.top >= parent.top &&
           child.right <= parent.right && child.bottom <= parent.bottom;
}

// 验证像素格式与其自述颜色空间不存在会误导下游转换的矛盾组合。
bool IsPixelColorSpaceCompatible(open_st::CapturedPixelFormat format,
                                 open_st::CapturedColorSpace colorSpace) noexcept
{
    switch (format)
    {
    case open_st::CapturedPixelFormat::Bgra8Unorm:
        return colorSpace == open_st::CapturedColorSpace::SdrGamma22P709;
    case open_st::CapturedPixelFormat::Rgba16FloatScRgb:
        return colorSpace == open_st::CapturedColorSpace::ScRgb;
    case open_st::CapturedPixelFormat::Rgb10A2Unorm:
        return true;
    default:
        return false;
    }
}
} // namespace

namespace open_st
{
// 按冻结像素格式返回紧凑布局的单像素字节数。
std::size_t CapturedBytesPerPixel(CapturedPixelFormat format) noexcept
{
    switch (format)
    {
    case CapturedPixelFormat::Bgra8Unorm:
    case CapturedPixelFormat::Rgb10A2Unorm:
        return 4U;
    case CapturedPixelFormat::Rgba16FloatScRgb:
        return 8U;
    default:
        return 0U;
    }
}

// 验证并接管已经去除驱动行填充、旋转到桌面方向的原生像素。
CapturedOutputPlane::CapturedOutputPlane(RectI bounds, CapturedPixelFormat format,
                                         CapturedColorSpace pixelColorSpace, OutputColorMetadata metadata,
                                         std::vector<std::uint8_t> pixels) noexcept
    : bounds_(bounds), format_(format), pixelColorSpace_(pixelColorSpace), metadata_(metadata),
      pixels_(std::move(pixels))
{
    std::size_t width{};
    std::size_t height{};
    const std::size_t bytesPerPixel = CapturedBytesPerPixel(format);
    if (!TryGetDimensions(bounds, width, height) || bytesPerPixel == 0U ||
        !IsPixelColorSpaceCompatible(format, pixelColorSpace) ||
        width > std::numeric_limits<std::size_t>::max() / bytesPerPixel)
    {
        this->bounds_ = {};
        this->pixels_.clear();
        return;
    }

    this->stride_ = width * bytesPerPixel;
    if (height > std::numeric_limits<std::size_t>::max() / this->stride_ ||
        this->pixels_.size() != this->stride_ * height)
    {
        this->bounds_ = {};
        this->stride_ = 0U;
        this->pixels_.clear();
    }
}

// 验证 plane 的所有派生尺寸仍与其拥有的紧凑像素一致。
bool CapturedOutputPlane::IsValid() const noexcept
{
    std::size_t width{};
    std::size_t height{};
    const std::size_t bytesPerPixel = CapturedBytesPerPixel(this->format_);
    return TryGetDimensions(this->bounds_, width, height) && bytesPerPixel != 0U &&
           IsPixelColorSpaceCompatible(this->format_, this->pixelColorSpace_) &&
           width <= std::numeric_limits<std::size_t>::max() / bytesPerPixel &&
           this->stride_ == width * bytesPerPixel &&
           height <= std::numeric_limits<std::size_t>::max() / this->stride_ &&
           this->pixels_.size() == this->stride_ * height;
}

// 返回 plane 的虚拟桌面边界。
const RectI& CapturedOutputPlane::Bounds() const noexcept
{
    return this->bounds_;
}

// 返回 plane 的桌面方向宽度。
int CapturedOutputPlane::Width() const noexcept
{
    return this->bounds_.Width();
}

// 返回 plane 的桌面方向高度。
int CapturedOutputPlane::Height() const noexcept
{
    return this->bounds_.Height();
}

// 返回 plane 的原生像素格式。
CapturedPixelFormat CapturedOutputPlane::Format() const noexcept
{
    return this->format_;
}

// 返回 plane 像素数据使用的颜色空间。
CapturedColorSpace CapturedOutputPlane::PixelColorSpace() const noexcept
{
    return this->pixelColorSpace_;
}

// 返回显示输出颜色能力和兼容降级元数据。
const OutputColorMetadata& CapturedOutputPlane::ColorMetadata() const noexcept
{
    return this->metadata_;
}

// 返回不含驱动 padding 的紧凑行跨度。
std::size_t CapturedOutputPlane::Stride() const noexcept
{
    return this->stride_;
}

// 返回只读原生像素视图。
std::span<const std::uint8_t> CapturedOutputPlane::Pixels() const noexcept
{
    return this->pixels_;
}

// 验证所有输出后一次性发布完整冻结桌面，避免暴露半成品。
FrozenDesktopFrame::FrozenDesktopFrame(RectI bounds, std::vector<CapturedOutputPlane> outputs) noexcept
    : bounds_(bounds), outputs_(std::move(outputs))
{
    if (!this->IsValid())
    {
        this->Clear();
    }
}

// 验证冻结桌面非空，并确保每个有效 plane 都落在虚拟桌面边界内。
bool FrozenDesktopFrame::IsValid() const noexcept
{
    if (this->bounds_.IsEmpty() || this->outputs_.empty())
    {
        return false;
    }

    for (const CapturedOutputPlane& output : this->outputs_)
    {
        if (!output.IsValid() || !ContainsRectangle(this->bounds_, output.Bounds()))
        {
            return false;
        }
    }
    return true;
}

// 返回冻结时刻的虚拟桌面边界。
const RectI& FrozenDesktopFrame::Bounds() const noexcept
{
    return this->bounds_;
}

// 返回按捕获枚举顺序保存的只读原生输出集合。
std::span<const CapturedOutputPlane> FrozenDesktopFrame::Outputs() const noexcept
{
    return this->outputs_;
}

// 释放全部原生像素并清空虚拟桌面边界。
void FrozenDesktopFrame::Clear() noexcept
{
    this->outputs_.clear();
    this->bounds_ = {};
}
} // namespace open_st
