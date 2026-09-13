// 验证设置和欢迎窗口的实际控件交互、提交顺序、回调重入与失败重试。

#include "json_file_test_access.h"
#include "settings_internal.h"
#include "settings_window_test_access.h"
#include <array>
#include <commctrl.h>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <settings.h>
#include <settings_window.h>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <welcome_window.h>
#include <windows.h>

namespace
{
struct ControlSearch
{
    std::wstring className;
    std::wstring text;
    HWND found{};
};

// 通过真实控件类型和文本定位，不依赖 Renderer 的内部数字 ID。
// 入参：control 为当前枚举到的子控件句柄；parameter 为借用的查找条件及结果结构指针。
// 返回：找到目标控件时返回 FALSE 停止枚举；未匹配时返回 TRUE 继续枚举。
BOOL CALLBACK FindControl(HWND control, LPARAM parameter)
{
    ControlSearch& search = *reinterpret_cast<ControlSearch*>(parameter);
    std::array<wchar_t, 512> className{};
    std::array<wchar_t, 512> text{};
    GetClassNameW(control, className.data(), static_cast<int>(className.size()));
    GetWindowTextW(control, text.data(), static_cast<int>(text.size()));
    if (search.className == className.data() && (search.text.empty() || search.text == text.data()))
    {
        search.found = control;
        return FALSE;
    }
    return TRUE;
}

// 只搜索当前测试线程的欢迎窗口，避免并行测试干扰其他进程。
// 入参：无显式入参。
// 返回：匹配控件或窗口的借用句柄；未找到时为 nullptr，调用方不取得销毁责任。
HWND FindWelcomeWindow()
{
    ControlSearch search{L"OpenST.WindowRenderer", L"welcome.title", nullptr};
    EnumThreadWindows(GetCurrentThreadId(), FindControl, reinterpret_cast<LPARAM>(&search));
    return search.found;
}

class SettingsWindowTest : public testing::Test
{
  protected:
    // 隔离文件绑定和磁盘目录，复制正式布局但不读取真实用户设置。
    // 入参：无显式入参。
    // 返回：无返回值。
    void SetUp() override
    {
        open_st::ShutdownSettings();
        ASSERT_TRUE(open_st::JsonFileTestAccess::ReleaseFile("settings.user"));
        ASSERT_TRUE(open_st::JsonFileTestAccess::ReleaseFile("settings.default"));
        ASSERT_TRUE(open_st::JsonFileTestAccess::ReleaseFile("settings.layout"));
        const testing::TestInfo* info = testing::UnitTest::GetInstance()->current_test_info();
        this->root_ = std::filesystem::temp_directory_path() /
                      ("open_st_settings_window_" + std::to_string(GetCurrentProcessId()) + "_" + info->name());
        std::filesystem::create_directories(this->root_ / "resources");
        std::filesystem::create_directories(this->root_ / "data");
        std::filesystem::copy_file(OPEN_ST_SETTINGS_LAYOUT_PATH, this->root_ / "resources/setting_windows.json",
                                   std::filesystem::copy_options::overwrite_existing);
        this->Write(
            "resources/default_settings.json",
            R"({"schemaVersion":1,"settings":{"ui.language":"en-US","startup.enabled":true,)"
            R"("onboarding.completed":false,"capture.hotkey":"Ctrl+Alt+Q",)"
            R"("capture.selection_border_color":"#000000","export.default_format":"jpeg","export.jpeg_quality":95}})");
        this->Write(
            "data/settings.json",
            R"({"schemaVersion":1,"settings":{"ui.language":"en-US","startup.enabled":true,)"
            R"("onboarding.completed":false,"capture.hotkey":"Ctrl+Alt+Q",)"
            R"("capture.selection_border_color":"#000000","export.default_format":"jpeg","export.jpeg_quality":95}})");
        ASSERT_TRUE(open_st::InitializeSettings(this->root_));
        open_st::SettingsWindowTestAccess::SetConfirmation(this->window_,
                                                           // 记录恢复默认确认次数，并返回用例指定的确认结果。
                                                           // 入参：无显式入参。
                                                           // 返回：confirm_，表示本用例模拟的恢复默认确认选择。
                                                           [this]()
                                                           {
                                                               ++this->confirmationCount_;
                                                               return this->confirm_;
                                                           });
    }

    // 在捕获对象销毁前关闭窗口，恢复属性并释放测试卡名。
    // 入参：无显式入参。
    // 返回：无返回值。
    void TearDown() override
    {
        this->window_.Close();
        this->Pump();
        open_st::ShutdownSettings();
        EXPECT_TRUE(open_st::JsonFileTestAccess::ReleaseFile("settings.user"));
        EXPECT_TRUE(open_st::JsonFileTestAccess::ReleaseFile("settings.default"));
        EXPECT_TRUE(open_st::JsonFileTestAccess::ReleaseFile("settings.layout"));
        (void)SetFileAttributesW((this->root_ / "data/settings.json").c_str(), FILE_ATTRIBUTE_NORMAL);
        std::error_code error;
        std::filesystem::remove_all(this->root_, error);
        EXPECT_FALSE(error);
    }

    // 模拟外部编辑器修改隔离文件。
    // 入参：relative 为相对隔离测试根目录的文件路径；content 为写入的原始文本。
    // 返回：无返回值。
    void Write(const std::filesystem::path& relative, std::string_view content)
    {
        std::ofstream output(this->root_ / relative, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(output.is_open());
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
        ASSERT_TRUE(output.good());
    }

    // 提供窄回调并记录真实持久化顺序，可模拟查询失败与生效失败。
    // 入参：无显式入参。
    // 返回：绑定测试夹具状态的设置窗口回调集合；夹具必须在窗口关闭前保持存活。
    open_st::SettingsWindowCallbacks Callbacks()
    {
        open_st::SettingsWindowCallbacks callbacks;
        // 从测试文本入口取得随模拟语言变化的界面文本。
        // 入参：key 为待查询的测试界面文本键。
        // 返回：测试键对应的宽字符串；标题附加当前已生效语言以便断言刷新。
        callbacks.text = [this](std::string_view key) { return this->Text(key); };
        // 模拟自启操作成功，不接触真实启动项。
        // 入参：未命名 bool 为期望的自启启用状态；本模拟回调不按该值区分处理。
        // 返回：固定为 true，表示模拟自启设置成功。
        callbacks.startupApplied = [](bool) { return true; };
        // 返回固定的模拟自启状态说明。
        // 入参：无显式入参。
        // 返回：固定的模拟自启状态文本 startup status。
        callbacks.startupStatus = []() { return L"startup status"; };
        // 查询当前模拟已生效语言，区别于尚未保存的草稿。
        // 入参：无显式入参。
        // 返回：appliedLanguage_，表示模拟的已生效语言代码。
        callbacks.currentLanguage = [this]() { return this->appliedLanguage_; };
        // 返回可用语言列表，或按开关注入查询异常。
        // 入参：无显式入参。
        // 返回：模拟的可用语言列表；启用异常开关时抛出异常。
        callbacks.availableLanguages = [this]()
        {
            if (this->queryThrows_)
            {
                throw std::runtime_error("test query failure");
            }
            return this->availableLanguages_;
        };
        // 记录保存后的语言通知，模拟生效失败及回调内关闭重入。
        // 入参：language 为保存完成后请求在运行期应用的语言代码。
        // 返回：applySucceeds_，表示模拟运行期语言应用结果。
        callbacks.languageApplied = [this](std::string_view language)
        {
            this->hotkeyEvents_.push_back("language");
            ++this->appliedCount_;
            this->savedAtNotification_ = open_st::GetStringSetting("ui.language").value_or("");
            if (this->applySucceeds_)
            {
                this->appliedLanguage_ = language;
            }
            if (this->closeInCallback_)
            {
                this->window_.Close();
            }
            return this->applySucceeds_;
        };
        // 仅识别测试实际使用的两个值，将产品解析规则留给 Hotkeys 模块测试。
        // 入参：value 为待验证的配置字符串。
        // 返回：测试允许的数值组合；其他字符串为空。
        callbacks.hotkeyDecode = [](std::string_view value) -> std::optional<open_st::SettingsHotkeyChord>
        {
            if (value == "Ctrl+Alt+Q")
                return open_st::SettingsHotkeyChord{MOD_CONTROL | MOD_ALT, 'Q'};
            if (value == "Ctrl+Shift+F8")
                return open_st::SettingsHotkeyChord{MOD_CONTROL | MOD_SHIFT, VK_F8};
            return std::nullopt;
        };
        // 将测试组合转换为固定 token，不引入 Settings 对 Hotkeys 的兄弟依赖。
        // 入参：value 为录入控件的数值组合。
        // 返回：测试允许组合的 token，其余为空。
        callbacks.hotkeyEncode = [](open_st::SettingsHotkeyChord value) -> std::optional<std::string>
        {
            if (value.modifiers == (MOD_CONTROL | MOD_ALT) && value.key == 'Q')
                return "Ctrl+Alt+Q";
            if (value.modifiers == (MOD_CONTROL | MOD_SHIFT) && value.key == VK_F8)
                return "Ctrl+Shift+F8";
            return std::nullopt;
        };
        // 为测试录入控件提供稳定显示值，不依赖机器键盘布局。
        // 入参：value 为待显示的组合。
        // 返回：对应组合的测试显示名称。
        callbacks.hotkeyFormat = [](open_st::SettingsHotkeyChord value)
        { return value.key == VK_F8 ? L"Ctrl+Shift+F8" : L"Ctrl+Alt+Q"; };
        // 模拟候选注册并记录准备时磁盘旧值，不调用真实 RegisterHotKey。
        // 入参：value 为即将提交的快捷键目标。
        // 返回：hotkeyPrepareSucceeds_ 控制准备成功或失败。
        callbacks.hotkeyPrepare = [this](std::string_view value)
        {
            ++this->hotkeyPrepareCount_;
            this->hotkeyEvents_.push_back("prepare");
            this->hotkeyAtPrepare_ = open_st::GetStringSetting("capture.hotkey").value_or("");
            if (this->hotkeyPrepareSucceeds_)
                this->preparedHotkey_ = value;
            return this->hotkeyPrepareSucceeds_;
        };
        // 记录发布或撤销，核验已保存值先于活动组合切换。
        // 入参：commit 为发布候选或撤销候选的标志。
        // 返回：hotkeyCleanupSucceeds_ 控制注销资源清理结果。
        callbacks.hotkeyFinish = [this](bool commit)
        {
            this->hotkeyEvents_.push_back(commit ? "activate" : "cancel");
            if (commit)
            {
                ++this->hotkeyActivateCount_;
                this->hotkeyAtActivation_ = open_st::GetStringSetting("capture.hotkey").value_or("");
                this->activeHotkey_.swap(this->preparedHotkey_);
            }
            else
                ++this->hotkeyCancelCount_;
            this->preparedHotkey_.clear();
            return this->hotkeyCleanupSucceeds_;
        };
        // 返回模拟活动组合，供正式布局查询动态状态。
        // 入参：无。
        // 返回：当前测试活动快捷键的宽字符串。
        callbacks.hotkeyStatus = [this]()
        { return std::wstring(this->activeHotkey_.begin(), this->activeHotkey_.end()); };
        // 比较目标与实际活动组合，使未注册状态无需制造草稿变化即可应用。
        // 入参：value 为当前待应用的组合 token。
        // 返回：活动目标不同或清理未完成时为 true。
        callbacks.hotkeyNeedsApply = [this](std::string_view value)
        { return value != this->activeHotkey_ || !this->hotkeyCleanupSucceeds_; };
        // 测试不注入实际键盘录入，此回调仅满足生产绑定契约。
        // 入参：未命名 bool 为录入开始或结束状态。
        // 返回：无。
        callbacks.hotkeyRecording = [](bool) {};
        // 在测试中刷新设置文本，模拟 App 保存完成后的界面同步。
        // 入参：无。
        // 返回：无。
        callbacks.hotkeyRefresh = [this]() { this->window_.RefreshTexts(); };
        // 为窗口测试提供严格颜色规则替身，不建立兄弟模块依赖。
        // 入参：value 为原始颜色输入。
        // 返回：合法六位颜色的大写值，否则为空。
        callbacks.normalizeBorderColor = [](std::string_view value) -> std::optional<std::string>
        {
            if (value.size() != 7 || value.front() != '#')
                return std::nullopt;
            std::string normalized(value);
            for (std::size_t index = 1; index < normalized.size(); ++index)
            {
                char& character = normalized[index];
                if (character >= 'a' && character <= 'f')
                    character = static_cast<char>(character - 'a' + 'A');
                else if (!(character >= '0' && character <= '9') && !(character >= 'A' && character <= 'F'))
                    return std::nullopt;
            }
            return normalized;
        };
        // 只允许设计中的两个保存 token。
        // 入参：value 为格式值。
        // 返回：jpeg/png 为 true。
        callbacks.validImageFormat = [](std::string_view value) { return value == "jpeg" || value == "png"; };
        // 验证整数 JPEG 质量范围。
        // 入参：value 为质量值。
        // 返回：1 到 100 含端点为 true。
        callbacks.validJpegQuality = [](std::int64_t value) { return value >= 1 && value <= 100; };
        // 用隔离计数器模拟维护，不打开真实目录或删除真实日志。
        // 入参：无。
        // 返回：当前测试指定的目录打开结果。
        callbacks.openLogDirectory = [this]()
        {
            ++this->openDirectoryCount_;
            return this->openDirectorySucceeds_;
        };
        // 模拟任务启动并记录请求次数，不清理真实文件。
        // 入参：无。
        // 返回：测试控制的任务启动结果。
        callbacks.clearHistoricalLogs = [this]()
        {
            ++this->clearHistoryCount_;
            this->maintenanceRunning_ = this->clearHistorySucceeds_;
            return this->clearHistorySucceeds_;
        };
        // 提供关闭和重开窗口后仍可取得的模拟任务快照。
        // 入参：无。
        // 返回：测试中的运行状态和显示文字。
        callbacks.maintenanceStatus = [this]()
        { return open_st::SettingsMaintenanceStatus{this->maintenanceRunning_, this->maintenanceText_}; };
        return callbacks;
    }

    // 解析设置窗口测试文案，以标题中的语言代码观察刷新是否生效。
    // 入参：key 为待查询的测试界面文本键。
    // 返回：测试键对应的宽字符串；标题附加当前已生效语言以便断言刷新。
    std::wstring Text(std::string_view key) const
    {
        const std::string value =
            key == "settings.title" ? std::string(key) + ":" + this->appliedLanguage_ : std::string(key);
        return std::wstring(value.begin(), value.end());
    }

    // 创建真实窗口并立即隐藏，只通过本线程消息进行自动交互。
    // 入参：无显式入参。
    // 返回：成功创建的借用设置窗口句柄；创建失败为 nullptr。
    HWND Open()
    {
        if (!this->window_.Show(GetModuleHandleW(nullptr), this->Callbacks()))
        {
            return nullptr;
        }
        const HWND window = open_st::SettingsWindowTestAccess::NativeHandle(this->window_);
        ShowWindow(window, SW_HIDE);
        return window;
    }

    // 从借用父窗递归查询控件，避免全局窗口搜索。
    // 入参：className 为要匹配的原生控件类名；text 为可选匹配文字，空值表示不限文字。
    // 返回：匹配控件或窗口的借用句柄；未找到时为 nullptr，调用方不取得销毁责任。
    HWND Control(std::wstring className, std::wstring text = {}) const
    {
        ControlSearch search{std::move(className), std::move(text), nullptr};
        EnumChildWindows(open_st::SettingsWindowTestAccess::NativeHandle(this->window_), FindControl,
                         reinterpret_cast<LPARAM>(&search));
        return search.found;
    }

    // 限量派发延迟关闭，避免自动测试无限等待消息。
    // 入参：无显式入参。
    // 返回：无返回值。
    void Pump()
    {
        MSG message{};
        for (int count = 0; count < 256 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++count)
        {
            if (message.message != WM_QUIT && !this->window_.ProcessDialogMessage(message))
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
    }

    // 选择项目后向实际父容器发送通知；CB_SETCURSEL 本身不能模拟用户编辑。
    // 入参：language 为要选中的语言项文本，必须存在于当前下拉列表。
    // 返回：无返回值。
    void Select(const wchar_t* language)
    {
        const HWND combo = this->Control(L"ComboBox");
        ASSERT_NE(combo, nullptr);
        const LRESULT index =
            SendMessageW(combo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(language));
        ASSERT_NE(index, CB_ERR);
        (void)SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
        (void)SendMessageW(GetParent(combo), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(combo), CBN_SELCHANGE),
                           reinterpret_cast<LPARAM>(combo));
        this->Pump();
    }

    // 真实按钮点击后派发可能产生的延迟关闭。
    // 入参：key 为要点击按钮的测试文本键。
    // 返回：无返回值。
    void Click(std::string_view key)
    {
        const HWND button = this->Control(L"Button", this->Text(key));
        ASSERT_NE(button, nullptr);
        ASSERT_NE(IsWindowEnabled(button), FALSE);
        (void)SendMessageW(button, BM_CLICK, 0, 0);
        this->Pump();
    }

    // 发送原生页签通知，使恢复默认使用真实当前页面。
    // 入参：index 为从零开始的布局页面索引。
    // 返回：无，GoogleTest 断言记录控件或切换失败。
    void SelectPage(int index)
    {
        const HWND tabs = this->Control(WC_TABCONTROLW);
        ASSERT_NE(tabs, nullptr);
        TabCtrl_SetCurSel(tabs, index);
        NMHDR notification{tabs, static_cast<UINT_PTR>(GetDlgCtrlID(tabs)), TCN_SELCHANGE};
        SendMessageW(this->window_.NativeHandle(), WM_NOTIFY, notification.idFrom,
                     reinterpret_cast<LPARAM>(&notification));
        this->Pump();
    }

    // 将组合按键送入真实设置消息导航，验证控件录入到业务草稿的完整链路。
    // 入参：control 为组合控件句柄；message 为键盘消息类型；key 为虚拟键码。
    // 返回：该输入被设置窗口消费时为 true。
    bool ChordKey(HWND control, UINT message, UINT key)
    {
        MSG input{};
        input.hwnd = control;
        input.message = message;
        input.wParam = key;
        input.time = GetTickCount();
        return this->window_.ProcessDialogMessage(input);
    }

    // 展开通知刷新选项，不实际弹出下拉窗口。
    // 入参：无显式入参。
    // 返回：无返回值。
    void RefreshOptions()
    {
        const HWND combo = this->Control(L"ComboBox");
        ASSERT_NE(combo, nullptr);
        (void)SendMessageW(GetParent(combo), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(combo), CBN_DROPDOWN),
                           reinterpret_cast<LPARAM>(combo));
        this->Pump();
    }

    // 在独立 CTest 进程中显式建立线程消息队列后投递自动交互。
    // 入参：message 为向本线程投递的自动交互消息编号。
    // 返回：线程消息投递成功时为 true，否则为 false。
    bool QueueWelcomeMessage(UINT message)
    {
        MSG existing{};
        (void)PeekMessageW(&existing, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
        return PostThreadMessageW(GetCurrentThreadId(), message, 0, 0) != FALSE;
    }

    std::filesystem::path root_;
    std::vector<std::string> availableLanguages_{"en-US", "zh-CN", "ja-JP"};
    std::string appliedLanguage_{"en-US"};
    std::string savedAtNotification_;
    int appliedCount_{};
    int confirmationCount_{};
    bool confirm_{true};
    bool applySucceeds_{true};
    bool queryThrows_{};
    bool closeInCallback_{};
    bool hotkeyPrepareSucceeds_{true};
    bool hotkeyCleanupSucceeds_{true};
    int hotkeyPrepareCount_{};
    int hotkeyActivateCount_{};
    int hotkeyCancelCount_{};
    std::string activeHotkey_{"Ctrl+Alt+Q"};
    std::string preparedHotkey_;
    std::string hotkeyAtPrepare_;
    std::string hotkeyAtActivation_;
    std::vector<std::string> hotkeyEvents_;
    int openDirectoryCount_{};
    int clearHistoryCount_{};
    bool openDirectorySucceeds_{true};
    bool clearHistorySucceeds_{true};
    bool maintenanceRunning_{};
    std::wstring maintenanceText_{L"maintenance idle"};
    open_st::SettingsWindow window_;
};

// 验证选择只改草稿，取消不写入也不切换运行语言。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(SettingsWindowTest, cancel_does_not_save_or_notify)
{
    ASSERT_NE(this->Open(), nullptr);
    this->Select(L"zh-CN");
    this->Click("settings.cancel");
    EXPECT_FALSE(this->window_.IsOpen());
    EXPECT_EQ(open_st::GetStringSetting("ui.language"), "en-US");
    EXPECT_EQ(this->appliedCount_, 0);
}

// 验证应用先保存后通知并保持窗口，无新修改的确定只关闭。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(SettingsWindowTest, apply_keeps_window_and_accept_does_not_notify_twice)
{
    ASSERT_NE(this->Open(), nullptr);
    this->Select(L"zh-CN");
    this->Click("settings.apply");
    EXPECT_TRUE(this->window_.IsOpen());
    EXPECT_EQ(this->savedAtNotification_, "zh-CN");
    EXPECT_EQ(this->appliedCount_, 1);
    this->Click("settings.ok");
    EXPECT_FALSE(this->window_.IsOpen());
    EXPECT_EQ(this->appliedCount_, 1);
}

// 验证确定带草稿时使用统一提交入口，生效成功后关闭。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(SettingsWindowTest, accept_saves_before_notifying_and_closes)
{
    ASSERT_NE(this->Open(), nullptr);
    this->Select(L"ja-JP");
    this->Click("settings.ok");
    EXPECT_FALSE(this->window_.IsOpen());
    EXPECT_EQ(this->savedAtNotification_, "ja-JP");
    EXPECT_EQ(this->appliedCount_, 1);
}

// 验证选项消失不能替用户改选，重新出现后应仍能提交原草稿。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(SettingsWindowTest, disappearing_option_preserves_intent)
{
    ASSERT_NE(this->Open(), nullptr);
    this->Select(L"zh-CN");
    this->availableLanguages_ = {"en-US"};
    this->RefreshOptions();
    EXPECT_EQ(SendMessageW(this->Control(L"ComboBox"), CB_GETCURSEL, 0, 0), CB_ERR);
    this->Click("settings.apply");
    EXPECT_EQ(this->appliedCount_, 0);
    EXPECT_EQ(open_st::GetStringSetting("ui.language"), "en-US");
    this->availableLanguages_.push_back("zh-CN");
    this->RefreshOptions();
    this->Click("settings.apply");
    EXPECT_EQ(this->savedAtNotification_, "zh-CN");
}

// 验证写入失败保留窗口，不能提前调用生效回调。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(SettingsWindowTest, save_failure_never_notifies)
{
    ASSERT_NE(this->Open(), nullptr);
    this->Select(L"zh-CN");
    ASSERT_NE(SetFileAttributesW((this->root_ / "data/settings.json").c_str(), FILE_ATTRIBUTE_READONLY), FALSE);
    this->Click("settings.apply");
    EXPECT_EQ(this->appliedCount_, 0);
    EXPECT_TRUE(this->window_.IsOpen());
    EXPECT_EQ(open_st::GetStringSetting("ui.language"), "en-US");
    EXPECT_NE(this->Control(L"Static", L"settings.save_failed"), nullptr);
}

// 验证生效失败后文件改只读仍可重试成功，证明没有再次写盘。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(SettingsWindowTest, application_retry_does_not_write_again)
{
    this->applySucceeds_ = false;
    ASSERT_NE(this->Open(), nullptr);
    this->Select(L"zh-CN");
    this->Click("settings.ok");
    EXPECT_TRUE(this->window_.IsOpen());
    EXPECT_EQ(this->appliedLanguage_, "en-US");
    EXPECT_EQ(open_st::GetStringSetting("ui.language"), "zh-CN");
    ASSERT_NE(SetFileAttributesW((this->root_ / "data/settings.json").c_str(), FILE_ATTRIBUTE_READONLY), FALSE);
    this->applySucceeds_ = true;
    this->Click("settings.apply");
    EXPECT_EQ(this->appliedCount_, 2);
    EXPECT_EQ(this->appliedLanguage_, "zh-CN");
}

// 验证重试前外部修改必须阻止旧目标生效。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(SettingsWindowTest, retry_detects_external_change)
{
    this->applySucceeds_ = false;
    ASSERT_NE(this->Open(), nullptr);
    this->Select(L"zh-CN");
    this->Click("settings.apply");
    this->Write("data/settings.json", R"({"schemaVersion":1,"settings":{"ui.language":"ja-JP"}})");
    this->applySucceeds_ = true;
    this->Click("settings.apply");
    EXPECT_EQ(this->appliedCount_, 1);
    EXPECT_NE(this->Control(L"Static", L"settings.conflict"), nullptr);
}

// 验证冲突重载需确认，拒绝保留草稿，接受读取最新基线。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(SettingsWindowTest, conflict_reload_requires_confirmation)
{
    ASSERT_NE(this->Open(), nullptr);
    this->Select(L"zh-CN");
    this->Write("data/settings.json", R"({"schemaVersion":1,"settings":{"ui.language":"ja-JP"}})");
    this->Click("settings.apply");
    EXPECT_NE(this->Control(L"Static", L"settings.conflict"), nullptr);
    this->confirm_ = false;
    this->Click("settings.reload");
    EXPECT_EQ(this->confirmationCount_, 1);
    EXPECT_NE(IsWindowEnabled(this->Control(L"Button", L"settings.apply")), FALSE);
    this->confirm_ = true;
    this->Click("settings.reload");
    EXPECT_EQ(this->confirmationCount_, 2);
    EXPECT_EQ(IsWindowEnabled(this->Control(L"Button", L"settings.apply")), FALSE);
    EXPECT_EQ(this->appliedCount_, 0);
}

// 验证默认恢复仅改草稿，取消不能覆盖已存用户值。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(SettingsWindowTest, restore_defaults_only_changes_draft)
{
    ASSERT_TRUE(open_st::SetStringSetting("ui.language", "zh-CN"));
    ASSERT_NE(this->Open(), nullptr);
    this->Click("settings.restore_page_defaults");
    EXPECT_EQ(this->confirmationCount_, 1);
    EXPECT_EQ(SendMessageW(this->Control(L"ComboBox"), CB_GETCURSEL, 0, 0), 0);
    EXPECT_EQ(open_st::GetStringSetting("ui.language"), "zh-CN");
    this->Click("settings.cancel");
    EXPECT_EQ(open_st::GetStringSetting("ui.language"), "zh-CN");
}

// 验证损坏读取允许打开可恢复窗口，修复后重载不必重建窗口。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(SettingsWindowTest, damaged_user_can_reload_without_recreating_window)
{
    this->Write("data/settings.json", "{broken");
    const HWND window = this->Open();
    ASSERT_NE(window, nullptr);
    EXPECT_EQ(IsWindowEnabled(this->Control(L"Button", L"settings.ok")), FALSE);
    EXPECT_EQ(IsWindowEnabled(this->Control(L"ComboBox")), FALSE);
    this->Write("data/settings.json", R"({"schemaVersion":1,"settings":{"ui.language":"ja-JP"}})");
    this->Click("settings.reload");
    EXPECT_EQ(open_st::SettingsWindowTestAccess::NativeHandle(this->window_), window);
    EXPECT_NE(IsWindowEnabled(this->Control(L"ComboBox")), FALSE);
    this->Select(L"zh-CN");
    this->Click("settings.apply");
    EXPECT_EQ(this->savedAtNotification_, "zh-CN");
}

// 验证重复打开保留草稿，应用成功重新取文本而不重建界面。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(SettingsWindowTest, repeated_show_preserves_draft_and_refreshes_text)
{
    const HWND window = this->Open();
    ASSERT_NE(window, nullptr);
    this->Select(L"zh-CN");
    EXPECT_EQ(this->Open(), window);
    this->Click("settings.apply");
    std::array<wchar_t, 256> title{};
    GetWindowTextW(window, title.data(), static_cast<int>(title.size()));
    EXPECT_STREQ(title.data(), L"settings.title:zh-CN");
    EXPECT_EQ(this->savedAtNotification_, "zh-CN");
}

// 验证查询异常转成可见错误，不越过窗口边界且不能保存。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(SettingsWindowTest, query_exception_prevents_save)
{
    ASSERT_NE(this->Open(), nullptr);
    this->Select(L"zh-CN");
    this->queryThrows_ = true;
    this->Click("settings.apply");
    EXPECT_TRUE(this->window_.IsOpen());
    EXPECT_EQ(this->appliedCount_, 0);
    EXPECT_EQ(open_st::GetStringSetting("ui.language"), "en-US");
    EXPECT_NE(this->Control(L"Static", L"settings.language.query_failed"), nullptr);
}

// 验证页面改名后恢复默认仍按实际字段所属页执行，不硬编码 general。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(SettingsWindowTest, renamed_page_still_restores_bound_field)
{
    nlohmann::json layout;
    {
        std::ifstream input(this->root_ / "resources/setting_windows.json");
        input >> layout;
    }
    layout["pages"][0]["id"] = "renamed";
    this->Write("resources/setting_windows.json", layout.dump());
    ASSERT_TRUE(open_st::SetStringSetting("ui.language", "zh-CN"));
    ASSERT_NE(this->Open(), nullptr);
    this->Click("settings.restore_page_defaults");
    EXPECT_EQ(this->confirmationCount_, 1);
    EXPECT_EQ(SendMessageW(this->Control(L"ComboBox"), CB_GETCURSEL, 0, 0), 0);
}

// 验证默认语言不可用时恢复失败保持现有草稿，不能显示并接受无效默认值。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(SettingsWindowTest, unavailable_default_preserves_current_draft)
{
    ASSERT_NE(this->Open(), nullptr);
    this->Select(L"zh-CN");
    this->availableLanguages_ = {"zh-CN", "ja-JP"};
    this->RefreshOptions();
    this->Click("settings.restore_page_defaults");
    this->Click("settings.apply");
    EXPECT_EQ(this->savedAtNotification_, "zh-CN");
}

// 验证生效回调重入 Close 必须延迟销毁，返回后正常完成且不访问释放内存。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(SettingsWindowTest, close_inside_application_callback_is_deferred)
{
    this->closeInCallback_ = true;
    ASSERT_NE(this->Open(), nullptr);
    this->Select(L"zh-CN");
    this->Click("settings.apply");
    EXPECT_EQ(this->appliedCount_, 1);
    EXPECT_EQ(this->savedAtNotification_, "zh-CN");
    EXPECT_FALSE(this->window_.IsOpen());
}
// 验证自启失败留下重试目标，语言成功后不重复调用，重试拒绝覆盖外部改变。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(SettingsWindowTest, startup_failure_retries_without_reapplying_language)
{
    open_st::SettingsWindowCallbacks callbacks = this->Callbacks();
    int startupCalls = 0;
    // 核对自启意图已保存，首次模拟失败、后续调用成功。
    // 入参：enabled 为已保存的自启启用意图，用于核验持久化先于系统回调。
    // 返回：首次调用为 false，后续调用为 true，模拟失败后重试成功。
    callbacks.startupApplied = [&startupCalls](bool enabled)
    {
        EXPECT_EQ(open_st::GetBoolSetting("startup.enabled"), enabled);
        ++startupCalls;
        return startupCalls > 1;
    };
    ASSERT_TRUE(this->window_.Show(GetModuleHandleW(nullptr), std::move(callbacks)));
    this->Select(L"zh-CN");
    this->Click("settings.startup.label");
    this->Click("settings.apply");
    EXPECT_EQ(startupCalls, 1);
    EXPECT_EQ(this->appliedCount_, 1);
    EXPECT_EQ(open_st::GetBoolSetting("startup.enabled"), false);
    this->Click("settings.apply");
    EXPECT_EQ(startupCalls, 2);
    EXPECT_EQ(this->appliedCount_, 1);
    this->Click("settings.ok");
    EXPECT_FALSE(this->window_.IsOpen());
}

// 验证显式修复不需要制造草稿变化，也不保存其他待编辑字段。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(SettingsWindowTest, repair_applies_saved_startup_without_committing_language_draft)
{
    open_st::SettingsWindowCallbacks callbacks = this->Callbacks();
    int startupCalls = 0;
    // 核对修复请求使用已保存的启用意图，并累计系统调用次数。
    // 入参：enabled 为已保存的自启启用意图，用于核验持久化先于系统回调。
    // 返回：固定为 true，表示模拟自启设置成功。
    callbacks.startupApplied = [&startupCalls](bool enabled)
    {
        EXPECT_TRUE(enabled);
        ++startupCalls;
        return true;
    };
    ASSERT_TRUE(this->window_.Show(GetModuleHandleW(nullptr), std::move(callbacks)));
    this->Select(L"zh-CN");
    this->Click("settings.startup.repair");
    EXPECT_EQ(startupCalls, 1);
    EXPECT_EQ(open_st::GetStringSetting("ui.language"), "en-US");
    EXPECT_EQ(this->appliedCount_, 0);
}

// 验证欢迎关闭完全不保存，自动用例通过线程消息驱动真实模态窗口。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(SettingsWindowTest, welcome_cancel_leaves_saved_intent_unchanged)
{
    open_st::WelcomeWindow welcome;
    int startupCalls = 0;
    open_st::SettingsWindowCallbacks callbacks = this->Callbacks();
    // 记录模拟自启调用，供断言取消欢迎窗口不会触发系统副作用。
    // 入参：未命名 bool 为期望的自启启用状态；本模拟回调不按该值区分处理。
    // 返回：固定为 true，表示模拟自启设置成功。
    callbacks.startupApplied = [&startupCalls](bool)
    {
        ++startupCalls;
        return true;
    };
    ASSERT_TRUE(this->QueueWelcomeMessage(WM_APP + 91));
    const bool completed = welcome.ShowModal(GetModuleHandleW(nullptr), std::move(callbacks),
                                             // 消费测试线程消息并关闭欢迎窗口，模拟用户取消。
                                             // 入参：message 为模态循环当前取得的线程消息，按测试消息编号决定是否消费。
                                             // 返回：测试消息已消费时为 true；其他消息为 false，交由模态循环继续处理。
                                             [](MSG& message)
                                             {
                                                 if (message.message != WM_APP + 91)
                                                     return false;
                                                 const HWND window = FindWelcomeWindow();
                                                 EXPECT_NE(window, nullptr);
                                                 if (window)
                                                     SendMessageW(window, WM_CLOSE, 0, 0);
                                                 else
                                                     PostQuitMessage(99);
                                                 return true;
                                             });
    EXPECT_FALSE(completed);
    EXPECT_FALSE(welcome.Failed());
    EXPECT_EQ(startupCalls, 0);
    EXPECT_EQ(open_st::GetBoolSetting("onboarding.completed"), false);
}

// 验证欢迎先保存确认标记和意图，系统失败后再次确认只重试系统。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(SettingsWindowTest, welcome_saves_before_system_effect_and_retries)
{
    open_st::WelcomeWindow welcome;
    int startupCalls = 0;
    open_st::SettingsWindowCallbacks callbacks = this->Callbacks();
    // 核对欢迎标记和自启意图先落盘，第二次系统调用才成功。
    // 入参：enabled 为已保存的自启启用意图，用于核验持久化先于系统回调。
    // 返回：仅第二次调用为 true，模拟欢迎确认后的系统操作重试。
    callbacks.startupApplied = [&startupCalls](bool enabled)
    {
        EXPECT_EQ(open_st::GetBoolSetting("onboarding.completed"), true);
        EXPECT_EQ(open_st::GetBoolSetting("startup.enabled"), enabled);
        return ++startupCalls == 2;
    };
    ASSERT_TRUE(this->QueueWelcomeMessage(WM_APP + 92));
    int clicks = 0;
    const bool completed =
        welcome.ShowModal(GetModuleHandleW(nullptr), std::move(callbacks),
                          // 按测试线程消息连续触发欢迎确认，验证系统失败后的重试。
                          // 入参：message 为模态循环当前取得的线程消息，按测试消息编号决定是否消费。
                          // 返回：测试消息已消费时为 true；其他消息为 false，交由模态循环继续处理。
                          [&clicks](MSG& message)
                          {
                              if (message.message != WM_APP + 92)
                                  return false;
                              const HWND window = FindWelcomeWindow();
                              ControlSearch search{L"Button", L"welcome.confirm", nullptr};
                              EnumChildWindows(window, FindControl, reinterpret_cast<LPARAM>(&search));
                              EXPECT_NE(search.found, nullptr);
                              if (!search.found)
                              {
                                  PostQuitMessage(99);
                                  return true;
                              }
                              SendMessageW(search.found, BM_CLICK, 0, 0);
                              if (++clicks == 1)
                                  PostThreadMessageW(GetCurrentThreadId(), WM_APP + 92, 0, 0);
                              return true;
                          });
    EXPECT_TRUE(completed);
    EXPECT_EQ(startupCalls, 2);
}
// 验证外部改写已保存意图后，旧窗口的待生效目标不能覆盖新值。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(SettingsWindowTest, startup_retry_rejects_external_saved_choice)
{
    open_st::SettingsWindowCallbacks callbacks = this->Callbacks();
    int calls = 0;
    // 累计并拒绝模拟自启请求，保留待重试状态。
    // 入参：未命名 bool 为期望的自启启用状态；本模拟回调不按该值区分处理。
    // 返回：固定为 false，模拟自启系统操作失败。
    callbacks.startupApplied = [&calls](bool)
    {
        ++calls;
        return false;
    };
    ASSERT_TRUE(this->window_.Show(GetModuleHandleW(nullptr), std::move(callbacks)));
    this->Click("settings.startup.label");
    this->Click("settings.apply");
    ASSERT_EQ(calls, 1);
    ASSERT_TRUE(open_st::SetBoolSetting("startup.enabled", true));
    this->Click("settings.apply");
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(open_st::GetBoolSetting("startup.enabled"), true);
}

// 验证已显示状态保存为本地化键，外部切换语言后不保留旧译文。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(SettingsWindowTest, refresh_texts_relocalizes_existing_status)
{
    open_st::SettingsWindowCallbacks callbacks = this->Callbacks();
    // 为已保存状态附带模拟运行语言，验证刷新时重新解析文本。
    // 入参：key 为待查询的测试界面文本键。
    // 返回：保存状态键附加模拟语言的文本；其他键使用夹具的常规文本。
    callbacks.text = [this](std::string_view key)
    {
        if (key == "settings.saved")
        {
            const std::string text = std::string(key) + ":" + this->appliedLanguage_;
            return std::wstring(text.begin(), text.end());
        }
        return this->Text(key);
    };
    ASSERT_TRUE(this->window_.Show(GetModuleHandleW(nullptr), std::move(callbacks)));
    this->Select(L"zh-CN");
    this->Click("settings.apply");
    ASSERT_NE(this->Control(L"Static", L"settings.saved:zh-CN"), nullptr);
    this->appliedLanguage_ = "ja-JP";
    this->window_.RefreshTexts();
    EXPECT_NE(this->Control(L"Static", L"settings.saved:ja-JP"), nullptr);
    EXPECT_EQ(this->Control(L"Static", L"settings.saved:zh-CN"), nullptr);
}
// 验证无效创建参数属于故障，不能当作正常取消静默忽略。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(SettingsWindowTest, welcome_reports_creation_failure)
{
    open_st::WelcomeWindow welcome;
    EXPECT_FALSE(welcome.ShowModal(nullptr, this->Callbacks()));
    EXPECT_TRUE(welcome.Failed());
    EXPECT_EQ(open_st::GetBoolSetting("onboarding.completed"), false);
}
// 验证系统操作失败后的退出保留已确认标记，下次启动不会再次首次欢迎。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(SettingsWindowTest, welcome_system_failure_exit_preserves_completion)
{
    open_st::WelcomeWindow welcome;
    open_st::SettingsWindowCallbacks callbacks = this->Callbacks();
    // 模拟自启操作失败，保留欢迎确认后等待处理的状态。
    // 入参：未命名 bool 为期望的自启启用状态；本模拟回调不按该值区分处理。
    // 返回：固定为 false，模拟自启系统操作失败。
    callbacks.startupApplied = [](bool) { return false; };
    ASSERT_TRUE(this->QueueWelcomeMessage(WM_APP + 93));
    EXPECT_FALSE(welcome.ShowModal(GetModuleHandleW(nullptr), std::move(callbacks),
                                   // 按测试线程消息先确认欢迎再退出，验证系统失败不撤销确认标记。
                                   // 入参：message 为模态循环当前取得的线程消息，按测试消息编号决定是否消费。
                                   // 返回：测试消息已消费时为 true；其他消息为 false，交由模态循环继续处理。
                                   [](MSG& message)
                                   {
                                       if (message.message != WM_APP + 93)
                                           return false;
                                       const HWND window = FindWelcomeWindow();
                                       ControlSearch search{L"Button", L"welcome.confirm", nullptr};
                                       EnumChildWindows(window, FindControl, reinterpret_cast<LPARAM>(&search));
                                       EXPECT_NE(search.found, nullptr);
                                       if (!search.found)
                                       {
                                           PostQuitMessage(99);
                                           return true;
                                       }
                                       SendMessageW(search.found, BM_CLICK, 0, 0);
                                       SendMessageW(window, WM_CLOSE, 0, 0);
                                       return true;
                                   }));
    EXPECT_FALSE(welcome.Failed());
    EXPECT_EQ(open_st::GetBoolSetting("onboarding.completed"), true);
    EXPECT_EQ(open_st::GetBoolSetting("startup.enabled"), true);
}
// 验证忙状态通知即使抛出异常仍成对恢复，副作用失败后窗口可继续交互。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(SettingsWindowTest, busy_notifications_are_paired_after_failure)
{
    open_st::SettingsWindowCallbacks callbacks = this->Callbacks();
    std::vector<bool> states;
    // 记录成对忙状态通知并抛出异常，验证通知失败仍恢复交互。
    // 入参：busy 为窗口通知的忙状态，true 表示进入操作，false 表示退出操作。
    // 返回：无返回值。
    callbacks.busyChanged = [&states](bool busy)
    {
        states.push_back(busy);
        throw std::runtime_error("notification failure");
    };
    // 模拟系统副作用抛出异常，检查窗口失败处理与忙状态恢复。
    // 入参：未命名 bool 为期望的自启启用状态；本模拟回调不按该值区分处理。
    // 返回：不正常返回；主动抛出测试异常，交由被测边界处理。
    callbacks.startupApplied = [](bool) -> bool { throw std::runtime_error("system failure"); };
    ASSERT_TRUE(this->window_.Show(GetModuleHandleW(nullptr), std::move(callbacks)));
    this->Click("settings.startup.label");
    this->Click("settings.apply");
    EXPECT_EQ(states, (std::vector<bool>{true, false}));
    EXPECT_TRUE(this->window_.IsOpen());
    EXPECT_NE(IsWindowEnabled(this->Control(L"Button", this->Text("settings.apply"))), FALSE);
    this->Click("settings.cancel");
    EXPECT_FALSE(this->window_.IsOpen());
}
// 验证全局组合占用时整批草稿不保存，语言与原活动快捷键均保持原状。
// 入参：无运行入参；GoogleTest 注册本用例。
// 返回：无，断言检查配置、活动组合与候选生命周期。
TEST_F(SettingsWindowTest, hotkey_prepare_failure_prevents_all_settings_commit)
{
    ASSERT_NE(this->Open(), nullptr);
    this->Select(L"zh-CN");
    ASSERT_TRUE(open_st::SettingsWindowTestAccess::ChangeHotkey(this->window_, "Ctrl+Shift+F8"));
    this->hotkeyPrepareSucceeds_ = false;
    this->Click("settings.apply");
    EXPECT_EQ(open_st::GetStringSetting("ui.language"), "en-US");
    EXPECT_EQ(open_st::GetStringSetting("capture.hotkey"), "Ctrl+Alt+Q");
    EXPECT_EQ(this->activeHotkey_, "Ctrl+Alt+Q");
    EXPECT_EQ(this->hotkeyPrepareCount_, 1);
    EXPECT_EQ(this->hotkeyActivateCount_, 0);
    EXPECT_EQ(this->appliedCount_, 0);
    EXPECT_EQ(open_st::SettingsWindowTestAccess::ReadHotkey(this->window_), "Ctrl+Shift+F8");
}

// 验证注册候选成功后遇到同键外部冲突，候选撤销且整批语言草稿不落盘。
// 入参：无运行入参；GoogleTest 注册本用例。
// 返回：无，断言检查撤销次数、旧组合和未知字段保留。
TEST_F(SettingsWindowTest, hotkey_commit_conflict_cancels_prepared_candidate)
{
    ASSERT_NE(this->Open(), nullptr);
    this->Select(L"zh-CN");
    ASSERT_TRUE(open_st::SettingsWindowTestAccess::ChangeHotkey(this->window_, "Ctrl+Shift+F8"));
    this->Write("data/settings.json",
                R"({"schemaVersion":1,"settings":{"ui.language":"en-US","capture.hotkey":"F9","other":42}})");
    this->Click("settings.apply");
    EXPECT_EQ(this->hotkeyCancelCount_, 1);
    EXPECT_EQ(this->hotkeyActivateCount_, 0);
    EXPECT_EQ(this->activeHotkey_, "Ctrl+Alt+Q");
    EXPECT_EQ(open_st::GetStringSetting("ui.language"), "en-US");
    EXPECT_EQ(open_st::GetStringSetting("capture.hotkey"), "F9");
    EXPECT_NE(this->Control(L"Static", L"settings.conflict"), nullptr);
}

// 验证只读文件使条件提交失败时回收候选，恢复可写后原草稿可直接重试。
// 入参：无运行入参；GoogleTest 注册本用例。
// 返回：无，断言检查失败和后续成功的注册生命周期。
TEST_F(SettingsWindowTest, hotkey_write_failure_cancels_candidate_and_keeps_draft)
{
    ASSERT_NE(this->Open(), nullptr);
    ASSERT_TRUE(open_st::SettingsWindowTestAccess::ChangeHotkey(this->window_, "Ctrl+Shift+F8"));
    ASSERT_NE(SetFileAttributesW((this->root_ / "data/settings.json").c_str(), FILE_ATTRIBUTE_READONLY), FALSE);
    this->Click("settings.apply");
    EXPECT_EQ(this->hotkeyCancelCount_, 1);
    EXPECT_EQ(this->activeHotkey_, "Ctrl+Alt+Q");
    EXPECT_EQ(open_st::GetStringSetting("capture.hotkey"), "Ctrl+Alt+Q");
    ASSERT_NE(SetFileAttributesW((this->root_ / "data/settings.json").c_str(), FILE_ATTRIBUTE_NORMAL), FALSE);
    this->Click("settings.apply");
    EXPECT_EQ(this->hotkeyActivateCount_, 1);
    EXPECT_EQ(this->activeHotkey_, "Ctrl+Shift+F8");
}

// 验证准备先于保存、激活读取到新配置且早于其他语言副作用，不重复激活。
// 入参：无运行入参；GoogleTest 注册本用例。
// 返回：无，断言检查实际磁盘观察和回调顺序。
TEST_F(SettingsWindowTest, hotkey_commit_activates_before_language_effect)
{
    ASSERT_NE(this->Open(), nullptr);
    this->Select(L"zh-CN");
    ASSERT_TRUE(open_st::SettingsWindowTestAccess::ChangeHotkey(this->window_, "Ctrl+Shift+F8"));
    this->Click("settings.apply");
    EXPECT_EQ(this->hotkeyAtPrepare_, "Ctrl+Alt+Q");
    EXPECT_EQ(this->hotkeyAtActivation_, "Ctrl+Shift+F8");
    EXPECT_EQ(this->hotkeyEvents_, (std::vector<std::string>{"prepare", "activate", "language"}));
    this->Click("settings.ok");
    EXPECT_EQ(this->hotkeyActivateCount_, 1);
    EXPECT_FALSE(this->window_.IsOpen());
}

// 验证未注册且无草稿变化时应用可用，只读文件也可完成注册而不重复写盘。
// 入参：无运行入参；GoogleTest 注册本用例。
// 返回：无，断言检查按钮状态、注册次数和正常生效后的按钮禁用。
TEST_F(SettingsWindowTest, hotkey_apply_without_changes_registers_without_writing)
{
    this->activeHotkey_.clear();
    ASSERT_NE(this->Open(), nullptr);
    ASSERT_NE(SetFileAttributesW((this->root_ / "data/settings.json").c_str(), FILE_ATTRIBUTE_READONLY), FALSE);
    this->Click("settings.apply");
    EXPECT_EQ(this->hotkeyPrepareCount_, 1);
    EXPECT_EQ(this->hotkeyActivateCount_, 1);
    EXPECT_EQ(open_st::GetStringSetting("ui.language"), "en-US");
    EXPECT_EQ(this->appliedCount_, 0);
    EXPECT_EQ(IsWindowEnabled(this->Control(L"Button", this->Text("settings.apply"))), FALSE);
}

// 验证显式重试先核验磁盘目标，外部同键修改后不再准备旧目标。
// 入参：无运行入参；GoogleTest 注册本用例。
// 返回：无，断言检查没有任何系统注册副作用。
TEST_F(SettingsWindowTest, hotkey_retry_rejects_external_change)
{
    this->activeHotkey_.clear();
    ASSERT_NE(this->Open(), nullptr);
    ASSERT_TRUE(open_st::SetStringSetting("capture.hotkey", "Ctrl+Shift+F8"));
    this->Click("settings.apply");
    EXPECT_EQ(this->hotkeyPrepareCount_, 0);
    EXPECT_EQ(this->hotkeyActivateCount_, 0);
    EXPECT_TRUE(this->activeHotkey_.empty());
    EXPECT_NE(this->Control(L"Static", L"settings.conflict"), nullptr);
}

// 验证旧用户文件缺少新键时按未变默认值重试成功，仍不补写用户文件。
// 入参：无运行入参；GoogleTest 注册本用例。
// 返回：无，断言检查注册成功和磁盘字段保持缺失。
TEST_F(SettingsWindowTest, hotkey_retry_accepts_unchanged_missing_field_default)
{
    this->activeHotkey_.clear();
    this->Write("data/settings.json", R"({"schemaVersion":1,"settings":{"ui.language":"en-US"}})");
    ASSERT_NE(this->Open(), nullptr);
    ASSERT_NE(SetFileAttributesW((this->root_ / "data/settings.json").c_str(), FILE_ATTRIBUTE_READONLY), FALSE);
    this->Click("settings.apply");
    EXPECT_EQ(this->hotkeyActivateCount_, 1);
    nlohmann::json document;
    std::ifstream input(this->root_ / "data/settings.json");
    input >> document;
    EXPECT_FALSE(document.at("settings").contains("capture.hotkey"));
}

// 验证用户字段缺失时重试仍检查默认资源，默认被外部改变后不能注册旧显示目标。
// 入参：无运行入参；GoogleTest 注册本用例。
// 返回：无，断言检查默认变化不会触发系统注册。
TEST_F(SettingsWindowTest, hotkey_retry_rejects_changed_missing_field_default)
{
    this->activeHotkey_.clear();
    this->Write("data/settings.json", R"({"schemaVersion":1,"settings":{"ui.language":"en-US"}})");
    ASSERT_NE(this->Open(), nullptr);
    this->Write("resources/default_settings.json",
                R"({"schemaVersion":1,"settings":{"capture.hotkey":"Ctrl+Shift+F8"}})");
    this->Click("settings.apply");
    EXPECT_EQ(this->hotkeyPrepareCount_, 0);
    EXPECT_EQ(this->hotkeyActivateCount_, 0);
    EXPECT_NE(this->Control(L"Static", L"settings.conflict"), nullptr);
}

// 验证快捷键默认资源语义非法时保留整份草稿，不替换为不可录入的组合。
// 入参：无运行入参；GoogleTest 注册本用例。
// 返回：无，断言检查默认失败提示和原快捷键草稿。
TEST_F(SettingsWindowTest, hotkey_invalid_default_preserves_current_draft)
{
    ASSERT_NE(this->Open(), nullptr);
    ASSERT_TRUE(open_st::SettingsWindowTestAccess::ChangeHotkey(this->window_, "Ctrl+Shift+F8"));
    this->Write("resources/default_settings.json",
                R"({"schemaVersion":1,"settings":{"capture.hotkey":"invalid-token"}})");
    this->SelectPage(1);
    this->Click("settings.restore_page_defaults");
    EXPECT_EQ(open_st::SettingsWindowTestAccess::ReadHotkey(this->window_), "Ctrl+Shift+F8");
    EXPECT_NE(this->Control(L"Static", L"settings.defaults_failed"), nullptr);
    EXPECT_EQ(this->hotkeyPrepareCount_, 0);
}

// 验证快捷键页恢复默认只修改该页草稿，取消不覆盖已保存组合或语言。
// 入参：无运行入参；GoogleTest 注册本用例。
// 返回：无，断言检查页面范围、草稿与取消无副作用。
TEST_F(SettingsWindowTest, hotkey_page_defaults_preserve_other_page_and_cancel)
{
    ASSERT_TRUE(open_st::SetStringSetting("capture.hotkey", "Ctrl+Shift+F8"));
    ASSERT_NE(this->Open(), nullptr);
    this->Select(L"zh-CN");
    this->SelectPage(1);
    this->Click("settings.restore_page_defaults");
    EXPECT_EQ(this->confirmationCount_, 1);
    EXPECT_EQ(open_st::SettingsWindowTestAccess::ReadHotkey(this->window_), "Ctrl+Alt+Q");
    EXPECT_EQ(SendMessageW(this->Control(L"ComboBox"), CB_GETCURSEL, 0, 0), 1);
    this->Click("settings.cancel");
    EXPECT_EQ(open_st::GetStringSetting("capture.hotkey"), "Ctrl+Shift+F8");
    EXPECT_EQ(open_st::GetStringSetting("ui.language"), "en-US");
    EXPECT_EQ(this->hotkeyPrepareCount_, 0);
}

// 验证已保存并激活的新组合不因旧注册清理失败而回滚，但确定仍保留窗口供重试。
// 入参：无运行入参；GoogleTest 注册本用例。
// 返回：无，断言检查持久化、活动组合和可重试窗口。
TEST_F(SettingsWindowTest, hotkey_cleanup_failure_preserves_saved_active_combination)
{
    ASSERT_NE(this->Open(), nullptr);
    ASSERT_TRUE(open_st::SettingsWindowTestAccess::ChangeHotkey(this->window_, "Ctrl+Shift+F8"));
    this->hotkeyCleanupSucceeds_ = false;
    this->Click("settings.ok");
    EXPECT_EQ(open_st::GetStringSetting("capture.hotkey"), "Ctrl+Shift+F8");
    EXPECT_EQ(this->activeHotkey_, "Ctrl+Shift+F8");
    EXPECT_TRUE(this->window_.IsOpen());
    this->hotkeyCleanupSucceeds_ = true;
    this->Click("settings.apply");
    this->Click("settings.ok");
    EXPECT_FALSE(this->window_.IsOpen());
}
// 验证真实录入完整组合后立即点亮应用，无须先 Enter 或离开录入控件。
// 入参：无运行入参；GoogleTest 注册本用例。
// 返回：无，断言检查按键、草稿、按钮和最终保存注册链路。
TEST_F(SettingsWindowTest, complete_chord_input_immediately_enables_apply)
{
    ASSERT_NE(this->Open(), nullptr);
    this->SelectPage(1);
    const HWND apply = this->Control(L"Button", this->Text("settings.apply"));
    ASSERT_NE(apply, nullptr);
    EXPECT_EQ(IsWindowEnabled(apply), FALSE);
    const HWND chord = this->Control(L"Button", L"Ctrl+Alt+Q");
    ASSERT_NE(chord, nullptr);
    SendMessageW(chord, WM_SETFOCUS, 0, 0);
    ASSERT_TRUE(this->ChordKey(chord, WM_KEYDOWN, VK_CONTROL));
    ASSERT_TRUE(this->ChordKey(chord, WM_KEYDOWN, VK_SHIFT));
    ASSERT_TRUE(this->ChordKey(chord, WM_KEYDOWN, VK_F8));
    EXPECT_EQ(open_st::SettingsWindowTestAccess::ReadHotkey(this->window_), "Ctrl+Shift+F8");
    EXPECT_NE(IsWindowEnabled(apply), FALSE);
    EXPECT_EQ(open_st::GetStringSetting("capture.hotkey"), "Ctrl+Alt+Q");
    this->Click("settings.apply");
    EXPECT_EQ(open_st::GetStringSetting("capture.hotkey"), "Ctrl+Shift+F8");
    EXPECT_EQ(this->activeHotkey_, "Ctrl+Shift+F8");
    EXPECT_EQ(this->hotkeyActivateCount_, 1);
    EXPECT_EQ(IsWindowEnabled(apply), FALSE);
}

// 验证即时录入被 Esc 撤销时恢复原始非法 token，不将取消误当作修复保存。
// 入参：无运行入参；GoogleTest 注册本用例。
// 返回：无，断言检查原始草稿和持久化都保持非法原值、窗口仍存在。
TEST_F(SettingsWindowTest, escape_after_live_chord_restores_invalid_original_token)
{
    ASSERT_TRUE(open_st::SetStringSetting("capture.hotkey", "invalid-original"));
    ASSERT_NE(this->Open(), nullptr);
    this->SelectPage(1);
    const HWND chord = this->Control(L"Button", L"Ctrl+Alt+Q");
    ASSERT_NE(chord, nullptr);
    SendMessageW(chord, WM_SETFOCUS, 0, 0);
    ASSERT_TRUE(this->ChordKey(chord, WM_KEYDOWN, VK_CONTROL));
    ASSERT_TRUE(this->ChordKey(chord, WM_KEYDOWN, VK_SHIFT));
    ASSERT_TRUE(this->ChordKey(chord, WM_KEYDOWN, VK_F8));
    EXPECT_EQ(open_st::SettingsWindowTestAccess::ReadHotkey(this->window_), "Ctrl+Shift+F8");
    ASSERT_TRUE(this->ChordKey(chord, WM_KEYDOWN, VK_ESCAPE));
    EXPECT_EQ(open_st::SettingsWindowTestAccess::ReadHotkey(this->window_), "invalid-original");
    EXPECT_EQ(open_st::GetStringSetting("capture.hotkey"), "invalid-original");
    EXPECT_TRUE(this->window_.IsOpen());
    EXPECT_EQ(this->hotkeyPrepareCount_, 0);
}

// 验证应用在重试未改的快捷键目标时仍将其他字段草稿作为同一批设置提交。
// 入参：无运行入参；GoogleTest 注册本用例。
// 返回：无，断言检查语言提交、快捷键激活及副作用顺序。
TEST_F(SettingsWindowTest, hotkey_apply_retry_commits_other_field_drafts)
{
    this->activeHotkey_.clear();
    ASSERT_NE(this->Open(), nullptr);
    this->Select(L"zh-CN");
    this->Click("settings.apply");
    EXPECT_EQ(open_st::GetStringSetting("ui.language"), "zh-CN");
    EXPECT_EQ(this->activeHotkey_, "Ctrl+Alt+Q");
    EXPECT_EQ(this->hotkeyEvents_, (std::vector<std::string>{"prepare", "activate", "language"}));
    EXPECT_EQ(IsWindowEnabled(this->Control(L"Button", this->Text("settings.apply"))), FALSE);
}
// 验证真实颜色输入即时激活应用，写入失败保留小写原文，成功后才显示规范大写。
// 入参：无运行入参。
// 返回：无，断言检查控件文字、磁盘值及失败恢复。
TEST_F(SettingsWindowTest, storage_color_normalizes_only_after_successful_commit)
{
    ASSERT_NE(this->Open(), nullptr);
    this->SelectPage(2);
    const HWND color = this->Control(L"Edit", L"#000000");
    ASSERT_NE(color, nullptr);
    SetWindowTextW(color, L"#aabbcc");
    this->Pump();
    EXPECT_NE(IsWindowEnabled(this->Control(L"Button", this->Text("settings.apply"))), FALSE);
    ASSERT_NE(SetFileAttributesW((this->root_ / "data/settings.json").c_str(), FILE_ATTRIBUTE_READONLY), FALSE);
    this->Click("settings.apply");
    std::array<wchar_t, 32> text{};
    GetWindowTextW(color, text.data(), static_cast<int>(text.size()));
    EXPECT_STREQ(text.data(), L"#aabbcc");
    EXPECT_EQ(open_st::GetStringSetting("capture.selection_border_color"), "#000000");
    ASSERT_NE(SetFileAttributesW((this->root_ / "data/settings.json").c_str(), FILE_ATTRIBUTE_NORMAL), FALSE);
    this->Click("settings.apply");
    EXPECT_EQ(open_st::GetStringSetting("capture.selection_border_color"), "#AABBCC");
    GetWindowTextW(color, text.data(), static_cast<int>(text.size()));
    EXPECT_STREQ(text.data(), L"#AABBCC");
}

// 验证半个色值与无效数字均阻止整个设置批次，语言刷新不会吞掉用户原输入。
// 入参：无运行入参。
// 返回：无，断言检查按钮禁用、原输入保留和取消无写入。
TEST_F(SettingsWindowTest, storage_invalid_input_blocks_all_fields_and_survives_text_refresh)
{
    ASSERT_NE(this->Open(), nullptr);
    this->Select(L"zh-CN");
    this->SelectPage(2);
    const HWND color = this->Control(L"Edit", L"#000000");
    const HWND quality = this->Control(L"Edit", L"95");
    ASSERT_NE(color, nullptr);
    ASSERT_NE(quality, nullptr);
    SetWindowTextW(color, L"#123");
    this->Pump();
    EXPECT_EQ(IsWindowEnabled(this->Control(L"Button", this->Text("settings.apply"))), FALSE);
    EXPECT_EQ(IsWindowEnabled(this->Control(L"Button", this->Text("settings.ok"))), FALSE);
    SetWindowTextW(color, L"#123456");
    SetWindowTextW(quality, L"95.5");
    this->Pump();
    this->window_.RefreshTexts();
    std::array<wchar_t, 32> text{};
    GetWindowTextW(quality, text.data(), static_cast<int>(text.size()));
    EXPECT_STREQ(text.data(), L"95.5");
    EXPECT_EQ(IsWindowEnabled(this->Control(L"Button", this->Text("settings.apply"))), FALSE);
    this->Click("settings.cancel");
    EXPECT_EQ(open_st::GetStringSetting("ui.language"), "en-US");
    EXPECT_EQ(open_st::GetIntegerSetting("export.jpeg_quality"), 95);
}

// 验证原始字段类型错误显示默认并允许显式应用修复，未知字段不被删除。
// 入参：无运行入参。
// 返回：无，断言检查待修复按钮和持久化真实类型。
TEST_F(SettingsWindowTest, storage_wrong_types_enable_explicit_default_repair)
{
    this->Write(
        "data/settings.json",
        R"({"schemaVersion":1,"settings":{"ui.language":"en-US",)"
        R"("capture.selection_border_color":42,"export.default_format":false,"export.jpeg_quality":95.0,"unknown":7}})");
    ASSERT_NE(this->Open(), nullptr);
    this->Click("settings.apply");
    nlohmann::json document;
    std::ifstream input(this->root_ / "data/settings.json");
    input >> document;
    EXPECT_EQ(document.at("settings").at("capture.selection_border_color"), "#000000");
    EXPECT_EQ(document.at("settings").at("export.default_format"), "jpeg");
    EXPECT_TRUE(document.at("settings").at("export.jpeg_quality").is_number_integer());
    EXPECT_EQ(document.at("settings").at("unknown"), 7);
    EXPECT_EQ(IsWindowEnabled(this->Control(L"Button", this->Text("settings.apply"))), FALSE);
}

// 验证业务非法质量原值保持可见，不夹取成最大值；恢复本页默认只改变草稿。
// 入参：无运行入参。
// 返回：无，断言检查语义错误和默认恢复后的未保存边界。
TEST_F(SettingsWindowTest, storage_semantic_quality_error_requires_edit_or_defaults)
{
    ASSERT_TRUE(open_st::SetIntegerSetting("export.jpeg_quality", 999));
    ASSERT_NE(this->Open(), nullptr);
    this->SelectPage(2);
    EXPECT_NE(this->Control(L"Edit", L"999"), nullptr);
    EXPECT_EQ(IsWindowEnabled(this->Control(L"Button", this->Text("settings.ok"))), FALSE);
    this->Click("settings.restore_page_defaults");
    EXPECT_NE(this->Control(L"Edit", L"95"), nullptr);
    EXPECT_NE(IsWindowEnabled(this->Control(L"Button", this->Text("settings.apply"))), FALSE);
    this->Click("settings.cancel");
    EXPECT_EQ(open_st::GetIntegerSetting("export.jpeg_quality"), 999);
}
// 验证颜色仅改大小写仍检查原始字段冲突，冲突阻止同批语言和快捷键提交。
// 入参：无运行入参。
// 返回：无，断言检查外部值保留、原始输入保留以及候选撤销。
TEST_F(SettingsWindowTest, storage_case_only_edit_detects_external_conflict_for_entire_batch)
{
    ASSERT_TRUE(open_st::SetStringSetting("capture.selection_border_color", "#AABBCC"));
    ASSERT_NE(this->Open(), nullptr);
    this->Select(L"zh-CN");
    ASSERT_TRUE(open_st::SettingsWindowTestAccess::ChangeHotkey(this->window_, "Ctrl+Shift+F8"));
    this->SelectPage(2);
    const HWND color = this->Control(L"Edit", L"#AABBCC");
    ASSERT_NE(color, nullptr);
    SetWindowTextW(color, L"#aabbcc");
    this->Pump();
    ASSERT_TRUE(open_st::SetStringSetting("capture.selection_border_color", "#123456"));
    this->Click("settings.apply");
    EXPECT_NE(this->Control(L"Static", L"settings.conflict"), nullptr);
    EXPECT_EQ(open_st::GetStringSetting("capture.selection_border_color"), "#123456");
    EXPECT_EQ(open_st::GetStringSetting("ui.language"), "en-US");
    EXPECT_EQ(open_st::GetStringSetting("capture.hotkey"), "Ctrl+Alt+Q");
    EXPECT_EQ(this->hotkeyCancelCount_, 1);
    EXPECT_EQ(this->hotkeyActivateCount_, 0);
    std::array<wchar_t, 32> text{};
    GetWindowTextW(color, text.data(), static_cast<int>(text.size()));
    EXPECT_STREQ(text.data(), L"#aabbcc");
}

// 验证无外部冲突的大小写修改完成规范化并清除草稿，不重写内容相同文件的原始排版。
// 入参：无运行入参。
// 返回：无，断言检查可见规范值、按钮状态和用户文件字节。
TEST_F(SettingsWindowTest, storage_case_only_edit_normalizes_without_rewriting_identical_document)
{
    const std::string original = "{ \"schemaVersion\" : 1, \"settings\" : { \"ui.language\" : \"en-US\", "
                                 "\"capture.selection_border_color\" : \"#AABBCC\" } }\n";
    this->Write("data/settings.json", original);
    ASSERT_NE(this->Open(), nullptr);
    this->SelectPage(2);
    const HWND color = this->Control(L"Edit", L"#AABBCC");
    ASSERT_NE(color, nullptr);
    SetWindowTextW(color, L"#aabbcc");
    this->Pump();
    this->Click("settings.apply");
    std::array<wchar_t, 32> text{};
    GetWindowTextW(color, text.data(), static_cast<int>(text.size()));
    EXPECT_STREQ(text.data(), L"#AABBCC");
    EXPECT_EQ(IsWindowEnabled(this->Control(L"Button", this->Text("settings.apply"))), FALSE);
    std::ifstream input(this->root_ / "data/settings.json", std::ios::binary);
    const std::string saved{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    EXPECT_EQ(saved, original);
    EXPECT_EQ(this->hotkeyPrepareCount_, 0);
}
// 验证非法草稿不阻断日志操作，确认取消不启动任务，运行期间只禁用清理按钮。
// 入参：无运行入参。
// 返回：无，通过控件、文件及模拟调用断言验证结果。
TEST_F(SettingsWindowTest, maintenance_ignores_invalid_draft_and_prevents_duplicate_task)
{
    ASSERT_NE(this->Open(), nullptr);
    this->SelectPage(2);
    const HWND color = this->Control(L"Edit", L"#000000");
    ASSERT_NE(color, nullptr);
    SetWindowTextW(color, L"#12");
    this->SelectPage(3);
    this->Click("settings.maintenance.open_directory");
    EXPECT_EQ(this->openDirectoryCount_, 1);
    this->confirm_ = false;
    this->Click("settings.maintenance.clear_history");
    EXPECT_EQ(this->clearHistoryCount_, 0);
    this->confirm_ = true;
    this->Click("settings.maintenance.clear_history");
    EXPECT_EQ(this->clearHistoryCount_, 1);
    EXPECT_EQ(IsWindowEnabled(this->Control(L"Button", this->Text("settings.maintenance.clear_history"))), FALSE);
    EXPECT_NE(IsWindowEnabled(this->Control(L"Button", this->Text("settings.maintenance.open_directory"))), FALSE);
    std::array<wchar_t, 32> text{};
    GetWindowTextW(color, text.data(), static_cast<int>(text.size()));
    EXPECT_STREQ(text.data(), L"#12");
    EXPECT_EQ(open_st::GetStringSetting("capture.selection_border_color"), "#000000");
    this->window_.Close();
    ASSERT_NE(this->Open(), nullptr);
    EXPECT_EQ(IsWindowEnabled(this->Control(L"Button", this->Text("settings.maintenance.clear_history"))), FALSE);
    this->maintenanceRunning_ = false;
    this->maintenanceText_ = L"maintenance completed";
    this->window_.RefreshTexts();
    EXPECT_NE(IsWindowEnabled(this->Control(L"Button", this->Text("settings.maintenance.clear_history"))), FALSE);
    EXPECT_NE(this->Control(L"Static", L"maintenance completed"), nullptr);
}

// 验证缺失维护回调时按钮确实禁用，未接入宿主不能显示为执行成功。
// 入参：无运行入参。
// 返回：无，通过控件、文件及模拟调用断言验证结果。
TEST_F(SettingsWindowTest, maintenance_missing_callbacks_are_disabled)
{
    open_st::SettingsWindowCallbacks callbacks = this->Callbacks();
    callbacks.openLogDirectory = {};
    callbacks.clearHistoricalLogs = {};
    callbacks.maintenanceStatus = {};
    ASSERT_TRUE(this->window_.Show(GetModuleHandleW(nullptr), std::move(callbacks)));
    EXPECT_EQ(IsWindowEnabled(this->Control(L"Button", this->Text("settings.maintenance.open_directory"))), FALSE);
    EXPECT_EQ(IsWindowEnabled(this->Control(L"Button", this->Text("settings.maintenance.clear_history"))), FALSE);
}

// 验证目录打开失败与清理启动失败各自报告失败，失败后清理仍能重新确认启动。
// 入参：无运行入参。
// 返回：无，通过控件、文件及模拟调用断言验证结果。
TEST_F(SettingsWindowTest, maintenance_action_failures_remain_retryable)
{
    ASSERT_NE(this->Open(), nullptr);
    this->SelectPage(3);
    this->openDirectorySucceeds_ = false;
    this->Click("settings.maintenance.open_directory");
    EXPECT_NE(this->Control(L"Static", this->Text("settings.maintenance.open_failed")), nullptr);
    this->clearHistorySucceeds_ = false;
    this->Click("settings.maintenance.clear_history");
    EXPECT_NE(this->Control(L"Static", this->Text("settings.maintenance.start_failed")), nullptr);
    this->clearHistorySucceeds_ = true;
    this->Click("settings.maintenance.clear_history");
    EXPECT_EQ(this->clearHistoryCount_, 2);
    EXPECT_TRUE(this->maintenanceRunning_);
}

// 验证恢复取消保留原始颜色和半成品数字，确认后立即保存且强制检查未变化的系统意图。
// 入参：无运行入参。
// 返回：无，通过控件、文件及模拟调用断言验证结果。
TEST_F(SettingsWindowTest, maintenance_restore_replaces_invalid_draft_only_after_confirmation)
{
    ASSERT_NE(this->Open(), nullptr);
    this->SelectPage(2);
    const HWND color = this->Control(L"Edit", L"#000000");
    const HWND quality = this->Control(L"Edit", L"95");
    ASSERT_NE(color, nullptr);
    ASSERT_NE(quality, nullptr);
    SetWindowTextW(color, L"#12");
    SetWindowTextW(quality, L"95.");
    this->SelectPage(3);
    this->confirm_ = false;
    this->Click("settings.maintenance.restore_all");
    std::array<wchar_t, 32> text{};
    GetWindowTextW(quality, text.data(), static_cast<int>(text.size()));
    EXPECT_STREQ(text.data(), L"95.");
    EXPECT_EQ(this->hotkeyPrepareCount_, 0);
    this->confirm_ = true;
    this->Click("settings.maintenance.restore_all");
    EXPECT_EQ(this->hotkeyPrepareCount_, 1);
    EXPECT_EQ(this->hotkeyActivateCount_, 1);
    EXPECT_EQ(this->appliedCount_, 1);
    GetWindowTextW(quality, text.data(), static_cast<int>(text.size()));
    EXPECT_STREQ(text.data(), L"95");
    GetWindowTextW(color, text.data(), static_cast<int>(text.size()));
    EXPECT_STREQ(text.data(), L"#000000");
    EXPECT_EQ(IsWindowEnabled(this->Control(L"Button", this->Text("settings.apply"))), FALSE);
}

// 验证恢复仅覆盖六项设置，保留未知字段、首次欢迎状态及保存目录记忆。
// 入参：无运行入参。
// 返回：无，通过控件、文件及模拟调用断言验证结果。
TEST_F(SettingsWindowTest, maintenance_restore_preserves_noneditable_settings)
{
    this->Write("data/settings.json",
                R"({"schemaVersion":1,"settings":{"ui.language":"zh-CN","startup.enabled":false,)"
                R"("capture.hotkey":"Ctrl+Shift+F8","capture.selection_border_color":"#FFFFFF",)"
                R"("export.default_format":"png","export.jpeg_quality":70,"onboarding.completed":true,)"
                R"("capture.last_save_directory":"kept-directory","future.setting":"kept"}})");
    ASSERT_NE(this->Open(), nullptr);
    this->SelectPage(3);
    this->Click("settings.maintenance.restore_all");
    EXPECT_EQ(open_st::GetStringSetting("ui.language"), "en-US");
    EXPECT_EQ(open_st::GetBoolSetting("startup.enabled"), true);
    EXPECT_EQ(open_st::GetStringSetting("capture.hotkey"), "Ctrl+Alt+Q");
    EXPECT_EQ(open_st::GetStringSetting("capture.selection_border_color"), "#000000");
    EXPECT_EQ(open_st::GetStringSetting("export.default_format"), "jpeg");
    EXPECT_EQ(open_st::GetIntegerSetting("export.jpeg_quality"), 95);
    EXPECT_EQ(open_st::GetBoolSetting("onboarding.completed"), true);
    EXPECT_EQ(open_st::GetStringSetting("capture.last_save_directory"), "kept-directory");
    EXPECT_EQ(open_st::GetStringSetting("future.setting"), "kept");
}

// 验证默认组合被占用和提交只读失败均保留草稿；已准备的候选必须撤销。
// 入参：无运行入参。
// 返回：无，通过控件、文件及模拟调用断言验证结果。
TEST_F(SettingsWindowTest, maintenance_restore_prepare_and_write_failure_preserve_draft)
{
    this->Write("data/settings.json", R"({"schemaVersion":1,"settings":{"ui.language":"ja-JP","startup.enabled":true,)"
                                      R"("capture.hotkey":"Ctrl+Alt+Q","capture.selection_border_color":"#000000",)"
                                      R"("export.default_format":"jpeg","export.jpeg_quality":95}})");
    ASSERT_NE(this->Open(), nullptr);
    this->Select(L"zh-CN");
    this->SelectPage(3);
    this->hotkeyPrepareSucceeds_ = false;
    this->Click("settings.maintenance.restore_all");
    EXPECT_NE(this->Control(L"Static", this->Text("settings.maintenance.defaults_hotkey_failed")), nullptr);
    EXPECT_EQ(this->appliedCount_, 0);
    EXPECT_EQ(this->hotkeyActivateCount_, 0);
    this->hotkeyPrepareSucceeds_ = true;
    ASSERT_NE(SetFileAttributesW((this->root_ / "data/settings.json").c_str(), FILE_ATTRIBUTE_READONLY), FALSE);
    this->Click("settings.maintenance.restore_all");
    EXPECT_EQ(this->hotkeyCancelCount_, 1);
    EXPECT_EQ(this->hotkeyActivateCount_, 0);
    ASSERT_NE(SetFileAttributesW((this->root_ / "data/settings.json").c_str(), FILE_ATTRIBUTE_NORMAL), FALSE);
    this->Click("settings.apply");
    EXPECT_EQ(open_st::GetStringSetting("ui.language"), "zh-CN");
}

// 验证全部恢复即使同值也检查外部修改，冲突时不覆盖外部设置。
// 入参：无运行入参。
// 返回：无，通过控件、文件及模拟调用断言验证结果。
TEST_F(SettingsWindowTest, maintenance_restore_checks_same_value_external_conflict)
{
    ASSERT_NE(this->Open(), nullptr);
    this->Write("data/settings.json", R"({"schemaVersion":1,"settings":{"ui.language":"ja-JP","startup.enabled":true,)"
                                      R"("capture.hotkey":"Ctrl+Alt+Q","capture.selection_border_color":"#000000",)"
                                      R"("export.default_format":"jpeg","export.jpeg_quality":95}})");
    this->SelectPage(3);
    this->Click("settings.maintenance.restore_all");
    EXPECT_EQ(open_st::GetStringSetting("ui.language"), "ja-JP");
    EXPECT_EQ(this->hotkeyCancelCount_, 1);
    EXPECT_EQ(this->hotkeyActivateCount_, 0);
    EXPECT_EQ(this->appliedCount_, 0);
}

// 验证默认候选在确认前冻结，确认期间默认资源变化不替换已展示的数值。
// 入参：无运行入参。
// 返回：无，通过控件、文件及模拟调用断言验证结果。
TEST_F(SettingsWindowTest, maintenance_restore_uses_frozen_default_candidate)
{
    ASSERT_NE(this->Open(), nullptr);
    open_st::SettingsWindowTestAccess::SetConfirmation(
        this->window_,
        // 模拟确认窗口打开期间默认资源被外部修改。
        // 入参：无。
        // 返回：true，确认采用此前冻结的候选。
        [this]()
        {
            this->Write("resources/default_settings.json",
                        R"({"schemaVersion":1,"settings":{"ui.language":"ja-JP","startup.enabled":false,)"
                        R"("capture.hotkey":"Ctrl+Shift+F8","capture.selection_border_color":"#FFFFFF",)"
                        R"("export.default_format":"png","export.jpeg_quality":70}})");
            return true;
        });
    this->SelectPage(3);
    this->Click("settings.maintenance.restore_all");
    EXPECT_EQ(open_st::GetStringSetting("ui.language"), "en-US");
    EXPECT_EQ(open_st::GetBoolSetting("startup.enabled"), true);
    EXPECT_EQ(open_st::GetIntegerSetting("export.jpeg_quality"), 95);
}

// 验证坏默认在确认前被拒绝，坏用户文件在条件提交时被拒绝且不重建文件。
// 入参：无运行入参。
// 返回：无，通过控件、文件及模拟调用断言验证结果。
TEST_F(SettingsWindowTest, maintenance_restore_rejects_invalid_defaults_and_corrupt_user_file)
{
    ASSERT_NE(this->Open(), nullptr);
    this->Select(L"zh-CN");
    this->SelectPage(3);
    this->Write("resources/default_settings.json",
                R"({"schemaVersion":1,"settings":{"ui.language":"en-US","startup.enabled":true,)"
                R"("capture.hotkey":"Ctrl+Alt+Q","capture.selection_border_color":"#12",)"
                R"("export.default_format":"jpeg","export.jpeg_quality":95}})");
    this->Click("settings.maintenance.restore_all");
    EXPECT_EQ(this->confirmationCount_, 0);
    EXPECT_EQ(this->hotkeyPrepareCount_, 0);
    this->Write("resources/default_settings.json",
                R"({"schemaVersion":1,"settings":{"ui.language":"en-US","startup.enabled":true,)"
                R"("capture.hotkey":"Ctrl+Alt+Q","capture.selection_border_color":"#000000",)"
                R"("export.default_format":"jpeg","export.jpeg_quality":95}})");
    this->Write("data/settings.json", "{broken-user-file");
    this->Click("settings.maintenance.restore_all");
    EXPECT_EQ(this->hotkeyCancelCount_, 1);
    EXPECT_EQ(this->hotkeyActivateCount_, 0);
    EXPECT_EQ(this->appliedCount_, 0);
    std::ifstream input(this->root_ / "data/settings.json", std::ios::binary);
    const std::string saved{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    EXPECT_EQ(saved, "{broken-user-file");
}

// 验证恢复在配置同值时仍应用自启与语言，副作用失败后应用按钮重试已保存目标。
// 入参：无运行入参。
// 返回：无，通过控件、文件及模拟调用断言验证结果。
TEST_F(SettingsWindowTest, maintenance_restore_retries_effects_after_defaults_saved)
{
    int startupCount = 0;
    bool startupSucceeds = false;
    open_st::SettingsWindowCallbacks callbacks = this->Callbacks();
    // 记录已持久化默认意图的系统应用请求，允许模拟失败后重试。
    // 入参：target 为已保存的自启目标。
    // 返回：startupSucceeds，表示本次模拟系统应用是否成功。
    callbacks.startupApplied = [&startupCount, &startupSucceeds](bool target)
    {
        ++startupCount;
        EXPECT_TRUE(target);
        EXPECT_EQ(open_st::GetBoolSetting("startup.enabled"), true);
        return startupSucceeds;
    };
    ASSERT_TRUE(this->window_.Show(GetModuleHandleW(nullptr), std::move(callbacks)));
    this->applySucceeds_ = false;
    this->SelectPage(3);
    this->Click("settings.maintenance.restore_all");
    EXPECT_EQ(startupCount, 1);
    EXPECT_EQ(this->appliedCount_, 1);
    EXPECT_EQ(this->hotkeyActivateCount_, 1);
    EXPECT_NE(IsWindowEnabled(this->Control(L"Button", this->Text("settings.apply"))), FALSE);
    this->applySucceeds_ = true;
    startupSucceeds = true;
    this->Click("settings.apply");
    EXPECT_EQ(startupCount, 2);
    EXPECT_EQ(this->appliedCount_, 2);
    EXPECT_EQ(this->hotkeyActivateCount_, 1);
    EXPECT_EQ(IsWindowEnabled(this->Control(L"Button", this->Text("settings.apply"))), FALSE);
}
} // namespace
