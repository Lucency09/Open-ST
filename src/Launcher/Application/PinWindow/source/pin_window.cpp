// 实现单张贴图的原生窗口、图像呈现和输入消息，排序及生命周期交给管理器。
#include "pin_window.h"
#include "pin_renderer.h"
#include <array>
#include <exception>
#include <log.h>
#include <pin_image.h>
#include <type_traits>
#include <utility>
#include <windowsx.h>

namespace
{
constexpr wchar_t PIN_WINDOW_CLASS[] = L"OpenST.PinWindow";
enum class MenuCommand : UINT
{
    Copy = 1,
    Save = 2,
    Raise = 3,
    Close = 5
};
struct MenuItem final
{
    MenuCommand command;
    const char* key;
    bool separatorBefore;
};
struct MenuDeleter final
{
    // 归还原生菜单句柄。
    // 入参：menu 为当前作用域拥有的菜单。
    // 返回：无返回值，非空句柄被销毁。
    void operator()(HMENU menu) const noexcept
    {
        if (menu != nullptr)
            DestroyMenu(menu);
    }
};

// 把渲染器宽字符诊断转换为日志接受的 UTF-8 文本。
// 入参：text 为失败原因，不会显示给用户。
// 返回：转换后的诊断；转换失败时返回固定的英文说明。
std::string DiagnosticUtf8(const std::wstring& text)
{
    if (text.empty())
        return "No renderer diagnostic available.";
    const int count = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (count <= 0)
        return "Cannot convert renderer diagnostic.";
    std::string result(static_cast<std::size_t>(count), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, result.data(), count, nullptr, nullptr) == 0)
        return "Cannot convert renderer diagnostic.";
    result.pop_back();
    return result;
}
} // namespace
namespace open_st
{
// 保存图像快照并建立尚未创建 HWND 的单图记录。
// 入参：manager 为借用管理器，id 为唯一编号，image 为共享只读原图。
// 返回：无返回值，图形资源由 Prepare 创建。
PinWindow::PinWindow(PinWindowManager& manager, PinId id, std::shared_ptr<const PinImage> image)
    : manager_(manager), id_(id), image_(std::move(image)), renderer_(std::make_unique<PinRenderer>())
{
}

// 释放图形引用后关闭 HWND，防止销毁消息访问正在析构的记录。
// 入参：无。
// 返回：无返回值，窗口与图像资源均交回系统或各自所有者。
PinWindow::~PinWindow()
{
    if (this->window_ != nullptr)
        SetWindowLongPtrW(this->window_, GWLP_USERDATA, 0);
    this->CancelDrag();
    this->renderer_->Reset();
    if (this->window_ != nullptr)
    {
        DestroyWindow(this->window_);
        this->window_ = nullptr;
    }
}

// 创建无边框置顶隐藏窗口，并在发布前完成图像首帧和合成提交。
// 入参：instance 为模块实例，origin 为屏幕物理坐标，error 接收失败诊断。
// 返回：创建和首帧均成功时 true，否则 false，候选资源由析构回收。
bool PinWindow::Prepare(HINSTANCE instance, POINT origin, std::wstring& error)
{
    if (this->image_ == nullptr)
    {
        error = L"Missing pin image.";
        return false;
    }
    WNDCLASSEXW description{sizeof(description)};
    description.hInstance = instance;
    description.lpfnWndProc = &PinWindow::WindowProc;
    description.lpszClassName = PIN_WINDOW_CLASS;
    description.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    description.style = CS_DBLCLKS;
    if (RegisterClassExW(&description) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        error = L"Cannot register pin window class.";
        return false;
    }
    const std::wstring title = this->manager_.Text("pin.title");
    this->window_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP, PIN_WINDOW_CLASS,
                                    title.c_str(), WS_POPUP, origin.x, origin.y, this->image_->Width(),
                                    this->image_->Height(), nullptr, nullptr, instance, this);
    if (this->window_ == nullptr)
    {
        error = L"Cannot create pin window.";
        return false;
    }
    this->prepared_ = this->renderer_->Initialize(this->window_, this->image_, error);
    return this->prepared_;
}

