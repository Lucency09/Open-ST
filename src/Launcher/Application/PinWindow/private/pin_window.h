// 定义单贴图原生窗口及其借用管理器关系，所有权始终位于 Manager。
#pragma once
#include "pin_interaction_model.h"
#include <memory>
#include <pin_window_manager.h>

namespace open_st
{
class PinRenderer;
class PinWindow final
{
  public:
    // 建立单图记录。
    // 入参：manager 为借用管理器、id 为唯一 ID、image 为自有图像。
    // 返回：无返回值。
    PinWindow(PinWindowManager& manager, PinId id, std::shared_ptr<const PinImage> image);
    // 释放渲染资源与 HWND。
    // 入参：无。
    // 返回：无返回值，不允许在窗口入口自身执行期间析构。
    ~PinWindow();
    // 禁止复制 HWND。
    // 入参：源窗口。
    // 返回：调用在编译期拒绝。
    PinWindow(const PinWindow&) = delete;
    // 禁止复制赋值 HWND。
    // 入参：源窗口。
    // 返回：调用在编译期拒绝。
    PinWindow& operator=(const PinWindow&) = delete;
    // 创建隐藏窗口并提交首帧。
    // 入参：instance 为应用模块实例；origin 为屏幕物理像素左上角；error 为失败诊断输出。
    // 返回：隐藏窗口和首帧提交成功 true；创建或图形初始化失败 false。
    [[nodiscard]] bool Prepare(HINSTANCE instance, POINT origin, std::wstring& error);
    // 重绘当前图像与透明度。
    // 入参：无。
    // 返回：绘制成功 true；渲染器恢复仍失败时报告本地化错误并申请关闭，返回 false。
    [[nodiscard]] bool Redraw() noexcept;
    // 将矩形应用到窗口而不提层或激活。
    // 入参：rectangle 为屏幕物理像素。
    // 返回：无返回值。
    void ApplyRectangle(RECT rectangle) noexcept;
    // 结束鼠标拖动并释放捕获。
    // 入参：无。
    // 返回：无返回值。
    void CancelDrag() noexcept;
    // 在可达位置显示原生右键菜单。
    // 入参：point 为屏幕物理像素。
    // 返回：无返回值，业务在菜单结束后执行。
    void ShowMenu(POINT point) noexcept;
    // 返回记录 ID。
    // 入参：无。
    // 返回：进程内不复用的稳定标识。
    [[nodiscard]] PinId Id() const noexcept
    {
        return this->id_;
    }
    // 返回借用 HWND。
    // 入参：无。
    // 返回：当前窗口的借用 HWND，未创建或已经销毁时为 nullptr。
    [[nodiscard]] HWND Handle() const noexcept
    {
        return this->window_;
    }
    // 返回共享独立图像。
    // 入参：无。
    // 返回：不借用截图会话的只读快照。
    [[nodiscard]] std::shared_ptr<const PinImage> Image() const noexcept
    {
        return this->image_;
    }

    bool closing{};
    PinInteractionState state{};

  private:
    friend class PinWindowManager;
    PinWindowManager& manager_;
    PinId id_{};
    std::shared_ptr<const PinImage> image_;
    std::unique_ptr<PinRenderer> renderer_;
    HWND window_{};
    bool dragging_{};
    bool rendering_{};
    bool failed_{};
    bool prepared_{};
    POINT dragPoint_{};
    RECT dragRectangle_{};
    // 在已保护窗口记录的作用域内绘制。
    // 入参：无。
    // 返回：渲染器绘制或受控恢复成功 true；最终失败 false，并申请关闭失效贴图。
    [[nodiscard]] bool RedrawProtected() noexcept;
    // 在已保护窗口记录的作用域内运行菜单。
    // 入参：point 为屏幕物理像素位置。
    // 返回：无返回值，菜单结束后执行有效业务命令。
    void ShowMenuProtected(POINT point) noexcept;
    // 路由原生窗口消息并守护记录生命周期。
    // 入参：window 为目标 HWND；message 为消息编号；wParam/lParam 为消息约定的参数。
    // 返回：Win32 消息结果，未知消息由默认窗口过程处理。
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) noexcept;
    // 处理单张窗口输入与重绘。
    // 入参：message 为消息编号；wParam/lParam 为当前窗口的消息参数。
    // 返回：输入、绘制或窗口生命周期消息的 Win32 处理结果。
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
};
} // namespace open_st
