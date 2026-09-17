// 声明标注属性表单的业务入口，布局与窗口生命周期由公共 WindowRenderer 提供。
#pragma once
#include <annotation.h>
#include <functional>
#include <span>
#include <string>
#include <windows.h>

namespace open_st
{
enum class AnnotationStyleResult
{
    Accepted,
    Cancelled,
    Failed
};
using AnnotationStylePreview = std::function<bool(const AnnotationStyle&)>;
struct AnnotationTextStyle
{
    unsigned fontSize{24};
    bool allowEdit{};
    bool editRequested{};
};
using AnnotationTextStylePreview = std::function<bool(const AnnotationStyle&, unsigned)>;

// 复用样式表单编辑文字字号，并可在确认属性后请求原位编辑正文。
// 入参：style/textStyle 为候选；text/error 为文字与诊断；preview 同步预览完整文字样式。
// 返回：仅确认发布属性及编辑正文意图，不在窗口内修改文档。
AnnotationStyleResult ShowAnnotationTextStyleDialog(HWND owner, AnnotationStyle& style, AnnotationTextStyle& textStyle,
                                                    const std::function<std::wstring(std::string_view)>& text,
                                                    std::wstring& error,
                                                    const AnnotationTextStylePreview& preview = {});

// 同步编辑标注默认样式或元素属性，只在确认成功时修改输入值。
// 入参：owner 为宿主；value 为原样式；allowLineWidth 决定线宽项；text 查询本地化；error 接收诊断；
// preview 为同步借用的可选预览回调，失败必须保持先前预览。
// 返回：确认、取消或失败；窗口、DPI、模态与控件事件由 WindowRenderer 统一管理。
AnnotationStyleResult ShowAnnotationStyleDialog(HWND owner, AnnotationStyle& value, bool allowLineWidth,
                                                const std::function<std::wstring(std::string_view)>& text,
                                                std::wstring& error, const AnnotationStylePreview& preview = {});

// 使用公共表单编辑一个离散工具参数，取消时保持原值。
// 入参：owner 为宿主；value 为当前值；choices 为允许值；titleKey/labelKey 为本地化键；text/error 为文字与诊断。
// 返回：确认、取消或失败；不生成标注文档或历史。
AnnotationStyleResult ShowAnnotationChoiceDialog(HWND owner, unsigned& value, std::span<const unsigned> choices,
                                                 std::string_view titleKey, std::string_view labelKey,
                                                 const std::function<std::wstring(std::string_view)>& text,
                                                 std::wstring& error,
                                                 const std::function<bool(unsigned)>& preview = {});
} // namespace open_st
