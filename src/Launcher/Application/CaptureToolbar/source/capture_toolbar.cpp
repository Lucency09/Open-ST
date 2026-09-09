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

    // DIP 转换统一使用 App 指定的目标屏幕 DPI。
    int Scale(int value) const noexcept
    {
        return MulDiv(value, static_cast<int>(this->dpi), 96);
    }

    // 清除按下、悬停、捕获和提示，防止旧交互跨过保存模态窗口。
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

    // 同步原生控件可用性，保留 App 下发的独立业务禁用状态。
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

    // 命令投递前锁定；投递失败或异常立即恢复，绝不执行业务函数。
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

    // 绘制本地矢量图标，不依赖字符字体或外部图片。
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
        const auto point = [&](int x, int y) { return POINT{left + this->Scale(x), top + this->Scale(y)}; };
        const auto line = [&](int x1, int y1, int x2, int y2)
        {
            const POINT first = point(x1, y1);
            const POINT last = point(x2, y2);
            MoveToEx(draw.hDC, first.x, first.y, nullptr);
            LineTo(draw.hDC, last.x, last.y);
        };
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

    // 顶层原生窗口消息入口。
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    // 拦截原生按钮鼠标行为，保证点击不会抢占截图键盘焦点。
    static LRESULT CALLBACK ButtonProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR subclassId,
                                       DWORD_PTR reference);
};
// 原生 BUTTON 保留可访问名称；鼠标路径由子类处理，避免默认按钮过程 SetFocus。
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

// 顶层窗口只处理呈现、原生命令和 DPI，不向 App 转发显示拓扑变化。
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
// 实例只持有自身窗口资源，不依赖应用或截图模块。
CaptureToolbar::CaptureToolbar() : impl_(std::make_unique<Impl>()) {}
// 析构和显式关闭共享幂等资源释放路径。
CaptureToolbar::~CaptureToolbar()
{
    this->Close();
}

// 创建无激活 owned popup、可访问原生按钮及独立 tooltip。
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

// 按显式工作区和 DPI 应用布局，不激活或抢占其他窗口焦点。
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

// 新选区代次解除旧请求锁，并把工具栏提升到所有覆盖窗口之上。
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

// 临时隐藏同时撤销鼠标捕获和当前 tooltip。
void CaptureToolbar::Hide() noexcept
{
    this->impl_->ResetInteraction();
    this->impl_->shown = false;
    if (this->impl_->window != nullptr)
    {
        ShowWindow(this->impl_->window, SW_HIDE);
    }
}

// 业务完成后由 App 明确解锁，丢弃忙状态前积压的鼠标按下消息。
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

// 更新部分按钮状态前校验所有 ID，失败时保留旧状态。
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

// 先解析全部自有字符串，再更新窗口名称与 tooltip，避免借用临时 c_str。
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

// 首先解除业务回调，再销毁 tooltip 和工具栏；允许所有者提前销毁和重复关闭。
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

// 返回借用句柄，仅供所有者协调或测试查询，不转移所有权。
HWND CaptureToolbar::NativeHandle() const noexcept
{
    return this->impl_->window;
}
} // namespace open_st
