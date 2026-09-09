// 验证界面文本资源加载、动态语言查询、回退格式化与运行期读取失败处理。

#include <ui_text.h>

#include "json_file_test_access.h"
#include "ui_text_internal.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace
{
class UiTextTest : public testing::Test
{
  protected:
    // 重置本地化业务与测试具名绑定，并创建独立资源目录。
    // 入参：无显式入参。
    // 返回：无返回值。
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
    // 入参：无显式入参。
    // 返回：无返回值。
    void TearDown() override
    {
        open_st::ShutdownUiText();
        EXPECT_TRUE(open_st::JsonFileTestAccess::ReleaseFile("localization.ui_text"));
        std::error_code error;
        std::filesystem::remove_all(this->root_, error);
        EXPECT_FALSE(error);
    }

    // 将动态文本对象包装为合法本地化资源。
    // 入参：texts 为动态文本键及其语言译文对象；languages 为资源声明的可选语言代码数组。
    // 返回：无返回值。
    void WriteTexts(const nlohmann::json& texts, const nlohmann::json& languages = nlohmann::json::array({"en-US"}))
    {
        const nlohmann::json document{{"schemaVersion", 1}, {"languages", languages}, {"texts", texts}};
        UiTextTest::WriteRaw(this->root_ / "resources" / "ui_text.json", document.dump(2));
    }

    // 模拟外部程序直接写入资源，用于验证更新与损坏边界。
    // 入参：path 为测试文件路径；contents 为写入的原始字节文本。
    // 返回：无返回值。
    static void WriteRaw(const std::filesystem::path& path, std::string_view contents)
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(output.is_open());
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        ASSERT_TRUE(output.good());
    }

    std::filesystem::path root_;
};

// 枚举声明列表的协议错误，供首次读取与热更新共同验证。
// 入参：无显式入参。
// 返回：包含各类非法语言声明结构的 JSON 文档列表。
std::vector<nlohmann::json> InvalidLanguageDocuments()
{
    const nlohmann::json base{{"schemaVersion", 1}, {"texts", {{"sample", {{"en-US", "Rejected"}}}}}};
    std::vector<nlohmann::json> documents{base};
    const std::vector<nlohmann::json> invalidLists{nlohmann::json::array(),
                                                   nullptr,
                                                   "en-US",
                                                   nlohmann::json::object(),
                                                   nlohmann::json::array({"en-US", "en-US"}),
                                                   nlohmann::json::array({"en-US", 7}),
                                                   nlohmann::json::array({"en-US", ""}),
                                                   nlohmann::json::array({"en-US", "fr FR"}),
                                                   nlohmann::json::array({"fr-FR"})};
    for (const nlohmann::json& languages : invalidLists)
    {
        nlohmann::json document = base;
        document["languages"] = languages;
        documents.push_back(std::move(document));
    }
    return documents;
}

// 验证初始化只建立懒加载句柄，动态文本 key 在第一次查询时才从 JSON 读取。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(UiTextTest, loads_dynamic_key_on_first_lookup)
{
    this->WriteTexts({{"runtime.added_key", {{"en-US", "Loaded dynamically"}}}});

    ASSERT_TRUE(open_st::InitializeUiText(this->root_));
    EXPECT_EQ(open_st::GetUiText("runtime.added_key"), L"Loaded dynamically");
}

// 验证取得懒句柄时不要求文件存在，但显式可用性读取会报告缺失资源。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(UiTextTest, availability_check_reports_missing_resource)
{
    ASSERT_TRUE(open_st::InitializeUiText(this->root_));
    EXPECT_FALSE(open_st::IsUiTextAvailable());
}

// 验证本地化不再自行读取 settings.json，初始化后的默认语言固定为英文。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
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

// 验证 SetUiLanguage 接受资源明确声明的新增语言代码。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(UiTextTest, switches_to_declared_language)
{
    this->WriteTexts({{"sample", {{"en-US", "English"}, {"fr-FR", "Français"}}}}, {"en-US", "fr-FR"});
    ASSERT_TRUE(open_st::InitializeUiText(this->root_));

    ASSERT_TRUE(open_st::SetUiLanguage("fr-FR"));
    EXPECT_EQ(open_st::CurrentUiLanguageCode(), "fr-FR");
    EXPECT_EQ(open_st::GetUiText("sample"), L"Français");
}

