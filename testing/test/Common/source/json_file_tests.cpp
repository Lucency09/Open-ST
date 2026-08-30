#include <json_file.h>

#include "json_file_test_access.h"
#include "logger.h"

#include <gtest/gtest.h>
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
class ExclusiveFile final
{
  public:
    // 独占测试文件，模拟外部程序阻止 Common 打开读写句柄。
    explicit ExclusiveFile(const std::filesystem::path& path)
        : handle_(CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr))
    {
    }
    // 断言提前结束时也关闭模拟的外部独占句柄。
    ~ExclusiveFile()
    {
        if (this->handle_ != INVALID_HANDLE_VALUE)
        {
            CloseHandle(this->handle_);
        }
    }
    // 禁止复制 Win32 句柄所有权。
    ExclusiveFile(const ExclusiveFile&) = delete;
    // 禁止复制赋值以避免重复释放句柄。
    ExclusiveFile& operator=(const ExclusiveFile&) = delete;
    // 返回独占文件条件是否成功建立。
    bool IsValid() const noexcept
    {
        return this->handle_ != INVALID_HANDLE_VALUE;
    }
  private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

class JsonFileTest : public testing::Test
{
  protected:
    // 每例使用隔离目录与动态卡名，不操作产品运行文件。
    void SetUp() override
    {
        open_st::Logger::Shutdown();
        const testing::TestInfo* info = testing::UnitTest::GetInstance()->current_test_info();
        this->root_ = std::filesystem::temp_directory_path() /
            ("open_st_json_" + std::to_string(GetCurrentProcessId()) + "_" + info->name());
        std::error_code error;
        std::filesystem::remove_all(this->root_, error);
        ASSERT_FALSE(error);
        std::filesystem::create_directories(this->root_, error);
        ASSERT_FALSE(error);
        this->cardName_ = "common.test." + std::string(info->name());
    }
    // 释放测试绑定、恢复只读属性并清理测试目录。
    void TearDown() override
    {
        open_st::Logger::Shutdown();
        EXPECT_TRUE(open_st::JsonFileTestAccess::ReleaseFile(this->cardName_));
        EXPECT_TRUE(open_st::JsonFileTestAccess::ReleaseFile(this->cardName_ + ".alias"));
        std::error_code error;
        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(this->root_, error))
        {
            if (entry.is_regular_file())
            {
                (void)SetFileAttributesW(entry.path().c_str(), FILE_ATTRIBUTE_NORMAL);
            }
        }
        std::filesystem::remove_all(this->root_, error);
        EXPECT_FALSE(error);
    }
    // 模拟外部程序绕过 Common 修改文件，供变更和冲突测试使用。
    static void WriteRaw(const std::filesystem::path& path, std::string_view contents)
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(output.is_open());
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        ASSERT_TRUE(output.good());
    }
    // 读取原始字节，检查失败或无变化操作没有重排或覆盖原文件。
    static std::string ReadRaw(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
    // 返回本用例唯一的具名文件入口，不执行文件读取。
    open_st::JsonFileHandle Open(const std::filesystem::path& path) const
    {
        return open_st::JsonFileManager::Instance().GetFile(this->cardName_, path);
    }
    std::filesystem::path root_;
    std::string cardName_;
};

// 验证获取入口不创建文件，缺失读取失败保留输出，外部创建后正常读取。
TEST_F(JsonFileTest, handle_acquisition_is_lazy)
{
    const std::filesystem::path path = this->root_ / "lazy.json";
    const open_st::JsonFileHandle handle = this->Open(path);
    ASSERT_TRUE(handle.IsValid());
    EXPECT_FALSE(std::filesystem::exists(path));
    nlohmann::json output = "sentinel";
    EXPECT_FALSE(handle.Read(output));
    EXPECT_EQ(output, "sentinel");
    EXPECT_FALSE(std::filesystem::exists(path));
    JsonFileTest::WriteRaw(path, R"({"later":true})");
    ASSERT_TRUE(handle.Read(output));
    EXPECT_TRUE(output.at("later").get<bool>());
}

