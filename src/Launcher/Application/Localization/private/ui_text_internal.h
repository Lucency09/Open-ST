// 提供本地化初始化的测试目录注入入口，隔离实际运行资源。

#pragma once

#include <filesystem>

namespace open_st
{
// 建立本地化资源的懒加载句柄并重置运行语言。
// 入参：applicationDirectory：包含 resources/ui_text.json 的应用目录。
// 返回：取得有效 JSON 句柄时 true，不表示资源内容已读入；句柄获取或初始化异常时 false。
[[nodiscard]] bool InitializeUiText(const std::filesystem::path& applicationDirectory) noexcept;
} // namespace open_st
