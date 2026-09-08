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
    void Write(const std::filesystem::path& relative, std::string_view content)
    {
        std::ofstream output(this->root_ / relative, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(output.is_open());
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
        ASSERT_TRUE(output.good());
    }

    // 独立读取用户文档以验证真正磁盘结果。
    nlohmann::json ReadUser()
    {
        std::ifstream input(this->root_ / "data/settings.json", std::ios::binary);
        return nlohmann::json::parse(input);
    }

    std::filesystem::path root_;
};

// 缺失字段显示默认值但不补写，改回有效基线也必须保持文件缺失字段。
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

// 一次差异提交保留外部新增字段，成功基线允许连续编辑并验证仅重试生效。
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

// 缺失和显式 null 不等价，任一字段冲突必须取消整批写入且保留全部草稿。
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

// 原始错误类型与有效默认分离，外部写入相同目标仍可安全完成提交。
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

// 数字七和浮点七虽然 JSON 数值相等，原始类型改变仍须报告冲突。
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

// 尚未建立可靠基线的会话不能因没有差异就被认定可提交。
TEST_F(SettingsEditTest, unopened_session_rejects_commit_and_unknown_field)
{
    open_st::SettingsEditSession session;
    EXPECT_EQ(session.Commit(), open_st::SettingsCommitResult::ReadFailed);
    EXPECT_FALSE(session.ChangeString("ui.language", "ja-JP"));
    EXPECT_EQ(session.VerifySavedString("ui.language", "ja-JP"), open_st::SettingsCommitResult::InvalidField);
    EXPECT_FALSE(session.Open({"ui.language", "ui.language"}));
    EXPECT_EQ(session.Commit(), open_st::SettingsCommitResult::ReadFailed);
}

// 删除和损坏用户文件后拒绝重建；修复并显式重开后才取得新的可靠基线。
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

// 只读提交失败不能推进基线；解除只读后可按原草稿重试。
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

// 本页默认只改变指定字段，多个字段中一个默认无效时不能部分恢复。
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

// 布局必须读取初始化目录的 Common 入口，并在读取失败时保持输出原值。
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
} // namespace
