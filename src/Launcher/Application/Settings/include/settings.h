// 声明动态设置读写、启动期语言读取及持久化告警接口。

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace open_st
{
// 在完整设置初始化前只读启动语言，不创建用户配置。
// 入参：无显式入参。
// 返回：有效的启动语言代码；用户值无效时回退默认资源，两者均不可用时为 std::nullopt。
[[nodiscard]] std::optional<std::string> ReadStartupLanguage() noexcept;
// 读取默认设置并检查用户设置；仅在用户文件缺失时安全创建默认文档。
// 入参：无显式入参。
// 返回：默认设置可用且业务状态初始化成功时为 true；失败为 false。用户文件不可持久化通过独立状态查询报告。
[[nodiscard]] bool InitializeSettings() noexcept;
// 释放设置业务持有的句柄，不干预 Common 内部缓存生命周期。
// 入参：无显式入参。
// 返回：无返回值。
void ShutdownSettings() noexcept;
// 查询设置业务当前记录的持久化可用性。
// 入参：无显式入参。
// 返回：最近一次初始化或成功写入记录为可持久化时为 true；未初始化或记录失败时为 false，不保证下一次写入成功。
[[nodiscard]] bool IsSettingsPersistenceAvailable() noexcept;
// 消费一次运行期读取告警；持续故障不重复报告，读取恢复后允许再次报告。
// 入参：无显式入参。
// 返回：本次取走待显示读取告警时为 true；没有待显示告警为 false。
[[nodiscard]] bool ConsumeSettingsReadWarning() noexcept;

// 按动态属性名读取字符串设置，用户值不可用时回退默认资源。
// 入参：key 为 settings 对象中的动态设置属性名。
// 返回：用户配置或默认资源中的有效字符串值；均不可用或类型错误时为 std::nullopt。
[[nodiscard]] std::optional<std::string> GetStringSetting(std::string_view key) noexcept;
// 按动态属性名读取布尔设置，用户值不可用时回退默认资源。
// 入参：key 为 settings 对象中的动态设置属性名。
// 返回：用户配置或默认资源中的有效布尔值；均不可用或类型错误时为 std::nullopt。
[[nodiscard]] std::optional<bool> GetBoolSetting(std::string_view key) noexcept;
// 按动态属性名读取有符号整数设置，用户值不可用时回退默认资源。
// 入参：key 为 settings 对象中的动态设置属性名。
// 返回：用户配置或默认资源中可由 int64_t 表示的整数；均缺失、类型错误或超范围时为 std::nullopt。
[[nodiscard]] std::optional<std::int64_t> GetIntegerSetting(std::string_view key) noexcept;

// 只更新指定动态 key，并保留文档中的其他设置和未知字段。
// 入参：key 为 settings 对象中的动态设置属性名。value 为要持久化的字符串值。
// 返回：字段成功写入或安全核验无变化时为 true；参数、读取、文档结构或写入失败为 false。
[[nodiscard]] bool SetStringSetting(std::string_view key, std::string_view value) noexcept;
// 修改指定布尔设置并保留未知字段；失败不提交修改。
// 入参：key 为 settings 对象中的动态设置属性名。value 为要持久化的布尔值。
// 返回：字段成功写入或安全核验无变化时为 true；参数、读取、文档结构或写入失败为 false。
[[nodiscard]] bool SetBoolSetting(std::string_view key, bool value) noexcept;
// 修改指定整数设置并保留未知字段；失败不提交修改。
// 入参：key 为 settings 对象中的动态设置属性名。value 为要持久化的有符号整数值。
// 返回：字段成功写入或安全核验无变化时为 true；参数、读取、文档结构或写入失败为 false。
[[nodiscard]] bool SetIntegerSetting(std::string_view key, std::int64_t value) noexcept;
} // namespace open_st
