// 用通用 Renderer 显示简单模态提示，并在退出时撤销活动窗口借用。

#include "simple_message_window.h"

#include <log.h>
#include <window_renderer.h>

namespace open_st
{
// 显示带标题、正文和确认按钮的简单模态窗口。
// 入参：owner、icon：借用的所属窗口及图标；title、message、confirmText：文本查询回调；activeRenderer：输出当前活动 Renderer
// 的借用指针；processThreadMessage：可选线程消息处理回调。
// 返回：正常关闭或 WM_QUIT 时 true；创建、运行或异常失败时 false；发布局部 Renderer 借用后，退出时撤销该借用。
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
            // 在简单提示退出时撤销外部对局部 Renderer 的借用。
            // 入参：无。
            // 返回：析构函数无返回值；将 active 引用指向的指针置空。
            ~ActiveGuard()
            {
                this->active = nullptr;
            }
        } guard{activeRenderer};
        activeRenderer = &renderer;
        if (!renderer.LoadLayout(layout) ||
            !renderer.SetTextResolver(
                // 为简单消息布局查询标题、正文或确认按钮文字。
                // 入参：key：布局文本键；借用 title、message、confirmText 三个查询回调。
                // 返回：title 或 message 键调用相应提供器；其他键返回确认按钮文字。
                [&title, &message, &confirmText](std::string_view key)
                { return key == "title"     ? title()
                         : key == "message" ? message()
                                            : confirmText(); }) ||
            // 把确认或系统关闭动作转为简单提示窗口的延迟关闭请求。
            // 入参：无显式入参；借用当前 renderer。
            // 返回：无返回值；请求关闭，让当前回调先安全返回。
            !renderer.BindAction("confirm", [&renderer]() { (void)renderer.RequestClose(); }) ||
            // 把确认或系统关闭动作转为简单提示窗口的延迟关闭请求。
            // 入参：无显式入参；借用当前 renderer。
            // 返回：无返回值；请求关闭，让当前回调先安全返回。
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
