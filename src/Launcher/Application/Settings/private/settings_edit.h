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
    // 同时取得可靠用户原始基线和有效显示值；失败保留原会话以供明确重试。
    [[nodiscard]] bool Open(const std::vector<std::string>& keys,
                            const std::vector<std::string>& boolKeys = {}) noexcept;
    // 读取已注册字段草稿；未知字段返回空值。
    [[nodiscard]] std::optional<std::string> ReadString(std::string_view key) const noexcept;
    // 修改已注册字段的草稿，不写盘或执行业务副作用。
    [[nodiscard]] bool ChangeString(std::string_view key, std::string_view value) noexcept;
    // 读取布尔草稿，拒绝类型不符的字段。
    [[nodiscard]] std::optional<bool> ReadBool(std::string_view key) const noexcept;
    // 修改布尔草稿，不产生持久化或系统副作用。
    [[nodiscard]] bool ChangeBool(std::string_view key, bool value) noexcept;
    // 校验重试目标仍是磁盘保存的布尔值。
    [[nodiscard]] SettingsCommitResult VerifySavedBool(std::string_view key, bool value) const noexcept;
    // 原子更新给定字段的默认草稿；默认资源或任意字段无效时保留全部草稿。
    [[nodiscard]] bool RestoreDefaults(const std::vector<std::string>& keys) noexcept;
    // 按显示基线比较，默认回退与修改后恢复原值不产生写入。
    [[nodiscard]] bool IsDirty() const noexcept;
    // 一次 Common 锁内提交差异及显式要求持久化的字段，失败不更新基线。
    [[nodiscard]] SettingsCommitResult Commit(const std::vector<std::string>& requiredKeys = {}) noexcept;
    // 查询单字段差异，供宿主只触发变动字段的副作用。
    [[nodiscard]] bool IsDirty(std::string_view key) const noexcept;
    // 只读核对已保存目标，供宿主重试运行时生效，不执行写盘。
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
