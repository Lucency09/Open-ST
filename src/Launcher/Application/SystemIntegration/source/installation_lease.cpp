// 在公共非阻塞文件租约上定义固定安装协调路径。
#include <installation_lease.h>

namespace open_st
{
// 拒绝非常规根目录，复用 Common 在逐级锚定后创建协调目录。
// 入参：executableDirectory 为程序目录；maintenance 为独占维护模式。
// 返回：成功、占用或明确系统失败。
InstallationLeaseStatus InstallationLease::Acquire(const std::filesystem::path& executableDirectory, bool maintenance)
{
    this->Reset();
    if (!executableDirectory.is_absolute() || executableDirectory.native().starts_with(L"\\\\"))
        return {InstallationLeaseState::Failed, ERROR_INVALID_PARAMETER};
    for (const auto& component : executableDirectory)
    {
        if (component == L"..")
            return {InstallationLeaseState::Failed, ERROR_INVALID_PARAMETER};
    }
    FileLeaseError error;
    if (this->lease_.TryAcquire(executableDirectory / L"data" / L".coordination" / L"install.lock",
                                maintenance ? FileLeaseMode::Exclusive : FileLeaseMode::Shared, &error, true))
        return {InstallationLeaseState::Acquired, ERROR_SUCCESS};
    return {error.code == FileLeaseErrorCode::Busy ? InstallationLeaseState::Busy : InstallationLeaseState::Failed,
            error.systemCode};
}
// 只释放租约句柄，稳定锁文件留在原位。
// 入参：无。
// 返回：无。
void InstallationLease::Reset() noexcept
{
    this->lease_.Reset();
}
} // namespace open_st
