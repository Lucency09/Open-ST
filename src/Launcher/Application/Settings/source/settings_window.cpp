#include <settings_window.h>

#include <log.h>
#include <settings.h>

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
constexpr wchar_t SETTINGS_WINDOW_CLASS[] = L"OpenST.SettingsWindow";
constexpr int LANGUAGE_COMBO_ID = 1001;
constexpr int WINDOW_WIDTH = 420;
constexpr int WINDOW_HEIGHT = 190;
constexpr UINT BASE_DPI = 96;

// 把以 96 DPI 表示的布局值换算成当前显示器上的物理像素。
int ScaleForDpi(int value, UINT dpi) noexcept
{
    return MulDiv(value, static_cast<int>(dpi), static_cast<int>(BASE_DPI));
}

// 语言代码由 JSON 属性名提供，当前只需进行无损 ASCII 到宽字符转换。
std::wstring LanguageCodeToWide(std::string_view languageCode)
{
    return std::wstring(languageCode.begin(), languageCode.end());
}
} // namespace

namespace open_st
{
class SettingsWindow::Impl final
{
  public:
    // 注册窗口类并创建一份非模态设置窗口；已打开时只恢复并前置现有窗口。
    bool Show(HINSTANCE instance, SettingsWindowCallbacks callbacks)
    {
        if (this->window_ != nullptr)
        {
            ShowWindow(this->window_, SW_RESTORE);
            SetForegroundWindow(this->window_);
            return true;
        }

        if (!callbacks.text || !callbacks.currentLanguage || !callbacks.availableLanguages ||
            !callbacks.languageApplied)
        {
            OPEN_ST_LOG_ERROR("Settings window callbacks are incomplete.");
            return false;
        }
        this->instance_ = instance;
        this->callbacks_ = std::move(callbacks);
        WNDCLASSEXW windowClass{sizeof(windowClass)};
        windowClass.lpfnWndProc = SettingsWindow::Impl::WindowProc;
        windowClass.hInstance = this->instance_;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        windowClass.lpszClassName = SETTINGS_WINDOW_CLASS;
        if (RegisterClassExW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            OPEN_ST_LOG_ERROR("Failed to register the settings window class. win32_error=", GetLastError());
            return false;
        }

        RECT workArea{};
        (void)SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
        const UINT initialDpi = GetDpiForSystem();
        const int windowWidth = ScaleForDpi(WINDOW_WIDTH, initialDpi);
        const int windowHeight = ScaleForDpi(WINDOW_HEIGHT, initialDpi);
        const int x = workArea.left + ((workArea.right - workArea.left) - windowWidth) / 2;
        const int y = workArea.top + ((workArea.bottom - workArea.top) - windowHeight) / 2;
        const std::wstring title = this->callbacks_.text("settings.title");
        this->window_ =
            CreateWindowExW(WS_EX_DLGMODALFRAME, SETTINGS_WINDOW_CLASS, title.c_str(), WS_CAPTION | WS_SYSMENU,
                            x, y, windowWidth, windowHeight, nullptr, nullptr, this->instance_, this);
        if (this->window_ == nullptr)
        {
            OPEN_ST_LOG_ERROR("Failed to create the settings window. win32_error=", GetLastError());
            return false;
        }

        // 只设置窗口实例图标，不让长期注册的窗口类保留外部借用句柄。
        if (this->callbacks_.largeIcon != nullptr)
        {
            SendMessageW(this->window_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(this->callbacks_.largeIcon));
        }
        if (this->callbacks_.smallIcon != nullptr)
        {
            SendMessageW(this->window_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(this->callbacks_.smallIcon));
        }
        const DWM_WINDOW_CORNER_PREFERENCE cornerPreference = DWMWCP_ROUND;
        (void)DwmSetWindowAttribute(this->window_, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPreference,
                                    sizeof(cornerPreference));
        ShowWindow(this->window_, SW_SHOW);
        SetForegroundWindow(this->window_);
        return true;
    }

