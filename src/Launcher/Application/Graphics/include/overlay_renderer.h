// 文件职责：声明截图遮罩渲染器，接收不可变预览和选区快照并管理窗口图形资源。

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <windows.h>

namespace open_st
{
struct SelectionSnapshot;
struct OutputPreviewFrame;
struct OverlayRendererTestAccess;

// 使用 D3D11 + DXGI 交换链 + Direct2D 1.1 设备上下文呈现截图覆盖窗口。
// Windows/DirectX COM 对象隐藏在 Impl 中，避免把大量系统头暴露给调用模块。
class OverlayRenderer final
{
  public:
    // 创建覆盖窗口渲染器的内部资源容器，延迟到首次初始化时建立图形设备。
    // 入参：无。
    // 返回：无返回值；构造后的对象尚未建立图形设备，内存分配失败可抛出异常。
    OverlayRenderer();
    // 销毁覆盖渲染器并释放其拥有的图形资源。
    // 入参：无。
    // 返回：无返回值；析构完成对应资源清理。
    ~OverlayRenderer();

    // 禁止复制渲染器，确保 PImpl 中的图形资源只有一个所有者。
    // 入参：未命名 const OverlayRenderer 引用：拟复制的源对象；该操作被禁止。
    // 返回：无；函数已删除，尝试调用会产生编译错误。
    OverlayRenderer(const OverlayRenderer&) = delete;
    // 禁止复制赋值，避免多个对象共享同一组 COM 资源。
    // 入参：未命名 const OverlayRenderer 引用：拟复制的源对象；该操作被禁止。
    // 返回：无；函数已删除，尝试调用会产生编译错误。
    OverlayRenderer& operator=(const OverlayRenderer&) = delete;

    // 为指定截图覆盖窗口建立绘制资源并上传该屏不可变预览。
    // 入参：window：借用的覆盖窗口句柄；frame：调用期间有效的单屏预览，上传后不再借用其像素；configuredBorderColor：调用期间借用的可选 #RRGGBB
    // 边框色，缺失或非法用黑色；errorMessage：输出参数，接收失败诊断。
    // 返回：窗口、预览、显示身份和图形资源全部有效时为 true；输入校验或初始化失败时为 false。
    [[nodiscard]] bool Initialize(HWND window, const OutputPreviewFrame& frame,
                                   std::optional<std::string_view> configuredBorderColor,
                                   std::wstring& errorMessage);

    // 根据覆盖窗口客户区大小重建交换链绘制目标。
    // 入参：width、height：新的客户区物理像素宽高；errorMessage：输出参数，失败时写入诊断。
    // 返回：目标重建成功或无交换链/零尺寸无需操作时为 true；ResizeBuffers 或目标位图重建失败时为 false。
    [[nodiscard]] bool Resize(unsigned int width, unsigned int height, std::wstring& errorMessage);

    // 将冻结桌面预览、选区外暗层、边框和控制点绘制到覆盖窗口并呈现。
    // 入参：snapshot：按值传入的虚拟桌面物理像素选区状态；errorMessage：输出参数，接收失效或绘制失败原因。
    // 返回：绘制并呈现成功时为 true；未初始化、显示状态过期或图形调用失败时为 false。
    [[nodiscard]] bool Render(SelectionSnapshot snapshot, std::wstring& errorMessage);
    // 结束当前覆盖渲染会话并释放设备、交换链和绘制资源。
    // 入参：无。
    // 返回：无返回值；渲染器恢复未初始化状态。
    void Reset() noexcept;

  private:
    friend struct OverlayRendererTestAccess;
    // 绘制冻结预览、选区外暗层与控制点，并按调用方要求提交交换链。
    // 入参：snapshot：虚拟桌面物理像素选区快照；present：是否调用 Present；errorMessage：输出参数，接收失效或绘制错误原因。
    // 返回：完成绘制及所请求呈现时为 true；未初始化、显示配置过期、窗口或图形调用失败时为 false。
    [[nodiscard]] bool DrawFrame(SelectionSnapshot snapshot, bool present, std::wstring& errorMessage);
    // 为集成测试读取当前 GPU 后缓冲，验证实际绘制像素。
    // 入参：frame：输出参数，接收回读的预览格式和像素；errorMessage：输出参数，接收图形回读失败原因。
    // 返回：后缓冲成功复制到 CPU 内存时为 true；资源获取、复制准备或映射失败时为 false。
    [[nodiscard]] bool ReadbackFrame(OutputPreviewFrame& frame, std::wstring& errorMessage);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace open_st