// 在当前客户区绘制图像，并保护失败提示期间的窗口记录。
// 入参：无。
// 返回：绘制成功 true；最终失败 false，并在本地化提示后申请关闭窗口。
bool PinWindow::Redraw() noexcept
{
    PinWindowManager& manager = this->manager_;
    manager.EnterDispatch();
    const bool result = this->RedrawProtected();
    manager.LeaveDispatch();
    return result;
}

// 在调用深度保护内绘制图像，渲染器恢复仍失败时提示并关闭失效贴图。
// 入参：无。
// 返回：绘制成功 true；失败 false，资源恢复次数由渲染器统一控制。
bool PinWindow::RedrawProtected() noexcept
{
    if (!this->prepared_ || this->rendering_ || this->failed_ || this->window_ == nullptr || this->closing)
        return false;
    this->rendering_ = true;
    bool result = false;
    std::wstring error;
    try
    {
        RECT client{};
        if (GetClientRect(this->window_, &client) == FALSE)
            error = L"Cannot query pin client rectangle.";
        else if (client.right <= 0 || client.bottom <= 0)
            result = true;
        else
            result = this->renderer_->Render(client.right, client.bottom, this->state.opacity, error);
        if (!result)
            OPEN_ST_LOG_ERROR("Pin rendering failed. pin_id=", this->id_, " error=", DiagnosticUtf8(error));
    }
    catch (...)
    {
        OPEN_ST_LOG_ERROR("Exception while drawing pin window. pin_id=", this->id_);
    }
    this->rendering_ = false;
    if (result)
        return true;
    this->failed_ = true;
    PinWindowManager& manager = this->manager_;
    const PinId id = this->id_;
    const HWND owner = this->window_;
    try
    {
        const std::wstring message = manager.Text("pin.render_failed");
        const std::wstring title = manager.Text("pin.title");
        if (manager.BeginModal(id))
        {
            MessageBoxW(owner, message.c_str(), title.c_str(), MB_OK | MB_ICONERROR);
            manager.Close(id);
            manager.EndModal();
        }
        else
            manager.Close(id);
    }
    catch (...)
    {
        manager.Close(id);
    }
    return false;
}