    // 把键盘导航交给 Win32 对话框消息处理器，使 Tab、Enter 和 Escape 符合常规设置窗口行为。
    bool ProcessDialogMessage(MSG& message) const noexcept
    {
        return this->window_ != nullptr && IsDialogMessageW(this->window_, &message) != FALSE;
    }

    // 销毁现有设置窗口；没有窗口时为空操作。
    void Close() noexcept
    {
        if (this->window_ != nullptr)
        {
            DestroyWindow(this->window_);
        }
        this->callbacks_ = {};
    }

    // 返回设置窗口是否已经创建且尚未销毁。
    bool IsOpen() const noexcept
    {
        return this->window_ != nullptr;
    }

  private:
    // 从 HWND 用户数据槽恢复 Impl，并把静态 Win32 回调转交给实例方法。
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    try
    {
        SettingsWindow::Impl* impl =
            reinterpret_cast<SettingsWindow::Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            const CREATESTRUCTW* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            impl = static_cast<SettingsWindow::Impl*>(create->lpCreateParams);
            impl->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(impl));
        }
        return impl != nullptr ? impl->HandleMessage(window, message, wParam, lParam)
                               : DefWindowProcW(window, message, wParam, lParam);
    }
    catch (...)
    {
        OPEN_ST_LOG_ERROR("Settings window operation failed with an exception.");
        // 创建阶段用 Win32 约定取消创建；其他消息取消本次操作，不让 C++ 异常穿越系统回调。
        return message == WM_CREATE ? -1 : 0;
    }

    // 创建标签、动态语言下拉框和确认/取消按钮。
    bool CreateControls(HWND parent)
    {
        const std::wstring languageLabel = this->callbacks_.text("settings.language.label");
        this->languageLabel_ = CreateWindowExW(0, L"STATIC", languageLabel.c_str(), WS_CHILD | WS_VISIBLE,
                                               28, 28, 350, 22, parent, nullptr, this->instance_, nullptr);
        this->languageCombo_ = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                                                 CBS_DROPDOWNLIST | WS_VSCROLL,
                                               28, 54, 350, 180, parent,
                                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(LANGUAGE_COMBO_ID)),
                                               this->instance_, nullptr);
        const std::wstring okText = this->callbacks_.text("settings.ok");
        this->okButton_ = CreateWindowExW(0, L"BUTTON", okText.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                                                            BS_DEFPUSHBUTTON,
                                          216, 112, 76, 28, parent,
                                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)),
                                          this->instance_, nullptr);
        const std::wstring cancelText = this->callbacks_.text("settings.cancel");
        this->cancelButton_ = CreateWindowExW(0, L"BUTTON", cancelText.c_str(),
                                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 302, 112, 76, 28,
                                              parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)),
                                              this->instance_, nullptr);
        if (this->languageLabel_ == nullptr || this->languageCombo_ == nullptr || this->okButton_ == nullptr ||
            this->cancelButton_ == nullptr)
        {
            return false;
        }

        const UINT dpi = GetDpiForWindow(parent);
        this->RefreshFont(dpi);
        this->LayoutControls(dpi);

        const std::optional<std::string> configuredLanguage = GetStringSetting("ui.language");
        this->PopulateLanguages(configuredLanguage.value_or(this->callbacks_.currentLanguage()));
        return true;
    }

    // 按当前显示器 DPI 重建系统消息字体，并应用到全部子控件。
    void RefreshFont(UINT dpi)
    {
        NONCLIENTMETRICSW metrics{sizeof(metrics)};
        HFONT newFont = nullptr;
        if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi) != FALSE)
        {
            newFont = CreateFontIndirectW(&metrics.lfMessageFont);
        }
        if (newFont == nullptr)
        {
            newFont = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        }

        (void)SendMessageW(this->languageLabel_, WM_SETFONT, reinterpret_cast<WPARAM>(newFont), TRUE);
        (void)SendMessageW(this->languageCombo_, WM_SETFONT, reinterpret_cast<WPARAM>(newFont), TRUE);
        (void)SendMessageW(this->okButton_, WM_SETFONT, reinterpret_cast<WPARAM>(newFont), TRUE);
        (void)SendMessageW(this->cancelButton_, WM_SETFONT, reinterpret_cast<WPARAM>(newFont), TRUE);

        if (this->uiFont_ != nullptr)
        {
            (void)DeleteObject(this->uiFont_);
        }
        this->uiFont_ = newFont == GetStockObject(DEFAULT_GUI_FONT) ? nullptr : newFont;
    }

    // 所有坐标均以 96 DPI 为设计基准；跨显示器后用新 DPI 一次性重排，避免控件仍停留在旧比例。
    void LayoutControls(UINT dpi) const noexcept
    {
        (void)MoveWindow(this->languageLabel_, ScaleForDpi(28, dpi), ScaleForDpi(28, dpi),
                         ScaleForDpi(350, dpi), ScaleForDpi(22, dpi), TRUE);
        (void)MoveWindow(this->languageCombo_, ScaleForDpi(28, dpi), ScaleForDpi(54, dpi),
                         ScaleForDpi(350, dpi), ScaleForDpi(180, dpi), TRUE);
        (void)MoveWindow(this->okButton_, ScaleForDpi(216, dpi), ScaleForDpi(112, dpi),
                         ScaleForDpi(76, dpi), ScaleForDpi(28, dpi), TRUE);
        (void)MoveWindow(this->cancelButton_, ScaleForDpi(302, dpi), ScaleForDpi(112, dpi),
                         ScaleForDpi(76, dpi), ScaleForDpi(28, dpi), TRUE);
    }

    // 在每次窗口打开或下拉展开时重新读取资源，并用动态语言代码重建选项。
    void PopulateLanguages(std::string_view preferredLanguage)
    {
        const std::vector<std::string> availableLanguages = this->callbacks_.availableLanguages();
        this->languages_.clear();
        (void)SendMessageW(this->languageCombo_, CB_RESETCONTENT, 0, 0);
        int selectedIndex = -1;
        for (const std::string& languageCode : availableLanguages)
        {
            const std::wstring language = LanguageCodeToWide(languageCode);
            const LRESULT addedIndex = SendMessageW(this->languageCombo_, CB_ADDSTRING, 0,
                                                    reinterpret_cast<LPARAM>(language.c_str()));
            if (addedIndex < 0)
            {
                continue;
            }
            this->languages_.push_back(languageCode);
            if (languageCode == preferredLanguage)
            {
                selectedIndex = static_cast<int>(addedIndex);
            }
        }
        if (selectedIndex < 0 && !this->languages_.empty())
        {
            const std::vector<std::string>::const_iterator english =
                std::find(this->languages_.begin(), this->languages_.end(), "en-US");
            selectedIndex = english == this->languages_.end()
                                ? 0
                                : static_cast<int>(std::distance(this->languages_.cbegin(), english));
        }
        if (selectedIndex >= 0)
        {
            (void)SendMessageW(this->languageCombo_, CB_SETCURSEL, static_cast<WPARAM>(selectedIndex), 0);
        }
    }

    // 返回下拉框当前选择的语言代码；没有有效选择时返回空字符串。
    std::string SelectedLanguage() const
    {
        const LRESULT selectedIndex = SendMessageW(this->languageCombo_, CB_GETCURSEL, 0, 0);
        if (selectedIndex < 0 || static_cast<std::size_t>(selectedIndex) >= this->languages_.size())
        {
            return {};
        }
        return this->languages_[static_cast<std::size_t>(selectedIndex)];
    }

    // 保存成功后才切换运行时语言并通知 App 刷新；失败时保持窗口打开。
    void Apply()
    {
        const std::string selectedBeforeRefresh = this->SelectedLanguage();
        this->PopulateLanguages(selectedBeforeRefresh);
        const std::string selectedLanguage = this->SelectedLanguage();
        if (selectedLanguage.empty() || !SetStringSetting("ui.language", selectedLanguage))
        {
            const std::wstring message = this->callbacks_.text("settings.save_failed");
            const std::wstring title = this->callbacks_.text("settings.title");
            (void)MessageBoxW(this->window_, message.c_str(), title.c_str(), MB_OK | MB_ICONWARNING);
            return;
        }

        this->callbacks_.languageApplied(selectedLanguage);
        DestroyWindow(this->window_);
    }

    // 处理设置窗口自身消息，不接管应用主消息窗口或截图覆盖窗口。
    LRESULT HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_CREATE:
            if (!this->CreateControls(window))
            {
                return -1;
            }
            return 0;
        case WM_COMMAND:
            if (LOWORD(wParam) == LANGUAGE_COMBO_ID && HIWORD(wParam) == CBN_DROPDOWN)
            {
                this->PopulateLanguages(this->SelectedLanguage());
                return 0;
            }
            if (LOWORD(wParam) == IDOK)
            {
                this->Apply();
                return 0;
            }
            if (LOWORD(wParam) == IDCANCEL)
            {
                DestroyWindow(window);
                return 0;
            }
            break;
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_DPICHANGED:
        {
            const UINT dpi = HIWORD(wParam);
            const RECT* suggestedBounds = reinterpret_cast<const RECT*>(lParam);
            (void)SetWindowPos(window, nullptr, suggestedBounds->left, suggestedBounds->top,
                               suggestedBounds->right - suggestedBounds->left,
                               suggestedBounds->bottom - suggestedBounds->top,
                               SWP_NOACTIVATE | SWP_NOZORDER);
            this->RefreshFont(dpi);
            this->LayoutControls(dpi);
            return 0;
        }
        case WM_DESTROY:
            if (this->uiFont_ != nullptr)
            {
                (void)DeleteObject(this->uiFont_);
                this->uiFont_ = nullptr;
            }
            this->window_ = nullptr;
            this->languageLabel_ = nullptr;
            this->languageCombo_ = nullptr;
            this->okButton_ = nullptr;
            this->cancelButton_ = nullptr;
            this->languages_.clear();
            this->callbacks_ = {};
            return 0;
        default:
            break;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    HINSTANCE instance_{};
    HWND window_{};
    HWND languageLabel_{};
    HWND languageCombo_{};
    HWND okButton_{};
    HWND cancelButton_{};
    HFONT uiFont_{};
    std::vector<std::string> languages_;
    SettingsWindowCallbacks callbacks_;
};

