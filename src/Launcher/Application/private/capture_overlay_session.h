#pragma once

#include <overlay_renderer.h>

#include <memory>
#include <string>
#include <vector>
#include <windows.h>

namespace open_st
{
// 每屏窗口仅拥有呈现资源；原生冻结帧和全局选区由 App 统一拥有。
struct CaptureOverlayOutput final
{
    HWND window{};
    std::unique_ptr<OverlayRenderer> renderer;
};

// 管理同一截图会话的多个 HWND；销毁前解除回调绑定，避免重复释放共享状态。
class CaptureOverlaySession final
{
  public:
    // 创建空会话，窗口在捕获完成后逐一登记。
    CaptureOverlaySession() = default;
    // 关闭已登记的全部窗口和呈现资源。
    ~CaptureOverlaySession();
    // 禁止复制窗口所有权。
    CaptureOverlaySession(const CaptureOverlaySession&) = delete;
    // 禁止复制赋值，防止重复销毁 HWND。
    CaptureOverlaySession& operator=(const CaptureOverlaySession&) = delete;
    // 登记尚未显示的窗口，并返回其稳定的记录地址。
    CaptureOverlayOutput& Add(HWND window);
    // 按 HWND 找到当前输出；不是本会话的窗口时返回 nullptr。
    [[nodiscard]] CaptureOverlayOutput* Find(HWND window) const noexcept;
    // 返回光标所在输出窗口；查询失败时退回首个窗口。
    [[nodiscard]] HWND ActivationWindow() const noexcept;
    // 按监视器借用本会话窗口，为工具栏提供目标输出的实际窗口 DPI。
    [[nodiscard]] HWND WindowForMonitor(HMONITOR monitor) const noexcept;
    // 标记全部显示输出重绘，保证共享选区跨屏同步。
    void Invalidate() const noexcept;
    // 仅释放属于本截图会话的鼠标捕获。
    void ReleaseMouse() const noexcept;
    // 所有隐藏窗口准备成功后统一显示，并激活光标所在窗口。
    void Show() const noexcept;
    // 从模态操作恢复选区窗口的前台与键盘焦点。
    void RestoreFocus() const noexcept;
    // 刷新全部覆盖窗口的本地化标题。
    void SetTitle(const std::wstring& title) const noexcept;
    // 清理前先解除所有窗口回调绑定，使同步销毁消息不能重入 App。
    void Close() noexcept;

  private:
    std::vector<std::unique_ptr<CaptureOverlayOutput>> outputs_;
};
} // namespace open_st
