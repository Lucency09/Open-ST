// 提供独立原位纯文本输入；坐标为桌面物理像素，不拥有业务文档。
#pragma once
#include <memory>
#include <window_renderer.h>

namespace open_st
{
enum class InlineEditCommand
{
    SelectAll,
    Copy,
    Paste,
    Cut,
    Undo,
    Redo
};
enum class InlineTextRequest
{
    Rejected,
    Queued,
    Finished
};
struct InlineTextOptions
{
    HWND owner{};
    RECT bounds{}, clip{};
    unsigned fontPixelHeight{24};
    std::uint32_t rgb{};
    std::wstring fontFamily{L"Segoe UI"};
    std::wstring text;
    std::size_t maxLength{8192};
};
struct InlineTextCallbacks
{
    std::function<bool(std::wstring_view)> change;
    std::function<bool(std::wstring_view, bool)> finish;
    // 原生输入拒绝的结构化通知；宿主自行本地化，可调用 SetError，不得同步销毁组件。
    std::function<void(std::string_view)> error;
};
class InlineTextEditor final
{
  public:
    // 创建未打开的组件。入参：无。返回：无。
    InlineTextEditor();
    // 关闭组件且不调用业务。入参：无。返回：无。
    ~InlineTextEditor();
    // 禁止复制窗口所有权。入参：源对象。返回：已删除。
    InlineTextEditor(const InlineTextEditor&) = delete;
    // 禁止复制窗口所有权。入参：源对象。返回：已删除。
    InlineTextEditor& operator=(const InlineTextEditor&) = delete;
    // 打开可激活的有主窗口；失败不保留窗口。入参：options 为物理布局及初值，callbacks 同步借用文本。
    // 返回：成功为空错误；owner 销毁会自动关闭窗口，不调用 finish，宿主须自行取消业务事务。
    RendererResult Open(const InlineTextOptions& options, InlineTextCallbacks callbacks);
    // 请求确认有效正文；组合输入中拒绝。入参：无。返回：宿主接受通知为 Queued，已结束为 Finished。
    InlineTextRequest RequestCommit() noexcept;
    // 程序取消可终止组合，不依赖文本有效性。入参：无。返回：宿主接受通知为 Queued。
    InlineTextRequest RequestCancel() noexcept;
    // 查询输入法组合。入参：无。返回：组合中为 true。
    bool IsComposing() const noexcept;
    // 提供输入宿主窗口。入参：无。返回：借用 HWND，关闭时为空。
    HWND NativeHandle() const noexcept;
    // 清理窗口和回调，重复安全；业务须在回调返回后销毁组件。入参：无。返回：无，不发送 finish。
    void Close() noexcept;
    // 设置宿主提供的错误提示。入参：text 为可见文字，空值清除。返回：无。
    void SetError(std::wstring text);
    // 恢复输入焦点，不提交。入参：无。返回：无。
    void Focus() noexcept;
    // 宿主排队提交失败后恢复同一控件，不丢失原生历史和选区。入参：无。返回：无，不触发 change。
    void Resume() noexcept;
    // 转发已被系统热键截获的编辑动作，不合成键盘。入参：command 为原生编辑命令。
    // 返回：本控件持焦点且空闲时已分派为 true；组合、结束或跨线程时 false。
    bool InvokeEditCommand(InlineEditCommand command) noexcept;

  private:
    friend class InlineTextEditorTestAccess;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace open_st
