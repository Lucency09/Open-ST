// 验证安装目录运行共享、维护独占及安装器启动边界。
#include <chrono>
#include <filesystem>
#include <gtest/gtest.h>
#include <installation_lease.h>
#include <installer_launcher.h>

namespace open_st
{
namespace
{
class InstallationLeaseTest : public testing::Test
{
  protected:
    std::filesystem::path directory_;
    // 在系统临时目录建立仅本用例拥有的路径。
    // 入参：无。
    // 返回：无。
    void SetUp() override
    {
        static unsigned sequence{};
        this->directory_ = std::filesystem::temp_directory_path() /
                           (L"Open-ST-installation-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                            std::to_wstring(++sequence));
        std::filesystem::create_directories(this->directory_);
    }
    // 仅清理本用例目录，租约局部变量已释放。
    // 入参：无。
    // 返回：无。
    void TearDown() override
    {
        std::error_code error;
        std::filesystem::remove_all(this->directory_, error);
    }
};
// 共享运行可共存，独占维护立即失败，最后运行退出后维护成功。
// 入参：无。
// 返回：测试断言。
TEST_F(InstallationLeaseTest, shared_running_blocks_maintenance_until_all_release)
{
    InstallationLease first, second, maintenance;
    ASSERT_EQ(first.Acquire(this->directory_).state, InstallationLeaseState::Acquired);
    ASSERT_EQ(second.Acquire(this->directory_).state, InstallationLeaseState::Acquired);
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(maintenance.Acquire(this->directory_, true).state, InstallationLeaseState::Busy);
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(1));
    first.Reset();
    EXPECT_EQ(maintenance.Acquire(this->directory_, true).state, InstallationLeaseState::Busy);
    second.Reset();
    EXPECT_EQ(maintenance.Acquire(this->directory_, true).state, InstallationLeaseState::Acquired);
}
// 维护期间新运行与其他维护均立即拒绝，其他目录独立。
// 入参：无。
// 返回：测试断言。
TEST_F(InstallationLeaseTest, maintenance_blocks_running_but_other_directory_is_independent)
{
    InstallationLease maintenance, running, second;
    ASSERT_EQ(maintenance.Acquire(this->directory_, true).state, InstallationLeaseState::Acquired);
    EXPECT_EQ(running.Acquire(this->directory_).state, InstallationLeaseState::Busy);
    EXPECT_EQ(second.Acquire(this->directory_ / L"other").state, InstallationLeaseState::Acquired);
    maintenance.Reset();
    EXPECT_EQ(running.Acquire(this->directory_).state, InstallationLeaseState::Acquired);
    EXPECT_TRUE(std::filesystem::exists(this->directory_ / L"data/.coordination/install.lock"));
}
// 非本地绝对路径和目录穿越不能创建协调文件。
// 入参：无。
// 返回：测试断言。
TEST_F(InstallationLeaseTest, invalid_roots_are_rejected)
{
    InstallationLease lease;
    EXPECT_EQ(lease.Acquire(L"relative").state, InstallationLeaseState::Failed);
    EXPECT_EQ(lease.Acquire(L"\\\\server\\share").state, InstallationLeaseState::Failed);
    EXPECT_EQ(lease.Acquire(this->directory_ / L"../outside").state, InstallationLeaseState::Failed);
}
// 系统启动接口拒绝非 EXE、相对、远程及缺失路径，避免调用 shell。
// 入参：无。
// 返回：测试断言。
TEST_F(InstallationLeaseTest, launcher_rejects_invalid_or_absent_paths)
{
    EXPECT_EQ(LaunchInstaller(L"relative.exe").error, ERROR_INVALID_PARAMETER);
    EXPECT_EQ(LaunchInstaller(L"https://example.com/setup.exe").error, ERROR_INVALID_PARAMETER);
    EXPECT_EQ(LaunchInstaller(L"\\\\server\\share\\setup.exe").error, ERROR_INVALID_PARAMETER);
    EXPECT_EQ(LaunchInstaller(this->directory_ / L"setup.txt").error, ERROR_INVALID_PARAMETER);
    EXPECT_FALSE(LaunchInstaller(this->directory_ / L"absent.exe").started);
}
} // namespace
} // namespace open_st
