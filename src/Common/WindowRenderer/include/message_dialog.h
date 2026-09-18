// 提供基于公共 Renderer 的通用提示与确认窗口，不解释业务文本键。

#pragma once
#include <functional>
#include <string>
#include <windows.h>

namespace open_st
{
class WindowRenderer;
enum class MessageDialogResult
{
    Accepted,
    Cancelled,
    Failed
};
struct MessageDialogOptions
{
    HWND owner{};
    HICON icon{};
    std::function<std::wstring()> title;
    std::function<std::wstring()> message;
    std::function<std::wstring()> acceptText;
    std::function<std::wstring()> cancelText;
    bool confirmation{};
    WindowRenderer** activeRenderer{};
    std::function<bool(MSG&)> processThreadMessage;
};

// 同步显示通用消息；确认模式默认焦点动作是取消，避免误确认。
// 入参：options：借用 owner、icon、回调及可选活动窗口槽；回调须持续有效至返回。
// 返回：明确点击确认 Accepted；关闭、取消或退出消息 Cancelled；准备或运行失败 Failed。
[[nodiscard]] MessageDialogResult ShowMessageDialog(const MessageDialogOptions& options) noexcept;
} // namespace open_st
