// 文件职责：实现 SDR 选区像素缓冲区的接管、合法性验证与只读访问。

#include <climits>
#include <limits>
#include <sdr_selection_frame.h>
#include <utility>

namespace open_st
{
// 接管完整 SDR 选区像素，供剪贴板或文件输出借用。
// 入参：bounds：选区在虚拟桌面的物理像素半开边界；pixels：转移所有权的紧凑 BGRX8 像素字节。
// 返回：无返回值；尺寸或存储不合法时对象无效。
SdrSelectionFrame::SdrSelectionFrame(RectI bounds, std::vector<std::uint8_t> pixels) noexcept
    : bounds_(bounds), pixels_(std::move(pixels))
{
    if (!this->IsValid())
    {
        this->bounds_ = {};
        this->pixels_.clear();
    }
}
// 检查 SDR 选区帧是否可供下游读取和处理。
// 入参：无。
// 返回：边界、行跨度及像素存储符合帧格式时为 true，否则为 false。
bool SdrSelectionFrame::IsValid() const noexcept
{
    const std::int64_t width = static_cast<std::int64_t>(this->bounds_.right) - this->bounds_.left;
    const std::int64_t height = static_cast<std::int64_t>(this->bounds_.bottom) - this->bounds_.top;
    if (width <= 0 || height <= 0 || width > INT_MAX || height > INT_MAX)
    {
        return false;
    }
    const std::size_t stride = static_cast<std::size_t>(width) * 4U;
    return static_cast<std::size_t>(height) <= std::numeric_limits<std::size_t>::max() / stride &&
           this->pixels_.size() == stride * static_cast<std::size_t>(height);
}
// 提供图像在虚拟桌面中的物理像素范围。
// 入参：无。
// 返回：选区物理像素半开矩形的值副本，不借用对象内存。
RectI SdrSelectionFrame::Bounds() const noexcept
{
    return this->bounds_;
}
// 查询逐行访问图像所需的字节步长。
// 入参：无。
// 返回：有效图像每行的紧凑字节数，即宽度乘 4；图像无效时返回 0。
std::size_t SdrSelectionFrame::Stride() const noexcept
{
    return this->IsValid() ? static_cast<std::size_t>(this->bounds_.Width()) * 4U : 0U;
}
// 向图像消费者提供只读像素视图，避免复制完整缓冲区。
// 入参：无。
// 返回：借用当前对象像素内存的只读 span；对象销毁或缓冲区改变后不可继续使用。
std::span<const std::uint8_t> SdrSelectionFrame::Pixels() const noexcept
{
    return this->pixels_;
}
} // namespace open_st
