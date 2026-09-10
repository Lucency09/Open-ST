// 文件职责：声明贴图自有不可变 SDR BGRX 图像，隔离截图会话与显示资源的生命周期。

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace open_st
{
class PinImage final
{
  public:
    // 校验并复制 SDR BGRX 像素，保留每个像素的第四字节且移除输入行填充。
    // 入参：width、height 为正的物理像素尺寸；stride 为输入行字节数；pixels 可省略末行填充；error 接收诊断。
    // 返回：成功时为独立不可变图像，失败时为空指针且填写 error；尺寸上限为每边 16384 像素。
    [[nodiscard]] static std::shared_ptr<const PinImage> Create(int width, int height, std::size_t stride,
                                                                std::span<const std::uint8_t> pixels,
                                                                std::wstring& error);

    // 将已校验的临时图像移入共享所有权容器，不复制像素。
    // 入参：source 为已校验的临时图像，其像素所有权被转移。
    // 返回：无返回值；新对象接管图像数据。
    PinImage(PinImage&& source) noexcept;
    // 禁止复制图像对象，调用方通过共享不可变快照延长生命周期。
    // 入参：未命名引用为拟复制的图像。
    // 返回：无；已删除的函数不能调用。
    PinImage(const PinImage&) = delete;
    // 禁止赋值以保持已发布图像的数据不变。
    // 入参：未命名引用为拟赋值的源图像。
    // 返回：无；已删除的函数不能调用。
    PinImage& operator=(const PinImage&) = delete;
    // 释放图像独占的像素缓冲区。
    // 入参：无。
    // 返回：无返回值；最后一个共享快照释放后回收像素。
    ~PinImage();

    // 查询原图宽度。
    // 入参：无。
    // 返回：以物理像素计的正宽度。
    [[nodiscard]] int Width() const noexcept;
    // 查询原图高度。
    // 入参：无。
    // 返回：以物理像素计的正高度。
    [[nodiscard]] int Height() const noexcept;
    // 查询自有像素的行跨度。
    // 入参：无。
    // 返回：每行字节数，恒为 Width() 乘四。
    [[nodiscard]] std::size_t Stride() const noexcept;
    // 借用原始 BGRX 像素用于显示上传或导出适配。
    // 入参：无。
    // 返回：图像存活期间有效的只读像素视图；第四字节不表示透明度。
    [[nodiscard]] std::span<const std::uint8_t> Pixels() const noexcept;

  private:
    // 接收工厂已校验并复制完成的紧密图像。
    // 入参：width、height 为物理像素尺寸；pixels 为紧密 BGRX 像素所有权。
    // 返回：无返回值；建立不可变图像的初始状态。
    PinImage(int width, int height, std::vector<std::uint8_t> pixels) noexcept;

    int width_{};
    int height_{};
    std::vector<std::uint8_t> pixels_;
};
} // namespace open_st
