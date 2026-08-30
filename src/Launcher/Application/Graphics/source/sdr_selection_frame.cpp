#include <climits>
#include <limits>
#include <sdr_selection_frame.h>
#include <utility>

namespace open_st
{
// 验证后接管图像，不发布尺寸不一致的输出。
SdrSelectionFrame::SdrSelectionFrame(RectI bounds, std::vector<std::uint8_t> pixels) noexcept
    : bounds_(bounds), pixels_(std::move(pixels))
{
    if (!this->IsValid())
    {
        this->bounds_ = {};
        this->pixels_.clear();
    }
}
// 使用宽整数计算边长，拒绝 int 差值溢出及缓冲区乘法溢出。
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
// 返回半开物理像素边界。
RectI SdrSelectionFrame::Bounds() const noexcept
{
    return this->bounds_;
}
// 无效输出的行跨度为零。
std::size_t SdrSelectionFrame::Stride() const noexcept
{
    return this->IsValid() ? static_cast<std::size_t>(this->bounds_.Width()) * 4U : 0U;
}
// 返回只读最终图像。
std::span<const std::uint8_t> SdrSelectionFrame::Pixels() const noexcept
{
    return this->pixels_;
}
} // namespace open_st
