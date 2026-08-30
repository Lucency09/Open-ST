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
    // 创建尚未绑定窗口和图形资源的渲染器。
    OverlayRenderer();
    // 按资源依赖逆序释放当前渲染会话。
    ~OverlayRenderer();

    // 禁止复制渲染器，确保 PImpl 中的图形资源只有一个所有者。
    OverlayRenderer(const OverlayRenderer&) = delete;
    // 禁止复制赋值，避免多个对象共享同一组 COM 资源。
    OverlayRenderer& operator=(const OverlayRenderer&) = delete;

    // Initialize 一次性上传覆盖预览；window 和 frame 必须在调用期间有效，上传后不再借用 frame 内存。
    // configuredBorderColor 仅在本次调用借用；缺失或非法 #RRGGBB 使用黑色，不访问设置服务。
    [[nodiscard]] bool Initialize(HWND window, const OutputPreviewFrame& frame,
                                   std::optional<std::string_view> configuredBorderColor,
                                   std::wstring& errorMessage);

    // Resize 只重建与交换链尺寸相关的目标，冻结帧位图和设备保持不变。
    [[nodiscard]] bool Resize(unsigned int width, unsigned int height, std::wstring& errorMessage);

    // Render 按“冻结帧 → 选区外 48% 黑色遮罩 → 选区边框/控制点 → Present”的顺序绘制客户区。
    // snapshot 是虚拟桌面物理像素的不可变值快照，渲染器不持有 SelectionModel。
    [[nodiscard]] bool Render(SelectionSnapshot snapshot, std::wstring& errorMessage);
    // 释放全部窗口、交换链、D2D/D3D 资源并恢复到未初始化状态。
    void Reset() noexcept;

  private:
    friend struct OverlayRendererTestAccess;
    // 绘制完整一帧；测试可关闭 Present，在翻转前读取确定的后缓冲内容。
    [[nodiscard]] bool DrawFrame(SelectionSnapshot snapshot, bool present, std::wstring& errorMessage);
    // 仅供友元集成测试读取 GPU 后缓冲；正式保存与剪贴板禁止使用覆盖窗口内容。
    [[nodiscard]] bool ReadbackFrame(OutputPreviewFrame& frame, std::wstring& errorMessage);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace open_st