// 分配窗口实现，不立即创建 HWND。
SettingsWindow::SettingsWindow() : impl_(std::make_unique<Impl>()) {}
// 在提供回调的上级对象销毁前同步关闭窗口。
SettingsWindow::~SettingsWindow()
{
    this->Close();
}

// 创建窗口失败或回调抛出异常时清理资源并返回失败。
bool SettingsWindow::Show(HINSTANCE instance, SettingsWindowCallbacks callbacks) noexcept
try
{
    const bool shown = this->impl_->Show(instance, std::move(callbacks));
    if (!shown)
    {
        this->Close();
    }
    return shown;
}
catch (...)
{
    OPEN_ST_LOG_ERROR("Failed to open settings window with an exception.");
    this->Close();
    return false;
}

// 将键盘消息交给当前非模态对话框处理。
bool SettingsWindow::ProcessDialogMessage(MSG& message) const noexcept
{
    return this->impl_->ProcessDialogMessage(message);
}

// 同步释放当前窗口，重复关闭为空操作。
void SettingsWindow::Close() noexcept
{
    this->impl_->Close();
}

// 查询设置窗口是否已创建且尚未销毁。
bool SettingsWindow::IsOpen() const noexcept
{
    return this->impl_->IsOpen();
}
} // namespace open_st
