#pragma once

#include <filesystem>

namespace open_st
{
// 允许测试注入隔离应用目录；产品代码使用无参数 InitializeSettings()，不公开测试路径接口。
[[nodiscard]] bool InitializeSettings(const std::filesystem::path& applicationDirectory) noexcept;
} // namespace open_st
