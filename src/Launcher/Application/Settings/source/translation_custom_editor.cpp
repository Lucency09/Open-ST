// 以通用可见性隐藏非活动模式，草稿仍由当前子表单保留。
#include "translation_custom_editor.h"
#include <stdexcept>
namespace open_st
{
namespace
{
// 公共布局组。
// 入参：ID及节点数组。
// 返回：纵向组。
nlohmann::json Group(std::string id, nlohmann::json children)
{
    return {{"type", "column"}, {"id", std::move(id)}, {"gap", 10}, {"children", std::move(children)}};
}
// 组标题只消费本地化键。
// 入参：分组名。
// 返回：静态文字。
nlohmann::json Heading(std::string_view name)
{
    return {{"type", "text"},
            {"id", "custom.heading." + std::string(name)},
            {"textKey", "settings.translation.custom." + std::string(name)}};
}
// 失败向既有边界传播，不包含输入正文。
// 入参：结果。
// 返回：无。
void RequireCustom(const RendererResult& result)
{
    if (!result)
        throw std::runtime_error("Custom translation form failed");
}
} // namespace
// 建立唯一模式草稿。
// 入参：renderer/callbacks为本次子窗借用，profile为原条目。
// 返回：未绑定对象。
CustomTranslationEditor::CustomTranslationEditor(WindowRenderer& renderer, const SettingsWindowCallbacks& callbacks,
                                                 const nlohmann::json& profile)
    : renderer_(renderer), callbacks_(callbacks), draft_(profile)
{
}
// 记录字段绑定，控件只用公共edit/select。
// 入参：name为控件名，value为唯一草稿，choices为领域选项键，lines/maximum为显示预算。
// 返回：节点。
nlohmann::json CustomTranslationEditor::String(std::string name, std::string& value, std::string choices, int lines,
                                               int maximum)
{
    const std::string id = "custom." + name;
    nlohmann::json result{{"type", choices.empty() ? "edit" : "select"},
                          {"id", id},
                          {"labelKey", "settings.translation.custom." + name},
                          {"width", "fill"}};
    if (choices.empty())
    {
        result["maxLength"] = maximum;
        if (lines > 1)
        {
            result["multiline"] = true;
            result["visibleLines"] = lines;
            result["verticalScroll"] = true;
        }
    }
    this->strings_.push_back({id, std::move(choices), &value});
    return result;
}
// 登记checkbox借用。
// 入参：name为控件名，value为本次草稿布尔值。
// 返回：节点。
nlohmann::json CustomTranslationEditor::Boolean(std::string name, bool& value)
{
    const std::string id = "custom." + name;
    this->bools_.push_back({id, &value});
    return {{"type", "checkbox"}, {"id", id}, {"labelKey", "settings.translation.custom." + name}};
}
// 所有键值表共用同一编辑器。
// 入参：name/value标识对象草稿，keys/values为领域枚举，secret/emptyKey控制呈现。
// 返回：节点。
nlohmann::json CustomTranslationEditor::Table(std::string name, nlohmann::json& value, std::string keys,
                                              std::string values, bool secret, bool emptyKey)
{
    auto editor = std::make_unique<TranslationKeyValueEditor>(this->renderer_, this->callbacks_, value,
                                                              "custom." + name, "settings.translation.custom." + name,
                                                              std::move(keys), std::move(values), secret, emptyKey);
    nlohmann::json node = editor->Layout();
    this->tables_.push_back(std::move(editor));
    return node;
}
// 全部组在一个滚动内容区，不建立标签页或额外消息循环。
// 入参：无。
// 返回：布局。
nlohmann::json CustomTranslationEditor::Layout()
{
    using Json = nlohmann::json;
    Json basics =
        Json::array({Heading("basic"), this->String("method", this->draft_.method, "custom.method"),
                     this->String("url", this->draft_.url, {}, 1, 2048),
                     this->Table("source_languages", this->draft_.sourceLanguages, "translation.source_language"),
                     this->Table("target_languages", this->draft_.targetLanguages, "translation.target_language")});
    Json request = Json::array(
        {Heading("request"),
         this->Table("headers", this->draft_.headers),
         this->Table("query", this->draft_.query),
         {{"type", "text"}, {"id", "custom.templateHint"}, {"textKey", "settings.translation.custom.template_help"}},
         this->String("body_mode", this->draft_.bodyMode, "custom.body_mode"),
         Group("custom.jsonBodyGroup", Json::array({this->String("json_body", this->draft_.jsonBody, {}, 8, 65536)})),
         this->Table("form_body", this->draft_.formBody),
         Group("custom.rawBodyGroup", Json::array({this->String("raw_body", this->draft_.rawBody, {}, 5, 65536)}))});
    Json success =
        Json::array({this->String("success_pointer", this->draft_.successPointer, {}, 1, 256),
                     this->String("scalar_type", this->draft_.scalarType, "custom.scalar_type"),
                     Group("custom.successTextGroup",
                           Json::array({this->String("success_value", this->draft_.successText, {}, 3, 65536)})),
                     Group("custom.successNumberGroup",
                           Json::array({this->String("success_number", this->draft_.successNumber, {}, 1, 128)})),
                     Group("custom.successBooleanGroup",
                           Json::array({this->Boolean("success_boolean", this->draft_.successBoolean)}))});
    Json mapping = Json::array(
        {this->String("extraction", this->draft_.extraction, "custom.extraction"),
         Group("custom.singleGroup", Json::array({this->String("text_pointer", this->draft_.textPointer, {}, 1, 256)})),
         Group("custom.arrayGroup", Json::array({this->String("array_pointer", this->draft_.arrayPointer, {}, 1, 256),
                                                 this->String("item_pointer", this->draft_.itemPointer, {}, 1, 256),
                                                 this->String("separator", this->draft_.separator, {}, 3, 32)})),
         this->Boolean("use_success", this->draft_.useSuccess), Group("custom.successGroup", std::move(success)),
         this->Boolean("use_errors", this->draft_.useErrors),
         Group("custom.errorGroup", Json::array({this->String("error_pointer", this->draft_.errorPointer, {}, 1, 256),
                                                 this->Table("error_map", this->draft_.errorMap, {},
                                                             "custom.error_category", false, true)}))});
    Json response = Json::array({Heading("response"),
                                 this->String("response_mode", this->draft_.responseMode, "custom.response_mode"),
                                 Group("custom.jsonResponseGroup", std::move(mapping))});
    return Group("custom.root",
                 Json::array({Group("custom.basic", std::move(basics)), Group("custom.request", std::move(request)),
                              Group("custom.response", std::move(response)), Heading("secrets"),
                              this->Table("secrets", this->draft_.secrets, {}, {}, true)}));
}
// 绑定到唯一草稿，控件刷新只发生在必要的表选择处。
// 入参：变化回调。
// 返回：无。
void CustomTranslationEditor::Bind(std::function<void()> changed)
{
    for (const StringField& field : this->strings_)
    {
        RequireCustom(this->renderer_.BindString(
            field.id, // 读取当前控件绑定的唯一字段草稿。
            // 入参：无，value借用本次子窗草稿字段。
            // 返回：对应字符串或布尔控件的成功值快照。
            [value = field.value]() { return RendererStringResult{true, *value, {}}; },
            // 保存字符串中间输入并触发完整候选校验，不刷新原生编辑记录。
            // 入参：input为控件当前UTF-8内容。
            // 返回：接受本次编辑；完整提交准入由changed回调更新。
            [value = field.value, changed](std::string_view input)
            {
                *value = input;
                changed();
                return RendererChangeResult{};
            }));
        if (!field.choices.empty())
            RequireCustom(this->renderer_.BindOptions(
                field.id, // 通过宿主读取领域选项，Settings不复制允许值规则。
                // 入参：无，key为该字段的领域目录键。
                // 返回：已适配到公共渲染器的选项列表。
                [this, key = field.choices]()
                { return TranslationRendererOptions(this->callbacks_.translationChoices(key)); }));
    }
    for (const BoolField& field : this->bools_)
        RequireCustom(this->renderer_.BindBool(
            field.id, // 读取当前控件绑定的唯一字段草稿。
            // 入参：无，value借用本次子窗草稿字段。
            // 返回：对应字符串或布尔控件的成功值快照。
            [value = field.value]() { return RendererBoolResult{true, *value, {}}; },
            // 保存布尔选择并刷新活动分组与提交准入。
            // 入参：input为复选框候选状态。
            // 返回：接受本次编辑的公共结果。
            [value = field.value, changed](bool input)
            {
                *value = input;
                changed();
                return RendererChangeResult{};
            }));
    for (auto& table : this->tables_)
        table->Bind(changed);
}
// 所有非活动模式仅隐藏，保留其原始中间输入及原生撤销历史。
// 入参：无。
// 返回：无。
void CustomTranslationEditor::RefreshVisibility()
{
    (void)this->renderer_.SetVisible("custom.jsonBodyGroup", this->draft_.bodyMode == "json");
    (void)this->renderer_.SetVisible("custom.form_body.group", this->draft_.bodyMode == "form");
    (void)this->renderer_.SetVisible("custom.rawBodyGroup", this->draft_.bodyMode == "raw");
    (void)this->renderer_.SetVisible("custom.jsonResponseGroup", this->draft_.responseMode == "json");
    (void)this->renderer_.SetVisible("custom.singleGroup", this->draft_.extraction == "single");
    (void)this->renderer_.SetVisible("custom.arrayGroup", this->draft_.extraction == "array");
    (void)this->renderer_.SetVisible("custom.successGroup", this->draft_.useSuccess);
    (void)this->renderer_.SetVisible("custom.errorGroup", this->draft_.useErrors);
    (void)this->renderer_.SetVisible("custom.successTextGroup", this->draft_.scalarType == "string");
    (void)this->renderer_.SetVisible("custom.successNumberGroup", this->draft_.scalarType == "number");
    (void)this->renderer_.SetVisible("custom.successBooleanGroup", this->draft_.scalarType == "boolean");
}
// 只在候选验证/确认时装配协议对象。
// 入参：输出。
// 返回：活动JSON解析结果。
bool CustomTranslationEditor::Apply(nlohmann::json& profile) const
{
    nlohmann::json config;
    if (!this->draft_.Build(config))
        return false;
    profile["configuration"] = std::move(config);
    profile["secrets"] = this->draft_.secrets;
    return true;
}
} // namespace open_st
