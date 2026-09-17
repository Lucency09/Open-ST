// 将标注样式草稿绑定到公共表单；不自行创建 HWND、字体、消息循环或解析颜色算法。
#include "annotation_style_dialog.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <window_renderer.h>

namespace open_st
{
namespace
{
constexpr std::array<unsigned, 5> WIDTHS{1, 2, 3, 5, 8};
constexpr std::array<std::uint32_t, 6> COLORS{0xFF0000, 0xFFCC00, 0x00AA44, 0x0066FF, 0xFFFFFF, 0x000000};

// 格式化数值颜色，解析仍统一使用 Annotation 的纯值入口。
// 入参：rgb 为 24 位颜色。返回：规范化颜色文本。
std::string ColorText(std::uint32_t rgb)
{
    char text[8]{};
    sprintf_s(text, "#%06X", static_cast<unsigned>(rgb));
    return text;
}

struct StyleEditor final
{
    WindowRenderer renderer;
    AnnotationStyle displayed;
    std::string color;
    std::optional<std::int64_t> transparency;
    std::string width;
    bool allowWidth{};
    bool finished{};
    bool previewing{};
    bool previewFailed{};
    AnnotationStyleResult result{AnnotationStyleResult::Cancelled};
    const std::function<std::wstring(std::string_view)>& text;
    const AnnotationStylePreview& preview;
    AnnotationTextStyle* textStyle{};
    const AnnotationTextStylePreview* textPreview{};
    unsigned fontCandidate{24}, displayedFont{24};
    bool editTextRequested{};

    // 记录公共控件或窗口准备失败，让外层统一返回失败。
    // 入参：value 为 Renderer 的结构化结果。返回：无，失败抛出诊断异常。
    void Require(const RendererResult& value)
    {
        if (!value)
            throw std::runtime_error(value.code);
    }

    // 校验完整草稿并发布一次有效预览，失败不改变上次成功样式。
    // 入参：无。返回：可以确认时 true。
    bool Validate()
    {
        if (this->finished || this->previewing)
            return false;
        AnnotationStyle candidate = this->displayed;
        bool valid = ParseAnnotationColor(this->color, candidate.rgb) && this->transparency &&
                     *this->transparency >= 0 && *this->transparency <= 100;
        if (valid)
            candidate.transparency = static_cast<unsigned>(*this->transparency);
        if (this->allowWidth)
        {
            valid = valid && std::any_of(WIDTHS.begin(), WIDTHS.end(),
                                         // 检查候选是否属于界面提供的线宽档位。
                                         // 入参：value 为允许档位。返回：匹配时 true。
                                         [this](unsigned value) { return this->width == std::to_string(value); });
            if (valid)
                candidate.lineWidth = static_cast<double>(std::stoi(this->width));
        }
        if (this->textStyle)
            valid = valid && IsValidAnnotationFontSize(this->fontCandidate);
        valid = valid && IsValidAnnotationStyle(candidate);
        bool accepted = valid;
        const bool changed = candidate.rgb != this->displayed.rgb ||
                             candidate.transparency != this->displayed.transparency ||
                             candidate.lineWidth != this->displayed.lineWidth ||
                             (this->textStyle && this->fontCandidate != this->displayedFont);
        if (valid && (changed || this->previewFailed) && (this->preview || (this->textPreview && *this->textPreview)))
        {
            this->previewing = true;
            try
            {
                accepted = this->textPreview && *this->textPreview
                               ? (*this->textPreview)(candidate, this->fontCandidate)
                               : this->preview(candidate);
            }
            catch (...)
            {
                accepted = false;
            }
            this->previewing = false;
            this->previewFailed = !accepted;
            if (this->finished)
                return false;
        }
        if (accepted)
        {
            this->displayed = candidate;
            this->displayedFont = this->fontCandidate;
            this->Require(this->renderer.RefreshValue("sample"));
        }
        this->Require(this->renderer.SetEnabled("confirm", accepted));
        if (this->textStyle && this->textStyle->allowEdit)
            this->Require(this->renderer.SetEnabled("editText", accepted));
        this->Require(this->renderer.SetStatus(!valid     ? this->text("annotation.style.invalid")
                                               : accepted ? L""
                                                          : this->text("annotation.style.preview_failed")));
        return accepted;
    }

