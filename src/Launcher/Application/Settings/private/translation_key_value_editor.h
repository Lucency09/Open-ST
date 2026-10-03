// 普通键值表的Settings适配：使用公共只读table与模态编辑，不创建原生控件或消息循环。
#pragma once
#include "translation_settings.h"
namespace open_st
{
class TranslationKeyValueEditor final
{
  public:
    // 借用子表单唯一对象草稿，选项仅由领域回调提供。
    // 入参：id/title标识控件；keys/values为可选枚举字段；secret遮罩显示；emptyKey允许空业务错误码。
    // 返回：尚未绑定的编辑器。
    TranslationKeyValueEditor(WindowRenderer& renderer, const SettingsWindowCallbacks& callbacks,
                              nlohmann::json& entries, std::string id, std::string title, std::string keys = {},
                              std::string values = {}, bool secret = false, bool emptyKey = false);
    // 生成复用公共布局的表和操作列。
    // 入参：无。
    // 返回：独立布局节点。
    nlohmann::json Layout() const;
    // 绑定查询与动作。
    // 入参：changed用于重校验父候选。
    // 返回：无，绑定失败抛错。
    void Bind(std::function<void()> changed);

  private:
    // 当前草稿行只向Renderer提供安全显示值。
    // 入参：无。
    // 返回：稳定行ID。
    RendererRowsResult Rows();
    // 操作仅编辑当前子草稿。
    // 入参：add/edit/delete。
    // 返回：无。
    void Act(std::string_view action);
    // 用公共子表单确认一项。
    // 入参：key/value在确认时输出。
    // 返回：明确确认状态。
    bool Edit(std::string& key, std::string& value, bool adding);
    WindowRenderer& renderer_;
    const SettingsWindowCallbacks& callbacks_;
    nlohmann::json& entries_;
    std::string id_, title_, keys_, values_, selected_;
    bool secret_{}, emptyKey_{};
    std::function<void()> changed_;
};
} // namespace open_st
