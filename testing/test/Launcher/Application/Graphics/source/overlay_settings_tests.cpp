#include <gtest/gtest.h>

#include "overlay_settings.h"

#include <array>
#include <optional>
#include <string_view>

// 验证严格 #RRGGBB 支持大小写十六进制、颜色极值和正确的 RGB 通道顺序。
TEST(OverlaySettingsTest, parses_valid_rgb_colors)
{
    EXPECT_EQ(open_st::ResolveSelectionBorderColor(std::string_view{"#000000"}), 0x000000U);
    EXPECT_EQ(open_st::ResolveSelectionBorderColor(std::string_view{"#FFFFFF"}), 0xFFFFFFU);
    EXPECT_EQ(open_st::ResolveSelectionBorderColor(std::string_view{"#12aBcF"}), 0x12ABCFU);
    EXPECT_EQ(open_st::ResolveSelectionBorderColor(std::string_view{"#FF0000"}), 0xFF0000U);
    EXPECT_EQ(open_st::ResolveSelectionBorderColor(std::string_view{"#00FF00"}), 0x00FF00U);
    EXPECT_EQ(open_st::ResolveSelectionBorderColor(std::string_view{"#0000FF"}), 0x0000FFU);
}

// 验证设置缺失时直接使用约定的纯黑默认值。
TEST(OverlaySettingsTest, falls_back_to_black_when_setting_is_missing)
{
    EXPECT_EQ(open_st::ResolveSelectionBorderColor(std::nullopt), open_st::DEFAULT_SELECTION_BORDER_RGB);
}

// 验证长度、字符、alpha、空格或前缀不符合协议时统一回退纯黑。
TEST(OverlaySettingsTest, rejects_ambiguous_or_invalid_color_formats)
{
    constexpr std::array<std::string_view, 8> invalidValues{
        "000000",
        "#000",
        "#00000000",
        " #000000",
        "#000000 ",
        "#GG0000",
        "",
        "#12345Z",
    };

    for (const std::string_view invalidValue : invalidValues)
    {
        EXPECT_EQ(open_st::ResolveSelectionBorderColor(invalidValue), open_st::DEFAULT_SELECTION_BORDER_RGB);
    }
}
