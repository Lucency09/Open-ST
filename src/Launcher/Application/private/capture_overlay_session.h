// 声明截图覆盖会话及每屏窗口资源，集中约束 HWND 和渲染器生命周期。

#pragma once

#include <overlay_renderer.h>

#include <memory>
#include <string>
#include <vector>
#include <windows.h>

namespace open_st
{
// 每屏窗口仅拥有呈现资源；原生冻结帧和全局选区由 App 统一拥有。
struct CaptureOverlayOutput final
{
    HWND window{};
    std::unique_ptr<OverlayRenderer> renderer;
};

// 管理同一截图会话的多个 HWND；销毁前解除回调绑定，避免重复释放共享状态。
class CaptureOverlaySession final
{
  public:
    // 创建空的截图覆盖会话，供捕获完成后登记各屏窗口。
    // 入参：无。
    // 返回：构造函数无返回值；初始不持有窗口。
    CaptureOverlaySession() = default;
    // 销毁截图覆盖会话并释放全部已登记窗口和渲染资源。
    // 入参：无。
    // 返回：析构函数无返回值。
    ~CaptureOverlaySession();
    // 禁止复制构造，确保截图覆盖窗口和渲染资源只由原对象管理。
    // 入参：未命名的同类型 const 引用：拟复制的源对象。
    // 返回：函数已删除，调用会导致编译错误，无运行时返回结果。
    CaptureOverlaySession(const CaptureOverlaySession&) = delete;
    // 禁止复制赋值，避免截图覆盖窗口和渲染资源出现多个所有者。
    // 入参：未命名的同类型 const 引用：拟复制的源对象。
    // 返回：函数已删除，调用会导致编译错误，无运行时返回结果。
    CaptureOverlaySession& operator=(const CaptureOverlaySession&) = delete;
    // 把截图覆盖窗口交给当前会话统一管理。
    // 入参：window：交由会话负责销毁的窗口句柄。
    // 返回：新增输出记录的引用；会话关闭前地址稳定，不因后续登记而移动。
    CaptureOverlayOutput& Add(HWND window);
    // 查找窗口在当前截图会话中的输出记录。
    // 入参：window：待查找的截图覆盖窗口句柄。
    // 返回：匹配记录的借用指针；不存在时返回 nullptr。
    [[nodiscard]] CaptureOverlayOutput* Find(HWND window) const noexcept;
    // 选择应接收截图键盘输入的覆盖窗口。
    // 入参：无。
    // 返回：光标所在覆盖窗口的借用句柄；查询失败或未命中时取首个窗口，空会话返回 nullptr。
    [[nodiscard]] HWND ActivationWindow() const noexcept;
    // 查找指定显示器对应的截图覆盖窗口。
    // 入参：monitor：目标显示器句柄。
    // 返回：匹配窗口的借用句柄；未找到时返回 nullptr，不转移窗口所有权。
    [[nodiscard]] HWND WindowForMonitor(HMONITOR monitor) const noexcept;
    // 请求当前会话的所有覆盖窗口重绘，使跨屏选区显示保持同步。
    // 入参：无。
    // 返回：无返回值；仅标记重绘区域，实际绘制由窗口消息驱动。
    void Invalidate() const noexcept;
    // 解除当前截图会话占用的鼠标捕获。
    // 入参：无。
    // 返回：无返回值；其他窗口持有的鼠标捕获保持不变。
    void ReleaseMouse() const noexcept;
    // 显示已准备好的全部截图覆盖窗口，并激活光标所在窗口。
    // 入参：无。
    // 返回：无返回值；请求前台、键盘焦点及重绘，不报告系统拒绝激活的结果。
    void Show() const noexcept;
    // 在模态操作结束后恢复截图覆盖窗口的焦点及选区显示。
    // 入参：无。
    // 返回：无返回值；无可激活窗口时不操作。
    void RestoreFocus() const noexcept;
    // 更新当前会话全部截图覆盖窗口的标题。
    // 入参：title：由宿主提供的本地化标题，本次调用借用。
    // 返回：无返回值；标题写入各窗口，不保存字符串引用。
    void SetTitle(const std::wstring& title) const noexcept;
    // 关闭整个截图覆盖会话，先解绑窗口回调再释放渲染器和窗口。
    // 入参：无。
    // 返回：无返回值；清空输出记录，可重复调用。
    void Close() noexcept;

  private:
    std::vector<std::unique_ptr<CaptureOverlayOutput>> outputs_;
};
} // namespace open_st
