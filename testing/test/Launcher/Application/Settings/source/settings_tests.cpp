// 验证动态设置的类型访问、默认值合并、持久化与外部文件故障处理。

#include <file_lease.h>
#include <settings.h>

#include "json_file_test_access.h"
#include "settings_internal.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <windows.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <string>

namespace
{
class SettingsTest : public testing::Test
{
  protected:
    // 隔离具名文件绑定和测试目录，避免单例缓存污染不同测试。
    // 入参：无显式入参。
    // 返回：无返回值。
    void SetUp() override
    {
        open_st::ShutdownSettings();
        (void)open_st::JsonFileTestAccess::ReleaseFile("settings.user");
        (void)open_st::JsonFileTestAccess::ReleaseFile("settings.default");
        const testing::TestInfo* testInfo = testing::UnitTest::GetInstance()->current_test_info();
        this->root_ = std::filesystem::temp_directory_path() /
                      ("open_st_settings_" + std::to_string(GetCurrentProcessId()) + "_" + testInfo->name());
        std::error_code error;
        std::filesystem::remove_all(this->root_, error);
        ASSERT_FALSE(error);
        std::filesystem::create_directories(this->root_ / "resources", error);
        ASSERT_FALSE(error);
    }

    // 先关闭业务并释放测试绑定，再恢复文件属性和删除测试数据。
    // 入参：无显式入参。
    // 返回：无返回值。
    void TearDown() override
    {
        open_st::ShutdownSettings();
        EXPECT_TRUE(open_st::JsonFileTestAccess::ReleaseFile("settings.user"));
        EXPECT_TRUE(open_st::JsonFileTestAccess::ReleaseFile("settings.default"));
        const std::filesystem::path userPath = this->root_ / "data" / "settings.json";
        if (std::filesystem::exists(userPath))
        {
            (void)SetFileAttributesW(userPath.c_str(), FILE_ATTRIBUTE_NORMAL);
        }
        std::error_code error;
        std::filesystem::remove_all(this->root_, error);
        EXPECT_FALSE(error);
    }

    // 直接构造磁盘内容，以模拟外部程序更新和损坏文件。
    // 入参：path 为测试文件路径；contents 为写入的原始字节文本。
    // 返回：无返回值。
    static void WriteRaw(const std::filesystem::path& path, std::string_view contents)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(output.is_open());
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        ASSERT_TRUE(output.good());
    }

    // 独立读取真实文件，确认业务写入或损坏保护的磁盘结果。
    // 入参：path 为测试文件路径。
    // 返回：文件的原始字节字符串，供磁盘内容断言使用。
    static std::string ReadRaw(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }

    // 写入指定默认语言的合法设置资源。
    // 入参：language 为写入测试默认资源的语言代码，默认为 en-US。
    // 返回：无返回值。
    void WriteDefault(std::string_view language = "en-US")
    {
        const nlohmann::json document{{"schemaVersion", 1}, {"settings", {{"ui.language", language}}}};
        SettingsTest::WriteRaw(this->root_ / "resources" / "default_settings.json", document.dump(2));
    }

    std::filesystem::path root_;
};

// 验证启动时仅在用户配置缺失时从默认资源自动生成 settings.json。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(SettingsTest, initialization_creates_missing_user_file)
{
    this->WriteDefault("ja-JP");

    ASSERT_TRUE(open_st::InitializeSettings(this->root_));
    EXPECT_TRUE(open_st::IsSettingsPersistenceAvailable());
    EXPECT_TRUE(std::filesystem::exists(this->root_ / "data" / "settings.json"));
    const std::optional<std::string> language = open_st::GetStringSetting("ui.language");
    ASSERT_TRUE(language.has_value());
    EXPECT_EQ(*language, "ja-JP");
}

// 验证损坏的现有用户配置不会被默认配置覆盖，读取时仍可使用默认值。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(SettingsTest, invalid_user_file_is_preserved_and_defaults_are_read)
{
    this->WriteDefault();
    const std::filesystem::path userPath = this->root_ / "data" / "settings.json";
    SettingsTest::WriteRaw(userPath, "{broken settings");

    ASSERT_TRUE(open_st::InitializeSettings(this->root_));
    EXPECT_FALSE(open_st::IsSettingsPersistenceAvailable());
    const std::optional<std::string> language = open_st::GetStringSetting("ui.language");
    ASSERT_TRUE(language.has_value());
    EXPECT_EQ(*language, "en-US");
    EXPECT_EQ(SettingsTest::ReadRaw(userPath), "{broken settings");
    EXPECT_FALSE(open_st::SetStringSetting("ui.language", "zh-CN"));
    EXPECT_EQ(SettingsTest::ReadRaw(userPath), "{broken settings");
}

