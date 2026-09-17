// 管理标注属性请求与原位文字输入的交互寿命，不拥有文档、预览或撤销历史。
#pragma once
#include "capture_annotation_state.h"
#include <functional>
#include <memory>
#include <optional>
#include <windows.h>

namespace open_st
{
class InlineTextEditor;
enum class InlineEditCommand;
enum class InlineTextRequest;
struct AnnotationPropertyContext
{
    CaptureAnnotationState* state{}; // 仅在同步调用期间借用。
    RectI selection{};
    std::uint64_t token{};
    bool ready{};
};
struct AnnotationPropertyRequest
{
    std::uint64_t token{}, revision{}, id{};
};
struct AnnotationTextHost
{
    HWND owner{}, notificationWindow{};
    std::function<bool()> valid;
    std::function<void()> changed;
    std::function<std::wstring(std::string_view)> text;
};
class AnnotationInteractionController final
{
  public:
    // 建立跨截图会话存活的交互所有者。
    // 入参：无。返回：空闲实例，序号仅在开始请求时增长。
    AnnotationInteractionController();
    // 释放输入窗口；宿主须在销毁编辑状态之前调用 CloseSession。
    // 入参：无。返回：无。
    ~AnnotationInteractionController();
    // 禁止复制自有窗口与借用事务。
    // 入参：源实例。返回：不可调用。
    AnnotationInteractionController(const AnnotationInteractionController&) = delete;
    // 禁止复制赋值。
    // 入参：源实例。返回：不可调用。
    AnnotationInteractionController& operator=(const AnnotationInteractionController&) = delete;
    // 命中元素并固定右键资格，图形返回后重验上下文。
    // 入参：window、point 为点击；query 返回当前会话准入。返回：无图形或窗口错误时 true。
    bool BeginProperty(HWND window, PointI point, const std::function<AnnotationPropertyContext()>& query) noexcept;
    // 移动超过容差后取消点击资格。
    // 入参：point 为物理坐标。返回：无。
    void MoveProperty(PointI point) noexcept;
    // 结束点击并投递稳定序号，失败不留下待处理请求。
    // 入参：point、context 为当前点击和会话；notificationWindow 为通知窗口。返回：无错误时 true。
    bool EndProperty(PointI point, AnnotationPropertyContext context, HWND notificationWindow) noexcept;
    // 消费匹配请求，迟到消息不清除新请求。
    // 入参：serial 为通知序号。返回：匹配时返回请求值。
    std::optional<AnnotationPropertyRequest> TakeProperty(std::uint64_t serial) noexcept;
    // 清除点击资格，不影响已经排队的请求。
    // 入参：无。返回：无。
    void LeaveProperty() noexcept;
    // 取消所有属性请求，但不重置单调序号。
    // 入参：无。返回：无。
    void CancelProperties() noexcept;
    // 查询当前待处理请求身份。
    // 入参：无。返回：零表示无请求。
    std::uint64_t PendingProperty() const noexcept;
    // 开始文字事务并建立公共输入组件，失败取消已开始事务。
    // 入参：state 借用至结束；selection/point/id 为目标；host 为窄宿主接口。
    // 返回：成功或无操作为 true，控件创建失败为 false。
    bool BeginText(CaptureAnnotationState& state, RectI selection, PointI point, std::uint64_t id,
                   AnnotationTextHost host) noexcept;
    // 处理输入窗口以外的点击，不复用点击启动下一手势。
    // 入参：point 为物理位置；selection 为当前裁剪。返回：无。
    void ClickText(PointI point, RectI selection) noexcept;
    // 消费完成请求，回调或图形忙时保留结束意图。
    // 入参：serial、accept 为通知；graphicsBusy 为图形栈保护。返回：本次已结束文字为 true。
    bool FinishText(std::uint64_t serial, bool accept, bool graphicsBusy) noexcept;
    // 在安全消息边界消费延后完成或失效取消。
    // 入参：graphicsBusy 为图形栈保护。返回：已结束文字为 true。
    bool DrainText(bool graphicsBusy) noexcept;
    // 请求公共输入组件确认或取消，通知仍由宿主消息窗口接收。
    // 入参：accept 为确认意图。返回：公共组件请求状态。
    InlineTextRequest RequestText(bool accept) noexcept;
    // 转发已经通过宿主热键身份校验的编辑动作。
    // 入参：command 为通用编辑命令。返回：控件接受时 true。
    bool InvokeTextCommand(InlineEditCommand command) noexcept;
    // 查询是否持有文字事务，包括创建控件期间。
    // 入参：无。返回：绑定存在时 true。
    bool TextActive() const noexcept;
    // 查询窗口回调或创建是否阻止安全关闭。
    // 入参：无。返回：可立即关闭时 true。
    bool CanClose() const noexcept;
    // 取消并解除全部会话借用，回调期间拒绝立即关闭。
    // 入参：无。返回：关闭完成时 true，否则宿主须延后回收。
    bool CloseSession() noexcept;

  private:
    friend struct AppAnnotationTextTestAccess;
    struct PropertyClick
    {
        PointI point{};
        int toleranceX{}, toleranceY{};
        AnnotationPropertyRequest request{};
        bool armed{};
    };
    // 查询宿主会话是否仍有效，异常一律视为失效。
    // 入参：无。返回：允许继续提交时 true。
    bool TextValid() const noexcept;
    // 在安全边界释放控件和状态借用，不改序号。
    // 入参：无。返回：无。
    void ReleaseText() noexcept;
    PropertyClick click_{};
    AnnotationPropertyRequest propertyRequest_{};
    std::uint64_t propertySerial_{}, propertyPending_{};
    CaptureAnnotationState* textState_{};
    AnnotationTextHost textHost_{};
    std::unique_ptr<InlineTextEditor> editor_;
    std::uint64_t textSerial_{};
    bool textPending_{}, textDispatching_{}, textOpening_{}, textDeferred_{}, textAccept_{};
};
} // namespace open_st
