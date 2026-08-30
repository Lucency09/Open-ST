#pragma once

#include <cstdint>
#include <geometry.h>
#include <span>
#include <vector>

namespace open_st
{
// 拥有虚拟桌面选区的紧凑顶向下 SDR/sRGB BGRX；第四字节不表示透明度。
class SdrSelectionFrame final
{
  public:
    // 创建无效空输出。
    SdrSelectionFrame() = default;
    // 接管精确匹配边界的像素；非法尺寸或字节数保持无效。
    SdrSelectionFrame(RectI bounds, std::vector<std::uint8_t> pixels) noexcept;
    // 禁止隐式复制完整选区图像。
    SdrSelectionFrame(const SdrSelectionFrame&) = delete;
    // 禁止复制赋值造成额外全尺寸缓冲区。
    SdrSelectionFrame& operator=(const SdrSelectionFrame&) = delete;
    // 转移输出像素所有权。
    SdrSelectionFrame(SdrSelectionFrame&&) noexcept = default;
    // 用新的完整输出替换原像素。
    SdrSelectionFrame& operator=(SdrSelectionFrame&&) noexcept = default;
    // 返回输出尺寸与像素是否一致。
    [[nodiscard]] bool IsValid() const noexcept;
    // 返回虚拟桌面物理像素边界。
    [[nodiscard]] RectI Bounds() const noexcept;
    // 返回紧凑行跨度，单位字节。
    [[nodiscard]] std::size_t Stride() const noexcept;
    // 借用最终像素，生命周期不超过当前对象。
    [[nodiscard]] std::span<const std::uint8_t> Pixels() const noexcept;

  private:
    RectI bounds_{};
    std::vector<std::uint8_t> pixels_;
};
} // namespace open_st
