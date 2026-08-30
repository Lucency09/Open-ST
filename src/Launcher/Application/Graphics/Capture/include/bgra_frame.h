#pragma once

#include <geometry.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace open_st
{
// 保存紧凑顶向下的 8 位 BGRA 像素；当前主要用于由原生冻结 plane 派生覆盖预览。
// 像素格式固定为 8 位 BGRA，按从上到下的行顺序连续存储；bounds 使用虚拟桌面物理像素。
// 正式裁切输出必须读取 FrozenDesktopFrame，不能把已经转换过的覆盖预览当作源图。
class BgraFrame final
{
  public:
    // 创建不包含像素的无效空帧。
    BgraFrame() = default;
    // 按给定半开边界分配紧凑 BGRA8 像素缓冲区。
    explicit BgraFrame(RectI bounds);

    // 禁止复制大尺寸像素缓冲区。
    BgraFrame(const BgraFrame&) = delete;
    // 禁止复制赋值，保持缓冲区所有权唯一。
    BgraFrame& operator=(const BgraFrame&) = delete;
    // 允许移动像素缓冲区所有权。
    BgraFrame(BgraFrame&&) noexcept = default;
    // 允许通过移动赋值替换像素缓冲区所有权。
    BgraFrame& operator=(BgraFrame&&) noexcept = default;

    // 验证边界、stride 与像素缓冲区大小一致。
    [[nodiscard]] bool IsValid() const noexcept;
    // 返回像素对应的虚拟桌面半开边界。
    [[nodiscard]] const RectI& Bounds() const noexcept;
    // 返回像素宽度。
    [[nodiscard]] int Width() const noexcept;
    // 返回像素高度。
    [[nodiscard]] int Height() const noexcept;
    // 返回紧凑行跨度，等于 Width() × 4。
    [[nodiscard]] std::size_t Stride() const noexcept;
    // span 不拥有内存，其有效期不超过 BgraFrame；Stride() 用于逐行定位像素。
    [[nodiscard]] std::span<const std::uint8_t> Pixels() const noexcept;

    // 捕获模块独占写入此缓冲区；帧交给应用后只通过 const 接口读取。
    [[nodiscard]] std::span<std::uint8_t> WritablePixels() noexcept;

    // 清空后 bounds、stride 和像素缓冲区同时复位，IsValid() 必须返回 false。
    void Clear() noexcept;

  private:
    RectI bounds_{};       // 虚拟桌面物理像素坐标，可能为负数。
    std::size_t stride_{}; // 每行字节数，等于 Width() * 4；0 表示无效帧。
    std::vector<std::uint8_t>
        pixels_{}; // BGRA 像素缓冲区(实际存储截屏数据的地方)，按行连续存储；大小等于 Stride() * Height()。
};
} // namespace open_st
