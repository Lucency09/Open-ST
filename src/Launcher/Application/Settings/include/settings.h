#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace open_st
{
// 读取默认设置并检查用户设置；仅在用户文件缺失时安全创建默认文档。
[[nodiscard]] bool InitializeSettings() noexcept;
// 释放设置业务持有的句柄，不干预 Common 内部缓存生命周期。
void ShutdownSettings() noexcept;
// 返回初始化或最近成功保存后记录的持久化可用性，不保证未来写入成功。
[[nodiscard]] bool IsSettingsPersistenceAvailable() noexcept;
// 消费一次运行期读取告警；持续故障不重复报告，读取恢复后允许再次报告。
[[nodiscard]] bool ConsumeSettingsReadWarning() noexcept;

// key 直接对应 settings 对象中的动态属性；用户值缺失或类型不符时读取默认配置。
[[nodiscard]] std::optional<std::string> GetStringSetting(std::string_view key) noexcept;
// 读取动态布尔设置；用户值不可用时回退默认值。
[[nodiscard]] std::optional<bool> GetBoolSetting(std::string_view key) noexcept;
// 读取动态整数设置；用户值不可用时回退默认值。
[[nodiscard]] std::optional<std::int64_t> GetIntegerSetting(std::string_view key) noexcept;

// 只更新指定动态 key，并保留文档中的其他设置和未知字段。
[[nodiscard]] bool SetStringSetting(std::string_view key, std::string_view value) noexcept;
// 修改指定布尔设置并保留未知字段；失败不提交修改。
[[nodiscard]] bool SetBoolSetting(std::string_view key, bool value) noexcept;
// 修改指定整数设置并保留未知字段；失败不提交修改。
[[nodiscard]] bool SetIntegerSetting(std::string_view key, std::int64_t value) noexcept;
} // namespace open_st
