// 文件职责：解析可选 RGB 边框颜色文本，对缺失或无效输入采用统一默认颜色。

#include "overlay_settings.h"

#include <log.h>

#include <cstddef>
#include <optional>

namespace
{
// 解析一个十六进制字符以支持 RGB 边框颜色配置。
// 入参：value：待解析的 ASCII 字符。
// 返回：合法十六进制字符对应 0 至 15；其他字符返回 -1。
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

// 校验并解析严格的 #RRGGBB 文本为边框 RGB 值。
// 入参：value：待解析的颜色文本；color：输出参数，成功时写入 0xRRGGBB。
// 返回：格式和全部六位字符合法时为 true；失败为 false 且不改变 color。
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
// 从可选设置文本确定截图边框颜色，统一缺失和非法值的回退。
// 入参：configuredColor：可选的 #RRGGBB 文本视图，仅在本次调用借用。
// 返回：合法文本对应的 0xRRGGBB；缺失或解析失败返回默认黑色。
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