// 验证已有但只读的用户配置仍可读取，同时启动状态会准确报告不可持久化且拒绝写入。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(SettingsTest, read_only_user_file_is_reported_as_not_persistable)
{
    this->WriteDefault();
    const std::filesystem::path userPath = this->root_ / "data" / "settings.json";
    const nlohmann::json user{{"schemaVersion", 1}, {"settings", {{"ui.language", "zh-CN"}}}};
    SettingsTest::WriteRaw(userPath, user.dump(2));
    ASSERT_NE(SetFileAttributesW(userPath.c_str(), FILE_ATTRIBUTE_READONLY), FALSE);

    ASSERT_TRUE(open_st::InitializeSettings(this->root_));
    EXPECT_FALSE(open_st::IsSettingsPersistenceAvailable());
    EXPECT_EQ(open_st::GetStringSetting("ui.language"), std::optional<std::string>("zh-CN"));
    EXPECT_FALSE(open_st::SetStringSetting("ui.language", "ja-JP"));
}

// 验证每次设置读取都会通过 Common 检查磁盘，并取得外部修改后的最新值。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(SettingsTest, getter_observes_external_file_changes)
{
    this->WriteDefault();
    ASSERT_TRUE(open_st::InitializeSettings(this->root_));
    ASSERT_EQ(open_st::GetStringSetting("ui.language"), std::optional<std::string>("en-US"));

    const nlohmann::json changed{{"schemaVersion", 1}, {"settings", {{"ui.language", "zh-CN"}}}};
    SettingsTest::WriteRaw(this->root_ / "data" / "settings.json", changed.dump(2));
    EXPECT_EQ(open_st::GetStringSetting("ui.language"), std::optional<std::string>("zh-CN"));
}

// 验证动态 key 的类型化读写不会删除用户文档中的未知字段。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(SettingsTest, typed_setters_preserve_unknown_keys)
{
    this->WriteDefault();
    const nlohmann::json user{{"schemaVersion", 1}, {"settings", {{"ui.language", "en-US"}, {"unknown", 42}}}};
    SettingsTest::WriteRaw(this->root_ / "data" / "settings.json", user.dump(2));
    ASSERT_TRUE(open_st::InitializeSettings(this->root_));

    ASSERT_TRUE(open_st::SetStringSetting("ui.language", "ja-JP"));
    ASSERT_TRUE(open_st::SetBoolSetting("capture.cursor", true));
    ASSERT_TRUE(open_st::SetIntegerSetting("capture.delay", 125));
    EXPECT_EQ(open_st::GetStringSetting("ui.language"), std::optional<std::string>("ja-JP"));
    EXPECT_EQ(open_st::GetBoolSetting("capture.cursor"), std::optional<bool>(true));
    EXPECT_EQ(open_st::GetIntegerSetting("capture.delay"), std::optional<std::int64_t>(125));

    const nlohmann::json saved = nlohmann::json::parse(SettingsTest::ReadRaw(this->root_ / "data" / "settings.json"));
    EXPECT_EQ(saved.at("settings").at("unknown").get<int>(), 42);
}

// 验证无符号整数超出 int64_t 范围时回退默认值，而合法上下界仍可读取。
// 入参：无运行入参。
// 返回：无返回值；通过 GoogleTest 断言记录范围和回退结果。
TEST_F(SettingsTest, integer_overflow_falls_back_without_wrapping)
{
    const std::int64_t maximum = std::numeric_limits<std::int64_t>::max();
    const std::int64_t minimum = std::numeric_limits<std::int64_t>::min();
    const nlohmann::json defaults{{"schemaVersion", 1}, {"settings", {{"overflow", 17}}}};
    const nlohmann::json user{{"schemaVersion", 1},
                              {"settings",
                               {{"overflow", static_cast<std::uint64_t>(maximum) + 1},
                                {"no.default", std::numeric_limits<std::uint64_t>::max()},
                                {"maximum", maximum},
                                {"minimum", minimum},
                                {"floating", 1.0}}}};
    SettingsTest::WriteRaw(this->root_ / "resources" / "default_settings.json", defaults.dump());
    SettingsTest::WriteRaw(this->root_ / "data" / "settings.json", user.dump());

    ASSERT_TRUE(open_st::InitializeSettings(this->root_));
    EXPECT_EQ(open_st::GetIntegerSetting("overflow"), 17);
    EXPECT_FALSE(open_st::GetIntegerSetting("no.default").has_value());
    EXPECT_EQ(open_st::GetIntegerSetting("maximum"), maximum);
    EXPECT_EQ(open_st::GetIntegerSetting("minimum"), minimum);
    EXPECT_FALSE(open_st::GetIntegerSetting("floating").has_value());
}

