// 使用系统 RichEdit 提供原位纯文本与 IME；不包含业务事务或消息循环。
#include "edit_support.h"
#include "inline_text_editor_test_access.h"
#include <algorithm>
#include <climits>
#include <commctrl.h>
#include <imm.h>
#include <inline_text_editor.h>
#include <richedit.h>

namespace open_st
{
struct InlineTextEditor::Impl
{
    HWND window{}, edit{}, status{};
    HFONT font{};
    HMODULE rich{};
    InlineTextOptions options;
    InlineTextCallbacks callbacks;
    bool composing{}, finished{}, ready{}, notifying{}, valid{true};
    wchar_t pendingHigh{};
    DWORD thread{GetCurrentThreadId()};
    std::function<HWND()> foregroundQuery;

    // 查询实际前台；测试可注入环境结果，失败按无前台处理。
    // 入参：无。返回：前台窗口借用值。
    HWND Foreground() const noexcept
    {
        try
        {
            return this->foregroundQuery ? this->foregroundQuery() : GetForegroundWindow();
        }
        catch (...)
        {
            return nullptr;
        }
    }

    // 执行通用编辑命令，复用原生及本组件的粘贴前置校验。入参：command。返回：已分派时 true。
    bool Invoke(InlineEditCommand command) noexcept
    {
        if (GetCurrentThreadId() != this->thread || !this->edit || GetFocus() != this->edit ||
            this->Foreground() != this->window || this->composing || this->finished || this->notifying)
            return false;
        switch (command)
        {
        case InlineEditCommand::SelectAll:
            SendMessageW(this->edit, EM_SETSEL, 0, -1);
            break;
        case InlineEditCommand::Copy:
            SendMessageW(this->edit, WM_COPY, 0, 0);
            break;
        case InlineEditCommand::Paste:
            SendMessageW(this->edit, WM_PASTE, 0, 0);
            break;
        case InlineEditCommand::Cut:
            SendMessageW(this->edit, WM_CUT, 0, 0);
            break;
        case InlineEditCommand::Undo:
            SendMessageW(this->edit, EM_UNDO, 0, 0);
            break;
        case InlineEditCommand::Redo:
            SendMessageW(this->edit, EM_REDO, 0, 0);
            break;
        default:
            return false;
        }
        return true;
    }

    // 把系统换行统一为 LF，RichEdit 选区索引每个换行只占一个字符。入参：value。返回：标准文本。
    static std::wstring Normalize(std::wstring_view value)
    {
        std::wstring result;
        result.reserve(value.size());
        for (std::size_t index = 0; index < value.size(); ++index)
        {
            if (value[index] == L'\r')
            {
                result.push_back(L'\n');
                if (index + 1 < value.size() && value[index + 1] == L'\n')
                    ++index;
            }
            else
                result.push_back(value[index]);
        }
        return result;
    }
    // 复用公共读取后统一正文换行。入参：无。返回：LF 正文，不改写控件。
    std::wstring Text() const
    {
        return Normalize(renderer_detail::ReadEditText(this->edit));
    }

    // 重申物理字号和布局，不重新设置文本或撤销历史。入参：无。返回：无。
    void ApplyPhysicalLayout() noexcept
    {
        if (!this->edit)
            return;
        SendMessageW(this->edit, EM_SETZOOM, 0, 0);
        SendMessageW(this->edit, WM_SETFONT, reinterpret_cast<WPARAM>(this->font), TRUE);
        RECT content{0, 0, this->options.bounds.right - this->options.bounds.left,
                     this->options.bounds.bottom - this->options.bounds.top};
        SendMessageW(this->edit, EM_SETRECT, 0, reinterpret_cast<LPARAM>(&content));
        // NULL 目标设备的屏幕模式使用 1 禁止软换行；0 实测恢复按窗口换行。
        SendMessageW(this->edit, EM_SETTARGETDEVICE, 0, 1);
    }

