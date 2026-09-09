#include "capture_overlay_session.h"

namespace open_st
{
// 所有输出窗口覆盖自身捕获屏幕，无匹配输出时返回空句柄。
HWND CaptureOverlaySession::WindowForMonitor(HMONITOR monitor) const noexcept
{
    for (const std::unique_ptr<CaptureOverlayOutput>& output : this->outputs_)
    {
        if (output->window != nullptr && MonitorFromWindow(output->window, MONITOR_DEFAULTTONULL) == monitor)
        {
            return output->window;
        }
    }
    return nullptr;
}

// 即使初始化中途失败，也释放已经登记的隐藏窗口。
CaptureOverlaySession::~CaptureOverlaySession()
{
    this->Close();
}

// 记录对象单独分配，后续 vector 扩容不会使回调中的记录地址失效。
CaptureOverlayOutput& CaptureOverlaySession::Add(HWND window)
{
    std::unique_ptr<CaptureOverlayOutput> output = std::make_unique<CaptureOverlayOutput>();
    output->window = window;
    this->outputs_.push_back(std::move(output));
    return *this->outputs_.back();
}

// 线性查询数量很少的物理显示输出，不持有窗口之外的系统状态。
CaptureOverlayOutput* CaptureOverlaySession::Find(HWND window) const noexcept
{
    for (const std::unique_ptr<CaptureOverlayOutput>& output : this->outputs_)
    {
        if (output->window == window)
        {
            return output.get();
        }
    }
    return nullptr;
}

// 以物理屏幕坐标匹配激活窗口，不依赖主显示器或 vector 的枚举顺序。
HWND CaptureOverlaySession::ActivationWindow() const noexcept
{
    POINT point{};
    if (GetCursorPos(&point) != FALSE)
    {
        for (const std::unique_ptr<CaptureOverlayOutput>& output : this->outputs_)
        {
            RECT rectangle{};
            if (GetWindowRect(output->window, &rectangle) != FALSE && PtInRect(&rectangle, point) != FALSE)
            {
                return output->window;
            }
        }
    }
    return this->outputs_.empty() ? nullptr : this->outputs_.front()->window;
}

// 每个窗口绘制同一个最新快照，只裁剪自己所覆盖的物理屏幕区域。
void CaptureOverlaySession::Invalidate() const noexcept
{
    for (const std::unique_ptr<CaptureOverlayOutput>& output : this->outputs_)
    {
        InvalidateRect(output->window, nullptr, FALSE);
    }
}

// 不干扰其他应用或设置窗口持有的鼠标捕获。
void CaptureOverlaySession::ReleaseMouse() const noexcept
{
    if (GetCapture() != nullptr && this->Find(GetCapture()) != nullptr)
    {
        ReleaseCapture();
    }
}

// 先显示所有已预绘制的交换链，再把键盘焦点交给光标所在显示器。
void CaptureOverlaySession::Show() const noexcept
{
    for (const std::unique_ptr<CaptureOverlayOutput>& output : this->outputs_)
    {
        ShowWindow(output->window, SW_SHOWNOACTIVATE);
    }
    const HWND activeWindow = this->ActivationWindow();
    SetForegroundWindow(activeWindow);
    SetFocus(activeWindow);
    this->Invalidate();
}

// 模态对话框结束后恢复冻结选区的焦点，保持现有几何和像素。
void CaptureOverlaySession::RestoreFocus() const noexcept
{
    const HWND window = this->ActivationWindow();
    if (window != nullptr)
    {
        SetForegroundWindow(window);
        SetFocus(window);
        this->Invalidate();
    }
}

// 全部标题使用 App 提供的同一本地化文本。
void CaptureOverlaySession::SetTitle(const std::wstring& title) const noexcept
{
    for (const std::unique_ptr<CaptureOverlayOutput>& output : this->outputs_)
    {
        SetWindowTextW(output->window, title.c_str());
    }
}

// 先断开所有 HWND 到 App 的关联，再释放鼠标和窗口，避免 WM_DESTROY 递归关闭会话。
void CaptureOverlaySession::Close() noexcept
{
    for (const std::unique_ptr<CaptureOverlayOutput>& output : this->outputs_)
    {
        SetWindowLongPtrW(output->window, GWLP_USERDATA, 0);
    }
    this->ReleaseMouse();
    for (const std::unique_ptr<CaptureOverlayOutput>& output : this->outputs_)
    {
        output->renderer.reset();
        if (IsWindow(output->window) != FALSE)
        {
            DestroyWindow(output->window);
        }
    }
    this->outputs_.clear();
}
} // namespace open_st
