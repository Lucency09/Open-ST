#pragma once
#include <functional>
#include <windows.h>

namespace open_st
{
class SettingsWindow;
// 仅由镜像测试访问，不进入产品公开头文件集。
struct SettingsWindowTestAccess final
{
    // 返回当前借用窗口，关闭后失效。
    static HWND NativeHandle(const SettingsWindow& window) noexcept;
    // 替换确认边界以测试恢复和重载，不启动模态提示。
    static void SetConfirmation(SettingsWindow& window, std::function<bool()> confirmation);
};
} // namespace open_st