// 验证用户值缺失或类型错误时，类型化 getter 会读取默认配置中的同名动态 key。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(SettingsTest, getters_fall_back_to_default_values)
{
    const nlohmann::json defaults{{"schemaVersion", 1},
                                  {"settings", {{"name", "default"}, {"enabled", true}, {"count", 7}}}};
    SettingsTest::WriteRaw(this->root_ / "resources" / "default_settings.json", defaults.dump(2));
    const nlohmann::json user{{"schemaVersion", 1}, {"settings", {{"name", false}, {"enabled", "wrong"}}}};
    SettingsTest::WriteRaw(this->root_ / "data" / "settings.json", user.dump(2));

    ASSERT_TRUE(open_st::InitializeSettings(this->root_));
    EXPECT_EQ(open_st::GetStringSetting("name"), std::optional<std::string>("default"));
    EXPECT_EQ(open_st::GetBoolSetting("enabled"), std::optional<bool>(true));
    EXPECT_EQ(open_st::GetIntegerSetting("count"), std::optional<std::int64_t>(7));
    EXPECT_FALSE(open_st::GetStringSetting("missing").has_value());
}

// 验证启动检查已有配置的写入条件时不会重新序列化或覆盖原始排版。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(SettingsTest, initialization_preserves_existing_file_bytes)
{
    this->WriteDefault();
    const std::filesystem::path userPath = this->root_ / "data" / "settings.json";
    const std::string original = "{ \"schemaVersion\" : 1, \"settings\" : {\"ui.language\":\"ja-JP\"} }\n";
    SettingsTest::WriteRaw(userPath, original);

    ASSERT_TRUE(open_st::InitializeSettings(this->root_));
    EXPECT_TRUE(open_st::IsSettingsPersistenceAvailable());
    EXPECT_EQ(SettingsTest::ReadRaw(userPath), original);
}

// 验证外层协议错误不会被默认配置修复或被设置修改覆盖。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(SettingsTest, invalid_business_schema_is_preserved)
{
    this->WriteDefault();
    const std::filesystem::path userPath = this->root_ / "data" / "settings.json";
    const std::string original = R"({"schemaVersion":2,"settings":{"ui.language":"ja-JP"}})";
    SettingsTest::WriteRaw(userPath, original);

    ASSERT_TRUE(open_st::InitializeSettings(this->root_));
    EXPECT_FALSE(open_st::IsSettingsPersistenceAvailable());
    EXPECT_EQ(open_st::GetStringSetting("ui.language"), std::optional<std::string>("en-US"));
    EXPECT_FALSE(open_st::SetStringSetting("ui.language", "zh-CN"));
    EXPECT_EQ(SettingsTest::ReadRaw(userPath), original);
}

// 验证已读配置损坏后不把 Common 旧缓存冒充本次成功，Settings 自行读取默认值且能恢复。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(SettingsTest, failed_user_read_uses_defaults_and_recovers)
{
    this->WriteDefault();
    ASSERT_TRUE(open_st::InitializeSettings(this->root_));
    ASSERT_TRUE(open_st::SetStringSetting("ui.language", "ja-JP"));
    ASSERT_EQ(open_st::GetStringSetting("ui.language"), std::optional<std::string>("ja-JP"));

    const std::filesystem::path userPath = this->root_ / "data" / "settings.json";
    SettingsTest::WriteRaw(userPath, "{invalid changed user document");
    EXPECT_EQ(open_st::GetStringSetting("ui.language"), std::optional<std::string>("en-US"));
    EXPECT_FALSE(open_st::SetStringSetting("ui.language", "zh-CN"));
    SettingsTest::WriteRaw(userPath, R"({"schemaVersion":1,"settings":{"ui.language":"zh-CN"}})");
    EXPECT_EQ(open_st::GetStringSetting("ui.language"), std::optional<std::string>("zh-CN"));
}

// 验证同进程重启设置业务不会清理 Common 绑定，也不会复用损坏默认资源的旧缓存。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(SettingsTest, reinitialization_rejects_damaged_default_resource)
{
    this->WriteDefault();
    ASSERT_TRUE(open_st::InitializeSettings(this->root_));
    open_st::ShutdownSettings();
    SettingsTest::WriteRaw(this->root_ / "resources" / "default_settings.json", "{damaged defaults");

    EXPECT_FALSE(open_st::InitializeSettings(this->root_));
    EXPECT_FALSE(open_st::GetStringSetting("ui.language").has_value());
    this->WriteDefault("ja-JP");
    EXPECT_TRUE(open_st::InitializeSettings(this->root_));
}
// 验证用户文件读取告警只消费一次，默认值读取成功不重置用户故障，恢复后可再次告警。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(SettingsTest, user_read_warning_is_deduplicated_and_rearmed_after_recovery)
{
    this->WriteDefault();
    ASSERT_TRUE(open_st::InitializeSettings(this->root_));
    EXPECT_FALSE(open_st::ConsumeSettingsReadWarning());
    const std::filesystem::path userPath = this->root_ / "data" / "settings.json";
    SettingsTest::WriteRaw(userPath, "{broken user settings");
    EXPECT_EQ(open_st::GetStringSetting("ui.language"), std::optional<std::string>("en-US"));
    EXPECT_TRUE(open_st::ConsumeSettingsReadWarning());
    EXPECT_FALSE(open_st::ConsumeSettingsReadWarning());
    EXPECT_EQ(open_st::GetStringSetting("ui.language"), std::optional<std::string>("en-US"));
    EXPECT_FALSE(open_st::ConsumeSettingsReadWarning());

    SettingsTest::WriteRaw(userPath, R"({"schemaVersion":1,"settings":{"ui.language":"ja-JP"}})");
    EXPECT_EQ(open_st::GetStringSetting("ui.language"), std::optional<std::string>("ja-JP"));
    SettingsTest::WriteRaw(userPath, R"({"schemaVersion":2,"settings":{}})");
    EXPECT_EQ(open_st::GetStringSetting("ui.language"), std::optional<std::string>("en-US"));
    EXPECT_TRUE(open_st::ConsumeSettingsReadWarning());
    EXPECT_EQ(open_st::GetStringSetting("ui.language"), std::optional<std::string>("en-US"));
    EXPECT_FALSE(open_st::ConsumeSettingsReadWarning());
}

