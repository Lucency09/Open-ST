#include "renderer_model.h"
#include <algorithm>
#include <array>
#include <commctrl.h>
#include <dwmapi.h>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>
#include <window_renderer.h>
#include <windowsx.h>

namespace open_st
{
using renderer_detail::Node;
using renderer_detail::NodeType;
using renderer_detail::Page;

struct WindowRenderer::Impl
{
    struct Control
    {
        const Node* node{};
        std::string page;
        HWND window{};
        HWND label{};
        HWND errorWindow{};
        std::wstring text;
        std::wstring error;
        bool enabled{true};
        std::vector<RendererOption> options;
        std::function<RendererStringResult()> read;
        std::function<RendererChangeResult(std::string_view)> change;
        std::function<RendererOptionsResult()> query;
        std::function<void()> action;
    };

    renderer_detail::Layout layout;
    std::map<std::string, Control, std::less<>> controls;
    std::map<std::string, const Node*, std::less<>> nodes;
    std::function<std::wstring(std::string_view)> text;
    std::function<void()> close;
    std::function<void(const RendererResult&)> errorHandler;
    std::string defaultAction;
    HWND window{};
    HWND tabs{};
    HWND viewport{};
    HWND statusWindow{};
    HWND savedFocus{};
    HFONT font{};
    DWORD thread{GetCurrentThreadId()};
    UINT dpi{96};
    std::size_t pageIndex{};
    int scroll{};
    int contentHeight{};
    bool loaded{};
    bool busy{};
    bool modal{};
    bool modalCloseRequested{};
    bool refreshing{};
    bool arranging{};
    std::wstring status;
    static constexpr UINT CLOSE_MESSAGE = WM_APP + 91;

    // 事件没有同步返回值时交给宿主显示错误，错误回调本身不能穿过系统边界。
    void Report(const RendererResult& result) const noexcept
    {
        try
        {
            if (this->errorHandler)
                this->errorHandler(result);
            else
                OutputDebugStringW(L"WindowRenderer: event failed.\n");
        }
        catch (...)
        {
            OutputDebugStringW(L"WindowRenderer: error callback failed.\n");
        }
    }

    // 键盘导航到被裁剪的字段后滚动，保持焦点本身不变。
    void RevealFocus()
    {
        const HWND focused = GetFocus();
        if (focused == nullptr || GetParent(focused) != this->viewport)
            return;
        RECT bounds{};
        RECT view{};
        GetWindowRect(focused, &bounds);
        MapWindowPoints(nullptr, this->viewport, reinterpret_cast<POINT*>(&bounds), 2);
        GetClientRect(this->viewport, &view);
        if (bounds.top < 0)
            this->scroll += bounds.top;
        else if (bounds.bottom > view.bottom)
            this->scroll += bounds.bottom - view.bottom;
        else
            return;
        this->Arrange();
    }

    // 宿主线程检查，错误不包含用户字段内容。
    RendererResult Check(bool mutableBindings = false) const
    {
        if (GetCurrentThreadId() != this->thread)
            return {"wrong_thread", {}, {}};
        if (mutableBindings && this->window != nullptr)
            return {"window_active", {}, {}};
        return {};
    }

    // 将 DIP 集中换算为本窗口物理像素。
    int Scale(int value) const
    {
        return MulDiv(value, static_cast<int>(this->dpi), 96);
    }

    // 递归建立稳定索引，只有叶节点创建原生控件。
    void Index(const Node& node, const std::string& page)
    {
        this->nodes.emplace(node.id, &node);
        if (node.type != NodeType::Column)
        {
            Control control;
            control.node = &node;
            control.page = page;
            this->controls.emplace(node.id, std::move(control));
        }
        for (const Node& child : node.children)
            this->Index(child, page);
    }

    // 按类型检查绑定目标，不允许无效 ID 或跨控件类型注册。
    RendererResult Find(std::string_view id, NodeType type, Control*& output)
    {
        const RendererResult checked = this->Check(true);
        if (!checked)
            return checked;
        const auto found = this->controls.find(id);
        if (found == this->controls.end())
            return {"unknown_id", {}, std::string(id)};
        if (found->second.node->type != type)
            return {"wrong_control_type", {}, std::string(id)};
        output = &found->second;
        return {};
    }

    // 检查所有交互节点均有对应业务回调。
    RendererResult Validate() const
    {
        const RendererResult checked = this->Check();
        if (!checked)
            return checked;
        if (!this->loaded)
            return {"layout_missing", {}, {}};
        if (!this->text || !this->close || this->defaultAction.empty())
            return {"window_binding_missing", {}, {}};
        for (const auto& [id, control] : this->controls)
        {
            if (control.node->type == NodeType::Select && (!control.read || !control.change || !control.query))
                return {"field_binding_missing", {}, id};
            if (control.node->type == NodeType::Button && !control.action)
                return {"action_missing", {}, id};
        }
        return {};
    }

    // 以系统字体测量单行宽度，调用方负责分配换行空间。
    int TextWidth(const std::wstring& value) const
    {
        HDC dc = GetDC(this->window);
        if (dc == nullptr)
            return this->Scale(100);
        HGDIOBJ previous = SelectObject(dc, this->font);
        SIZE size{};
        GetTextExtentPoint32W(dc, value.c_str(), static_cast<int>(value.size()), &size);
        SelectObject(dc, previous);
        ReleaseDC(this->window, dc);
        return size.cx;
    }

