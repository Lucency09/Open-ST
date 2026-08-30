#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace open_st
{
inline constexpr std::uint32_t DEFAULT_SELECTION_BORDER_RGB = 0x000000U;

// 将可选的严格 #RRGGBB 配置解析为 0xRRGGBB；缺失或非法时返回默认纯黑。
[[nodiscard]] std::uint32_t ResolveSelectionBorderColor(std::optional<std::string_view> configuredColor) noexcept;
} // namespace open_st