// 验证默认文件故障单独去重，缺失字段或类型回退不误报文件错误，关闭后无新告警。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(SettingsTest, default_read_warning_tracks_file_failure_not_missing_keys)
{
    this->WriteDefault();
    ASSERT_TRUE(open_st::InitializeSettings(this->root_));
    EXPECT_FALSE(open_st::GetIntegerSetting("ui.language").has_value());
    EXPECT_FALSE(open_st::ConsumeSettingsReadWarning());
    const std::filesystem::path defaultPath = this->root_ / "resources" / "default_settings.json";
    SettingsTest::WriteRaw(defaultPath, "{broken default settings");
    EXPECT_FALSE(open_st::GetStringSetting("missing").has_value());
    EXPECT_TRUE(open_st::ConsumeSettingsReadWarning());
    EXPECT_FALSE(open_st::GetStringSetting("missing").has_value());
    EXPECT_FALSE(open_st::ConsumeSettingsReadWarning());

    this->WriteDefault();
    EXPECT_FALSE(open_st::GetStringSetting("missing").has_value());
    EXPECT_FALSE(open_st::ConsumeSettingsReadWarning());
    SettingsTest::WriteRaw(defaultPath, "{broken again after recovery");
    EXPECT_FALSE(open_st::GetStringSetting("missing").has_value());
    EXPECT_TRUE(open_st::ConsumeSettingsReadWarning());
    open_st::ShutdownSettings();
    EXPECT_FALSE(open_st::GetStringSetting("missing").has_value());
    EXPECT_FALSE(open_st::ConsumeSettingsReadWarning());
}
// 验证第二实例的早期提示读取用户语言，完全不初始化或创建文件。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(SettingsTest, startup_language_reads_without_initialization_or_writes)
{
    this->WriteDefault("en-US");
    EXPECT_EQ(open_st::ReadStartupLanguage(this->root_), "en-US");
    EXPECT_FALSE(std::filesystem::exists(this->root_ / "data" / "settings.json"));
    this->WriteRaw(this->root_ / "data" / "settings.json", R"({"schemaVersion":1,"settings":{"ui.language":"zh-CN"}})");
    EXPECT_EQ(open_st::ReadStartupLanguage(this->root_), "zh-CN");
    this->WriteRaw(this->root_ / "data" / "settings.json", R"({"schemaVersion":1,"settings":{"ui.language":false}})");
    EXPECT_EQ(open_st::ReadStartupLanguage(this->root_), "en-US");
    EXPECT_FALSE(open_st::IsSettingsPersistenceAvailable());
}
// 验证默认专用 getter 不返回用户覆盖值，默认资源外改后读取新值且不写用户文件。
// 入参：无运行入参；GoogleTest 注册本用例。
// 返回：无，断言检查默认来源、实时读取和磁盘内容保持。
TEST_F(SettingsTest, default_string_getter_ignores_user_override_and_observes_resource)
{
    this->WriteDefault("en-US");
    const std::filesystem::path userPath = this->root_ / "data" / "settings.json";
    const std::string user = R"({"schemaVersion":1,"settings":{"ui.language":"zh-CN"}})";
    SettingsTest::WriteRaw(userPath, user);
    ASSERT_TRUE(open_st::InitializeSettings(this->root_));
    EXPECT_EQ(open_st::GetDefaultStringSetting("ui.language"), "en-US");
    EXPECT_EQ(open_st::GetStringSetting("ui.language"), "zh-CN");
    this->WriteDefault("ja-JP");
    EXPECT_EQ(open_st::GetDefaultStringSetting("ui.language"), "ja-JP");
    EXPECT_EQ(SettingsTest::ReadRaw(userPath), user);
    SettingsTest::WriteRaw(this->root_ / "resources" / "default_settings.json", "{broken");
    EXPECT_FALSE(open_st::GetDefaultStringSetting("ui.language"));
    EXPECT_TRUE(open_st::ConsumeSettingsReadWarning());
    EXPECT_FALSE(open_st::GetDefaultStringSetting("ui.language"));
    EXPECT_FALSE(open_st::ConsumeSettingsReadWarning());
}

