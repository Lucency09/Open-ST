// 实现截图存储参数校验和默认回退，设置窗口与两条输出链共享规则。
#include "capture_storage_options.h"
#include <settings.h>

namespace open_st
{
// 校验后构造规范 RGB 文本，输入错误不产生候选值。
// 入参：value 为原始颜色文本。
// 返回：大写 #RRGGBB 或空值。
std::optional<std::string> NormalizeSelectionBorderColor(std::string_view value)
{
    if (value.size() != 7 || value.front() != '#')
        return std::nullopt;
    for (std::size_t index = 1; index < value.size(); ++index)
    {
        const char digit = value[index];
        if (!((digit >= '0' && digit <= '9') || (digit >= 'A' && digit <= 'F') || (digit >= 'a' && digit <= 'f')))
            return std::nullopt;
    }
    std::string normalized(value);
    for (char& digit : normalized)
        if (digit >= 'a' && digit <= 'f')
            digit = static_cast<char>(digit - 'a' + 'A');
    return normalized;
}
// 只接受产品支持的固定格式 token。
// 入参：value 为配置值。
// 返回：jpeg/png 为 true。
bool IsImageFormatSetting(std::string_view value) noexcept
{
    return value == "jpeg" || value == "png";
}
// 集中声明产品 JPEG 质量范围。
// 入参：value 为整数级别。
// 返回：1–100 为 true。
bool IsJpegQualitySetting(std::int64_t value) noexcept
{
    return value >= 1 && value <= 100;
}
// 从类型化设置接口读取参数；跨 getter 不承诺外部文件的原子快照。
// 入参：无。
// 返回：参数及故障位，不可用质量不阻断后续 PNG 选择。
ImageSavePreferences ReadImageSavePreferences() noexcept
{
    ImageSavePreferences preferences;
    bool invalidFormatType = false;
    std::optional<std::string> format = GetStringSetting("export.default_format", invalidFormatType);
    if (invalidFormatType || !format || !IsImageFormatSetting(*format))
    {
        preferences.invalidFields |= 1U;
        format = GetDefaultStringSetting("export.default_format");
    }
    if (format && IsImageFormatSetting(*format))
        preferences.defaultFormat = *format == "jpeg" ? ImageFileFormat::Jpeg : ImageFileFormat::Png;
    bool invalidQualityType = false;
    std::optional<std::int64_t> quality = GetIntegerSetting("export.jpeg_quality", invalidQualityType);
    if (invalidQualityType || !quality || !IsJpegQualitySetting(*quality))
    {
        preferences.invalidFields |= 2U;
        quality = GetDefaultIntegerSetting("export.jpeg_quality");
    }
    if (quality && IsJpegQualitySetting(*quality))
        preferences.jpegQuality = static_cast<int>(*quality);
    return preferences;
}
// 按实际格式检验固定参数，不把 JPEG 质量错误应用到 PNG。
// 入参：preferences 为固定参数；format 为实际选择；options 接收结果。
// 返回：当前格式所需参数满足要求为 true，否则保留输出。
bool MakeImageEncodingOptions(const ImageSavePreferences& preferences, ImageFileFormat format,
                              ImageEncodingOptions& options) noexcept
{
    if (format != ImageFileFormat::Jpeg && format != ImageFileFormat::Png)
        return false;
    if (format == ImageFileFormat::Jpeg &&
        (!preferences.jpegQuality || !IsJpegQualitySetting(*preferences.jpegQuality)))
        return false;
    options = ImageEncodingOptions{format, preferences.jpegQuality.value_or(0)};
    return true;
}
} // namespace open_st
