// 设置侧的翻译列表呈现和子表单，借用统一设置草稿，不持有翻译任务。
#pragma once
#include "settings_edit.h"
#include <array>
#include <settings_window.h>
#include <window_renderer.h>

namespace open_st
{
inline constexpr std::string_view TRANSLATION_INTERFACES_KEY = "translation.interfaces";
inline constexpr std::array<std::string_view, 4> TRANSLATION_STRING_KEYS{
    "translation.source_language", "translation.target_language", "translation.network.proxy_mode",
    "translation.network.proxy_address"};

// 将设置自有选项适配到公共控件，避免两个模块共享业务类型。
// 入参：options 为领域经宿主提供的显示选项。
// 返回：公共渲染器可直接消费的独立选项。
RendererOptionsResult TranslationRendererOptions(const std::vector<SettingsOption>& options);

// 校验整个设置候选，规则与选项均来自上级提供的领域回调。
// 入参：session 为候选；callbacks 为有效回调。
// 返回：空表示合法，否则为安全文案。
std::wstring TranslationSettingsError(const SettingsEditSession& session, const SettingsWindowCallbacks& callbacks);
// 在公共模态表单中编辑独立条目草稿，确认前不修改原条目。
// 入参：owner/icon 为宿主；profile 为确认后替换的条目；callbacks 提供领域规则；creating 允许新增时选择类型。
// 返回：明确确认且校验通过为 true，取消或窗口失败为 false。
bool EditTranslationProfile(HWND owner, HICON icon, nlohmann::json& profile, const SettingsWindowCallbacks& callbacks,
                            bool creating, std::string proxyMode, std::string proxyAddress);

class TranslationSettingsPanel final
{
  public:
    // 借用同一事务与 Renderer，交互选择不成为另一份列表草稿。
    // 入参：renderer/session/callbacks 必须覆盖面板寿命；changed 刷新宿主；modal 运行现有忙保护。
    // 返回：未绑定面板。
    TranslationSettingsPanel(WindowRenderer& renderer, SettingsEditSession& session,
                             const SettingsWindowCallbacks& callbacks, std::function<void()> changed,
                             std::function<void(std::function<void()>)> modal);
    // 创建绑定，复用公共 table/select/edit/button。
    // 入参：无。
    // 返回：无，绑定错误抛出至既有窗口边界。
    void Bind();
    // 同步选中身份、列表值及状态，不覆盖正在编辑的普通输入。
    // 入参：无。
    // 返回：无。
    void Refresh();
    // 让所有列表操作遵守宿主草稿可用状态。
    // 入参：ready 为是否已加载事务。
    // 返回：无。
    void Enable(bool ready);

  private:
    // 执行一次本地列表操作，子表单确认后按稳定ID替换。
    // 入参：operation 为布局动作名。
    // 返回：无，失败保留草稿。
    void Act(std::string_view operation);
    // 校验候选后一次替换统一事务。
    // 入参：value 为候选数组。
    // 返回：更新成功为 true。
    bool Store(const nlohmann::json& value);
    // 读取当前草稿列表；缺失时仅返回空数组供窗口降级显示。
    // 入参：无。
    // 返回：独立只读候选。
    nlohmann::json List() const;
    // 生成列表显示，名称不能代替稳定ID。
    // 入参：无。
    // 返回：独立公共表格行。
    RendererRowsResult Rows();
    WindowRenderer& renderer_;
    SettingsEditSession& session_;
    const SettingsWindowCallbacks& callbacks_;
    std::function<void()> changed_;
    std::function<void(std::function<void()>)> modal_;
    std::string selected_;
};
} // namespace open_st
