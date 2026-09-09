// 定义设置编辑会话，负责字段草稿、冲突检查及条件提交。

#pragma once

#include <json_file.h>

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace open_st
{
enum class SettingsCommitResult
{
    Unchanged,
    Saved,
    Conflict,
    ReadFailed,
    WriteFailed,
    InvalidField
};

// UI 线程拥有的私有草稿；动态字段只由调用方显式提供，不维护业务键注册表。
class SettingsEditSession final
{
  public:
    // 读取原始用户字段和有效显示值，为指定设置字段建立编辑基线与草稿。
    // 入参：keys 为字符串字段名列表；boolKeys 为布尔字段名列表。
    // 返回：全部基线和有效草稿准备成功时为 true；文件、字段或类型不满足时为 false，保留原会话。
    [[nodiscard]] bool Open(const std::vector<std::string>& keys,
                            const std::vector<std::string>& boolKeys = {}) noexcept;
    // 读取已注册字段草稿；未知字段返回空值。
    // 入参：key 为 settings 对象中的动态设置属性名。
    // 返回：字符串草稿的副本；字段未注册、类型不符或读取失败为 std::nullopt。
    [[nodiscard]] std::optional<std::string> ReadString(std::string_view key) const noexcept;
    // 修改已注册字段的草稿，不写盘或执行业务副作用。
    // 入参：key 为 settings 对象中的动态设置属性名。value 为新的字符串草稿。
    // 返回：草稿修改成功为 true；未知字段、类型不符或分配失败为 false，保留原值。
    [[nodiscard]] bool ChangeString(std::string_view key, std::string_view value) noexcept;
    // 读取布尔草稿，拒绝类型不符的字段。
    // 入参：key 为 settings 对象中的动态设置属性名。
    // 返回：布尔草稿值；字段未注册、类型不符或读取失败为 std::nullopt。
    [[nodiscard]] std::optional<bool> ReadBool(std::string_view key) const noexcept;
    // 修改布尔草稿，不产生持久化或系统副作用。
    // 入参：key 为 settings 对象中的动态设置属性名。value 为新的布尔草稿。
    // 返回：已注册布尔字段修改成功为 true；未知字段、类型不符或异常为 false。
    [[nodiscard]] bool ChangeBool(std::string_view key, bool value) noexcept;
    // 校验重试目标仍是磁盘保存的布尔值。
    // 入参：key 为 settings 对象中的动态设置属性名。value 为待核验的已保存布尔目标。
    // 返回：磁盘字段仍等于目标时为 Unchanged；字段不合法为 InvalidField，读取失败为 ReadFailed，目标已变化为 Conflict。
    [[nodiscard]] SettingsCommitResult VerifySavedBool(std::string_view key, bool value) const noexcept;
    // 原子更新给定字段的默认草稿；默认资源或任意字段无效时保留全部草稿。
    // 入参：keys 为本次恢复默认的已注册字段名列表。
    // 返回：所有目标默认值有效并整体替换草稿时为 true；失败为 false 且保留全部原草稿。
    [[nodiscard]] bool RestoreDefaults(const std::vector<std::string>& keys) noexcept;
    // 检查整个编辑会话是否存在未提交的有效值变更。
    // 入参：无显式入参。
    // 返回：至少一个草稿偏离有效显示基线时为 true，否则为 false。
    [[nodiscard]] bool IsDirty() const noexcept;
    // 将变化字段及显式必需字段作为一批提交，并在提交成功后推进原始与显示基线。
    // 入参：requiredKeys 为即使显示值未改变也要求以准确类型持久化的字段名列表。
    // 返回：无须写入为 Unchanged，提交成功为 Saved；失败区分 Conflict、ReadFailed、WriteFailed 和 InvalidField。
    [[nodiscard]] SettingsCommitResult Commit(const std::vector<std::string>& requiredKeys = {}) noexcept;
    // 查询单字段差异，供宿主只触发变动字段的副作用。
    // 入参：key 为 settings 对象中的动态设置属性名。
    // 返回：指定字段草稿偏离有效显示基线时为 true；未注册或未改变为 false。
    [[nodiscard]] bool IsDirty(std::string_view key) const noexcept;
    // 只读核对已保存目标，供宿主重试运行时生效，不执行写盘。
    // 入参：key 为 settings 对象中的动态设置属性名。value 为待核验的已保存字符串目标。
    // 返回：磁盘字段仍等于目标时为 Unchanged；字段不合法为 InvalidField，读取失败为 ReadFailed，目标已变化为 Conflict。
    [[nodiscard]] SettingsCommitResult VerifySavedString(std::string_view key, std::string_view value) const noexcept;

  private:
    struct Field
    {
        std::optional<nlohmann::json> raw;
        nlohmann::json baseline;
        nlohmann::json draft;
    };

    JsonFileHandle userFile_;
    JsonFileHandle defaultFile_;
    std::map<std::string, Field, std::less<>> fields_;
    bool ready_{};
};
} // namespace open_st
