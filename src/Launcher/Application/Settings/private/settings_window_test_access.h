// 声明仅测试使用的设置窗口句柄和确认回调注入入口。

#pragma once
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <windows.h>

namespace open_st
{
class SettingsWindow;
// 仅由镜像测试访问，不进入产品公开头文件集。
struct SettingsWindowTestAccess final
{
    // 返回当前借用窗口，关闭后失效。
    // 入参：window 为要检查的设置窗口对象。
    // 返回：借用的原生窗口 HWND；未打开时为 nullptr，关闭后失效。
    static HWND NativeHandle(const SettingsWindow& window) noexcept;
    // 替换确认边界以测试恢复和重载，不启动模态提示。
    // 入参：window 为被测设置窗口；confirmation 为确认替身，返回 true 接受、false 取消；捕获对象须保持有效。
    // 返回：无返回值。
    static void SetConfirmation(SettingsWindow& window, std::function<bool()> confirmation);
    // 通过与录入相同的业务验证更新快捷键草稿，不产生注册或持久化。
    // 入参：window 为被测设置窗口；value 为规范快捷键 token。
    // 返回：草稿接受时为 true，无效值或窗口不可编辑时为 false。
    static bool ChangeHotkey(SettingsWindow& window, std::string_view value);
    // 读取快捷键草稿以检查恢复默认和取消的边界。
    // 入参：window 为被测设置窗口。
    // 返回：当前快捷键草稿；窗口无有效编辑会话时为空。
    static std::optional<std::string> ReadHotkey(const SettingsWindow& window);
};
} // namespace open_st
