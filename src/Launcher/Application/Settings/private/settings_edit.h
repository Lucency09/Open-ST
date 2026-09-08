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
    [[nodiscard]] bool Open(const std::vector<std::string>& keys) noexcept;
    // 读取已注册字段草稿；未知字段返回空值。
    [[nodiscard]] std::optional<std::string> ReadString(std::string_view key) const noexcept;
    // 修改已注册字段的草稿，不写盘或执行业务副作用。
    [[nodiscard]] bool ChangeString(std::string_view key, std::string_view value) noexcept;
    // 原子更新给定字段的默认草稿；默认资源或任意字段无效时保留全部草稿。
    [[nodiscard]] bool RestoreDefaults(const std::vector<std::string>& keys) noexcept;
    // 按显示基线比较，默认回退与修改后恢复原值不产生写入。
    [[nodiscard]] bool IsDirty() const noexcept;
    // 一次 Common 锁内编辑提交全部差异，失败不更新基线。
    [[nodiscard]] SettingsCommitResult Commit() noexcept;
    // 只读核对已保存目标，供宿主重试运行时生效，不执行写盘。
    [[nodiscard]] SettingsCommitResult VerifySavedString(std::string_view key, std::string_view value) const noexcept;

  private:
    struct Field
    {
        std::optional<nlohmann::json> raw;
        std::string baseline;
        std::string draft;
    };

    JsonFileHandle userFile_;
    JsonFileHandle defaultFile_;
    std::map<std::string, Field, std::less<>> fields_;
    bool ready_{};
};
} // namespace open_st
