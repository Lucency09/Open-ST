// 新增/编辑共用普通模态表单；所有类型草稿属于本次窗口，确认后才替换父条目。
#include "translation_custom_editor.h"
#include "translation_settings.h"
#include <map>
#include <stdexcept>
namespace open_st
{
namespace
{
// 将公共窗口失败交给既有宿主边界。
// 入参：结果。
// 返回：无，失败抛安全异常。
void RequireProfile(const RendererResult& result)
{
    if (!result)
        throw std::runtime_error("Translation profile form failed");
}
// 由领域字段生成通用控件，不在Settings复制供应商规则。
// 入参：kind用于隔离各类型控件，field为领域元数据。
// 返回布局节点。
nlohmann::json ProfileField(std::string_view kind, const SettingsTranslationField& field)
{
    nlohmann::json node{{"type", field.options.empty() ? "edit" : "select"},
                        {"id", "field." + std::string(kind) + "." + field.key},
                        {"labelKey", "settings.translation.field." + field.key},
                        {"width", "fill"}};
    if (field.options.empty())
    {
        node["maxLength"] = field.multiline ? 65536 : 4096;
        node["password"] = field.secret;
        node["multiline"] = field.multiline;
        if (field.multiline)
            node["visibleLines"] = 5;
    }
    return node;
}
} // namespace
// 新增允许切换类型，编辑固定类型；所有切换仅改变可见组，保留尚未提交的输入。
// 入参：owner/icon为宿主；profile仅确认后输出；callbacks为领域规则；creating表示新增。
// 返回：明确确认且完整校验通过为true，取消与失败均不改profile。
bool EditTranslationProfile(HWND owner, HICON icon, nlohmann::json& profile, const SettingsWindowCallbacks& callbacks,
                            bool creating, std::string proxyMode, std::string proxyAddress)
{
    using Json = nlohmann::json;
    const std::string identity = profile.at("id").get<std::string>();
    const std::string originalKind = profile.at("kind").get<std::string>();
    std::string kind = originalKind, name = profile.at("name").get<std::string>();
    bool enabled = profile.at("enabled").get<bool>();
    bool accepted = false;
    bool nameEdited = false;
    std::map<std::string, Json> drafts;
    std::map<std::string, std::vector<SettingsTranslationField>> descriptions;
    std::vector<SettingsOption> kinds =
        callbacks.translationChoices ? callbacks.translationChoices("translation.kind") : std::vector<SettingsOption>{};
    if (kinds.empty())
        kinds.push_back({kind, std::wstring(kind.begin(), kind.end())});
    drafts[kind] = profile;
    if (creating)
        for (const SettingsOption& option : kinds)
            if (option.value != kind)
                drafts[option.value] = callbacks.createTranslationProfile(option.value);
    WindowRenderer renderer;
    std::unique_ptr<CustomTranslationEditor> custom;
    Json children =
        Json::array({{{"type", "edit"},
                      {"id", "profileName"},
                      {"labelKey", "settings.translation.profile_name"},
                      {"maxLength", 128}},
                     {{"type", "select"}, {"id", "profileKind"}, {"labelKey", "settings.translation.profile_type"}},
                     {{"type", "checkbox"}, {"id", "profileEnabled"}, {"labelKey", "settings.translation.enabled"}},
                     {{"type", "text"}, {"id", "profileHint"}, {"textKey", "settings.translation.profile_hint"}}});
    for (const auto& entry : drafts)
    {
        Json fields = Json::array();
        if (entry.first == "custom_http")
        {
            custom = std::make_unique<CustomTranslationEditor>(renderer, callbacks, entry.second);
            fields.push_back(custom->Layout());
        }
        else
        {
            descriptions[entry.first] = callbacks.translationProfileFields(entry.first);
            for (const auto& field : descriptions.at(entry.first))
                fields.push_back(ProfileField(entry.first, field));
        }
        children.push_back({{"type", "column"}, {"id", "kindGroup." + entry.first}, {"gap", 10}, {"children", fields}});
    }
    children.push_back({{"type", "text"}, {"id", "profileStatus"}, {"textKey", "settings.translation.profile_status"}});
    children.push_back({{"type", "text"}, {"id", "probeSample"}, {"textKey", "translation.test.sample"}});
    children.push_back({{"type", "button"}, {"id", "testConnection"}, {"textKey", "translation.test.action"}});
    children.push_back({{"type", "edit"},
                        {"id", "testResult"},
                        {"labelKey", "translation.test.output"},
                        {"multiline", true},
                        {"readOnly", true},
                        {"visibleLines", 8},
                        {"maxLength", 65536},
                        {"width", "fill"}});
    RequireProfile(renderer.LoadLayout(
        {{"schemaVersion", 1},
         {"window",
          {{"titleKey", creating ? "settings.translation.add" : "settings.translation.edit"},
           {"initialSize", {780, 720}},
           {"minSize", {560, 420}},
           {"resizable", true}}},
         {"content", {{"type", "column"}, {"id", "profileBody"}, {"padding", 16}, {"gap", 10}, {"children", children}}},
         {"footer",
          {{"trailing", Json::array({{{"type", "button"}, {"id", "accept"}, {"textKey", "dialog.ok"}},
                                     {{"type", "button"}, {"id", "cancel"}, {"textKey", "dialog.cancel"}}})}}}}));
    SettingsTranslationTestStatus probe;
    Json testedCandidate;
    // 异常和窗口退出统一失效自身请求，不触碰其他翻译任务。
    struct ProbeGuard final
    {
        const SettingsWindowCallbacks& callbacks;
        SettingsTranslationTestStatus& status;
        ~ProbeGuard()
        {
            if (status.requestId && callbacks.cancelTranslationTest)
                try
                {
                    callbacks.cancelTranslationTest(status.requestId);
                }
                catch (...)
                {
                }
        }
    } guard{callbacks, probe};
    Json candidate;
    // 每次校验从当前类型草稿装配，标识/名称/启用只由顶部唯一状态拥有。
    // 入参：output为候选输出。
    // 返回活动JSON是否可解析。
    const auto assemble = [&](Json& output)
    {
        output = drafts.at(kind);
        output["id"] = identity;
        output["kind"] = kind;
        output["name"] = name;
        output["enabled"] = enabled;
        return kind != "custom_http" || (custom && custom->Apply(output));
    };
    RequireProfile(renderer.SetTextResolver(
        // 动态显示当前类型安全状态，其余文案交给宿主本地化。
        // 入参：key为公共表单请求的文字键。
        // 返回：不含秘密值的显示文本。
        [&](std::string_view key)
        {
            if (key == "translation.test.action")
                return callbacks.text(probe.running ? "translation.test.cancel" : "translation.test.start");
            if (key == "settings.translation.profile_status")
            {
                Json current;
                (void)assemble(current);
                return callbacks.translationProfileStatus ? callbacks.translationProfileStatus(current)
                                                          : std::wstring{};
            }
            return callbacks.text(key);
        }));
    // 按模式更新显示和提交准入，不重设正文控件或原生撤销历史。
    // 入参：无。
    // 返回安全错误说明。
    const auto validate = [&]()
    {
        for (const auto& entry : drafts)
            (void)renderer.SetVisible("kindGroup." + entry.first, entry.first == kind);
        if (custom)
            custom->RefreshVisibility();
        const std::wstring error = assemble(candidate) ? callbacks.validateTranslationProfile(candidate)
                                                       : callbacks.text("settings.translation.invalid_json");
        (void)renderer.SetStatus(error);
        (void)renderer.SetEnabled("accept", error.empty());
        (void)renderer.SetEnabled(
            "testConnection", probe.running || (error.empty() && callbacks.submitTranslationTest &&
                                                callbacks.translationTestStatus && callbacks.cancelTranslationTest));
        (void)renderer.RefreshValue("testResult");
        (void)renderer.RefreshTexts();
        return error;
    };
    RequireProfile(renderer.BindString(
        "testResult",
        // 结果由宿主脱敏后交付；草稿变动时保留旧诊断并明确标识。
        // 入参：无。返回：可选择复制的 UTF-8 只读文字。
        [&]()
        {
            std::wstring text = probe.text;
            Json current;
            if (!testedCandidate.is_null() && (!assemble(current) || current != testedCandidate))
                text = callbacks.text("translation.test.previous") + L"\r\n" + text;
            if (text.empty())
                return RendererStringResult{true, {}, {}};
            const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                                   static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
            if (length <= 0)
                return RendererStringResult{false, {}, {}};
            std::string value(static_cast<std::size_t>(length), '\0');
            WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), value.data(),
                                length, nullptr, nullptr);
            return RendererStringResult{true, std::move(value), {}};
        }));
    RequireProfile(renderer.BindAction("testConnection",
                                       // 测试当前草稿，未启用的条目也可诊断；取消只作用于本窗口请求。
                                       // 入参：无。返回：无，诊断结果不写入设置。
                                       [&]()
                                       {
                                           if (probe.running)
                                           {
                                               callbacks.cancelTranslationTest(probe.requestId);
                                               probe.running = false;
                                               probe.text = callbacks.text("translation.test.cancelled");
                                           }
                                           else if (validate().empty() && callbacks.submitTranslationTest)
                                           {
                                               if (probe.requestId && callbacks.cancelTranslationTest)
                                                   callbacks.cancelTranslationTest(probe.requestId);
                                               testedCandidate = candidate;
                                               probe =
                                                   callbacks.submitTranslationTest(candidate, proxyMode, proxyAddress);
                                           }
                                           (void)validate();
                                       }));
    RequireProfile(renderer.BindString(
        "profileName", // 查询顶部唯一名称草稿。
        // 入参：无。
        // 返回：当前名称的公共字符串结果。
        [&]() { return RendererStringResult{true, name, {}}; },
        // 保存顶部文本或类型选择并重校验，类型切换保护用户自定名称。
        // 入参：value为当前控件候选字符串。
        // 返回：接受或拒绝结果及安全校验说明。
        [&](std::string_view value)
        {
            name = value;
            nameEdited = true;
            return RendererChangeResult{true, validate()};
        }));
    RequireProfile(renderer.BindBool(
        "profileEnabled", // 查询顶部唯一启用意图。
        // 入参：无。
        // 返回：当前布尔草稿，不执行翻译。
        [&]() { return RendererBoolResult{true, enabled, {}}; },
        // 仅修改当前条目的启用草稿。
        // 入参：value为复选框候选值。
        // 返回：当前完整候选的校验结果。
        [&](bool value)
        {
            enabled = value;
            return RendererChangeResult{true, validate()};
        }));
    RequireProfile(renderer.BindOptions("profileKind", // 显示领域发布的全部接口类型。
                                                       // 入参：无。
                                                       // 返回：公共类型下拉选项。
                                        [&]() { return TranslationRendererOptions(kinds); }));
    RequireProfile(renderer.BindString(
        "profileKind", // 查询当前正在编辑的接口类型。
        // 入参：无。
        // 返回：稳定kind值，编辑既有条目时控件禁用。
        [&]() { return RendererStringResult{true, kind, {}}; },
        // 保存顶部文本或类型选择并重校验，类型切换保护用户自定名称。
        // 入参：value为当前控件候选字符串。
        // 返回：接受或拒绝结果及安全校验说明。
        [&](std::string_view value)
        {
            if (!creating || !drafts.contains(std::string(value)))
                return RendererChangeResult{false, {}};
            kind = value;
            if (!nameEdited)
            {
                name = drafts.at(kind).at("name").get<std::string>();
                (void)renderer.RefreshValue("profileName");
            }
            return RendererChangeResult{true, validate()};
        }));
    for (const auto& entry : descriptions)
        for (const SettingsTranslationField& field : entry.second)
        {
            const std::string fieldKind = entry.first, id = "field." + fieldKind + "." + field.key;
            RequireProfile(renderer.BindString(
                id,
                // 读取指定类型的配置或秘密草稿，不读取设置文件。
                // 入参：无，捕获的字段描述决定所属对象。
                // 返回：该控件的字符串值；秘密只进入密码控件。
                [&, fieldKind, field]()
                {
                    return RendererStringResult{
                        true,
                        drafts.at(fieldKind)[field.secret ? "secrets" : "configuration"].value(field.key, ""),
                        {}};
                },
                // 保存普通提供方字段的中间编辑。
                // 入参：value为该字段UTF-8候选。
                // 返回：接受编辑及当前完整条目校验说明。
                [&, fieldKind, field](std::string_view value)
                {
                    drafts.at(fieldKind)[field.secret ? "secrets" : "configuration"][field.key] = value;
                    return RendererChangeResult{true, validate()};
                }));
            if (!field.options.empty())
                RequireProfile(renderer.BindOptions(id, // 转交领域字段选项。
                                                        // 入参：无。
                                                        // 返回：当前字段的公共选项结果。
                                                    [field]() { return TranslationRendererOptions(field.options); }));
        }
    if (custom)
        custom->Bind( // 自定义子编辑器发生变化后统一更新可见组和提交准入。
                      // 入参：无。
                      // 返回：无，不重建输入控件或原生撤销栈。
            [&]() { (void)validate(); });
    RequireProfile(renderer.BindAction("accept",
                                       // 确认时重新组装和校验完整候选，随后才允许返回父事务。
                                       // 入参：无。
                                       // 返回：无，非法中间输入继续留在窗口。
                                       [&]()
                                       {
                                           if (validate().empty())
                                           {
                                               accepted = true;
                                               (void)renderer.RequestClose();
                                           }
                                       }));
    // 取消新增或编辑，不覆盖父条目。
    // 入参：无。
    // 返回：无，结束本次公共模态表单。
    const auto cancel = [&]()
    {
        accepted = false;
        (void)renderer.RequestClose();
    };
    RequireProfile(renderer.BindAction("cancel", cancel));
    RequireProfile(renderer.SetCloseHandler(cancel));
    RequireProfile(renderer.SetDefaultAction("accept"));
    (void)renderer.SetEnabled("profileKind", creating);
    (void)validate();
    // 宿主处理截图及任务唤醒后刷新自身快照，晚到结果必须匹配请求身份。
    // 入参：message 为公共模态循环尚未处理的消息。返回：宿主是否已消费。
    const auto dispatch = [&](MSG& message)
    {
        const bool handled = callbacks.processThreadMessage && callbacks.processThreadMessage(message);
        if (probe.running && callbacks.translationTestStatus)
        {
            SettingsTranslationTestStatus current = callbacks.translationTestStatus(probe.requestId);
            if (current.requestId == probe.requestId &&
                (current.running != probe.running || current.text != probe.text))
            {
                probe = std::move(current);
                (void)validate();
            }
        }
        return handled;
    };
    if (!renderer.ShowModal({owner, icon}, dispatch) || !accepted)
        return false;
    profile = std::move(candidate);
    return true;
}
} // namespace open_st
