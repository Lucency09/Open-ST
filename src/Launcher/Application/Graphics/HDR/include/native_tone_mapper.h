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
    // 创建惰性转换器；设备在 Prepare 或第一次 Convert 时建立。
    NativeToneMapper();
    // 释放效果图、设备及所有内部图像资源。
    ~NativeToneMapper();
    // 禁止复制 COM 设备及立即上下文的所有权。
    NativeToneMapper(const NativeToneMapper&) = delete;
    // 禁止复制赋值，避免并发共享未同步的效果图。
    NativeToneMapper& operator=(const NativeToneMapper&) = delete;

    // 建立设备并以微小图像预热；硬件初始化失败最多降级 WARP 一次，失败提供诊断文本。
    [[nodiscard]] bool Prepare(std::wstring& errorMessage);
    // 同步借用输入并输出紧凑 BGRA8、alpha=255；非法输入或转换失败清空 bgra，不重复缩放 SDR 白。
    [[nodiscard]] bool Convert(const HdrImageView& image, std::vector<std::uint8_t>& bgra, std::wstring& errorMessage);
    // 断开效果图对图像的引用并释放 D2D 图像缓存；保留设备和小型效果对象，可重复调用。
    void ReleaseImageResources() noexcept;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace open_st
