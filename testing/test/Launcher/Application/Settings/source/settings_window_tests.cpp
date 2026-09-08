#include "json_file_test_access.h"
#include "settings_internal.h"
#include "settings_window_test_access.h"
#include <array>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <settings.h>
#include <settings_window.h>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
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

class SettingsWindowTest : public testing::Test
{
  protected:
    // 隔离文件绑定和磁盘目录，复制正式布局但不读取真实用户设置。
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
        this->Write("resources/default_settings.json", R"({"schemaVersion":1,"settings":{"ui.language":"en-US"}})");
        this->Write("data/settings.json", R"({"schemaVersion":1,"settings":{"ui.language":"en-US"}})");
        ASSERT_TRUE(open_st::InitializeSettings(this->root_));
        open_st::SettingsWindowTestAccess::SetConfirmation(this->window_,
                                                           [this]()
                                                           {
                                                               ++this->confirmationCount_;
                                                               return this->confirm_;
                                                           });
    }

    // 在捕获对象销毁前关闭窗口，恢复属性并释放测试卡名。
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
    void Write(const std::filesystem::path& relative, std::string_view content)
    {
        std::ofstream output(this->root_ / relative, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(output.is_open());
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
        ASSERT_TRUE(output.good());
    }

    // 提供窄回调并记录真实持久化顺序，可模拟查询失败与生效失败。
    open_st::SettingsWindowCallbacks Callbacks()
    {
        open_st::SettingsWindowCallbacks callbacks;
        callbacks.text = [this](std::string_view key) { return this->Text(key); };
        callbacks.currentLanguage = [this]() { return this->appliedLanguage_; };
        callbacks.availableLanguages = [this]()
        {
            if (this->queryThrows_)
            {
                throw std::runtime_error("test query failure");
            }
            return this->availableLanguages_;
        };
        callbacks.languageApplied = [this](std::string_view language)
        {
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
        return callbacks;
    }

    // 标题带运行语言，以观察成功后是否重新取文本。
    std::wstring Text(std::string_view key) const
    {
        const std::string value =
            key == "settings.title" ? std::string(key) + ":" + this->appliedLanguage_ : std::string(key);
        return std::wstring(value.begin(), value.end());
    }

    // 创建真实窗口并立即隐藏，只通过本线程消息进行自动交互。
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
    HWND Control(std::wstring className, std::wstring text = {}) const
    {
        ControlSearch search{std::move(className), std::move(text), nullptr};
        EnumChildWindows(open_st::SettingsWindowTestAccess::NativeHandle(this->window_), FindControl,
                         reinterpret_cast<LPARAM>(&search));
        return search.found;
    }

    // 限量派发延迟关闭，避免自动测试无限等待消息。
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
    void Click(std::string_view key)
    {
        const HWND button = this->Control(L"Button", this->Text(key));
        ASSERT_NE(button, nullptr);
        ASSERT_NE(IsWindowEnabled(button), FALSE);
        (void)SendMessageW(button, BM_CLICK, 0, 0);
        this->Pump();
    }

    // 展开通知刷新选项，不实际弹出下拉窗口。
    void RefreshOptions()
    {
        const HWND combo = this->Control(L"ComboBox");
        ASSERT_NE(combo, nullptr);
        (void)SendMessageW(GetParent(combo), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(combo), CBN_DROPDOWN),
                           reinterpret_cast<LPARAM>(combo));
        this->Pump();
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
    open_st::SettingsWindow window_;
};

// 选择只改草稿，取消不写入也不切换运行语言。
TEST_F(SettingsWindowTest, cancel_does_not_save_or_notify)
{
    ASSERT_NE(this->Open(), nullptr);
    this->Select(L"zh-CN");
    this->Click("settings.cancel");
    EXPECT_FALSE(this->window_.IsOpen());
    EXPECT_EQ(open_st::GetStringSetting("ui.language"), "en-US");
    EXPECT_EQ(this->appliedCount_, 0);
}

// 应用先保存后通知并保持窗口，无新修改的确定只关闭。
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

// 确定带草稿时使用统一提交入口，生效成功后关闭。
TEST_F(SettingsWindowTest, accept_saves_before_notifying_and_closes)
{
    ASSERT_NE(this->Open(), nullptr);
    this->Select(L"ja-JP");
    this->Click("settings.ok");
    EXPECT_FALSE(this->window_.IsOpen());
    EXPECT_EQ(this->savedAtNotification_, "ja-JP");
    EXPECT_EQ(this->appliedCount_, 1);
}

// 选项消失不能替用户改选，重新出现后应仍能提交原草稿。
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

// 写入失败保留窗口，不能提前调用生效回调。
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

// 生效失败后文件改只读仍可重试成功，证明没有再次写盘。
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

// 重试前外部修改必须阻止旧目标生效。
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

// 冲突重载需确认，拒绝保留草稿，接受读取最新基线。
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

// 默认恢复仅改草稿，取消不能覆盖已存用户值。
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

// 损坏读取允许打开可恢复窗口，修复后重载不必重建窗口。
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

// 重复打开保留草稿，应用成功重新取文本而不重建界面。
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

// 查询异常转成可见错误，不越过窗口边界且不能保存。
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

// 页面改名后恢复默认仍按实际字段所属页执行，不硬编码 general。
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

// 默认语言不可用时恢复失败保持现有草稿，不能显示并接受无效默认值。
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

// 生效回调重入 Close 必须延迟销毁，返回后正常完成且不访问释放内存。
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
} // namespace
