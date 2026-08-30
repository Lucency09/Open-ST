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
    // 捕获一次完整冻结桌面；任一已附着输出失败时不发布部分结果。
    [[nodiscard]] bool Capture(FrozenDesktopFrame& frame, std::wstring& errorMessage) const;
};
} // namespace open_st
