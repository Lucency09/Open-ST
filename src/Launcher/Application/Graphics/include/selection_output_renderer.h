// 文件职责：声明冻结桌面选区到 SDR 输出的裁切与跨屏拼接入口，隔离后续导出业务。

#pragma once

#include <frozen_desktop_frame.h>
#include <native_tone_mapper.h>
#include <sdr_selection_frame.h>
#include <string>

namespace open_st
{
// 从冻结原生像素生成正式输出；同一线程复用 HDR 设备，绝不读取覆盖窗口。
class SelectionOutputRenderer final
{
  public:
    // 建立并预热 HDR 色调映射资源，避免第一次选区输出承担全部初始化成本。
    // 入参：errorMessage：输出参数，失败时接收初始化或预热原因。
    // 返回：设备及微小图像预热成功时为 true；受控回退后仍失败或分配异常时为 false。
    [[nodiscard]] bool Prepare(std::wstring& errorMessage);
    // 从冻结桌面裁切并拼接指定选区，将 HDR 区域转换为 SDR 供最终输出。
    // 入参：desktop：只读原生冻结桌面；selection：虚拟桌面物理像素半开选区；output：输出参数，接收完整 SDR 图像；errorMessage：输出参数，接收转换或几何错误原因。
    // 返回：整张选区完成时为 true；失败时为 false 且清空 output，不发布部分图像；显示器之间的空洞填不透明黑色。
    [[nodiscard]] bool Render(const FrozenDesktopFrame& desktop, RectI selection, SdrSelectionFrame& output,
                              std::wstring& errorMessage);
    // 释放本次图像转换使用的大块资源，同时保留可复用转换设备。
    // 入参：无。
    // 返回：无返回值；效果图不再引用单张图像，图像缓存被释放，可再次执行转换。
    void ReleaseImageResources() noexcept;

  private:
    NativeToneMapper toneMapper_;
};
} // namespace open_st
