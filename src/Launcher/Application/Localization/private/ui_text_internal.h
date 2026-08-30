#pragma once

#include <filesystem>

namespace open_st
{
// 测试注入应用目录；产品代码使用无参数 InitializeUiText()，不接触资源文件实现细节。
[[nodiscard]] bool InitializeUiText(const std::filesystem::path& applicationDirectory) noexcept;
} // namespace open_st
