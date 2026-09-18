// 实现设置草稿读写、默认值恢复、已保存值复核及冲突保护提交。

#include "settings_edit.h"
#include "settings_internal.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace
{
// 严格读取可表示整数，统一 JSON 正数和显式有符号输入的比较语义。
// 入参：value 为原始 JSON 值。
// 返回：int64_t 整数；浮点、布尔或溢出时为空。
std::optional<std::int64_t> IntegerValue(const nlohmann::json& value)
{
    if (!value.is_number_integer() ||
        (value.is_number_unsigned() &&
         value.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())))
        return std::nullopt;
    return value.get<std::int64_t>();
}
// 提取字段的原始持久化值，用于区分缺失、显式 null 和有效显示默认值。
// 入参：document 为已通过编辑协议校验的设置文档；key 为动态字段名。
// 返回：字段存在时返回保留原始类型的 JSON 副本，包括显式 null；缺失时返回 std::nullopt。
std::optional<nlohmann::json> RawField(const nlohmann::json& document, std::string_view key)
{
    const nlohmann::json& settings = document.at("settings");
    const nlohmann::json::const_iterator value = settings.find(key);
    return value == settings.end() ? std::nullopt : std::optional<nlohmann::json>(*value);
}

// 比较存在性与原始 JSON 类型，避免数值跨类型相等掩盖外部修改。
// 入参：left、right 为待比较的原始字段 optional，空值表示字段缺失。
// 返回：存在性、JSON 类型和内容均一致为 true，否则为 false。
bool SameRaw(const std::optional<nlohmann::json>& left, const std::optional<nlohmann::json>& right)
{
    if (left && right && left->is_number_integer() && right->is_number_integer())
    {
        const std::optional<std::int64_t> leftInteger = IntegerValue(*left);
        const std::optional<std::int64_t> rightInteger = IntegerValue(*right);
        if (leftInteger && rightInteger)
            return *leftInteger == *rightInteger;
    }
    return left.has_value() == right.has_value() &&
           (!left.has_value() || (left->type() == right->type() && *left == *right));
}
} // namespace

