// 验证设置编辑草稿的变更检测、默认值恢复与提交校验。

#include "json_file_test_access.h"
#include "settings_edit.h"
#include "settings_internal.h"
#include <settings.h>

#include <gtest/gtest.h>
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iterator>

namespace
{
class SettingsEditTest : public testing::Test
{
  protected:
    // 隔离业务状态、Common 名称绑定和磁盘目录，避免测试影响正式设置。
    // 入参：无显式入参。
    // 返回：无返回值。
    void SetUp() override
    {
        open_st::ShutdownSettings();
        (void)open_st::JsonFileTestAccess::ReleaseFile("settings.user");
        (void)open_st::JsonFileTestAccess::ReleaseFile("settings.default");
        (void)open_st::JsonFileTestAccess::ReleaseFile("settings.layout");
        const testing::TestInfo* info = testing::UnitTest::GetInstance()->current_test_info();
        this->root_ = std::filesystem::temp_directory_path() /
                      ("open_st_edit_" + std::to_string(GetCurrentProcessId()) + "_" + info->name());
        std::filesystem::create_directories(this->root_ / "resources");
        std::filesystem::create_directories(this->root_ / "data");
        this->Write("resources/default_settings.json",
                    R"({"schemaVersion":1,"settings":{"ui.language":"en-US","other":"default"}})");
        this->Write("data/settings.json", R"({"schemaVersion":1,"settings":{}})");
        ASSERT_TRUE(open_st::InitializeSettings(this->root_));
    }

    // 释放全部窗口资源卡并恢复只读属性后清理隔离目录。
    // 入参：无显式入参。
    // 返回：无返回值。
    void TearDown() override
    {
        open_st::ShutdownSettings();
        (void)open_st::JsonFileTestAccess::ReleaseFile("settings.user");
        (void)open_st::JsonFileTestAccess::ReleaseFile("settings.default");
        (void)open_st::JsonFileTestAccess::ReleaseFile("settings.layout");
        (void)SetFileAttributesW((this->root_ / "data/settings.json").c_str(), FILE_ATTRIBUTE_NORMAL);
        std::error_code error;
        std::filesystem::remove_all(this->root_, error);
        EXPECT_FALSE(error);
    }

