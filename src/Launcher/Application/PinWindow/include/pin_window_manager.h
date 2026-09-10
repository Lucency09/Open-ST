// 声明多贴图管理入口，统一独立图像、窗口层级、输入资格与模态生命周期。
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <windows.h>

namespace open_st
{
class PinImage;
class PinWindow;
using PinId = std::uint64_t;
enum class PinCommand : std::uint32_t
{
    Copy = 1,
    Save = 2
};
struct PinWindowCallbacks final
{
    // 查询当前语言的贴图文字。
    // 入参：文本键。
    // 返回：已解析宽字符串。
    std::function<std::wstring(std::string_view)> text;
    // 投递贴图业务命令。
    // 入参：稳定 ID 和命令。
    // 返回：是否成功入队，不代表业务成功。
    std::function<bool(PinId, PinCommand)> command;
    // 在退出请求的全部窗口及模态调用结束后通知宿主。
    // 入参：无。
    // 返回：无返回值；只通知一次，宿主应投递消息而不在回调内销毁管理器。
    std::function<void()> stopped;
};
class PinWindowManager final
{
  public:
    // 建立在调用线程使用的管理器。
    // 入参：模块实例和借用宿主的回调。
    // 返回：无返回值。
    PinWindowManager(HINSTANCE instance, PinWindowCallbacks callbacks);
    // 释放全部窗口与图像。
    // 入参：无。
    // 返回：无返回值，宿主须在模态和窗口调用栈结束后析构。
    ~PinWindowManager();
    // 禁止复制窗口所有权。
    // 入参：未命名源管理器。
    // 返回：无返回值，调用在编译期拒绝。
    PinWindowManager(const PinWindowManager&) = delete;
    // 禁止复制赋值窗口所有权。
    // 入参：未命名源管理器。
    // 返回：无返回值，调用在编译期拒绝。
    PinWindowManager& operator=(const PinWindowManager&) = delete;
    // 准备隐藏窗口并完成首帧。
    // 入参：image 为独立原图、origin 为物理坐标、id/error 为输出。
    // 返回：成功 true。
    [[nodiscard]] bool Prepare(std::shared_ptr<const PinImage> image, POINT origin, PinId& id, std::wstring& error);
    // 显示已准备贴图且不授予滚轮资格。
    // 入参：稳定 ID。
    // 返回：无返回值，无效 ID 或截图暂停期间不操作。
    void Show(PinId id) noexcept;
    // 暂停贴图输入与层级修复，保持当前外观参与捕获并由遮罩覆盖。
    // 入参：error 接收忙状态或退出诊断。
    // 返回：成功 true，重复调用不改变状态。
    [[nodiscard]] bool BeginCapture(std::wstring& error);
    // 在宿主关闭遮罩后恢复输入与层级管理。
    // 入参：无。
    // 返回：无返回值，退出期间不再恢复交互，不改变可见性。
    void EndCapture() noexcept;
    // 获取独立原图快照。
    // 入参：稳定 ID。
    // 返回：共享只读图像，无效或待关闭 ID 返回空。
    [[nodiscard]] std::shared_ptr<const PinImage> Image(PinId id) const noexcept;
    // 查询借用 HWND。
    // 入参：稳定 ID。
    // 返回：无效或待关闭 ID 返回 nullptr，调用方不得销毁句柄。
    [[nodiscard]] HWND Window(PinId id) const noexcept;
    // 为宿主提示选择最高的可见贴图，避免普通模态窗口被置顶图像遮挡。
    // 入参：无。
    // 返回：借用 HWND，无可用可见贴图时为空；宿主须在模态保护期间使用。
    [[nodiscard]] HWND ModalOwner() const noexcept;
    // 申请关闭单图。
    // 入参：稳定 ID。
    // 返回：无返回值，模态/窗口调用栈期间延迟销毁。
    void Close(PinId id) noexcept;
    // 申请关闭全部图。
    // 入参：无。
    // 返回：无返回值，已有模态 owner 保持至调用返回。
    void CloseAll() noexcept;
    // 刷新窗口标题。
    // 入参：无。
    // 返回：无返回值，菜单文字仍在每次打开时查询。
    void RefreshText() noexcept;
    // 把不可达贴图移回有效屏幕。
    // 入参：无。
    // 返回：无返回值，保持图像物理尺寸与排序。
    void RelocateVisible() noexcept;
    // 查询可用贴图数。
    // 入参：无。
    // 返回：不含待关闭窗口的数量。
    [[nodiscard]] std::size_t Count() const noexcept;
    // 进入全局可嵌套模态暂停。
    // 入参：owner 为存活贴图 ID；零表示没有贴图所属窗口的全局暂停。
    // 返回：成功 true，退出/无效 ID 时 false。
    [[nodiscard]] bool BeginModal(PinId owner) noexcept;
    // 离开一次模态暂停。
    // 入参：无。
    // 返回：无返回值，最外层退出时执行延迟销毁及排序恢复。
    void EndModal() noexcept;
    // 停止新请求并关闭全部图。
    // 入参：无。
    // 返回：无返回值，窗口/模态调用栈结束前仅标记退出。
    void Shutdown() noexcept;
    // 查询是否仍有需要延迟宿主销毁的调用。
    // 入参：无。
    // 返回：模态或窗口入口执行中返回 true。
    [[nodiscard]] bool IsBusy() const noexcept;

