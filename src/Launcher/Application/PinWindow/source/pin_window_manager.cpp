// 管理独立贴图的所有权、固定层级、输入资格及可重入窗口消息期间的延迟销毁。
#include "pin_window.h"
#include <algorithm>
#include <atomic>
#include <limits>
#include <log.h>
#include <pin_image.h>
#include <pin_window_manager.h>
#include <utility>
#include <vector>

namespace open_st
{
class PinWindowManager::Impl final
{
  public:
    HINSTANCE instance{};
    PinWindowCallbacks callbacks;
    // 从底部到顶部保存唯一顺序；焦点变化不修改该集合。
    std::vector<std::unique_ptr<PinWindow>> windows;
    PinId clicked{};
    unsigned int dispatchDepth{};
    unsigned int modalDepth{};
    bool capturing{};
    bool stopping{};
    bool repairing{};
    bool draining{};
};

namespace
{
std::atomic<PinId> nextPinId{1};

// 丢弃暂停期间投递到自有窗口的输入，防止恢复后回放旧点击或快捷键。
// 入参：window 为借用的贴图窗口，不读取其他程序的消息。
// 返回：无返回值；只移除已经在队列中的鼠标和键盘输入。
void DiscardPausedInput(HWND window) noexcept
{
    if (window == nullptr)
        return;
    MSG message{};
    while (PeekMessageW(&message, window, WM_MOUSEFIRST, WM_MOUSELAST, PM_REMOVE))
    {
        if (message.message == WM_QUIT)
        {
            PostQuitMessage(static_cast<int>(message.wParam));
            return;
        }
    }
    while (PeekMessageW(&message, window, WM_KEYFIRST, WM_KEYLAST, PM_REMOVE))
    {
        if (message.message == WM_QUIT)
        {
            PostQuitMessage(static_cast<int>(message.wParam));
            return;
        }
    }
}
} // namespace

// 保存宿主回调，实际窗口和图形资源延迟到准备图像时创建。
// 入参：instance 为模块实例；callbacks 为转移到管理器的文字及命令回调。
// 返回：无返回值；分配失败通过异常报告。
PinWindowManager::PinWindowManager(HINSTANCE instance, PinWindowCallbacks callbacks) : impl_(std::make_unique<Impl>())
{
    this->impl_->instance = instance;
    this->impl_->callbacks = std::move(callbacks);
}

// 在宿主保证没有模态或窗口调用栈后释放全部贴图。
// 入参：无。
// 返回：无返回值；窗口销毁前停止外部回调。
PinWindowManager::~PinWindowManager()
{
    this->Shutdown();
}

// 查找仍由管理器持有的记录，包含等待安全回收的窗口。
// 入参：id 为进程内稳定标识。
// 返回：借用指针；不存在时为空。
PinWindow* PinWindowManager::Find(PinId id) const noexcept
{
    for (const std::unique_ptr<PinWindow>& pin : this->impl_->windows)
        if (pin->Id() == id)
            return pin.get();
    return nullptr;
}

// 构造隐藏候选窗口，首帧完成后才发布到有序集合。
// 入参：image 为独立原图；origin 为物理坐标；id/error 为成功标识与失败诊断输出。
// 返回：完整准备成功为 true；失败不发布窗口且保持 id 为零。
bool PinWindowManager::Prepare(std::shared_ptr<const PinImage> image, POINT origin, PinId& id, std::wstring& error)
{
    id = 0;
    if (this->impl_->stopping || this->impl_->modalDepth != 0 || !image)
    {
        error = L"Pin manager is unavailable or image is missing.";
        return false;
    }
    // 饱和后拒绝新建，避免整数绕回导致旧命令命中新图。
    PinId candidateId = nextPinId.load();
    do
    {
        if (candidateId == (std::numeric_limits<PinId>::max)())
        {
            error = L"Pin identifier space is exhausted.";
            return false;
        }
    } while (!nextPinId.compare_exchange_weak(candidateId, candidateId + 1));
    this->EnterDispatch();
    bool prepared = false;
    try
    {
        std::unique_ptr<PinWindow> candidate = std::make_unique<PinWindow>(*this, candidateId, std::move(image));
        if (candidate->Prepare(this->impl_->instance, origin, error) && !this->impl_->stopping)
        {
            this->impl_->windows.push_back(std::move(candidate));
            id = candidateId;
            prepared = true;
        }
    }
    catch (...)
    {
        error = L"Cannot allocate or prepare pin window resources.";
    }
    this->LeaveDispatch();
    return prepared;
}

// 显示已经准备的窗口，截图暂停期间拒绝显示。
// 入参：id 为稳定标识。
// 返回：无返回值；显示不授予滚轮资格。
void PinWindowManager::Show(PinId id) noexcept
{
    PinWindow* pin = this->Find(id);
    if (!pin || pin->closing || this->impl_->stopping || this->impl_->capturing)
        return;
    this->EnterDispatch();
    ShowWindow(pin->Handle(), SW_SHOWNOACTIVATE);
    this->RepairOrder();
    this->LeaveDispatch();
}

// 查询有效贴图拥有的不可变原图。
// 入参：id 为稳定标识。
// 返回：共享快照；已关闭或不存在时为空。
std::shared_ptr<const PinImage> PinWindowManager::Image(PinId id) const noexcept
{
    const PinWindow* pin = this->Find(id);
    return pin && !pin->closing ? pin->Image() : nullptr;
}

// 查询有效贴图的原生窗口。
// 入参：id 为稳定标识。
// 返回：借用句柄；已申请关闭的窗口不可再被新业务使用。
HWND PinWindowManager::Window(PinId id) const noexcept
{
    const PinWindow* pin = this->Find(id);
    return pin && !pin->closing ? pin->Handle() : nullptr;
}

// 统计未申请关闭的贴图。
// 入参：无。
// 返回：当前可用贴图数量。
std::size_t PinWindowManager::Count() const noexcept
{
    std::size_t count = 0;
    for (const std::unique_ptr<PinWindow>& pin : this->impl_->windows)
        if (!pin->closing)
            ++count;
    return count;
}

// 从顶部向下选择仍可见且未关闭的提示所属窗口。
// 入参：无。
// 返回：借用 HWND；隐藏候选和待关闭窗口不参与选择。
HWND PinWindowManager::ModalOwner() const noexcept
{
    for (auto iterator = this->impl_->windows.rbegin(); iterator != this->impl_->windows.rend(); ++iterator)
        if (!(*iterator)->closing && IsWindowVisible((*iterator)->Handle()))
            return (*iterator)->Handle();
    return nullptr;
}

// 标记单张图失效，实际销毁等待全部受保护调用退出。
// 入参：id 为稳定标识。
// 返回：无返回值；重复关闭无额外影响。
void PinWindowManager::Close(PinId id) noexcept
{
    PinWindow* pin = this->Find(id);
    if (!pin)
        return;
    this->EnterDispatch();
    pin->closing = true;
    pin->CancelDrag();
    this->LoseFocus(id);
    this->LeaveDispatch();
}

// 使全部贴图立即对新请求失效，并延迟回收模态所属窗口。
// 入参：无。
// 返回：无返回值。
void PinWindowManager::CloseAll() noexcept
{
    this->EnterDispatch();
    this->impl_->clicked = 0;
    for (const std::unique_ptr<PinWindow>& pin : this->impl_->windows)
    {
        pin->closing = true;
        pin->CancelDrag();
    }
    this->LeaveDispatch();
}

// 登记窗口操作保护，避免同步消息回收外层仍在访问的记录。
// 入参：无。
// 返回：无返回值；必须与 LeaveDispatch 成对。
void PinWindowManager::EnterDispatch() noexcept
{
    ++this->impl_->dispatchDepth;
}

// 结束一层窗口操作保护，最外层结束后尝试清理。
// 入参：无。
// 返回：无返回值。
void PinWindowManager::LeaveDispatch() noexcept
{
    if (this->impl_->dispatchDepth != 0)
        --this->impl_->dispatchDepth;
    this->DrainClosed();
}

// 先从集合移除待关闭记录再析构，防止销毁消息重入正在修改的容器。
// 入参：无。
// 返回：无返回值；模态、窗口调用或清理重入期间推迟执行。
void PinWindowManager::DrainClosed() noexcept
{
    if (this->IsBusy() || this->impl_->draining)
        return;
    this->impl_->draining = true;
    bool removed = false;
    for (std::size_t index = 0; index < this->impl_->windows.size();)
    {
        if (!this->impl_->windows[index]->closing)
        {
            ++index;
            continue;
        }
        std::unique_ptr<PinWindow> retired = std::move(this->impl_->windows[index]);
        this->impl_->windows.erase(this->impl_->windows.begin() + static_cast<std::ptrdiff_t>(index));
        retired.reset();
        removed = true;
    }
    this->impl_->draining = false;
    if (removed)
        this->RepairOrder();
    if (this->impl_->stopping && this->impl_->windows.empty() && this->impl_->callbacks.stopped)
    {
        std::function<void()> notify = std::move(this->impl_->callbacks.stopped);
        try
        {
            notify();
        }
        catch (...)
        {
            OPEN_ST_LOG_ERROR("Pin shutdown notification failed.");
        }
    }
}

// 判断宿主是否必须继续保留管理器。
// 入参：无。
// 返回：存在模态或受保护窗口调用时为 true。
bool PinWindowManager::IsBusy() const noexcept
{
    return this->impl_->modalDepth != 0 || this->impl_->dispatchDepth != 0;
}

// 禁止新请求并释放宿主回调，已有 owner 等调用栈退出后回收。
// 入参：无。
// 返回：无返回值；退出后不再恢复交互。
void PinWindowManager::Shutdown() noexcept
{
    this->impl_->stopping = true;
    this->impl_->callbacks.text = {};
    this->impl_->callbacks.command = {};
    this->CloseAll();
}
// 判断指定窗口能否开始一次新的用户交互。
// 入参：id 为目标贴图标识。
// 返回：窗口存活、可见且没有截图或模态暂停时为 true。
bool PinWindowManager::CanInteract(PinId id) const noexcept
{
    const PinWindow* pin = this->Find(id);
    return pin && !pin->closing && !this->impl_->stopping && !this->impl_->capturing && this->impl_->modalDepth == 0 &&
           IsWindowVisible(pin->Handle());
}

// 在真实点击之后申请前台和键盘焦点，始终保留业务排序。
// 入参：id 为收到点击的贴图。
// 返回：无返回值；系统拒绝聚焦时不授予滚轮资格。
void PinWindowManager::Click(PinId id) noexcept
{
    if (!this->CanInteract(id))
        return;
    this->EnterDispatch();
    const HWND window = this->Window(id);
    this->impl_->clicked = 0;
    SetForegroundWindow(window);
    SetFocus(window);
    this->RepairOrder();
    if (this->CanInteract(id) && GetForegroundWindow() == window && GetFocus() == window)
        this->impl_->clicked = id;
    this->LeaveDispatch();
}

// 使失焦窗口不再具有滚轮操作资格。
// 入参：id 为失焦贴图。
// 返回：无返回值；不改变其他贴图的资格或排序。
void PinWindowManager::LoseFocus(PinId id) noexcept
{
    if (this->impl_->clicked == id)
        this->impl_->clicked = 0;
}

// 同时核对点击资格、真实焦点和光标最上方命中窗口。
// 入参：id 为滚轮接收贴图；point 为消息中的屏幕物理坐标。
// 返回：全部条件有效时为 true，其他窗口上方的滚轮不会误操作当前图。
bool PinWindowManager::CanWheel(PinId id, POINT point) const noexcept
{
    if (this->impl_->clicked != id || !this->CanInteract(id))
        return false;
    const HWND window = this->Window(id);
    return GetForegroundWindow() == window && GetFocus() == window && WindowFromPoint(point) == window;
}

// 查询业务集合顶部，隐藏窗口仍保留原有相对顺序。
// 入参：id 为待比较标识。
// 返回：最高未关闭记录匹配时为 true。
bool PinWindowManager::IsHighest(PinId id) const noexcept
{
    for (auto iterator = this->impl_->windows.rbegin(); iterator != this->impl_->windows.rend(); ++iterator)
        if (!(*iterator)->closing)
            return (*iterator)->Id() == id;
    return false;
}

// 将一张存活贴图移到集合顶部，其余相对顺序保持。
// 入参：id 为菜单明确要求提升的标识。
// 返回：无返回值；模态、截图或退出期间不接受新提层。
void PinWindowManager::Raise(PinId id) noexcept
{
    if (this->impl_->stopping || this->impl_->capturing || this->impl_->modalDepth != 0)
        return;
    for (auto iterator = this->impl_->windows.begin(); iterator != this->impl_->windows.end(); ++iterator)
    {
        if ((*iterator)->Id() != id || (*iterator)->closing)
            continue;
        std::rotate(iterator, iterator + 1, this->impl_->windows.end());
        this->RepairOrder();
        return;
    }
}

// 在消息边界按唯一业务顺序摆放可见置顶窗口。
// 入参：无。
// 返回：无返回值；同步重入、模态和截图期间不干涉系统排序。
void PinWindowManager::RepairOrder() noexcept
{
    if (this->impl_->repairing || this->impl_->draining || this->impl_->stopping || this->impl_->capturing ||
        this->impl_->modalDepth != 0)
        return;
    this->EnterDispatch();
    this->impl_->repairing = true;
    HWND previous = HWND_TOPMOST;
    for (auto iterator = this->impl_->windows.rbegin(); iterator != this->impl_->windows.rend(); ++iterator)
    {
        const PinWindow& pin = **iterator;
        if (pin.closing || !IsWindowVisible(pin.Handle()))
            continue;
        if (!SetWindowPos(pin.Handle(), previous, 0, 0, 0, 0,
                          SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER))
            OPEN_ST_LOG_ERROR("Cannot restore pin order. pin_id=", pin.Id(), " error=", GetLastError());
        previous = pin.Handle();
    }
    this->impl_->repairing = false;
    this->LeaveDispatch();
}

// 将自动激活引起的提层请求限制到当前排序中的直接前驱之后。
// 入参：id 为目标记录；position 为 WM_WINDOWPOSCHANGING 借用的可改参数。
// 返回：无返回值；不改变位置、尺寸或系统激活语义。
void PinWindowManager::ConstrainOrder(PinId id, WINDOWPOS& position) noexcept
{
    if ((position.flags & SWP_NOZORDER) != 0 || this->impl_->repairing || this->impl_->draining ||
        this->impl_->stopping || this->impl_->capturing || this->impl_->modalDepth != 0)
        return;
    HWND previous = HWND_TOPMOST;
    for (auto iterator = this->impl_->windows.rbegin(); iterator != this->impl_->windows.rend(); ++iterator)
    {
        const PinWindow& pin = **iterator;
        if (pin.closing)
            continue;
        if (pin.Id() == id)
        {
            position.hwndInsertAfter = previous;
            return;
        }
        if (IsWindowVisible(pin.Handle()))
            previous = pin.Handle();
    }
}

// 从宿主取得本次菜单或标题的本地化文字。
// 入参：key 为资源键。
// 返回：解析文字；回调不可用或抛出时为问号。
std::wstring PinWindowManager::Text(std::string_view key) const noexcept
{
    try
    {
        if (this->impl_->callbacks.text)
            return this->impl_->callbacks.text(key);
    }
    catch (...)
    {
        OPEN_ST_LOG_ERROR("Pin text callback failed.");
    }
    return L"?";
}

// 将有效复制保存命令交给宿主异步排队。
// 入参：id 为目标标识；command 为已定义的业务命令。
// 返回：回调接受请求时为 true；无效、暂停或异常时为 false。
bool PinWindowManager::Submit(PinId id, PinCommand command) noexcept
{
    if (!this->CanInteract(id) || (command != PinCommand::Copy && command != PinCommand::Save))
        return false;
    try
    {
        return this->impl_->callbacks.command && this->impl_->callbacks.command(id, command);
    }
    catch (...)
    {
        OPEN_ST_LOG_ERROR("Pin command callback failed. pin_id=", id);
    }
    return false;
}
// 暂停全部输入并保留所有模态 owner，零标识允许宿主级提示使用。
// 入参：owner 为存活贴图 ID 或零。
// 返回：成功进入暂停为 true；退出中或 owner 无效时为 false。
bool PinWindowManager::BeginModal(PinId owner) noexcept
{
    if (this->impl_->stopping || (owner != 0 && this->Window(owner) == nullptr))
        return false;
    this->EnterDispatch();
    ++this->impl_->modalDepth;
    this->impl_->clicked = 0;
    for (const std::unique_ptr<PinWindow>& pin : this->impl_->windows)
        pin->CancelDrag();
    this->LeaveDispatch();
    return true;
}

// 退出一层全局模态，最外层返回后恢复排序并回收待关闭记录。
// 入参：无。
// 返回：无返回值；退出期间不恢复交互。
void PinWindowManager::EndModal() noexcept
{
    if (this->impl_->modalDepth == 0)
        return;
    this->EnterDispatch();
    if (this->impl_->modalDepth == 1)
        for (const std::unique_ptr<PinWindow>& pin : this->impl_->windows)
            DiscardPausedInput(pin->Handle());
    --this->impl_->modalDepth;
    this->LeaveDispatch();
    this->DrainClosed();
    if (this->impl_->modalDepth == 0)
        this->RepairOrder();
}

// 暂停输入与层级修复，已有贴图保持显示并由截图遮罩覆盖。
// 入参：error 接收忙状态或退出诊断。
// 返回：暂停成功为 true，重复调用不改变状态。
bool PinWindowManager::BeginCapture(std::wstring& error)
{
    if (this->impl_->stopping || this->impl_->modalDepth != 0)
    {
        error = L"Pin manager is busy or stopping.";
        return false;
    }
    if (this->impl_->capturing)
        return true;
    this->EnterDispatch();
    this->impl_->capturing = true;
    this->impl_->clicked = 0;
    for (const std::unique_ptr<PinWindow>& pin : this->impl_->windows)
        pin->CancelDrag();
    this->LeaveDispatch();
    return true;
}

// 在宿主关闭遮罩后结束输入与层级暂停，丢弃暂停期间积压的输入。
// 入参：无。
// 返回：无返回值；退出期间不恢复交互，不改变窗口可见性。
void PinWindowManager::EndCapture() noexcept
{
    if (!this->impl_->capturing || this->impl_->stopping)
        return;
    this->EnterDispatch();
    this->impl_->clicked = 0;
    for (const std::unique_ptr<PinWindow>& pin : this->impl_->windows)
        DiscardPausedInput(pin->Handle());
    this->impl_->capturing = false;
    this->RepairOrder();
    this->LeaveDispatch();
}

// 刷新已创建窗口标题，菜单在打开时独立查询当前语言。
// 入参：无。
// 返回：无返回值；文本回调异常由 Text 处理。
void PinWindowManager::RefreshText() noexcept
{
    this->EnterDispatch();
    const std::wstring title = this->Text("pin.title");
    for (const std::unique_ptr<PinWindow>& pin : this->impl_->windows)
        if (!pin->closing && !SetWindowTextW(pin->Handle(), title.c_str()))
            OPEN_ST_LOG_ERROR("Cannot update pin title. pin_id=", pin->Id(), " error=", GetLastError());
    this->LeaveDispatch();
}

// 将完全离开显示器的图像移回最近工作区，保留物理尺寸。
// 入参：无。
// 返回：无返回值；无法查询显示信息时保留位置并记录错误。
void PinWindowManager::RelocateVisible() noexcept
{
    this->EnterDispatch();
    for (const std::unique_ptr<PinWindow>& pin : this->impl_->windows)
    {
        if (pin->closing)
            continue;
        RECT rectangle{};
        if (!GetWindowRect(pin->Handle(), &rectangle))
            continue;
        if (MonitorFromRect(&rectangle, MONITOR_DEFAULTTONULL) != nullptr)
            continue;
        const HMONITOR monitor = MonitorFromRect(&rectangle, MONITOR_DEFAULTTONEAREST);
        MONITORINFO information{sizeof(information)};
        if (!GetMonitorInfoW(monitor, &information))
        {
            OPEN_ST_LOG_ERROR("Cannot relocate pin. pin_id=", pin->Id(), " error=", GetLastError());
            continue;
        }
        pin->ApplyRectangle(MovePinRectangle(rectangle,
                                             static_cast<long long>(information.rcWork.left) - rectangle.left,
                                             static_cast<long long>(information.rcWork.top) - rectangle.top));
    }
    this->LeaveDispatch();
}
} // namespace open_st
