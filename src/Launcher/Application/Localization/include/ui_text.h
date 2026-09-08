#pragma once

#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace open_st
{
struct UiTextArgument final
{
    std::wstring_view name;
    std::wstring_view value;
};

// 建立可执行文件旁 resources/ui_text.json 的懒加载句柄；第一次查询文本时才读盘。
[[nodiscard]] bool InitializeUiText() noexcept;
// 释放本地化业务句柄、已生效文本及告警状态。
void ShutdownUiText() noexcept;
// 按需读取并判断是否存在可供显示的已生效业务文本。
[[nodiscard]] bool IsUiTextAvailable() noexcept;
// 消费一次读取或业务结构故障告警；连续故障只报告一次，恢复后重新允许报告。
[[nodiscard]] bool ConsumeUiTextReadWarning() noexcept;

// 接受当前 JSON languages 数组声明的语言代码；无效值会切回默认 en-US 并返回 false。
[[nodiscard]] bool SetUiLanguage(std::string_view languageCode) noexcept;
// 返回当前运行时语言代码的副本。
[[nodiscard]] std::string CurrentUiLanguageCode() noexcept;
// 按已接受资源的 languages 数组顺序返回语言代码；无有效资源时返回空列表。
[[nodiscard]] std::vector<std::string> GetAvailableUiLanguages() noexcept;

// key 直接对应 JSON 中的文本键；查询前会检查资源文件版本，arguments 只执行受控占位符替换。
[[nodiscard]] std::wstring GetUiText(std::string_view key, std::initializer_list<UiTextArgument> arguments = {});
} // namespace open_st