  private:
    friend class PinWindow;
    friend struct PinWindowTestAccess;
    friend struct PinWindowGateTestAccess;
    class Impl;
    std::unique_ptr<Impl> impl_;
    // 查找内部窗口记录。
    // 入参：ID。
    // 返回：借用记录，已申请关闭的记录仍可能存在。
    [[nodiscard]] PinWindow* Find(PinId id) const noexcept;
    // 判断是否允许输入。
    // 入参：ID。
    // 返回：存活可见且未暂停时返回 true。
    [[nodiscard]] bool CanInteract(PinId id) const noexcept;
    // 登记实际鼠标点击。
    // 入参：ID。
    // 返回：无返回值，仅实际聚焦后授予资格。
    void Click(PinId id) noexcept;
    // 清除指定窗口输入资格。
    // 入参：ID。
    // 返回：无返回值，不改变排序。
    void LoseFocus(PinId id) noexcept;
    // 校验滚轮资格。
    // 入参：ID 和屏幕物理坐标。
    // 返回：仅焦点和命中均有效时返回 true。
    [[nodiscard]] bool CanWheel(PinId id, POINT point) const noexcept;
    // 将指定图提升到本程序顶部。
    // 入参：ID。
    // 返回：无返回值，其余顺序保持。
    void Raise(PinId id) noexcept;
    // 查询本程序最上方贴图。
    // 入参：ID。
    // 返回：当前最高有效 ID 返回 true。
    [[nodiscard]] bool IsHighest(PinId id) const noexcept;
    // 按业务顺序修复原生窗口层级。
    // 入参：无。
    // 返回：无返回值，模态或修复重入时跳过。
    void RepairOrder() noexcept;
    // 约束原生提层请求。
    // 入参：ID 和可改 WINDOWPOS。
    // 返回：无返回值，不修改尺寸或位置。
    void ConstrainOrder(PinId id, WINDOWPOS& position) noexcept;
    // 读取本地化文字。
    // 入参：key。
    // 返回：当前文字，回调异常或不可用时返回问号。
    [[nodiscard]] std::wstring Text(std::string_view key) const noexcept;
    // 投递复制保存命令。
    // 入参：ID 和 command。
    // 返回：成功入队 true，暂停/失效或异常 false。
    [[nodiscard]] bool Submit(PinId id, PinCommand command) noexcept;
    // 登记进入窗口回调。
    // 入参：无。
    // 返回：无返回值，使销毁延迟到最外层入口返回。
    void EnterDispatch() noexcept;
    // 退出窗口回调。
    // 入参：无。
    // 返回：无返回值，最外层返回时清理待关闭记录。
    void LeaveDispatch() noexcept;
    // 回收已经申请关闭且不再被调用栈引用的记录。
    // 入参：无。
    // 返回：无返回值。
    void DrainClosed() noexcept;
};
} // namespace open_st
