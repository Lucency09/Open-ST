// 把对象键值草稿绑定到公共表格；秘密值只进入密码控件，列表始终提供遮罩。
#include "translation_key_value_editor.h"
#include <algorithm>
#include <stdexcept>
namespace open_st
{
namespace
{
// 收敛公共绑定失败。
// 入参：结果。
// 返回：无，失败抛无正文异常。
void RequireKv(const RendererResult& result)
{
    if (!result)
        throw std::runtime_error("Translation key-value form failed");
}
// 转换显示文本，不解释正文。
// 入参：UTF8。
// 返回：宽文本。
std::wstring KvText(std::string_view value)
{
    if (value.empty())
        return {};
    const int size =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0)
        return L"?";
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(),
                        size);
    return result;
}
} // namespace
// 建立借用。
// 入参：renderer/callbacks为表单借用，entries为对象草稿，id/title标识控件，keys/values为枚举键，secret/emptyKey控制显示。
// 返回：未绑定编辑器。
TranslationKeyValueEditor::TranslationKeyValueEditor(WindowRenderer& renderer, const SettingsWindowCallbacks& callbacks,
                                                     nlohmann::json& entries, std::string id, std::string title,
                                                     std::string keys, std::string values, bool secret, bool emptyKey)
    : renderer_(renderer), callbacks_(callbacks), entries_(entries), id_(std::move(id)), title_(std::move(title)),
      keys_(std::move(keys)), values_(std::move(values)), secret_(secret), emptyKey_(emptyKey)
{
}
// 使用公共列布局和原生只读table。
// 入参：无。
// 返回：布局。
nlohmann::json TranslationKeyValueEditor::Layout() const
{
    nlohmann::json actions = nlohmann::json::array();
    for (const char* action : {"add", "edit", "delete"})
        actions.push_back({{"type", "button"},
                           {"id", this->id_ + "." + action},
                           {"textKey", "settings.translation." + std::string(action)}});
    return {
        {"type", "row"},
        {"id", this->id_ + ".group"},
        {"gap", 10},
        {"children",
         nlohmann::json::array(
             {{{"type", "table"},
               {"id", this->id_},
               {"labelKey", this->title_},
               {"width", "fill"},
               {"visibleRows", 4},
               {"columns", nlohmann::json::array({{{"textKey", "settings.translation.kv.key"}, {"width", 180}},
                                                  {{"textKey", "settings.translation.kv.value"}, {"width", "fill"}}})}},
              {{"type", "column"},
               {"id", this->id_ + ".actions"},
               {"width", 112},
               {"gap", 6},
               {"children", actions}}})}};
}
// 避免把空键与无选中混淆，稳定身份统一加前缀。
// 入参：无。
// 返回：安全显示行。
RendererRowsResult TranslationKeyValueEditor::Rows()
{
    RendererRowsResult result;
    for (const auto& entry : this->entries_.items())
    {
        const std::string value = entry.value().get<std::string>();
        result.rows.push_back(
            {"key:" + entry.key(),
             {KvText(entry.key()),
              this->secret_ ? (value.empty() ? this->callbacks_.text("settings.translation.secret_empty") : L"••••••••")
                            : KvText(value)}});
    }
    if (std::none_of(result.rows.begin(), result.rows.end(),
                     // 按稳定值检查选择是否仍在当前键值表中。
                     // 入参：row为安全显示行。
                     // 返回：对应当前选择时true。
                     [this](const RendererTableRow& row) { return row.value == this->selected_; }))
        this->selected_ = result.rows.empty() ? "" : result.rows.front().value;
    return result;
}
// 绑定表选择与模态操作。
// 入参：父草稿重校验。
// 返回：无。
void TranslationKeyValueEditor::Bind(std::function<void()> changed)
{
    this->changed_ = std::move(changed);
    RequireKv(this->renderer_.BindRows(this->id_, // 刷新安全显示行，秘密值在交给公共控件前已遮罩。
                                                  // 入参：无。
                                                  // 返回：当前对象草稿的稳定行快照。
                                       [this]() { return this->Rows(); }));
    RequireKv(this->renderer_.BindString(
        this->id_,
        // 查询仍有效的当前选择。
        // 入参：无。
        // 返回：稳定行身份，空字符串表示无选中。
        [this]()
        {
            (void)this->Rows();
            return RendererStringResult{true, this->selected_, {}};
        },
        // 只更新键值表选择，不修改对象内容。
        // 入参：value为公共表格选中的稳定键身份。
        // 返回：接受选择的公共结果。
        [this](std::string_view value)
        {
            this->selected_ = value;
            return RendererChangeResult{};
        }));
    RequireKv(this->renderer_.BindAction(this->id_, // 双击或Enter通过同一模态入口编辑所选键值。
                                                    // 入参：无。
                                                    // 返回：无，取消不改变对象草稿。
                                         [this]() { this->Act("edit"); }));
    for (const std::string action : {"add", "edit", "delete"})
        RequireKv(this->renderer_.BindAction(this->id_ + "." + action, // 执行当前表格的新增、编辑或删除命令。
                                                                       // 入参：无，action为已绑定动作名。
                                                                       // 返回：无，只影响本次子表单草稿。
                                             [this, action]() { this->Act(action); }));
}
// 只在确认后一次替换键值，重名不得静默覆盖另一行。
// 入参为操作。
// 返回无，取消保留整表。
void TranslationKeyValueEditor::Act(std::string_view action)
{
    const bool adding = action == "add";
    if (!adding && this->selected_.empty())
        return;
    const std::string original = adding ? "" : this->selected_.substr(4);
    if (!adding && !this->entries_.contains(original))
        return;
    if (action == "delete")
        this->entries_.erase(original);
    else
    {
        std::string key = original, value = adding ? "" : this->entries_.at(original).get<std::string>();
        if (!this->Edit(key, value, adding))
            return;
        nlohmann::json candidate = this->entries_;
        if (!adding)
            candidate.erase(original);
        candidate[key] = std::move(value);
        this->entries_ = std::move(candidate);
        this->selected_ = "key:" + key;
    }
    (void)this->Rows();
    (void)this->renderer_.RefreshValue(this->id_);
    if (this->changed_)
        this->changed_();
}
// 普通二字段模态表单。
// 入参：候选。
// 返回：确认时才输出，秘密永不进入状态文案。
bool TranslationKeyValueEditor::Edit(std::string& key, std::string& value, bool adding)
{
    const std::string original = key;
    std::string draftKey = key, draftValue = value;
    const auto keyOptions =
        this->keys_.empty() ? std::vector<SettingsOption>{} : this->callbacks_.translationChoices(this->keys_);
    const auto valueOptions =
        this->values_.empty() ? std::vector<SettingsOption>{} : this->callbacks_.translationChoices(this->values_);
    if (adding && !keyOptions.empty())
        draftKey = keyOptions.front().value;
    if (adding && !valueOptions.empty())
        draftValue = valueOptions.front().value;
    WindowRenderer child;
    nlohmann::json keyNode{
        {"type", keyOptions.empty() ? "edit" : "select"}, {"id", "kvKey"}, {"labelKey", "settings.translation.kv.key"}};
    if (keyOptions.empty())
    {
        // UI只给有界编辑空间，最终键/字符预算由领域校验；错误码允许原协议的空串与换行。
        keyNode["maxLength"] = 65536;
        if (this->emptyKey_)
        {
            keyNode["multiline"] = true;
            keyNode["visibleLines"] = 3;
        }
    }
    nlohmann::json valueNode{{"type", valueOptions.empty() ? "edit" : "select"},
                             {"id", "kvValue"},
                             {"labelKey", "settings.translation.kv.value"}};
    if (valueOptions.empty())
    {
        valueNode["maxLength"] = 65536;
        valueNode["password"] = this->secret_;
        valueNode["multiline"] = !this->secret_;
        if (!this->secret_)
            valueNode["visibleLines"] = 4;
    }
    RequireKv(child.LoadLayout(
        {{"schemaVersion", 1},
         {"window",
          {{"titleKey", "settings.translation.kv.edit"}, {"initialSize", {520, 300}}, {"minSize", {380, 240}}}},
         {"content",
          {{"type", "column"},
           {"id", "kvBody"},
           {"padding", 16},
           {"gap", 10},
           {"children", nlohmann::json::array({keyNode, valueNode})}}},
         {"footer",
          {{"trailing",
            nlohmann::json::array({{{"type", "button"}, {"id", "accept"}, {"textKey", "dialog.ok"}},
                                   {{"type", "button"}, {"id", "cancel"}, {"textKey", "dialog.cancel"}}})}}}}));
    RequireKv(child.SetTextResolver(this->callbacks_.text));
    // 在键值子表单中阻止空的必需键或覆盖另一现有键。
    // 入参：无，读取当前独立键和值草稿。
    // 返回：允许确认时true，同时更新安全状态和确认按钮。
    const auto validate = [&]()
    {
        std::wstring error;
        if (!this->emptyKey_ && draftKey.empty())
            error = this->callbacks_.text("settings.translation.kv.invalid_key");
        else if ((adding || draftKey != original) && this->entries_.contains(draftKey))
            error = this->callbacks_.text("settings.translation.kv.duplicate_key");
        (void)child.SetStatus(error);
        (void)child.SetEnabled("accept", error.empty());
        return error.empty();
    };
    RequireKv(child.BindString(
        "kvKey", // 读取当前键名草稿。
        // 入参：无。
        // 返回：字符串控件值，不改变父对象。
        [&]() { return RendererStringResult{true, draftKey, {}}; },
        // 保存当前输入并重新检查键名冲突。
        // 入参：input为键名或值控件的UTF-8草稿。
        // 返回：接受编辑，是否能确认由局部校验控制。
        [&](std::string_view input)
        {
            draftKey = input;
            (void)validate();
            return RendererChangeResult{};
        }));
    RequireKv(child.BindString(
        "kvValue", // 读取当前值，秘密值仅交给原生密码输入框。
        // 入参：无。
        // 返回：字符串控件值，不加入状态或列表文字。
        [&]() { return RendererStringResult{true, draftValue, {}}; },
        // 保存当前输入并重新检查键名冲突。
        // 入参：input为键名或值控件的UTF-8草稿。
        // 返回：接受编辑，是否能确认由局部校验控制。
        [&](std::string_view input)
        {
            draftValue = input;
            (void)validate();
            return RendererChangeResult{};
        }));
    if (!keyOptions.empty())
        RequireKv(child.BindOptions("kvKey", // 提供领域限定的键选项。
                                             // 入参：无。
                                             // 返回：当前语言映射键的显示选项。
                                    [&]() { return TranslationRendererOptions(keyOptions); }));
    if (!valueOptions.empty())
        RequireKv(child.BindOptions("kvValue", // 提供领域限定的值选项。
                                               // 入参：无。
                                               // 返回：业务错误类别等固定值的显示选项。
                                    [&]() { return TranslationRendererOptions(valueOptions); }));
    bool accepted = false;
    RequireKv(child.BindAction("accept",
                               // 确认前复查键冲突，成功后才标记交付。
                               // 入参：无。
                               // 返回：无，失败保留当前输入窗口。
                               [&]()
                               {
                                   if (validate())
                                   {
                                       accepted = true;
                                       (void)child.RequestClose();
                                   }
                               }));
    // 取消本次键值编辑，不交付任何草稿。
    // 入参：无。
    // 返回：无，要求公共模态窗口关闭。
    const auto cancel = [&]()
    {
        accepted = false;
        (void)child.RequestClose();
    };
    RequireKv(child.BindAction("cancel", cancel));
    RequireKv(child.SetCloseHandler(cancel));
    RequireKv(child.SetDefaultAction("accept"));
    (void)validate();
    if (!child.ShowModal({this->renderer_.NativeHandle(), this->callbacks_.largeIcon},
                         this->callbacks_.processThreadMessage) ||
        !accepted)
        return false;
    key = std::move(draftKey);
    value = std::move(draftValue);
    return true;
}
} // namespace open_st