// 验证相同绑定共享状态并观察外部更新，读取副本被业务修改不污染缓存。
TEST_F(JsonFileTest, handles_share_state_and_reload_external_changes)
{
    const std::filesystem::path path = this->root_ / "shared.json";
    JsonFileTest::WriteRaw(path, R"({"value":"before"})");
    const open_st::JsonFileHandle first = this->Open(path);
    const open_st::JsonFileHandle second = this->Open(path);
    nlohmann::json output;
    ASSERT_TRUE(first.Read(output));
    EXPECT_EQ(output.at("value").get<std::string>(), "before");
    output["value"] = "caller edit";
    ASSERT_TRUE(second.Read(output));
    EXPECT_EQ(output.at("value").get<std::string>(), "before");
    JsonFileTest::WriteRaw(path, R"({"value":"after and a different size"})");
    ASSERT_TRUE(second.Read(output));
    EXPECT_EQ(output.at("value").get<std::string>(), "after and a different size");
}

// 验证一个卡名不能绑定两条路径，同一路径也不能绑定不同卡名。
TEST_F(JsonFileTest, rejects_conflicting_name_and_path_bindings)
{
    const std::filesystem::path path = this->root_ / "first.json";
    const open_st::JsonFileHandle first = this->Open(path);
    EXPECT_TRUE(first.IsValid());
    EXPECT_FALSE(this->Open(this->root_ / "second.json").IsValid());
    EXPECT_FALSE(open_st::JsonFileManager::Instance().GetFile(this->cardName_ + ".alias", path).IsValid());
    EXPECT_TRUE(this->Open(path).IsValid());
}

// 验证无效参数和句柄均失败，不修改输出、不执行编辑回调或创建文件。
TEST_F(JsonFileTest, invalid_handles_and_arguments_fail_without_side_effects)
{
    const std::filesystem::path path = this->root_ / "invalid.json";
    EXPECT_FALSE(open_st::JsonFileManager::Instance().GetFile("", path).IsValid());
    EXPECT_FALSE(open_st::JsonFileManager::Instance().GetFile(this->cardName_, {}).IsValid());
    const open_st::JsonFileHandle invalid;
    EXPECT_FALSE(invalid.IsValid());
    nlohmann::json output = "sentinel";
    EXPECT_FALSE(invalid.Read(output));
    EXPECT_EQ(output, "sentinel");
    EXPECT_FALSE(invalid.Write(nlohmann::json{{"value", 1}}));
    bool called = false;
    // 无效入口必须在调用业务前失败。
    EXPECT_FALSE(invalid.Write([&called](std::optional<nlohmann::json>&)
    {
        called = true;
        return true;
    }));
    EXPECT_FALSE(called);
    const open_st::JsonFileHandle valid = this->Open(path);
    EXPECT_FALSE(valid.Write(open_st::JsonDocumentEditor{}));
    EXPECT_FALSE(std::filesystem::exists(path));
}

// 验证损坏后不会用旧缓存冒充成功，普通写和编辑写均拒绝覆盖损坏文件。
TEST_F(JsonFileTest, invalid_external_update_preserves_output_and_rejects_writes)
{
    const std::filesystem::path path = this->root_ / "stale.json";
    JsonFileTest::WriteRaw(path, R"({"known":1})");
    const open_st::JsonFileHandle handle = this->Open(path);
    nlohmann::json output;
    ASSERT_TRUE(handle.Read(output));
    JsonFileTest::WriteRaw(path, "{broken json");
    output = "sentinel";
    EXPECT_FALSE(handle.Read(output));
    EXPECT_FALSE(handle.Read(output));
    EXPECT_EQ(output, "sentinel");
    bool called = false;
    // 损坏文档不交给编辑器，避免业务基于旧快照写回。
    EXPECT_FALSE(handle.Write([&called](std::optional<nlohmann::json>& document)
    {
        called = true;
        document = nlohmann::json{{"known", 2}};
        return true;
    }));
    EXPECT_FALSE(called);
    EXPECT_FALSE(handle.Write(nlohmann::json{{"known", 2}}));
    EXPECT_EQ(JsonFileTest::ReadRaw(path), "{broken json");
}

// 验证无效签名不会阻止文件恢复后重新读取，失败期间输出仍保持原值。
TEST_F(JsonFileTest, changed_invalid_file_is_retried_after_next_update)
{
    const std::filesystem::path path = this->root_ / "retry.json";
    JsonFileTest::WriteRaw(path, R"({"value":"first"})");
    const open_st::JsonFileHandle handle = this->Open(path);
    nlohmann::json output;
    ASSERT_TRUE(handle.Read(output));
    JsonFileTest::WriteRaw(path, "{invalid and a distinct size");
    EXPECT_FALSE(handle.Read(output));
    EXPECT_EQ(output.at("value").get<std::string>(), "first");
    JsonFileTest::WriteRaw(path, R"({"value":"recovered with another size"})");
    ASSERT_TRUE(handle.Read(output));
    EXPECT_EQ(output.at("value").get<std::string>(), "recovered with another size");
}

