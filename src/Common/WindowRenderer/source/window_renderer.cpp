// 将布局树和宿主回调绑定到 Win32 控件，管理排版、输入及模态消息循环。

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
        std::function<RendererBoolResult()> readBool;
        std::function<RendererChangeResult(bool)> changeBool;
        bool checked{};
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

    // 把无法同步返回的控件事件错误交给宿主处理。
    // 入参：result：调用期间借用的结构化错误，不包含用户字段正文。
    // 返回：无返回值；无错误接收器或接收器抛异常时写调试输出，不让错误回调异常穿过系统边界。
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

    // 滚动当前页面使键盘导航到的控件进入可见区域。
    // 入参：无。
    // 返回：无返回值；焦点不在页面直接子控件或本就可见时不滚动，不改变焦点对象。
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

    // 检查操作是否在所属 UI 线程以及当前是否允许更改绑定。
    // 入参：mutableBindings：true 要求尚未创建窗口，false 只检查线程。
    // 返回：检查通过返回空错误码；跨线程返回 wrong_thread，窗口活动时改绑定返回 window_active。
    RendererResult Check(bool mutableBindings = false) const
    {
        if (GetCurrentThreadId() != this->thread)
            return {"wrong_thread", {}, {}};
        if (mutableBindings && this->window != nullptr)
            return {"window_active", {}, {}};
        return {};
    }

    // 将布局 DIP 转换为当前窗口的物理像素。
    // 入参：value：以 96 DPI 为基准的 DIP 长度。
    // 返回：按当前 dpi 四舍五入后的物理像素长度。
    int Scale(int value) const
    {
        return MulDiv(value, static_cast<int>(this->dpi), 96);
    }

    // 递归登记布局节点和叶控件，建立后续绑定的查找表。
    // 入参：node：自有布局树内的节点，地址在索引使用期间须稳定；page：所属页面 ID，底部控件为空。
    // 返回：无返回值；保存节点借用指针及叶控件状态，不创建 HWND。
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

    // 查找可进行绑定的指定类型控件。
    // 入参：id：目标布局控件 ID；type：要求的节点类型；output：输出参数，成功时接收内部控件借用指针。
    // 返回：线程、窗口状态及类型均满足时返回空错误码；失败返回结构化错误且不修改 output。
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

    // 检查显示窗口所需的布局、默认动作和控件回调是否完整。
    // 入参：无。
    // 返回：完整时返回空错误码；缺失布局、窗口绑定或某控件绑定时返回对应结构化错误。
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
            if (control.node->type == NodeType::Checkbox && (!control.readBool || !control.changeBool))
                return {"field_binding_missing", {}, id};
            if (control.node->type == NodeType::Button && !control.action)
                return {"action_missing", {}, id};
        }
        return {};
    }

    // 测量当前字体下单行文本的自然宽度。
    // 入参：value：借用的宽字符文本。
    // 返回：物理像素宽度；无法取得设备上下文时回退为 100 DIP 对应的像素值。
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

    // 测量指定宽度内文本换行所需的高度。
    // 入参：value：借用的宽字符文本；width：可用宽度，单位物理像素。
    // 返回：空文本返回 0；其余返回至少 20 DIP 的像素高度，设备上下文不可用时回退到 24 DIP。
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

    // 按当前 DPI 获取系统消息字体并应用到窗口控件。
    // 入参：无。
    // 返回：无返回值；字体创建失败时用共享默认 GUI 字体，替换后释放原自有字体。
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

    // 测量或放置一棵页面节点树，统一处理文字、输入框与字段错误。
    // 入参：node：待布局节点；x、y：未扣除滚动偏移的页面像素原点；width：可用物理像素宽度；place：true 放置控件，false
    // 只测量。
    // 返回：整棵节点占用的物理像素高度；放置时将 y 减去当前滚动偏移。
    int ArrangeNode(const Node& node, int x, int y, int width, bool place);
    // 重排底部按钮、状态、标签页及当前页面，并更新滚动范围。
    // 入参：无。
    // 返回：无返回值；窗口不存在或正在排版时不操作，按钮可换行，页面内容在独立视口内裁剪。
    void Arrange();
    // 按照布局顺序创建原生控件并初始化显示草稿。
    // 入参：无。
    // 返回：窗口控件、初始值及布局建立成功时为 true；创建或初始读取失败时为 false，已建子窗口由所属窗口统一回收。
    bool CreateControls();
    // 重读下拉框或复选框的草稿并刷新可选项及字段错误。
    // 入参：control：输入输出内部控件状态，已绑定所需读取和选项查询回调。
    // 返回：刷新流程完成返回空错误码；回调异常、重复选项或控件更新失败返回结构化错误；业务读取失败显示宿主错误，不调用变更回调。
    RendererResult RefreshControl(Control& control);
    // 将原生按钮、复选框和下拉框通知转换为宿主回调。
    // 入参：wParam：WM_COMMAND 的控件编号及通知码；lParam：发出通知的 HWND。
    // 返回：无返回值；忙或程序刷新时忽略，拒绝输入恢复已接受值，未在本层收敛的异常交由窗口过程处理。
    void Command(WPARAM wParam, LPARAM lParam);
    // 按稳定控件 ID 执行可用按钮的业务动作。
    // 入参：id：拟触发按钮的布局 ID。
    // 返回：无返回值；忙、禁用、未知 ID 或无动作时忽略；动作异常由外层消息边界处理。
    void Invoke(std::string_view id);
    // 恢复 HWND 关联的渲染器并在异常边界内处理主窗口消息。
    // 入参：window：目标窗口；message：Win32 消息编号；wParam、lParam：该消息的附加参数，创建时 lParam 提供借用 Impl
    // 指针。
    // 返回：返回实例或默认窗口过程的 LRESULT；处理异常时 WM_CREATE 返回 -1 取消创建，其余返回 0 并报告错误。
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    // 处理页面视口的控件通知、滚轮和滚动条消息。
    // 入参：window：页面视口 HWND；message：Win32 消息编号；wParam、lParam：消息附加参数，创建时用于关联借用 Impl。
    // 返回：已消费的通知或滚动返回 0；其他消息交给 DefWindowProcW，回调异常被捕获并报告。
    static LRESULT CALLBACK PageProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    // 分派主窗口控件事件、布局变化和宿主关闭请求。
    // 入参：target：接收消息的主窗口；message：Win32 消息编号；wParam、lParam：对应附加参数。
    // 返回：已处理消息返回 Win32 约定结果；WM_CREATE 控件创建失败返回 -1，其余未处理消息返回默认窗口过程结果。
    LRESULT Message(HWND target, UINT message, WPARAM wParam, LPARAM lParam);
};

