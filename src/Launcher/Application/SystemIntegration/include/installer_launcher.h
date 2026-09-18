// 提供已验证本地安装包的窄系统启动边界。
#pragma once
#include <Windows.h>
#include <filesystem>

namespace open_st
{
struct InstallerLaunchResult
{
    bool started{};
    DWORD error{};
};
// 请求系统打开安装包，调用方须在交接完成前继续持有文件保护句柄。
// 入参：verifiedLocalInstaller 为已校验绝对本地 EXE 路径；owner 为提示所有者。
// 返回：系统接受启动时 started 为 true，否则携带原始错误。
[[nodiscard]] InstallerLaunchResult LaunchInstaller(const std::filesystem::path& verifiedLocalInstaller,
                                                    HWND owner = nullptr);
} // namespace open_st