// 验证来源重载区分缺失与类型非法，回退结果相同也不能丢失非法字段信息。
// 入参：无运行入参；GoogleTest 注册本用例。
// 返回：无，断言检查数字、null、缺失和有效字符串的来源标记。
TEST_F(SettingsTest, string_getter_reports_invalid_type_without_marking_missing_field)
{
    this->WriteDefault("en-US");
    const std::filesystem::path userPath = this->root_ / "data" / "settings.json";
    SettingsTest::WriteRaw(userPath, R"({"schemaVersion":1,"settings":{"ui.language":123}})");
    ASSERT_TRUE(open_st::InitializeSettings(this->root_));
    bool invalidUser = false;
    EXPECT_EQ(open_st::GetStringSetting("ui.language", invalidUser), "en-US");
    EXPECT_TRUE(invalidUser);
    SettingsTest::WriteRaw(userPath, R"({"schemaVersion":1,"settings":{"ui.language":null}})");
    EXPECT_EQ(open_st::GetStringSetting("ui.language", invalidUser), "en-US");
    EXPECT_TRUE(invalidUser);
    SettingsTest::WriteRaw(userPath, R"({"schemaVersion":1,"settings":{}})");
    EXPECT_EQ(open_st::GetStringSetting("ui.language", invalidUser), "en-US");
    EXPECT_FALSE(invalidUser);
    SettingsTest::WriteRaw(userPath, R"({"schemaVersion":1,"settings":{"ui.language":"ja-JP"}})");
    EXPECT_EQ(open_st::GetStringSetting("ui.language", invalidUser), "ja-JP");
    EXPECT_FALSE(invalidUser);
}

// 验证默认专用 getter 严格拒绝默认类型错误，不以合法用户值冒充默认或改写原文件。
// 入参：无运行入参；GoogleTest 注册本用例。
// 返回：无，断言检查默认字段类型以及正常用户读取仍可用。
TEST_F(SettingsTest, default_string_getter_rejects_wrong_type_without_using_user_value)
{
    this->WriteDefault("en-US");
    ASSERT_TRUE(open_st::InitializeSettings(this->root_));
    SettingsTest::WriteRaw(this->root_ / "resources" / "default_settings.json",
                           R"({"schemaVersion":1,"settings":{"ui.language":false}})");
    EXPECT_FALSE(open_st::GetDefaultStringSetting("ui.language"));
    EXPECT_EQ(open_st::GetStringSetting("ui.language"), "en-US");
    EXPECT_FALSE(open_st::GetDefaultStringSetting("missing"));
    EXPECT_FALSE(open_st::ConsumeSettingsReadWarning());
}
// 验证默认整数读取不消费用户覆盖，非法用户类型回退保留来源，默认故障产生去重告警。
// 入参：无运行入参。
// 返回：无，断言检查读取来源、溢出边界和默认资源告警。
TEST_F(SettingsTest, default_integer_and_invalid_user_source_are_distinct)
{
    SettingsTest::WriteRaw(this->root_ / "resources/default_settings.json",
                           R"({"schemaVersion":1,"settings":{"quality":95}})");
    SettingsTest::WriteRaw(this->root_ / "data/settings.json", R"({"schemaVersion":1,"settings":{"quality":50}})");
    ASSERT_TRUE(open_st::InitializeSettings(this->root_));
    bool invalidUser = true;
    EXPECT_EQ(open_st::GetIntegerSetting("quality", invalidUser), 50);
    EXPECT_FALSE(invalidUser);
    EXPECT_EQ(open_st::GetDefaultIntegerSetting("quality"), 95);
    SettingsTest::WriteRaw(this->root_ / "data/settings.json",
                           R"({"schemaVersion":1,"settings":{"quality":18446744073709551615}})");
    EXPECT_EQ(open_st::GetIntegerSetting("quality", invalidUser), 95);
    EXPECT_TRUE(invalidUser);
    SettingsTest::WriteRaw(this->root_ / "resources/default_settings.json", "{broken");
    EXPECT_FALSE(open_st::GetDefaultIntegerSetting("quality"));
    EXPECT_TRUE(open_st::ConsumeSettingsReadWarning());
    EXPECT_FALSE(open_st::GetDefaultIntegerSetting("quality"));
    EXPECT_FALSE(open_st::ConsumeSettingsReadWarning());
}
// 验证发行模式只来自默认资源，新用户配置不保存该资源字段。
// 入参：无。
// 返回：GoogleTest 断言结果。
TEST_F(SettingsTest, distribution_mode_is_resource_only_for_new_and_existing_user_files)
{
    this->WriteRaw(this->root_ / "resources/default_settings.json",
                   R"({"schemaVersion":1,"settings":{"ui.language":"en-US","distribution.mode":"installed"}})");
    ASSERT_TRUE(open_st::InitializeSettings(this->root_));
    const auto userPath = this->root_ / "data/settings.json";
    EXPECT_FALSE(nlohmann::json::parse(this->ReadRaw(userPath))["settings"].contains("distribution.mode"));
    EXPECT_EQ(open_st::GetDefaultStringSetting("distribution.mode"), "installed");
    this->WriteRaw(userPath, R"({"schemaVersion":1,"settings":{"distribution.mode":"portable"}})");
    const auto before = this->ReadRaw(userPath);
    EXPECT_EQ(open_st::GetDefaultStringSetting("distribution.mode"), "installed");
    EXPECT_EQ(this->ReadRaw(userPath), before);
    EXPECT_FALSE(std::filesystem::exists(this->root_ / "resources/default_settings.json.lock"));
}

