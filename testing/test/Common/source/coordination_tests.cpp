// 通过真实子进程验证立即失败、事务完整性与活跃日志保留。

#include "json_file_test_access.h"
#include "logger.h"
#include <atomic>
#include <chrono>
#include <file_lease.h>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <json_file.h>
#include <log.h>
#include <string>
#include <vector>
#include <windows.h>
#include <windows_util.h>

namespace
{
class Probe final
{
  public:
    // 启动独立探针，等待其确认已经取得待测试资源。
    // 入参：mode：探针模式；path：隔离测试路径。
    // 返回：构造后 Ready 表示握手是否成功。
    Probe(std::wstring_view mode, const std::filesystem::path& path)
    {
        static std::atomic<unsigned> sequence{};
        const std::wstring name = L"Local\\OpenSTCoordinationTest-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                  std::to_wstring(++sequence);
        this->ready_ = CreateEventW(nullptr, TRUE, FALSE, (name + L"-ready").c_str());
        this->release_ = CreateEventW(nullptr, TRUE, FALSE, (name + L"-release").c_str());
        const auto executable = open_st::GetExecutableDirectory() / "open_st_common_process_probe.exe";
        std::wstring command = L"\"" + executable.native() + L"\" " + std::wstring(mode) + L" \"" + path.native() +
                               L"\" \"" + name + L"-ready\" \"" + name + L"-release\"";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (this->ready_ != nullptr && this->release_ != nullptr &&
            CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                           nullptr, &startup, &process))
        {
            CloseHandle(process.hThread);
            this->process_ = process.hProcess;
            HANDLE events[]{this->ready_, this->process_};
            this->isReady_ = WaitForMultipleObjects(2, events, FALSE, 5000) == WAIT_OBJECT_0;
        }
    }
    // 释放或终止仅由当前测试创建的子进程，并关闭事件句柄。
    // 入参：无。
    // 返回：无返回值。
    ~Probe()
    {
        (void)this->Finish();
        if (this->ready_ != nullptr)
            CloseHandle(this->ready_);
        if (this->release_ != nullptr)
            CloseHandle(this->release_);
    }
    // 禁止复制子进程所有权。
    // 入参：源探针。
    // 返回：不可调用。
    Probe(const Probe&) = delete;
    // 禁止复制子进程所有权。
    // 入参：源探针。
    // 返回：不可调用。
    Probe& operator=(const Probe&) = delete;
    // 查询探针是否进入受保护区域。
    // 入参：无。
    // 返回：握手成功为 true。
    bool Ready() const
    {
        return this->isReady_;
    }
    // 异常结束本测试创建的子进程，验证系统自动回收租约。
    // 入参：无。
    // 返回：终止请求成功为 true。
    bool Terminate()
    {
        return this->process_ != nullptr && TerminateProcess(this->process_, 71) != FALSE;
    }
    // 结束探针并读取退出码，超时仅终止本测试创建的进程。
    // 入参：无。
    // 返回：退出码，失败为非零。
    DWORD Finish()
    {
        if (this->process_ == nullptr)
            return this->exitCode_;
        SetEvent(this->release_);
        if (WaitForSingleObject(this->process_, 5000) != WAIT_OBJECT_0)
        {
            TerminateProcess(this->process_, 99);
            WaitForSingleObject(this->process_, 5000);
        }
        GetExitCodeProcess(this->process_, &this->exitCode_);
        CloseHandle(this->process_);
        this->process_ = nullptr;
        return this->exitCode_;
    }

  private:
    HANDLE process_{};
    HANDLE ready_{};
    HANDLE release_{};
    DWORD exitCode_{99};
    bool isReady_{};
};

class CoordinationTest : public testing::Test
{
  protected:
    // 为当前用例创建隔离目录和卡名。
    // 入参：无。
    // 返回：无返回值。
    void SetUp() override
    {
        open_st::Logger::Shutdown();
        const auto name = testing::UnitTest::GetInstance()->current_test_info()->name();
        this->card_ = std::string("coordination.") + name;
        this->root_ = std::filesystem::temp_directory_path() /
                      ("open_st_coordination_" + std::to_string(GetCurrentProcessId()) + "_" + name);
        std::filesystem::create_directories(this->root_);
    }
    // 释放绑定并仅清理本测试创建的隔离目录。
    // 入参：无。
    // 返回：无返回值。
    void TearDown() override
    {
        open_st::Logger::Shutdown();
        EXPECT_TRUE(open_st::JsonFileTestAccess::ReleaseFile(this->card_));
        std::error_code error;
        std::filesystem::remove_all(this->root_, error);
        EXPECT_FALSE(error);
    }
    // 枚举当前用例的真实日志，排除稳定锁文件。
    // 入参：无。
    // 返回：普通日志文件路径。
    std::vector<std::filesystem::path> Logs() const
    {
        std::vector<std::filesystem::path> result;
        for (const auto& item : std::filesystem::directory_iterator(this->root_ / "data" / "logs"))
            if (item.path().extension() == ".log")
                result.push_back(item.path());
        return result;
    }
    std::filesystem::path root_;
    std::string card_;
};

