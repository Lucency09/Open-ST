// 实现无激活截图工具栏、自绘按钮与提示窗口，并向宿主投递命令。

#include "toolbar_layout.h"
#include <capture_toolbar.h>

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <utility>

namespace open_st
{
struct CaptureToolbar::Impl
{
    struct Button
    {
        HWND window{};
        std::wstring text;
        bool hovered{};
        bool pressed{};
    };
    HWND window{};
    HWND tooltip{};
    std::vector<ToolbarButtonSpec> specs;
    std::vector<ToolbarButtonState> states;
    std::vector<Button> buttons;
    TextResolver textResolver;
    CommandHandler onCommand;
    toolbar_detail::ToolbarLayout layout;
    RECT selection{};
    RECT workArea{};
    UINT dpi{96};
    std::uint64_t token{};
    DWORD inputBarrier{};
    bool busy{};
    bool pending{};
    bool placed{};
    bool shown{};

    // 将工具栏图标尺寸从 DIP 换算为目标屏幕物理像素。
    // 入参：value：以 96 DPI 为基准的 DIP 长度。
    // 返回：按宿主指定 DPI 四舍五入后的物理像素长度。
    int Scale(int value) const noexcept
    {
        return MulDiv(value, static_cast<int>(this->dpi), 96);
    }

    // 撤销工具栏的当前鼠标交互，阻止模态操作前的旧输入继续生效。
    // 入参：无。
    // 返回：无返回值；清除按下和悬停状态、收起提示并释放自身按钮的鼠标捕获。
    void ResetInteraction() noexcept
    {
        this->inputBarrier = GetTickCount();
        if (this->tooltip != nullptr)
        {
            SendMessageW(this->tooltip, TTM_POP, 0, 0);
        }
        for (Button& button : this->buttons)
        {
            button.hovered = false;
            button.pressed = false;
            if (GetCapture() == button.window)
            {
                ReleaseCapture();
            }
        }
    }

    // 把业务按钮状态和工具栏忙状态同步到原生控件。
    // 入参：无。
    // 返回：无返回值；按可见性显示按钮，仅在业务允许且无忙或待处理请求时启用。
    void ApplyStates() noexcept
    {
        for (std::size_t index = 0; index < this->buttons.size(); ++index)
        {
            const HWND button = this->buttons[index].window;
            EnableWindow(button, this->states[index].enabled && !this->busy && !this->pending);
            ShowWindow(button, this->states[index].visible ? SW_SHOWNOACTIVATE : SW_HIDE);
            InvalidateRect(button, nullptr, FALSE);
        }
    }

    // 提交被点击按钮的稳定命令 ID 和当前选区代次。
    // 入参：index：按钮在工具栏内部列表中的索引。
    // 返回：无返回值；非法或禁用点击被忽略，提交失败或回调异常时解除待处理锁。
    void Invoke(std::size_t index)
    {
        if (index >= this->buttons.size() || !this->shown || this->busy || this->pending ||
            !this->states[index].enabled || !this->states[index].visible || !this->onCommand)
        {
            return;
        }
        this->pending = true;
        this->ResetInteraction();
        this->ApplyStates();
        bool accepted = false;
        try
        {
            accepted = this->onCommand(this->specs[index].command, this->token);
        }
        catch (...)
        {
            accepted = false;
        }
        if (!accepted)
        {
            this->pending = false;
            this->ApplyStates();
        }
    }