// 修改窗口物理矩形而不激活或改变原生层级。
// 入参：rectangle 为屏幕半开物理像素边界。
// 返回：无返回值，WM_SIZE 驱动对应图像重绘。
void PinWindow::ApplyRectangle(RECT rectangle) noexcept
{
    SetWindowPos(this->window_, nullptr, rectangle.left, rectangle.top, rectangle.right - rectangle.left,
                 rectangle.bottom - rectangle.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

// 清除拖动状态并释放本窗口持有的鼠标捕获。
// 入参：无。
// 返回：无返回值，不干涉其他窗口的鼠标捕获。
void PinWindow::CancelDrag() noexcept
{
    this->dragging_ = false;
    if (this->window_ != nullptr && GetCapture() == this->window_)
        ReleaseCapture();
}

// 按有序条目描述创建原生菜单，菜单退出后才执行稳定命令 ID。
// 入参：point 为弹出位置的屏幕物理像素坐标。
// 返回：无返回值，取消不触发业务；失败记录日志，菜单 owner 在整个作用域内保持有效。
void PinWindow::ShowMenu(POINT point) noexcept
{
    PinWindowManager& manager = this->manager_;
    manager.EnterDispatch();
    this->ShowMenuProtected(point);
    manager.LeaveDispatch();
}

// 在窗口记录受保护期间运行菜单及菜单后的业务分派。
// 入参：point 为弹出位置的屏幕物理像素坐标。
// 返回：无返回值，窗口关闭仅登记到最外层调用结束后回收。
void PinWindow::ShowMenuProtected(POINT point) noexcept
{
    PinWindowManager& manager = this->manager_;
    const PinId id = this->id_;
    if (!manager.CanInteract(id))
        return;
    try
    {
        constexpr std::array<MenuItem, 4> items{{{MenuCommand::Copy, "pin.copy", false},
                                                 {MenuCommand::Save, "pin.save", false},
                                                 {MenuCommand::Raise, "pin.raise", true},
                                                 {MenuCommand::Close, "pin.close", true}}};
        std::unique_ptr<std::remove_pointer_t<HMENU>, MenuDeleter> menu(CreatePopupMenu());
        if (menu == nullptr)
        {
            OPEN_ST_LOG_ERROR("Failed to create pin menu.");
            return;
        }
        for (const MenuItem& item : items)
        {
            const std::wstring text = manager.Text(item.key);
            UINT flags = MF_STRING;
            if (item.command == MenuCommand::Raise && manager.IsHighest(id))
                flags |= MF_GRAYED;
            if ((item.separatorBefore && AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr) == FALSE) ||
                AppendMenuW(menu.get(), flags, static_cast<UINT_PTR>(item.command), text.c_str()) == FALSE)
            {
                OPEN_ST_LOG_ERROR("Failed to append pin menu item.");
                return;
            }
        }
        if (!manager.BeginModal(id))
            return;
        const UINT selected = static_cast<UINT>(TrackPopupMenuEx(
            menu.get(), TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, point.x, point.y, this->window_, nullptr));
        menu.reset();
        manager.EndModal();
        if (manager.Window(id) == nullptr)
            return;
        switch (static_cast<MenuCommand>(selected))
        {
        case MenuCommand::Copy:
            (void)manager.Submit(id, PinCommand::Copy);
            break;
        case MenuCommand::Save:
            (void)manager.Submit(id, PinCommand::Save);
            break;
        case MenuCommand::Raise:
            manager.Raise(id);
            break;
        case MenuCommand::Close:
            manager.Close(id);
            break;
        default:
            break;
        }
    }
    catch (...)
    {
        OPEN_ST_LOG_ERROR("Exception while showing pin menu.");
    }
}

// 分派单张贴图的鼠标、窗口绘制和关闭消息。
// 入参：message/wParam/lParam 为原生窗口消息；鼠标坐标显式换算为屏幕物理像素。
// 返回：已处理消息返回对应 Win32 结果，其余交默认窗口过程。
LRESULT PinWindow::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_MOUSEACTIVATE:
        return this->manager_.CanInteract(this->id_) ? MA_ACTIVATE : MA_NOACTIVATEANDEAT;
    case WM_NCHITTEST:
        return HTCLIENT;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT paint{};
        BeginPaint(this->window_, &paint);
        EndPaint(this->window_, &paint);
        if (this->prepared_)
            (void)this->Redraw();
        return 0;
    }
    case WM_SIZE:
        if (this->prepared_ && wParam != SIZE_MINIMIZED)
            (void)this->Redraw();
        return 0;
    case WM_LBUTTONDOWN:
    {
        if (!this->manager_.CanInteract(this->id_))
            return 0;
        this->manager_.Click(this->id_);
        if (GetFocus() != this->window_ || GetForegroundWindow() != this->window_)
            return 0;
        this->dragPoint_ = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ClientToScreen(this->window_, &this->dragPoint_);
        GetWindowRect(this->window_, &this->dragRectangle_);
        this->dragging_ = true;
        SetCapture(this->window_);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (this->dragging_ && this->manager_.CanInteract(this->id_) && GetCapture() == this->window_)
        {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ClientToScreen(this->window_, &point);
            this->ApplyRectangle(MovePinRectangle(this->dragRectangle_,
                                                  static_cast<long long>(point.x) - this->dragPoint_.x,
                                                  static_cast<long long>(point.y) - this->dragPoint_.y));
        }
        return 0;
    case WM_LBUTTONUP:
    case WM_CANCELMODE:
    case WM_CAPTURECHANGED:
        this->CancelDrag();
        return 0;
    case WM_RBUTTONDOWN:
        if (this->manager_.CanInteract(this->id_))
            this->manager_.Click(this->id_);
        return 0;
    case WM_RBUTTONUP:
    {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ClientToScreen(this->window_, &point);
        this->ShowMenu(point);
        return 0;
    }
    case WM_CONTEXTMENU:
    {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (point.x == -1 && point.y == -1)
        {
            RECT rectangle{};
            GetWindowRect(this->window_, &rectangle);
            point = {rectangle.left + 8, rectangle.top + 8};
        }
        this->ShowMenu(point);
        return 0;
    }
    case WM_MOUSEWHEEL:
    {
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (!this->manager_.CanWheel(this->id_, point))
            return 0;
        const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        if ((GET_KEYSTATE_WPARAM(wParam) & MK_CONTROL) != 0)
        {
            AdjustPinOpacity(this->state, delta);
            (void)this->Redraw();
        }
        else
        {
            RECT rectangle{};
            GetWindowRect(this->window_, &rectangle);
            this->ApplyRectangle(
                ZoomPin(this->state, {this->image_->Width(), this->image_->Height()}, rectangle, point, delta));
        }
        return 0;
    }
    case WM_LBUTTONDBLCLK:
        if (this->manager_.CanInteract(this->id_))
        {
            this->manager_.Click(this->id_);
            RECT rectangle{};
            GetWindowRect(this->window_, &rectangle);
            this->ApplyRectangle(ResetPinZoom(this->state, {this->image_->Width(), this->image_->Height()}, rectangle));
        }
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE && GetFocus() == this->window_ && this->manager_.CanInteract(this->id_))
            this->manager_.Close(this->id_);
        return 0;
    case WM_KILLFOCUS:
        this->manager_.LoseFocus(this->id_);
        this->CancelDrag();
        break;
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE)
        {
            this->manager_.LoseFocus(this->id_);
            this->CancelDrag();
        }
        else
            this->manager_.RepairOrder();
        break;
    case WM_WINDOWPOSCHANGING:
        this->manager_.ConstrainOrder(this->id_, *reinterpret_cast<WINDOWPOS*>(lParam));
        break;
    case WM_WINDOWPOSCHANGED:
    {
        const LRESULT result = DefWindowProcW(this->window_, message, wParam, lParam);
        this->manager_.RepairOrder();
        return result;
    }
    case WM_DPICHANGED:
        return 0;
    case WM_DISPLAYCHANGE:
        this->manager_.RelocateVisible();
        return 0;
    case WM_CLOSE:
        this->manager_.Close(this->id_);
        return 0;
    case WM_DESTROY:
        this->prepared_ = false;
        this->renderer_->Reset();
        this->CancelDrag();
        return 0;
    default:
        break;
    }
    return DefWindowProcW(this->window_, message, wParam, lParam);
}

