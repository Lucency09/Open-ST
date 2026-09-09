// 文件职责：实现原生 plane 和冻结桌面的格式、尺寸、包含关系校验以及像素所有权访问。

#include <frozen_desktop_frame.h>

#include <cstdint>
#include <limits>
#include <utility>

namespace
{
// 以宽整数减法计算矩形尺寸，避免有符号坐标相减溢出。
// 入参：bounds：物理像素半开矩形；width、height：输出参数，成功时写入正的像素宽高。
// 返回：尺寸有效时为 true；空矩形或非正尺寸为 false，失败不写输出尺寸。
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

// 判断显示输出矩形是否完整落在冻结桌面的范围内。
// 入参：parent：容纳范围；child：待检查范围；两者均为虚拟桌面物理像素半开矩形。
// 返回：两矩形均非空且 child 完整位于 parent 内时为 true，否则为 false。
bool ContainsRectangle(open_st::RectI parent, open_st::RectI child) noexcept
{
    return !parent.IsEmpty() && !child.IsEmpty() && child.left >= parent.left && child.top >= parent.top &&
           child.right <= parent.right && child.bottom <= parent.bottom;
}

// 检查原生像素格式与自述颜色空间是否相容，防止下游错误解码。
// 入参：format：原生像素格式；colorSpace：像素自身声明的颜色空间。
// 返回：BGRA8 对应 SDR、FP16 对应 scRGB 或格式为 RGB10A2 时为 true，其余为 false。
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
// 确定原生像素格式的存储宽度以计算行跨度和复制偏移。
// 入参：format：待查询的捕获像素格式。
// 返回：BGRA8/RGB10A2 返回 4 字节，FP16 返回 8 字节，未知格式返回 0。
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

// 构造并独占一个显示输出的原生冻结像素，校验颜色格式和内存布局。
// 入参：bounds：桌面方向物理像素半开边界；format：原生像素格式；pixelColorSpace：像素编码颜色空间；metadata：显示输出颜色信息；pixels：转移所有权的紧凑原生像素字节。
// 返回：无返回值；尺寸或存储不合法时对象无效。
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

// 检查原生输出 plane是否可供下游读取和处理。
// 入参：无。
// 返回：边界、行跨度及像素存储符合帧格式时为 true，否则为 false。
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

// 提供图像在虚拟桌面中的物理像素范围。
// 入参：无。
// 返回：当前对象所持半开矩形的只读引用；仅借用对象内存，不转移所有权。
const RectI& CapturedOutputPlane::Bounds() const noexcept
{
    return this->bounds_;
}

// 查询矩形或图像的水平物理像素尺寸。
// 入参：无。
// 返回：right 减 left 的有符号宽度，空或反向边界可能为零或负数。
int CapturedOutputPlane::Width() const noexcept
{
    return this->bounds_.Width();
}

// 查询矩形或图像的垂直物理像素尺寸。
// 入参：无。
// 返回：bottom 减 top 的有符号高度，空或反向边界可能为零或负数。
int CapturedOutputPlane::Height() const noexcept
{
    return this->bounds_.Height();
}

// 查询原生 plane 使用的像素编码格式。
// 入参：无。
// 返回：捕获时保存的 CapturedPixelFormat 枚举值。
CapturedPixelFormat CapturedOutputPlane::Format() const noexcept
{
    return this->format_;
}

// 查询原生像素自身的颜色空间以选择正确解码方式。
// 入参：无。
// 返回：plane 保存的像素颜色空间，不等同于显示输出的传输颜色空间。
CapturedColorSpace CapturedOutputPlane::PixelColorSpace() const noexcept
{
    return this->pixelColorSpace_;
}

// 提供显示输出颜色能力、参考白及兼容回退信息。
// 入参：无。
// 返回：当前 plane 所持颜色元数据的只读引用，其生命周期不超过当前对象。
const OutputColorMetadata& CapturedOutputPlane::ColorMetadata() const noexcept
{
    return this->metadata_;
}

// 查询逐行访问图像所需的字节步长。
// 入参：无。
// 返回：一行紧凑像素的字节数，不包含驱动行填充。
std::size_t CapturedOutputPlane::Stride() const noexcept
{
    return this->stride_;
}

// 向图像消费者提供只读像素视图，避免复制完整缓冲区。
// 入参：无。
// 返回：借用当前对象像素内存的只读 span；对象销毁或缓冲区改变后不可继续使用。
std::span<const std::uint8_t> CapturedOutputPlane::Pixels() const noexcept
{
    return this->pixels_;
}

// 接管一次完整捕获的全部显示输出，形成可重复读取的冻结桌面。
// 入参：bounds：虚拟桌面物理像素半开边界；outputs：转移所有权的全部显示输出 plane。
// 返回：无返回值；尺寸或存储不合法时对象无效。
FrozenDesktopFrame::FrozenDesktopFrame(RectI bounds, std::vector<CapturedOutputPlane> outputs) noexcept
    : bounds_(bounds), outputs_(std::move(outputs))
{
    if (!this->IsValid())
    {
        this->Clear();
    }
}

// 检查冻结桌面是否可供下游读取和处理。
// 入参：无。
// 返回：桌面边界非空、输出集合非空且所有有效 plane 均被边界包含时为 true，否则为 false。
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

// 提供冻结桌面在虚拟桌面中的物理像素范围。
// 入参：无。
// 返回：当前对象所持半开矩形的只读引用；仅借用对象内存，不转移所有权。
const RectI& FrozenDesktopFrame::Bounds() const noexcept
{
    return this->bounds_;
}

// 向预览和选区裁切提供冻结桌面的原生输出集合。
// 入参：无。
// 返回：按捕获枚举顺序排列的只读 plane 视图，借用当前冻结桌面的内存。
std::span<const CapturedOutputPlane> FrozenDesktopFrame::Outputs() const noexcept
{
    return this->outputs_;
}

// 丢弃冻结桌面的全部原生输出并复位图像几何信息。
// 入参：无。
// 返回：无返回值；对象恢复为空且 IsValid() 为 false。
void FrozenDesktopFrame::Clear() noexcept
{
    this->outputs_.clear();
    this->bounds_ = {};
}
} // namespace open_st
