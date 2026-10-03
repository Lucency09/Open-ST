// 自定义HTTP一页分组表单；只拥有UI草稿并消费领域元数据，不执行HTTP。
#pragma once
#include "translation_custom_draft.h"
#include "translation_key_value_editor.h"
namespace open_st
{
class CustomTranslationEditor final
{
  public:
    // 保存子表单独立草稿。
    // 入参：借用窗口、元数据和原条目。
    // 返回：未绑定编辑器。
    CustomTranslationEditor(WindowRenderer& renderer, const SettingsWindowCallbacks& callbacks,
                            const nlohmann::json& profile);
    // 建立纵向组和字段目录。
    // 入参：无。
    // 返回：公共布局节点，仅调用一次。
    nlohmann::json Layout();
    // 绑定普通字段及键值表。
    // 入参：候选改变通知。
    // 返回：无。
    void Bind(std::function<void()> changed);
    // 根据当前模式显示相关组，不改写任何输入。
    // 入参：无。
    // 返回：无。
    void RefreshVisibility();
    // 把当前活动模式装配成协议配置。
    // 入参：profile为输出候选。
    // 返回：解析状态。
    bool Apply(nlohmann::json& profile) const;

  private:
    struct StringField
    {
        std::string id, choices;
        std::string* value;
    };
    struct BoolField
    {
        std::string id;
        bool* value;
    };
    // 描述并登记一个字符串字段。
    // 入参：ID/草稿/枚举/行数/最大长度。
    // 返回：布局节点。
    nlohmann::json String(std::string name, std::string& value, std::string choices = {}, int lines = 1,
                          int maximum = 4096);
    // 描述并登记布尔字段。
    // 入参：ID和草稿。
    // 返回：checkbox布局。
    nlohmann::json Boolean(std::string name, bool& value);
    // 描述键值组并保持编辑器寿命。
    // 入参：UI名/对象/可选枚举及遮罩策略。
    // 返回：布局。
    nlohmann::json Table(std::string name, nlohmann::json& value, std::string keys = {}, std::string values = {},
                         bool secret = false, bool emptyKey = false);
    WindowRenderer& renderer_;
    const SettingsWindowCallbacks& callbacks_;
    CustomTranslationDraft draft_;
    std::vector<StringField> strings_;
    std::vector<BoolField> bools_;
    std::vector<std::unique_ptr<TranslationKeyValueEditor>> tables_;
};
} // namespace open_st