    // 结束表单，公共 Renderer 延迟关闭以保护当前回调栈。
    // 入参：accepted 为确认意图。返回：无，非法确认保留窗口。
    void Finish(bool accepted)
    {
        if (this->finished || (accepted && !this->Validate()))
            return;
        this->result = accepted ? AnnotationStyleResult::Accepted : AnnotationStyleResult::Cancelled;
        this->finished = true;
        this->Require(this->renderer.RequestClose());
    }

    // 使用现有表单控件及通用数值色块构建标注属性布局。
    // 入参：无。返回：无，准备失败交由外层处理。
    void Prepare()
    {
        nlohmann::json layout = nlohmann::json::parse(R"({
          "schemaVersion":1,
          "window":{"titleKey":"annotation.style.title","initialSize":[460,400],"minSize":[360,300],"resizable":false},
          "content":{"type":"column","id":"body","padding":16,"gap":10,"children":[
            {"type":"row","id":"colorRow","gap":10,"children":[
              {"type":"edit","id":"color","labelKey":"annotation.style.color"},
              {"type":"swatch","id":"sample","textKey":"annotation.style.color","width":44}]},
            {"type":"row","id":"palette","gap":8,"children":[]},
            {"type":"integer","id":"transparency","labelKey":"annotation.style.transparency","minimum":0,"maximum":100,"slider":true}
          ]},
          "footer":{"leading":[],"trailing":[
            {"type":"button","id":"confirm","textKey":"annotation.style.ok"},
            {"type":"button","id":"cancel","textKey":"annotation.style.cancel"}]}
        })");
        for (std::size_t index = 0; index < COLORS.size(); ++index)
            layout["content"]["children"][1]["children"].push_back(
                {{"type", "swatch"}, {"id", "preset" + std::to_string(index)}, {"textKey", ColorText(COLORS[index])}});
        if (this->allowWidth)
            layout["content"]["children"].push_back(
                {{"type", "select"}, {"id", "width"}, {"labelKey", "annotation.style.width"}});
        if (this->textStyle)
        {
            layout["content"]["children"].push_back(
                {{"type", "select"}, {"id", "fontSize"}, {"labelKey", "annotation.text.size"}});
            if (this->textStyle->allowEdit)
                layout["footer"]["leading"].push_back(
                    {{"type", "button"}, {"id", "editText"}, {"textKey", "annotation.text.edit"}});
        }
        this->Require(this->renderer.LoadLayout(layout));
        if (this->textStyle)
        {
            this->Require(this->renderer.BindString(
                "fontSize",
                // 读取完整字号候选。
                // 入参：无。返回：稳定档位值。
                [this]() { return RendererStringResult{true, std::to_string(this->fontCandidate), {}}; },
                // 字号与颜色透明度共用一次预览和确认。
                // 入参：value 为档位字符串。返回：接受合法候选。
                [this](std::string_view value)
                {
                    if (this->finished || this->previewing)
                        return RendererChangeResult{false, {}};
                    for (unsigned size : ANNOTATION_FONT_SIZES)
                        if (value == std::to_string(size))
                        {
                            this->fontCandidate = size;
                            (void)this->Validate();
                            return RendererChangeResult{};
                        }
                    return RendererChangeResult{false, {}};
                }));
            this->Require(this->renderer.BindOptions("fontSize",
                                                     // 只展示首版明确支持的物理字号。
                                                     // 入参：无。返回：字号档位。
                                                     []()
                                                     {
                                                         RendererOptionsResult options;
                                                         for (unsigned size : ANNOTATION_FONT_SIZES)
                                                             options.options.push_back(
                                                                 {std::to_string(size), std::to_wstring(size)});
                                                         return options;
                                                     }));
            if (this->textStyle->allowEdit)
                this->Require(this->renderer.BindAction("editText",
                                                        // 先确认属性，再由App在表单返回后进入原位正文编辑。
                                                        // 入参：无。返回：无，不嵌套创建输入窗口。
                                                        [this]()
                                                        {
                                                            this->Finish(true);
                                                            if (this->result == AnnotationStyleResult::Accepted)
                                                                this->editTextRequested = true;
                                                        }));
        }
        this->Require(this->renderer.SetTextResolver(
            // 色块可访问名称采用颜色代码，其余文字由宿主查询。
            // 入参：key 为布局文字键。返回：显示文本。
            [this](std::string_view key)
            { return key.starts_with('#') ? std::wstring(key.begin(), key.end()) : this->text(key); }));
        this->Require(this->renderer.BindString(
            "color",
            // 读取原始颜色草稿，包括尚未完成的输入。
            // 入参：无。返回：当前文本。
            [this]() { return RendererStringResult{true, this->color, {}}; },
            // 保存完整原文并由统一业务校验决定能否预览。
            // 入参：value 为用户原文。返回：控件保留输入。
            [this](std::string_view value)
            {
                if (!this->finished && !this->previewing)
                {
                    this->color = value;
                    (void)this->Validate();
                }
                return RendererChangeResult{};
            }));
        this->Require(this->renderer.BindInteger(
            "transparency",
            // 非法整数由 Renderer 保留原始文字，业务不将它覆盖为零。
            // 入参：无。返回：上次完整数值或无效标记。
            [this]()
            { return RendererIntegerResult{this->transparency.has_value(), this->transparency.value_or(0), {}}; },
            // 接受合法整数或无效中间输入，再检查整份样式。
            // 入参：value 为完整整数或空。返回：控件保留输入。
            [this](std::optional<std::int64_t> value)
            {
                if (!this->finished && !this->previewing)
                {
                    this->transparency = value;
                    (void)this->Validate();
                }
                return RendererChangeResult{};
            }));
        this->Require(this->renderer.BindColor("sample",
                                               // 色样只反映最近一次成功预览，不能显示失败候选。
                                               // 入参：无。返回：数值 RGB。
                                               [this]()
                                               { return RendererColorResult{true, this->displayed.rgb, {}}; }));
        for (std::size_t index = 0; index < COLORS.size(); ++index)
        {
            const std::uint32_t rgb = COLORS[index];
            this->Require(this->renderer.BindColor(
                "preset" + std::to_string(index),
                // 读取固定预设数值，公共色块不解析标注数据。
                // 入参：无。返回：预设颜色。
                [rgb]() { return RendererColorResult{true, rgb, {}}; },
                // 选择预设只改变颜色，保留其他字段的未完成输入。
                // 入参：无。返回：无。
                [this, rgb]()
                {
                    if (this->finished || this->previewing)
                        return;
                    this->color = ColorText(rgb);
                    this->Require(this->renderer.RefreshValue("color"));
                    (void)this->Validate();
                }));
        }
        if (this->allowWidth)
        {
            this->Require(this->renderer.BindString(
                "width",
                // 读取稳定线宽选项值。
                // 入参：无。返回：数值文本。
                [this]() { return RendererStringResult{true, this->width, {}}; },
                // 更新线宽草稿，预览失败仍保留候选供继续调整。
                // 入参：value 为选项值。返回：接受控件选择。
                [this](std::string_view value)
                {
                    if (!this->finished && !this->previewing)
                    {
                        this->width = value;
                        (void)this->Validate();
                    }
                    return RendererChangeResult{};
                }));
            this->Require(this->renderer.BindOptions("width",
                                                     // 将界面线宽档位适配成公共下拉选项。
                                                     // 入参：无。返回：稳定数值及文字。
                                                     []()
                                                     {
                                                         RendererOptionsResult values;
                                                         for (unsigned width : WIDTHS)
                                                             values.options.push_back(
                                                                 {std::to_string(width), std::to_wstring(width)});
                                                         return values;
                                                     }));
        }
        this->Require(this->renderer.BindAction("confirm",
                                                // 最终确认重新校验，但不重复成功预览。
                                                // 入参：无。返回：无。
                                                [this]() { this->Finish(true); }));
        this->Require(this->renderer.BindAction("cancel",
                                                // 取消时不改输入值，由宿主撤销临时预览。
                                                // 入参：无。返回：无。
                                                [this]() { this->Finish(false); }));
        this->Require(this->renderer.SetCloseHandler(
            // 标题栏和 Esc 与取消按钮共用业务退出。
            // 入参：无。返回：无。
            [this]() { this->Finish(false); }));
        this->Require(this->renderer.SetDefaultAction("confirm"));
        this->Require(this->renderer.SetErrorHandler(
            // 公共控件错误不能伪装成成功确认。
            // 入参：未命名参数为结构化错误。返回：无。
            [this](const RendererResult&)
            {
                this->result = AnnotationStyleResult::Failed;
                this->finished = true;
                (void)this->renderer.RequestClose();
            }));
    }
};
} // namespace