    // 绘制截图工具栏按钮的背景、交互反馈和矢量图标。
    // 入参：draw：系统自绘消息提供的按钮 ID、绘制矩形和借用设备上下文。
    // 返回：无返回值；未知按钮 ID 不绘制，不持有设备上下文。
    void DrawButton(const DRAWITEMSTRUCT& draw)
    {
        const std::size_t index = static_cast<std::size_t>(draw.CtlID - 1U);
        if (index >= this->buttons.size())
        {
            return;
        }
        const Button& button = this->buttons[index];
        const bool enabled = IsWindowEnabled(button.window) != FALSE;
        const COLORREF background =
            enabled && button.pressed
                ? RGB(185, 219, 243)
                : (enabled && (button.hovered || this->states[index].checked) ? RGB(218, 237, 250)
                                                                              : RGB(244, 244, 244));
        const HBRUSH brush = CreateSolidBrush(background);
        FillRect(draw.hDC, &draw.rcItem, brush);
        DeleteObject(brush);
        const HPEN pen =
            CreatePen(PS_SOLID, std::max(1, this->Scale(1)), enabled ? RGB(69, 69, 69) : RGB(170, 170, 170));
        const HGDIOBJ oldPen = SelectObject(draw.hDC, pen);
        const HGDIOBJ oldBrush = SelectObject(draw.hDC, GetStockObject(NULL_BRUSH));
        const int left = (draw.rcItem.right - this->Scale(16)) / 2;
        const int top = (draw.rcItem.bottom - this->Scale(16)) / 2;
        // 把图标局部点映射到按钮绘制坐标。
        // 入参：x、y：图标局部 DIP 坐标；捕获的 left、top 为物理像素原点。
        // 返回：经当前 DPI 缩放并加上原点的物理像素 POINT。
        const auto point = [&](int x, int y) { return POINT{left + this->Scale(x), top + this->Scale(y)}; };
        // 在按钮上绘制一段矢量图标直线。
        // 入参：x1、y1、x2、y2：线段两端的局部 DIP 坐标；借用当前绘制上下文。
        // 返回：无返回值；向设备上下文绘制缩放后的线段。
        const auto line = [&](int x1, int y1, int x2, int y2)
        {
            const POINT first = point(x1, y1);
            const POINT last = point(x2, y2);
            MoveToEx(draw.hDC, first.x, first.y, nullptr);
            LineTo(draw.hDC, last.x, last.y);
        };
        // 在按钮上绘制矩形图标轮廓。
        // 入参：x1、y1、x2、y2：矩形两角的局部 DIP 坐标；借用当前绘制上下文。
        // 返回：无返回值；向设备上下文绘制缩放后的矩形。
        const auto rectangle = [&](int x1, int y1, int x2, int y2)
        {
            const POINT first = point(x1, y1);
            const POINT last = point(x2, y2);
            Rectangle(draw.hDC, first.x, first.y, last.x, last.y);
        };
        switch (this->specs[index].icon)
        {
        case ToolbarIcon::Cancel:
            line(4, 4, 13, 13);
            line(12, 4, 3, 13);
            break;
        case ToolbarIcon::Copy:
            rectangle(2, 1, 12, 12);
            rectangle(5, 4, 15, 15);
            break;
        case ToolbarIcon::Save:
            rectangle(2, 1, 15, 15);
            rectangle(5, 1, 12, 6);
            rectangle(5, 9, 12, 15);
            break;
        }
        SelectObject(draw.hDC, oldBrush);
        SelectObject(draw.hDC, oldPen);
        DeleteObject(pen);
    }

