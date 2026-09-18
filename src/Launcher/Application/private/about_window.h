// 定义关于窗口的业务适配输入，所有原生窗口机制复用 WindowRenderer。
#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <windows.h>
namespace open_st
{
class WindowRenderer;
struct AboutWindowOptions
{
    HWND owner{};
    HICON icon{};
    std::string version, distribution;
    std::filesystem::path cacheRoot;
    std::function<bool()> stopping;
    std::function<bool(MSG&)> processThreadMessage;
    std::function<bool(const std::filesystem::path&, HWND)> launchInstaller;
    std::function<bool(const std::wstring&)> openPage;
    WindowRenderer** activeRenderer{};
};
// 显示版本与检查更新入口，确认前不下载，关闭取消未交接任务。
// 入参：options 为借用至返回的宿主信息和窄动作。返回：窗口正常运行时 true。
bool ShowAboutWindow(const AboutWindowOptions& options) noexcept;
} // namespace open_st