// 测量或放置一棵页面节点树，统一处理文字、输入框与字段错误。
// 入参：node：待布局节点；x、y：未扣除滚动偏移的页面像素原点；width：可用物理像素宽度；place：true 放置控件，false
// 只测量。
// 返回：整棵节点占用的物理像素高度；放置时将 y 减去当前滚动偏移。
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
    if (node.type == NodeType::Checkbox)
        height = std::max(this->Scale(24),
                          this->TextHeight(control.text, std::max(1, actualWidth - this->Scale(24))) + this->Scale(4));
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

// 重排底部按钮、状态、标签页及当前页面，并更新滚动范围。
// 入参：无。
// 返回：无返回值；窗口不存在或正在排版时不操作，按钮可换行，页面内容在独立视口内裁剪。
void WindowRenderer::Impl::Arrange()
{
    if (this->window == nullptr || this->arranging)
        return;
    this->arranging = true;
    struct ArrangeGuard
    {
        bool& arranging;
        // 无论排版正常结束还是异常退出，都恢复下一次排版的准入状态。
        // 入参：无。
        // 返回：析构函数无返回值，不传播异常。
        ~ArrangeGuard() noexcept
        {
            this->arranging = false;
        }
    };
    const ArrangeGuard arrangeGuard{this->arranging};
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
    const Page& page = this->layout.pages[this->pageIndex];
    // 视口没有边框；先按无滚动条的完整宽度测量，避免旧滚动条让本可容纳的页面继续换行。
    this->contentHeight = this->ArrangeNode(page.content, 0, 0, width, false);
    const bool needsScroll = this->contentHeight > pageHeight;
    ShowScrollBar(this->viewport, SB_VERT, needsScroll ? TRUE : FALSE);
    RECT view{};
    GetClientRect(this->viewport, &view);
    const int contentWidth = std::max(1L, view.right);
    if (contentWidth != width)
        this->contentHeight = this->ArrangeNode(page.content, 0, 0, contentWidth, false);
    this->scroll = std::clamp(this->scroll, 0, std::max(0, this->contentHeight - pageHeight));
    SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS};
    info.nMin = 0;
    info.nMax = std::max(0, this->contentHeight - 1);
    info.nPage = static_cast<UINT>(pageHeight);
    info.nPos = this->scroll;
    SetScrollInfo(this->viewport, SB_VERT, &info, TRUE);
    this->ArrangeNode(page.content, 0, 0, contentWidth, true);
    for (const auto& [id, control] : this->controls)
    {
        const bool visible = control.page.empty() || control.page == page.id;
        for (HWND child : {control.window, control.label, control.errorWindow})
            if (child != nullptr)
                ShowWindow(child, visible ? SW_SHOWNA : SW_HIDE);
    }
}

