#pragma once

#include <memory>
#include <string>
#include <windows.h>

namespace open_st
{
class FrozenDesktopFrame;
class CaptureOverlaySession;
class SelectionModel;
class SettingsWindow;
class SelectionOutputRenderer;
class CaptureCompletion;

// 进程级应用协调器：拥有隐藏消息窗口、托盘、全局热键和当前截图会话。
// 捕获与呈现的具体实现由独立模块完成，App 只负责按 Win32 消息驱动它们的生命周期。
class App final
{
  public:
    // 绑定当前进程模块实例，其他系统资源在 Run 中按需创建。
    explicit App(HINSTANCE instance) noexcept;
    // 禁止复制应用协调器，确保窗口、互斥体和托盘资源只有一个所有者。
    App(const App&) = delete;
    // 禁止复制赋值，避免系统资源所有权重复。
    App& operator=(const App&) = delete;
    // 关闭设置/截图会话并按逆序释放进程级系统资源。
    ~App();

    // 复制当前稳定选区；供快捷键及后续结果菜单共用。
    void CopySelection();
    // 保存当前稳定选区；取消或失败保留会话供重试。
    void SaveSelection();

    // 进入主消息循环；参数保留 Win32 入口语义，当前托盘程序不依赖初始显示状态。
    int Run(int showCommand);

  private:
    // 从隐藏消息窗口的用户数据中找回 App，并把系统消息转发给实例处理器。
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    // 处理截图覆盖窗口的绘制、鼠标、键盘、捕获与销毁消息。
    static LRESULT CALLBACK OverlayProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    // 协调隐藏消息窗口收到的热键、托盘命令和进程退出消息。
    LRESULT HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    // 注册并创建不可见的 message-only window，作为进程消息中枢。
    bool CreateMessageWindow();
    // 添加系统托盘图标；失败时仅记录日志，不阻止应用继续运行。
    void AddTrayIcon();
    // 已成功添加托盘图标时将其从通知区域移除。
    void RemoveTrayIcon() noexcept;
    // 语言变化后刷新已存在窗口的标题和托盘提示。
    void RefreshLocalizedUi();
    // 在消息处理完成且没有截图会话时，合并并消费业务读取失败的粗略告警。
    void ReportDataReadWarnings();
    // 在当前光标附近显示截图、设置、关于和退出菜单。
    void ShowTrayMenu();
    // 创建或激活单实例非模态设置窗口。
    void ShowSettings();
    // 捕获冻结桌面帧并建立一次覆盖窗口、渲染器和选区模型会话。
    void StartCapture();
    // 按“当前拖动、已有选区、覆盖窗口”三级语义处理 Esc 或右键取消。
    void CancelSelectionOrClose() noexcept;
    // 释放当前截图会话资源并销毁覆盖窗口。
    void CloseOverlay() noexcept;
    // 显示本地化的程序版本与说明信息。
    void ShowAbout();
    // 使用本地化外壳显示一次截图失败详情。
    void ShowCaptureError(const std::wstring& detail);
    // 以模态 Renderer 显示简单消息，异常或创建失败退回系统提示，守卫阻止托盘和热键重入。
    void ShowSimpleMessage(const std::wstring& message, const std::wstring& title, UINT fallbackFlags);

    // 协调统一完成流程与错误提示；save 为 true 时选择文件目标。
    void CompleteSelection(bool save);

    bool completionBusy_{}; // 包括错误弹窗在内的忙状态。
    bool dialogActive_{}; // 简单模态弹窗及其系统兜底期间禁止重新打开业务入口。
    bool comInitialized_{}; // 仅成功初始化时配对 CoUninitialize。
    std::unique_ptr<SelectionOutputRenderer> outputRenderer_;
    std::unique_ptr<CaptureCompletion> completion_;

    HICON largeIcon_{}; // 自有非共享图标，窗口关闭后由 App 析构释放。
    HICON smallIcon_{}; // 托盘与设置窗口借用，移除后再释放。
    HINSTANCE instance_{};   // 模块实例句柄，不拥有。
    HWND messageWindow_{};   // HWND_MESSAGE：接收热键、托盘和退出消息，不显示界面。
    HANDLE instanceMutex_{}; // 命名互斥体句柄，用于限制每个用户会话只运行一个实例。
    bool trayAdded_{};       // 只有成功加入系统托盘后，析构时才发送 NIM_DELETE。
    bool overlayPreparing_{}; // 创建/显示 HWND 的同步消息期间禁止销毁正在使用的会话记录。
    bool overlayInvalidated_{}; // 准备期间布局或窗口失效时，在同步调用返回后统一回收。

    // frozen frame、renderer 和 selection 仅在一次截图会话中存在；renderer 不把预览帧当作输出源。
    // App 对外头文件使用前置声明，避免把 DirectX 头传播给依赖 application 的模块。
    std::unique_ptr<FrozenDesktopFrame> frozenDesktopFrame_;
    std::unique_ptr<CaptureOverlaySession> overlaySession_;
    std::unique_ptr<SelectionModel> selectionModel_;
    std::unique_ptr<SettingsWindow> settingsWindow_;
};
} // namespace open_st
