// 文件职责：声明截图边框颜色的解析规则和默认值，避免渲染器直接依赖设置模块。

#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace open_st
{
inline constexpr std::uint32_t DEFAULT_SELECTION_BORDER_RGB = 0x000000U;

// 从可选设置文本确定截图边框颜色，统一缺失和非法值的回退。
// 入参：configuredColor：可选的 #RRGGBB 文本视图，仅在本次调用借用。
// 返回：合法文本对应的 0xRRGGBB；缺失或解析失败返回默认黑色。
[[nodiscard]] std::uint32_t ResolveSelectionBorderColor(std::optional<std::string_view> configuredColor) noexcept;
} // namespace open_st