// 验证删除已有缓存对应文件时失败不改输出，重新创建后取得新内容。
TEST_F(JsonFileTest, deleted_file_fails_read_and_recovers_after_recreation)
{
    const std::filesystem::path path = this->root_ / "deleted.json";
    JsonFileTest::WriteRaw(path, R"({"known":1})");
    const open_st::JsonFileHandle handle = this->Open(path);
    nlohmann::json output;
    ASSERT_TRUE(handle.Read(output));
    ASSERT_TRUE(std::filesystem::remove(path));
    output = "sentinel";
    EXPECT_FALSE(handle.Read(output));
    EXPECT_EQ(output, "sentinel");
    JsonFileTest::WriteRaw(path, R"({"known":222})");
    ASSERT_TRUE(handle.Read(output));
    EXPECT_EQ(output.at("known").get<int>(), 222);
}

// 验证外部独占使读取失败时不返回旧缓存、不执行编辑，解除独占后恢复。
TEST_F(JsonFileTest, inaccessible_changed_file_fails_without_running_editor)
{
    const std::filesystem::path path = this->root_ / "exclusive.json";
    JsonFileTest::WriteRaw(path, R"({"value":1})");
    const open_st::JsonFileHandle handle = this->Open(path);
    nlohmann::json output;
    ASSERT_TRUE(handle.Read(output));
    JsonFileTest::WriteRaw(path, R"({"value":987654})");
    {
        const ExclusiveFile exclusive(path);
        ASSERT_TRUE(exclusive.IsValid());
        output = "sentinel";
        EXPECT_FALSE(handle.Read(output));
        EXPECT_EQ(output, "sentinel");
        bool called = false;
        // 无法访问当前文件时不把旧内容提供给编辑器。
        EXPECT_FALSE(handle.Write([&called](std::optional<nlohmann::json>&)
        {
            called = true;
            return true;
        }));
        EXPECT_FALSE(called);
        EXPECT_FALSE(handle.Write(nlohmann::json{{"value", 0}}));
    }
    ASSERT_TRUE(handle.Read(output));
    EXPECT_EQ(output.at("value").get<int>(), 987654);
}

// 验证编辑器识别缺失后填入业务文档可安全创建，并立即读取新内容。
TEST_F(JsonFileTest, create_creates_file_and_publishes_snapshot)
{
    const std::filesystem::path path = this->root_ / "created.json";
    const open_st::JsonFileHandle handle = this->Open(path);
    // 仅缺失时填充默认内容，Common 不参与默认字段决策。
    ASSERT_TRUE(handle.Write([](std::optional<nlohmann::json>& document)
    {
        if (document.has_value())
        {
            return false;
        }
        document = nlohmann::json{{"created", true}};
        return true;
    }));
    nlohmann::json output;
    ASSERT_TRUE(handle.Read(output));
    EXPECT_TRUE(output.at("created").get<bool>());
}

// 验证普通整份写入也能创建不存在的父目录和文件。
TEST_F(JsonFileTest, whole_document_write_creates_missing_parent_directories)
{
    const std::filesystem::path path = this->root_ / "nested" / "created.json";
    const open_st::JsonFileHandle handle = this->Open(path);
    const nlohmann::json expected{{"created", 1}};
    ASSERT_TRUE(handle.Write(expected));
    nlohmann::json output;
    ASSERT_TRUE(handle.Read(output));
    EXPECT_EQ(output, expected);
}

// 验证整份写入按明确目标替换旧文档，旧字段不被意外保留。
TEST_F(JsonFileTest, write_replaces_existing_file_and_publishes_snapshot)
{
    const std::filesystem::path path = this->root_ / "replaced.json";
    JsonFileTest::WriteRaw(path, R"({"before":true,"removed":42})");
    const open_st::JsonFileHandle handle = this->Open(path);
    nlohmann::json output;
    ASSERT_TRUE(handle.Read(output));
    const nlohmann::json replacement{{"after", true}};
    ASSERT_TRUE(handle.Write(replacement));
    ASSERT_TRUE(handle.Read(output));
    EXPECT_EQ(output, replacement);
}

