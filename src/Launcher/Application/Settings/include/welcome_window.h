// 声明首次启动欢迎窗口，确认后提交选择并通过宿主应用启动项。

#pragma once
#include <settings_window.h>

namespace open_st
{
// 首次告知窗口；只在确认后保存选择并请求上级应用系统入口。
class WelcomeWindow final
{
  public:
    // 创建空欢迎窗口容器。
    // 入参：无显式入参。
    // 返回：无返回值。
    WelcomeWindow();
    // 释放窗口和捕获回调。
    // 入参：无显式入参。
    // 返回：无返回值。
    ~WelcomeWindow();
    // 禁止复制窗口所有权。
    // 入参：未命名 WelcomeWindow 引用为被禁止复制或赋值的窗口所有权来源。
    // 返回：该操作已删除；尝试调用会导致编译错误。
    WelcomeWindow(const WelcomeWindow&) = delete;
    // 禁止复制赋值以避免重复释放句柄。
    // 入参：未命名 WelcomeWindow 引用为被禁止复制或赋值的窗口所有权来源。
    // 返回：该操作已删除；尝试调用会导致编译错误。
    WelcomeWindow& operator=(const WelcomeWindow&) = delete;
    // 显示欢迎表单并运行模态消息循环，直到确认完成或用户退出。
    // 入参：instance 为程序模块句柄；callbacks 为宿主回调集合；processThreadMessage 为可选线程消息处理器，返回 true 表示已消费。
    // 返回：欢迎确认已持久化且系统自启应用成功时为 true；用户取消或窗口失败为 false，Failed 可区分窗口故障。
    [[nodiscard]] bool ShowModal(HINSTANCE instance, SettingsWindowCallbacks callbacks,
                                 std::function<bool(MSG&)> processThreadMessage = {}) noexcept;
    // 区分窗口初始化或消息循环失败与用户正常退出。
    // 入参：无显式入参。
    // 返回：本次窗口初始化或模态循环失败为 true；正常执行或用户取消为 false。
    [[nodiscard]] bool Failed() const noexcept;
    // 重复启动时激活当前欢迎窗口。
    // 入参：无显式入参。
    // 返回：无返回值。
    void Activate() noexcept;
    // 语言资源更新时重新取得文字和状态。
    // 入参：无显式入参。
    // 返回：无返回值。
    void RefreshTexts() noexcept;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace open_st
