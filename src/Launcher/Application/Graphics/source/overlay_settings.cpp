#include "overlay_settings.h"

#include <log.h>

#include <cstddef>
#include <optional>

namespace
{
// 将单个十六进制字符转换为 0–15；非法字符返回 -1。
int HexDigit(char value) noexcept
{
    if (value >= '0' && value <= '9')
    {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f')
    {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F')
    {
        return value - 'A' + 10;
    }
    return -1;
}

// 严格解析七字符 #RRGGBB，不接受 alpha、简写、空格或缺少井号的形式。
bool TryParseRgb(std::string_view value, std::uint32_t& color) noexcept
{
    if (value.size() != 7U || value.front() != '#')
    {
        return false;
    }

    std::uint32_t parsed = 0U;
    for (std::size_t index = 1U; index < value.size(); ++index)
    {
        const int digit = HexDigit(value[index]);
        if (digit < 0)
        {
            return false;
        }
        parsed = (parsed << 4U) | static_cast<std::uint32_t>(digit);
    }
    color = parsed;
    return true;
}
} // namespace

namespace open_st
{
// 将可选颜色配置解析为 0xRRGGBB；任何缺失或语义非法值都回退纯黑。
std::uint32_t ResolveSelectionBorderColor(std::optional<std::string_view> configuredColor) noexcept
{
    std::uint32_t parsedColor{};
    if (!configuredColor.has_value())
    {
        return DEFAULT_SELECTION_BORDER_RGB;
    }

    if (!TryParseRgb(*configuredColor, parsedColor))
    {
        OPEN_ST_LOG_WARNING("The selection border color setting is invalid; using the black default.");
        return DEFAULT_SELECTION_BORDER_RGB;
    }
    return parsedColor;
}
} // namespace open_st
