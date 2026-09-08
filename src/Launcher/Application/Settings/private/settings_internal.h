#pragma once

#include <filesystem>
#include <json_file.h>

namespace open_st
{
// 允许测试注入隔离应用目录；产品代码使用无参数 InitializeSettings()，不公开测试路径接口。
[[nodiscard]] bool InitializeSettings(const std::filesystem::path& applicationDirectory) noexcept;
// 复制初始化时的用户和默认句柄供私有编辑会话使用，失败不改变输出。
[[nodiscard]] bool GetSettingsEditFiles(JsonFileHandle& userFile, JsonFileHandle& defaultFile) noexcept;
// 通过 Common 读取初始化目录下的布局资源，失败不改变输出。
[[nodiscard]] bool ReadSettingsLayout(nlohmann::json& document) noexcept;
} // namespace open_st
