// 将应用现有简单提示入口适配到公共消息窗口，保留活动窗口和线程消息契约。

#include "simple_message_window.h"
#include <log.h>
#include <message_dialog.h>

namespace open_st
{
// 显示简单提示并复用公共 Renderer 消息窗口。
// 入参：owner、icon 为借用上下文；title、message、confirmText 为文本提供器；activeRenderer
// 为活动借用槽；processThreadMessage 为线程消息适配。 返回：正常确认、关闭或 WM_QUIT 为 true；准备或运行失败为 false。
bool TryShowSimpleMessageWindow(HWND owner, HICON icon, const std::function<std::wstring()>& title,
                                const std::function<std::wstring()>& message,
                                const std::function<std::wstring()>& confirmText, WindowRenderer*& activeRenderer,
                                const std::function<bool(MSG&)>& processThreadMessage) noexcept
{
    try
    {
        MessageDialogOptions options;
        options.owner = owner;
        options.icon = icon;
        options.title = title;
        options.message = message;
        options.acceptText = confirmText;
        options.activeRenderer = &activeRenderer;
        options.processThreadMessage = processThreadMessage;
        const bool shown = ShowMessageDialog(options) != MessageDialogResult::Failed;
        if (!shown)
            OPEN_ST_LOG_ERROR("Simple message window failed.");
        return shown;
    }
    catch (...)
    {
        OPEN_ST_LOG_ERROR("Simple message window failed with an exception.");
        return false;
    }
}
} // namespace open_st