    // 分派工具栏窗口的绘制、命令和 DPI 消息。
    // 入参：window：接收消息的窗口句柄；message：Win32 消息编号；wParam、lParam：对应消息的附加数据。
    // 返回：已处理消息的 Win32 结果；其他消息交给 DefWindowProcW。
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    // 处理工具栏按钮的鼠标交互并防止点击抢走截图键盘焦点。
    // 入参：window：接收消息的窗口句柄；message：Win32 消息编号；wParam、lParam：对应消息的附加数据；subclassId：子类注册标识；reference：注册时借用的工具栏 Impl 指针。
    // 返回：已处理消息的结果；未拦截消息交给 DefSubclassProc。
    static LRESULT CALLBACK ButtonProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR subclassId,
                                       DWORD_PTR reference);
};
// 处理工具栏按钮的鼠标交互并防止点击抢走截图键盘焦点。
// 入参：window：接收消息的窗口句柄；message：Win32 消息编号；wParam、lParam：对应消息的附加数据；subclassId：子类注册标识；reference：注册时借用的工具栏 Impl 指针。
// 返回：已处理消息的结果；未拦截消息交给 DefSubclassProc。
LRESULT CALLBACK CaptureToolbar::Impl::ButtonProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                                                  UINT_PTR subclassId, DWORD_PTR reference)
{
    Impl* self = reinterpret_cast<Impl*>(reference);
    const std::size_t index = static_cast<std::size_t>(subclassId - 1U);
    if (index >= self->buttons.size())
    {
        return DefSubclassProc(window, message, wParam, lParam);
    }
    Button& button = self->buttons[index];
    if (message == WM_MOUSEACTIVATE)
    {
        return MA_NOACTIVATE;
    }
    if (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST && self->tooltip != nullptr)
    {
        MSG relay{window, message, wParam, lParam, static_cast<DWORD>(GetMessageTime()), {}};
        GetCursorPos(&relay.pt);
        SendMessageW(self->tooltip, TTM_RELAYEVENT, 0, reinterpret_cast<LPARAM>(&relay));
    }
    switch (message)
    {
    case WM_MOUSEMOVE:
    {
        button.hovered = true;
        TRACKMOUSEEVENT track{sizeof(TRACKMOUSEEVENT), TME_LEAVE, window, 0};
        TrackMouseEvent(&track);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSELEAVE:
        button.hovered = false;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        if (static_cast<LONG>(static_cast<DWORD>(GetMessageTime()) - self->inputBarrier) <= 0 ||
            !IsWindowEnabled(window) || self->busy || self->pending || !self->shown)
        {
            return 0;
        }
        button.pressed = true;
        SetCapture(window);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP:
    {
        RECT client{};
        GetClientRect(window, &client);
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const bool invoke = button.pressed && GetCapture() == window && PtInRect(&client, point) &&
                            static_cast<LONG>(static_cast<DWORD>(GetMessageTime()) - self->inputBarrier) > 0;
        button.pressed = false;
        if (GetCapture() == window)
        {
            ReleaseCapture();
        }
        InvalidateRect(window, nullptr, FALSE);
        if (invoke)
        {
            self->Invoke(index);
        }
        return 0;
    }
    case WM_CANCELMODE:
    case WM_CAPTURECHANGED:
        button.pressed = false;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case BM_CLICK:
        self->Invoke(index);
        return 0;
    case WM_NCDESTROY:
        RemoveWindowSubclass(window, ButtonProc, subclassId);
        break;
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

// 分派工具栏窗口的绘制、命令和 DPI 消息。
// 入参：window：接收消息的窗口句柄；message：Win32 消息编号；wParam、lParam：对应消息的附加数据。
// 返回：已处理消息的 Win32 结果；其他消息交给 DefWindowProcW。
LRESULT CALLBACK CaptureToolbar::Impl::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    Impl* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        self->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self == nullptr)
    {
        return DefWindowProcW(window, message, wParam, lParam);
    }
    switch (message)
    {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        const HBRUSH brush = CreateSolidBrush(RGB(244, 244, 244));
        const HPEN pen = CreatePen(PS_SOLID, 1, RGB(101, 181, 232));
        const HGDIOBJ oldBrush = SelectObject(dc, brush);
        const HGDIOBJ oldPen = SelectObject(dc, pen);
        RoundRect(dc, 0, 0, client.right, client.bottom, self->Scale(6), self->Scale(6));
        const HPEN divider = CreatePen(PS_SOLID, 1, RGB(190, 190, 190));
        SelectObject(dc, divider);
        for (int x : self->layout.separators)
        {
            MoveToEx(dc, x, self->Scale(8), nullptr);
            LineTo(dc, x, client.bottom - self->Scale(8));
        }
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(divider);
        DeleteObject(pen);
        DeleteObject(brush);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_DRAWITEM:
        self->DrawButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lParam));
        return TRUE;
    case WM_COMMAND:
        if (HIWORD(wParam) == BN_CLICKED && LOWORD(wParam) > 0)
        {
            self->Invoke(static_cast<std::size_t>(LOWORD(wParam) - 1));
        }
        return 0;
    case WM_DPICHANGED:
        // App 的 UpdatePlacement 决定最终几何，不使用 Windows 的建议矩形覆盖物理选区适配。
        return 0;
    case WM_NCDESTROY:
        self->shown = false;
        self->window = nullptr;
        self->tooltip = nullptr;
        self->onCommand = {};
        self->textResolver = {};
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
// 创建尚未绑定窗口的工具栏对象。
// 入参：无。
// 返回：构造函数无返回值；分配内部状态，后续由 Create 注入按钮和回调。
CaptureToolbar::CaptureToolbar() : impl_(std::make_unique<Impl>()) {}
// 销毁工具栏并解除回调及窗口资源。
// 入参：无。
// 返回：析构函数无返回值；执行幂等 Close 清理。
CaptureToolbar::~CaptureToolbar()
{
    this->Close();
}

// 创建不抢焦点的截图工具栏及按钮、提示窗口。
// 入参：instance：借用的进程模块句柄；owner：工具栏所属覆盖窗口；buttons：按显示顺序移入的按钮描述；textResolver：文本键查询回调；onCommand：接收命令及代次的提交回调。
// 返回：ToolbarResult：成功时 success 为 true；失败时为 false，error 仅用于日志诊断。
ToolbarResult CaptureToolbar::Create(HINSTANCE instance, HWND owner, std::vector<ToolbarButtonSpec> buttons,
                                     TextResolver textResolver, CommandHandler onCommand)
{
    this->Close();
    const ToolbarResult validation = toolbar_detail::ValidateButtons(buttons);
    if (!validation.success)
    {
        return validation;
    }
    if (instance == nullptr || !IsWindow(owner) || !textResolver || !onCommand)
    {
        return {false, L"工具栏窗口所有者或回调无效。"};
    }
    this->impl_ = std::make_unique<Impl>();
    Impl& data = *this->impl_;
    data.specs = std::move(buttons);
    data.textResolver = std::move(textResolver);
    data.onCommand = std::move(onCommand);
    data.buttons.resize(data.specs.size());
    for (const ToolbarButtonSpec& spec : data.specs)
    {
        data.states.push_back({spec.command});
    }
    WNDCLASSEXW windowClass{sizeof(WNDCLASSEXW)};
    windowClass.lpfnWndProc = Impl::WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = L"OpenST.CaptureToolbar";
    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        this->Close();
        return {false, L"注册工具栏窗口类失败。"};
    }
    const INITCOMMONCONTROLSEX controls{sizeof(INITCOMMONCONTROLSEX), ICC_WIN95_CLASSES};
    InitCommonControlsEx(&controls);
    if (CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, windowClass.lpszClassName, L"",
                        WS_POPUP | WS_CLIPCHILDREN, 0, 0, 1, 1, owner, nullptr, instance, &data) == nullptr)
    {
        this->Close();
        return {false, L"创建工具栏窗口失败。"};
    }
    data.tooltip = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                                   WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                   CW_USEDEFAULT, data.window, nullptr, instance, nullptr);
    if (data.tooltip == nullptr)
    {
        this->Close();
        return {false, L"创建工具栏提示窗口失败。"};
    }
    SendMessageW(data.tooltip, TTM_SETMAXTIPWIDTH, 0, 600);
    for (std::size_t index = 0; index < data.buttons.size(); ++index)
    {
        const UINT_PTR controlId = index + 1;
        data.buttons[index].window =
            CreateWindowExW(WS_EX_NOACTIVATE, L"BUTTON", L"", WS_CHILD | BS_OWNERDRAW, 0, 0, 1, 1, data.window,
                            reinterpret_cast<HMENU>(controlId), instance, nullptr);
        if (data.buttons[index].window == nullptr || !SetWindowSubclass(data.buttons[index].window, Impl::ButtonProc,
                                                                        controlId, reinterpret_cast<DWORD_PTR>(&data)))
        {
            this->Close();
            return {false, L"创建工具栏按钮失败。"};
        }
        TOOLINFOW tool{sizeof(TOOLINFOW)};
        tool.uFlags = TTF_IDISHWND;
        tool.hwnd = data.window;
        tool.uId = reinterpret_cast<UINT_PTR>(data.buttons[index].window);
        tool.lpszText = const_cast<wchar_t*>(L"");
        if (!SendMessageW(data.tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool)))
        {
            this->Close();
            return {false, L"注册工具栏提示失败。"};
        }
    }
    const ToolbarResult texts = this->RefreshTexts();
    if (!texts.success)
    {
        this->Close();
    }
    return texts;
}

