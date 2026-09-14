// 管理多屏截图覆盖窗口的登记、显示、焦点恢复与统一解绑销毁。

#include "capture_overlay_session.h"

#include <limits>
#include <stdexcept>
#include <type_traits>

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
    if (this->painting_ || this->closePending_)
    {
        throw std::logic_error("Cannot add an output during overlay painting or closing.");
    }
    if (window != nullptr && this->Find(window) != nullptr)
    {
        throw std::logic_error("Cannot register an overlay window twice.");
    }
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
    if (window == nullptr)
    {
        return nullptr;
    }
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
void CaptureOverlaySession::Invalidate() noexcept
{
    if (this->closePending_)
    {
        return;
    }
    if (this->requestGeneration_ == std::numeric_limits<std::uint64_t>::max())
    {
        this->invalidationFailed_ = true;
        return;
    }
    ++this->requestGeneration_;
    for (const std::unique_ptr<CaptureOverlayOutput>& output : this->outputs_)
    {
        if (output->window != nullptr)
        {
            output->redrawPending_ = true;
            if (IsWindow(output->window) == FALSE || InvalidateRect(output->window, nullptr, FALSE) == FALSE)
            {
                this->invalidationFailed_ = true;
            }
        }
    }
}

// 消费触发窗口的绘制消息，并在固定批次内绘制所有待更新输出。
// 入参：trigger 为收到 WM_PAINT 的窗口；drawOutput 为同步绘制边界；shouldStop 检查宿主失效；error 接收失败原因。
// 返回：无触发或重入为 Skipped；成功为 Completed；关闭或宿主失效为 Interrupted；窗口或绘制失败为 Failed。
OverlayPaintResult CaptureOverlaySession::Paint(
    HWND trigger, const std::function<bool(CaptureOverlayOutput&, std::wstring&)>& drawOutput,
    const std::function<bool()>& shouldStop, std::wstring& error)
{
    error.clear();
    if (trigger == nullptr)
    {
        return OverlayPaintResult::Skipped;
    }
    CaptureOverlayOutput* triggeringOutput = this->Find(trigger);
    if (triggeringOutput == nullptr || IsWindow(trigger) == FALSE)
    {
        error = L"截图绘制窗口已失效或不属于当前会话。";
        return OverlayPaintResult::Failed;
    }
    if (this->painting_)
    {
        triggeringOutput->redrawPending_ = true;
        if (this->requestGeneration_ == std::numeric_limits<std::uint64_t>::max())
        {
            this->invalidationFailed_ = true;
        }
        else
        {
            ++this->requestGeneration_;
        }
        PAINTSTRUCT paint{};
        const HDC context = BeginPaint(trigger, &paint);
        const BOOL ended = EndPaint(trigger, &paint);
        if (context == nullptr || ended == FALSE)
        {
            this->invalidationFailed_ = true;
            error = L"无法消费重入的截图绘制请求。";
            return OverlayPaintResult::Failed;
        }
        return OverlayPaintResult::Skipped;
    }

    this->painting_ = true;
    OverlayPaintResult result = OverlayPaintResult::Completed;
    try
    {
        const std::uint64_t batchGeneration = this->requestGeneration_;
        PAINTSTRUCT paint{};
        const HDC context = BeginPaint(trigger, &paint);
        const BOOL ended = EndPaint(trigger, &paint);
        if (context == nullptr || ended == FALSE)
        {
            throw std::runtime_error("Cannot consume the overlay paint message.");
        }
        if (this->closePending_ || (shouldStop && shouldStop()) || this->closePending_)
        {
            (void)this->FinishPaint();
            return OverlayPaintResult::Interrupted;
        }
        struct PendingOutput final
        {
            CaptureOverlayOutput* output{};
            HWND window{};
            DWORD thread{};
            DWORD process{};
            LONG_PTR binding{};

            // 检查固定批次记录仍指向同一个存活窗口及其原绑定。
            // 入参：无。
            // 返回：HWND、进程、线程和回调绑定均未变化时 true。
            bool IsCurrent() const noexcept
            {
                if (this->output->window != this->window || IsWindow(this->window) == FALSE)
                {
                    return false;
                }
                DWORD currentProcess{};
                const DWORD currentThread = GetWindowThreadProcessId(this->window, &currentProcess);
                SetLastError(ERROR_SUCCESS);
                const LONG_PTR currentBinding = GetWindowLongPtrW(this->window, GWLP_USERDATA);
                const DWORD bindingError = GetLastError();
                return currentThread == this->thread && currentProcess == this->process &&
                       currentBinding == this->binding && (currentBinding != 0 || bindingError == ERROR_SUCCESS);
            }
        };
        std::vector<PendingOutput> batch;
        batch.reserve(this->outputs_.size());
        for (const std::unique_ptr<CaptureOverlayOutput>& output : this->outputs_)
        {
            if (output->window == nullptr)
            {
                continue;
            }
            if (IsWindow(output->window) == FALSE)
            {
                throw std::runtime_error("Invalid overlay window.");
            }
            // GetUpdateRgn 可区别空区域与 API 错误；不请求擦除，不在收集期间同步绘制。
            using RegionHandle = std::unique_ptr<std::remove_pointer_t<HRGN>, decltype(&DeleteObject)>;
            RegionHandle region(CreateRectRgn(0, 0, 0, 0), &DeleteObject);
            if (!region)
            {
                throw std::runtime_error("Cannot allocate an update region.");
            }
            const int regionKind = GetUpdateRgn(output->window, region.get(), FALSE);
            const BOOL deleted = DeleteObject(region.get());
            if (deleted != FALSE)
            {
                (void)region.release();
            }
            if (regionKind == ERROR || deleted == FALSE)
            {
                throw std::runtime_error("Cannot query an overlay update region.");
            }
            if (output->window == trigger || output->redrawPending_ || regionKind != NULLREGION)
            {
                PendingOutput pending;
                pending.output = output.get();
                pending.window = output->window;
                pending.thread = GetWindowThreadProcessId(output->window, &pending.process);
                SetLastError(ERROR_SUCCESS);
                pending.binding = GetWindowLongPtrW(output->window, GWLP_USERDATA);
                if (pending.thread == 0 || (pending.binding == 0 && GetLastError() != ERROR_SUCCESS))
                {
                    throw std::runtime_error("Cannot identify an overlay window.");
                }
                for (const PendingOutput& existing : batch)
                {
                    if (existing.window == pending.window)
                    {
                        throw std::runtime_error("Duplicate overlay window identity.");
                    }
                }
                batch.push_back(pending);
            }
        }
        for (const PendingOutput& pending : batch)
        {
            // BeginPaint 可能同步重入；代次变化时保留那段调用期间新增的内部失效。
            if (this->requestGeneration_ == batchGeneration)
            {
                pending.output->redrawPending_ = false;
            }
        }
        // 全批验证必须早于第一次绘制；回调期间的新系统区域不在返回后清除。
        for (const PendingOutput& pending : batch)
        {
            if (!pending.IsCurrent() || ValidateRect(pending.window, nullptr) == FALSE)
            {
                throw std::runtime_error("Cannot validate an overlay update region.");
            }
        }
        if (this->invalidationFailed_)
        {
            throw std::runtime_error("An overlay invalidation request failed.");
        }
        for (const PendingOutput& pending : batch)
        {
            if (this->closePending_ || (shouldStop && shouldStop()) || this->closePending_)
            {
                result = OverlayPaintResult::Interrupted;
                break;
            }
            if (!pending.IsCurrent())
            {
                error = L"截图批次中的窗口身份发生变化。";
                result = OverlayPaintResult::Failed;
                break;
            }
            const bool drawn = drawOutput && drawOutput(*pending.output, error);
            if (this->closePending_ || (shouldStop && shouldStop()) || this->closePending_)
            {
                result = OverlayPaintResult::Interrupted;
                break;
            }
            if (!drawn)
            {
                if (error.empty())
                {
                    error = L"截图输出绘制失败。";
                }
                result = OverlayPaintResult::Failed;
                break;
            }
            if (!pending.IsCurrent())
            {
                error = L"截图输出在绘制期间被销毁。";
                result = OverlayPaintResult::Failed;
                break;
            }
        }
        if (this->requestGeneration_ != batchGeneration && this->invalidationFailed_)
        {
            result = OverlayPaintResult::Failed;
            error = L"截图批次中的新增绘制请求失败。";
        }
    }
    catch (...)
    {
        result = OverlayPaintResult::Failed;
        try
        {
            error = L"截图绘制批次发生窗口操作或回调异常。";
        }
        catch (...)
        {
            (void)this->FinishPaint();
            throw;
        }
    }
    if (!this->FinishPaint() && result == OverlayPaintResult::Completed)
    {
        result = OverlayPaintResult::Failed;
        error = L"无法安排截图批次中的后续绘制请求。";
    }
    return result;
}

// 在异常或正常退出批次时恢复准入，并补发批次中新增的无效请求。
// 入参：无。
// 返回：全部有效窗口重新失效成功为 true；关闭请求在此安全回收。
bool CaptureOverlaySession::FinishPaint() noexcept
{
    this->painting_ = false;
    if (this->closePending_)
    {
        this->Close();
        return true;
    }
    bool success = !this->invalidationFailed_;
    for (const std::unique_ptr<CaptureOverlayOutput>& output : this->outputs_)
    {
        if (output->redrawPending_ && output->window != nullptr &&
            (IsWindow(output->window) == FALSE || InvalidateRect(output->window, nullptr, FALSE) == FALSE))
        {
            success = false;
        }
    }
    this->invalidationFailed_ = !success;
    return success;
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
void CaptureOverlaySession::Show() noexcept
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
void CaptureOverlaySession::RestoreFocus() noexcept
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
    if (this->painting_)
    {
        this->closePending_ = true;
        return;
    }
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
    this->closePending_ = false;
    this->invalidationFailed_ = false;
}
} // namespace open_st