// 通过公共 Renderer 显示标注表单，保留确认一次、取消不变及失败不提交的契约。
// 入参：owner、value、allowLineWidth、text、error、preview 见私有头声明。
// 返回：最终表单结果，不取得截图窗口或文档的所有权。
static AnnotationStyleResult RunStyleDialog(HWND owner, AnnotationStyle& value, bool allowLineWidth,
                                            const std::function<std::wstring(std::string_view)>& text,
                                            std::wstring& error, const AnnotationStylePreview& preview,
                                            AnnotationTextStyle* textStyle,
                                            const AnnotationTextStylePreview& textPreview)
{
    error.clear();
    if (!text || !IsValidAnnotationStyle(value) ||
        std::find(WIDTHS.begin(), WIDTHS.end(), value.lineWidth) == WIDTHS.end())
    {
        error = L"样式窗口参数无效。";
        return AnnotationStyleResult::Failed;
    }
    try
    {
        StyleEditor editor{{},
                           value,
                           ColorText(value.rgb),
                           static_cast<std::int64_t>(value.transparency),
                           std::to_string(static_cast<unsigned>(value.lineWidth)),
                           allowLineWidth,
                           false,
                           false,
                           false,
                           AnnotationStyleResult::Cancelled,
                           text,
                           preview};
        editor.textStyle = textStyle;
        editor.textPreview = &textPreview;
        if (textStyle)
            editor.fontCandidate = editor.displayedFont = textStyle->fontSize;
        editor.Prepare();
        RendererWindowOptions options;
        options.owner = owner;
        options.topmost = true;
        const RendererResult shown = editor.renderer.ShowModal(options);
        if (!shown || editor.result == AnnotationStyleResult::Failed)
        {
            error = L"显示样式窗口失败。";
            return AnnotationStyleResult::Failed;
        }
        if (editor.result == AnnotationStyleResult::Accepted)
        {
            value = editor.displayed;
            if (textStyle)
            {
                textStyle->fontSize = editor.displayedFont;
                textStyle->editRequested = editor.editTextRequested;
            }
        }
        return editor.result;
    }
    catch (...)
    {
        error = L"编辑样式时发生异常。";
        return AnnotationStyleResult::Failed;
    }
}
// 编辑通用标注颜色和线宽，沿用同一表单实现。
// 入参：见私有头。返回：确认、取消或失败。
AnnotationStyleResult ShowAnnotationStyleDialog(HWND owner, AnnotationStyle& value, bool allowLineWidth,
                                                const std::function<std::wstring(std::string_view)>& text,
                                                std::wstring& error, const AnnotationStylePreview& preview)
{
    return RunStyleDialog(owner, value, allowLineWidth, text, error, preview, nullptr, {});
}
// 为同一表单补文字字号与正文编辑入口。
// 入参：见私有头。返回：仅确认发布参数，不操作文档。
AnnotationStyleResult ShowAnnotationTextStyleDialog(HWND owner, AnnotationStyle& style, AnnotationTextStyle& textStyle,
                                                    const std::function<std::wstring(std::string_view)>& text,
                                                    std::wstring& error, const AnnotationTextStylePreview& preview)
{
    if (!IsValidAnnotationFontSize(textStyle.fontSize))
    {
        error = L"文字字号无效。";
        return AnnotationStyleResult::Failed;
    }
    return RunStyleDialog(owner, style, false, text, error, {}, &textStyle, preview);
}
} // namespace open_st
