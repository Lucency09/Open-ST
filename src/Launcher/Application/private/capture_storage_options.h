// 声明截图存储参数校验及一次保存固定参数，不暴露设置 JSON 或文件句柄。
#pragma once
#include <cstdint>
#include <image_file_writer.h>
#include <optional>
#include <string>
#include <string_view>

namespace open_st
{
struct ImageSavePreferences final
{
    std::optional<ImageFileFormat> defaultFormat;
    std::optional<int> jpegQuality;
    std::uint32_t invalidFields{}; // 位 0 为格式，位 1 为质量，仅用于连续告警去重。
};
// 校验严格 RGB 文本并生成显式保存使用的大写值。
// 入参：value 为原始 #RRGGBB，不接受空白、简写或透明度。
// 返回：合法规范值；无效为空。
std::optional<std::string> NormalizeSelectionBorderColor(std::string_view value);
// 校验与语言无关的文件格式 token。
// 入参：value 为配置文本。
// 返回：jpeg 或 png 为 true。
bool IsImageFormatSetting(std::string_view value) noexcept;
// 校验 JPEG 质量整数。
// 入参：value 为质量级别。
// 返回：1–100 为 true。
bool IsJpegQualitySetting(std::int64_t value) noexcept;
// 读取用户优先的固定参数，语义错误时尝试默认资源，不写回配置。
// 入参：无。
// 返回：可用参数及故障位；无有效默认时对应参数为空。
ImageSavePreferences ReadImageSavePreferences() noexcept;
// 为本次实际格式生成编码参数，PNG 不要求 JPEG 质量可用。
// 入参：preferences 为固定参数；format 为选择格式；options 接收结果。
// 返回：参数可用为 true；失败不改输出。
bool MakeImageEncodingOptions(const ImageSavePreferences& preferences, ImageFileFormat format,
                              ImageEncodingOptions& options) noexcept;
} // namespace open_st
