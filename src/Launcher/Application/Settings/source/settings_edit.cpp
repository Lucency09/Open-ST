#include "settings_edit.h"
#include "settings_internal.h"

#include <utility>

namespace
{
// 编辑协议只接受已存在的版本一设置对象，拒绝缺失和结构损坏的覆盖重建。
bool IsEditableDocument(const nlohmann::json& document)
{
    return document.is_object() && document.contains("schemaVersion") &&
           document.at("schemaVersion").is_number_integer() && document.at("schemaVersion") == 1 &&
           document.contains("settings") && document.at("settings").is_object();
}

// 空 optional 表示字段缺失；显式 null 与其他原始类型均完整保留。
std::optional<nlohmann::json> RawField(const nlohmann::json& document, std::string_view key)
{
    const nlohmann::json& settings = document.at("settings");
    const nlohmann::json::const_iterator value = settings.find(key);
    return value == settings.end() ? std::nullopt : std::optional<nlohmann::json>(*value);
}

// 比较存在性与原始 JSON 类型，避免数值跨类型相等掩盖外部修改。
bool SameRaw(const std::optional<nlohmann::json>& left, const std::optional<nlohmann::json>& right)
{
    return left.has_value() == right.has_value() &&
           (!left.has_value() || (left->type() == right->type() && *left == *right));
}
} // namespace

namespace open_st
{
// 基线准备全部成功后才交换会话，避免读取失败留下部分注册字段。
bool SettingsEditSession::Open(const std::vector<std::string>& keys) noexcept
{
    try
    {
        SettingsEditSession candidate;
        if (!GetSettingsEditFiles(candidate.userFile_, candidate.defaultFile_))
        {
            return false;
        }
        nlohmann::json user;
        nlohmann::json defaults;
        if (!candidate.userFile_.Read(user) || !IsEditableDocument(user))
        {
            return false;
        }
        const bool defaultsValid = candidate.defaultFile_.Read(defaults) && IsEditableDocument(defaults);
        for (const std::string& key : keys)
        {
            if (key.empty() || candidate.fields_.contains(key))
            {
                return false;
            }
            Field field;
            field.raw = RawField(user, key);
            std::optional<nlohmann::json> effective = field.raw;
            if (!effective.has_value() || !effective->is_string())
            {
                effective = defaultsValid ? RawField(defaults, key) : std::nullopt;
            }
            if (!effective.has_value() || !effective->is_string())
            {
                return false;
            }
            field.baseline = effective->get<std::string>();
            field.draft = field.baseline;
            candidate.fields_.emplace(key, std::move(field));
        }
        candidate.ready_ = true;
        *this = std::move(candidate);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

// 返回值副本保证控件刷新不会持有可失效的草稿引用。
std::optional<std::string> SettingsEditSession::ReadString(std::string_view key) const noexcept
{
    try
    {
        const auto field = this->fields_.find(key);
        return field == this->fields_.end() ? std::nullopt : std::optional<std::string>(field->second.draft);
    }
    catch (...)
    {
        return std::nullopt;
    }
}

// 字符串分配成功后交换草稿，失败不改变原有值。
bool SettingsEditSession::ChangeString(std::string_view key, std::string_view value) noexcept
{
    try
    {
        const auto field = this->fields_.find(key);
        if (!this->ready_ || field == this->fields_.end())
        {
            return false;
        }
        std::string candidate(value);
        field->second.draft.swap(candidate);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

// 每次恢复都重新读取默认资源，采用候选副本保证多字段全成或全败。
bool SettingsEditSession::RestoreDefaults(const std::vector<std::string>& keys) noexcept
{
    try
    {
        nlohmann::json defaults;
        if (!this->ready_ || !this->defaultFile_.Read(defaults) || !IsEditableDocument(defaults))
        {
            return false;
        }
        auto candidate = this->fields_;
        for (const std::string& key : keys)
        {
            const auto field = candidate.find(key);
            const std::optional<nlohmann::json> value = RawField(defaults, key);
            if (field == candidate.end() || !value.has_value() || !value->is_string())
            {
                return false;
            }
            field->second.draft = value->get<std::string>();
        }
        this->fields_.swap(candidate);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

// 仅真正偏离有效显示基线的字段才参与提交。
bool SettingsEditSession::IsDirty() const noexcept
{
    for (const auto& [key, field] : this->fields_)
    {
        if (field.draft != field.baseline)
        {
            return true;
        }
    }
    return false;
}

// 预先准备成功后的基线，文件提交成功后只交换内存，不再进行可能失败的分配。
SettingsCommitResult SettingsEditSession::Commit() noexcept
{
    try
    {
        if (!this->ready_)
        {
            return SettingsCommitResult::ReadFailed;
        }
        if (!this->IsDirty())
        {
            return SettingsCommitResult::Unchanged;
        }
        auto committed = this->fields_;
        for (auto& [key, field] : committed)
        {
            if (field.draft != field.baseline)
            {
                field.raw = field.draft;
                field.baseline = field.draft;
            }
        }
        SettingsCommitResult failure = SettingsCommitResult::WriteFailed;
        const bool saved = this->userFile_.Write(
            [this, &failure](std::optional<nlohmann::json>& document)
            {
                if (!document.has_value() || !IsEditableDocument(*document))
                {
                    failure = SettingsCommitResult::ReadFailed;
                    return false;
                }
                for (const auto& [key, field] : this->fields_)
                {
                    if (field.draft == field.baseline)
                    {
                        continue;
                    }
                    const std::optional<nlohmann::json> latest = RawField(*document, key);
                    const std::optional<nlohmann::json> target = nlohmann::json(field.draft);
                    if (!SameRaw(latest, field.raw) && !SameRaw(latest, target))
                    {
                        failure = SettingsCommitResult::Conflict;
                        return false;
                    }
                }
                for (const auto& [key, field] : this->fields_)
                {
                    if (field.draft != field.baseline)
                    {
                        (*document)["settings"][key] = field.draft;
                    }
                }
                return true;
            });
        if (!saved)
        {
            return failure;
        }
        this->fields_.swap(committed);
        return SettingsCommitResult::Saved;
    }
    catch (...)
    {
        return SettingsCommitResult::WriteFailed;
    }
}

// 重试只接受持久化字符串仍等于目标的情况，默认回退不能证明目标已保存。
SettingsCommitResult SettingsEditSession::VerifySavedString(std::string_view key, std::string_view value) const noexcept
{
    try
    {
        if (!this->ready_ || !this->fields_.contains(key))
        {
            return SettingsCommitResult::InvalidField;
        }
        nlohmann::json document;
        if (!this->userFile_.Read(document) || !IsEditableDocument(document))
        {
            return SettingsCommitResult::ReadFailed;
        }
        const std::optional<nlohmann::json> persisted = RawField(document, key);
        return persisted.has_value() && persisted->is_string() && persisted->get_ref<const std::string&>() == value
                   ? SettingsCommitResult::Unchanged
                   : SettingsCommitResult::Conflict;
    }
    catch (...)
    {
        return SettingsCommitResult::ReadFailed;
    }
}
} // namespace open_st
