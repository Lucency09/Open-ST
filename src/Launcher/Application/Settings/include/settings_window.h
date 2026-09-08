#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>

namespace open_st
{
// 由上级 Application 提供本地化查询及保存通知，Settings 不依赖同级本地化模块。
struct SettingsWindowCallbacks final
{
    // 可选借用图标，调用者保持有效至窗口关闭；Settings 不销毁句柄。
    HICON largeIcon{};
    HICON smallIcon{};
    // 按调用方请求的动态文本键查询当前语言文本。
    std::function<std::wstring(std::string_view)> text;
    // 查询当前已生效语言，作为设置缺失时的选中项。
    std::function<std::string()> currentLanguage;
    // 每次展开下拉框时重新查询资源实际提供的语言代码。
    std::function<std::vector<std::string>()> availableLanguages;
    // 通知宿主确认、保存及副作用执行期间的忙状态；通知异常不影响窗口恢复。
    std::function<void(bool)> busyChanged;
    // 保存后应用当前用户启动入口，返回是否已生效。
    std::function<bool(bool)> startupApplied;
    // 动态查询并本地化系统启动入口状态。
    std::function<std::wstring()> startupStatus;
    // 仅持久化成功后通知上级应用语言；返回是否生效，参数只在回调期间有效。
    std::function<bool(std::string_view)> languageApplied;
};

// 管理进程内唯一的非模态设置窗口；关闭窗口不会结束应用消息循环。
class SettingsWindow final
{
  public:
    // 创建空窗口容器，实际 HWND 在 Show 时创建。
    SettingsWindow();
    // 禁止复制窗口所有权。
    SettingsWindow(const SettingsWindow&) = delete;
    // 禁止复制赋值，避免重复销毁窗口资源。
    SettingsWindow& operator=(const SettingsWindow&) = delete;
    // 关闭窗口并释放其持有的回调和控件资源。
    ~SettingsWindow();

    // 创建或激活窗口；新窗口要求全部回调有效，其捕获对象必须活到 Close 完成。
    [[nodiscard]] bool Show(HINSTANCE instance, SettingsWindowCallbacks callbacks) noexcept;
    // 处理非模态窗口的 Tab、Enter 和 Escape 键盘导航。
    [[nodiscard]] bool ProcessDialogMessage(MSG& message) const noexcept;
    // 同步关闭窗口并释放回调，允许重复调用。
    void Close() noexcept;
    // 重新获取当前语言的布局、状态和字段错误文字。
    void RefreshTexts() noexcept;
    // 借用当前窗口句柄，供宿主指定模态 owner；关闭后立即失效，不得销毁。
    [[nodiscard]] HWND NativeHandle() const noexcept;
    // 返回窗口是否仍然存在。
    [[nodiscard]] bool IsOpen() const noexcept;

  private:
    friend struct SettingsWindowTestAccess;
    class Impl;

    std::unique_ptr<Impl> impl_;
};
} // namespace open_st
