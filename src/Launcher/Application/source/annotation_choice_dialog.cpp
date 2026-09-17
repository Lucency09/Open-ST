// 通过公共表单编辑笔刷大小等离散参数，不持有标注文档或历史。
#include "annotation_style_dialog.h"
#include <algorithm>
#include <stdexcept>
#include <window_renderer.h>

namespace open_st
{
// 以公共下拉框和模态生命周期提供离散工具参数，不复制窗口基础设施。
// 入参：owner、value、choices、titleKey、labelKey、text、error 见私有头。
// 返回：仅完整确认时发布选择，任何失败保持原值。
AnnotationStyleResult ShowAnnotationChoiceDialog(HWND owner, unsigned& value, std::span<const unsigned> choices,
                                                 std::string_view titleKey, std::string_view labelKey,
                                                 const std::function<std::wstring(std::string_view)>& text,
                                                 std::wstring& error, const std::function<bool(unsigned)>& preview)
{
    error.clear();
    if (!text || choices.empty() || std::find(choices.begin(), choices.end(), value) == choices.end())
    {
        error = L"工具参数窗口输入无效。";
        return AnnotationStyleResult::Failed;
    }
    try
    {
        WindowRenderer renderer;
        unsigned draft = value;
        AnnotationStyleResult result = AnnotationStyleResult::Cancelled;
        bool finished = false;
        unsigned displayed = value;
        bool previewFailed = false;
        bool previewing = false;
        const nlohmann::json layout = nlohmann::json::parse(R"({
          "schemaVersion":1,"window":{"titleKey":"title","initialSize":[380,220],"minSize":[320,200],"resizable":false},
          "content":{"type":"column","id":"body","padding":16,"children":[
            {"type":"select","id":"choice","labelKey":"label"}]},
          "footer":{"leading":[],"trailing":[
            {"type":"button","id":"confirm","textKey":"annotation.style.ok"},
            {"type":"button","id":"cancel","textKey":"annotation.style.cancel"}]}})");
        // 公共接口失败由外层统一转换为失败结果。
        // 入参：checked 为控件调用结果。返回：无，失败抛出。
        const auto require = [](const RendererResult& checked)
        {
            if (!checked)
                throw std::runtime_error(checked.code);
        };
        // 只有有效候选成功预览才能确认；失败后回原值也重新核验。
        // 入参：无。返回：候选可提交时true。
        const auto validate = [&]()
        {
            if (finished || previewing)
                return false;
            bool accepted = true;
            if (preview && (draft != displayed || previewFailed))
            {
                previewing = true;
                try
                {
                    accepted = preview(draft);
                }
                catch (...)
                {
                    accepted = false;
                }
                previewing = false;
                previewFailed = !accepted;
            }
            if (finished)
                return false;
            if (accepted)
                displayed = draft;
            require(renderer.SetEnabled("confirm", accepted));
            require(renderer.SetStatus(accepted ? L"" : text("annotation.style.preview_failed")));
            return accepted;
        };
        require(renderer.LoadLayout(layout));
        require(renderer.SetTextResolver(
            // 将通用字段键适配为本次工具参数文字。
            // 入参：key 为布局键。返回：本地化文本。
            [&text, titleKey, labelKey](std::string_view key)
            {
                return text(key == "title" ? titleKey : key == "label" ? labelKey : key);
            }));
        require(renderer.BindString(
            "choice",
            // 读取本次候选，确认前不写入工具默认值。
            // 入参：无。返回：选项值。
            [&draft]() { return RendererStringResult{true, std::to_string(draft), {}}; },
            // 只接受显式允许的档位，避免从控件字符串隐式扩展规则。
            // 入参：candidate 为选项值。返回：是否接受。
            [&draft, &finished, &previewing, &validate, choices](std::string_view candidate)
            {
                if (finished || previewing)
                    return RendererChangeResult{false, {}};
                for (unsigned choice : choices)
                    if (candidate == std::to_string(choice))
                    {
                        draft = choice;
                        (void)validate();
                        return RendererChangeResult{};
                    }
                return RendererChangeResult{false, {}};
            }));
        require(renderer.BindOptions("choice",
                                     // 生成本次工具给定的离散档位。
                                     // 入参：无。返回：下拉选项。
                                     [choices]()
                                     {
                                         RendererOptionsResult options;
                                         for (unsigned choice : choices)
                                             options.options.push_back(
                                                 {std::to_string(choice), std::to_wstring(choice)});
                                         return options;
                                     }));
        require(renderer.BindAction("confirm",
                                    // 完成时只标记意图，模态成功返回后发布候选。
                                    // 入参：无。返回：无。
                                    [&renderer, &result, &require, &finished, &validate]()
                                    {
                                        if (finished || !validate())
                                            return;
                                        finished = true;
                                        result = AnnotationStyleResult::Accepted;
                                        require(renderer.RequestClose());
                                    }));
        // 标题栏、Esc 及取消按钮共用无提交关闭。
        // 入参：无。返回：无。
        const auto cancel = [&renderer, &require, &result, &finished]()
        {
            if (finished)
                return;
            finished = true;
            result = AnnotationStyleResult::Cancelled;
            require(renderer.RequestClose());
        };
        require(renderer.BindAction("cancel", cancel));
        require(renderer.SetCloseHandler(cancel));
        require(renderer.SetDefaultAction("confirm"));
        require(renderer.SetErrorHandler(
            // 任何控件错误都不能误报已确认。
            // 入参：未命名参数为结构化错误。返回：无。
            [&renderer, &result, &finished](const RendererResult&)
            {
                finished = true;
                result = AnnotationStyleResult::Failed;
                (void)renderer.RequestClose();
            }));
        RendererWindowOptions options;
        options.owner = owner;
        options.topmost = true;
        if (!renderer.ShowModal(options) || result == AnnotationStyleResult::Failed)
        {
            error = L"工具参数窗口失败。";
            return AnnotationStyleResult::Failed;
        }
        if (result == AnnotationStyleResult::Accepted)
            value = draft;
        return result;
    }
    catch (...)
    {
        error = L"编辑工具参数时发生异常。";
        return AnnotationStyleResult::Failed;
    }
}
} // namespace open_st
