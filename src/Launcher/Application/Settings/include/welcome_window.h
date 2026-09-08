#pragma once
#include <settings_window.h>

namespace open_st
{
// 首次告知窗口；只在确认后保存选择并请求上级应用系统入口。
class WelcomeWindow final
{
  public:
    // 创建空欢迎窗口容器。
    WelcomeWindow();
    // 释放窗口和捕获回调。
    ~WelcomeWindow();
    // 禁止复制窗口所有权。
    WelcomeWindow(const WelcomeWindow&) = delete;
    // 禁止复制赋值以避免重复释放句柄。
    WelcomeWindow& operator=(const WelcomeWindow&) = delete;
    // 等待用户完成或退出；返回 true 表示保存和系统操作均成功。
    [[nodiscard]] bool ShowModal(HINSTANCE instance, SettingsWindowCallbacks callbacks,
                                 std::function<bool(MSG&)> processThreadMessage = {}) noexcept;
    // 区分窗口初始化或消息循环失败与用户正常退出。
    [[nodiscard]] bool Failed() const noexcept;
    // 重复启动时激活当前欢迎窗口。
    void Activate() noexcept;
    // 语言资源更新时重新取得文字和状态。
    void RefreshTexts() noexcept;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace open_st
