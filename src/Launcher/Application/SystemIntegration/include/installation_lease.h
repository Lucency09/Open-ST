// 安装目录运行与维护互斥协议，复用 Common 的文件租约。
#pragma once
#include <Windows.h>
#include <file_lease.h>
#include <filesystem>

namespace open_st
{
enum class InstallationLeaseState
{
    Acquired,
    Busy,
    Failed
};
struct InstallationLeaseStatus
{
    InstallationLeaseState state{InstallationLeaseState::Failed};
    DWORD error{};
};
class InstallationLease
{
  public:
    // 为程序生命周期或维护操作取得目录租约，不等待竞争者。
    // 入参：executableDirectory 为程序绝对目录；maintenance 为是否独占维护。
    // 返回：获取结果及系统错误，失败不访问业务资源。
    [[nodiscard]] InstallationLeaseStatus Acquire(const std::filesystem::path& executableDirectory,
                                                  bool maintenance = false);
    // 释放租约，调用方须先完成所有设置与日志退出处理。
    // 入参：无。
    // 返回：无。
    void Reset() noexcept;

  private:
    FileLease lease_;
};
} // namespace open_st