// 验证业务只在缺失时创建的意图不会覆盖已存在文件。
TEST_F(JsonFileTest, create_never_overwrites_existing_file)
{
    const std::filesystem::path path = this->root_ / "existing.json";
    const std::string original = R"({"owner":"external"})";
    JsonFileTest::WriteRaw(path, original);
    const open_st::JsonFileHandle handle = this->Open(path);
    // 文档已存在则由业务取消，不需要底层认识默认配置。
    EXPECT_FALSE(handle.Write([](std::optional<nlohmann::json>& document)
    {
        if (document.has_value())
        {
            return false;
        }
        document = nlohmann::json{{"owner", "application"}};
        return true;
    }));
    EXPECT_EQ(JsonFileTest::ReadRaw(path), original);
}

// 验证锁内编辑只改目标字段，保留业务未识别的所有字段。
TEST_F(JsonFileTest, update_preserves_unknown_values)
{
    const std::filesystem::path path = this->root_ / "update.json";
    JsonFileTest::WriteRaw(path, R"({"settings":{"ui.language":"en-US","unknown":42}})");
    const open_st::JsonFileHandle handle = this->Open(path);
    // 编辑最新文档中的单个字段。
    ASSERT_TRUE(handle.Write([](std::optional<nlohmann::json>& document)
    {
        document.value()["settings"]["ui.language"] = "ja-JP";
        return true;
    }));
    nlohmann::json output;
    ASSERT_TRUE(handle.Read(output));
    EXPECT_EQ(output.at("settings").at("ui.language").get<std::string>(), "ja-JP");
    EXPECT_EQ(output.at("settings").at("unknown").get<int>(), 42);
}

// 验证四线程各五次累计修改全部串行成功，总计二十次修改不丢失。
TEST_F(JsonFileTest, serializes_concurrent_updates)
{
    const std::filesystem::path path = this->root_ / "concurrent.json";
    JsonFileTest::WriteRaw(path, R"({"counter":0})");
    const open_st::JsonFileHandle handle = this->Open(path);
    std::vector<std::thread> workers;
    for (int workerIndex = 0; workerIndex < 4; ++workerIndex)
    {
        // 各线程复制轻量句柄，连续请求五次原子编辑。
        workers.emplace_back([handle]()
        {
            for (int updateIndex = 0; updateIndex < 5; ++updateIndex)
            {
                // 读计数和改计数属于同一个被锁保护的编辑。
                const bool updated = handle.Write([](std::optional<nlohmann::json>& document)
                {
                    document.value()["counter"] = document.value()["counter"].get<int>() + 1;
                    return true;
                });
                EXPECT_TRUE(updated);
            }
        });
    }
    for (std::thread& worker : workers)
    {
        worker.join();
    }
    nlohmann::json output;
    ASSERT_TRUE(handle.Read(output));
    EXPECT_EQ(output.at("counter").get<int>(), 20);
}

// 验证编辑拒绝、抛异常和清空 optional 均取消，不改磁盘、不发布候选数据、不自动重放。
TEST_F(JsonFileTest, rejected_throwing_and_reset_editors_do_not_commit)
{
    const std::filesystem::path path = this->root_ / "cancel.json";
    const std::string original = R"({"stable":1})";
    JsonFileTest::WriteRaw(path, original);
    const open_st::JsonFileHandle handle = this->Open(path);
    int calls = 0;
    // 修改候选值后主动拒绝提交。
    EXPECT_FALSE(handle.Write([&calls](std::optional<nlohmann::json>& document)
    {
        ++calls;
        document.value()["stable"] = 2;
        return false;
    }));
    // 修改候选值后抛异常，要求只执行一次且异常不穿过接口。
    EXPECT_FALSE(handle.Write([&calls](std::optional<nlohmann::json>& document) -> bool
    {
        ++calls;
        document.value()["stable"] = 3;
        throw std::runtime_error("cancel edit");
    }));
    // 清空 optional 不是文件删除操作，只取消本次修改。
    EXPECT_FALSE(handle.Write([&calls](std::optional<nlohmann::json>& document)
    {
        ++calls;
        document.reset();
        return true;
    }));
    EXPECT_EQ(calls, 3);
    EXPECT_EQ(JsonFileTest::ReadRaw(path), original);
    nlohmann::json output;
    ASSERT_TRUE(handle.Read(output));
    EXPECT_EQ(output.at("stable").get<int>(), 1);
}