// 按照布局顺序创建原生控件并初始化显示草稿。
// 入参：无。
// 返回：窗口控件、初始值及布局建立成功时为 true；创建或初始读取失败时为 false，已建子窗口由所属窗口统一回收。
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
    const std::function<bool(const Node&, HWND)> create =
        // 按布局顺序递归创建叶控件，使原生 Tab 顺序与页面一致。
        // 入参：node：当前布局节点；parent：叶控件所属 HWND；捕获 Impl、模块句柄、递增 controlId 及递归函数引用。
        // 返回：当前节点及其子树全部创建成功时为 true；任一 HWND 创建失败时为 false，控件由 parent 管理。
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
        else if (node.type == NodeType::Checkbox)
        {
            control.window = CreateWindowExW(0, L"BUTTON", control.text.c_str(),
                                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX | BS_MULTILINE, 0, 0,
                                             1, 1, parent, id, instance, nullptr);
            control.errorWindow = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_NOPREFIX, 0, 0, 1, 1,
                                                  parent, nullptr, instance, nullptr);
            if (!control.errorWindow)
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
        if (control.node->type == NodeType::Select || control.node->type == NodeType::Checkbox)
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

// 重读下拉框或复选框的草稿并刷新可选项及字段错误。
// 入参：control：输入输出内部控件状态，已绑定所需读取和选项查询回调。
// 返回：刷新流程完成返回空错误码；回调异常、重复选项或控件更新失败返回结构化错误；业务读取失败显示宿主错误，不调用变更回调。
RendererResult WindowRenderer::Impl::RefreshControl(Control& control)
{
    if (control.node->type == NodeType::Checkbox)
    {
        try
        {
            const RendererBoolResult value = control.readBool();
            if (value.success)
                control.checked = value.value;
            SendMessageW(control.window, BM_SETCHECK, control.checked ? BST_CHECKED : BST_UNCHECKED, 0);
            control.error = value.error;
            SetWindowTextW(control.errorWindow, control.error.c_str());
            return {};
        }
        catch (...)
        {
            this->Report({"callback_failed", {}, control.node->id});
            return {"callback_failed", {}, control.node->id};
        }
    }
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

// 按稳定控件 ID 执行可用按钮的业务动作。
// 入参：id：拟触发按钮的布局 ID。
// 返回：无返回值；忙、禁用、未知 ID 或无动作时忽略；动作异常由外层消息边界处理。
void WindowRenderer::Impl::Invoke(std::string_view id)
{
    const auto found = this->controls.find(id);
    if (this->busy || found == this->controls.end() || !found->second.enabled || !found->second.action)
        return;
    found->second.action();
}

// 将原生按钮、复选框和下拉框通知转换为宿主回调。
// 入参：wParam：WM_COMMAND 的控件编号及通知码；lParam：发出通知的 HWND。
// 返回：无返回值；忙或程序刷新时忽略，拒绝输入恢复已接受值，未在本层收敛的异常交由窗口过程处理。
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
        if (control.node->type == NodeType::Checkbox && HIWORD(wParam) == BN_CLICKED)
        {
            const bool previous = control.checked;
            const bool value = SendMessageW(control.window, BM_GETCHECK, 0, 0) == BST_CHECKED;
            RendererChangeResult result;
            bool callbackFailed = false;
            try
            {
                result = control.changeBool(value);
            }
            catch (...)
            {
                result.accepted = false;
                callbackFailed = true;
            }
            control.checked = result.accepted ? value : previous;
            SendMessageW(control.window, BM_SETCHECK, control.checked ? BST_CHECKED : BST_UNCHECKED, 0);
            control.error = result.error;
            SetWindowTextW(control.errorWindow, control.error.c_str());
            if (callbackFailed)
                this->Report({"callback_failed", {}, id});
            this->Arrange();
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

// 恢复 HWND 关联的渲染器并在异常边界内处理主窗口消息。
// 入参：window：目标窗口；message：Win32 消息编号；wParam、lParam：该消息的附加参数，创建时 lParam 提供借用 Impl 指针。
// 返回：返回实例或默认窗口过程的 LRESULT；处理异常时 WM_CREATE 返回 -1 取消创建，其余返回 0 并报告错误。
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

// 处理页面视口的控件通知、滚轮和滚动条消息。
// 入参：window：页面视口 HWND；message：Win32 消息编号；wParam、lParam：消息附加参数，创建时用于关联借用 Impl。
// 返回：已消费的通知或滚动返回 0；其他消息交给 DefWindowProcW，回调异常被捕获并报告。
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

// 分派主窗口控件事件、布局变化和宿主关闭请求。
// 入参：target：接收消息的主窗口；message：Win32 消息编号；wParam、lParam：对应附加参数。
// 返回：已处理消息返回 Win32 约定结果；WM_CREATE 控件创建失败返回 -1，其余未处理消息返回默认窗口过程结果。
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

// 创建通用窗口渲染器并固定所属 UI 线程。
// 入参：无。
// 返回：构造函数无返回值；尚未创建窗口或加载布局，内部存储分配失败可抛出异常。
WindowRenderer::WindowRenderer() : impl_(std::make_unique<Impl>()) {}

// 停止窗口分派并释放渲染器拥有的窗口、字体及注册回调。
// 入参：无。
// 返回：析构函数无返回值；宿主借用的 HWND 在销毁后失效。
WindowRenderer::~WindowRenderer()
{
    if (this->impl_->window != nullptr)
        DestroyWindow(this->impl_->window);
    if (this->impl_->font != nullptr && this->impl_->font != GetStockObject(DEFAULT_GUI_FONT))
        DeleteObject(this->impl_->font);
}

// 解析并复制完整窗口布局，为控件绑定建立索引。
// 入参：document：调用期间借用的布局 JSON。
// 返回：成功返回空错误码并清空旧绑定；解析或状态检查失败返回结构化错误，保留之前布局。
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

// 注册窗口所有文本键使用的本地化查询器。
// 入参：callback：接收文本键并返回宽字符文本的回调，移入渲染器；其捕获对象须保持存活。
// 返回：成功时返回空错误码的 RendererResult；失败返回含错误码、路径或控件 ID
// 的结构化结果；须在显示前注册，空回调或重复绑定被拒绝。
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

// 把下拉框连接到宿主字符串草稿的读取和变更操作。
// 入参：id：布局中的下拉框
// ID；read：返回当前字符串及读取结果的回调；change：接收拟选字符串并返回是否接受的回调；回调移入渲染器。
// 返回：成功时返回空错误码的 RendererResult；失败返回含错误码、路径或控件 ID
// 的结构化结果；非下拉框、空回调或重复绑定被拒绝，拒绝变更时恢复已接受值。
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

// 把复选框连接到宿主布尔草稿的读取和变更操作。
// 入参：id：布局中的复选框 ID；read：读取当前布尔值的回调；change：接收新布尔值并返回是否接受的回调；回调移入渲染器。
// 返回：成功时返回空错误码的 RendererResult；失败返回含错误码、路径或控件 ID
// 的结构化结果；类型不符、空回调或重复绑定被拒绝。
RendererResult WindowRenderer::BindBool(std::string_view id, std::function<RendererBoolResult()> read,
                                        std::function<RendererChangeResult(bool)> change)
{
    Impl::Control* control{};
    const RendererResult found = this->impl_->Find(id, NodeType::Checkbox, control);
    if (!found)
        return found;
    if (!read || !change)
        return {"empty_callback", {}, std::string(id)};
    if (control->readBool || control->changeBool)
        return {"duplicate_binding", {}, std::string(id)};
    control->readBool = std::move(read);
    control->changeBool = std::move(change);
    return {};
}

// 为下拉框注册动态选项查询。
// 入参：id：布局中的下拉框 ID；query：返回稳定值和显示名称列表的回调，移入渲染器。
// 返回：成功时返回空错误码的 RendererResult；失败返回含错误码、路径或控件 ID
// 的结构化结果；选项值唯一性在实际读取选项时检查。
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

// 将布局按钮绑定到宿主业务动作。
// 入参：id：布局中的按钮 ID；callback：按钮触发时同步调用的无参动作，移入渲染器。
// 返回：成功时返回空错误码的 RendererResult；失败返回含错误码、路径或控件 ID
// 的结构化结果；非按钮、空回调或重复绑定被拒绝。
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

// 把标题栏关闭及 Esc 操作交由宿主决定如何处理。
// 入参：callback：无参关闭请求回调，移入渲染器；需要退出时由宿主调用 RequestClose。
// 返回：成功时返回空错误码的 RendererResult；失败返回含错误码、路径或控件 ID
// 的结构化结果；须在显示前注册，空回调或重复绑定被拒绝。
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

// 为控件事件错误注册宿主接收器。
// 入参：callback：借用 RendererResult 处理结构化错误的回调，移入渲染器；宿主负责本地化。
// 返回：成功时返回空错误码的 RendererResult；失败返回含错误码、路径或控件 ID
// 的结构化结果；须在显示前注册，空回调或重复绑定被拒绝。
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

// 指定未被原生控件消费的 Enter 键所触发的按钮。
// 入参：id：布局中的按钮 ID，复制到渲染器。
// 返回：成功时返回空错误码的 RendererResult；失败返回含错误码、路径或控件 ID 的结构化结果；非按钮或重复指定被拒绝。
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

// 检查布局和全部必需回调是否满足窗口创建条件。
// 入参：无。
// 返回：布局及绑定完整时返回空错误码；缺失或无效时返回定位到对应控件的结构化错误。
RendererResult WindowRenderer::ValidateBindings() const
{
    return this->impl_->Validate();
}

// 校验绑定并创建和显示非模态原生窗口。
// 入参：options：借用的 owner 和 icon，以及初始显示命令；图标由宿主保持存活。
// 返回：成功时返回空错误码的 RendererResult；失败返回含错误码、路径或控件 ID
// 的结构化结果；创建过程失败会回收本次窗口资源。
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

// 创建并同步运行模态窗口，暂时禁用原本启用的所属窗口。
// 入参：options：借用所属窗口、图标及显示命令；processThreadMessage：可选线程消息回调，返回 true 表示已消费消息。
// 返回：正常关闭或收到 WM_QUIT 时返回空错误码；创建、消息循环或回调失败返回结构化错误；恢复本次禁用的 owner，并原码重投
// WM_QUIT。
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
        // 恢复本次模态调用暂时禁用的所属窗口。
        // 入参：无。
        // 返回：无返回值；仅窗口仍存在且 restore 为 true 时重新启用，随后清除 restore，可重复调用。
        void Restore()
        {
            if (this->restore && IsWindow(this->owner))
                EnableWindow(this->owner, TRUE);
            this->restore = false;
        }
        // 在模态调用退出或异常展开时恢复所属窗口启用状态。
        // 入参：无。
        // 返回：析构函数无返回值；委托 Restore，原先禁用的窗口保持禁用。
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
// 重读控件草稿及动态选项，使显示值与宿主当前状态一致。
// 入参：无。
// 返回：成功时返回空错误码的 RendererResult；失败返回含错误码、路径或控件 ID
// 的结构化结果；不调用变更回调，结构化刷新错误停止本次刷新，业务读取失败显示控件错误；已刷新的控件不回滚。
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
            if (control.node->type != NodeType::Select && control.node->type != NodeType::Checkbox)
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

// 重新查询全部本地化文字并调整窗口布局。
// 入参：无。
// 返回：文本查询及更新请求完成时返回空错误码；状态无效或回调异常返回结构化错误；保留原生控件、焦点和草稿。
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

// 设置单个控件的宿主可用状态。
// 入参：id：目标控件 ID；enabled：true 允许交互，false 禁用。
// 返回：成功时返回空错误码的 RendererResult；失败返回含错误码、路径或控件 ID
// 的结构化结果；窗口忙时控件仍禁用，解除忙状态后恢复该值。
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

// 临时禁止窗口交互和关闭请求，结束后恢复各控件状态。
// 入参：busy：true 进入忙状态，false 恢复交互。
// 返回：成功时返回空错误码的 RendererResult；失败返回含错误码、路径或控件 ID
// 的结构化结果；解除时尝试恢复之前仍有效且可用的焦点控件。
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

// 向宿主提供当前页面的业务标识。
// 入参：无。
// 返回：当前页面 ID 的字符串副本；尚未加载布局时返回空字符串。
std::string WindowRenderer::GetActivePageId() const
{
    return this->impl_->loaded ? this->impl_->layout.pages[this->impl_->pageIndex].id : std::string{};
}

// 查询某控件在当前布局中所属的页面。
// 入参：id：待查询的控件 ID。
// 返回：所属页面 ID 的副本；未知控件或底部公共控件返回空字符串。
std::string WindowRenderer::GetControlPageId(std::string_view id) const
{
    const auto found = this->impl_->controls.find(id);
    return found == this->impl_->controls.end() ? std::string{} : found->second.page;
}

// 在输入控件附近显示宿主提供的字段错误。
// 入参：id：下拉框或复选框 ID；text：已本地化的错误文字，移入控件状态，空串用于清除。
// 返回：成功时返回空错误码的 RendererResult；失败返回含错误码、路径或控件 ID 的结构化结果；未知 ID
// 或非输入控件被拒绝，错误参与重新布局。
RendererResult WindowRenderer::SetFieldError(std::string_view id, std::wstring text)
{
    const RendererResult checked = this->impl_->Check();
    if (!checked)
        return checked;
    const auto found = this->impl_->controls.find(id);
    if (found == this->impl_->controls.end())
        return {"unknown_id", {}, std::string(id)};
    if (found->second.node->type != NodeType::Select && found->second.node->type != NodeType::Checkbox)
        return {"wrong_control_type", {}, std::string(id)};
    found->second.error = std::move(text);
    if (found->second.errorWindow != nullptr)
        SetWindowTextW(found->second.errorWindow, found->second.error.c_str());
    this->impl_->Arrange();
    return {};
}

// 设置窗口底部的公共状态文字。
// 入参：text：宿主已本地化的状态文本，移入渲染器，空串用于清除。
// 返回：成功时返回空错误码的 RendererResult；失败返回含错误码、路径或控件 ID
// 的结构化结果；更新已存在的状态控件并重新计算布局。
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

// 投递延迟关闭请求，避免销毁正在执行的事件回调。
// 入参：无。
// 返回：投递成功或窗口已不存在时返回空错误码；忙状态、线程检查或投递失败返回结构化错误，成功不表示窗口已同步销毁。
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

// 为窗口处理 Enter、Esc 和 Tab 等键盘导航消息。
// 入参：message：待处理的 Win32 消息引用，所属窗口及子控件消息才参与处理。
// 返回：消息被消费时为 true；无有效窗口、消息不属于本窗口或下拉框应先自行处理时为 false。
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

// 向宿主提供用于窗口协调的原生句柄。
// 入参：无。
// 返回：当前 HWND 的借用值；尚未创建或已销毁时为 nullptr，宿主不得据此取得销毁所有权。
HWND WindowRenderer::NativeHandle() const noexcept
{
    return this->impl_->window;
}
} // namespace open_st
