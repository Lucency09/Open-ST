// 文件职责：声明 Windows 原生 HDR 到 SDR 色调映射器，管理图形资源并返回自有 SDR 像素。

#pragma once

#include <hdr_image_view.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace open_st
{
// 同一线程串行使用的原生 HDR→SDR 转换器，设备与效果可复用，单张图像资源不跨调用保留。
class NativeToneMapper final
{
  public:
    // 创建HDR 原生色调映射器的内部资源容器，延迟到首次初始化时建立图形设备。
    // 入参：无。
    // 返回：无返回值；构造后的对象尚未建立图形设备，内存分配失败可抛出异常。
    NativeToneMapper();
    // 释放色调映射效果、设备及仍被效果引用的单张图像。
    // 入参：无。
    // 返回：无返回值；析构完成对应资源清理。
    ~NativeToneMapper();
    // 禁止复制 COM 设备及立即上下文的所有权。
    // 入参：未命名 const NativeToneMapper 引用：拟复制的源对象；该操作被禁止。
    // 返回：无；函数已删除，尝试调用会产生编译错误。
    NativeToneMapper(const NativeToneMapper&) = delete;
    // 禁止复制赋值，避免并发共享未同步的效果图。
    // 入参：未命名 const NativeToneMapper 引用：拟复制的源对象；该操作被禁止。
    // 返回：无；函数已删除，尝试调用会产生编译错误。
    NativeToneMapper& operator=(const NativeToneMapper&) = delete;

    // 建立并预热 HDR 色调映射资源，避免第一次选区输出承担全部初始化成本。
    // 入参：errorMessage：输出参数，失败时接收初始化或预热原因。
    // 返回：设备及微小图像预热成功时为 true；受控回退后仍失败或分配异常时为 false。
    [[nodiscard]] bool Prepare(std::wstring& errorMessage);
    // 通过 Windows 原生效果将借用 HDR 图像转换为紧凑 SDR/sRGB 输出。
    // 入参：image：只在调用期间借用的 HDR 图像视图；bgra：输出参数，接收紧凑 BGRA8 字节且 alpha 为 255；errorMessage：输出参数，接收失败诊断。
    // 返回：完成解码、原生映射和 GPU 回读时为 true；失败为 false，bgra 清空，不发布部分图像。
    [[nodiscard]] bool Convert(const HdrImageView& image, std::vector<std::uint8_t>& bgra, std::wstring& errorMessage);
    // 释放本次图像转换使用的大块资源，同时保留可复用转换设备。
    // 入参：无。
    // 返回：无返回值；效果图不再引用单张图像，图像缓存被释放，可再次执行转换。
    void ReleaseImageResources() noexcept;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace open_st