// 验证下拉选项只取声明数组并保留顺序，文本内未声明的语言不能被选择。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(UiTextTest, lists_only_declared_languages_in_array_order)
{
    this->WriteTexts({{"first", {{"zh-CN", "一"}, {"fr-FR", "Un"}}},
                      {"second", {{"en-US", "Two"}, {"ja-JP", "二"}, {"fr-FR", "Deux"}}}},
                     {"zh-CN", "en-US", "fr-FR"});
    ASSERT_TRUE(open_st::InitializeUiText(this->root_));

    const std::vector<std::string> languages = open_st::GetAvailableUiLanguages();
    const std::vector<std::string> expected{"zh-CN", "en-US", "fr-FR"};
    EXPECT_EQ(languages, expected);
    EXPECT_FALSE(open_st::SetUiLanguage("ja-JP"));
}

// 验证仅声明语言而尚未添加任何译文时仍可选择，并使用英文文本。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(UiTextTest, declared_language_without_translation_uses_english)
{
    this->WriteTexts({{"sample", {{"en-US", "English fallback"}}}}, {"en-US", "fr-FR"});
    ASSERT_TRUE(open_st::InitializeUiText(this->root_));
    ASSERT_TRUE(open_st::SetUiLanguage("fr-FR"));
    EXPECT_EQ(open_st::CurrentUiLanguageCode(), "fr-FR");
    EXPECT_EQ(open_st::GetUiText("sample"), L"English fallback");
}

// 验证外部只扩展声明数组即可在下一次查询中提供新语言。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(UiTextTest, language_list_reloads_added_declaration)
{
    const nlohmann::json texts{{"sample", {{"en-US", "English"}, {"fr-FR", "Français"}}}};
    this->WriteTexts(texts);
    ASSERT_TRUE(open_st::InitializeUiText(this->root_));
    EXPECT_EQ(open_st::GetAvailableUiLanguages(), std::vector<std::string>({"en-US"}));
    EXPECT_FALSE(open_st::SetUiLanguage("fr-FR"));

    this->WriteTexts(texts, {"fr-FR", "en-US"});
    EXPECT_EQ(open_st::GetAvailableUiLanguages(), std::vector<std::string>({"fr-FR", "en-US"}));
    ASSERT_TRUE(open_st::SetUiLanguage("fr-FR"));
    EXPECT_EQ(open_st::GetUiText("sample"), L"Français");
}

// 验证首次读取缺失或非法声明时不可用，且不从文本属性恢复语言列表。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(UiTextTest, invalid_language_declaration_without_snapshot_is_unavailable)
{
    for (const nlohmann::json& document : InvalidLanguageDocuments())
    {
        SCOPED_TRACE(document.dump());
        open_st::ShutdownUiText();
        UiTextTest::WriteRaw(this->root_ / "resources" / "ui_text.json", document.dump(2));
        ASSERT_TRUE(open_st::InitializeUiText(this->root_));
        EXPECT_FALSE(open_st::IsUiTextAvailable());
        EXPECT_TRUE(open_st::GetAvailableUiLanguages().empty());
        EXPECT_EQ(open_st::GetUiText("sample"), L"?");
        EXPECT_FALSE(open_st::SetUiLanguage("fr-FR"));
    }
}

// 验证非法声明热更新保留最后有效语言列表、当前语言及文本快照。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(UiTextTest, invalid_language_declaration_keeps_last_valid_snapshot)
{
    ASSERT_TRUE(open_st::InitializeUiText(this->root_));
    for (const nlohmann::json& document : InvalidLanguageDocuments())
    {
        SCOPED_TRACE(document.dump());
        this->WriteTexts({{"sample", {{"en-US", "Accepted"}, {"fr-FR", "Français"}}}}, {"fr-FR", "en-US"});
        ASSERT_TRUE(open_st::SetUiLanguage("fr-FR"));
        ASSERT_EQ(open_st::GetUiText("sample"), L"Français");

        UiTextTest::WriteRaw(this->root_ / "resources" / "ui_text.json", document.dump(2));
        EXPECT_EQ(open_st::GetAvailableUiLanguages(), std::vector<std::string>({"fr-FR", "en-US"}));
        EXPECT_EQ(open_st::CurrentUiLanguageCode(), "fr-FR");
        EXPECT_EQ(open_st::GetUiText("sample"), L"Français");
    }
}

// 验证新增语言允许翻译不完整，当前 key 缺少时会回退该 key 的英文文本。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(UiTextTest, missing_translation_falls_back_to_english)
{
    this->WriteTexts({{"translated", {{"en-US", "English"}, {"fr-FR", "Français"}}},
                      {"incomplete", {{"en-US", "English fallback"}}}},
                     {"en-US", "fr-FR"});
    ASSERT_TRUE(open_st::InitializeUiText(this->root_));
    ASSERT_TRUE(open_st::SetUiLanguage("fr-FR"));

    EXPECT_EQ(open_st::GetUiText("incomplete"), L"English fallback");
}