namespace open_st
{
// 读取原始用户字段和有效显示值，为指定设置字段建立编辑基线与草稿。
// 入参：keys 为字符串列表；boolKeys 为布尔列表；integerKeys 为整数列表。
// 返回：全部基线和有效草稿准备成功时为 true；文件、字段或类型不满足时为 false，保留原会话。
bool SettingsEditSession::Open(const std::vector<std::string>& keys, const std::vector<std::string>& boolKeys,
                               const std::vector<std::string>& integerKeys) noexcept
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
        if (!candidate.userFile_.Read(user) || !IsSettingsDocument(user))
        {
            return false;
        }
        const bool defaultsValid = candidate.defaultFile_.Read(defaults) && IsSettingsDocument(defaults);
        std::vector<std::string> allKeys = keys;
        allKeys.insert(allKeys.end(), boolKeys.begin(), boolKeys.end());
        allKeys.insert(allKeys.end(), integerKeys.begin(), integerKeys.end());
        for (const std::string& key : allKeys)
        {
            const bool boolean = std::find(boolKeys.begin(), boolKeys.end(), key) != boolKeys.end();
            const bool integer = std::find(integerKeys.begin(), integerKeys.end(), key) != integerKeys.end();
            // 依据本字段登记类型检查原始值，不混同浮点和整数。
            // 入参：value 为待验证 JSON 字段。
            // 返回：类型和可表示范围正确时为 true。
            const auto valid = [boolean, integer](const nlohmann::json& value)
            {
                return integer ? IntegerValue(value).has_value() : boolean ? value.is_boolean() : value.is_string();
            };
            if (key.empty() || candidate.fields_.contains(key))
            {
                return false;
            }
            Field field;
            field.integer = integer;
            field.raw = RawField(user, key);
            field.requiresRepair = field.raw && !valid(*field.raw);
            std::optional<nlohmann::json> effective = field.raw;
            if (!effective || !valid(*effective))
            {
                effective = defaultsValid ? RawField(defaults, key) : std::nullopt;
            }
            if (!effective || !valid(*effective))
            {
                return false;
            }
            field.baseline = integer ? nlohmann::json(*IntegerValue(*effective)) : *effective;
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
// 入参：key 为 settings 对象中的动态设置属性名。
// 返回：字符串草稿的副本；字段未注册、类型不符或读取失败为 std::nullopt。
std::optional<std::string> SettingsEditSession::ReadString(std::string_view key) const noexcept
{
    try
    {
        const auto field = this->fields_.find(key);
        return field == this->fields_.end() || !field->second.draft.is_string()
                   ? std::nullopt
                   : std::optional<std::string>(field->second.draft.get<std::string>());
    }
    catch (...)
    {
        return std::nullopt;
    }
}

// 字符串分配成功后交换草稿，失败不改变原有值。
// 入参：key 为 settings 对象中的动态设置属性名。value 为新的字符串草稿。
// 返回：草稿修改成功为 true；未知字段、类型不符或分配失败为 false，保留原值。
bool SettingsEditSession::ChangeString(std::string_view key, std::string_view value) noexcept
{
    try
    {
        const auto field = this->fields_.find(key);
        if (!this->ready_ || field == this->fields_.end() || !field->second.draft.is_string())
        {
            return false;
        }
        nlohmann::json candidate = std::string(value);
        field->second.draft.swap(candidate);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

// 每次恢复都重新读取默认资源，采用候选副本保证多字段全成或全败。
// 入参：keys 为本次恢复默认的已注册字段名列表。
// 返回：所有目标默认值有效并整体替换草稿时为 true；失败为 false 且保留全部原草稿。
bool SettingsEditSession::RestoreDefaults(const std::vector<std::string>& keys) noexcept
{
    try
    {
        nlohmann::json defaults;
        if (!this->ready_ || !this->defaultFile_.Read(defaults) || !IsSettingsDocument(defaults))
        {
            return false;
        }
        auto candidate = this->fields_;
        for (const std::string& key : keys)
        {
            const auto field = candidate.find(key);
            const std::optional<nlohmann::json> value = RawField(defaults, key);
            if (field == candidate.end() || !value ||
                (field->second.integer ? !IntegerValue(*value).has_value()
                                       : value->type() != field->second.baseline.type()))
            {
                return false;
            }
            field->second.draft = field->second.integer ? nlohmann::json(*IntegerValue(*value)) : *value;
        }
        this->fields_.swap(candidate);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

// 检查整个编辑会话是否存在未提交的有效值变更。
// 入参：无显式入参。
// 返回：至少一个草稿偏离有效显示基线时为 true，否则为 false。
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

// 将变化字段及显式必需字段作为一批提交，并在提交成功后推进原始与显示基线。
// 入参：requiredKeys 为需要准确类型持久化的字段；explicitlyEditedKeys 为规范化后仍须条件提交的显式编辑字段。
// 返回：无须写入为 Unchanged，提交成功为 Saved；失败区分 Conflict、ReadFailed、WriteFailed 和 InvalidField。
SettingsCommitResult SettingsEditSession::Commit(const std::vector<std::string>& requiredKeys,
                                                 const std::vector<std::string>& explicitlyEditedKeys) noexcept
{
    try
    {
        if (!this->ready_)
        {
            return SettingsCommitResult::ReadFailed;
        }
        for (const std::string& key : requiredKeys)
        {
            if (!this->fields_.contains(key))
                return SettingsCommitResult::InvalidField;
        }
        for (const std::string& key : explicitlyEditedKeys)
        {
            if (!this->fields_.contains(key))
                return SettingsCommitResult::InvalidField;
        }
        // 判断字段是否改动，或必需字段是否尚未以准确类型持久化。
        // 入参：key 为待检查字段名；field 包含该字段的原始值、显示基线和当前草稿。
        // 返回：草稿已改变或必需字段尚未准确持久化时为 true，否则为 false。
        const auto needsWrite = [&requiredKeys, &explicitlyEditedKeys](const std::string& key, const Field& field)
        {
            return std::find(explicitlyEditedKeys.begin(), explicitlyEditedKeys.end(), key) !=
                       explicitlyEditedKeys.end() ||
                   field.draft != field.baseline ||
                   (std::find(requiredKeys.begin(), requiredKeys.end(), key) != requiredKeys.end() &&
                    !SameRaw(field.raw, std::optional<nlohmann::json>(field.draft)));
        };
        if (!std::any_of(this->fields_.begin(), this->fields_.end(),
                         // 按字段键和草稿检查是否存在需要写入的设置项。
                         // 入参：entry 为字段映射中的键与 Field 条目。
                         // 返回：该字段需要写入时为 true，否则为 false。
                         [&needsWrite](const auto& entry) { return needsWrite(entry.first, entry.second); }))
        {
            return SettingsCommitResult::Unchanged;
        }
        auto committed = this->fields_;
        for (auto& [key, field] : committed)
        {
            if (needsWrite(key, field))
            {
                field.raw = field.draft;
                field.baseline = field.draft;
                field.requiresRepair = false;
            }
        }
        SettingsCommitResult failure = SettingsCommitResult::WriteFailed;
        JsonFileError writeError;
        const bool saved = this->userFile_.Write(
            // 在文件编辑锁内先核对所有待写字段的基线，再一次性更新候选文档。
            // 入参：document 为 Common 锁内的候选文档，空 optional 表示文件缺失。
            // 返回：文档合法且所有待写字段基线未冲突时更新候选并返回 true；否则记录失败类别并返回 false。
            [this, &failure, &needsWrite](std::optional<nlohmann::json>& document)
            {
                if (!document.has_value() || !IsSettingsDocument(*document))
                {
                    failure = SettingsCommitResult::ReadFailed;
                    return false;
                }
                for (const auto& [key, field] : this->fields_)
                {
                    if (!needsWrite(key, field))
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
                    if (needsWrite(key, field))
                    {
                        (*document)["settings"][key] = field.draft;
                    }
                }
                return true;
            },
            &writeError);
        if (!saved)
        {
            return writeError.code == JsonFileErrorCode::Busy ? SettingsCommitResult::Busy : failure;
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
// 入参：key 为 settings 对象中的动态设置属性名。value 为待核验的已保存字符串目标。
// 返回：磁盘字段仍等于目标时为 Unchanged；字段不合法为 InvalidField，读取失败为 ReadFailed，目标已变化为 Conflict。
SettingsCommitResult SettingsEditSession::VerifySavedString(std::string_view key, std::string_view value) const noexcept
{
    try
    {
        if (!this->ready_ || !this->fields_.contains(key))
        {
            return SettingsCommitResult::InvalidField;
        }
        nlohmann::json document;
        JsonFileError error;
        if (!this->userFile_.Read(document, &error) || !IsSettingsDocument(document))
        {
            return error.code == JsonFileErrorCode::Busy ? SettingsCommitResult::Busy
                                                         : SettingsCommitResult::ReadFailed;
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
// 布尔读取保持类型严格，缺失不自动当作 false。
// 入参：key 为 settings 对象中的动态设置属性名。
// 返回：布尔草稿值；字段未注册、类型不符或读取失败为 std::nullopt。
std::optional<bool> SettingsEditSession::ReadBool(std::string_view key) const noexcept
{
    const auto field = this->fields_.find(key);
    return field == this->fields_.end() || !field->second.draft.is_boolean()
               ? std::nullopt
               : std::optional<bool>(field->second.draft.get<bool>());
}

// 布尔字段只接受布尔修改。
// 入参：key 为 settings 对象中的动态设置属性名。value 为新的布尔草稿。
// 返回：已注册布尔字段修改成功为 true；未知字段、类型不符或异常为 false。
bool SettingsEditSession::ChangeBool(std::string_view key, bool value) noexcept
{
    const auto field = this->fields_.find(key);
    if (!this->ready_ || field == this->fields_.end() || !field->second.draft.is_boolean())
        return false;
    field->second.draft = value;
    return true;
}

// 重试前读取原始文件，默认值不能冒充已保存意图。
// 入参：key 为 settings 对象中的动态设置属性名。value 为待核验的已保存布尔目标。
// 返回：磁盘字段仍等于目标时为 Unchanged；字段不合法为 InvalidField，读取失败为 ReadFailed，目标已变化为 Conflict。
SettingsCommitResult SettingsEditSession::VerifySavedBool(std::string_view key, bool value) const noexcept
{
    try
    {
        if (!this->ready_ || !this->fields_.contains(key))
            return SettingsCommitResult::InvalidField;
        nlohmann::json document;
        JsonFileError error;
        if (!this->userFile_.Read(document, &error) || !IsSettingsDocument(document))
            return error.code == JsonFileErrorCode::Busy ? SettingsCommitResult::Busy
                                                         : SettingsCommitResult::ReadFailed;
        const std::optional<nlohmann::json> persisted = RawField(document, key);
        return persisted.has_value() && persisted->is_boolean() && persisted->get<bool>() == value
                   ? SettingsCommitResult::Unchanged
                   : SettingsCommitResult::Conflict;
    }
    catch (...)
    {
        return SettingsCommitResult::ReadFailed;
    }
}
// 检查字段有效草稿是否偏离打开时的有效值。
// 入参：key 为 settings 对象中的动态设置属性名。
// 返回：指定字段草稿偏离有效显示基线时为 true；未注册或未改变为 false。
bool SettingsEditSession::IsDirty(std::string_view key) const noexcept
{
    const auto field = this->fields_.find(key);
    return field != this->fields_.end() && field->second.draft != field->second.baseline;
}
// 核验有效目标同时保留存在性和原始类型冲突检查，不创建缺失字段。
// 入参：key 为已登记字符串字段；value 为期望目标。
// 返回：基线和当前有效值一致为 Unchanged，否则为失败类别。
SettingsCommitResult SettingsEditSession::VerifyCurrentString(std::string_view key,
                                                              std::string_view value) const noexcept
{
    try
    {
        const auto field = this->fields_.find(key);
        if (!this->ready_ || field == this->fields_.end())
            return SettingsCommitResult::InvalidField;
        nlohmann::json user;
        JsonFileError error;
        if (!this->userFile_.Read(user, &error) || !IsSettingsDocument(user))
            return error.code == JsonFileErrorCode::Busy ? SettingsCommitResult::Busy
                                                         : SettingsCommitResult::ReadFailed;
        std::optional<nlohmann::json> effective = RawField(user, key);
        if (!SameRaw(effective, field->second.raw))
            return SettingsCommitResult::Conflict;
        if (!effective || !effective->is_string())
        {
            nlohmann::json defaults;
            if (!this->defaultFile_.Read(defaults, &error) || !IsSettingsDocument(defaults))
                return error.code == JsonFileErrorCode::Busy ? SettingsCommitResult::Busy
                                                             : SettingsCommitResult::ReadFailed;
            effective = RawField(defaults, key);
        }
        return effective && effective->is_string() && effective->get<std::string>() == value
                   ? SettingsCommitResult::Unchanged
                   : SettingsCommitResult::Conflict;
    }
    catch (...)
    {
        return SettingsCommitResult::ReadFailed;
    }
}
// 读取整数草稿，拒绝其他登记类型。
// 入参：key 为字段名。
// 返回：整数值或空值。
std::optional<std::int64_t> SettingsEditSession::ReadInteger(std::string_view key) const noexcept
{
    const auto field = this->fields_.find(key);
    return field == this->fields_.end() || !field->second.integer ? std::nullopt : IntegerValue(field->second.draft);
}

// 将数值更新到已登记整数草稿。
// 入参：key 为字段名；value 为新整数。
// 返回：成功为 true，失败保留原草稿。
bool SettingsEditSession::ChangeInteger(std::string_view key, std::int64_t value) noexcept
{
    const auto field = this->fields_.find(key);
    if (!this->ready_ || field == this->fields_.end() || !field->second.integer)
        return false;
    field->second.draft = value;
    return true;
}

// 区分类型非法默认回退与正常缺字段回退。
// 入参：key 为字段名。
// 返回：原始类型尚待显式修复时为 true。
bool SettingsEditSession::RequiresRepair(std::string_view key) const noexcept
{
    const auto field = this->fields_.find(key);
    return field != this->fields_.end() && field->second.requiresRepair;
}
} // namespace open_st