// 验证跨进程共享兼容，独占冲突立即返回，释放后新操作成功。
// 入参：无。
// 返回：GoogleTest 断言结果。
TEST_F(CoordinationTest, shared_process_lease_rejects_exclusive_without_waiting)
{
    const auto path = this->root_ / "lease.lock";
    Probe probe(L"shared", path);
    ASSERT_TRUE(probe.Ready());
    open_st::FileLease shared;
    ASSERT_TRUE(shared.TryAcquire(path, open_st::FileLeaseMode::Shared));
    open_st::FileLease exclusive;
    open_st::FileLeaseError error;
    const auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(exclusive.TryAcquire(path, open_st::FileLeaseMode::Exclusive, &error));
    EXPECT_EQ(error.code, open_st::FileLeaseErrorCode::Busy);
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(1));
    shared.Reset();
    EXPECT_EQ(probe.Finish(), 0U);
    EXPECT_TRUE(exclusive.TryAcquire(path, open_st::FileLeaseMode::Exclusive, &error));
    EXPECT_EQ(error.code, open_st::FileLeaseErrorCode::None);
    EXPECT_TRUE(std::filesystem::exists(path));
}

// 验证租约转移只有一个持有者，并拒绝硬链接身份歧义。
// 入参：无。
// 返回：GoogleTest 断言结果。
TEST_F(CoordinationTest, lease_move_and_hardlink_rejection)
{
    const auto path = this->root_ / "lease.lock";
    open_st::FileLease original;
    open_st::FileLeaseError initialError;
    ASSERT_TRUE(original.TryAcquire(path, open_st::FileLeaseMode::Exclusive, &initialError)) << initialError.systemCode;
    open_st::FileLease moved(std::move(original));
    EXPECT_FALSE(original.IsHeld());
    EXPECT_TRUE(moved.IsHeld());
    moved.Reset();
    const auto alias = this->root_ / "alias.lock";
    ASSERT_TRUE(CreateHardLinkW(alias.c_str(), path.c_str(), nullptr));
    open_st::FileLeaseError error;
    EXPECT_FALSE(original.TryAcquire(alias, open_st::FileLeaseMode::Exclusive, &error));
    EXPECT_EQ(error.code, open_st::FileLeaseErrorCode::AccessDenied);
}

// 验证独占租约拒绝共享获取，异常退出后无需删除锁文件即可重新获取。
// 入参：无。
// 返回：GoogleTest 断言结果。
TEST_F(CoordinationTest, crashed_process_releases_exclusive_lease)
{
    const auto path = this->root_ / "lease.lock";
    Probe probe(L"exclusive", path);
    ASSERT_TRUE(probe.Ready());
    open_st::FileLease lease;
    open_st::FileLeaseError error;
    EXPECT_FALSE(lease.TryAcquire(path, open_st::FileLeaseMode::Shared, &error));
    EXPECT_EQ(error.code, open_st::FileLeaseErrorCode::Busy);
    ASSERT_TRUE(probe.Terminate());
    EXPECT_EQ(probe.Finish(), 71U);
    EXPECT_TRUE(lease.TryAcquire(path, open_st::FileLeaseMode::Exclusive));
}

// 验证父目录只在明确请求时逐层建立，默认失败不产生路径副作用。
// 入参：无。
// 返回：GoogleTest 断言结果。
TEST_F(CoordinationTest, lease_creates_parents_only_when_explicitly_requested)
{
    const auto path = this->root_ / "new" / "nested" / "resource.lock";
    open_st::FileLease lease;
    EXPECT_FALSE(lease.TryAcquire(path, open_st::FileLeaseMode::Exclusive));
    EXPECT_FALSE(std::filesystem::exists(this->root_ / "new"));
    EXPECT_TRUE(lease.TryAcquire(path, open_st::FileLeaseMode::Exclusive, nullptr, true));
    EXPECT_TRUE(std::filesystem::is_regular_file(path));
}