    // 计算长文本换行高度，空文本不占空间。
    int TextHeight(const std::wstring& value, int width) const
    {
        if (value.empty())
            return 0;
        HDC dc = GetDC(this->window);
        if (dc == nullptr)
            return this->Scale(24);
        HGDIOBJ previous = SelectObject(dc, this->font);
        RECT bounds{0, 0, std::max(1, width), 0};
        DrawTextW(dc, value.c_str(), static_cast<int>(value.size()), &bounds, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(dc, previous);
        ReleaseDC(this->window, dc);
        return std::max(this->Scale(20), static_cast<int>(bounds.bottom));
    }

    // 字体更新后所有子控件统一使用同一系统字体。
    void RefreshFont()
    {
        NONCLIENTMETRICSW metrics{sizeof(metrics)};
        HFONT candidate{};
        if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, this->dpi))
            candidate = CreateFontIndirectW(&metrics.lfMessageFont);
        if (candidate == nullptr)
            candidate = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        const HFONT previous = this->font;
        this->font = candidate;
        for (const auto& [id, control] : this->controls)
        {
            for (HWND child : {control.window, control.label, control.errorWindow})
                if (child != nullptr)
                    SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(candidate), TRUE);
        }
        SendMessageW(this->tabs, WM_SETFONT, reinterpret_cast<WPARAM>(candidate), TRUE);
        SendMessageW(this->statusWindow, WM_SETFONT, reinterpret_cast<WPARAM>(candidate), TRUE);
        if (previous != nullptr && previous != GetStockObject(DEFAULT_GUI_FONT))
            DeleteObject(previous);
    }

    // 定位一棵页面布局树，先测量后按滚动偏移放置。
    int ArrangeNode(const Node& node, int x, int y, int width, bool place);
    // 根据客户区重排固定按钮、标签和可滚动页面。
    void Arrange();
    // 创建原生控件，不读盘且不调用业务写入。
    bool CreateControls();
    // 重新获取一个下拉框的选项与草稿，刷新不发送变更回调。
    RendererResult RefreshControl(Control& control);
    // 执行按钮或下拉框事件，异常在窗口过程边界收敛。
    void Command(WPARAM wParam, LPARAM lParam);
    // 执行指定动作，禁用和忙状态下不分派。
    void Invoke(std::string_view id);
    // 主窗口静态过程恢复实例后处理消息。
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    // 页面容器向所属窗口转发事件并处理滚动。
    static LRESULT CALLBACK PageProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    // 主窗口消息实现与系统默认行为衔接。
    LRESULT Message(HWND target, UINT message, WPARAM wParam, LPARAM lParam);
};

// 根据节点尺寸策略测量和排列，页面坐标均为像素。
int WindowRenderer::Impl::ArrangeNode(const Node& node, int x, int y, int width, bool place)
{
    int actualWidth = width;
    if (node.width > 0)
        actualWidth = std::min(width, this->Scale(node.width));
    if (node.type == NodeType::Column)
    {
        const int padding = this->Scale(node.padding);
        int cursor = padding;
        for (std::size_t index = 0; index < node.children.size(); ++index)
        {
            if (index != 0)
                cursor += this->Scale(node.gap);
            cursor += this->ArrangeNode(node.children[index], x + padding, y + cursor,
                                        std::max(1, actualWidth - 2 * padding), place);
        }
        return cursor + padding;
    }
    Control& control = this->controls.at(node.id);
    if (node.width == 0)
        actualWidth = std::min(width, std::max(this->Scale(80), this->TextWidth(control.text) + this->Scale(24)));
    int height = this->TextHeight(control.text, actualWidth);
    if (node.type == NodeType::Button)
        height = std::max(this->Scale(28), height + this->Scale(10));
    if (place)
    {
        const HWND textWindow = node.type == NodeType::Select ? control.label : control.window;
        MoveWindow(textWindow, x, y - this->scroll, actualWidth, height, TRUE);
    }
    if (node.type == NodeType::Select)
    {
        const int fieldY = y + height + this->Scale(4);
        if (place)
            MoveWindow(control.window, x, fieldY - this->scroll, actualWidth, this->Scale(180), TRUE);
        height += this->Scale(32);
    }
    const int errorHeight = this->TextHeight(control.error, actualWidth);
    if (place && control.errorWindow != nullptr)
        MoveWindow(control.errorWindow, x, y + height - this->scroll, actualWidth, errorHeight, TRUE);
    return height + errorHeight;
}

