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
    // 在 UI 线程预热可复用 HDR 设备；失败不阻止纯 SDR 输出。
    [[nodiscard]] bool Prepare(std::wstring& errorMessage);
    // 同步裁切拼接；失败清空输出，布局空洞为不透明黑色。
    [[nodiscard]] bool Render(const FrozenDesktopFrame& desktop, RectI selection, SdrSelectionFrame& output,
                              std::wstring& errorMessage);
    // 释放全尺寸 GPU 图像，保留可复用设备。
    void ReleaseImageResources() noexcept;

  private:
    NativeToneMapper toneMapper_;
};
} // namespace open_st
