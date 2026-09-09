// 声明界面语言选择、动态文本查询、参数替换和资源读取告警接口。

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

// 以当前可执行文件目录为基准建立本地化懒加载句柄。
// 入参：无。
// 返回：目录查询及句柄初始化成功时 true；失败或异常时 false，初始化不读取内容，后续查询或可用性检查时读取。
[[nodiscard]] bool InitializeUiText() noexcept;
// 释放本地化句柄和已接受的业务文本，重置语言与告警状态。
// 入参：无。
// 返回：无返回值；恢复默认 en-US，不清理 Common 管理器的缓存或名称绑定。
void ShutdownUiText() noexcept;
// 尝试读取当前资源并判断是否存在可显示的业务文本。
// 入参：无。
// 返回：存在当前或先前已接受的有效文档时 true；没有有效文档时 false，并非本次磁盘读取成功标志。
[[nodiscard]] bool IsUiTextAvailable() noexcept;
// 消费一次需要由应用显示的资源读取或结构错误告警。
// 入参：无。
// 返回：原本有待提示告警时 true，否则 false；仅清除待提示标记，持续故障仍保持去重。
[[nodiscard]] bool ConsumeUiTextReadWarning() noexcept;

// 将运行界面语言切换为当前资源声明的语言。
// 入参：languageCode：拟生效的语言代码。
// 返回：支持该代码时 true 并切换；不支持或异常时 false 并回退 en-US。
[[nodiscard]] bool SetUiLanguage(std::string_view languageCode) noexcept;
// 查询当前生效的运行语言。
// 入参：无。
// 返回：当前语言代码的独立字符串副本，查询本身不重新读取资源。
[[nodiscard]] std::string CurrentUiLanguageCode() noexcept;
// 获取供设置窗口使用的动态语言选项。
// 入参：无。
// 返回：按已接受文档 languages 顺序返回代码列表；没有有效文档或提取异常时返回空列表。
[[nodiscard]] std::vector<std::string> GetAvailableUiLanguages() noexcept;

// 取得本地化界面文本并替换调用方指定的命名占位符。
// 入参：key：动态文本键；arguments：占位符 name 和替换 value，仅在本次调用借用，空名称忽略。
// 返回：按参数顺序完成 {name} 替换的宽字符串；文本查找失败保留问号结果，未指定占位符不替换。
[[nodiscard]] std::wstring GetUiText(std::string_view key, std::initializer_list<UiTextArgument> arguments = {});
} // namespace open_st
