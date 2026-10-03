// 翻译列表和网络设置复用已有事务；供应商规则只经窄回调消费。
#include "translation_settings.h"
#include <algorithm>
#include <stdexcept>

namespace open_st
{
namespace
{
// 将渲染绑定失败交给既有窗口异常边界。
// 入参：result 为公共结果。
// 返回：无，失败抛安全诊断。
void RequirePanel(const RendererResult& result)
{
    if (!result)
        throw std::runtime_error("Translation settings binding failed");
}
// 转换用户名称为显示文本，非法编码不输出原始字节。
// 入参：value 为 UTF-8。
// 返回：宽字符显示值或问号。
std::wstring Display(std::string_view value)
{
    if (value.empty())
        return {};
    const int count =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0)
        return L"?";
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(),
                        count);
    return result;
}
} // namespace

// 保留选项稳定值和本地化标签，不向公共窗口模块传递 Settings 类型。
// 入参：options 为设置侧选项列表。
// 返回：用于下拉绑定的公共选项结果。
RendererOptionsResult TranslationRendererOptions(const std::vector<SettingsOption>& options)
{
    RendererOptionsResult result;
    result.options.reserve(options.size());
    for (const SettingsOption& option : options)
        result.options.push_back({option.value, option.label});
    return result;
}

// 检查统一事务中的列表和语言/网络字段，不要求未启用服务拥有凭据。
// 入参：session/callbacks 为候选及领域边界。
// 返回：不含配置值的错误或空。
std::wstring TranslationSettingsError(const SettingsEditSession& session, const SettingsWindowCallbacks& callbacks)
{
    if (!callbacks.translationAvailable)
        return {};
    const auto list = session.ReadJson(TRANSLATION_INTERFACES_KEY);
    if (!list)
        return callbacks.text("settings.translation.invalid");
    const std::wstring error = callbacks.validateTranslationInterfaces(*list);
    if (!error.empty())
        return error;
    nlohmann::json values = {{std::string(TRANSLATION_INTERFACES_KEY), *list}};
    for (const std::string_view key : TRANSLATION_STRING_KEYS)
    {
        const auto value = session.ReadString(key);
        if (!value)
            return callbacks.text("settings.translation.invalid");
        const std::wstring issue = callbacks.validateTranslationField(key, *value);
        if (!issue.empty())
            return issue;
        values[std::string(key)] = *value;
    }
    return callbacks.validateTranslationSettings(values);
}
// 借用父设置事务和 UI，面板只拥有选中身份。
// 入参：renderer/session/callbacks 为长寿命借用；changed/modal 为父窗口窄协调。
// 返回：未绑定实例。
TranslationSettingsPanel::TranslationSettingsPanel(WindowRenderer& renderer, SettingsEditSession& session,
                                                   const SettingsWindowCallbacks& callbacks,
                                                   std::function<void()> changed,
                                                   std::function<void(std::function<void()>)> modal)
    : renderer_(renderer), session_(session), callbacks_(callbacks), changed_(std::move(changed)),
      modal_(std::move(modal))
{
}
// 取得统一草稿数组，不维护并行列表。
// 入参：无。
// 返回：候选副本。
nlohmann::json TranslationSettingsPanel::List() const
{
    return this->session_.ReadJson(TRANSLATION_INTERFACES_KEY).value_or(nlohmann::json{});
}
// 生成只读的名称、类型、启用三列，行序就是父草稿的尝试顺序。
// 入参：无。
// 返回：使用稳定ID的表格行，不包含配置或密钥。
RendererRowsResult TranslationSettingsPanel::Rows()
{
    const nlohmann::json list = this->List();
    RendererRowsResult result;
    if (!list.is_array())
        return result;
    const auto kinds = this->callbacks_.translationChoices("translation.kind");
    for (const nlohmann::json& profile : list)
    {
        if (!profile.is_object() || !profile.contains("id") || !profile["id"].is_string())
            continue;
        const std::string kind = profile.value("kind", "");
        const auto found = std::find_if(kinds.begin(), kinds.end(),
                                        // 查找当前行类型的领域显示标签。
                                        // 入参：option为领域选项。
                                        // 返回：稳定类型相等时true。
                                        [&kind](const SettingsOption& option) { return option.value == kind; });
        const bool enabled = profile.value("enabled", false);
        result.rows.push_back(
            {profile["id"].get<std::string>(),
             {Display(profile.value("name", "?")), found == kinds.end() ? Display(kind) : found->label,
              this->callbacks_.text(enabled ? "settings.translation.on" : "settings.translation.off")}});
    }
    const bool found = std::any_of(result.rows.begin(), result.rows.end(),
                                   // 检查选中身份是否仍存在于重排后的列表。
                                   // 入参：row为当前显示行。
                                   // 返回：对应所选身份时true。
                                   [this](const RendererTableRow& row) { return row.value == this->selected_; });
    if (!found)
        this->selected_ = result.rows.empty() ? "" : result.rows.front().value;
    return result;
}
// 连接列表、静态语言/代理字段和全部按钮。
// 入参：无。
// 返回：无，失败抛出。
void TranslationSettingsPanel::Bind()
{
    // 每次展开重取当前父草稿，不缓存另一份配置。
    // 入参：无。
    // 返回：列表选项。
    RequirePanel(this->renderer_.BindRows("translationInterfaces", [this]() { return this->Rows(); }));
    RequirePanel(this->renderer_.BindString(
        "translationInterfaces",
        // 读取当前选中身份。
        // 入参：无。
        // 返回：稳定ID。
        [this]()
        {
            (void)this->Rows();
            return RendererStringResult{true, this->selected_, {}};
        },
        // 改变选择不算配置改动。
        // 入参：value 为稳定ID。
        // 返回：接受。
        [this](std::string_view value)
        {
            this->selected_ = value;
            this->changed_();
            return RendererChangeResult{};
        }));
    // 双击和Enter只打开现有条目，复用父窗口统一模态忙保护。
    RequirePanel(this->renderer_.BindAction("translationInterfaces",
                                            [this]() { this->modal_([this]() { this->Act("edit"); }); }));
    for (const std::string_view keyView : TRANSLATION_STRING_KEYS)
    {
        const std::string key(keyView);
        if (key != "translation.network.proxy_address")
            // 下拉允许值来自领域，不在 Settings 复制。
            // 入参：无。
            // 返回：选项。
            RequirePanel(this->renderer_.BindOptions(
                key, [this, key]() { return TranslationRendererOptions(this->callbacks_.translationChoices(key)); }));
        RequirePanel(this->renderer_.BindString(
            key,
            // 读取统一字符串草稿。
            // 入参：无。
            // 返回：值及存在性。
            [this, key]()
            {
                const auto value = this->session_.ReadString(key);
                return RendererStringResult{value.has_value(), value.value_or(""), {}};
            },
            // 原始输入保留供修改，校验不偷偷替换设置。
            // 入参：value 为用户输入。
            // 返回：接受输入及字段错误。
            [this, key](std::string_view value)
            {
                if (!this->session_.ChangeString(key, value))
                    return RendererChangeResult{false, this->callbacks_.text("settings.operation_failed")};
                this->changed_();
                return RendererChangeResult{true, this->callbacks_.validateTranslationField(key, value)};
            }));
    }
    for (const std::string operation : {"add", "edit", "delete", "up", "down"})
        // 所有命令只编辑父事务，子窗口通过父级既有模态忙保护运行。
        // 入参：无。
        // 返回：无。
        RequirePanel(this->renderer_.BindAction("translation." + operation, [this, operation]()
                                                { this->modal_([this, operation]() { this->Act(operation); }); }));
}
// 验证完整候选并一次替换草稿，失败不泄露配置。
// 入参：value 为候选数组。
// 返回：接受为 true。
bool TranslationSettingsPanel::Store(const nlohmann::json& value)
{
    const std::wstring error = this->callbacks_.validateTranslationInterfaces(value);
    if (!error.empty())
    {
        (void)this->renderer_.SetStatus(error);
        return false;
    }
    if (!this->session_.ChangeJson(TRANSLATION_INTERFACES_KEY, value))
        return false;
    this->Refresh();
    this->changed_();
    return true;
}
// 在副本上执行操作，确认前绝不改变父数组；操作目标使用稳定ID。
// 入参：operation 为绑定动作。
// 返回：无。
void TranslationSettingsPanel::Act(std::string_view operation)
{
    nlohmann::json list = this->List();
    if (!list.is_array())
        return;
    const auto found = std::find_if(list.begin(), list.end(),
                                    // 身份定位只读取固定字段。
                                    // 入参：item 为数组对象。
                                    // 返回：对应当前选择为 true。
                                    [this](const nlohmann::json& item)
                                    { return item.is_object() && item.value("id", "") == this->selected_; });
    std::optional<std::string> proxyMode, proxyAddress;
    if (operation == "add" || operation == "edit")
    {
        proxyMode = this->session_.ReadString("translation.network.proxy_mode");
        proxyAddress = this->session_.ReadString("translation.network.proxy_address");
        if (!proxyMode || !proxyAddress)
        {
            (void)this->renderer_.SetFieldError("translationInterfaces",
                                                this->callbacks_.text("translation.error.configuration"));
            return;
        }
    }
    if (operation == "add")
    {
        const auto kinds = this->callbacks_.translationChoices("translation.kind");
        if (kinds.empty())
            return;
        nlohmann::json profile = this->callbacks_.createTranslationProfile(kinds.front().value);
        if (EditTranslationProfile(this->renderer_.NativeHandle(), this->callbacks_.largeIcon, profile,
                                   this->callbacks_, true, *proxyMode, *proxyAddress))
        {
            list.push_back(profile);
            if (this->Store(list))
            {
                this->selected_ = profile.at("id").get<std::string>();
                this->Refresh();
            }
        }
        return;
    }
    if (found == list.end())
        return;
    if (operation == "edit")
    {
        nlohmann::json candidate = *found;
        const std::string identity = candidate.at("id").get<std::string>();
        if (!EditTranslationProfile(this->renderer_.NativeHandle(), this->callbacks_.largeIcon, candidate,
                                    this->callbacks_, false, *proxyMode, *proxyAddress))
            return;
        // 父表单模态期间冻结，仍按 ID 重新定位，不把旧索引用于替换。
        nlohmann::json current = this->List();
        for (nlohmann::json& item : current)
            if (item.is_object() && item.value("id", "") == identity)
            {
                item = std::move(candidate);
                (void)this->Store(current);
                return;
            }
        return;
    }
    if (operation == "delete")
        list.erase(found);
    else if (operation == "up" && found != list.begin())
        std::iter_swap(found, found - 1);
    else if (operation == "down" && found + 1 != list.end())
        std::iter_swap(found, found + 1);
    (void)this->Store(list);
}
// 刷新列表与字段错误，语言/代理编辑内容不被覆盖。
// 入参：无。
// 返回：无。
void TranslationSettingsPanel::Refresh()
{
    (void)this->Rows();
    (void)this->renderer_.RefreshValue("translationInterfaces");
    const std::wstring error = TranslationSettingsError(this->session_, this->callbacks_);
    (void)this->renderer_.SetFieldError("translationInterfaces",
                                        !error.empty() ? error
                                        : this->session_.RequiresRepair(TRANSLATION_INTERFACES_KEY)
                                            ? this->callbacks_.text("settings.storage.repair_pending")
                                            : L"");
    for (const std::string_view key : TRANSLATION_STRING_KEYS)
    {
        const auto value = this->session_.ReadString(key);
        std::wstring fieldError = value ? this->callbacks_.validateTranslationField(key, *value)
                                        : this->callbacks_.text("settings.translation.invalid");
        if (fieldError.empty() && this->session_.RequiresRepair(key))
            fieldError = this->callbacks_.text("settings.storage.repair_pending");
        (void)this->renderer_.SetFieldError(key, fieldError);
    }
    if (!error.empty())
        (void)this->renderer_.SetStatus(error);
}
// 配置不可编辑时禁用列表操作；空列表仍允许新增。
// 入参：ready 为父事务可用性。
// 返回：无。
void TranslationSettingsPanel::Enable(bool ready)
{
    for (const std::string_view key : TRANSLATION_STRING_KEYS)
        (void)this->renderer_.SetEnabled(key, ready);
    (void)this->renderer_.SetEnabled("translationInterfaces", ready);
    for (const std::string operation : {"add", "edit", "delete", "up", "down"})
        (void)this->renderer_.SetEnabled("translation." + operation,
                                         ready && (operation == "add" || !this->selected_.empty()));
}
} // namespace open_st