    // 通知原生输入拒绝，错误回调不能越过窗口过程。入参：code。返回：无。
    void Reject(std::string_view code = "text_limit") noexcept
    {
        if (this->finished || this->notifying)
            return;
        try
        {
            const std::function<void(std::string_view)> callback = this->callbacks.error;
            if (callback)
                callback(code);
        }
        catch (...)
        {
        }
    }

    // 检查 UTF16 及长度，不截断输入。入参：value。返回：完整有效时 true。
    bool Valid(std::wstring_view value) const noexcept
    {
        if (value.size() > this->options.maxLength || value.find(L'\0') != std::wstring_view::npos)
            return false;
        try
        {
            (void)renderer_detail::EditUtf8(value);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }
    // 检查选区替换是否完整可接受。入参：value 为替换文本。返回：不超限且编码有效为 true。
    bool Fits(std::wstring_view value) const
    {
        CHARRANGE selection{};
        SendMessageW(this->edit, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&selection));
        const std::wstring current = this->Text();
        const std::size_t first = std::min(current.size(), static_cast<std::size_t>(std::max(0L, selection.cpMin)));
        const std::size_t last =
            std::min(current.size(), static_cast<std::size_t>(std::max(selection.cpMin, selection.cpMax)));
        std::wstring candidate = current.substr(0, first);
        candidate.append(Normalize(value));
        candidate.append(current.substr(last));
        return this->Valid(candidate);
    }
    // 通知完整文本，拒绝时保留原文供修正。入参：无。返回：无。
    void Changed()
    {
        if (!this->ready || this->notifying || this->finished || this->composing || !this->edit)
            return;
        const std::wstring value = this->Text();
        this->valid = this->Valid(value);
        if (!this->valid)
            return;
        const std::function<bool(std::wstring_view)> callback = this->callbacks.change;
        this->notifying = true;
        try
        {
            this->valid = callback(value);
        }
        catch (...)
        {
            this->valid = false;
        }
        this->notifying = false;
    }
    // 请求宿主排队结束，回调期间不得同步删除组件。入参：accept。返回：请求状态。
    InlineTextRequest Finish(bool accept)
    {
        if (GetCurrentThreadId() != this->thread)
            return InlineTextRequest::Rejected;
        if (this->finished || !this->window)
            return InlineTextRequest::Finished;
        if (this->notifying || (accept && this->composing))
            return InlineTextRequest::Rejected;
        if (!accept && this->composing)
        {
            const HIMC context = ImmGetContext(this->edit);
            if (context)
            {
                ImmNotifyIME(context, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
                ImmReleaseContext(this->edit, context);
            }
            this->composing = false;
        }
        if (accept)
        {
            this->Changed();
            if (!this->valid || !this->window)
                return InlineTextRequest::Rejected;
        }
        const std::wstring value = this->Text();
        bool accepted{};
        const std::function<bool(std::wstring_view, bool)> callback = this->callbacks.finish;
        this->notifying = true;
        try
        {
            accepted = callback(value, accept);
        }
        catch (...)
        {
            accepted = false;
        }
        this->notifying = false;
        if (!accepted)
            return InlineTextRequest::Rejected;
        this->finished = true;
        if (this->edit)
            SendMessageW(this->edit, EM_SETREADONLY, TRUE, 0);
        return InlineTextRequest::Queued;
    }
    // 分派输入优先级并在粘贴前检查完整长度。入参：系统 subclass 参数。返回：处理结果。
    static LRESULT CALLBACK EditProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data)
    {
        Impl* self = reinterpret_cast<Impl*>(data);
        try
        {
            if (message == WM_NCDESTROY)
            {
                RemoveWindowSubclass(window, EditProc, 1);
                self->edit = nullptr;
                return DefSubclassProc(window, message, wParam, lParam);
            }
            // 排队结束后，原生撤销及程序替换也必须冻结，Resume 才恢复输入。
            if (self->finished &&
                (message == WM_CHAR || message == WM_UNICHAR || message == WM_KEYDOWN || message == WM_SYSKEYDOWN ||
                 message == WM_PASTE || message == WM_CUT || message == WM_CLEAR || message == WM_UNDO ||
                 message == EM_UNDO || message == EM_REDO || message == WM_SETTEXT || message == EM_REPLACESEL ||
                 message == EM_SETTEXTEX || message == EM_STREAMIN || message == WM_IME_STARTCOMPOSITION ||
                 message == WM_IME_COMPOSITION || message == WM_IME_ENDCOMPOSITION))
                return 0;
            if (message == WM_DPICHANGED || message == WM_DPICHANGED_BEFOREPARENT ||
                message == WM_DPICHANGED_AFTERPARENT)
            {
                self->ApplyPhysicalLayout();
                return 0;
            }
            if (message == WM_IME_STARTCOMPOSITION)
                self->composing = true;
            if (message == WM_IME_ENDCOMPOSITION)
            {
                const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
                self->composing = false;
                self->Changed();
                return result;
            }
            if (message == WM_KEYDOWN)
            {
                const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                if (control && wParam == 'S')
                    return 0;
                if (!self->composing &&
                    ((control && wParam == 'V') || (wParam == VK_INSERT && (GetKeyState(VK_SHIFT) & 0x8000) != 0)))
                {
                    (void)self->Invoke(InlineEditCommand::Paste);
                    return 0;
                }
                if (!self->composing && control && (wParam == 'Z' || wParam == 'Y'))
                {
                    (void)self->Invoke(wParam == 'Z' ? InlineEditCommand::Undo : InlineEditCommand::Redo);
                    return 0;
                }
                if (!self->composing && (wParam == VK_ESCAPE || (control && wParam == VK_RETURN)))
                {
                    (void)self->Finish(wParam == VK_RETURN);
                    return 0;
                }
            }
            if (message == WM_CHAR && ((GetKeyState(VK_CONTROL) & 0x8000) != 0) &&
                (wParam == L'\r' || wParam == L'\n' || wParam == 19 ||
                 (!self->composing && (wParam == 22 || wParam == 25 || wParam == 26))))
                return 0;
            if (message == EM_REPLACESEL && lParam && !self->Fits(reinterpret_cast<const wchar_t*>(lParam)))
            {
                self->Reject();
                return 0;
            }
            if (message == WM_SETTEXT && self->ready && lParam &&
                !self->Valid(reinterpret_cast<const wchar_t*>(lParam)))
            {
                self->Reject();
                return 0;
            }
            if (message == WM_PASTE)
            {
                if (!OpenClipboard(window))
                    return 0;
                const HANDLE handle = GetClipboardData(CF_UNICODETEXT);
                const wchar_t* text = handle ? static_cast<const wchar_t*>(GlobalLock(handle)) : nullptr;
                std::wstring value;
                try
                {
                    if (text)
                        value = text;
                }
                catch (...)
                {
                    if (text)
                        GlobalUnlock(handle);
                    CloseClipboard();
                    throw;
                }
                if (text)
                    GlobalUnlock(handle);
                CloseClipboard();
                if (!text || !self->Fits(value))
                {
                    self->Reject();
                    return 0;
                }
                return DefSubclassProc(window, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(value.c_str()));
            }
            if (message == WM_CHAR && (wParam >= 32 || wParam == L'\r' || wParam == L'\t'))
            {
                const wchar_t character = static_cast<wchar_t>(wParam);
                if (character < 0xd800 || character > 0xdfff)
                {
                    self->pendingHigh = 0;
                    if (!self->Fits(std::wstring_view(&character, 1)))
                    {
                        self->Reject();
                        return 0;
                    }
                }
                else if (character <= 0xdbff)
                {
                    // 暂存高代理，配齐后作为一次原生替换，避免中间值和分裂的撤销。
                    self->pendingHigh = character;
                    return 0;
                }
                else
                {
                    const wchar_t pair[]{self->pendingHigh, character, 0};
                    self->pendingHigh = 0;
                    if (!self->Fits(std::wstring_view(pair, 2)))
                    {
                        self->Reject("text_encoding");
                        return 0;
                    }
                    return DefSubclassProc(window, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(pair));
                }
            }
            if (message == WM_IME_COMPOSITION && (lParam & GCS_RESULTSTR))
            {
                const HIMC context = ImmGetContext(window);
                if (context)
                {
                    const LONG bytes = ImmGetCompositionStringW(context, GCS_RESULTSTR, nullptr, 0);
                    std::wstring value;
                    if (bytes > 0)
                    {
                        value.resize(static_cast<std::size_t>(bytes) / sizeof(wchar_t));
                        ImmGetCompositionStringW(context, GCS_RESULTSTR, value.data(), static_cast<DWORD>(bytes));
                    }
                    const bool fits = bytes >= 0 && self->Fits(value);
                    if (!fits)
                        ImmNotifyIME(context, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
                    ImmReleaseContext(window, context);
                    if (!fits)
                    {
                        self->Reject();
                        return 0;
                    }
                }
            }
            return DefSubclassProc(window, message, wParam, lParam);
        }
        catch (...)
        {
            self->valid = false;
            return 0;
        }
    }
    // 宿主只转发修改并保持固定物理布局。入参：Win32 消息。返回：处理结果。
    static LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        Impl* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            self->window = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self)
            return DefWindowProcW(window, message, wParam, lParam);
        try
        {
            if (message == WM_COMMAND && HIWORD(wParam) == EN_CHANGE)
            {
                self->Changed();
                return 0;
            }
            if (message == WM_DPICHANGED)
            {
                self->ApplyPhysicalLayout();
                return 0;
            }
            if (message == WM_CLOSE)
            {
                (void)self->Finish(false);
                return 0;
            }
            if (message == WM_NCDESTROY)
            {
                self->window = nullptr;
                self->ready = false;
                self->finished = true;
                self->composing = false;
                SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            }
        }
        catch (...)
        {
            self->valid = false;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }
};

// 创建组件。入参：无。返回：未打开对象。
InlineTextEditor::InlineTextEditor() : impl_(std::make_unique<Impl>()) {}
// 释放自有资源。入参：无。返回：无。
InlineTextEditor::~InlineTextEditor()
{
    this->Close();
}
// 打开纯文本编辑宿主。入参：见头文件。返回：结构化错误，不启动额外消息循环。
RendererResult InlineTextEditor::Open(const InlineTextOptions& options, InlineTextCallbacks callbacks)
try
{
    if (GetCurrentThreadId() != this->impl_->thread)
        return {"wrong_thread", {}, {}};
    this->Close();
    Impl& self = *this->impl_;
    if (GetCurrentThreadId() != self.thread || !IsWindow(options.owner) || !callbacks.change || !callbacks.finish ||
        options.maxLength == 0 || options.maxLength > 1048576 || options.fontPixelHeight == 0 ||
        options.fontPixelHeight > 1024 || options.rgb > 0xffffff || options.fontFamily.empty() ||
        options.fontFamily.size() >= LF_FACESIZE)
        return {"invalid_options", {}, {}};
    const std::int64_t width = static_cast<std::int64_t>(options.bounds.right) - options.bounds.left;
    const std::int64_t height = static_cast<std::int64_t>(options.bounds.bottom) - options.bounds.top;
    RECT visible{};
    if (width <= 0 || height <= 0 || width > 32767 || height > 32767 ||
        !IntersectRect(&visible, &options.bounds, &options.clip))
        return {"invalid_bounds", {}, {}};
    self.options = options;
    if (!self.Valid(options.text))
        return {"invalid_text", {}, {}};
    self.callbacks = std::move(callbacks);
    self.finished = false;
    self.valid = true;
    self.pendingHigh = 0;
    self.rich = LoadLibraryExW(L"Msftedit.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!self.rich)
        return {"richedit_unavailable", {}, {}};
    WNDCLASSW type{};
    type.lpfnWndProc = Impl::Proc;
    type.hInstance = GetModuleHandleW(nullptr);
    type.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
    type.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    type.lpszClassName = L"OpenST.InlineTextEditor";
    if (!RegisterClassW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        this->Close();
        return {"register_failed", {}, {}};
    }
    self.window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, type.lpszClassName, L"", WS_POPUP,
                                  options.bounds.left, options.bounds.top, static_cast<int>(width),
                                  static_cast<int>(height), options.owner, nullptr, type.hInstance, &self);
    if (!self.window)
    {
        this->Close();
        return {"window_failed", {}, {}};
    }
    self.edit = CreateWindowExW(0, MSFTEDIT_CLASS, L"",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL |
                                    ES_WANTRETURN,
                                0, 0, static_cast<int>(width), static_cast<int>(height), self.window,
                                reinterpret_cast<HMENU>(1), type.hInstance, nullptr);
    if (!self.edit || !SetWindowSubclass(self.edit, Impl::EditProc, 1, reinterpret_cast<DWORD_PTR>(&self)))
    {
        this->Close();
        return {"edit_failed", {}, {}};
    }
    if (SendMessageW(self.edit, EM_SETTEXTMODE, TM_PLAINTEXT | TM_MULTILEVELUNDO | TM_MULTICODEPAGE, 0) != 0)
    {
        this->Close();
        return {"text_mode_failed", {}, {}};
    }
    SendMessageW(self.edit, EM_EXLIMITTEXT, 0, LONG_MAX);
    if (!SendMessageW(self.edit, EM_SETTARGETDEVICE, 0, 1))
    {
        this->Close();
        return {"text_layout_failed", {}, {}};
    }
    SendMessageW(self.edit, EM_SETBKGNDCOLOR, 0, GetSysColor(COLOR_WINDOW));
    LOGFONTW description{};
    description.lfHeight = -static_cast<LONG>(options.fontPixelHeight);
    description.lfCharSet = DEFAULT_CHARSET;
    description.lfQuality = CLEARTYPE_QUALITY;
    std::copy(options.fontFamily.begin(), options.fontFamily.end(), description.lfFaceName);
    self.font = renderer_detail::CreateEditFont(description);
    if (!self.font)
    {
        this->Close();
        return {"font_failed", {}, {}};
    }
    SendMessageW(self.edit, WM_SETFONT, reinterpret_cast<WPARAM>(self.font), FALSE);
    CHARFORMAT2W format{};
    format.cbSize = sizeof(format);
    format.dwMask = CFM_COLOR;
    format.crTextColor = RGB((options.rgb >> 16) & 255, (options.rgb >> 8) & 255, options.rgb & 255);
    if (!SendMessageW(self.edit, EM_SETCHARFORMAT, SCF_ALL, reinterpret_cast<LPARAM>(&format)))
    {
        this->Close();
        return {"text_color_failed", {}, {}};
    }
    RECT content{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    SendMessageW(self.edit, EM_SETRECT, 0, reinterpret_cast<LPARAM>(&content));
    SendMessageW(self.edit, EM_SETTARGETDEVICE, 0, 1);
    if (!SetWindowTextW(self.edit, options.text.c_str()))
    {
        this->Close();
        return {"initial_text_failed", {}, {}};
    }
    SendMessageW(self.edit, EM_EMPTYUNDOBUFFER, 0, 0);
    SendMessageW(self.edit, EM_SETEVENTMASK, 0, ENM_CHANGE);
    const HRGN region = CreateRectRgn(visible.left - options.bounds.left, visible.top - options.bounds.top,
                                      visible.right - options.bounds.left, visible.bottom - options.bounds.top);
    if (!region || !SetWindowRgn(self.window, region, FALSE))
    {
        if (region)
            DeleteObject(region);
        this->Close();
        return {"clip_failed", {}, {}};
    }
    self.ready = true;
    ShowWindow(self.window, SW_SHOW);
    this->Focus();
    return {};
}
catch (...)
{
    this->Close();
    return {"open_failed", {}, {}};
}
// 请求确认。入参：无。返回：请求状态。
InlineTextRequest InlineTextEditor::RequestCommit() noexcept
{
    try
    {
        return this->impl_->Finish(true);
    }
    catch (...)
    {
        this->impl_->notifying = false;
        this->impl_->Reject("input_failed");
        return InlineTextRequest::Rejected;
    }
}
// 请求取消。入参：无。返回：请求状态。
InlineTextRequest InlineTextEditor::RequestCancel() noexcept
{
    try
    {
        return this->impl_->Finish(false);
    }
    catch (...)
    {
        this->impl_->notifying = false;
        this->impl_->Reject("input_failed");
        return InlineTextRequest::Rejected;
    }
}
// 查询组合状态。入参：无。返回：状态。
bool InlineTextEditor::IsComposing() const noexcept
{
    return this->impl_->composing;
}
// 查询宿主。入参：无。返回：借用窗口。
HWND InlineTextEditor::NativeHandle() const noexcept
{
    return this->impl_->window;
}
// 无回调释放。入参：无。返回：无。
void InlineTextEditor::Close() noexcept
{
    Impl& self = *this->impl_;
    self.ready = false;
    if (self.window)
        DestroyWindow(self.window);
    self.window = nullptr;
    self.edit = nullptr;
    self.status = nullptr;
    if (self.font)
        DeleteObject(self.font);
    self.font = nullptr;
    if (self.rich)
        FreeLibrary(self.rich);
    self.rich = nullptr;
    self.callbacks = {};
    self.composing = false;
    self.finished = true;
}
// 显示由宿主本地化的输入错误，不覆盖正文。入参：text。返回：无。
void InlineTextEditor::SetError(std::wstring text)
{
    Impl& self = *this->impl_;
    if (!self.window)
        return;
    if (!self.status)
        self.status = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | SS_LEFT, 0, 0, 1, 1, self.window, nullptr,
                                      GetModuleHandleW(nullptr), nullptr);
    RECT bounds{};
    GetClientRect(self.window, &bounds);
    SetWindowPos(self.status, HWND_TOP, 0, std::max(0L, bounds.bottom - 24), bounds.right, 24, SWP_NOACTIVATE);
    SetWindowTextW(self.status, text.c_str());
    ShowWindow(self.status, text.empty() ? SW_HIDE : SW_SHOWNOACTIVATE);
}
// 恢复焦点。入参：无。返回：无。
void InlineTextEditor::Focus() noexcept
{
    if (this->impl_->window && !this->impl_->finished)
    {
        SetForegroundWindow(this->impl_->window);
        SetActiveWindow(this->impl_->window);
        SetFocus(this->impl_->edit);
    }
}
// 恢复排队失败后的同一个原生编辑实例。入参：无。返回：无。
void InlineTextEditor::Resume() noexcept
{
    Impl& self = *this->impl_;
    if (GetCurrentThreadId() == self.thread && self.window && self.edit && self.finished && !self.notifying)
    {
        self.finished = false;
        SendMessageW(self.edit, EM_SETREADONLY, FALSE, 0);
    }
}
// 转发通用编辑动作并保留统一输入准入。入参：command。返回：已经分派时 true。
bool InlineTextEditor::InvokeEditCommand(InlineEditCommand command) noexcept
{
    return this->impl_->Invoke(command);
}
// 只替换环境观察，不改变原生控件、文档、真实前台或编辑命令逻辑。
// 入参：editor为测试独占组件；query为空时恢复系统查询。返回：无。
void InlineTextEditorTestAccess::SetForegroundQuery(InlineTextEditor& editor, std::function<HWND()> query)
{
    if (GetCurrentThreadId() != editor.impl_->thread)
        return;
    editor.impl_->foregroundQuery = std::move(query);
}
} // namespace open_st
