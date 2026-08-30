#include <ui_text.h>

#include "ui_text_internal.h"
#include "json_file_test_access.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
class UiTextTest : public testing::Test
{
  protected:
    // 重置本地化业务与测试具名绑定，并创建独立资源目录。
    void SetUp() override
    {
        open_st::ShutdownUiText();
        (void)open_st::JsonFileTestAccess::ReleaseFile("localization.ui_text");
        const testing::TestInfo* testInfo = testing::UnitTest::GetInstance()->current_test_info();
        this->root_ = std::filesystem::temp_directory_path() /
                      ("open_st_ui_text_" + std::to_string(GetCurrentProcessId()) + "_" + testInfo->name());
        std::error_code error;
        std::filesystem::remove_all(this->root_, error);
        ASSERT_FALSE(error);
        std::filesystem::create_directories(this->root_ / "resources", error);
        ASSERT_FALSE(error);
    }

    // 释放句柄和测试缓存绑定，再清理测试资源。
    void TearDown() override
    {
        open_st::ShutdownUiText();
        EXPECT_TRUE(open_st::JsonFileTestAccess::ReleaseFile("localization.ui_text"));
        std::error_code error;
        std::filesystem::remove_all(this->root_, error);
        EXPECT_FALSE(error);
    }

    // 将动态文本对象包装为合法本地化资源。
    void WriteTexts(const nlohmann::json& texts)
    {
        const nlohmann::json document{{"schemaVersion", 1}, {"texts", texts}};
        UiTextTest::WriteRaw(this->root_ / "resources" / "ui_text.json", document.dump(2));
    }

    // 模拟外部程序直接写入资源，用于验证更新与损坏边界。
    static void WriteRaw(const std::filesystem::path& path, std::string_view contents)
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(output.is_open());
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        ASSERT_TRUE(output.good());
    }

    std::filesystem::path root_;
};

// 验证初始化只建立懒加载句柄，动态文本 key 在第一次查询时才从 JSON 读取。
TEST_F(UiTextTest, loads_dynamic_key_on_first_lookup)
{
    this->WriteTexts({{"runtime.added_key", {{"en-US", "Loaded dynamically"}}}});

    ASSERT_TRUE(open_st::InitializeUiText(this->root_));
    EXPECT_EQ(open_st::GetUiText("runtime.added_key"), L"Loaded dynamically");
}

// 验证取得懒句柄时不要求文件存在，但显式可用性读取会报告缺失资源。
TEST_F(UiTextTest, availability_check_reports_missing_resource)
{
    ASSERT_TRUE(open_st::InitializeUiText(this->root_));
    EXPECT_FALSE(open_st::IsUiTextAvailable());
}

// 验证本地化不再自行读取 settings.json，初始化后的默认语言固定为英文。
TEST_F(UiTextTest, defaults_to_english_independently_of_settings_file)
{
    this->WriteTexts({{"sample", {{"en-US", "English"}, {"ja-JP", "日本語"}}}});
    std::filesystem::create_directories(this->root_ / "data");
    UiTextTest::WriteRaw(this->root_ / "data" / "settings.json",
                         R"({"schemaVersion":1,"settings":{"ui.language":"ja-JP"}})");

    ASSERT_TRUE(open_st::InitializeUiText(this->root_));
    EXPECT_EQ(open_st::CurrentUiLanguageCode(), "en-US");
    EXPECT_EQ(open_st::GetUiText("sample"), L"English");
}

// 验证 SetUiLanguage 接受从资源动态发现的新增语言代码。
TEST_F(UiTextTest, switches_to_dynamically_discovered_language)
{
    this->WriteTexts({{"sample", {{"en-US", "English"}, {"fr-FR", "Français"}}}});
    ASSERT_TRUE(open_st::InitializeUiText(this->root_));

    ASSERT_TRUE(open_st::SetUiLanguage("fr-FR"));
    EXPECT_EQ(open_st::CurrentUiLanguageCode(), "fr-FR");
    EXPECT_EQ(open_st::GetUiText("sample"), L"Français");
}

// 验证语言列表取所有文本属性名的并集、去重并稳定排序，不依赖固定三语清单。
TEST_F(UiTextTest, lists_languages_dynamically)
{
    this->WriteTexts({{"first", {{"zh-CN", "一"}, {"fr-FR", "Un"}}},
                      {"second", {{"en-US", "Two"}, {"ja-JP", "二"}, {"fr-FR", "Deux"}}}});
    ASSERT_TRUE(open_st::InitializeUiText(this->root_));

    const std::vector<std::string> languages = open_st::GetAvailableUiLanguages();
    const std::vector<std::string> expected{"en-US", "fr-FR", "ja-JP", "zh-CN"};
    EXPECT_EQ(languages, expected);
}