// 验证初始化和直接写接口准确返回 Busy，手动新操作成功后清除该次错误输出。
// 入参：无。
// 返回：GoogleTest 断言结果。
TEST_F(SettingsTest, initialization_and_direct_writes_report_busy_without_changing_settings)
{
    this->WriteDefault();
    this->WriteRaw(this->root_ / "data/settings.json", R"({"schemaVersion":1,"settings":{"ui.language":"en-US"}})");
    open_st::FileLease lease;
    ASSERT_TRUE(lease.TryAcquire(this->root_ / "data/settings.json.lock", open_st::FileLeaseMode::Exclusive));
    open_st::SettingsWriteError error;
    ASSERT_TRUE(open_st::InitializeSettings(this->root_, &error));
    EXPECT_EQ(error, open_st::SettingsWriteError::Busy);
    EXPECT_FALSE(open_st::IsSettingsPersistenceAvailable());
    EXPECT_FALSE(open_st::SetStringSetting("ui.language", "ja-JP", &error));
    EXPECT_EQ(error, open_st::SettingsWriteError::Busy);
    EXPECT_FALSE(open_st::SetBoolSetting("startup.enabled", false, &error));
    EXPECT_EQ(error, open_st::SettingsWriteError::Busy);
    EXPECT_FALSE(open_st::SetIntegerSetting("export.jpeg_quality", 75, &error));
    EXPECT_EQ(error, open_st::SettingsWriteError::Busy);
    EXPECT_EQ(open_st::GetStringSetting("ui.language"), "en-US");
    lease.Reset();
    EXPECT_TRUE(open_st::SetStringSetting("ui.language", "ja-JP", &error));
    EXPECT_EQ(error, open_st::SettingsWriteError::None);
    EXPECT_EQ(open_st::GetStringSetting("ui.language"), "ja-JP");
}

// 快照保留显式空值及结构化内容，只对缺失字段使用默认，返回后不随磁盘变化。
// 入参：无。
// 返回：断言完整字段集合、默认来源和独立快照寿命。
TEST_F(SettingsTest, snapshot_merges_missing_fields_without_replacing_explicit_values)
{
    SettingsTest::WriteRaw(
        this->root_ / "resources/default_settings.json",
        R"({"schemaVersion":1,"settings":{"profiles":[],"mode":"system","address":"default-address","nullable":"default"}})");
    const std::filesystem::path userPath = this->root_ / "data/settings.json";
    SettingsTest::WriteRaw(
        userPath,
        R"({"schemaVersion":1,"settings":{"profiles":[{"id":"local"}],"mode":"custom","nullable":null,"unrelated":7}})");
    ASSERT_TRUE(open_st::InitializeSettings(this->root_));
    const std::array<std::string_view, 4> keys{"profiles", "mode", "address", "nullable"};
    const std::optional<nlohmann::json> snapshot = open_st::ReadSettingsSnapshot(keys);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->size(), keys.size());
    EXPECT_EQ(snapshot->at("profiles"), nlohmann::json::array({{{"id", "local"}}}));
    EXPECT_EQ(snapshot->at("mode"), "custom");
    EXPECT_EQ(snapshot->at("address"), "default-address");
    EXPECT_TRUE(snapshot->at("nullable").is_null());
    EXPECT_FALSE(snapshot->contains("unrelated"));
    SettingsTest::WriteRaw(
        userPath, R"({"schemaVersion":1,"settings":{"profiles":[],"mode":"system","address":"changed","nullable":5}})");
    const std::optional<nlohmann::json> changed = open_st::ReadSettingsSnapshot(keys);
    ASSERT_TRUE(changed.has_value());
    EXPECT_EQ(changed->at("mode"), "system");
    EXPECT_EQ(changed->at("address"), "changed");
    EXPECT_EQ(snapshot->at("mode"), "custom");
    EXPECT_EQ(snapshot->at("address"), "default-address");
}

