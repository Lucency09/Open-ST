#pragma once
#include <Windows.h>
#include <string>

namespace open_st
{
enum class StartupState
{
    Missing,
    CurrentPath,
    OtherPath,
    Conflict,
    Failed
};
struct StartupStatus
{
    StartupState state{StartupState::Failed};
    DWORD error{};
};
class StartupRegistration
{
  public:
    // 指定程序绝对路径；可注入测试专属注册表键，始终只操作当前用户的一个值。
    explicit StartupRegistration(std::wstring executablePath,
                                 std::wstring registryKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                                 std::wstring valueName = L"Open-ST");
    // 查询入口归属，不修改系统；旧位置与无法确认归属分别返回。
    [[nodiscard]] StartupStatus Query() const;
    // 启停后重新读取核验；旧位置的启用修复须由调用者确认。
    [[nodiscard]] StartupStatus Apply(bool enabled, bool allowPathRepair = false) const;

  private:
    std::wstring executablePath_;
    std::wstring registryKey_;
    std::wstring valueName_;
};
} // namespace open_st
