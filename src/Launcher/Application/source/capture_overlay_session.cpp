// 管理多屏截图覆盖窗口的登记、显示、焦点恢复与统一解绑销毁。

#include "capture_overlay_session.h"

namespace open_st
{
// 查找指定显示器对应的截图覆盖窗口。
// 入参：monitor：目标显示器句柄。
// 返回：匹配窗口的借用句柄；未找到时返回 nullptr，不转移窗口所有权。
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

// 销毁截图覆盖会话并释放全部已登记窗口和渲染资源。
// 入参：无。
// 返回：析构函数无返回值。
CaptureOverlaySession::~CaptureOverlaySession()
{
    this->Close();
}

// 把截图覆盖窗口交给当前会话统一管理。
// 入参：window：交由会话负责销毁的窗口句柄。
// 返回：新增输出记录的引用；会话关闭前地址稳定，不因后续登记而移动。
CaptureOverlayOutput& CaptureOverlaySession::Add(HWND window)
{
    std::unique_ptr<CaptureOverlayOutput> output = std::make_unique<CaptureOverlayOutput>();
    output->window = window;
    this->outputs_.push_back(std::move(output));
    return *this->outputs_.back();
}

// 查找窗口在当前截图会话中的输出记录。
// 入参：window：待查找的截图覆盖窗口句柄。
// 返回：匹配记录的借用指针；不存在时返回 nullptr。
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

// 选择应接收截图键盘输入的覆盖窗口。
// 入参：无。
// 返回：光标所在覆盖窗口的借用句柄；查询失败或未命中时取首个窗口，空会话返回 nullptr。
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

// 请求当前会话的所有覆盖窗口重绘，使跨屏选区显示保持同步。
// 入参：无。
// 返回：无返回值；仅标记重绘区域，实际绘制由窗口消息驱动。
void CaptureOverlaySession::Invalidate() const noexcept
{
    for (const std::unique_ptr<CaptureOverlayOutput>& output : this->outputs_)
    {
        InvalidateRect(output->window, nullptr, FALSE);
    }
}

// 解除当前截图会话占用的鼠标捕获。
// 入参：无。
// 返回：无返回值；其他窗口持有的鼠标捕获保持不变。
void CaptureOverlaySession::ReleaseMouse() const noexcept
{
    if (GetCapture() != nullptr && this->Find(GetCapture()) != nullptr)
    {
        ReleaseCapture();
    }
}

// 显示已准备好的全部截图覆盖窗口，并激活光标所在窗口。
// 入参：无。
// 返回：无返回值；请求前台、键盘焦点及重绘，不报告系统拒绝激活的结果。
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

// 在模态操作结束后恢复截图覆盖窗口的焦点及选区显示。
// 入参：无。
// 返回：无返回值；无可激活窗口时不操作。
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

// 更新当前会话全部截图覆盖窗口的标题。
// 入参：title：由宿主提供的本地化标题，本次调用借用。
// 返回：无返回值；标题写入各窗口，不保存字符串引用。
void CaptureOverlaySession::SetTitle(const std::wstring& title) const noexcept
{
    for (const std::unique_ptr<CaptureOverlayOutput>& output : this->outputs_)
    {
        SetWindowTextW(output->window, title.c_str());
    }
}

// 关闭整个截图覆盖会话，先解绑窗口回调再释放渲染器和窗口。
// 入参：无。
// 返回：无返回值；清空输出记录，可重复调用。
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
