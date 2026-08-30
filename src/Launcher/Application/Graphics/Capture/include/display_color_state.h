#pragma once

#include <frozen_desktop_frame.h>

namespace open_st
{
// 验证捕获时已知的 SDR 参考白仍未变化；查询失败返回 false，无参考白缓存时不作推断。
// 仅查询已绑定的 DisplayConfig 目标；调用方仍需通过 DXGI 工厂状态判断拓扑和 HDR 模式失效。
[[nodiscard]] bool IsCapturedOutputColorStateCurrent(const OutputColorMetadata& metadata) noexcept;
} // namespace open_st
