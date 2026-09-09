// 声明简单模态提示适配入口，注入文本查询和线程消息处理能力。

#pragma once

#include <functional>
#include <string>
#include <windows.h>

namespace open_st
{
class WindowRenderer;
// 显示带标题、正文和确认按钮的简单模态窗口。
// 入参：owner、icon：借用的所属窗口及图标；title、message、confirmText：文本查询回调；activeRenderer：输出当前活动 Renderer
// 的借用指针；processThreadMessage：可选线程消息处理回调。
// 返回：正常关闭或 WM_QUIT 时 true；创建、运行或异常失败时 false；发布局部 Renderer 借用后，退出时撤销该借用。
[[nodiscard]] bool TryShowSimpleMessageWindow(HWND owner, HICON icon, const std::function<std::wstring()>& title,
                                              const std::function<std::wstring()>& message,
                                              const std::function<std::wstring()>& confirmText,
                                              WindowRenderer*& activeRenderer,
                                              const std::function<bool(MSG&)>& processThreadMessage) noexcept;
} // namespace open_st
