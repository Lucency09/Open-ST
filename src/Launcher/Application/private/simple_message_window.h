#pragma once

#include <functional>
#include <string>
#include <windows.h>

namespace open_st
{
class WindowRenderer;
// 使用纯内存布局显示单文本模态窗口；关闭和 WM_QUIT 均成功，创建或运行失败返回 false 供宿主兜底。
[[nodiscard]] bool TryShowSimpleMessageWindow(HWND owner, HICON icon, const std::function<std::wstring()>& title,
                                              const std::function<std::wstring()>& message,
                                              const std::function<std::wstring()>& confirmText,
                                              WindowRenderer*& activeRenderer,
                                              const std::function<bool(MSG&)>& processThreadMessage) noexcept;
} // namespace open_st