// 验证新增语言允许翻译不完整，当前 key 缺少时会回退该 key 的英文文本。
TEST_F(UiTextTest, missing_translation_falls_back_to_english)
{
    this->WriteTexts({{"translated", {{"en-US", "English"}, {"fr-FR", "Français"}}},
                      {"incomplete", {{"en-US", "English fallback"}}}});
    ASSERT_TRUE(open_st::InitializeUiText(this->root_));
    ASSERT_TRUE(open_st::SetUiLanguage("fr-FR"));

    EXPECT_EQ(open_st::GetUiText("incomplete"), L"English fallback");
}

// 验证当前语言和英文都缺少时返回固定问号，避免界面静默显示空字符串。
TEST_F(UiTextTest, missing_selected_and_english_values_returns_question_mark)
{
    this->WriteTexts({{"language.seed", {{"en-US", "English"}, {"fr-FR", "Français"}}},
                      {"missing", {{"ja-JP", "日本語"}}}});
    ASSERT_TRUE(open_st::InitializeUiText(this->root_));
    ASSERT_TRUE(open_st::SetUiLanguage("fr-FR"));

    EXPECT_EQ(open_st::GetUiText("missing"), L"?");
    EXPECT_EQ(open_st::GetUiText("unknown.key"), L"?");
}

// 验证命名占位符只在取得本地化文本后执行受控替换。
TEST_F(UiTextTest, replaces_named_placeholders)
{
    this->WriteTexts({{"about.body", {{"en-US", "Open-ST {version}"}}}});
    ASSERT_TRUE(open_st::InitializeUiText(this->root_));

    EXPECT_EQ(open_st::GetUiText("about.body", {{L"version", L"9.8.7"}}), L"Open-ST 9.8.7");
}

// 验证没有后台监听时，下一次 GetUiText 仍会按请求发现并读取外部更新。
TEST_F(UiTextTest, lookup_reloads_changed_file)
{
    this->WriteTexts({{"hot.sample", {{"en-US", "Before"}}}});
    ASSERT_TRUE(open_st::InitializeUiText(this->root_));
    ASSERT_EQ(open_st::GetUiText("hot.sample"), L"Before");

    this->WriteTexts({{"hot.sample", {{"en-US", "After hot reload with another size"}}}});
    EXPECT_EQ(open_st::GetUiText("hot.sample"), L"After hot reload with another size");
}

// 验证 Common 读取语法损坏文件失败后，本地化仍保留已生效业务文本。
TEST_F(UiTextTest, syntax_error_keeps_last_valid_text)
{
    this->WriteTexts({{"safe.sample", {{"en-US", "Last valid value"}}}});
    ASSERT_TRUE(open_st::InitializeUiText(this->root_));
    ASSERT_EQ(open_st::GetUiText("safe.sample"), L"Last valid value");

    UiTextTest::WriteRaw(this->root_ / "resources" / "ui_text.json", "{invalid json and different size");
    EXPECT_EQ(open_st::GetUiText("safe.sample"), L"Last valid value");
}

// 验证语法合法但外层协议损坏的更新不会替换本地化模块最后验证通过的快照。
TEST_F(UiTextTest, invalid_schema_keeps_last_valid_text)
{
    this->WriteTexts({{"safe.sample", {{"en-US", "Last valid value"}}}});
    ASSERT_TRUE(open_st::InitializeUiText(this->root_));
    ASSERT_EQ(open_st::GetUiText("safe.sample"), L"Last valid value");

    UiTextTest::WriteRaw(this->root_ / "resources" / "ui_text.json",
                         R"({"schemaVersion":2,"texts":{"safe.sample":{"en-US":"Wrong"}}})");
    EXPECT_EQ(open_st::GetUiText("safe.sample"), L"Last valid value");
}

// 验证当前选择的动态语言被资源删除后，下一次资源读取立即把运行时语言退回英文。
TEST_F(UiTextTest, removed_current_language_falls_back_runtime_to_english)
{
    this->WriteTexts({{"sample", {{"en-US", "English"}, {"fr-FR", "Français"}}}});
    ASSERT_TRUE(open_st::InitializeUiText(this->root_));
    ASSERT_TRUE(open_st::SetUiLanguage("fr-FR"));

    this->WriteTexts({{"sample", {{"en-US", "Updated English"}}}});
    EXPECT_EQ(open_st::GetUiText("sample"), L"Updated English");
    EXPECT_EQ(open_st::CurrentUiLanguageCode(), "en-US");
}

// 验证连续读取损坏文件保留已生效文本，文件修复后仍能接受新业务内容。
TEST_F(UiTextTest, resource_recovers_after_repeated_read_failure)
{
    this->WriteTexts({{"sample", {{"en-US", "Before failure"}}}});
    ASSERT_TRUE(open_st::InitializeUiText(this->root_));
    ASSERT_EQ(open_st::GetUiText("sample"), L"Before failure");
    UiTextTest::WriteRaw(this->root_ / "resources" / "ui_text.json", "{broken ui text");
    EXPECT_EQ(open_st::GetUiText("sample"), L"Before failure");
    EXPECT_EQ(open_st::GetUiText("sample"), L"Before failure");

    this->WriteTexts({{"sample", {{"en-US", "Recovered resource"}}}});
    EXPECT_EQ(open_st::GetUiText("sample"), L"Recovered resource");
}