// 按选区和目标显示器工作区定位工具栏。
// 入参：selection、workArea：虚拟桌面物理像素矩形；dpi：目标显示器 DPI。
// 返回：布局及窗口定位成功时 success 为 true；无有效窗口或布局失败时为 false 并附诊断。
ToolbarResult CaptureToolbar::UpdatePlacement(RECT selection, RECT workArea, UINT dpi)
{
    Impl& data = *this->impl_;
    if (data.window == nullptr)
    {
        return {false, L"工具栏尚未创建。"};
    }
    toolbar_detail::ToolbarLayout layout;
    const ToolbarResult result = toolbar_detail::BuildLayout(data.specs, data.states, selection, workArea, dpi, layout);
    if (!result.success)
    {
        return result;
    }
    const RECT bounds = layout.bounds;
    data.dpi = dpi;
    if (!SetWindowPos(data.window, HWND_TOPMOST, bounds.left, bounds.top, bounds.right - bounds.left,
                      bounds.bottom - bounds.top, SWP_NOACTIVATE | SWP_NOOWNERZORDER))
    {
        return {false, L"更新工具栏位置失败。"};
    }
    data.layout = std::move(layout);
    data.selection = selection;
    data.workArea = workArea;
    data.placed = true;
    const HRGN region = CreateRoundRectRgn(0, 0, bounds.right - bounds.left + 1, bounds.bottom - bounds.top + 1,
                                           data.Scale(6), data.Scale(6));
    if (region != nullptr && !SetWindowRgn(data.window, region, TRUE))
    {
        DeleteObject(region);
    }
    for (std::size_t index = 0; index < data.buttons.size(); ++index)
    {
        const RECT button = data.layout.buttons[index];
        SetWindowPos(data.buttons[index].window, nullptr, button.left, button.top, button.right - button.left,
                     button.bottom - button.top, SWP_NOACTIVATE | SWP_NOZORDER);
    }
    data.ApplyStates();
    InvalidateRect(data.window, nullptr, TRUE);
    return {true, {}};
}