// 按钮空间不足时换行，页面内容在独立容器内裁剪及滚动。
void WindowRenderer::Impl::Arrange()
{
    if (this->window == nullptr || this->arranging)
        return;
    this->arranging = true;
    RECT client{};
    GetClientRect(this->window, &client);
    const int margin = this->Scale(12);
    const int gap = this->Scale(8);
    const int width = std::max(1, static_cast<int>(client.right) - 2 * margin);
    struct Position
    {
        HWND window;
        int x;
        int y;
        int width;
        int height;
    };
    std::vector<Position> positions;
    int x = 0;
    int y = 0;
    int rowHeight = 0;
    // 左组按序排列，右组能放入同一行时贴右，否则从下一行开始。
    for (int group = 0; group < 2; ++group)
    {
        const std::vector<Node>& buttons = group == 0 ? this->layout.leading : this->layout.trailing;
        int groupWidth = 0;
        for (const Node& node : buttons)
            groupWidth += std::min(width, std::max(this->Scale(76), this->TextWidth(this->controls.at(node.id).text) +
                                                                        this->Scale(24))) +
                          gap;
        if (group == 1 && !buttons.empty())
        {
            if (x != 0 && x + groupWidth - gap > width)
            {
                y += rowHeight + gap;
                x = 0;
                rowHeight = 0;
            }
            if (groupWidth - gap <= width)
                x = std::max(x, width - groupWidth + gap);
        }
        for (const Node& node : buttons)
        {
            Control& control = this->controls.at(node.id);
            const int buttonWidth =
                std::min(width, std::max(this->Scale(76), this->TextWidth(control.text) + this->Scale(24)));
            const int height = std::max(this->Scale(28), this->TextHeight(control.text, buttonWidth) + this->Scale(10));
            if (x != 0 && x + buttonWidth > width)
            {
                y += rowHeight + gap;
                x = 0;
                rowHeight = 0;
            }
            positions.push_back({control.window, x, y, buttonWidth, height});
            rowHeight = std::max(rowHeight, height);
            x += buttonWidth + gap;
        }
    }
    const int footerHeight = positions.empty() ? 0 : y + rowHeight;
    const int statusHeight = this->TextHeight(this->status, width);
    const int footerY = std::max(margin, static_cast<int>(client.bottom) - margin - footerHeight);
    for (const Position& position : positions)
        MoveWindow(position.window, margin + position.x, footerY + position.y, position.width, position.height, TRUE);
    const int statusY = footerY - (statusHeight != 0 ? statusHeight + gap : 0);
    MoveWindow(this->statusWindow, margin, statusY, width, statusHeight, TRUE);
    const int tabHeight = this->layout.showTabs ? this->Scale(28) : 0;
    MoveWindow(this->tabs, margin, margin, width, tabHeight, TRUE);
    const int pageY = margin + tabHeight + (this->layout.showTabs ? gap : 0);
    const int pageHeight = std::max(1, statusY - gap - pageY);
    MoveWindow(this->viewport, margin, pageY, width, pageHeight, TRUE);
    RECT view{};
    GetClientRect(this->viewport, &view);
    const Page& page = this->layout.pages[this->pageIndex];
    this->contentHeight = this->ArrangeNode(page.content, 0, 0, std::max(1L, view.right), false);
    this->scroll = std::clamp(this->scroll, 0, std::max(0, this->contentHeight - pageHeight));
    SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS};
    info.nMin = 0;
    info.nMax = std::max(0, this->contentHeight - 1);
    info.nPage = static_cast<UINT>(pageHeight);
    info.nPos = this->scroll;
    SetScrollInfo(this->viewport, SB_VERT, &info, TRUE);
    GetClientRect(this->viewport, &view);
    this->ArrangeNode(page.content, 0, 0, std::max(1L, view.right), true);
    for (const auto& [id, control] : this->controls)
    {
        const bool visible = control.page.empty() || control.page == page.id;
        for (HWND child : {control.window, control.label, control.errorWindow})
            if (child != nullptr)
                ShowWindow(child, visible ? SW_SHOWNA : SW_HIDE);
    }
    this->arranging = false;
}

// 创建所有真实控件，页面内容放进可裁剪的对话框导航容器。
bool WindowRenderer::Impl::CreateControls()
{
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    if (this->layout.showTabs)
        this->tabs = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 1, 1,
                                     this->window, nullptr, instance, nullptr);
    this->viewport = CreateWindowExW(WS_EX_CONTROLPARENT, L"OpenST.WindowRendererPage", L"",
                                     WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPCHILDREN, 0, 0, 1, 1, this->window,
                                     nullptr, instance, this);
    this->statusWindow = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_NOPREFIX, 0, 0, 1, 1,
                                         this->window, nullptr, instance, nullptr);
    if ((this->layout.showTabs && !this->tabs) || !this->viewport || !this->statusWindow)
        return false;
    int index = 0;
    for (const Page& page : this->layout.pages)
    {
        if (!this->layout.showTabs)
            break;
        std::wstring title = this->text(page.titleKey);
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        item.pszText = title.data();
        if (TabCtrl_InsertItem(this->tabs, index++, &item) == -1)
            return false;
    }
    int controlId = 100;
    // 遍历布局顺序创建 HWND，使 Tab 顺序与 JSON 内容一致。
    const std::function<bool(const Node&, HWND)> create =
        [this, instance, &controlId, &create](const Node& node, HWND parent)
    {
        if (node.type == NodeType::Column)
        {
            for (const Node& child : node.children)
                if (!create(child, parent))
                    return false;
            return true;
        }
        Control& control = this->controls.at(node.id);
        control.text = this->text(node.textKey);
        const HMENU id = reinterpret_cast<HMENU>(static_cast<INT_PTR>(controlId++));
        if (node.type == NodeType::Select)
        {
            control.label = CreateWindowExW(0, L"STATIC", control.text.c_str(), WS_CHILD | WS_VISIBLE | SS_NOPREFIX, 0,
                                            0, 1, 1, parent, nullptr, instance, nullptr);
            control.window =
                CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                0, 0, 100, 180, parent, id, instance, nullptr);
            control.errorWindow = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_NOPREFIX, 0, 0, 1, 1,
                                                  parent, nullptr, instance, nullptr);
            if (!control.label || !control.errorWindow)
                return false;
        }
        else
        {
            const bool button = node.type == NodeType::Button;
            control.window = CreateWindowExW(0, button ? L"BUTTON" : L"STATIC", control.text.c_str(),
                                             WS_CHILD | WS_VISIBLE | (button ? WS_TABSTOP | BS_MULTILINE : SS_NOPREFIX),
                                             0, 0, 1, 1, parent, id, instance, nullptr);
            if (button && node.id == this->defaultAction)
                SendMessageW(control.window, BM_SETSTYLE, BS_DEFPUSHBUTTON | BS_MULTILINE, TRUE);
        }
        return control.window != nullptr;
    };
    for (const Page& page : this->layout.pages)
        if (!create(page.content, this->viewport))
            return false;
    for (const Node& node : this->layout.leading)
        if (!create(node, this->window))
            return false;
    for (const Node& node : this->layout.trailing)
        if (!create(node, this->window))
            return false;
    this->RefreshFont();
    for (auto& [id, control] : this->controls)
    {
        if (control.node->type == NodeType::Select)
        {
            const RendererResult result = this->RefreshControl(control);
            if (!result)
                return false;
        }
        EnableWindow(control.window, control.enabled && !this->busy);
    }
    SetWindowTextW(this->statusWindow, this->status.c_str());
    this->Arrange();
    return true;
}

