// 声明当前用户启动项的归属查询、启停与路径修复接口。

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
    // 创建只管理当前用户单个启动项值的注册表访问对象。
    // 入参：executablePath 为程序绝对路径；registryKey 为当前用户下的启动项键路径；valueName 为只允许操作的单个值名。
    // 返回：无返回值。
    explicit StartupRegistration(std::wstring executablePath,
                                 std::wstring registryKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                                 std::wstring valueName = L"Open-ST");
    // 查询指定启动项是否属于当前程序，并区分旧路径、冲突和系统失败。
    // 入参：无显式入参。
    // 返回：启动项缺失、属于当前路径、属于旧路径、外部冲突或系统失败的状态及错误码。
    [[nodiscard]] StartupStatus Query() const;
    // 根据已确认的启用意图安全增删或修复启动项，并重新查询核验结果。
    // 入参：enabled 为期望启停状态；allowPathRepair 为是否允许将已识别的旧程序路径修复为当前路径。
    // 返回：操作后的核验状态；拒绝修复或外部冲突时保留原状态，系统操作失败附带错误码。
    [[nodiscard]] StartupStatus Apply(bool enabled, bool allowPathRepair = false) const;

  private:
    std::wstring executablePath_;
    std::wstring registryKey_;
    std::wstring valueName_;
};
} // namespace open_st
