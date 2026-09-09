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
    std::wstring error;
};

// 所有方法和回调仅在所属 UI 线程调用；回调只投递消息，不同步销毁工具栏。
class CaptureToolbar final
{
  public:
    using TextResolver = std::function<std::wstring(std::string_view)>;
    using CommandHandler = std::function<bool(CaptureToolbarCommand, std::uint64_t)>;

    // 构造空工具栏，稍后由 Create 注入 UI 所有者及回调。
    CaptureToolbar();
    // 析构时关闭自身窗口并解除所有业务回调。
    ~CaptureToolbar();
    // 窗口及回调不能复制到另一实例。
    CaptureToolbar(const CaptureToolbar&) = delete;
    // 禁止通过赋值共享窗口所有权。
    CaptureToolbar& operator=(const CaptureToolbar&) = delete;

    // 验证有序按钮列表并创建无激活窗口，失败返回明确原因。
    ToolbarResult Create(HINSTANCE instance, HWND owner, std::vector<ToolbarButtonSpec> buttons,
                         TextResolver textResolver, CommandHandler onCommand);
    // 接收虚拟桌面物理矩形及目标屏幕 DPI，不借用图形模块类型。
    ToolbarResult UpdatePlacement(RECT selection, RECT workArea, UINT dpi);
    // 按有效代次显示；新代次清除过期的待处理请求。
    ToolbarResult Show(std::uint64_t sessionToken);
    // 隐藏工具栏与提示并释放鼠标捕获，保留业务忙状态。
    void Hide() noexcept;
    // false 同时解除一次投递的 pending 锁；App 在命令完成或丢弃后调用。
    void SetBusy(bool busy) noexcept;
    // 原子校验部分状态更新，并重新排列可见按钮及分组。
    ToolbarResult UpdateButtonStates(std::span<const ToolbarButtonState> states);
    // 重取本地化窗口名称和各按钮自有提示字符串。
    ToolbarResult RefreshTexts();
    // 解除回调并释放窗口，重复关闭安全。
    void Close() noexcept;
    // 返回借用窗口句柄，用于所有者协调和测试查询。
    HWND NativeHandle() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace open_st
