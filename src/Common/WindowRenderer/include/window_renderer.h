#pragma once

#include <Windows.h>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace open_st
{
struct RendererResult
{
    std::string code;
    std::string path;
    std::string id;
    // 空错误码表示成功，错误仅含结构定位信息。
    explicit operator bool() const noexcept
    {
        return this->code.empty();
    }
};
struct RendererStringResult
{
    bool success = true;
    std::string value;
    std::wstring error;
};
struct RendererChangeResult
{
    bool accepted = true;
    std::wstring error;
};
struct RendererOption
{
    std::string value;
    std::wstring label;
};
struct RendererOptionsResult
{
    bool success = true;
    std::vector<RendererOption> options;
    std::wstring error;
};
struct RendererWindowOptions
{
    HWND owner = nullptr;
    HICON icon = nullptr; // 借用图标，宿主保持存活。
    int showCommand = SW_SHOWNORMAL;
};
class WindowRenderer final
{
  public:
    // 创建尚未加载布局的渲染器，调用线程作为所属 UI 线程。
    WindowRenderer();
    // 停止分派并销毁自身持有的窗口与字体。
    ~WindowRenderer();
    // 禁止复制持有 Win32 资源的渲染器。
    WindowRenderer(const WindowRenderer&) = delete;
    // 禁止赋值转移活动窗口所有权。
    WindowRenderer& operator=(const WindowRenderer&) = delete;
    // 解析并复制文档，失败保留之前已加载的布局。
    RendererResult LoadLayout(const nlohmann::json& document);
    // 设置全部文本键的解析回调，窗口显示后不能替换。
    RendererResult SetTextResolver(std::function<std::wstring(std::string_view)> callback);
    // 绑定下拉框草稿读取及变更；拒绝变更后恢复已接受值。
    RendererResult BindString(std::string_view id, std::function<RendererStringResult()> read,
                              std::function<RendererChangeResult(std::string_view)> change);
    // 绑定下拉框动态选项，稳定值必须唯一。
    RendererResult BindOptions(std::string_view id, std::function<RendererOptionsResult()> query);
    // 绑定按钮动作，禁止重复注册。
    RendererResult BindAction(std::string_view id, std::function<void()> callback);
    // 将窗口关闭与 Esc 交由宿主处理。
    RendererResult SetCloseHandler(std::function<void()> callback);
    // 接收异步控件事件的结构化错误，宿主负责翻译；不必为纯测试宿主注册。
    RendererResult SetErrorHandler(std::function<void(const RendererResult&)> callback);
    // 指定 Enter 对应按钮，不包含固定业务 ID。
    RendererResult SetDefaultAction(std::string_view id);
    // 检查创建窗口所需的所有绑定。
    RendererResult ValidateBindings() const;
    // 校验后创建窗口，失败回收本次资源。
    RendererResult Show(const RendererWindowOptions& options = {});
    // 同步模态显示并恢复原启用 owner；线程消息 hook 返回是否消费消息，WM_QUIT 原码重投。
    RendererResult ShowModal(const RendererWindowOptions& options = {},
                             std::function<bool(MSG&)> processThreadMessage = {});
    // 重读草稿和动态选项，不触发修改回调。
    RendererResult RefreshValues();
    // 重读全部本地化文字并重新布局。
    RendererResult RefreshTexts();
    // 改变一个控件的宿主可用状态。
    RendererResult SetEnabled(std::string_view id, bool enabled);
    // 临时禁止全部交互及关闭，解除后恢复单项状态。
    RendererResult SetBusy(bool busy);
    // 返回当前页面 ID；未加载页面时为空。
    std::string GetActivePageId() const;
    // 返回控件所属页面，未知 ID 或底部控件返回空。
    std::string GetControlPageId(std::string_view id) const;
    // 设置字段错误，错误文字由宿主本地化。
    RendererResult SetFieldError(std::string_view id, std::wstring text);
    // 设置窗口底部状态，文字由宿主本地化。
    RendererResult SetStatus(std::wstring text);
    // 延迟关闭，保证当前事件分派完成。
    RendererResult RequestClose();
    // 处理非模态键盘导航；下拉框展开时先交原生控件。
    bool ProcessDialogMessage(MSG& message);
    // 返回借用 HWND，仅在窗口存活期间有效。
    HWND NativeHandle() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace open_st
