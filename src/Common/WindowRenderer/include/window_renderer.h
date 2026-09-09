// 声明通用表单窗口的布局加载、类型化控件绑定及窗口生命周期接口。

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
    // 判断 Renderer 操作结果是否成功。
    // 入参：无。
    // 返回：code 为空时为 true，非空时为 false；不读取错误文本或控件 ID。
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
struct RendererBoolResult
{
    bool success = true;
    bool value = false;
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
    // 创建通用窗口渲染器并固定所属 UI 线程。
    // 入参：无。
    // 返回：构造函数无返回值；尚未创建窗口或加载布局，内部存储分配失败可抛出异常。
    WindowRenderer();
    // 停止窗口分派并释放渲染器拥有的窗口、字体及注册回调。
    // 入参：无。
    // 返回：析构函数无返回值；宿主借用的 HWND 在销毁后失效。
    ~WindowRenderer();
    // 禁止复制窗口渲染器的 HWND、字体及回调所有权。
    // 入参：未命名 const WindowRenderer 引用：拟复制的源渲染器。
    // 返回：无；函数已删除，调用会导致编译错误。
    WindowRenderer(const WindowRenderer&) = delete;
    // 禁止复制窗口渲染器的 HWND、字体及回调所有权。
    // 入参：未命名 const WindowRenderer 引用：拟复制的源渲染器。
    // 返回：无；函数已删除，调用会导致编译错误。
    WindowRenderer& operator=(const WindowRenderer&) = delete;
    // 解析并复制完整窗口布局，为控件绑定建立索引。
    // 入参：document：调用期间借用的布局 JSON。
    // 返回：成功返回空错误码并清空旧绑定；解析或状态检查失败返回结构化错误，保留之前布局。
    RendererResult LoadLayout(const nlohmann::json& document);
    // 注册窗口所有文本键使用的本地化查询器。
    // 入参：callback：接收文本键并返回宽字符文本的回调，移入渲染器；其捕获对象须保持存活。
    // 返回：成功时返回空错误码的 RendererResult；失败返回含错误码、路径或控件 ID 的结构化结果；须在显示前注册，空回调或重复绑定被拒绝。
    RendererResult SetTextResolver(std::function<std::wstring(std::string_view)> callback);
    // 把下拉框连接到宿主字符串草稿的读取和变更操作。
    // 入参：id：布局中的下拉框 ID；read：返回当前字符串及读取结果的回调；change：接收拟选字符串并返回是否接受的回调；回调移入渲染器。
    // 返回：成功时返回空错误码的 RendererResult；失败返回含错误码、路径或控件 ID 的结构化结果；非下拉框、空回调或重复绑定被拒绝，拒绝变更时恢复已接受值。
    RendererResult BindString(std::string_view id, std::function<RendererStringResult()> read,
                              std::function<RendererChangeResult(std::string_view)> change);
    // 把复选框连接到宿主布尔草稿的读取和变更操作。
    // 入参：id：布局中的复选框 ID；read：读取当前布尔值的回调；change：接收新布尔值并返回是否接受的回调；回调移入渲染器。
    // 返回：成功时返回空错误码的 RendererResult；失败返回含错误码、路径或控件 ID 的结构化结果；类型不符、空回调或重复绑定被拒绝。
    RendererResult BindBool(std::string_view id, std::function<RendererBoolResult()> read,
                            std::function<RendererChangeResult(bool)> change);
    // 为下拉框注册动态选项查询。
    // 入参：id：布局中的下拉框 ID；query：返回稳定值和显示名称列表的回调，移入渲染器。
    // 返回：成功时返回空错误码的 RendererResult；失败返回含错误码、路径或控件 ID 的结构化结果；选项值唯一性在实际读取选项时检查。
    RendererResult BindOptions(std::string_view id, std::function<RendererOptionsResult()> query);
    // 将布局按钮绑定到宿主业务动作。
    // 入参：id：布局中的按钮 ID；callback：按钮触发时同步调用的无参动作，移入渲染器。
    // 返回：成功时返回空错误码的 RendererResult；失败返回含错误码、路径或控件 ID 的结构化结果；非按钮、空回调或重复绑定被拒绝。
    RendererResult BindAction(std::string_view id, std::function<void()> callback);
    // 把标题栏关闭及 Esc 操作交由宿主决定如何处理。
    // 入参：callback：无参关闭请求回调，移入渲染器；需要退出时由宿主调用 RequestClose。
    // 返回：成功时返回空错误码的 RendererResult；失败返回含错误码、路径或控件 ID 的结构化结果；须在显示前注册，空回调或重复绑定被拒绝。
    RendererResult SetCloseHandler(std::function<void()> callback);
    // 为控件事件错误注册宿主接收器。
    // 入参：callback：借用 RendererResult 处理结构化错误的回调，移入渲染器；宿主负责本地化。
    // 返回：成功时返回空错误码的 RendererResult；失败返回含错误码、路径或控件 ID 的结构化结果；须在显示前注册，空回调或重复绑定被拒绝。
    RendererResult SetErrorHandler(std::function<void(const RendererResult&)> callback);
    // 指定未被原生控件消费的 Enter 键所触发的按钮。
    // 入参：id：布局中的按钮 ID，复制到渲染器。
    // 返回：成功时返回空错误码的 RendererResult；失败返回含错误码、路径或控件 ID 的结构化结果；非按钮或重复指定被拒绝。
    RendererResult SetDefaultAction(std::string_view id);
    // 检查布局和全部必需回调是否满足窗口创建条件。
    // 入参：无。
    // 返回：布局及绑定完整时返回空错误码；缺失或无效时返回定位到对应控件的结构化错误。
    RendererResult ValidateBindings() const;
    // 校验绑定并创建和显示非模态原生窗口。
    // 入参：options：借用的 owner 和 icon，以及初始显示命令；图标由宿主保持存活。
    // 返回：成功时返回空错误码的 RendererResult；失败返回含错误码、路径或控件 ID 的结构化结果；创建过程失败会回收本次窗口资源。
    RendererResult Show(const RendererWindowOptions& options = {});
    // 创建并同步运行模态窗口，暂时禁用原本启用的所属窗口。
    // 入参：options：借用所属窗口、图标及显示命令；processThreadMessage：可选线程消息回调，返回 true 表示已消费消息。
    // 返回：正常关闭或收到 WM_QUIT 时返回空错误码；创建、消息循环或回调失败返回结构化错误；恢复本次禁用的 owner，并原码重投 WM_QUIT。
    RendererResult ShowModal(const RendererWindowOptions& options = {},
                             std::function<bool(MSG&)> processThreadMessage = {});
    // 重读控件草稿及动态选项，使显示值与宿主当前状态一致。
    // 入参：无。
    // 返回：成功时返回空错误码的 RendererResult；失败返回含错误码、路径或控件 ID 的结构化结果；不调用变更回调，结构化刷新错误停止本次刷新，业务读取失败显示控件错误；已刷新的控件不回滚。
    RendererResult RefreshValues();
    // 重新查询全部本地化文字并调整窗口布局。
    // 入参：无。
    // 返回：文本查询及更新请求完成时返回空错误码；状态无效或回调异常返回结构化错误；保留原生控件、焦点和草稿。
    RendererResult RefreshTexts();
    // 设置单个控件的宿主可用状态。
    // 入参：id：目标控件 ID；enabled：true 允许交互，false 禁用。
    // 返回：成功时返回空错误码的 RendererResult；失败返回含错误码、路径或控件 ID 的结构化结果；窗口忙时控件仍禁用，解除忙状态后恢复该值。
    RendererResult SetEnabled(std::string_view id, bool enabled);
    // 临时禁止窗口交互和关闭请求，结束后恢复各控件状态。
    // 入参：busy：true 进入忙状态，false 恢复交互。
    // 返回：成功时返回空错误码的 RendererResult；失败返回含错误码、路径或控件 ID 的结构化结果；解除时尝试恢复之前仍有效且可用的焦点控件。
    RendererResult SetBusy(bool busy);
    // 向宿主提供当前页面的业务标识。
    // 入参：无。
    // 返回：当前页面 ID 的字符串副本；尚未加载布局时返回空字符串。
    std::string GetActivePageId() const;
    // 查询某控件在当前布局中所属的页面。
    // 入参：id：待查询的控件 ID。
    // 返回：所属页面 ID 的副本；未知控件或底部公共控件返回空字符串。
    std::string GetControlPageId(std::string_view id) const;
    // 在输入控件附近显示宿主提供的字段错误。
    // 入参：id：下拉框或复选框 ID；text：已本地化的错误文字，移入控件状态，空串用于清除。
    // 返回：成功时返回空错误码的 RendererResult；失败返回含错误码、路径或控件 ID 的结构化结果；未知 ID 或非输入控件被拒绝，错误参与重新布局。
    RendererResult SetFieldError(std::string_view id, std::wstring text);
    // 设置窗口底部的公共状态文字。
    // 入参：text：宿主已本地化的状态文本，移入渲染器，空串用于清除。
    // 返回：成功时返回空错误码的 RendererResult；失败返回含错误码、路径或控件 ID 的结构化结果；更新已存在的状态控件并重新计算布局。
    RendererResult SetStatus(std::wstring text);
    // 投递延迟关闭请求，避免销毁正在执行的事件回调。
    // 入参：无。
    // 返回：投递成功或窗口已不存在时返回空错误码；忙状态、线程检查或投递失败返回结构化错误，成功不表示窗口已同步销毁。
    RendererResult RequestClose();
    // 为窗口处理 Enter、Esc 和 Tab 等键盘导航消息。
    // 入参：message：待处理的 Win32 消息引用，所属窗口及子控件消息才参与处理。
    // 返回：消息被消费时为 true；无有效窗口、消息不属于本窗口或下拉框应先自行处理时为 false。
    bool ProcessDialogMessage(MSG& message);
    // 向宿主提供用于窗口协调的原生句柄。
    // 入参：无。
    // 返回：当前 HWND 的借用值；尚未创建或已销毁时为 nullptr，宿主不得据此取得销毁所有权。
    HWND NativeHandle() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace open_st
