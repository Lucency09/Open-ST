// 声明设置模块内部的文档校验、目录注入、布局读取和编辑文件获取接口。

#pragma once

#include <filesystem>
#include <json_file.h>

namespace open_st
{
// 统一设置读取与编辑的固定外层协议，动态设置键不设白名单。
// 入参：document 为待校验的设置文档。
// 返回：版本为整数 1 且 settings 为对象时为 true；其他结构或异常为 false。
[[nodiscard]] bool IsSettingsDocument(const nlohmann::json& document) noexcept;
// 从隔离目录只读启动语言，供早期启动测试使用。
// 入参：applicationDirectory 为应用根目录，其 resources 和 data 子目录分别保存资源与用户设置。
// 返回：有效的启动语言代码；用户值无效时回退默认资源，两者均不可用时为 std::nullopt。
[[nodiscard]] std::optional<std::string> ReadStartupLanguage(
    const std::filesystem::path& applicationDirectory) noexcept;
// 允许测试注入隔离应用目录；产品代码使用无参数 InitializeSettings()，不公开测试路径接口。
// 入参：applicationDirectory 为应用根目录，其 resources 和 data 子目录分别保存资源与用户设置。
// 返回：默认设置可用且业务状态初始化成功时为 true；失败为 false。用户文件不可持久化通过独立状态查询报告。
[[nodiscard]] bool InitializeSettings(const std::filesystem::path& applicationDirectory) noexcept;
// 复制初始化时的用户和默认句柄供私有编辑会话使用，失败不改变输出。
// 入参：userFile 输出用户设置文件句柄；defaultFile 输出默认资源句柄；二者共享 Common 管理的文件状态。
// 返回：业务已初始化并复制两个句柄时为 true；未初始化时为 false 且输出保持原值。
[[nodiscard]] bool GetSettingsEditFiles(JsonFileHandle& userFile, JsonFileHandle& defaultFile) noexcept;
// 通过 Common 读取初始化目录下的布局资源，失败不改变输出。
// 入参：document 输出读取成功的窗口布局 JSON。
// 返回：布局读取成功为 true；未初始化、文件或解析失败为 false，失败不改变 document。
[[nodiscard]] bool ReadSettingsLayout(nlohmann::json& document) noexcept;
} // namespace open_st
