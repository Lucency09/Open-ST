// 共享设置及欢迎保存失败的本地化提示适配，不创建原生控件。
#pragma once
#include <functional>
#include <string>
#include <string_view>
#include <windows.h>

namespace open_st
{
// 通过公共 Renderer 提示文件忙，调用方须先结束 JSON 事务并保留草稿。
// 入参：owner、icon 为借用窗口上下文；text 为当前语言文本提供器。
// 返回：提示正常关闭 true；创建或运行失败 false。
bool ShowSettingsBusyMessage(HWND owner, HICON icon,
                             const std::function<std::wstring(std::string_view)>& text) noexcept;
} // namespace open_st
