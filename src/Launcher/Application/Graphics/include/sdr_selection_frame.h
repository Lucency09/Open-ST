// 文件职责：声明自有的 SDR/sRGB 选区输出帧，明确 BGRX 字节、物理像素边界与移动所有权。

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
    // 入参：无。
    // 返回：无返回值；构造后的图像对象为空，IsValid() 为 false。
    SdrSelectionFrame() = default;
    // 接管完整 SDR 选区像素，供剪贴板或文件输出借用。
    // 入参：bounds：选区在虚拟桌面的物理像素半开边界；pixels：转移所有权的紧凑 BGRX8 像素字节。
    // 返回：无返回值；尺寸或存储不合法时对象无效。
    SdrSelectionFrame(RectI bounds, std::vector<std::uint8_t> pixels) noexcept;
    // 禁止隐式复制完整选区图像。
    // 入参：未命名 const SdrSelectionFrame 引用：拟复制的源对象；该操作被禁止。
    // 返回：无；函数已删除，尝试调用会产生编译错误。
    SdrSelectionFrame(const SdrSelectionFrame&) = delete;
    // 禁止复制赋值造成额外全尺寸缓冲区。
    // 入参：未命名 const SdrSelectionFrame 引用：拟复制的源对象；该操作被禁止。
    // 返回：无；函数已删除，尝试调用会产生编译错误。
    SdrSelectionFrame& operator=(const SdrSelectionFrame&) = delete;
    // 转移 SDR 选区图像的像素所有权，避免复制图像缓冲区。
    // 入参：未命名 SdrSelectionFrame 右值引用：交出像素所有权的源对象。
    // 返回：无返回值；构造的新对象接管源像素缓冲区。
    SdrSelectionFrame(SdrSelectionFrame&&) noexcept = default;
    // 转移 SDR 选区图像的像素所有权，避免复制图像缓冲区。
    // 入参：未命名 SdrSelectionFrame 右值引用：交出像素所有权的源对象。
    // 返回：赋值后的当前对象引用。
    SdrSelectionFrame& operator=(SdrSelectionFrame&&) noexcept = default;
    // 检查 SDR 选区帧是否可供下游读取和处理。
    // 入参：无。
    // 返回：边界、行跨度及像素存储符合帧格式时为 true，否则为 false。
    [[nodiscard]] bool IsValid() const noexcept;
    // 提供图像在虚拟桌面中的物理像素范围。
    // 入参：无。
    // 返回：选区物理像素半开矩形的值副本，不借用对象内存。
    [[nodiscard]] RectI Bounds() const noexcept;
    // 查询逐行访问图像所需的字节步长。
    // 入参：无。
    // 返回：有效图像每行的紧凑字节数，即宽度乘 4；图像无效时返回 0。
    [[nodiscard]] std::size_t Stride() const noexcept;
    // 向图像消费者提供只读像素视图，避免复制完整缓冲区。
    // 入参：无。
    // 返回：借用当前对象像素内存的只读 span；对象销毁或缓冲区改变后不可继续使用。
    [[nodiscard]] std::span<const std::uint8_t> Pixels() const noexcept;

  private:
    RectI bounds_{};
    std::vector<std::uint8_t> pixels_;
};
} // namespace open_st