    // 模拟外部编辑器直接改变隔离文件的内容。
    // 入参：relative 为相对隔离测试根目录的文件路径；content 为写入的原始文本。
    // 返回：无返回值。
    void Write(const std::filesystem::path& relative, std::string_view content)
    {
        std::ofstream output(this->root_ / relative, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(output.is_open());
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
        ASSERT_TRUE(output.good());
    }

    // 独立读取用户文档以验证真正磁盘结果。
    // 入参：无显式入参。
    // 返回：从测试用户配置文件解析得到的 JSON 文档。
    nlohmann::json ReadUser()
    {
        std::ifstream input(this->root_ / "data/settings.json", std::ios::binary);
        return nlohmann::json::parse(input);
    }

    std::filesystem::path root_;
};

// 验证版本校验不窄化数值，并确保读取、直接写入和编辑会话一致拒绝非法版本。
// 入参：无运行入参。
// 返回：无返回值；通过 GoogleTest 断言记录回退及文件保留结果。
TEST_F(SettingsEditTest, schema_numeric_boundaries_match_read_and_edit)
{
    const std::vector<nlohmann::json> invalidVersions{4294967297ULL, -4294967295LL, 18446744073709551615ULL, -1, 1.0};
    for (const nlohmann::json& version : invalidVersions)
    {
        SCOPED_TRACE(version.dump());
        open_st::ShutdownSettings();
        ASSERT_TRUE(open_st::JsonFileTestAccess::ReleaseFile("settings.user"));
        ASSERT_TRUE(open_st::JsonFileTestAccess::ReleaseFile("settings.default"));
        const nlohmann::json document{{"schemaVersion", version},
                                      {"settings", {{"ui.language", "ja-JP"}, {"unknown", 42}}}};
        this->Write("data/settings.json", document.dump());
        ASSERT_TRUE(open_st::InitializeSettings(this->root_));
        EXPECT_EQ(open_st::GetStringSetting("ui.language"), "en-US");
        EXPECT_FALSE(open_st::SetStringSetting("ui.language", "zh-CN"));
        open_st::SettingsEditSession session;
        EXPECT_FALSE(session.Open({"ui.language"}));
        EXPECT_EQ(this->ReadUser(), document);
    }
}

// 验证合法无符号版本 1 可用于读取和编辑，提交继续保留未知设置字段。
// 入参：无运行入参。
// 返回：无返回值；通过 GoogleTest 断言记录有效版本的兼容性。
TEST_F(SettingsEditTest, unsigned_schema_one_remains_readable_and_editable)
{
    const nlohmann::json document{{"schemaVersion", 1ULL}, {"settings", {{"ui.language", "ja-JP"}, {"unknown", 42}}}};
    this->Write("data/settings.json", document.dump());
    EXPECT_EQ(open_st::GetStringSetting("ui.language"), "ja-JP");
    open_st::SettingsEditSession session;
    ASSERT_TRUE(session.Open({"ui.language"}));
    ASSERT_TRUE(session.ChangeString("ui.language", "zh-CN"));
    EXPECT_EQ(session.Commit(), open_st::SettingsCommitResult::Saved);
    EXPECT_EQ(this->ReadUser().at("settings").at("unknown"), 42);
}

// 验证缺失字段显示默认值但不补写，改回有效基线也必须保持文件缺失字段。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(SettingsEditTest, fallback_and_reverted_draft_do_not_write)
{
    open_st::SettingsEditSession session;
    ASSERT_TRUE(session.Open({"ui.language"}));
    EXPECT_EQ(session.ReadString("ui.language"), "en-US");
    EXPECT_FALSE(session.IsDirty());
    ASSERT_TRUE(session.ChangeString("ui.language", "ja-JP"));
    EXPECT_TRUE(session.IsDirty());
    ASSERT_TRUE(session.ChangeString("ui.language", "en-US"));
    EXPECT_EQ(session.Commit(), open_st::SettingsCommitResult::Unchanged);
    EXPECT_FALSE(this->ReadUser().at("settings").contains("ui.language"));
}

// 验证一次差异提交保留外部新增字段，成功基线允许连续编辑并验证仅重试生效。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(SettingsEditTest, batch_commit_preserves_external_unrelated_fields)
{
    open_st::SettingsEditSession session;
    ASSERT_TRUE(session.Open({"ui.language", "other"}));
    ASSERT_TRUE(session.ChangeString("ui.language", "ja-JP"));
    ASSERT_TRUE(session.ChangeString("other", "changed"));
    this->Write("data/settings.json", R"({"schemaVersion":1,"settings":{"external":123}})");
    EXPECT_EQ(session.Commit(), open_st::SettingsCommitResult::Saved);
    const nlohmann::json saved = this->ReadUser();
    EXPECT_EQ(saved.at("settings").at("external"), 123);
    EXPECT_EQ(saved.at("settings").at("other"), "changed");
    EXPECT_EQ(saved.at("settings").at("ui.language"), "ja-JP");
    EXPECT_FALSE(session.IsDirty());
    EXPECT_EQ(session.VerifySavedString("ui.language", "ja-JP"), open_st::SettingsCommitResult::Unchanged);
    EXPECT_EQ(session.VerifySavedString("ui.language", "en-US"), open_st::SettingsCommitResult::Conflict);
}

// 验证缺失和显式 null 不等价，任一字段冲突必须取消整批写入且保留全部草稿。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(SettingsEditTest, raw_existence_conflict_cancels_entire_batch)
{
    open_st::SettingsEditSession session;
    ASSERT_TRUE(session.Open({"ui.language", "other"}));
    ASSERT_TRUE(session.ChangeString("ui.language", "ja-JP"));
    ASSERT_TRUE(session.ChangeString("other", "changed"));
    this->Write("data/settings.json", R"({"schemaVersion":1,"settings":{"ui.language":null}})");
    EXPECT_EQ(session.Commit(), open_st::SettingsCommitResult::Conflict);
    EXPECT_FALSE(this->ReadUser().at("settings").contains("other"));
    EXPECT_EQ(session.ReadString("other"), "changed");
    EXPECT_TRUE(session.IsDirty());
}

// 验证原始错误类型与有效默认分离，外部写入相同目标仍可安全完成提交。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(SettingsEditTest, invalid_raw_type_and_external_target_are_supported)
{
    this->Write("data/settings.json", R"({"schemaVersion":1,"settings":{"ui.language":7}})");
    open_st::SettingsEditSession session;
    ASSERT_TRUE(session.Open({"ui.language"}));
    EXPECT_EQ(session.ReadString("ui.language"), "en-US");
    ASSERT_TRUE(session.ChangeString("ui.language", "ja-JP"));
    this->Write("data/settings.json", R"({"schemaVersion":1,"settings":{"ui.language":"ja-JP"}})");
    EXPECT_EQ(session.Commit(), open_st::SettingsCommitResult::Saved);
    EXPECT_FALSE(session.IsDirty());
}

// 验证数字七和浮点七虽然 JSON 数值相等，原始类型改变仍须报告冲突。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(SettingsEditTest, raw_numeric_type_change_is_a_conflict)
{
    this->Write("data/settings.json", R"({"schemaVersion":1,"settings":{"ui.language":7}})");
    open_st::SettingsEditSession session;
    ASSERT_TRUE(session.Open({"ui.language"}));
    ASSERT_TRUE(session.ChangeString("ui.language", "ja-JP"));
    this->Write("data/settings.json", R"({"schemaVersion":1,"settings":{"ui.language":7.0}})");
    EXPECT_EQ(session.Commit(), open_st::SettingsCommitResult::Conflict);
    EXPECT_TRUE(this->ReadUser().at("settings").at("ui.language").is_number_float());
}

// 验证尚未建立可靠基线的会话不能因没有差异就被认定可提交。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(SettingsEditTest, unopened_session_rejects_commit_and_unknown_field)
{
    open_st::SettingsEditSession session;
    EXPECT_EQ(session.Commit(), open_st::SettingsCommitResult::ReadFailed);
    EXPECT_FALSE(session.ChangeString("ui.language", "ja-JP"));
    EXPECT_EQ(session.VerifySavedString("ui.language", "ja-JP"), open_st::SettingsCommitResult::InvalidField);
    EXPECT_FALSE(session.Open({"ui.language", "ui.language"}));
    EXPECT_EQ(session.Commit(), open_st::SettingsCommitResult::ReadFailed);
}

// 验证删除和损坏用户文件后拒绝重建；修复并显式重开后才取得新的可靠基线。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(SettingsEditTest, deleted_or_corrupt_user_is_never_recreated)
{
    open_st::SettingsEditSession session;
    ASSERT_TRUE(session.Open({"ui.language"}));
    ASSERT_TRUE(session.ChangeString("ui.language", "ja-JP"));
    ASSERT_TRUE(std::filesystem::remove(this->root_ / "data/settings.json"));
    EXPECT_EQ(session.Commit(), open_st::SettingsCommitResult::ReadFailed);
    EXPECT_FALSE(std::filesystem::exists(this->root_ / "data/settings.json"));
    this->Write("data/settings.json", "{invalid");
    EXPECT_NE(session.Commit(), open_st::SettingsCommitResult::Saved);
    EXPECT_FALSE(session.Open({"ui.language"}));
    EXPECT_EQ(session.ReadString("ui.language"), "ja-JP");
    this->Write("data/settings.json", R"({"schemaVersion":1,"settings":{"ui.language":"zh-CN"}})");
    ASSERT_TRUE(session.Open({"ui.language"}));
    EXPECT_EQ(session.ReadString("ui.language"), "zh-CN");
    EXPECT_FALSE(session.IsDirty());
}

// 验证只读提交失败不能推进基线；解除只读后可按原草稿重试。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(SettingsEditTest, read_only_failure_preserves_draft_for_retry)
{
    open_st::SettingsEditSession session;
    ASSERT_TRUE(session.Open({"ui.language"}));
    ASSERT_TRUE(session.ChangeString("ui.language", "ja-JP"));
    ASSERT_NE(SetFileAttributesW((this->root_ / "data/settings.json").c_str(), FILE_ATTRIBUTE_READONLY), FALSE);
    EXPECT_EQ(session.Commit(), open_st::SettingsCommitResult::WriteFailed);
    EXPECT_TRUE(session.IsDirty());
    ASSERT_NE(SetFileAttributesW((this->root_ / "data/settings.json").c_str(), FILE_ATTRIBUTE_NORMAL), FALSE);
    EXPECT_EQ(session.Commit(), open_st::SettingsCommitResult::Saved);
}

// 验证本页默认只改变指定字段，多个字段中一个默认无效时不能部分恢复。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(SettingsEditTest, restore_defaults_is_scoped_and_atomic)
{
    open_st::SettingsEditSession session;
    ASSERT_TRUE(session.Open({"ui.language", "other"}));
    ASSERT_TRUE(session.ChangeString("ui.language", "ja-JP"));
    ASSERT_TRUE(session.ChangeString("other", "changed"));
    ASSERT_TRUE(session.RestoreDefaults({"ui.language"}));
    EXPECT_EQ(session.ReadString("other"), "changed");
    this->Write("resources/default_settings.json",
                R"({"schemaVersion":1,"settings":{"ui.language":"zh-CN","other":false}})");
    EXPECT_FALSE(session.RestoreDefaults({"ui.language", "other"}));
    EXPECT_EQ(session.ReadString("ui.language"), "en-US");
    EXPECT_EQ(session.ReadString("other"), "changed");
}

// 验证布局必须读取初始化目录的 Common 入口，并在读取失败时保持输出原值。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(SettingsEditTest, layout_uses_initialized_resource_directory)
{
    this->Write("resources/setting_windows.json", R"({"schemaVersion":1,"pages":[]})");
    nlohmann::json layout;
    ASSERT_TRUE(open_st::ReadSettingsLayout(layout));
    EXPECT_TRUE(layout.at("pages").is_array());
    this->Write("resources/setting_windows.json", "broken");
    EXPECT_FALSE(open_st::ReadSettingsLayout(layout));
    EXPECT_TRUE(layout.at("pages").is_array());
}
// 验证布尔和字符串保持类型安全，并在欢迎确认时保存默认意图。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(SettingsEditTest, boolean_drafts_and_required_welcome_fields)
{
    this->Write(
        "resources/default_settings.json",
        R"({"schemaVersion":1,"settings":{"ui.language":"en-US","startup.enabled":true,"onboarding.completed":false}})");
    open_st::SettingsEditSession session;
    ASSERT_TRUE(session.Open({"ui.language"}, {"startup.enabled", "onboarding.completed"}));
    EXPECT_EQ(session.ReadBool("startup.enabled"), true);
    EXPECT_FALSE(session.ReadString("startup.enabled").has_value());
    EXPECT_FALSE(session.ChangeString("startup.enabled", "false"));
    EXPECT_FALSE(session.ChangeBool("ui.language", false));
    EXPECT_EQ(session.Commit(), open_st::SettingsCommitResult::Unchanged);
    ASSERT_TRUE(session.ChangeBool("onboarding.completed", true));
    ASSERT_EQ(session.Commit({"startup.enabled", "onboarding.completed"}), open_st::SettingsCommitResult::Saved);
    EXPECT_TRUE(this->ReadUser().at("settings").at("startup.enabled").get<bool>());
    EXPECT_EQ(session.VerifySavedBool("startup.enabled", true), open_st::SettingsCommitResult::Unchanged);
    ASSERT_TRUE(session.ChangeBool("startup.enabled", false));
    EXPECT_TRUE(session.IsDirty("startup.enabled"));
    ASSERT_TRUE(session.RestoreDefaults({"startup.enabled"}));
    EXPECT_FALSE(session.IsDirty());
}

// 验证同批布尔字段冲突阻止语言一起保存；重试校验不接受字符串冒充布尔。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(SettingsEditTest, boolean_conflict_preserves_entire_batch)
{
    this->Write("resources/default_settings.json",
                R"({"schemaVersion":1,"settings":{"ui.language":"en-US","startup.enabled":true}})");
    open_st::SettingsEditSession session;
    ASSERT_TRUE(session.Open({"ui.language"}, {"startup.enabled"}));
    ASSERT_TRUE(session.ChangeString("ui.language", "zh-CN"));
    ASSERT_TRUE(session.ChangeBool("startup.enabled", false));
    this->Write("data/settings.json", R"({"schemaVersion":1,"settings":{"startup.enabled":"false"}})");
    EXPECT_EQ(session.Commit(), open_st::SettingsCommitResult::Conflict);
    EXPECT_FALSE(this->ReadUser().at("settings").contains("ui.language"));
    EXPECT_EQ(session.VerifySavedBool("startup.enabled", false), open_st::SettingsCommitResult::Conflict);
}
} // namespace
