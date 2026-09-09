// 提供隔离设置和模拟自启回调的欢迎窗口人工验收用例。

#include "json_file_test_access.h"
#include "settings_internal.h"
#include <array>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <map>
#include <settings.h>
#include <string>
#include <utility>
#include <welcome_window.h>

namespace
{
class WelcomeManualTest : public testing::Test
{
  protected:
    // 人工验收必须显式开启，普通回归不创建窗口或等待输入。
    // 入参：无显式入参。
    // 返回：无返回值。
    void SetUp() override
    {
        std::array<wchar_t, 8> enabled{};
        if (GetEnvironmentVariableW(L"OPEN_ST_INTERACTIVE_UI_TESTS", enabled.data(),
                                    static_cast<DWORD>(enabled.size())) != 1 ||
            enabled[0] != L'1')
            GTEST_SKIP() << "Set OPEN_ST_INTERACTIVE_UI_TESTS=1 to inspect and close the welcome window manually.";
        open_st::ShutdownSettings();
        ASSERT_TRUE(open_st::JsonFileTestAccess::ReleaseFile("settings.user"));
        ASSERT_TRUE(open_st::JsonFileTestAccess::ReleaseFile("settings.default"));
        this->root_ = std::filesystem::temp_directory_path() /
                      ("open_st_welcome_manual_" + std::to_string(GetCurrentProcessId()) + "_" +
                       std::to_string(GetTickCount64()));
        std::filesystem::create_directories(this->root_ / "resources");
        {
            std::ofstream file(this->root_ / "resources" / "default_settings.json", std::ios::binary);
            file
                << R"({"schemaVersion":1,"settings":{"ui.language":"zh-CN","startup.enabled":true,"onboarding.completed":false}})";
            ASSERT_TRUE(file.good());
        }
        ASSERT_TRUE(open_st::InitializeSettings(this->root_));
    }

    // 只清理本用例创建的隔离设置目录，退出不触及真实启动入口。
    // 入参：无显式入参。
    // 返回：无返回值。
    void TearDown() override
    {
        if (this->root_.empty())
            return;
        open_st::ShutdownSettings();
        EXPECT_TRUE(open_st::JsonFileTestAccess::ReleaseFile("settings.user"));
        EXPECT_TRUE(open_st::JsonFileTestAccess::ReleaseFile("settings.default"));
        std::error_code error;
        std::filesystem::remove_all(this->root_, error);
        EXPECT_FALSE(error);
    }

    std::filesystem::path root_;
};

// 验证无定时器和自动关闭，由用户检查复选框后主动确认、退出或关闭窗口。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(WelcomeManualTest, waits_for_user_close)
{
    const std::map<std::string, std::wstring, std::less<>> texts = {
        {"welcome.title", L"Open-ST 欢迎窗口人工验收"},
        {"welcome.intro",
         L"按 Ctrl+Alt+Q "
         L"开始截图。确认后进入托盘运行。\n本测试使用临时设置和模拟启动入口，确认不会修改真实开机启动项。"},
        {"welcome.privacy", L"当前截图在本机处理。后续 OCR "
                            L"在本机执行；文字翻译上传文字，图片翻译上传处理后的图片。这些翻译能力尚未提供。\n请检查布"
                            L"局和复选框，然后点击确认、退出或右上角关闭。"},
        {"settings.startup.label", L"登录后启动 Open-ST"},
        {"welcome.confirm", L"确认"},
        {"welcome.exit", L"退出"}};
    open_st::SettingsWindowCallbacks callbacks;
    // 从人工验收专用文本表查询文案，未知键直接显示键名。
    // 入参：key 为待查询的测试界面文本键。
    // 返回：人工验收文案；未知键返回由键名构造的宽字符串。
    callbacks.text = [&texts](std::string_view key)
    {
        const auto found = texts.find(key);
        return found == texts.end() ? std::wstring(key.begin(), key.end()) : found->second;
    };
    // 模拟自启设置成功，人工验收不修改真实启动项。
    // 入参：未命名 bool 为期望的自启启用状态；本模拟回调不按该值区分处理。
    // 返回：固定为 true，表示模拟自启设置成功。
    callbacks.startupApplied = [](bool) { return true; };
    open_st::WelcomeWindow window;
    (void)window.ShowModal(GetModuleHandleW(nullptr), std::move(callbacks));
    EXPECT_FALSE(window.Failed());
}
} // namespace
