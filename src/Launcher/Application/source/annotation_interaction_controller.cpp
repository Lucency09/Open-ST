// 实现右键请求与原位文字的独占交互状态，通过窄接口借用宿主和唯一文档事务。
#include "annotation_interaction_controller.h"
#include <algorithm>
#include <annotation_hit_test.h>
#include <climits>
#include <inline_text_editor.h>
#include <limits>
#include <stdexcept>

namespace open_st
{
namespace
{
struct DispatchGuard
{
    bool& active;
    bool previous;
    // 在同步窗口回调内阻止宿主销毁当前组件。
    // 入参：value 为分派标记。返回：保护当前调用栈。
    explicit DispatchGuard(bool& value) : active(value), previous(value)
    {
        this->active = true;
    }
    // 恢复外层分派状态。
    // 入参：无。返回：无。
    ~DispatchGuard()
    {
        this->active = this->previous;
    }
};
} // namespace
// 创建未绑定截图会话的交互所有者。
// 入参：无。返回：空闲实例。
AnnotationInteractionController::AnnotationInteractionController() = default;
// 释放自有控件；会话借用必须先由宿主在安全边界解除。
// 入参：无。返回：无。
AnnotationInteractionController::~AnnotationInteractionController() = default;
// 固定点击目标并在图形查询返回后检查会话未改变。
// 入参：window、point 为物理点击；query 查询当前准入。返回：操作无错误时 true。
bool AnnotationInteractionController::BeginProperty(HWND window, PointI point,
                                                    const std::function<AnnotationPropertyContext()>& query) noexcept
try
{
    this->click_ = {};
    const AnnotationPropertyContext context = query();
    if (!context.ready || !context.state)
        return true;
    const UINT windowDpi = GetDpiForWindow(window);
    const UINT dpi = windowDpi == 0 ? USER_DEFAULT_SCREEN_DPI : windowDpi;
    const AnnotationSnapshot document = context.state->Committed();
    const std::uint64_t revision = context.state->Revision();
    std::uint64_t id{};
    std::wstring error;
    if (!HitTestAnnotations(document, context.selection, {static_cast<double>(point.x), static_cast<double>(point.y)},
                            3.0F * static_cast<float>(dpi) / 96.0F, id, error))
        return false;
    const AnnotationPropertyContext current = query();
    if (id == 0 || !current.ready || current.state != context.state || current.token != context.token ||
        current.state->Revision() != revision || current.state->Committed() != document)
        return true;
    TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
    if (!TrackMouseEvent(&tracking))
        return false;
    this->click_ = {point,
                    std::max(1, GetSystemMetricsForDpi(SM_CXDRAG, dpi) / 2),
                    std::max(1, GetSystemMetricsForDpi(SM_CYDRAG, dpi) / 2),
                    {context.token, revision, id},
                    true};
    return true;
}
catch (...)
{
    this->click_ = {};
    return false;
}
// 越过系统拖动容差即失去点击资格。
// 入参：point 为桌面物理坐标。返回：无。
void AnnotationInteractionController::MoveProperty(PointI point) noexcept
{
    if (!this->click_.armed)
        return;
    const std::int64_t dx = static_cast<std::int64_t>(point.x) - this->click_.point.x;
    const std::int64_t dy = static_cast<std::int64_t>(point.y) - this->click_.point.y;
    if (dx < -this->click_.toleranceX || dx > this->click_.toleranceX || dy < -this->click_.toleranceY ||
        dy > this->click_.toleranceY)
        this->click_.armed = false;
}
// 有效抬起仅投递稳定身份，不把文档地址放进窗口消息。
// 入参：point、context 为当前状态；notificationWindow 为宿主消息窗口。返回：无错误为 true。
bool AnnotationInteractionController::EndProperty(PointI point, AnnotationPropertyContext context,
                                                  HWND notificationWindow) noexcept
{
    this->MoveProperty(point);
    const PropertyClick click = this->click_;
    this->click_ = {};
    if (!click.armed || !context.ready || !context.state || click.request.token != context.token ||
        click.request.revision != context.state->Revision() || point.x < context.selection.left ||
        point.x >= context.selection.right || point.y < context.selection.top || point.y >= context.selection.bottom)
        return true;
    if (this->propertySerial_ == std::numeric_limits<std::uint64_t>::max())
        return false;
    this->propertyRequest_ = click.request;
    this->propertyPending_ = ++this->propertySerial_;
    if (PostMessageW(notificationWindow, WM_APP + 8, static_cast<WPARAM>(this->propertyPending_), 0))
        return true;
    this->CancelProperties();
    return false;
}
// 只消费当前预订的属性请求。
// 入参：serial 为排队消息身份。返回：匹配请求或空。
std::optional<AnnotationPropertyRequest> AnnotationInteractionController::TakeProperty(std::uint64_t serial) noexcept
{
    if (serial == 0 || serial != this->propertyPending_)
        return {};
    const AnnotationPropertyRequest request = this->propertyRequest_;
    this->propertyPending_ = 0;
    this->propertyRequest_ = {};
    return request;
}
// 离窗只撤销按下资格。
// 入参：无。返回：无。
void AnnotationInteractionController::LeaveProperty() noexcept
{
    this->click_ = {};
}
// 清空活动请求，不复用序号。
// 入参：无。返回：无。
void AnnotationInteractionController::CancelProperties() noexcept
{
    this->click_ = {};
    this->propertyRequest_ = {};
    this->propertyPending_ = 0;
}
// 查询当前属性通知身份。
// 入参：无。返回：零或有效序号。
std::uint64_t AnnotationInteractionController::PendingProperty() const noexcept
{
    return this->propertyPending_;
}
// 防止宿主查询异常跨越窗口过程。
// 入参：无。返回：会话有效为 true。
bool AnnotationInteractionController::TextValid() const noexcept
try
{
    return this->textState_ && this->textHost_.valid && this->textHost_.valid();
}
catch (...)
{
    return false;
}
// 创建文字事务及其原位输入组件，创建期间先发布忙状态保护同步窗口消息。
// 入参：state 为唯一事务所有者；selection、point、id 为目标；host 为宿主窄接口。
// 返回：成功或准入无操作为 true，初始化失败为 false。
bool AnnotationInteractionController::BeginText(CaptureAnnotationState& state, RectI selection, PointI point,
                                                std::uint64_t id, AnnotationTextHost host) noexcept
{
    if (this->TextActive() || this->textSerial_ == std::numeric_limits<std::uint64_t>::max())
        return true;
    if (!(id == 0 ? state.BeginText(point, selection) : state.BeginTextEdit(id, state.Revision(), selection)))
        return true;
    this->textState_ = &state;
    this->textHost_ = std::move(host);
    this->textPending_ = this->textDeferred_ = false;
    this->textOpening_ = true;
    const std::uint64_t serial = ++this->textSerial_;
    try
    {
        this->textHost_.changed();
        const AnnotationObject draft = *state.TextDraft();
        const AnnotationText& body = std::get<AnnotationText>(draft.payload);
        InlineTextOptions options;
        options.owner = this->textHost_.owner;
        options.clip = {selection.left, selection.top, selection.right, selection.bottom};
        const LONG x = static_cast<LONG>(draft.origin.x), y = static_cast<LONG>(draft.origin.y);
        const std::int64_t right = std::max<std::int64_t>(static_cast<std::int64_t>(x) + 120, selection.right);
        const std::int64_t bottom = std::min<std::int64_t>(static_cast<std::int64_t>(y) + 180, selection.bottom);
        options.bounds = {x, y, static_cast<LONG>(std::min<std::int64_t>(right, LONG_MAX)),
                          static_cast<LONG>(std::max<std::int64_t>(bottom, static_cast<std::int64_t>(y) + 1))};
        options.fontPixelHeight = static_cast<unsigned>(body.fontSize);
        options.fontFamily.assign(ANNOTATION_FONT_FAMILY.begin(), ANNOTATION_FONT_FAMILY.end());
        options.rgb = draft.style.rgb;
        if (body.text)
            options.text.assign(body.text->begin(), body.text->end());
        this->editor_ = std::make_unique<InlineTextEditor>();
        InlineTextCallbacks callbacks;
        // 输入缓存交给唯一文字事务，控件不保存文档历史。
        // 入参：value 为完整正文。返回：候选已被状态层接受为 true。
        callbacks.change = [this, serial](std::wstring_view value)
        {
            const DispatchGuard guard(this->textDispatching_);
            if (serial != this->textSerial_ || !this->editor_ || !this->TextValid())
                return false;
            const bool valid = this->textState_->UpdateText(std::u16string(value.begin(), value.end()));
            this->editor_->SetError(valid ? L"" : this->textHost_.text("annotation.text.invalid"));
            return valid;
        };
        // 完成通知只排队，回调栈内绝不销毁输入窗口。
        // 入参：value 为正文；accept 为确认意图。返回：通知成功排队为 true。
        callbacks.finish = [this, serial](std::wstring_view value, bool accept)
        {
            const DispatchGuard guard(this->textDispatching_);
            if (serial != this->textSerial_ || !this->textState_ || !this->editor_ || this->textPending_)
                return false;
            accept = accept && this->TextValid();
            if (accept)
            {
                const bool valid = this->textState_->UpdateText(std::u16string(value.begin(), value.end()));
                const std::optional<AnnotationObject> draft = this->textState_->TextDraft();
                if (!valid || !draft || (!this->textState_->CanCommitText() && draft->id != 0))
                {
                    this->editor_->SetError(this->textHost_.text("annotation.text.invalid"));
                    return false;
                }
            }
            this->textPending_ = true;
            this->textAccept_ = accept;
            if (!PostMessageW(this->textHost_.notificationWindow, WM_APP + 9, static_cast<WPARAM>(serial),
                              accept ? 1 : 0))
            {
                this->textPending_ = false;
                this->editor_->SetError(this->textHost_.text("annotation.edit_failed"));
                return false;
            }
            return true;
        };
        // 将公共错误码适配为产品文字，保持公共组件无标注依赖。
        // 入参：code 为输入错误码。返回：无。
        callbacks.error = [this, serial](std::string_view code)
        {
            const DispatchGuard guard(this->textDispatching_);
            if (serial == this->textSerial_ && this->editor_)
                this->editor_->SetError(
                    this->textHost_.text(code == "text_limit" ? "annotation.text.limit" : "annotation.text.invalid"));
        };
        if (!this->editor_->Open(options, std::move(callbacks)))
            throw std::runtime_error("Inline text creation failed");
        this->textOpening_ = false;
        return true;
    }
    catch (...)
    {
        this->textOpening_ = false;
        (void)this->textState_->EndText(false);
        this->ReleaseText();
        return false;
    }
}
// 输入框外的合法点击请求完成，其余点击恢复输入焦点。
// 入参：point 为物理位置；selection 为正式裁剪。返回：无。
void AnnotationInteractionController::ClickText(PointI point, RectI selection) noexcept
{
    if (!this->editor_ || this->textPending_)
        return;
    if (point.x >= selection.left && point.x < selection.right && point.y >= selection.top &&
        point.y < selection.bottom && !this->editor_->IsComposing())
        (void)this->editor_->RequestCommit();
    if (this->editor_ && !this->textPending_)
        this->editor_->Focus();
}
// 安全消费完成消息，失败时恢复同一控件及其原生撤销历史。
// 入参：serial、accept 为消息；graphicsBusy 为宿主图形保护。返回：已结束为 true。
bool AnnotationInteractionController::FinishText(std::uint64_t serial, bool accept, bool graphicsBusy) noexcept
{
    if (!this->TextActive() || serial != this->textSerial_)
        return false;
    if (!this->CanClose() || graphicsBusy)
    {
        this->textDeferred_ = true;
        this->textAccept_ = accept;
        return false;
    }
    this->textDeferred_ = false;
    accept = accept && this->TextValid();
    const AnnotationCommitResult changed = this->textState_->EndText(accept);
    if (accept && changed == AnnotationCommitResult::Failed && this->textState_->EditingText())
    {
        this->textPending_ = false;
        this->editor_->Resume();
        try
        {
            this->editor_->SetError(this->textHost_.text("annotation.edit_failed"));
        }
        catch (...)
        {
        }
        this->editor_->Focus();
        return false;
    }
    this->ReleaseText();
    return true;
}
// 在宿主消息边界补消费延迟请求，失效始终取消。
// 入参：graphicsBusy 为图形忙。返回：已结束为 true。
bool AnnotationInteractionController::DrainText(bool graphicsBusy) noexcept
{
    if (!this->TextActive() || !this->CanClose() || graphicsBusy)
        return false;
    if (!this->TextValid())
        return this->FinishText(this->textSerial_, false, false);
    return this->textDeferred_ && this->FinishText(this->textSerial_, this->textAccept_, false);
}
// 请求公共输入组件发送一次完成通知。
// 入参：accept 为确认意图。返回：公共请求状态。
InlineTextRequest AnnotationInteractionController::RequestText(bool accept) noexcept
{
    if (!this->editor_ || !this->CanClose())
        return InlineTextRequest::Rejected;
    return accept ? this->editor_->RequestCommit() : this->editor_->RequestCancel();
}
// 转发已经核验身份的通用输入命令。
// 入参：command 为编辑动作。返回：成功分派时 true。
bool AnnotationInteractionController::InvokeTextCommand(InlineEditCommand command) noexcept
{
    return this->editor_ && this->editor_->InvokeEditCommand(command);
}
// 查询文字事务是否绑定。
// 入参：无。返回：文字活动状态。
bool AnnotationInteractionController::TextActive() const noexcept
{
    return this->textState_ != nullptr;
}
// 查询窗口操作调用栈是否已退出。
// 入参：无。返回：安全关闭时 true。
bool AnnotationInteractionController::CanClose() const noexcept
{
    return !this->textDispatching_ && !this->textOpening_;
}
// 先销毁控件，再解除事务和宿主回调借用。
// 入参：无。返回：无。
void AnnotationInteractionController::ReleaseText() noexcept
{
    const DispatchGuard guard(this->textDispatching_);
    if (this->editor_)
        this->editor_->Close();
    this->editor_.reset();
    this->textState_ = nullptr;
    this->textHost_ = {};
    this->textPending_ = this->textDeferred_ = false;
}
// 会话关闭取消活动交互，序号留给下次会话继续使用。
// 入参：无。返回：已解除全部借用为 true。
bool AnnotationInteractionController::CloseSession() noexcept
{
    this->CancelProperties();
    if (!this->CanClose())
    {
        this->textDeferred_ = true;
        this->textAccept_ = false;
        return false;
    }
    CaptureAnnotationState* const state = this->textState_;
    this->ReleaseText();
    if (state)
        state->Cancel();
    return true;
}
} // namespace open_st
