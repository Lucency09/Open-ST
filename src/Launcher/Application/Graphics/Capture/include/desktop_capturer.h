// 文件职责：声明桌面冻结捕获入口，向上层返回各显示输出的原生像素与颜色元数据。

#pragma once

#include <frozen_desktop_frame.h>

#include <string>

namespace open_st
{
// 使用 DXGI Desktop Duplication 捕获当前虚拟桌面，并按显示输出保留原生 SDR/HDR plane。
// 优先使用 DuplicateOutput1 协商高色深格式；兼容降级必须在元数据中明确标记。
// 成功时 frame 有效且 errorMessage 为空；失败时 frame 被清空，errorMessage 描述失败阶段。
class DesktopCapturer final
{
  public:
    // 冻结当前虚拟桌面的全部真实显示输出，供一次截图会话重复读取。
    // 入参：frame：输出参数，接收拥有全部原生 plane 的冻结桌面；errorMessage：输出参数，接收捕获失败原因。
    // 返回：全部输出捕获并验证成功时为 true；任一阶段失败时为 false，frame 被清空且提供诊断。
    [[nodiscard]] bool Capture(FrozenDesktopFrame& frame, std::wstring& errorMessage) const;
};
} // namespace open_st
