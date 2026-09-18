// 复用 Renderer 的布局、绑定、模态消息循环和窗口资源管理。

#include <message_dialog.h>
#include <window_renderer.h>

namespace open_st
{
// 建立唯一通用消息布局，并把用户动作映射为结果。
// 入参：options：文本提供器、窗口上下文和可选线程消息转发。
// 返回：接受、取消或失败；不传播异常，不执行任何业务副作用。
MessageDialogResult ShowMessageDialog(const MessageDialogOptions& options) noexcept
{
    try
    {
        if (!options.title || !options.message || !options.acceptText || (options.confirmation && !options.cancelText))
            return MessageDialogResult::Failed;
        nlohmann::json layout = nlohmann::json::parse(R"({
            "schemaVersion":1,
            "window":{"titleKey":"title","initialSize":[560,280],"minSize":[360,200],"resizable":true},
            "content":{"type":"column","id":"body","padding":20,"gap":12,"children":[
                {"type":"text","id":"message","textKey":"message"}]},
            "footer":{"leading":[],"trailing":[{"type":"button","id":"confirm","textKey":"confirm"}]}
        })");
        if (options.confirmation)
            layout["footer"]["trailing"].push_back({{"type", "button"}, {"id", "cancel"}, {"textKey", "cancel"}});
        WindowRenderer renderer;
        struct ActiveGuard
        {
            WindowRenderer** slot;
            WindowRenderer* previous;
            // 撤销局部窗口借用，嵌套提示恢复外层借用。
            // 入参：无。
            // 返回：无返回值。
            ~ActiveGuard()
            {
                if (this->slot != nullptr)
                    *this->slot = this->previous;
            }
        } guard{options.activeRenderer, options.activeRenderer != nullptr ? *options.activeRenderer : nullptr};
        if (options.activeRenderer != nullptr)
            *options.activeRenderer = &renderer;
        MessageDialogResult choice = MessageDialogResult::Cancelled;
        bool failed = false;
        if (!renderer.LoadLayout(layout) ||
            !renderer.SetTextResolver(
                // 按通用布局键调用业务传入的文本提供器。
                // 入参：key：通用文本标识。
                // 返回：对应的当前文本。
                [&options](std::string_view key)
                {
                    if (key == "title")
                        return options.title();
                    if (key == "message")
                        return options.message();
                    if (key == "cancel")
                        return options.cancelText();
                    return options.acceptText();
                }) ||
            !renderer.BindAction("confirm",
                                 // 只记录明确确认，不在通用窗口中执行产品操作。
                                 // 入参：无。
                                 // 返回：无返回值。
                                 [&renderer, &choice]()
                                 {
                                     choice = MessageDialogResult::Accepted;
                                     (void)renderer.RequestClose();
                                 }) ||
            !renderer.SetCloseHandler(
                // 系统关闭和 Esc 均保留取消结果。
                // 入参：无。
                // 返回：无返回值。
                [&renderer]() { (void)renderer.RequestClose(); }) ||
            !renderer.SetErrorHandler(
                // 回调或渲染失败不能冒充用户确认。
                // 入参：忽略的 RendererResult 由返回值统一表达失败。
                // 返回：无返回值。
                [&renderer, &failed](const RendererResult&)
                {
                    failed = true;
                    (void)renderer.RequestClose();
                }) ||
            !renderer.SetDefaultAction(options.confirmation ? "cancel" : "confirm"))
            return MessageDialogResult::Failed;
        if (options.confirmation && !renderer.BindAction("cancel",
                                                         // 取消按钮请求退出，不改变默认取消结果。
                                                         // 入参：无。
                                                         // 返回：无返回值。
                                                         [&renderer]() { (void)renderer.RequestClose(); }))
            return MessageDialogResult::Failed;
        RendererWindowOptions window;
        window.owner = options.owner;
        window.icon = options.icon;
        if (!renderer.ShowModal(window, options.processThreadMessage) || failed)
            return MessageDialogResult::Failed;
        return choice;
    }
    catch (...)
    {
        return MessageDialogResult::Failed;
    }
}
} // namespace open_st