// 验证业务关闭后不再持有已生效文本，重新初始化不能把底层旧缓存当作成功读取。
TEST_F(UiTextTest, reinitialization_does_not_accept_cached_failed_read)
{
    this->WriteTexts({{"sample", {{"en-US", "Previously accepted"}}}});
    ASSERT_TRUE(open_st::InitializeUiText(this->root_));
    ASSERT_EQ(open_st::GetUiText("sample"), L"Previously accepted");
    open_st::ShutdownUiText();
    UiTextTest::WriteRaw(this->root_ / "resources" / "ui_text.json", "{broken after shutdown");

    ASSERT_TRUE(open_st::InitializeUiText(this->root_));
    EXPECT_FALSE(open_st::IsUiTextAvailable());
    EXPECT_EQ(open_st::GetUiText("sample"), L"?");
}
// 验证读取故障只触发一次粗略告警，消费不会解除故障去重，恢复后允许再次告警。
TEST_F(UiTextTest, read_warning_is_deduplicated_and_rearmed_after_recovery)
{
    this->WriteTexts({{"sample", {{"en-US", "Accepted"}}}});
    ASSERT_TRUE(open_st::InitializeUiText(this->root_));
    ASSERT_EQ(open_st::GetUiText("sample"), L"Accepted");
    EXPECT_FALSE(open_st::ConsumeUiTextReadWarning());
    const std::filesystem::path resourcePath = this->root_ / "resources" / "ui_text.json";
    UiTextTest::WriteRaw(resourcePath, "{broken resource");
    EXPECT_EQ(open_st::GetUiText("sample"), L"Accepted");
    EXPECT_TRUE(open_st::ConsumeUiTextReadWarning());
    EXPECT_FALSE(open_st::ConsumeUiTextReadWarning());
    EXPECT_EQ(open_st::GetUiText("sample"), L"Accepted");
    EXPECT_FALSE(open_st::ConsumeUiTextReadWarning());

    this->WriteTexts({{"sample", {{"en-US", "Recovered"}}}});
    EXPECT_EQ(open_st::GetUiText("sample"), L"Recovered");
    UiTextTest::WriteRaw(resourcePath, "{broken resource after recovery");
    EXPECT_EQ(open_st::GetUiText("sample"), L"Recovered");
    EXPECT_TRUE(open_st::ConsumeUiTextReadWarning());
}

// 验证业务结构失败也去重告警，合法资源恢复后重新武装，关闭清空待告警且不误报。
TEST_F(UiTextTest, schema_warning_is_deduplicated_and_shutdown_clears_pending)
{
    this->WriteTexts({{"sample", {{"en-US", "Accepted"}}}});
    ASSERT_TRUE(open_st::InitializeUiText(this->root_));
    ASSERT_TRUE(open_st::IsUiTextAvailable());
    const std::filesystem::path resourcePath = this->root_ / "resources" / "ui_text.json";
    UiTextTest::WriteRaw(resourcePath, R"({"schemaVersion":2,"texts":{}})");
    EXPECT_EQ(open_st::GetUiText("sample"), L"Accepted");
    EXPECT_TRUE(open_st::ConsumeUiTextReadWarning());
    EXPECT_EQ(open_st::GetUiText("sample"), L"Accepted");
    EXPECT_FALSE(open_st::ConsumeUiTextReadWarning());

    this->WriteTexts({{"sample", {{"en-US", "Recovered schema"}}}});
    EXPECT_EQ(open_st::GetUiText("sample"), L"Recovered schema");
    UiTextTest::WriteRaw(resourcePath, R"({"schemaVersion":3,"texts":{}})");
    EXPECT_EQ(open_st::GetUiText("sample"), L"Recovered schema");
    EXPECT_TRUE(open_st::ConsumeUiTextReadWarning());
    this->WriteTexts({{"sample", {{"en-US", "Final recovery"}}}});
    EXPECT_EQ(open_st::GetUiText("sample"), L"Final recovery");
    UiTextTest::WriteRaw(resourcePath, "{failure pending at shutdown");
    EXPECT_EQ(open_st::GetUiText("sample"), L"Final recovery");
    open_st::ShutdownUiText();
    EXPECT_FALSE(open_st::ConsumeUiTextReadWarning());
    EXPECT_EQ(open_st::GetUiText("sample"), L"?");
    EXPECT_FALSE(open_st::ConsumeUiTextReadWarning());
}
} // namespace
