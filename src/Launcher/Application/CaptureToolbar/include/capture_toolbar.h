// 声明截图工具栏按钮、状态、命令与窗口接口，通过回调连接宿主业务。

#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace open_st
{
enum class CaptureToolbarCommand : std::uint32_t
{
    Cancel = 1,
    Save,
    Copy
};
enum class ToolbarIcon
{
    Cancel,
    Save,
    Copy
};

struct ToolbarButtonSpec
{
    CaptureToolbarCommand command;
    ToolbarIcon icon;
    std::string tooltipKey;
    std::uint32_t group;
};

struct ToolbarButtonState
{
    CaptureToolbarCommand command;
    bool visible{true};
    bool enabled{true};
    bool checked{false};
};

struct ToolbarResult
{
    bool success{false};
    // 仅供日志诊断，不得直接显示到 UI；用户提示须由宿主通过本地化入口取得。
    std::wstring error;
};

// 所有方法和回调仅在所属 UI 线程调用；回调只投递消息，不同步销毁工具栏。
class CaptureToolbar final
{
  public:
    using TextResolver = std::function<std::wstring(std::string_view)>;
    using CommandHandler = std::function<bool(CaptureToolbarCommand, std::uint64_t)>;

    // 创建尚未绑定窗口的工具栏对象。
    // 入参：无。
    // 返回：构造函数无返回值；分配内部状态，后续由 Create 注入按钮和回调。
    CaptureToolbar();
    // 销毁工具栏并解除回调及窗口资源。
    // 入参：无。
    // 返回：析构函数无返回值；执行幂等 Close 清理。
    ~CaptureToolbar();
    // 禁止复制构造，确保工具栏窗口和宿主回调只由原对象管理。
    // 入参：未命名的同类型 const 引用：拟复制的源对象。
    // 返回：函数已删除，调用会导致编译错误，无运行时返回结果。
    CaptureToolbar(const CaptureToolbar&) = delete;
    // 禁止复制赋值，避免工具栏窗口和宿主回调出现多个所有者。
    // 入参：未命名的同类型 const 引用：拟复制的源对象。
    // 返回：函数已删除，调用会导致编译错误，无运行时返回结果。
    CaptureToolbar& operator=(const CaptureToolbar&) = delete;

    // 创建不抢焦点的截图工具栏及按钮、提示窗口。
    // 入参：instance：借用的进程模块句柄；owner：工具栏所属覆盖窗口；buttons：按显示顺序移入的按钮描述；textResolver：文本键查询回调；onCommand：接收命令及代次的提交回调。
    // 返回：ToolbarResult：成功时 success 为 true；失败时为 false，error 仅用于日志诊断。
    ToolbarResult Create(HINSTANCE instance, HWND owner, std::vector<ToolbarButtonSpec> buttons,
                         TextResolver textResolver, CommandHandler onCommand);
    // 按选区和目标显示器工作区定位工具栏。
    // 入参：selection、workArea：虚拟桌面物理像素矩形；dpi：目标显示器 DPI。
    // 返回：布局及窗口定位成功时 success 为 true；无有效窗口或布局失败时为 false 并附诊断。
    ToolbarResult UpdatePlacement(RECT selection, RECT workArea, UINT dpi);
    // 以指定选区代次显示工具栏并使其位于覆盖窗口上方。
    // 入参：sessionToken：非零的当前选区代次；新代次解除旧提交锁。
    // 返回：显示成功或全部按钮隐藏时 success 为 true；窗口、代次或布局无效时为 false 并附诊断。
    ToolbarResult Show(std::uint64_t sessionToken);
    // 暂时隐藏工具栏及提示并撤销当前鼠标交互。
    // 入参：无。
    // 返回：无返回值；保留窗口资源和业务忙状态，供后续再次显示。
    void Hide() noexcept;
    // 同步截图完成流程的忙状态并刷新按钮可用性。
    // 入参：busy：true 表示业务处理中；false 表示解除忙状态和本次提交锁。
    // 返回：无返回值；清理积压鼠标交互后重新应用按钮状态。
    void SetBusy(bool busy) noexcept;
    // 校验并应用按稳定命令 ID 指定的部分按钮状态更新。
    // 入参：states：本次要更新的可见、启用及选中状态列表，仅在调用期间借用。
    // 返回：成功时 success 为 true，已经定位过的工具栏会重新布局；校验、定位或显示失败时为 false 并附诊断。
    ToolbarResult UpdateButtonStates(std::span<const ToolbarButtonState> states);
    // 重新查询当前语言的工具栏名称和按钮提示文本。
    // 入参：无。
    // 返回：文本解析成功并提交窗口更新请求时 success 为 true；窗口或解析器缺失、解析异常时 false 并附诊断。
    ToolbarResult RefreshTexts();
    // 关闭工具栏并撤销与宿主之间的回调连接。
    // 入参：无。
    // 返回：无返回值；先解除回调再释放提示和按钮窗口，可重复调用。
    void Close() noexcept;
    // 向宿主提供工具栏原生窗口以便协调或测试。
    // 入参：无。
    // 返回：工具栏窗口的借用 HWND；尚未创建或已关闭时为 nullptr，不转移所有权。
    HWND NativeHandle() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace open_st