// 重新填充下拉框时不改变草稿，无匹配值保持无选择并展示宿主错误。
RendererResult WindowRenderer::Impl::RefreshControl(Control& control)
{
    RendererOptionsResult options;
    RendererStringResult value;
    try
    {
        options = control.query();
        value = control.read();
    }
    catch (...)
    {
        this->Report({"callback_failed", {}, control.node->id});
        return {"callback_failed", {}, control.node->id};
    }
    std::set<std::string> seen;
    for (const RendererOption& option : options.options)
        if (!seen.insert(option.value).second)
            return {"duplicate_option", {}, control.node->id};
    control.options = options.success ? options.options : std::vector<RendererOption>{};
    this->refreshing = true;
    SendMessageW(control.window, CB_RESETCONTENT, 0, 0);
    int selected = -1;
    for (std::size_t index = 0; index < control.options.size(); ++index)
    {
        const RendererOption& option = control.options[index];
        const LRESULT added =
            SendMessageW(control.window, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(option.label.c_str()));
        if (added < 0)
        {
            this->refreshing = false;
            return {"control_update_failed", {}, control.node->id};
        }
        if (value.success && option.value == value.value)
            selected = static_cast<int>(index);
    }
    SendMessageW(control.window, CB_SETCURSEL, static_cast<WPARAM>(selected), 0);
    control.error = !value.success ? value.error : options.error;
    SetWindowTextW(control.errorWindow, control.error.c_str());
    this->refreshing = false;
    if (selected < 0 && value.success && options.success)
        this->Report({"value_unavailable", {}, control.node->id});
    return {};
}

// 仅向可用按钮分派动作，JSON ID 不被解释为业务操作。
void WindowRenderer::Impl::Invoke(std::string_view id)
{
    const auto found = this->controls.find(id);
    if (this->busy || found == this->controls.end() || !found->second.enabled || !found->second.action)
        return;
    found->second.action();
}

// 原生通知通过实际 HWND 对应到绑定，程序填充值不回写。
void WindowRenderer::Impl::Command(WPARAM wParam, LPARAM lParam)
{
    if (this->busy || this->refreshing)
        return;
    const HWND source = reinterpret_cast<HWND>(lParam);
    for (auto& [id, control] : this->controls)
    {
        if (control.window != source || !control.enabled)
            continue;
        if (control.node->type == NodeType::Button && HIWORD(wParam) == BN_CLICKED)
        {
            this->Invoke(id);
            return;
        }
        if (control.node->type != NodeType::Select)
            return;
        if (HIWORD(wParam) == CBN_DROPDOWN)
        {
            const RendererResult result = this->RefreshControl(control);
            if (!result)
                throw std::runtime_error("Option refresh failed");
            this->Arrange();
        }
        else if (HIWORD(wParam) == CBN_SELCHANGE)
        {
            const LRESULT selected = SendMessageW(control.window, CB_GETCURSEL, 0, 0);
            if (selected < 0 || static_cast<std::size_t>(selected) >= control.options.size())
                return;
            // 按值复制，宿主回调允许刷新控件，不持有跨回调的选项引用。
            const std::string value = control.options[static_cast<std::size_t>(selected)].value;
            RendererChangeResult result;
            bool callbackFailed = false;
            try
            {
                result = control.change(value);
            }
            catch (...)
            {
                result.accepted = false;
                callbackFailed = true;
            }
            if (!result.accepted)
            {
                const RendererResult refreshed = this->RefreshControl(control);
                if (!refreshed)
                    throw std::runtime_error("Rejected value refresh failed");
            }
            control.error = result.error;
            SetWindowTextW(control.errorWindow, control.error.c_str());
            if (callbackFailed)
                this->Report({"callback_failed", {}, id});
            this->Arrange();
        }
        return;
    }
}

// 主窗口边界禁止异常传播到系统；创建异常明确取消创建。
LRESULT CALLBACK WindowRenderer::Impl::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    Impl* impl = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        impl = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        impl->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(impl));
    }
    try
    {
        return impl ? impl->Message(window, message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
    }
    catch (...)
    {
        OutputDebugStringW(L"WindowRenderer: window callback failed.\n");
        if (impl != nullptr && message != WM_CREATE)
            impl->Report({"callback_failed", {}, {}});
        return message == WM_CREATE ? -1 : 0;
    }
}

