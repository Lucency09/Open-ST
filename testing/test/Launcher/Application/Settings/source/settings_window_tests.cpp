#include <settings.h>
#include <settings_window.h>

#include "json_file_test_access.h"
#include "settings_internal.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <windows.h>

namespace
{
constexpr int LANGUAGE_COMBO_ID = 1001;

class SettingsWindowTest : public testing::Test
{
  protected:
    // 用独立测试目录初始化设置服务，窗口只查询测试提供的语言与文本。
    void SetUp() override
    {
        open_st::ShutdownSettings();
        ASSERT_TRUE(open_st::JsonFileTestAccess::ReleaseFile("settings.user"));
        ASSERT_TRUE(open_st::JsonFileTestAccess::ReleaseFile("settings.default"));
        const testing::TestInfo* info = testing::UnitTest::GetInstance()->current_test_info();
        this->root_ = std::filesystem::temp_directory_path() /
            ("open_st_settings_window_" + std::to_string(GetCurrentProcessId()) + "_" + info->name());
        std::filesystem::create_directories(this->root_ / "resources");
        {
            std::ofstream stream(this->root_ / "resources" / "default_settings.json", std::ios::binary);
            ASSERT_TRUE(stream.is_open());
            stream << R"({"schemaVersion":1,"settings":{"ui.language":"en-US"}})";
            ASSERT_TRUE(stream.good());
        }
        ASSERT_TRUE(open_st::InitializeSettings(this->root_));
    }

    // 在回调捕获的测试对象销毁前关闭窗口，再清理句柄绑定及隔离目录。
    void TearDown() override
    {
        this->window_.Close();
        open_st::ShutdownSettings();
        EXPECT_TRUE(open_st::JsonFileTestAccess::ReleaseFile("settings.user"));
        EXPECT_TRUE(open_st::JsonFileTestAccess::ReleaseFile("settings.default"));
        std::error_code error;
        std::filesystem::remove_all(this->root_, error);
        EXPECT_FALSE(error);
    }

    // 提供无需本地化模块的窄回调，记录保存通知触发时的持久化值。
    open_st::SettingsWindowCallbacks Callbacks()
    {
        open_st::SettingsWindowCallbacks callbacks;
        // 测试文本采用键本身，避免依赖任何真实语言资源。
        callbacks.text = [](std::string_view key)
        {
            return std::wstring(key.begin(), key.end());
        };
        // 返回模拟的当前运行语言，取消时此值必须保持不变。
        callbacks.currentLanguage = [this]()
        {
            return this->appliedLanguage_;
        };
        // 动态列表由测试直接调整，用于验证每次下拉都会重新查询。
        callbacks.availableLanguages = [this]()
        {
            return this->availableLanguages_;
        };
        // 只记录已成功写入的通知，不自行模拟或跳过真正的设置存储。
        callbacks.languageApplied = [this](std::string_view language)
        {
            ++this->appliedCount_;
            this->savedAtNotification_ = open_st::GetStringSetting("ui.language").value_or("");
            this->appliedLanguage_ = language;
        };
        return callbacks;
    }

    // 创建真实控件后立即隐藏，测试仅通过 Win32 消息模拟本窗口输入。
    HWND Open()
    {
        if (!this->window_.Show(GetModuleHandleW(nullptr), this->Callbacks()))
        {
            return nullptr;
        }
        const HWND window = FindWindowW(L"OpenST.SettingsWindow", nullptr);
        DWORD processId{};
        if (window == nullptr || GetWindowThreadProcessId(window, &processId) == 0 ||
            processId != GetCurrentProcessId())
        {
            return nullptr;
        }
        ShowWindow(window, SW_HIDE);
        return window;
    }

    std::filesystem::path root_;
    std::vector<std::string> availableLanguages_{"en-US", "zh-CN"};
    std::string appliedLanguage_{"en-US"};
    std::string savedAtNotification_;
    int appliedCount_{};
    open_st::SettingsWindow window_;
};

// 验证选择新语言后取消不会写入设置，也不触发运行语言切换回调。
TEST_F(SettingsWindowTest, cancel_does_not_save_or_notify)
{
    const HWND window = this->Open();
    ASSERT_NE(window, nullptr);
    const HWND combo = GetDlgItem(window, LANGUAGE_COMBO_ID);
    ASSERT_NE(combo, nullptr);
    (void)SendMessageW(combo, CB_SETCURSEL, 1, 0);
    (void)SendMessageW(window, WM_COMMAND, IDCANCEL, 0);
    EXPECT_FALSE(this->window_.IsOpen());
    EXPECT_EQ(open_st::GetStringSetting("ui.language").value_or(""), "en-US");
    EXPECT_EQ(this->appliedCount_, 0);
    EXPECT_EQ(this->appliedLanguage_, "en-US");
}

// 验证确认先持久化所选语言，再通知上级刷新，最后关闭窗口。
TEST_F(SettingsWindowTest, confirm_saves_before_notifying)
{
    const HWND window = this->Open();
    ASSERT_NE(window, nullptr);
    const HWND combo = GetDlgItem(window, LANGUAGE_COMBO_ID);
    ASSERT_NE(combo, nullptr);
    (void)SendMessageW(combo, CB_SETCURSEL, 1, 0);
    (void)SendMessageW(window, WM_COMMAND, IDOK, 0);
    EXPECT_FALSE(this->window_.IsOpen());
    EXPECT_EQ(this->appliedCount_, 1);
    EXPECT_EQ(this->savedAtNotification_, "zh-CN");
    EXPECT_EQ(this->appliedLanguage_, "zh-CN");
    EXPECT_EQ(open_st::GetStringSetting("ui.language").value_or(""), "zh-CN");
}

// 验证展开语言列表重新查询可用语言，且不提前写入或切换运行语言。
TEST_F(SettingsWindowTest, dropdown_refreshes_dynamic_languages)
{
    const HWND window = this->Open();
    ASSERT_NE(window, nullptr);
    const HWND combo = GetDlgItem(window, LANGUAGE_COMBO_ID);
    ASSERT_EQ(SendMessageW(combo, CB_GETCOUNT, 0, 0), 2);
    this->availableLanguages_.push_back("ja-JP");
    (void)SendMessageW(window, WM_COMMAND, MAKEWPARAM(LANGUAGE_COMBO_ID, CBN_DROPDOWN),
                       reinterpret_cast<LPARAM>(combo));
    EXPECT_EQ(SendMessageW(combo, CB_GETCOUNT, 0, 0), 3);
    EXPECT_EQ(this->appliedCount_, 0);
    EXPECT_EQ(open_st::GetStringSetting("ui.language").value_or(""), "en-US");
}

// 验证创建期间的查询回调异常被 Win32 边界捕获，窗口创建失败但异常不泄漏。
TEST_F(SettingsWindowTest, callback_exception_cancels_window_creation)
{
    open_st::SettingsWindowCallbacks callbacks = this->Callbacks();
    // 在 WM_CREATE 内模拟上级语言查询失败，检查异常不会跨越系统回调。
    callbacks.availableLanguages = []() -> std::vector<std::string>
    {
        throw std::runtime_error("test callback failure");
    };
    EXPECT_FALSE(this->window_.Show(GetModuleHandleW(nullptr), std::move(callbacks)));
    EXPECT_FALSE(this->window_.IsOpen());
    EXPECT_EQ(this->appliedCount_, 0);
}
} // namespace
