// 文件职责：声明拥有紧凑 BGRA8 像素的帧类型，明确虚拟桌面边界与像素缓冲区所有权。

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
    // 入参：无。
    // 返回：无返回值；构造后的图像对象为空，IsValid() 为 false。
    BgraFrame() = default;
    // 为指定桌面区域分配紧凑 BGRA8 帧，供捕获或像素处理写入。
    // 入参：bounds：虚拟桌面物理像素半开边界，用于确定 BGRA8 缓冲区尺寸。
    // 返回：无返回值；空边界得到无效帧，尺寸乘法溢出或内存分配失败时抛出异常。
    explicit BgraFrame(RectI bounds);

    // 禁止复制大尺寸像素缓冲区。
    // 入参：未命名 const BgraFrame 引用：拟复制的源对象；该操作被禁止。
    // 返回：无；函数已删除，尝试调用会产生编译错误。
    BgraFrame(const BgraFrame&) = delete;
    // 禁止复制赋值，保持缓冲区所有权唯一。
    // 入参：未命名 const BgraFrame 引用：拟复制的源对象；该操作被禁止。
    // 返回：无；函数已删除，尝试调用会产生编译错误。
    BgraFrame& operator=(const BgraFrame&) = delete;
    // 转移 BGRA 帧的像素所有权，避免复制图像缓冲区。
    // 入参：未命名 BgraFrame 右值引用：交出像素所有权的源对象。
    // 返回：无返回值；构造的新对象接管源像素缓冲区。
    BgraFrame(BgraFrame&&) noexcept = default;
    // 转移 BGRA 帧的像素所有权，避免复制图像缓冲区。
    // 入参：未命名 BgraFrame 右值引用：交出像素所有权的源对象。
    // 返回：赋值后的当前对象引用。
    BgraFrame& operator=(BgraFrame&&) noexcept = default;

    // 检查 BGRA 帧是否可供下游读取和处理。
    // 入参：无。
    // 返回：边界、行跨度及像素存储符合帧格式时为 true，否则为 false。
    [[nodiscard]] bool IsValid() const noexcept;
    // 提供图像在虚拟桌面中的物理像素范围。
    // 入参：无。
    // 返回：当前对象所持半开矩形的只读引用；仅借用对象内存，不转移所有权。
    [[nodiscard]] const RectI& Bounds() const noexcept;
    // 查询矩形或图像的水平物理像素尺寸。
    // 入参：无。
    // 返回：right 减 left 的有符号宽度，空或反向边界可能为零或负数。
    [[nodiscard]] int Width() const noexcept;
    // 查询矩形或图像的垂直物理像素尺寸。
    // 入参：无。
    // 返回：bottom 减 top 的有符号高度，空或反向边界可能为零或负数。
    [[nodiscard]] int Height() const noexcept;
    // 查询逐行访问图像所需的字节步长。
    // 入参：无。
    // 返回：一行紧凑像素的字节数，不包含驱动行填充。
    [[nodiscard]] std::size_t Stride() const noexcept;
    // 向图像消费者提供只读像素视图，避免复制完整缓冲区。
    // 入参：无。
    // 返回：借用当前对象像素内存的只读 span；对象销毁或缓冲区改变后不可继续使用。
    [[nodiscard]] std::span<const std::uint8_t> Pixels() const noexcept;

    // 为捕获阶段提供图像像素的可写视图。
    // 入参：无。
    // 返回：借用当前帧像素内存的可写 span；调用方不能延长缓冲区生命周期。
    [[nodiscard]] std::span<std::uint8_t> WritablePixels() noexcept;

    // 丢弃BGRA 帧像素并复位图像几何信息。
    // 入参：无。
    // 返回：无返回值；对象恢复为空且 IsValid() 为 false。
    void Clear() noexcept;

  private:
    RectI bounds_{};       // 虚拟桌面物理像素坐标，可能为负数。
    std::size_t stride_{}; // 每行字节数，等于 Width() * 4；0 表示无效帧。
    std::vector<std::uint8_t>
        pixels_{}; // BGRA 像素缓冲区(实际存储截屏数据的地方)，按行连续存储；大小等于 Stride() * Height()。
};
} // namespace open_st
