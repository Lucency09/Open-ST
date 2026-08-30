#include <bgra_frame.h>

#include <limits>
#include <stdexcept>

namespace
{
constexpr std::size_t BYTES_PER_PIXEL = 4;
}

namespace open_st
{
// 使用截屏矩形进行初始化，计算行跨度和总缓冲区大小，防止 size_t 溢出。
BgraFrame::BgraFrame(RectI bounds) : bounds_(bounds)
{
    // 判断矩形合法性
    if (this->bounds_.IsEmpty())
    {
        return;
    }

    // 计算行跨度和总缓冲区大小，防止 size_t 溢出。
    const std::size_t width = static_cast<std::size_t>(this->bounds_.Width());
    const std::size_t height = static_cast<std::size_t>(this->bounds_.Height());

    // 先分别验证“每行字节数”和“总缓冲区字节数”，防止 size_t 乘法溢出后少分配内存。
    if (width > std::numeric_limits<std::size_t>::max() / BYTES_PER_PIXEL)
    {
        throw std::overflow_error("BGRA 帧行跨度溢出");
    }

    // 计算每行字节数，等于 Width() * 4；0 表示无效帧。
    this->stride_ = width * BYTES_PER_PIXEL;
    if (height > std::numeric_limits<std::size_t>::max() / this->stride_)
    {
        throw std::overflow_error("BGRA 帧大小溢出");
    }

    // 计算总缓冲区大小，等于 Stride() * Height()。
    this->pixels_.resize(this->stride_ * height);
}

// 判断帧有效性，要求 bounds 非空、stride 非零且像素缓冲区大小正确。
bool BgraFrame::IsValid() const noexcept
{
    return !this->bounds_.IsEmpty() && this->stride_ != 0 &&
           this->pixels_.size() == this->stride_ * static_cast<std::size_t>(this->Height());
}

// 返回帧的虚拟桌面物理像素矩形，可能为负数。
const RectI& BgraFrame::Bounds() const noexcept
{
    return this->bounds_;
}

// 返回 BGRA8 帧的像素宽度。
int BgraFrame::Width() const noexcept
{
    return this->bounds_.Width();
}

// 返回 BGRA8 帧的像素高度。
int BgraFrame::Height() const noexcept
{
    return this->bounds_.Height();
}

// 返回每行字节数，等于 Width() * 4；0 表示无效帧。
std::size_t BgraFrame::Stride() const noexcept
{
    return this->stride_;
}

// span 不拥有内存，其有效期不超过 BgraFrame；Stride() 用于逐行定位像素。
// 因为this->pixels_;是一个std::vector<std::uint8_t>，所以返回一个span<const std::uint8_t>，表示只读的像素数据。
// 此处做了一次隐式转换，将vector的data()和size()传给span的构造函数，创建一个span对象。
std::span<const std::uint8_t> BgraFrame::Pixels() const noexcept
{
    return this->pixels_;
}

// 捕获模块独占写入此缓冲区；帧交给应用后只通过 const 接口读取。
// 此处相当于返回一个span<std::uint8_t>，表示可写的像素数据。
std::span<std::uint8_t> BgraFrame::WritablePixels() noexcept
{
    return this->pixels_;
}

// 释放像素并把边界和 stride 同步复位为无效状态。
void BgraFrame::Clear() noexcept
{
    this->bounds_ = {};
    this->stride_ = 0;
    this->pixels_.clear();
}
} // namespace open_st