// 对每次窗口入口登记调用深度，保证菜单和嵌套消息期间记录保持有效。
// 入参：window/message/wParam/lParam 为原生窗口消息。
// 返回：对应消息处理结果；异常记录日志后交默认窗口过程处理。
LRESULT CALLBACK PinWindow::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) noexcept
{
    PinWindow* pin = reinterpret_cast<PinWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        const CREATESTRUCTW* creation = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        pin = static_cast<PinWindow*>(creation->lpCreateParams);
        if (pin == nullptr)
            return FALSE;
        pin->window_ = window;
        SetLastError(ERROR_SUCCESS);
        if (SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pin)) == 0 &&
            GetLastError() != ERROR_SUCCESS)
        {
            pin->window_ = nullptr;
            return FALSE;
        }
    }
    if (pin == nullptr)
        return DefWindowProcW(window, message, wParam, lParam);
    PinWindowManager& manager = pin->manager_;
    manager.EnterDispatch();
    LRESULT result{};
    try
    {
        result = pin->HandleMessage(message, wParam, lParam);
    }
    catch (...)
    {
        OPEN_ST_LOG_ERROR("Exception in pin window message.");
        result = DefWindowProcW(window, message, wParam, lParam);
    }
    if (message == WM_NCDESTROY)
    {
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        pin->window_ = nullptr;
        pin->closing = true;
    }
    manager.LeaveDispatch();
    return result;
}
} // namespace open_st
