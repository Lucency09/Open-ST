// 把设置 Busy 语义适配为公共消息窗口输入。
#include "settings_messages.h"
#include <message_dialog.h>

namespace open_st
{
// 显示文件占用提示，公共层只收到文字而不解析设置错误。
// 入参：owner、icon 为借用上下文；text 为宿主提供的本地化查询。
// 返回：提示正常关闭 true；异常或窗口失败 false。
bool ShowSettingsBusyMessage(HWND owner, HICON icon, const std::function<std::wstring(std::string_view)>& text) noexcept
{
    try
    {
        if (!text)
            return false;
        MessageDialogOptions options;
        options.owner = owner;
        options.icon = icon;
        // 由设置模块解析业务文本键，公共窗口只调用通用提供器。
        // 入参：无。
        // 返回：当前语言的标题。
        options.title = [&text]() { return text("settings.title"); };
        // 读取文件忙提示。
        // 入参：无。
        // 返回：当前语言正文。
        options.message = [&text]() { return text("settings.file_busy"); };
        // 读取既有确认按钮文本。
        // 入参：无。
        // 返回：当前语言按钮文案。
        options.acceptText = [&text]() { return text("dialog.ok"); };
        return ShowMessageDialog(options) != MessageDialogResult::Failed;
    }
    catch (...)
    {
        return false;
    }
}
} // namespace open_st