// 以指定选区代次显示工具栏并使其位于覆盖窗口上方。
// 入参：sessionToken：非零的当前选区代次；新代次解除旧提交锁。
// 返回：显示成功或全部按钮隐藏时 success 为 true；窗口、代次或布局无效时为 false 并附诊断。
ToolbarResult CaptureToolbar::Show(std::uint64_t sessionToken)
{
    Impl& data = *this->impl_;
    if (data.window == nullptr || !data.placed || sessionToken == 0)
    {
        return {false, L"工具栏尚未定位或会话标识无效。"};
    }
    if (data.token != sessionToken || !data.shown)
    {
        data.ResetInteraction();
        if (data.token != sessionToken)
        {
            data.pending = false;
        }
        data.token = sessionToken;
    }
    if (std::none_of(data.states.begin(), data.states.end(),
                     // 检查按钮是否参与当前工具栏显示。
                     // 入参：state：待检查的按钮状态。
                     // 返回：state.visible；为 true 表示该按钮可见。
                     [](const ToolbarButtonState& state) { return state.visible; }))
    {
        this->Hide();
        return {true, {}};
    }
    data.ApplyStates();
    if (!SetWindowPos(data.window, HWND_TOPMOST, 0, 0, 0, 0,
                      SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW))
    {
        return {false, L"显示工具栏失败。"};
    }
    data.shown = true;
    return {true, {}};
}

// 暂时隐藏工具栏及提示并撤销当前鼠标交互。
// 入参：无。
// 返回：无返回值；保留窗口资源和业务忙状态，供后续再次显示。
void CaptureToolbar::Hide() noexcept
{
    this->impl_->ResetInteraction();
    this->impl_->shown = false;
    if (this->impl_->window != nullptr)
    {
        ShowWindow(this->impl_->window, SW_HIDE);
    }
}

// 同步截图完成流程的忙状态并刷新按钮可用性。
// 入参：busy：true 表示业务处理中；false 表示解除忙状态和本次提交锁。
// 返回：无返回值；清理积压鼠标交互后重新应用按钮状态。
void CaptureToolbar::SetBusy(bool busy) noexcept
{
    if (this->impl_->busy != busy || (!busy && this->impl_->pending))
    {
        this->impl_->ResetInteraction();
    }
    this->impl_->busy = busy;
    if (!busy)
    {
        this->impl_->pending = false;
    }
    this->impl_->ApplyStates();
}

