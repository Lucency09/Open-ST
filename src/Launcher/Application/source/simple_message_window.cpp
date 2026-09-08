#include "simple_message_window.h"

#include <log.h>
#include <window_renderer.h>

namespace open_st
{
// 文本已由 Application 本地化，布局不读取文件，也不引入设置字段或保存行为。
bool TryShowSimpleMessageWindow(HWND owner, HICON icon, const std::function<std::wstring()>& title,
                                const std::function<std::wstring()>& message,
                                const std::function<std::wstring()>& confirmText, WindowRenderer*& activeRenderer,
                                const std::function<bool(MSG&)>& processThreadMessage) noexcept
{
    try
    {
        const nlohmann::json layout = nlohmann::json::parse(R"({
            "schemaVersion":1,
            "window":{"titleKey":"title","initialSize":[560,280],"minSize":[360,200],"resizable":true},
            "content":{"type":"column","id":"body","padding":20,"gap":12,"children":[
                {"type":"text","id":"message","textKey":"message"}]},
            "footer":{"leading":[],"trailing":[{"type":"button","id":"confirm","textKey":"confirm"}]}
        })");
        WindowRenderer renderer;
        struct ActiveGuard
        {
            WindowRenderer*& active;
            // 模态退出前清空借用指针，避免语言通知访问已析构的窗口。
            ~ActiveGuard()
            {
                this->active = nullptr;
            }
        } guard{activeRenderer};
        activeRenderer = &renderer;
        if (!renderer.LoadLayout(layout) ||
            !renderer.SetTextResolver(
                [&title, &message, &confirmText](std::string_view key)
                { return key == "title"     ? title()
                         : key == "message" ? message()
                                            : confirmText(); }) ||
            !renderer.BindAction("confirm", [&renderer]() { (void)renderer.RequestClose(); }) ||
            !renderer.SetCloseHandler([&renderer]() { (void)renderer.RequestClose(); }) ||
            !renderer.SetDefaultAction("confirm"))
        {
            OPEN_ST_LOG_ERROR("Simple message window preparation failed.");
            return false;
        }
        RendererWindowOptions options;
        options.owner = owner;
        options.icon = icon;
        const RendererResult result = renderer.ShowModal(options, processThreadMessage);
        if (!result)
        {
            OPEN_ST_LOG_ERROR("Simple message window failed. code=", result.code);
        }
        return static_cast<bool>(result);
    }
    catch (...)
    {
        OPEN_ST_LOG_ERROR("Simple message window failed with an exception.");
        return false;
    }
}
} // namespace open_st
