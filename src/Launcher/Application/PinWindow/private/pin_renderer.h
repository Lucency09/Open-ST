// 文件职责：声明贴图专用 GPU 渲染器，隐藏交换链、Direct2D 与窗口合成资源。

#pragma once

#include <memory>
#include <string>
#include <windows.h>

namespace open_st
{
class PinImage;
struct PinRendererTestAccess;

class PinRenderer final
{
  public:
    // 创建尚未绑定窗口的渲染器。
    // 入参：无。
    // 返回：无返回值；仅分配内部资源容器，分配失败可抛出异常。
    PinRenderer();
    // 解除窗口合成关联并释放全部图形资源和图像快照。
    // 入参：无。
    // 返回：无返回值；窗口须保持存活至渲染器回收完成。
    ~PinRenderer();
    // 禁止复制唯一拥有的图形资源。
    // 入参：未命名引用为拟复制的渲染器。
    // 返回：无；已删除的函数不能调用。
    PinRenderer(const PinRenderer&) = delete;
    // 禁止复制赋值，避免窗口关联被多个渲染器拥有。
    // 入参：未命名引用为拟赋值的渲染器。
    // 返回：无；已删除的函数不能调用。
    PinRenderer& operator=(const PinRenderer&) = delete;

    // 绑定窗口与图像，上传不透明显示副本并完成首次呈现及合成提交。
    // 入参：window 为借用的有效 HWND；image 为共享不可变原图；error 接收仅供日志的失败诊断。
    // 返回：完整首帧成功时为 true；输入或图形资源失败时为 false 并回收候选资源。
    [[nodiscard]] bool Initialize(HWND window, std::shared_ptr<const PinImage> image, std::wstring& error);
    // 按物理尺寸绘制缩放图像并独立设置窗口不透明度，设备故障时从原图受控重建一次。
    // 入参：width、height 为窗口物理像素尺寸；opacity 为有限的 0～1；error 接收失败诊断。
    // 返回：绘制、呈现及提交均成功时为 true；非法输入或恢复后仍失败时为 false。
    [[nodiscard]] bool Render(int width, int height, float opacity, std::wstring& error);
    // 释放全部图形资源并忘记窗口及原图，可重复调用。
    // 入参：无。
    // 返回：无返回值；对象恢复未初始化状态。
    void Reset() noexcept;

  private:
    friend struct PinRendererTestAccess;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace open_st