// 容器独立裁剪页面，并将所有控件事件交回同一绑定表。
LRESULT CALLBACK WindowRenderer::Impl::PageProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    Impl* impl = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        impl = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(impl));
    }
    try
    {
        if (impl != nullptr && message == WM_COMMAND)
        {
            impl->Command(wParam, lParam);
            return 0;
        }
        if (impl != nullptr && (message == WM_VSCROLL || message == WM_MOUSEWHEEL))
        {
            if (impl->busy)
                return 0;
            SCROLLINFO info{sizeof(info), SIF_ALL};
            GetScrollInfo(window, SB_VERT, &info);
            int candidate = impl->scroll;
            if (message == WM_MOUSEWHEEL)
                candidate -= GET_WHEEL_DELTA_WPARAM(wParam) * impl->Scale(48) / WHEEL_DELTA;
            else
            {
                switch (LOWORD(wParam))
                {
                case SB_LINEUP:
                    candidate -= impl->Scale(24);
                    break;
                case SB_LINEDOWN:
                    candidate += impl->Scale(24);
                    break;
                case SB_PAGEUP:
                    candidate -= static_cast<int>(info.nPage);
                    break;
                case SB_PAGEDOWN:
                    candidate += static_cast<int>(info.nPage);
                    break;
                case SB_THUMBTRACK:
                    candidate = info.nTrackPos;
                    break;
                case SB_TOP:
                    candidate = 0;
                    break;
                case SB_BOTTOM:
                    candidate = info.nMax;
                    break;
                default:
                    break;
                }
            }
            impl->scroll = std::clamp(candidate, 0, std::max(0, impl->contentHeight - static_cast<int>(info.nPage)));
            impl->Arrange();
            return 0;
        }
    }
    catch (...)
    {
        OutputDebugStringW(L"WindowRenderer: page callback failed.\n");
        if (impl != nullptr)
            impl->Report({"callback_failed", {}, {}});
        if (message == WM_MOUSEWHEEL)
            return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

// 消息循环保留系统标题栏行为，业务关闭交由宿主。
LRESULT WindowRenderer::Impl::Message(HWND target, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CREATE:
        return this->CreateControls() ? 0 : -1;
    case WM_COMMAND:
        this->Command(wParam, lParam);
        return 0;
    case WM_CLOSE:
        if (!this->busy && this->close)
            this->close();
        return 0;
    case CLOSE_MESSAGE:
        if (!this->busy)
        {
            if (this->modal)
                this->modalCloseRequested = true;
            else
                DestroyWindow(target);
        }
        return 0;
    case WM_SIZE:
        this->Arrange();
        return 0;
    case WM_MOUSEWHEEL:
        return SendMessageW(this->viewport, message, wParam, lParam);
    case WM_NOTIFY:
        if (reinterpret_cast<NMHDR*>(lParam)->hwndFrom == this->tabs &&
            reinterpret_cast<NMHDR*>(lParam)->code == TCN_SELCHANGE)
        {
            const int selected = TabCtrl_GetCurSel(this->tabs);
            if (selected >= 0 && static_cast<std::size_t>(selected) < this->layout.pages.size())
            {
                this->pageIndex = static_cast<std::size_t>(selected);
                this->scroll = 0;
                this->Arrange();
            }
            return 0;
        }
        break;
    case WM_GETMINMAXINFO:
    {
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST), &monitor);
        RECT minimum{0, 0, this->Scale(this->layout.minWidth), this->Scale(this->layout.minHeight)};
        AdjustWindowRectExForDpi(&minimum, static_cast<DWORD>(GetWindowLongPtrW(target, GWL_STYLE)), FALSE, 0,
                                 this->dpi);
        MINMAXINFO* bounds = reinterpret_cast<MINMAXINFO*>(lParam);
        bounds->ptMinTrackSize.x = std::min(minimum.right - minimum.left, monitor.rcWork.right - monitor.rcWork.left);
        bounds->ptMinTrackSize.y = std::min(minimum.bottom - minimum.top, monitor.rcWork.bottom - monitor.rcWork.top);
        return 0;
    }
    case WM_DPICHANGED:
    {
        this->dpi = HIWORD(wParam);
        const RECT& suggested = *reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(target, nullptr, suggested.left, suggested.top, suggested.right - suggested.left,
                     suggested.bottom - suggested.top, SWP_NOACTIVATE | SWP_NOZORDER);
        this->RefreshFont();
        this->Arrange();
        return 0;
    }
    case WM_NCDESTROY:
        this->window = nullptr;
        this->tabs = nullptr;
        this->viewport = nullptr;
        this->statusWindow = nullptr;
        for (auto& [id, control] : this->controls)
            control.window = control.label = control.errorWindow = nullptr;
        SetWindowLongPtrW(target, GWLP_USERDATA, 0);
        break;
    default:
        break;
    }
    return DefWindowProcW(target, message, wParam, lParam);
}

// 构造对象固定 UI 线程，不创建窗口或读取文件。
WindowRenderer::WindowRenderer() : impl_(std::make_unique<Impl>()) {}

// HWND 先销毁，随后释放字体和捕获回调。
WindowRenderer::~WindowRenderer()
{
    if (this->impl_->window != nullptr)
        DestroyWindow(this->impl_->window);
    if (this->impl_->font != nullptr && this->impl_->font != GetStockObject(DEFAULT_GUI_FONT))
        DeleteObject(this->impl_->font);
}

// 候选文档完整解析后再发布并清空旧绑定，不保留外部 JSON 引用。
RendererResult WindowRenderer::LoadLayout(const nlohmann::json& document)
{
    const RendererResult checked = this->impl_->Check(true);
    if (!checked)
        return checked;
    std::unique_ptr<Impl> candidate = std::make_unique<Impl>();
    const RendererResult parsed = renderer_detail::ParseLayout(document, candidate->layout);
    if (!parsed)
        return parsed;
    candidate->loaded = true;
    for (const Page& page : candidate->layout.pages)
        candidate->Index(page.content, page.id);
    for (const Node& node : candidate->layout.leading)
        candidate->Index(node, {});
    for (const Node& node : candidate->layout.trailing)
        candidate->Index(node, {});
    if (this->impl_->font != nullptr && this->impl_->font != GetStockObject(DEFAULT_GUI_FONT))
        DeleteObject(this->impl_->font);
    this->impl_.swap(candidate);
    return {};
}