// 校验并应用按稳定命令 ID 指定的部分按钮状态更新。
// 入参：states：本次要更新的可见、启用及选中状态列表，仅在调用期间借用。
// 返回：成功时 success 为 true，已经定位过的工具栏会重新布局；校验、定位或显示失败时为 false 并附诊断。
ToolbarResult CaptureToolbar::UpdateButtonStates(std::span<const ToolbarButtonState> states)
{
    Impl& data = *this->impl_;
    if (data.window == nullptr)
    {
        return {false, L"工具栏尚未创建。"};
    }
    std::vector<ToolbarButtonState> next = data.states;
    for (std::size_t update = 0; update < states.size(); ++update)
    {
        for (std::size_t previous = 0; previous < update; ++previous)
        {
            if (states[previous].command == states[update].command)
            {
                return {false, L"工具栏状态包含重复 ID。"};
            }
        }
        // 定位需要合并新状态的已有工具栏按钮。
        // 入参：state：候选已有按钮状态；捕获的 states[update] 是当前更新项。
        // 返回：命令 ID 与更新项一致时为 true，否则 false。
        const auto found = std::find_if(next.begin(), next.end(), [&](const ToolbarButtonState& state)
                                        { return state.command == states[update].command; });
        if (found == next.end())
        {
            return {false, L"工具栏状态包含未知 ID。"};
        }
        *found = states[update];
    }
    std::vector<ToolbarButtonState> old = std::move(data.states);
    data.states = std::move(next);
    if (data.placed)
    {
        const ToolbarResult result = this->UpdatePlacement(data.selection, data.workArea, data.dpi);
        if (!result.success)
        {
            data.states = std::move(old);
            return result;
        }
    }
    data.ResetInteraction();
    data.ApplyStates();
    if (data.shown)
    {
        return this->Show(data.token);
    }
    return {true, {}};
}

// 重新查询当前语言的工具栏名称和按钮提示文本。
// 入参：无。
// 返回：文本解析成功并提交窗口更新请求时 success 为 true；窗口或解析器缺失、解析异常时 false 并附诊断。
ToolbarResult CaptureToolbar::RefreshTexts()
{
    Impl& data = *this->impl_;
    if (data.window == nullptr || !data.textResolver)
    {
        return {false, L"工具栏尚未创建。"};
    }
    std::vector<std::wstring> texts;
    std::wstring title;
    try
    {
        title = data.textResolver("capture.toolbar.title");
        for (const ToolbarButtonSpec& spec : data.specs)
        {
            texts.push_back(data.textResolver(spec.tooltipKey));
        }
    }
    catch (...)
    {
        return {false, L"解析工具栏文本失败。"};
    }
    SendMessageW(data.tooltip, TTM_POP, 0, 0);
    SetWindowTextW(data.window, title.c_str());
    for (std::size_t index = 0; index < texts.size(); ++index)
    {
        data.buttons[index].text = std::move(texts[index]);
        SetWindowTextW(data.buttons[index].window, data.buttons[index].text.c_str());
        TOOLINFOW tool{sizeof(TOOLINFOW)};
        tool.uFlags = TTF_IDISHWND;
        tool.hwnd = data.window;
        tool.uId = reinterpret_cast<UINT_PTR>(data.buttons[index].window);
        tool.lpszText = data.buttons[index].text.data();
        SendMessageW(data.tooltip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&tool));
    }
    return {true, {}};
}

// 关闭工具栏并撤销与宿主之间的回调连接。
// 入参：无。
// 返回：无返回值；先解除回调再释放提示和按钮窗口，可重复调用。
void CaptureToolbar::Close() noexcept
{
    this->impl_->onCommand = {};
    this->impl_->textResolver = {};
    this->Hide();
    if (IsWindow(this->impl_->tooltip))
    {
        DestroyWindow(this->impl_->tooltip);
    }
    this->impl_->tooltip = nullptr;
    if (IsWindow(this->impl_->window))
    {
        DestroyWindow(this->impl_->window);
    }
    this->impl_->window = nullptr;
    this->impl_->buttons.clear();
    this->impl_->placed = false;
}

// 向宿主提供工具栏原生窗口以便协调或测试。
// 入参：无。
// 返回：工具栏窗口的借用 HWND；尚未创建或已关闭时为 nullptr，不转移所有权。
HWND CaptureToolbar::NativeHandle() const noexcept
{
    return this->impl_->window;
}
} // namespace open_st
