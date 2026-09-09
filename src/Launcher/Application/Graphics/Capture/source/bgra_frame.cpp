// 文件职责：实现紧凑 BGRA8 帧的缓冲区分配、尺寸校验、像素访问和清理。

#include <bgra_frame.h>

#include <limits>
#include <stdexcept>

namespace
{
constexpr std::size_t BYTES_PER_PIXEL = 4;
}

namespace open_st
{
// 为指定桌面区域分配紧凑 BGRA8 帧，供捕获或像素处理写入。
// 入参：bounds：虚拟桌面物理像素半开边界，用于确定 BGRA8 缓冲区尺寸。
// 返回：无返回值；空边界得到无效帧，尺寸乘法溢出或内存分配失败时抛出异常。
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

// 检查 BGRA 帧是否可供下游读取和处理。
// 入参：无。
// 返回：边界、行跨度及像素存储符合帧格式时为 true，否则为 false。
bool BgraFrame::IsValid() const noexcept
{
    return !this->bounds_.IsEmpty() && this->stride_ != 0 &&
           this->pixels_.size() == this->stride_ * static_cast<std::size_t>(this->Height());
}

// 提供图像在虚拟桌面中的物理像素范围。
// 入参：无。
// 返回：当前对象所持半开矩形的只读引用；仅借用对象内存，不转移所有权。
const RectI& BgraFrame::Bounds() const noexcept
{
    return this->bounds_;
}

// 查询矩形或图像的水平物理像素尺寸。
// 入参：无。
// 返回：right 减 left 的有符号宽度，空或反向边界可能为零或负数。
int BgraFrame::Width() const noexcept
{
    return this->bounds_.Width();
}

// 查询矩形或图像的垂直物理像素尺寸。
// 入参：无。
// 返回：bottom 减 top 的有符号高度，空或反向边界可能为零或负数。
int BgraFrame::Height() const noexcept
{
    return this->bounds_.Height();
}

// 查询逐行访问图像所需的字节步长。
// 入参：无。
// 返回：一行紧凑像素的字节数，不包含驱动行填充。
std::size_t BgraFrame::Stride() const noexcept
{
    return this->stride_;
}

// 向图像消费者提供只读像素视图，避免复制完整缓冲区。
// 入参：无。
// 返回：借用当前对象像素内存的只读 span；对象销毁或缓冲区改变后不可继续使用。
std::span<const std::uint8_t> BgraFrame::Pixels() const noexcept
{
    return this->pixels_;
}

// 为捕获阶段提供图像像素的可写视图。
// 入参：无。
// 返回：借用当前帧像素内存的可写 span；调用方不能延长缓冲区生命周期。
std::span<std::uint8_t> BgraFrame::WritablePixels() noexcept
{
    return this->pixels_;
}

// 丢弃BGRA 帧像素并复位图像几何信息。
// 入参：无。
// 返回：无返回值；对象恢复为空且 IsValid() 为 false。
void BgraFrame::Clear() noexcept
{
    this->bounds_ = {};
    this->stride_ = 0;
    this->pixels_.clear();
}
} // namespace open_st