// 本地化只注册一个解析器，不维护文本键映射。
RendererResult WindowRenderer::SetTextResolver(std::function<std::wstring(std::string_view)> callback)
{
    const RendererResult checked = this->impl_->Check(true);
    if (!checked)
        return checked;
    if (!callback)
        return {"empty_callback", {}, {}};
    if (this->impl_->text)
        return {"duplicate_binding", {}, {}};
    this->impl_->text = std::move(callback);
    return {};
}

// 数据读取和修改成对绑定，选项绑定使用独立槽位。
RendererResult WindowRenderer::BindString(std::string_view id, std::function<RendererStringResult()> read,
                                          std::function<RendererChangeResult(std::string_view)> change)
{
    Impl::Control* control{};
    const RendererResult found = this->impl_->Find(id, NodeType::Select, control);
    if (!found)
        return found;
    if (!read || !change)
        return {"empty_callback", {}, std::string(id)};
    if (control->read || control->change)
        return {"duplicate_binding", {}, std::string(id)};
    control->read = std::move(read);
    control->change = std::move(change);
    return {};
}

// 动态选项来源与字段回调分别校验和注册。
RendererResult WindowRenderer::BindOptions(std::string_view id, std::function<RendererOptionsResult()> query)
{
    Impl::Control* control{};
    const RendererResult found = this->impl_->Find(id, NodeType::Select, control);
    if (!found)
        return found;
    if (!query)
        return {"empty_callback", {}, std::string(id)};
    if (control->query)
        return {"duplicate_binding", {}, std::string(id)};
    control->query = std::move(query);
    return {};
}

// 动作只允许绑定到按钮，不接受任意代码名称解释。
RendererResult WindowRenderer::BindAction(std::string_view id, std::function<void()> callback)
{
    Impl::Control* control{};
    const RendererResult found = this->impl_->Find(id, NodeType::Button, control);
    if (!found)
        return found;
    if (!callback)
        return {"empty_callback", {}, std::string(id)};
    if (control->action)
        return {"duplicate_binding", {}, std::string(id)};
    control->action = std::move(callback);
    return {};
}

// 关闭语义属于宿主，Renderer 仅转发用户关闭请求。
RendererResult WindowRenderer::SetCloseHandler(std::function<void()> callback)
{
    const RendererResult checked = this->impl_->Check(true);
    if (!checked)
        return checked;
    if (!callback)
        return {"empty_callback", {}, {}};
    if (this->impl_->close)
        return {"duplicate_binding", {}, {}};
    this->impl_->close = std::move(callback);
    return {};
}

// 错误接收器在窗口创建前注册，事件错误仍由宿主本地化。
RendererResult WindowRenderer::SetErrorHandler(std::function<void(const RendererResult&)> callback)
{
    const RendererResult checked = this->impl_->Check(true);
    if (!checked)
        return checked;
    if (!callback)
        return {"empty_callback", {}, {}};
    if (this->impl_->errorHandler)
        return {"duplicate_binding", {}, {}};
    this->impl_->errorHandler = std::move(callback);
    return {};
}

// 默认动作按按钮 ID 校验，避免与 Win32 数字 ID 耦合。
RendererResult WindowRenderer::SetDefaultAction(std::string_view id)
{
    Impl::Control* control{};
    const RendererResult found = this->impl_->Find(id, NodeType::Button, control);
    if (!found)
        return found;
    if (!this->impl_->defaultAction.empty())
        return {"duplicate_binding", {}, std::string(id)};
    this->impl_->defaultAction = id;
    return {};
}

// 创建前集中检查，允许调用方提前显示准确的绑定错误。
RendererResult WindowRenderer::ValidateBindings() const
{
    return this->impl_->Validate();
}

// 系统窗口创建异常转换为结果，失败不展示残缺界面。
RendererResult WindowRenderer::Show(const RendererWindowOptions& options)
{
    const RendererResult checked = this->ValidateBindings();
    if (!checked)
        return checked;
    if (this->impl_->window != nullptr)
        return {"window_active", {}, {}};
    try
    {
        INITCOMMONCONTROLSEX common{sizeof(common), ICC_TAB_CLASSES};
        if (!InitCommonControlsEx(&common))
            return {"control_initialization_failed", {}, {}};
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW mainClass{sizeof(mainClass)};
        mainClass.hInstance = instance;
        mainClass.lpfnWndProc = Impl::WindowProc;
        mainClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        mainClass.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
        mainClass.lpszClassName = L"OpenST.WindowRenderer";
        if (!RegisterClassExW(&mainClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return {"class_registration_failed", {}, {}};
        WNDCLASSEXW pageClass = mainClass;
        pageClass.lpfnWndProc = Impl::PageProc;
        pageClass.lpszClassName = L"OpenST.WindowRendererPage";
        if (!RegisterClassExW(&pageClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return {"class_registration_failed", {}, {}};
        POINT cursor{};
        GetCursorPos(&cursor);
        MONITORINFO monitor{sizeof(monitor)};
        if (!GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &monitor))
            return {"monitor_query_failed", {}, {}};
        this->impl_->dpi = GetDpiForSystem();
        DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
        if (this->impl_->layout.resizable)
            style |= WS_THICKFRAME | WS_MAXIMIZEBOX;
        RECT bounds{0, 0, this->impl_->Scale(this->impl_->layout.width),
                    this->impl_->Scale(this->impl_->layout.height)};
        AdjustWindowRectExForDpi(&bounds, style, FALSE, WS_EX_CONTROLPARENT, this->impl_->dpi);
        const int width = std::min(bounds.right - bounds.left, monitor.rcWork.right - monitor.rcWork.left);
        const int height = std::min(bounds.bottom - bounds.top, monitor.rcWork.bottom - monitor.rcWork.top);
        const std::wstring title = this->impl_->text(this->impl_->layout.titleKey);
        const HWND window =
            CreateWindowExW(WS_EX_CONTROLPARENT, mainClass.lpszClassName, title.c_str(), style,
                            monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - width) / 2,
                            monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - height) / 2, width,
                            height, options.owner, nullptr, instance, this->impl_.get());
        if (window == nullptr)
            return {"window_creation_failed", {}, {}};
        this->impl_->dpi = GetDpiForWindow(window);
        this->impl_->RefreshFont();
        this->impl_->Arrange();
        if (options.icon != nullptr)
        {
            SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(options.icon));
            SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(options.icon));
        }
        const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
        DwmSetWindowAttribute(window, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
        ShowWindow(window, options.showCommand);
        return {};
    }
    catch (...)
    {
        if (this->impl_->window != nullptr)
            DestroyWindow(this->impl_->window);
        return {"callback_or_creation_failed", {}, {}};
    }
}