// 验证当前语言和英文都缺少时返回固定问号，避免界面静默显示空字符串。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(UiTextTest, missing_selected_and_english_values_returns_question_mark)
{
    this->WriteTexts(
        {{"language.seed", {{"en-US", "English"}, {"fr-FR", "Français"}}}, {"missing", {{"ja-JP", "日本語"}}}},
        {"en-US", "fr-FR"});
    ASSERT_TRUE(open_st::InitializeUiText(this->root_));
    ASSERT_TRUE(open_st::SetUiLanguage("fr-FR"));

    EXPECT_EQ(open_st::GetUiText("missing"), L"?");
    EXPECT_EQ(open_st::GetUiText("unknown.key"), L"?");
}

// 验证命名占位符只在取得本地化文本后执行受控替换。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(UiTextTest, replaces_named_placeholders)
{
    this->WriteTexts({{"about.body", {{"en-US", "Open-ST {version}"}}}});
    ASSERT_TRUE(open_st::InitializeUiText(this->root_));

    EXPECT_EQ(open_st::GetUiText("about.body", {{L"version", L"9.8.7"}}), L"Open-ST 9.8.7");
}

// 验证没有后台监听时，下一次 GetUiText 仍会按请求发现并读取外部更新。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(UiTextTest, lookup_reloads_changed_file)
{
    this->WriteTexts({{"hot.sample", {{"en-US", "Before"}}}});
    ASSERT_TRUE(open_st::InitializeUiText(this->root_));
    ASSERT_EQ(open_st::GetUiText("hot.sample"), L"Before");

    this->WriteTexts({{"hot.sample", {{"en-US", "After hot reload with another size"}}}});
    EXPECT_EQ(open_st::GetUiText("hot.sample"), L"After hot reload with another size");
}

// 验证 Common 读取语法损坏文件失败后，本地化仍保留已生效业务文本。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(UiTextTest, syntax_error_keeps_last_valid_text)
{
    this->WriteTexts({{"safe.sample", {{"en-US", "Last valid value"}}}});
    ASSERT_TRUE(open_st::InitializeUiText(this->root_));
    ASSERT_EQ(open_st::GetUiText("safe.sample"), L"Last valid value");

    UiTextTest::WriteRaw(this->root_ / "resources" / "ui_text.json", "{invalid json and different size");
    EXPECT_EQ(open_st::GetUiText("safe.sample"), L"Last valid value");
}

// 验证语法合法但外层协议损坏的更新不会替换本地化模块最后验证通过的快照。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(UiTextTest, invalid_schema_keeps_last_valid_text)
{
    this->WriteTexts({{"safe.sample", {{"en-US", "Last valid value"}}}});
    ASSERT_TRUE(open_st::InitializeUiText(this->root_));
    ASSERT_EQ(open_st::GetUiText("safe.sample"), L"Last valid value");

    UiTextTest::WriteRaw(this->root_ / "resources" / "ui_text.json",
                         R"({"schemaVersion":2,"languages":["en-US"],"texts":{"safe.sample":{"en-US":"Wrong"}}})");
    EXPECT_EQ(open_st::GetUiText("safe.sample"), L"Last valid value");
}

// 验证当前选择的动态语言被资源删除后，下一次资源读取立即把运行时语言退回英文。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(UiTextTest, removed_current_language_falls_back_runtime_to_english)
{
    this->WriteTexts({{"sample", {{"en-US", "English"}, {"fr-FR", "Français"}}}}, {"en-US", "fr-FR"});
    ASSERT_TRUE(open_st::InitializeUiText(this->root_));
    ASSERT_TRUE(open_st::SetUiLanguage("fr-FR"));

    this->WriteTexts({{"sample", {{"en-US", "Updated English"}, {"fr-FR", "Français"}}}});
    EXPECT_EQ(open_st::GetUiText("sample"), L"Updated English");
    EXPECT_EQ(open_st::CurrentUiLanguageCode(), "en-US");
    EXPECT_EQ(open_st::GetAvailableUiLanguages(), std::vector<std::string>({"en-US"}));
    EXPECT_FALSE(open_st::SetUiLanguage("fr-FR"));
}

// 验证连续读取损坏文件保留已生效文本，文件修复后仍能接受新业务内容。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
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
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
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
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
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
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
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