// 验证合法 JSON null 是有值文档，不能与缺失文件的空 optional 混淆。
TEST_F(JsonFileTest, json_null_remains_a_present_document)
{
    const std::filesystem::path path = this->root_ / "null.json";
    const open_st::JsonFileHandle handle = this->Open(path);
    ASSERT_TRUE(handle.Write(nlohmann::json(nullptr)));
    nlohmann::json output = "sentinel";
    ASSERT_TRUE(handle.Read(output));
    EXPECT_TRUE(output.is_null());
    bool presentNull = false;
    // 对已有 null 进行普通业务结构替换。
    ASSERT_TRUE(handle.Write([&presentNull](std::optional<nlohmann::json>& document)
    {
        presentNull = document.has_value() && document->is_null();
        document = nlohmann::json{{"converted", true}};
        return true;
    }));
    EXPECT_TRUE(presentNull);
    ASSERT_TRUE(handle.Read(output));
    EXPECT_TRUE(output.at("converted").get<bool>());
}

// 验证同内容写入保留原排版与修改时间，完成后不残留临时探测文件。
TEST_F(JsonFileTest, unchanged_write_preserves_bytes_time_and_cleans_probe)
{
    const std::filesystem::path path = this->root_ / "unchanged.json";
    const std::string original = "{ \"stable\" : 1 }\r\n";
    JsonFileTest::WriteRaw(path, original);
    const std::filesystem::file_time_type writeTime = std::filesystem::last_write_time(path);
    const open_st::JsonFileHandle handle = this->Open(path);
    ASSERT_TRUE(handle.Write(nlohmann::json{{"stable", 1}}));
    // 不变更文档的编辑仍需检查写入前提。
    ASSERT_TRUE(handle.Write([](std::optional<nlohmann::json>& document)
    {
        return document.has_value();
    }));
    EXPECT_EQ(JsonFileTest::ReadRaw(path), original);
    EXPECT_EQ(std::filesystem::last_write_time(path), writeTime);
    std::size_t count = 0U;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(this->root_))
    {
        ++count;
        EXPECT_EQ(entry.path(), path);
    }
    EXPECT_EQ(count, 1U);
}

// 验证只读文件即使内容相同也写入失败，正常读取不受影响。
TEST_F(JsonFileTest, readonly_unchanged_write_fails_without_modifying_file)
{
    const std::filesystem::path path = this->root_ / "readonly.json";
    const std::string original = R"({"stable":1})";
    JsonFileTest::WriteRaw(path, original);
    ASSERT_NE(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY), FALSE);
    const open_st::JsonFileHandle handle = this->Open(path);
    EXPECT_FALSE(handle.Write(nlohmann::json{{"stable", 1}}));
    // 不变更的编辑不能绕过只读检查。
    EXPECT_FALSE(handle.Write([](std::optional<nlohmann::json>& document)
    {
        return document.has_value();
    }));
    EXPECT_EQ(JsonFileTest::ReadRaw(path), original);
    nlohmann::json output;
    ASSERT_TRUE(handle.Read(output));
    EXPECT_EQ(output.at("stable").get<int>(), 1);
}

// 验证缺失检查之后外部抢先创建文件时，Common 取消而不覆盖对方内容。
TEST_F(JsonFileTest, external_creation_during_editor_is_not_overwritten)
{
    const std::filesystem::path path = this->root_ / "create_race.json";
    const open_st::JsonFileHandle handle = this->Open(path);
    const std::string external = R"({"owner":"external process"})";
    // 仅测试故意绕过 Common 模拟外部创建；产品编辑器不允许此类副作用。
    EXPECT_FALSE(handle.Write([&path, &external](std::optional<nlohmann::json>& document)
    {
        EXPECT_FALSE(document.has_value());
        JsonFileTest::WriteRaw(path, external);
        document = nlohmann::json{{"owner", "application"}};
        return true;
    }));
    EXPECT_EQ(JsonFileTest::ReadRaw(path), external);
    nlohmann::json output;
    ASSERT_TRUE(handle.Read(output));
    EXPECT_EQ(output.at("owner").get<std::string>(), "external process");
}

// 验证编辑期间外部改写已有文件，提交检查发现冲突后不发布候选数据。
TEST_F(JsonFileTest, external_change_during_editor_is_not_overwritten)
{
    const std::filesystem::path path = this->root_ / "update_race.json";
    JsonFileTest::WriteRaw(path, R"({"owner":"old"})");
    const open_st::JsonFileHandle handle = this->Open(path);
    const std::string external = R"({"owner":"external changed to a distinct size"})";
    // 只在测试中模拟其他进程写入，不经同一锁重入。
    EXPECT_FALSE(handle.Write([&path, &external](std::optional<nlohmann::json>& document)
    {
        JsonFileTest::WriteRaw(path, external);
        document.value()["owner"] = "application";
        return true;
    }));
    EXPECT_EQ(JsonFileTest::ReadRaw(path), external);
    nlohmann::json output;
    ASSERT_TRUE(handle.Read(output));
    EXPECT_EQ(output.at("owner").get<std::string>(), "external changed to a distinct size");
}

