// 协调独立识别快照与结果窗口；引擎状态由OCR模块唯一拥有。
#include "ocr_session.h"
#include "ocr_result_window.h"
#include <log.h>
#include <sdr_selection_frame.h>
#include <stdexcept>
#include <windows_util.h>
#ifdef OPEN_ST_HAS_OCR
#include <ocr_client.h>
#endif
namespace open_st
{
struct OcrSession::Impl
{
    HWND owner{};
    HICON icon{};
    std::unique_ptr<OcrResultWindow> window;
    std::uint64_t request{}, seen{};
    bool stopping{};
#ifdef OPEN_ST_HAS_OCR
    std::unique_ptr<OcrClient> client;
#endif
};
// 建立不加载模型的协调组件。
// 入参：owner/icon为宿主资源。返回：可开始会话的对象。
OcrSession::OcrSession(HWND owner, HICON icon) : impl_(std::make_unique<Impl>())
{
    this->impl_->owner = owner;
    this->impl_->icon = icon;
#ifdef OPEN_ST_HAS_OCR
    this->impl_->client = std::make_unique<OcrClient>(
        GetExecutableDirectory(),
        // 只投递无对象地址的通知，真正结果可在定时补收时取得。
        // 入参：id为请求编号。返回：无。
        [owner](std::uint64_t id) { (void)PostMessageW(owner, OcrWakeMessage, static_cast<WPARAM>(id), 0); });
#endif
}
// 稳定消息窗口销毁之前回收任务。
// 入参：无。返回：无。
OcrSession::~OcrSession()
{
    KillTimer(this->impl_->owner, OcrPollTimer);
}
// 提交独立图像，不改变截图的选区或历史。
// 入参：frame/model/language为本次快照。返回：无。
void OcrSession::Begin(const SdrSelectionFrame& frame, const std::string& model, const std::string& language)
{
#ifdef OPEN_ST_HAS_OCR
    if (this->ActivateExisting())
        return;
    this->impl_->window = std::make_unique<OcrResultWindow>();
    if (!this->impl_->window->Show(this->impl_->owner, this->impl_->icon,
                                   // 窗口取消只使请求失效，不join工作线程。
                                   // 入参：无。返回：无。
                                   [this]()
                                   {
                                       this->impl_->request = 0;
                                       this->impl_->client->Cancel();
                                   }))
    {
        this->impl_->window.reset();
        throw std::runtime_error("Cannot create OCR result window");
    }
    if (!SetTimer(this->impl_->owner, OcrPollTimer, 100, nullptr))
    {
        this->impl_->window->Status("ocr.unavailable");
        return;
    }
    OcrError error{};
    const OcrImageView image{static_cast<std::uint32_t>(frame.Bounds().Width()),
                             static_cast<std::uint32_t>(frame.Bounds().Height()), frame.Stride(),
                             std::as_bytes(frame.Pixels())};
    if (!this->impl_->client->Submit(image, {model, language}, this->impl_->request, error))
    {
        this->impl_->window->Status(error == OcrError::Busy             ? "ocr.busy"
                                    : error == OcrError::InvalidInput   ? "ocr.too_large"
                                    : error == OcrError::InvalidOptions ? "settings.ocr.invalid"
                                                                        : "ocr.unavailable");
        return;
    }
    this->impl_->seen = 0;
    this->Poll();
#else
    (void)frame;
    (void)model;
    (void)language;
#endif
}
// 激活唯一结果窗口。
// 入参：无。返回：窗口仍存活为true。
bool OcrSession::ActivateExisting() noexcept
{
    if (!this->impl_->window || !this->impl_->window->IsOpen())
        return false;
    this->impl_->window->Activate();
    return true;
}
// 截图关闭立即失效，窗口以公共延迟机制关闭。
// 入参：无。返回：无。
void OcrSession::EndCapture() noexcept
{
    this->impl_->request = 0;
    if (this->impl_->window)
        this->impl_->window->Close();
#ifdef OPEN_ST_HAS_OCR
    this->impl_->client->Cancel();
#endif
}
// 消费状态代次，完成后只写一次草稿。
// 入参：无。返回：无，异常不逃出主循环。
void OcrSession::Poll() noexcept
{
#ifdef OPEN_ST_HAS_OCR
    try
    {
        const auto snapshot = this->impl_->client->Snapshot();
        if (!snapshot.busy && (!this->impl_->stopping || this->impl_->client->ShutdownComplete()))
            KillTimer(this->impl_->owner, OcrPollTimer);
        if (!this->impl_->window || !this->impl_->window->IsOpen())
        {
            if (this->impl_->request != 0)
                this->EndCapture();
            return;
        }
        if (snapshot.requestId != this->impl_->request || snapshot.revision == this->impl_->seen)
            return;
        this->impl_->seen = snapshot.revision;
        std::string_view status = "ocr.preparing";
        switch (snapshot.phase)
        {
        case OcrPhase::Loading:
            status = "ocr.loading";
            break;
        case OcrPhase::Recognizing:
            status = "ocr.recognizing";
            break;
        case OcrPhase::Succeeded:
            if (snapshot.text)
                this->impl_->window->SetResult(*snapshot.text);
            status = "ocr.completed";
            break;
        case OcrPhase::Empty:
            status = "ocr.empty";
            break;
        case OcrPhase::Failed:
            status = snapshot.error == OcrError::ModelMissing || snapshot.error == OcrError::ModelIntegrity
                         ? "ocr.model_error"
                     : snapshot.error == OcrError::ResultTooLarge ? "ocr.too_large"
                                                                  : "ocr.failed";
            break;
        case OcrPhase::Cancelled:
            status = "ocr.cancelled";
            break;
        default:
            break;
        }
        this->impl_->window->Status(status);
    }
    catch (...)
    {
        OPEN_ST_LOG_WARNING("OCR UI state refresh failed.");
    }
#endif
}
// 复用窗口消息导航。
// 入参：message为主循环消息。返回：消费结果。
bool OcrSession::Process(MSG& message)
{
    return this->impl_->window && this->impl_->window->Process(message);
}
// 全局注册热键的文字归属适配。
// 入参：modifiers/key为组合。返回：消费结果。
bool OcrSession::RegisteredHotkey(UINT modifiers, UINT key) noexcept
{
    return this->impl_->window && this->impl_->window->RegisteredHotkey(modifiers, key);
}
// 请求后台收尾，不在当前UI调用等待。
// 入参：无。返回：无。
void OcrSession::Shutdown() noexcept
{
    this->impl_->stopping = true;
    this->EndCapture();
#ifdef OPEN_ST_HAS_OCR
    this->impl_->client->RequestShutdown();
    if (!this->impl_->client->ShutdownComplete())
        (void)SetTimer(this->impl_->owner, OcrPollTimer, 100, nullptr);
#endif
}
// 判断后台线程和通知是否全部释放。
// 入参：无。返回：可退出为true。
bool OcrSession::ShutdownComplete() const noexcept
{
#ifdef OPEN_ST_HAS_OCR
    return this->impl_->client->ShutdownComplete();
#else
    return true;
#endif
}
// 结果窗参与所有既有模态入口的统一禁用。
// 入参：paused为输入暂停状态。返回：无。
void OcrSession::Pause(bool paused) noexcept
{
    if (this->impl_->window)
        this->impl_->window->Pause(paused);
}
} // namespace open_st
