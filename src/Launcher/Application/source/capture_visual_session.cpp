// 实现默认参数读取、本次截图临时状态与公共实时设置面板。
#include "capture_visual_session.h"
#include <frozen_desktop_frame.h>
#include <log.h>
#include <native_tone_mapper.h>
#include <overlay_renderer.h>
#include <sdr_selection_frame.h>
#include <selection_preview_worker.h>
#include <settings.h>
#include <window_renderer.h>
#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <utility>

namespace open_st
{
namespace
{
// 为公共表单失败补充不含输入值的结构诊断，宿主统一显示粗略错误。
// 入参：result 为公共控件结果。返回：成功为 true；失败记录位置并抛出诊断。
bool CheckVisualRenderer(const RendererResult& result)
{
    if (result)
        return true;
    OPEN_ST_LOG_ERROR("Capture settings renderer failed. code=", result.code, " path=", result.path, " id=", result.id);
    throw std::runtime_error("Capture settings renderer: " + result.code + " path=" + result.path + " id=" + result.id);
}
} // namespace

// 将完整设置键交给对应图形领域校验。
// 入参：key 为设置键；value 为整数。返回：领域允许时为 true。
bool IsCaptureVisualSetting(std::string_view key, std::int64_t value) noexcept
{
    return key == "capture.hdr_brightness_percent" ? IsHdrBrightnessPercent(value)
           : key == "capture.mask_opacity_percent" ? IsMaskOpacityPercent(value)
                                                   : false;
}
// 一次读取完整快照，映射策略仅允许缺失字段取资源默认，非法字段拒绝。
// 入参：无。返回：有效默认参数或空。
std::optional<CaptureVisualDefaults> ReadCaptureVisualDefaults() noexcept
{
    try
    {
        const std::array<std::string_view, 3> keys{"capture.hdr_brightness_percent", "capture.mask_opacity_percent",
                                                   "capture.hdr_tone_mapping"};
        const auto snapshot = ReadSettingsSnapshot(keys);
        if (!snapshot)
            return std::nullopt;
        std::array<unsigned int, 2> values{};
        for (std::size_t index = 0; index < values.size(); ++index)
        {
            const auto& raw = snapshot->at(keys[index]);
            std::optional<std::int64_t> value;
            if (raw.is_number_integer() && (!raw.is_number_unsigned() || raw.get<std::uint64_t>() <= 200U))
                value = raw.get<std::int64_t>();
            if (!value || !IsCaptureVisualSetting(keys[index], *value))
                value = GetDefaultIntegerSetting(keys[index]);
            if (!value || !IsCaptureVisualSetting(keys[index], *value))
                return std::nullopt;
            values[index] = static_cast<unsigned int>(*value);
        }
        auto policy = snapshot->at(keys[2]);
        if (!policy.is_object())
            return std::nullopt;
        if (!policy.contains("highlight_ceiling_nits"))
        {
            const auto defaults = GetDefaultJsonSetting(keys[2]);
            if (!defaults || !defaults->is_object() || !defaults->contains("highlight_ceiling_nits"))
                return std::nullopt;
            policy["highlight_ceiling_nits"] = defaults->at("highlight_ceiling_nits");
        }
        const auto& ceiling = policy.at("highlight_ceiling_nits");
        if (!ceiling.is_number_integer() ||
            (ceiling.is_number_unsigned() && ceiling.get<std::uint64_t>() > HDR_MAX_HIGHLIGHT_CEILING_NITS))
            return std::nullopt;
        const auto value = ceiling.get<std::int64_t>();
        if (!IsHdrHighlightCeilingNits(value))
            return std::nullopt;
        return CaptureVisualDefaults{values[0], values[1], {static_cast<unsigned int>(value)}};
    }
    catch (...)
    {
        return std::nullopt;
    }
}
// 创建唯一工作线程，图形转换器仅在该线程使用。
// 入参：wake 为线程安全通知。返回：无。
CaptureVisualSession::CaptureVisualSession(std::function<void()> wake)
    : worker_(std::make_unique<SelectionPreviewWorker>(std::move(wake)))
{
}
// 保持通知目标有效直到所有后台图形资源释放。
// 入参：无。返回：无。
CaptureVisualSession::~CaptureVisualSession()
{
    this->End();
    this->worker_.reset();
}
// 初始化本次参数及安全持有的不可变来源。
// 入参：frame/defaults/changed 为本次输入及宿主更新回调。返回：无。
void CaptureVisualSession::Begin(std::shared_ptr<const FrozenDesktopFrame> frame, CaptureVisualDefaults defaults,
                                 std::function<void()> changed)
{
    if (!frame || !frame->IsValid() || !IsHdrBrightnessPercent(defaults.brightness) ||
        !IsMaskOpacityPercent(defaults.mask) || !IsHdrHighlightCeilingNits(defaults.toneMapping.highlightCeilingNits))
        throw std::invalid_argument("Invalid capture visual defaults");
    this->End();
    this->frame_ = std::move(frame);
    this->defaults_ = defaults;
    this->brightness_ = defaults.brightness;
    this->mask_ = defaults.mask;
    this->changed_ = std::move(changed);
    this->interactive_ = true;
}
// 撤销请求及回调，后台输入由任务自己的共享引用保护。
// 入参：无。返回：无。
void CaptureVisualSession::End() noexcept
{
    this->worker_->Cancel();
    if (this->panel_)
        (void)this->panel_->RequestClose();
    this->panel_.reset();
    this->frame_.reset();
    this->preview_.reset();
    this->selection_.reset();
    this->requested_ = 0;
    this->pending_ = false;
    this->deferredOutput_ = {};
    this->changed_ = {};
    this->text_ = {};
}
// 获取唯一临时亮度。
// 入参：无。返回：百分比。
unsigned CaptureVisualSession::Brightness() const noexcept
{
    return this->brightness_;
}
// 获取唯一临时遮罩。
// 入参：无。返回：百分比。
unsigned CaptureVisualSession::Mask() const noexcept
{
    return this->mask_;
}
// 查询截图开始时持有的显式策略副本。
// 入参：无。返回：本次策略。
HdrToneMappingOptions CaptureVisualSession::ToneMappingOptions() const noexcept
{
    return this->defaults_.toneMapping;
}

// 检查可转换的HDR平面与选区交集，系统已转SDR的平面不重复转换。
// 入参：selection 为物理选区。返回：包含原生HDR为 true。
bool CaptureVisualSession::HasHdr(RectI selection) const noexcept
{
    if (!this->frame_ || selection.IsEmpty())
        return false;
    for (const CapturedOutputPlane& plane : this->frame_->Outputs())
    {
        const RectI bounds = plane.Bounds();
        if (std::max(bounds.left, selection.left) < std::min(bounds.right, selection.right) &&
            std::max(bounds.top, selection.top) < std::min(bounds.bottom, selection.bottom) &&
            plane.PixelColorSpace() != CapturedColorSpace::SdrGamma22P709 &&
            !plane.ColorMetadata().systemConvertedToSdr)
            return true;
    }
    return false;
}
// 固定最新目标，新的请求ID在对象整个寿命中不复用。
// 入参：selection 为正式物理选区。返回：提交成功为 true。
bool CaptureVisualSession::Request(RectI selection)
{
    if (!this->frame_)
        return false;
    const bool same = this->selection_ && this->selection_->left == selection.left &&
                      this->selection_->top == selection.top && this->selection_->right == selection.right &&
                      this->selection_->bottom == selection.bottom;
    if (same && this->requested_ != 0)
        return true;
    if (this->serial_ == std::numeric_limits<std::uint64_t>::max())
        return false;
    this->requested_ = ++this->serial_;
    this->selection_ = selection;
    this->hdr_ = this->HasHdr(selection);
    this->preview_.reset();
    this->pending_ = this->hdr_;
    if (!this->hdr_)
        this->worker_->Cancel();
    else if (!this->worker_->Submit(this->frame_, selection, this->brightness_, this->requested_,
                                    this->defaults_.toneMapping))
    {
        this->requested_ = 0;
        this->pending_ = false;
        this->RefreshPanel();
        return false;
    }
    this->RefreshPanel();
    return true;
}
// 仅接收当前请求结果，旧结果由工作线程及本层双重阻止。
// 入参：error 接收最新请求失败。返回：发布新底图时 true。
bool CaptureVisualSession::Poll(std::wstring& error)
{
    error.clear();
    std::optional<SelectionPreviewResult> result = this->worker_->TakeResult();
    if (!result || !this->frame_ || result->requestId != this->requested_)
        return false;
    this->pending_ = false;
    if (!result->success)
        error = result->error.empty() ? (this->text_ ? this->text_("capture.error.unknown") : L"HDR preview failed")
                                      : std::move(result->error);
    else
        this->preview_ = std::make_shared<SdrSelectionFrame>(std::move(result->frame));
    this->RefreshPanel();
    return this->preview_ != nullptr;
}
// 待更新时不会发布上一档底图。
// 入参：无。返回：当前版本的只读底图。
const SdrSelectionFrame* CaptureVisualSession::Preview() const noexcept
{
    return this->preview_.get();
}
// 共享当前版本，避免马赛克重新执行HDR转换。
// 入参：无。返回：共享只读底图。
std::shared_ptr<const SdrSelectionFrame> CaptureVisualSession::SharedPreview() const noexcept
{
    return this->preview_;
}
// 底图仍在处理时只保留一个完成请求。
// 入参：action 为完成动作。返回：成功保留为 true。
bool CaptureVisualSession::DeferOutput(std::function<void()> action)
{
    if (!this->frame_ || !this->pending_ || !action || this->deferredOutput_)
        return false;
    this->deferredOutput_ = std::move(action);
    return true;
}
// 查询等待底图的完成资格。
// 入参：无。返回：存在回调为 true。
bool CaptureVisualSession::HasDeferredOutput() const noexcept
{
    return static_cast<bool>(this->deferredOutput_);
}
// 完成请求只交付一次，调用方负责解除忙状态后执行业务。
// 入参：无。返回：移交的回调。
std::function<void()> CaptureVisualSession::TakeDeferredOutput() noexcept
{
    return std::exchange(this->deferredOutput_, {});
}
// 将后台工作计入应用退出屏障。
// 入参：无。返回：是否仍有后台任务。
bool CaptureVisualSession::HasWork() const noexcept
{
    return this->worker_->HasWork();
}
// 查询当前请求身份，避免静态画面主动循环呈现。
// 入参：无。返回：当前请求版本。
std::uint64_t CaptureVisualSession::Version() const noexcept
{
    return this->requested_;
}
// 更新合法临时参数并发起宿主重绘。
// 入参：brightness/mask 为候选。返回：合法且可交互时 true。
bool CaptureVisualSession::Change(unsigned brightness, unsigned mask)
{
    if (!this->frame_ || !this->interactive_ || !IsHdrBrightnessPercent(brightness) || !IsMaskOpacityPercent(mask))
        return false;
    if (this->brightness_ == brightness && this->mask_ == mask)
        return true;
    if (this->brightness_ != brightness)
    {
        this->worker_->Cancel();
        this->requested_ = 0;
        this->preview_.reset();
    }
    this->brightness_ = brightness;
    this->mask_ = mask;
    if (this->changed_)
        this->changed_();
    this->RefreshPanel();
    return true;
}
// 刷新可用状态与处理进度，不覆盖正在输入的数字。
// 入参：无。返回：无。
void CaptureVisualSession::RefreshPanel()
{
    if (!this->panel_ || !this->panel_->NativeHandle())
        return;
    (void)this->panel_->SetEnabled("brightness", this->interactive_ && this->hdr_);
    (void)this->panel_->SetEnabled("mask", this->interactive_);
    (void)this->panel_->SetEnabled("restore", this->interactive_);
    (void)this->panel_->RefreshTexts();
}
// 创建带连续整数滑条的公共非模态表单。
// 入参：owner/anchor/icon 为借用资源；text 为本地化入口。返回：显示成功为 true。
bool CaptureVisualSession::Show(HWND owner, HWND anchor, HICON icon, std::function<std::wstring(std::string_view)> text)
{
    if (!this->frame_ || !text)
        return false;
    if (this->Window())
    {
        SetForegroundWindow(this->Window());
        return true;
    }
    this->text_ = std::move(text);
    this->panel_ = std::make_unique<WindowRenderer>();
    const nlohmann::json layout = {
        {"schemaVersion", 1},
        {"window",
         {{"titleKey", "capture.settings.title"},
          {"initialSize", {430, 300}},
          {"minSize", {360, 270}},
          {"resizable", false}}},
        {"content",
         {{"type", "column"},
          {"id", "captureSettingsContent"},
          {"padding", 16},
          {"gap", 10},
          {"children",
           nlohmann::json::array({{{"type", "integer"},
                                   {"id", "brightness"},
                                   {"labelKey", "capture.settings.brightness"},
                                   {"minimum", static_cast<std::int64_t>(HDR_MIN_BRIGHTNESS_PERCENT)},
                                   {"maximum", static_cast<std::int64_t>(HDR_MAX_BRIGHTNESS_PERCENT)},
                                   {"slider", true}},
                                  {{"type", "integer"},
                                   {"id", "mask"},
                                   {"labelKey", "capture.settings.mask"},
                                   {"minimum", 0},
                                   {"maximum", static_cast<std::int64_t>(MAX_CAPTURE_MASK_OPACITY_PERCENT)},
                                   {"slider", true}},
                                  {{"type", "text"}, {"id", "help"}, {"textKey", "capture.settings.help"}},
                                  {{"type", "text"}, {"id", "status"}, {"textKey", "capture.settings.ready"}}})}}},
        {"footer",
         {{"leading", nlohmann::json::array()},
          {"trailing",
           nlohmann::json::array({{{"type", "button"}, {"id", "restore"}, {"textKey", "capture.settings.restore"}},
                                  {{"type", "button"}, {"id", "close"}, {"textKey", "capture.settings.close"}}})}}}};
    if (!CheckVisualRenderer(this->panel_->LoadLayout(layout)) ||
        !CheckVisualRenderer(this->panel_->SetTextResolver(
            // 状态文本由会话查询，其他文字统一转交本地化入口。
            // 入参：key 为文本键。返回：当前显示文字。
            [this](std::string_view key)
            {
                return this->text_(key == "capture.settings.ready" ? (!this->hdr_      ? "capture.settings.sdr_only"
                                                                      : this->pending_ ? "capture.settings.processing"
                                                                                       : "capture.settings.ready")
                                                                   : key);
            })))
        return false;
    for (const bool brightness : {true, false})
    {
        const char* id = brightness ? "brightness" : "mask";
        if (!CheckVisualRenderer(this->panel_->BindInteger(
                id,
                // 查询会话唯一临时值，公共控件只保存输入缓冲。
                // 入参：无。返回：当前整数。
                [this, brightness]()
                { return RendererIntegerResult{true, brightness ? this->brightness_ : this->mask_, {}}; },
                // 只接收合法完整整数；无效输入不覆盖当前效果。
                // 入参：value 为完整整数或空。返回：接受状态及本地化错误。
                [this, brightness](std::optional<std::int64_t> value)
                {
                    const bool valid =
                        value && (brightness ? IsHdrBrightnessPercent(*value) : IsMaskOpacityPercent(*value));
                    const bool accepted =
                        valid && this->Change(brightness ? static_cast<unsigned>(*value) : this->brightness_,
                                              brightness ? this->mask_ : static_cast<unsigned>(*value));
                    return RendererChangeResult{accepted, accepted ? L"" : this->text_("capture.settings.invalid")};
                })))
            return false;
    }
    if (!CheckVisualRenderer(this->panel_->BindAction("restore",
                                                      // 恢复本次启动快照，不重新读取全局默认。
                                                      // 入参：无。返回：无。
                                                      [this]()
                                                      {
                                                          if (this->Change(this->defaults_.brightness,
                                                                           this->defaults_.mask))
                                                              (void)this->panel_->RefreshValues();
                                                      })) ||
        !CheckVisualRenderer(this->panel_->BindAction("close", [this]() { (void)this->panel_->RequestClose(); })) ||
        !CheckVisualRenderer(this->panel_->SetCloseHandler([this]() { (void)this->panel_->RequestClose(); })))
        return false;
    // Enter只收起本次面板，已接受的参数继续有效。
    if (!CheckVisualRenderer(this->panel_->SetDefaultAction("close")))
        return false;
    const RendererWindowOptions options{owner, icon, SW_SHOWNORMAL, true};
    if (!CheckVisualRenderer(this->panel_->Show(options)))
        return false;
    RECT nearby{}, rectangle{}, work{};
    if (GetWindowRect(anchor ? anchor : owner, &nearby) && GetWindowRect(this->Window(), &rectangle))
    {
        MONITORINFO monitor{sizeof(monitor)};
        if (GetMonitorInfoW(MonitorFromRect(&nearby, MONITOR_DEFAULTTONEAREST), &monitor))
        {
            work = monitor.rcWork;
            const int x = std::clamp(nearby.left, work.left,
                                     std::max(work.left, work.right - (rectangle.right - rectangle.left)));
            const int y = std::clamp(nearby.bottom + 8, work.top,
                                     std::max(work.top, work.bottom - (rectangle.bottom - rectangle.top)));
            SetWindowPos(this->Window(), HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE);
        }
    }
    this->RefreshPanel();
    return true;
}
// 面板内Esc仅收起面板，其他键交公共表单处理。
// 入参：message 为线程消息。返回：已消费为 true。
bool CaptureVisualSession::Process(MSG& message)
{
    const HWND window = this->Window();
    if (!window || (message.hwnd != window && !IsChild(window, message.hwnd)))
        return false;
    if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE)
    {
        (void)this->panel_->RequestClose();
        return true;
    }
    return this->panel_->ProcessDialogMessage(message);
}
// 模态操作暂停滑条，恢复时保留临时值。
// 入参：enabled 为是否可操作。返回：无。
void CaptureVisualSession::SetInteractive(bool enabled)
{
    this->interactive_ = enabled;
    this->RefreshPanel();
}
// 只借用公共面板当前窗口。
// 入参：无。返回：借用句柄。
HWND CaptureVisualSession::Window() const noexcept
{
    return this->panel_ ? this->panel_->NativeHandle() : nullptr;
}
} // namespace open_st
