#include <settings.h>

#include "json_file_test_access.h"
#include "settings_internal.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace
{
class SettingsTest : public testing::Test
{
  protected:
    // 隔离具名文件绑定和测试目录，避免单例缓存污染不同测试。
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
    static void WriteRaw(const std::filesystem::path& path, std::string_view contents)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(output.is_open());
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        ASSERT_TRUE(output.good());
    }

    // 独立读取真实文件，确认业务写入或损坏保护的磁盘结果。
    static std::string ReadRaw(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }

    // 写入指定默认语言的合法设置资源。
    void WriteDefault(std::string_view language = "en-US")
    {
        const nlohmann::json document{{"schemaVersion", 1}, {"settings", {{"ui.language", language}}}};
        SettingsTest::WriteRaw(this->root_ / "resources" / "default_settings.json", document.dump(2));
    }

    std::filesystem::path root_;
};

// 验证启动时仅在用户配置缺失时从默认资源自动生成 settings.json。
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

// 验证用户值缺失或类型错误时，类型化 getter 会读取默认配置中的同名动态 key。
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
// 第二实例的早期提示读取用户语言，完全不初始化或创建文件。
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
} // namespace
