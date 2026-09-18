// 文件职责：验证安装包缓存的完整性、权限与路径保护和显式清理，不执行任何测试 EXE。

#include "protected_download.h"

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <sddl.h>
#include <string>
#include <windows.h>

namespace open_st::update_detail
{
namespace
{
constexpr std::string_view ABC_SHA256 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
constexpr std::array<std::byte, 3> ABC{std::byte{0x61}, std::byte{0x62}, std::byte{0x63}};

struct TestHandleCloser
{
    // 关闭仅由测试打开的对象句柄。
    // 入参：value 为 Win32 句柄或无效值。
    // 返回：无。
    void operator()(void* value) const noexcept
    {
        if (value != nullptr && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
};
using TestHandle = std::unique_ptr<void, TestHandleCloser>;

class ProtectedDownloadTest : public testing::Test
{
  protected:
    std::filesystem::path root_;
    std::filesystem::path cache_;

    // 建立本用例独有目录，包含中文与空格以覆盖本地路径编码。
    // 入参：无。
    // 返回：无；不使用实际应用数据。
    void SetUp() override
    {
        static std::atomic<unsigned> sequence{};
        this->root_ =
            std::filesystem::temp_directory_path() /
            (L"Open-ST 更新缓存-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++sequence));
        this->cache_ = this->root_ / L"data" / L"updates";
        std::filesystem::create_directories(this->cache_.parent_path());
    }

    // 删除仅属于当前用例的隔离目录；链接测试在退出前先删除自己的链接。
    // 入参：无。
    // 返回：无。
    void TearDown() override
    {
        std::error_code error;
        std::filesystem::remove_all(this->root_, error);
    }

    // 创建固定三字节响应的下载事务，详细错误留给断言。
    // 入参：error 输出创建失败原因。
    // 返回：下载对象或空。
    std::unique_ptr<ProtectedDownload> Create(UpdateDownloadError& error)
    {
        return ProtectedDownload::Create(this->cache_, ABC.size(), ABC_SHA256, error);
    }
};

// 精确 SHA-256 完成后允许读取，禁止改写、文件改名和祖先改名，析构删除本请求。
// 入参：无。
// 返回：测试断言。
TEST_F(ProtectedDownloadTest, completion_preserves_identity_and_blocks_mutation_until_release)
{
    UpdateDownloadError error;
    std::unique_ptr<ProtectedDownload> download = this->Create(error);
    ASSERT_NE(download, nullptr) << static_cast<int>(error);
    EXPECT_EQ(download->Path().extension(), L".part");
    ASSERT_TRUE(download->Append(ABC, error));
    ASSERT_TRUE(download->Complete(error)) << static_cast<int>(error) << " win32=" << GetLastError();
    const std::filesystem::path file = download->Path();
    EXPECT_EQ(file.extension(), L".exe");
    const TestHandle read(CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
    ASSERT_NE(read.get(), INVALID_HANDLE_VALUE);
    std::array<std::byte, 3> actual{};
    DWORD count = 0;
    ASSERT_TRUE(ReadFile(read.get(), actual.data(), static_cast<DWORD>(actual.size()), &count, nullptr));
    EXPECT_EQ(actual, ABC);
    const TestHandle write(CreateFileW(file.c_str(), GENERIC_WRITE,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                       0, nullptr));
    EXPECT_EQ(write.get(), INVALID_HANDLE_VALUE);
    EXPECT_FALSE(MoveFileExW(file.c_str(), (file.parent_path() / L"changed.exe").c_str(), 0));
    EXPECT_FALSE(MoveFileExW(this->cache_.c_str(), (this->root_ / L"changed").c_str(), 0));
    // 此读句柄故意不参与析构清理断言，缓存清理由后续显式清理验证。
    download->Preserve();
    download.reset();
    EXPECT_TRUE(std::filesystem::exists(file));
}

// 没有系统启动交接的完成文件和 .part 都由唯一事务析构回收。
// 入参：无。
// 返回：测试断言。
TEST_F(ProtectedDownloadTest, unhanded_partial_and_complete_files_are_removed)
{
    UpdateDownloadError error;
    std::unique_ptr<ProtectedDownload> download = this->Create(error);
    ASSERT_NE(download, nullptr);
    std::filesystem::path directory = download->Path().parent_path();
    download.reset();
    EXPECT_FALSE(std::filesystem::exists(directory));
    download = this->Create(error);
    ASSERT_NE(download, nullptr);
    ASSERT_TRUE(download->Append(ABC, error));
    ASSERT_TRUE(download->Complete(error));
    directory = download->Path().parent_path();
    download.reset();
    EXPECT_FALSE(std::filesystem::exists(directory));
    EXPECT_TRUE(std::filesystem::exists(this->cache_));
}

// 少于、超过精确长度及散列错误均不得获得可启动资格，Preserve 不保留失败文件。
// 入参：无。
// 返回：测试断言。
TEST_F(ProtectedDownloadTest, short_overlong_and_corrupt_payloads_cannot_complete)
{
    UpdateDownloadError error;
    std::unique_ptr<ProtectedDownload> download = this->Create(error);
    ASSERT_NE(download, nullptr);
    ASSERT_TRUE(download->Append(std::span(ABC).first(2), error));
    EXPECT_FALSE(download->Complete(error));
    EXPECT_EQ(error, UpdateDownloadError::SizeMismatch);
    download->Preserve();
    const std::filesystem::path shortFile = download->Path();
    download.reset();
    EXPECT_FALSE(std::filesystem::exists(shortFile));
    download = this->Create(error);
    ASSERT_NE(download, nullptr);
    ASSERT_TRUE(download->Append(ABC, error));
    EXPECT_FALSE(download->Append(std::span(ABC).first(1), error));
    EXPECT_EQ(error, UpdateDownloadError::SizeMismatch);
    EXPECT_FALSE(download->Complete(error));
    download = this->Create(error);
    ASSERT_NE(download, nullptr);
    const std::array<std::byte, 3> corrupt{};
    ASSERT_TRUE(download->Append(corrupt, error));
    EXPECT_FALSE(download->Complete(error));
    EXPECT_EQ(error, UpdateDownloadError::HashMismatch);
}

// 非法远程路径、无效散列和发行上限外长度在创建文件前拒绝。
// 入参：无。
// 返回：测试断言。
TEST_F(ProtectedDownloadTest, invalid_request_metadata_creates_no_download)
{
    UpdateDownloadError error;
    EXPECT_EQ(ProtectedDownload::Create(this->cache_, 0, ABC_SHA256, error), nullptr);
    EXPECT_EQ(ProtectedDownload::Create(this->cache_, 512ULL * 1024 * 1024 + 1, ABC_SHA256, error), nullptr);
    EXPECT_EQ(ProtectedDownload::Create(this->cache_, 3, "invalid", error), nullptr);
    EXPECT_EQ(ProtectedDownload::Create(L"relative", 3, ABC_SHA256, error), nullptr);
    EXPECT_EQ(ProtectedDownload::Create(L"\\\\server\\share", 3, ABC_SHA256, error), nullptr);
    EXPECT_FALSE(std::filesystem::exists(this->cache_));
}

// 活跃事务和已启动包的读占用都被保留；释放占用后的显式清理按句柄回收。
// 入参：无。
// 返回：测试断言。
TEST_F(ProtectedDownloadTest, explicit_cleanup_retains_active_files_then_cleans_released_owned_cache)
{
    UpdateDownloadError error;
    std::unique_ptr<ProtectedDownload> download = this->Create(error);
    ASSERT_NE(download, nullptr);
    ASSERT_TRUE(download->Append(ABC, error));
    ASSERT_TRUE(download->Complete(error));
    const std::filesystem::path file = download->Path();
    ProtectedDownload::CleanupOwned(this->cache_);
    ASSERT_TRUE(std::filesystem::exists(file));
    download->Preserve();
    download.reset();
    TestHandle active(CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
    ASSERT_NE(active.get(), INVALID_HANDLE_VALUE);
    ProtectedDownload::CleanupOwned(this->cache_);
    EXPECT_TRUE(std::filesystem::exists(file));
    active.reset();
    ProtectedDownload::CleanupOwned(this->cache_);
    EXPECT_FALSE(std::filesystem::exists(file.parent_path()));
}

// 私有请求夹里出现未知内容时不得部分删除，未核验权限的相似名称目录同样保留。
// 入参：无。
// 返回：测试断言。
TEST_F(ProtectedDownloadTest, cleanup_preserves_unknown_contents_and_unverified_directory)
{
    UpdateDownloadError error;
    std::unique_ptr<ProtectedDownload> download = this->Create(error);
    ASSERT_NE(download, nullptr);
    ASSERT_TRUE(download->Append(ABC, error));
    ASSERT_TRUE(download->Complete(error));
    const std::filesystem::path file = download->Path();
    download->Preserve();
    download.reset();
    {
        std::ofstream unknown(file.parent_path() / L"notes.txt");
        unknown << "keep";
    }
    const std::filesystem::path unverified = this->cache_ / (L"request-" + std::wstring(48, L'0'));
    std::filesystem::create_directory(unverified);
    {
        std::ofstream unknown(unverified / L"setup.part");
        unknown << "keep";
    }
    ProtectedDownload::CleanupOwned(this->cache_);
    EXPECT_TRUE(std::filesystem::exists(file));
    EXPECT_TRUE(std::filesystem::exists(file.parent_path() / L"notes.txt"));
    EXPECT_TRUE(std::filesystem::exists(unverified / L"setup.part"));
}

// 目录正在被其他句柄保留 DELETE 权限时立即拒绝路径锚定，不能退化为无保护下载。
// 入参：无。
// 返回：测试断言。
TEST_F(ProtectedDownloadTest, ancestor_delete_access_conflict_fails_immediately)
{
    const TestHandle blocker(CreateFileW(this->root_.c_str(), DELETE | FILE_READ_ATTRIBUTES,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                         FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    ASSERT_NE(blocker.get(), INVALID_HANDLE_VALUE);
    UpdateDownloadError error;
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    EXPECT_EQ(this->Create(error), nullptr);
    EXPECT_EQ(error, UpdateDownloadError::Busy);
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(1));
}

// 目录符号链接不能作为缓存祖先，拒绝后外部目录没有新增请求文件。
// 入参：无。
// 返回：测试断言；系统未授予创建符号链接权限时明确跳过。
TEST_F(ProtectedDownloadTest, reparse_ancestor_is_rejected_without_writing_target)
{
    const std::filesystem::path target = this->root_ / L"outside";
    std::filesystem::create_directory(target);
    if (!CreateSymbolicLinkW(this->cache_.c_str(), target.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY | 0x2))
    {
        if (GetLastError() == ERROR_PRIVILEGE_NOT_HELD || GetLastError() == ERROR_ACCESS_DENIED)
            GTEST_SKIP() << "Environment does not permit symbolic-link creation.";
        FAIL() << "Symbolic-link creation failed: " << GetLastError();
    }
    UpdateDownloadError error;
    EXPECT_EQ(this->Create(error), nullptr);
    EXPECT_EQ(error, UpdateDownloadError::InvalidPath);
    EXPECT_TRUE(std::filesystem::is_empty(target));
    std::filesystem::remove(this->cache_);
}
} // namespace
} // namespace open_st::update_detail