// 用户文件损坏、结构错误或缺失时不能用默认网络配置拼出可执行快照。
// 入参：无。
// 返回：断言故障整组失败，恢复后取得新配置，不使用旧快照或默认模式。
TEST_F(SettingsTest, snapshot_rejects_user_file_failures_without_default_fallback)
{
    SettingsTest::WriteRaw(this->root_ / "resources/default_settings.json",
                           R"({"schemaVersion":1,"settings":{"mode":"system","address":""}})");
    const std::filesystem::path userPath = this->root_ / "data/settings.json";
    SettingsTest::WriteRaw(userPath, R"({"schemaVersion":1,"settings":{"mode":"custom","address":"private"}})");
    ASSERT_TRUE(open_st::InitializeSettings(this->root_));
    const std::array<std::string_view, 2> keys{"mode", "address"};
    ASSERT_TRUE(open_st::ReadSettingsSnapshot(keys).has_value());
    for (const std::string_view broken : {"{broken", R"({"schemaVersion":1,"settings":[]})"})
    {
        SettingsTest::WriteRaw(userPath, broken);
        EXPECT_FALSE(open_st::ReadSettingsSnapshot(keys));
        EXPECT_EQ(SettingsTest::ReadRaw(userPath), broken);
    }
    ASSERT_TRUE(std::filesystem::remove(userPath));
    EXPECT_FALSE(open_st::ReadSettingsSnapshot(keys));
    SettingsTest::WriteRaw(userPath, R"({"schemaVersion":1,"settings":{"mode":"custom","address":"restored"}})");
    const std::optional<nlohmann::json> restored = open_st::ReadSettingsSnapshot(keys);
    ASSERT_TRUE(restored.has_value());
    EXPECT_EQ(restored->at("mode"), "custom");
    EXPECT_EQ(restored->at("address"), "restored");
}

// 真实独占句柄令快照立即失败，已读缓存和默认值不能掩盖不可访问的用户配置。
// 入参：无。
// 返回：断言占用期间无快照，句柄释放后可显式重新读取。
TEST_F(SettingsTest, snapshot_rejects_exclusive_file_access_even_after_successful_read)
{
    this->WriteDefault("en-US");
    ASSERT_TRUE(open_st::InitializeSettings(this->root_));
    const std::array<std::string_view, 1> keys{"ui.language"};
    ASSERT_TRUE(open_st::ReadSettingsSnapshot(keys).has_value());
    SettingsTest::WriteRaw(this->root_ / "data/settings.json",
                           R"({"schemaVersion":1,"settings":{"ui.language":"ja-JP","external":1}})");
    // 只释放测试持有的文件句柄，失败句柄不交给系统关闭。
    // 入参：handle 为独占读取句柄或失败值。
    // 返回：无。
    const auto closeHandle = [](void* handle)
    {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
    };
    std::unique_ptr<void, decltype(closeHandle)> held(CreateFileW((this->root_ / "data/settings.json").c_str(),
                                                                  GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                                                  FILE_ATTRIBUTE_NORMAL, nullptr),
                                                      closeHandle);
    ASSERT_NE(held.get(), INVALID_HANDLE_VALUE);
    EXPECT_FALSE(open_st::ReadSettingsSnapshot(keys));
    held.reset();
    EXPECT_TRUE(open_st::ReadSettingsSnapshot(keys).has_value());
}

// 默认文件只在补齐时读取，缺少任一所需值或默认文档损坏都不能发布部分配置。
// 入参：无。
// 返回：断言完整性失败及用户已完整配置时不受无关默认故障影响。
TEST_F(SettingsTest, snapshot_requires_complete_fields_and_reads_defaults_only_for_missing_values)
{
    this->WriteDefault("en-US");
    ASSERT_TRUE(open_st::InitializeSettings(this->root_));
    const std::array<std::string_view, 2> keys{"ui.language", "mode"};
    EXPECT_FALSE(open_st::ReadSettingsSnapshot(keys));
    SettingsTest::WriteRaw(this->root_ / "resources/default_settings.json", "{broken");
    EXPECT_FALSE(open_st::ReadSettingsSnapshot(keys));
    SettingsTest::WriteRaw(this->root_ / "data/settings.json",
                           R"({"schemaVersion":1,"settings":{"ui.language":"ja-JP","mode":"custom"}})");
    const std::optional<nlohmann::json> complete = open_st::ReadSettingsSnapshot(keys);
    ASSERT_TRUE(complete.has_value());
    EXPECT_EQ(complete->at("ui.language"), "ja-JP");
    EXPECT_EQ(complete->at("mode"), "custom");
}
// 正常关闭删除空闲用户锁，保留配置并允许下次初始化重建锁。
// 入参：无。返回：GoogleTest 断言结果。
TEST_F(SettingsTest, shutdown_removes_idle_user_lock_and_restart_recreates_it)
{
    this->WriteDefault();
    ASSERT_TRUE(open_st::InitializeSettings(this->root_));
    const std::filesystem::path path = this->root_ / "data" / "settings.json";
    const std::filesystem::path lockPath = this->root_ / "data" / "settings.json.lock";
    const std::string before = SettingsTest::ReadRaw(path);
    ASSERT_TRUE(std::filesystem::exists(lockPath));
    open_st::ShutdownSettings();
    EXPECT_FALSE(std::filesystem::exists(lockPath));
    EXPECT_EQ(SettingsTest::ReadRaw(path), before);
    EXPECT_FALSE(open_st::SetStringSetting("ui.language", "zh-CN"));
    ASSERT_TRUE(open_st::InitializeSettings(this->root_));
    EXPECT_TRUE(std::filesystem::exists(lockPath));
    open_st::ShutdownSettings();
    EXPECT_FALSE(std::filesystem::exists(lockPath));
}

