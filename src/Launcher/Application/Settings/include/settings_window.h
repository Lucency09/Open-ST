// 声明设置窗口及宿主注入的本地化、启动项应用和忙状态回调。

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
    // 入参：std::string_view 为只在调用期间借用的界面文本键。
    // 返回：该键在当前语言下的宽字符串文案。
    std::function<std::wstring(std::string_view)> text;
    // 查询当前已生效语言，作为设置缺失时的选中项。
    // 入参：无。
    // 返回：宿主当前已生效的语言代码。
    std::function<std::string()> currentLanguage;
    // 每次展开下拉框时重新查询资源实际提供的语言代码。
    // 入参：无。
    // 返回：资源当前声明的可用语言代码列表。
    std::function<std::vector<std::string>()> availableLanguages;
    // 通知宿主设置窗口进入或退出确认、保存及系统应用的忙状态。
    // 入参：bool 表示是否忙碌；true 为进入，false 为退出。
    // 返回：无返回值；通知异常由窗口捕获，不影响交互恢复。
    std::function<void(bool)> busyChanged;
    // 在意图保存后应用当前用户的系统启动入口。
    // 入参：bool 为已保存的自启启用意图。
    // 返回：系统启动入口已按目标生效时为 true，否则为 false。
    std::function<bool(bool)> startupApplied;
    // 动态查询并本地化系统启动入口状态。
    // 入参：无。
    // 返回：当前启动入口状态的本地化宽字符串。
    std::function<std::wstring()> startupStatus;
    // 在语言持久化成功后通知宿主应用运行期语言。
    // 入参：std::string_view 为已保存的语言代码，只在本次调用期间有效。
    // 返回：运行期语言已生效时为 true，否则为 false。
    std::function<bool(std::string_view)> languageApplied;
};

// 管理进程内唯一的非模态设置窗口；关闭窗口不会结束应用消息循环。
class SettingsWindow final
{
  public:
    // 创建空窗口容器，实际 HWND 在 Show 时创建。
    // 入参：无显式入参。
    // 返回：无返回值。
    SettingsWindow();
    // 禁止复制窗口所有权。
    // 入参：未命名 SettingsWindow 引用为被禁止复制或赋值的窗口所有权来源。
    // 返回：该操作已删除；尝试调用会导致编译错误。
    SettingsWindow(const SettingsWindow&) = delete;
    // 禁止复制赋值，避免重复销毁窗口资源。
    // 入参：未命名 SettingsWindow 引用为被禁止复制或赋值的窗口所有权来源。
    // 返回：该操作已删除；尝试调用会导致编译错误。
    SettingsWindow& operator=(const SettingsWindow&) = delete;
    // 关闭窗口并释放其持有的回调和控件资源。
    // 入参：无显式入参。
    // 返回：无返回值。
    ~SettingsWindow();

    // 在 UI 线程创建并显示设置窗口；窗口已存在时激活原窗口。
    // 入参：instance 为当前程序模块句柄；callbacks 为移交给窗口的宿主回调集合，捕获对象须活到窗口释放回调。
    // 返回：已有窗口激活或新建显示成功为 true；参数、布局、绑定或创建失败为 false。
    [[nodiscard]] bool Show(HINSTANCE instance, SettingsWindowCallbacks callbacks) noexcept;
    // 处理非模态窗口的 Tab、Enter 和 Escape 键盘导航。
    // 入参：message 为应用消息循环取得的待处理消息。
    // 返回：消息已被窗口键盘导航处理为 true，否则为 false。
    [[nodiscard]] bool ProcessDialogMessage(MSG& message) const noexcept;
    // 空闲时同步关闭并释放回调；忙时仅请求延迟关闭，回调保留至后续空闲 Close 或销毁。
    // 入参：无显式入参。
    // 返回：无返回值。
    void Close() noexcept;
    // 重新获取当前语言的布局、状态和字段错误文字。
    // 入参：无显式入参。
    // 返回：无返回值。
    void RefreshTexts() noexcept;
    // 取得当前设置窗口句柄，供宿主设置模态窗口的 owner。
    // 入参：无显式入参。
    // 返回：借用的当前设置窗口 HWND；未打开时为 nullptr，关闭后失效，不得由调用方销毁。
    [[nodiscard]] HWND NativeHandle() const noexcept;
    // 返回窗口是否仍然存在。
    // 入参：无显式入参。
    // 返回：实际设置窗口存在时为 true，否则为 false。
    [[nodiscard]] bool IsOpen() const noexcept;

  private:
    friend struct SettingsWindowTestAccess;
    class Impl;

    std::unique_ptr<Impl> impl_;
};
} // namespace open_st