// 模态循环只暂时禁用原启用的 owner，所有退出路径恢复并保留 WM_QUIT 退出码。
RendererResult WindowRenderer::ShowModal(const RendererWindowOptions& options,
                                         std::function<bool(MSG&)> processThreadMessage)
{
    const RendererResult checked = this->impl_->Check();
    if (!checked)
        return checked;
    if (this->impl_->window != nullptr)
        return {"window_active", {}, {}};
    if (options.owner != nullptr && !IsWindow(options.owner))
        return {"owner_invalid", {}, {}};
    const HWND previousFocus = GetFocus();
    struct OwnerGuard
    {
        HWND owner;
        bool restore;
        // 只恢复本次禁用的 owner，允许在销毁模态窗口前提前恢复。
        void Restore()
        {
            if (this->restore && IsWindow(this->owner))
                EnableWindow(this->owner, TRUE);
            this->restore = false;
        }
        // 异常或创建失败同样恢复 owner；原先禁用的窗口始终保持禁用。
        ~OwnerGuard()
        {
            this->Restore();
        }
    } guard{options.owner, options.owner != nullptr && IsWindowEnabled(options.owner) != FALSE};
    if (guard.restore)
        EnableWindow(options.owner, FALSE);
    const RendererResult shown = this->Show(options);
    if (!shown)
        return shown;
    this->impl_->modal = true;
    this->impl_->modalCloseRequested = false;
    RendererResult result;
    bool quit = false;
    int exitCode = 0;
    try
    {
        while (this->impl_->window != nullptr && !this->impl_->modalCloseRequested)
        {
            MSG message{};
            const BOOL received = GetMessageW(&message, nullptr, 0, 0);
            if (received == -1)
            {
                result = {"message_loop_failed", {}, {}};
                break;
            }
            if (received == 0)
            {
                quit = true;
                exitCode = static_cast<int>(message.wParam);
                break;
            }
            if (this->ProcessDialogMessage(message))
                continue;
            if (processThreadMessage && processThreadMessage(message))
                continue;
            if (message.hwnd != nullptr)
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
    }
    catch (...)
    {
        result = {"callback_failed", {}, {}};
    }
    const bool restoreFocus = !quit && this->impl_->window != nullptr && GetForegroundWindow() == this->impl_->window &&
                              guard.restore && IsWindow(options.owner) && IsWindowVisible(options.owner);
    guard.Restore();
    if (this->impl_->window != nullptr)
        DestroyWindow(this->impl_->window);
    this->impl_->modal = false;
    this->impl_->modalCloseRequested = false;
    if (restoreFocus && IsWindow(options.owner))
    {
        SetForegroundWindow(options.owner);
        if (IsWindow(previousFocus) && IsWindowEnabled(previousFocus) &&
            (previousFocus == options.owner || IsChild(options.owner, previousFocus)))
            SetFocus(previousFocus);
    }
    if (quit)
        PostQuitMessage(exitCode);
    return result;
}
// 重新取草稿和选项，刷新失败由调用方显示业务错误。
RendererResult WindowRenderer::RefreshValues()
{
    const RendererResult checked = this->impl_->Check();
    if (!checked)
        return checked;
    if (this->impl_->window == nullptr)
        return {"window_missing", {}, {}};
    try
    {
        for (auto& [id, control] : this->impl_->controls)
        {
            if (control.node->type != NodeType::Select)
                continue;
            const RendererResult result = this->impl_->RefreshControl(control);
            if (!result)
                return result;
        }
        this->impl_->Arrange();
        return {};
    }
    catch (...)
    {
        this->impl_->refreshing = false;
        return {"callback_failed", {}, {}};
    }
}

// 文本刷新保留原生控件、焦点、草稿和当前页面。
RendererResult WindowRenderer::RefreshTexts()
{
    const RendererResult checked = this->impl_->Check();
    if (!checked)
        return checked;
    if (this->impl_->window == nullptr)
        return {"window_missing", {}, {}};
    try
    {
        SetWindowTextW(this->impl_->window, this->impl_->text(this->impl_->layout.titleKey).c_str());
        int index = 0;
        for (const Page& page : this->impl_->layout.pages)
        {
            if (!this->impl_->layout.showTabs)
                break;
            std::wstring title = this->impl_->text(page.titleKey);
            TCITEMW item{};
            item.mask = TCIF_TEXT;
            item.pszText = title.data();
            TabCtrl_SetItem(this->impl_->tabs, index++, &item);
        }
        for (auto& [id, control] : this->impl_->controls)
        {
            control.text = this->impl_->text(control.node->textKey);
            SetWindowTextW(control.label != nullptr ? control.label : control.window, control.text.c_str());
        }
        this->impl_->Arrange();
        return {};
    }
    catch (...)
    {
        return {"callback_failed", {}, {}};
    }
}

// 可用状态与临时忙状态分离，解除忙状态不错误启用原来禁用的按钮。
RendererResult WindowRenderer::SetEnabled(std::string_view id, bool enabled)
{
    const RendererResult checked = this->impl_->Check();
    if (!checked)
        return checked;
    const auto found = this->impl_->controls.find(id);
    if (found == this->impl_->controls.end())
        return {"unknown_id", {}, std::string(id)};
    found->second.enabled = enabled;
    if (found->second.window != nullptr)
        EnableWindow(found->second.window, enabled && !this->impl_->busy);
    return {};
}

// 忙状态阻止重复操作与关闭，保存每项独立可用状态。
RendererResult WindowRenderer::SetBusy(bool busy)
{
    const RendererResult checked = this->impl_->Check();
    if (!checked)
        return checked;
    if (busy && !this->impl_->busy)
        this->impl_->savedFocus = GetFocus();
    this->impl_->busy = busy;
    for (auto& [id, control] : this->impl_->controls)
        if (control.window != nullptr)
            EnableWindow(control.window, control.enabled && !busy);
    if (this->impl_->tabs != nullptr)
        EnableWindow(this->impl_->tabs, !busy);
    if (!busy && this->impl_->savedFocus != nullptr && IsChild(this->impl_->window, this->impl_->savedFocus) &&
        IsWindowEnabled(this->impl_->savedFocus))
    {
        SetFocus(this->impl_->savedFocus);
        this->impl_->savedFocus = nullptr;
    }
    return {};
}

// 当前页面身份供宿主决定恢复哪些业务字段。
std::string WindowRenderer::GetActivePageId() const
{
    return this->impl_->loaded ? this->impl_->layout.pages[this->impl_->pageIndex].id : std::string{};
}

// 控件所在页由实际布局得到，移动控件不需要修改业务页面映射。
std::string WindowRenderer::GetControlPageId(std::string_view id) const
{
    const auto found = this->impl_->controls.find(id);
    return found == this->impl_->controls.end() ? std::string{} : found->second.page;
}

// 字段错误文字由宿主本地化提供，并参与页面测量。
RendererResult WindowRenderer::SetFieldError(std::string_view id, std::wstring text)
{
    const RendererResult checked = this->impl_->Check();
    if (!checked)
        return checked;
    const auto found = this->impl_->controls.find(id);
    if (found == this->impl_->controls.end())
        return {"unknown_id", {}, std::string(id)};
    if (found->second.node->type != NodeType::Select)
        return {"wrong_control_type", {}, std::string(id)};
    found->second.error = std::move(text);
    if (found->second.errorWindow != nullptr)
        SetWindowTextW(found->second.errorWindow, found->second.error.c_str());
    this->impl_->Arrange();
    return {};
}

// 保存状态在窗口底部固定显示，长文本重新分配页面空间。
RendererResult WindowRenderer::SetStatus(std::wstring text)
{
    const RendererResult checked = this->impl_->Check();
    if (!checked)
        return checked;
    this->impl_->status = std::move(text);
    if (this->impl_->statusWindow != nullptr)
        SetWindowTextW(this->impl_->statusWindow, this->impl_->status.c_str());
    this->impl_->Arrange();
    return {};
}

// 使用消息延迟销毁，避免销毁正在分派的 std::function。
RendererResult WindowRenderer::RequestClose()
{
    const RendererResult checked = this->impl_->Check();
    if (!checked)
        return checked;
    if (this->impl_->busy)
        return {"busy", {}, {}};
    if (this->impl_->window != nullptr && !PostMessageW(this->impl_->window, Impl::CLOSE_MESSAGE, 0, 0))
        return {"close_post_failed", {}, {}};
    return {};
}

// 下拉框展开优先处理 Enter/Esc，其余交给默认动作和原生 Tab 导航。
bool WindowRenderer::ProcessDialogMessage(MSG& message)
{
    if (!this->impl_->Check() || this->impl_->window == nullptr)
        return false;
    if (message.hwnd != this->impl_->window && !IsChild(this->impl_->window, message.hwnd))
        return false;
    if (message.message == WM_KEYDOWN && (message.wParam == VK_RETURN || message.wParam == VK_ESCAPE))
    {
        for (const auto& [id, control] : this->impl_->controls)
        {
            if (control.node->type == NodeType::Select && SendMessageW(control.window, CB_GETDROPPEDSTATE, 0, 0))
                return false;
        }
        if (!this->impl_->busy)
        {
            if (message.wParam == VK_ESCAPE)
                this->impl_->close();
            else
                this->impl_->Invoke(this->impl_->defaultAction);
        }
        return true;
    }
    const bool handled = IsDialogMessageW(this->impl_->window, &message) != FALSE;
    if (handled)
        this->impl_->RevealFocus();
    return handled;
}

// HWND 仅借用，禁止宿主销毁或改变 Renderer 所有权。
HWND WindowRenderer::NativeHandle() const noexcept
{
    return this->impl_->window;
}
} // namespace open_st