// 验证数值相等但类型不同的 1.0 到 1 必须真正提交，不能把 JSON 数值相等误判为无需写入。
TEST_F(JsonFileTest, whole_write_preserves_requested_integer_type_when_float_was_equal)
{
    const std::filesystem::path path = this->root_ / "number_type.json";
    const std::string original = R"({"value":1.0})";
    JsonFileTest::WriteRaw(path, original);
    const open_st::JsonFileHandle handle = this->Open(path);
    nlohmann::json output;
    ASSERT_TRUE(handle.Read(output));
    ASSERT_TRUE(output.at("value").is_number_float());
    const nlohmann::json requested{{"value", 1}};
    ASSERT_TRUE(handle.Write(requested));
    ASSERT_TRUE(handle.Read(output));
    EXPECT_TRUE(output.at("value").is_number_integer());
    EXPECT_FALSE(output.at("value").is_number_float());
    EXPECT_EQ(output.at("value").get<int>(), 1);
    const std::string bytes = JsonFileTest::ReadRaw(path);
    EXPECT_NE(bytes, original);
    const nlohmann::json disk = nlohmann::json::parse(bytes);
    EXPECT_TRUE(disk.at("value").is_number_integer());
    EXPECT_FALSE(disk.at("value").is_number_float());
}

// 验证存活句柄阻止私有清理，句柄销毁后绑定仍由管理器保留，显式测试清理后才可重绑。
TEST_F(JsonFileTest, manager_retains_binding_until_last_handle_is_gone_and_test_releases_it)
{
    const std::filesystem::path path = this->root_ / "retained.json";
    const std::filesystem::path replacementPath = this->root_ / "replacement.json";
    {
        const open_st::JsonFileHandle handle = this->Open(path);
        ASSERT_TRUE(handle.IsValid());
        EXPECT_FALSE(open_st::JsonFileTestAccess::ReleaseFile(this->cardName_));
        {
            const open_st::JsonFileHandle copied = handle;
            EXPECT_TRUE(copied.IsValid());
            EXPECT_FALSE(open_st::JsonFileTestAccess::ReleaseFile(this->cardName_));
        }
        EXPECT_FALSE(open_st::JsonFileTestAccess::ReleaseFile(this->cardName_));
    }
    EXPECT_FALSE(this->Open(replacementPath).IsValid());
    EXPECT_TRUE(open_st::JsonFileTestAccess::ReleaseFile(this->cardName_));
    const open_st::JsonFileHandle rebound = this->Open(replacementPath);
    EXPECT_TRUE(rebound.IsValid());
}

// 验证解析与编辑异常留下文件诊断，但日志不含 JSON 正文或异常携带的业务秘密。
TEST_F(JsonFileTest, failures_log_diagnostics_without_document_or_exception_contents)
{
    ASSERT_TRUE(open_st::Logger::Initialize(this->root_));
    const std::filesystem::path path = this->root_ / "diagnostic.json";
    const open_st::JsonFileHandle handle = this->Open(path);
    JsonFileTest::WriteRaw(path, "{\"key\":\"SECRET_JSON_CONTENT_761\", malformed");
    nlohmann::json output;
    EXPECT_FALSE(handle.Read(output));
    JsonFileTest::WriteRaw(path, R"({"stable":1})");
    // 模拟异常正文携带敏感数据，日志只能记录错误类别。
    EXPECT_FALSE(handle.Write([](std::optional<nlohmann::json>&) -> bool
    {
        throw std::runtime_error("SECRET_EXCEPTION_CONTENT_762");
    }));
    open_st::Logger::Shutdown();
    std::string logs;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(this->root_ / "data" / "logs"))
    {
        if (entry.is_regular_file())
        {
            logs += JsonFileTest::ReadRaw(entry.path());
        }
    }
    EXPECT_FALSE(logs.empty());
    EXPECT_NE(logs.find(this->cardName_), std::string::npos);
    EXPECT_EQ(logs.find("SECRET_JSON_CONTENT_761"), std::string::npos);
    EXPECT_EQ(logs.find("SECRET_EXCEPTION_CONTENT_762"), std::string::npos);
}
} // namespace