// 验证租约真正阻止祖先目录改名；只查询属性的句柄不足以保证 Windows 共享约束。
// 入参：无。
// 返回：GoogleTest 断言结果。
TEST_F(CoordinationTest, lease_pins_ancestor_against_actual_rename)
{
    const auto parent = this->root_ / "pinned";
    const auto moved = this->root_ / "moved";
    open_st::FileLease lease;
    ASSERT_TRUE(lease.TryAcquire(parent / "nested" / "file.lock", open_st::FileLeaseMode::Exclusive, nullptr, true));
    EXPECT_FALSE(MoveFileExW(parent.c_str(), moved.c_str(), 0));
    EXPECT_EQ(GetLastError(), static_cast<DWORD>(ERROR_SHARING_VIOLATION));
    EXPECT_TRUE(std::filesystem::exists(parent / "nested" / "file.lock"));
    lease.Reset();
    EXPECT_TRUE(MoveFileExW(parent.c_str(), moved.c_str(), 0));
    EXPECT_TRUE(std::filesystem::exists(moved / "nested" / "file.lock"));
}

// 验证进程日志持有期间整个数据目录不可被替换，关闭后目录不再被无关句柄占用。
// 入参：无。
// 返回：GoogleTest 断言结果。
TEST_F(CoordinationTest, logger_pins_data_directory_until_shutdown)
{
    ASSERT_TRUE(open_st::Logger::Initialize(this->root_));
    const auto directory = this->root_ / "data";
    const auto moved = this->root_ / "moved-data";
    EXPECT_FALSE(MoveFileExW(directory.c_str(), moved.c_str(), 0));
    EXPECT_EQ(GetLastError(), static_cast<DWORD>(ERROR_SHARING_VIOLATION));
    open_st::Logger::Shutdown();
    // 整目录改名还受子文件外部占用影响，不能以其失败推断 Logger 锚点残留。
    // 立即取得 DELETE 权限直接证明原防删除锚点已解除，不等待、不重试。
    const HANDLE handle = CreateFileW(directory.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    ASSERT_NE(handle, INVALID_HANDLE_VALUE) << GetLastError();
    const std::unique_ptr<void, decltype(&CloseHandle)> releasedDirectory(handle, CloseHandle);
}

// 验证已有目录删除句柄与祖先锚定互斥，未获保护时不创建子目录。
// 入参：无。
// 返回：GoogleTest 断言结果。
TEST_F(CoordinationTest, lease_rejects_existing_directory_delete_access_before_creating_children)
{
    const auto directory = this->root_ / "delete-access";
    ASSERT_TRUE(std::filesystem::create_directory(directory));
    const HANDLE handle = CreateFileW(directory.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    ASSERT_NE(handle, INVALID_HANDLE_VALUE);
    const std::unique_ptr<void, decltype(&CloseHandle)> held(handle, CloseHandle);
    open_st::FileLease lease;
    open_st::FileLeaseError error;
    EXPECT_FALSE(
        lease.TryAcquire(directory / "not-created" / "lease.lock", open_st::FileLeaseMode::Exclusive, &error, true));
    EXPECT_EQ(error.code, open_st::FileLeaseErrorCode::Busy);
    EXPECT_FALSE(std::filesystem::exists(directory / "not-created"));
}

// 验证外部事务持锁期间不进入编辑器，释放后读取最新磁盘值而不是旧缓存。
// 入参：无。
// 返回：GoogleTest 断言结果。
TEST_F(CoordinationTest, json_process_transaction_is_busy_then_edits_latest_value)
{
    const auto path = this->root_ / "settings.json";
    const auto file = open_st::JsonFileManager::Instance().GetFile(this->card_, path);
    ASSERT_TRUE(file.Write(nlohmann::json{{"count", 0}}));
    Probe probe(L"json", path);
    ASSERT_TRUE(probe.Ready());
    bool called = false;
    open_st::JsonFileError error;
    // Busy 时此回调必须完全不执行。
    // 入参：文档不使用。
    // 返回：若被错误调用则接受编辑供断言捕获。
    EXPECT_FALSE(file.Write(
        [&called](std::optional<nlohmann::json>&)
        {
            called = true;
            return true;
        },
        &error));
    EXPECT_FALSE(called);
    EXPECT_EQ(error.code, open_st::JsonFileErrorCode::Busy);
    EXPECT_FALSE(file.Write(nlohmann::json{{"count", 99}}, &error));
    EXPECT_EQ(error.code, open_st::JsonFileErrorCode::Busy);
    EXPECT_EQ(probe.Finish(), 0U);
    // 使用刚提交的新版本做第二次事务。
    // 入参：document：最新文档。
    // 返回：接受计数递增。
    ASSERT_TRUE(file.Write(
        [](std::optional<nlohmann::json>& document)
        {
            (*document)["count"] = document->value("count", 0) + 1;
            return true;
        },
        &error));
    EXPECT_EQ(error.code, open_st::JsonFileErrorCode::None);
    nlohmann::json result;
    ASSERT_TRUE(file.Read(result));
    EXPECT_EQ(result["count"], 2);
}

// 验证纯读取不创建资源旁锁文件。
// 入参：无。
// 返回：GoogleTest 断言结果。
TEST_F(CoordinationTest, resource_read_does_not_create_sidecar)
{
    const auto path = this->root_ / "resource.json";
    std::ofstream(path) << "{\"mode\":\"portable\"}";
    const auto file = open_st::JsonFileManager::Instance().GetFile(this->card_, path);
    nlohmann::json result;
    ASSERT_TRUE(file.Read(result));
    EXPECT_FALSE(std::filesystem::exists(this->root_ / "resource.json.lock"));
}

// 验证两个进程使用独立活跃日志，维护和退出清理正常保留另一进程的文件。
// 入参：无。
// 返回：GoogleTest 断言结果。
TEST_F(CoordinationTest, logger_process_files_are_separate_and_live_file_is_retained)
{
    ASSERT_TRUE(open_st::Logger::Initialize(this->root_));
    OPEN_ST_LOG_ERROR("parent record");
    Probe probe(L"logger", this->root_);
    ASSERT_TRUE(probe.Ready());
    EXPECT_EQ(this->Logs().size(), 2U);
    const auto cleaned = open_st::ClearHistoricalLogs();
    EXPECT_EQ(cleaned.status, open_st::LogCleanupStatus::Completed);
    EXPECT_EQ(cleaned.retained, 2U);
    EXPECT_EQ(cleaned.deleted, 0U);
    std::size_t retained{};
    EXPECT_TRUE(open_st::ShutdownAndClearLogging(nullptr, &retained));
    EXPECT_EQ(retained, 1U);
    EXPECT_EQ(this->Logs().size(), 1U);
    EXPECT_EQ(probe.Finish(), 0U);
}

// 验证协调占用立即返回 Busy，写普通行不尝试目录锁，轮转失败只通知一次。
// 入参：无。
// 返回：GoogleTest 断言结果。
TEST_F(CoordinationTest, logger_coordination_busy_is_not_retried_or_reported_recursively)
{
    open_st::LogOptions options;
    options.maxLines = 1;
    ASSERT_TRUE(open_st::Logger::Initialize(this->root_, options));
    Probe probe(L"exclusive", this->root_ / "data" / "logs" / ".coordination.lock");
    ASSERT_TRUE(probe.Ready());
    const auto clean = open_st::ClearHistoricalLogs();
    EXPECT_EQ(clean.status, open_st::LogCleanupStatus::Busy);
    EXPECT_EQ(clean.deleted, 0U);
    EXPECT_FALSE(open_st::ConsumeLoggingFailure().has_value());
    OPEN_ST_LOG_ERROR("first ordinary line");
    EXPECT_FALSE(open_st::ConsumeLoggingFailure().has_value());
    OPEN_ST_LOG_ERROR("rotation needs coordination");
    const auto failure = open_st::ConsumeLoggingFailure();
    ASSERT_TRUE(failure.has_value());
    EXPECT_EQ(failure->code, open_st::FileLeaseErrorCode::Busy);
    OPEN_ST_LOG_ERROR("disabled logging cannot recurse");
    EXPECT_FALSE(open_st::ConsumeLoggingFailure().has_value());
    EXPECT_EQ(probe.Finish(), 0U);
}

// 验证退出清理用结构化 Busy 保留真实文件，释放后仅用户新操作能清理成功。
// 入参：无。
// 返回：GoogleTest 断言结果。
TEST_F(CoordinationTest, shutdown_cleanup_returns_busy_without_deleting_logs)
{
    ASSERT_TRUE(open_st::Logger::Initialize(this->root_));
    OPEN_ST_LOG_ERROR("preserved until a new explicit cleanup");
    Probe probe(L"exclusive", this->root_ / "data" / "logs" / ".coordination.lock");
    ASSERT_TRUE(probe.Ready());
    open_st::FileLeaseError error;
    EXPECT_FALSE(open_st::ShutdownAndClearLogging(&error));
    EXPECT_EQ(error.code, open_st::FileLeaseErrorCode::Busy);
    EXPECT_EQ(this->Logs().size(), 1U);
    EXPECT_EQ(probe.Finish(), 0U);
    EXPECT_EQ(this->Logs().size(), 1U);
    EXPECT_TRUE(open_st::ShutdownAndClearLogging(&error));
    EXPECT_EQ(error.code, open_st::FileLeaseErrorCode::None);
    EXPECT_TRUE(this->Logs().empty());
}
} // namespace