// 外部持有租约时退出不删除文件，也不阻塞；以后空闲退出仍可清理。
// 入参：无。返回：GoogleTest 断言结果。
TEST_F(SettingsTest, shutdown_preserves_busy_user_lock)
{
    this->WriteDefault();
    ASSERT_TRUE(open_st::InitializeSettings(this->root_));
    const std::filesystem::path lockPath = this->root_ / "data" / "settings.json.lock";
    open_st::FileLease external;
    ASSERT_TRUE(external.TryAcquire(lockPath, open_st::FileLeaseMode::Shared));
    open_st::ShutdownSettings();
    EXPECT_TRUE(std::filesystem::exists(lockPath));
    external.Reset();
    ASSERT_TRUE(open_st::InitializeSettings(this->root_));
    open_st::ShutdownSettings();
    EXPECT_FALSE(std::filesystem::exists(lockPath));
}

// 默认资源失败时仍清理先前遗留的空闲用户锁，不删除用户文档。
// 入参：无。返回：GoogleTest 断言结果。
TEST_F(SettingsTest, failed_initialization_removes_stale_idle_user_lock)
{
    this->WriteDefault();
    const std::filesystem::path path = this->root_ / "data" / "settings.json";
    const std::filesystem::path lockPath = this->root_ / "data" / "settings.json.lock";
    SettingsTest::WriteRaw(path, R"({"schemaVersion":1,"settings":{"ui.language":"en-US"}})");
    const std::string before = SettingsTest::ReadRaw(path);
    open_st::FileLease stale;
    ASSERT_TRUE(stale.TryAcquire(lockPath, open_st::FileLeaseMode::Exclusive));
    stale.Reset();
    SettingsTest::WriteRaw(this->root_ / "resources" / "default_settings.json", "broken");
    EXPECT_FALSE(open_st::InitializeSettings(this->root_));
    EXPECT_FALSE(std::filesystem::exists(lockPath));
    EXPECT_EQ(SettingsTest::ReadRaw(path), before);
}

} // namespace

// 旧配置只补新增链接字段，显式配置和已有凭据不被后续默认资源覆盖。
// 入参：无。返回：隔离文件验证。
TEST_F(SettingsTest, provider_resources_are_backfilled_once_without_overwriting_user_values)
{
    const nlohmann::json resources{{"baidu", nlohmann::json::array({{{"label_key", "resource.docs"},
                                                                     {"url", "https://example.invalid/default"}}})}};
    const nlohmann::json defaults{
        {"schemaVersion", 1}, {"settings", {{"translation.provider_resources", resources}, {"other.new.default", 1}}}};
    const std::filesystem::path userPath = this->root_ / "data/settings.json";
    const nlohmann::json original{{"schemaVersion", 1}, {"settings", {{"existing.secret", "preserved"}}}};
    this->WriteRaw(this->root_ / "resources/default_settings.json", defaults.dump());
    this->WriteRaw(userPath, original.dump());
    ASSERT_TRUE(open_st::InitializeSettings(this->root_));
    nlohmann::json actual = nlohmann::json::parse(this->ReadRaw(userPath));
    EXPECT_EQ(actual["settings"]["translation.provider_resources"], resources);
    EXPECT_EQ(actual["settings"]["existing.secret"], "preserved");
    EXPECT_FALSE(actual["settings"].contains("other.new.default"));
    open_st::ShutdownSettings();
    actual["settings"]["translation.provider_resources"] = nlohmann::json::object();
    this->WriteRaw(userPath, actual.dump());
    ASSERT_TRUE(open_st::InitializeSettings(this->root_));
    EXPECT_EQ(nlohmann::json::parse(this->ReadRaw(userPath)), actual);
}
