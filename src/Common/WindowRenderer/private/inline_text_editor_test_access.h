// 为原位输入测试替换只读前台查询；不修改系统焦点，不进入产品公开接口。
#pragma once
#include <inline_text_editor.h>

namespace open_st
{
class InlineTextEditorTestAccess final
{
  public:
    // 注入环境查询以确定性验证前台准入，空查询恢复真实 Win32 查询。
    // 入参：editor为同线程组件；query为查询器。返回：无；不会执行编辑动作。
    static void SetForegroundQuery(InlineTextEditor& editor, std::function<HWND()> query);
};
} // namespace open_st
