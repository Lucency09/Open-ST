// 将截图完成入口适配到独立OCR会话，模型及线程状态不进入App。
#include "capture_annotation_state.h"
#include "capture_text_session.h"
#include "app_translation_state.h"
#include <windows_util.h>
#include <app.h>
#include <log.h>
#include <sdr_selection_frame.h>
#include <selection_model.h>
#include <settings.h>
#include <stdexcept>
#include <ui_text.h>
#ifdef OPEN_ST_HAS_TRANSLATION
#include <translation_client.h>
#endif
namespace open_st
{
// 固定当前选区与已提交标注，生成后立即释放编辑准入锁。
// 入参：无。返回：无，异步识别不占用截图完成忙状态。
void App::ProcessSelectionText()
{
#ifdef OPEN_ST_HAS_OCR
    if (!this->CanSubmitToolbarCommand())
        return;
    try
    {
        if (!this->textSession_)
        {
            std::function<std::optional<nlohmann::json>()> read;
#ifdef OPEN_ST_HAS_TRANSLATION
            // 一次取得全部领域设置；失败不得降级到默认代理。
            // 入参：无。返回：完整配置快照，读取失败为空。
            read = []() { return ReadSettingsSnapshot(TranslationSettingKeys()); };
#endif
            this->textSession_ = std::make_unique<CaptureTextSession>(this->messageWindow_, this->largeIcon_,
                                                                      std::move(read), this->EnsureTranslationClient());
        }
        if (this->textSession_->ActivateExisting())
        {
            this->RefreshCaptureToolbar();
            return;
        }
        SdrSelectionFrame frame;
        std::wstring error;
        this->completionBusy_ = true;
        this->UpdateCaptureGate();
        const auto selection = this->selectionModel_->Snapshot().rectangle;
        const auto annotations = this->annotation_ ? this->annotation_->Committed() : AnnotationSnapshot{};
        const bool generated = this->GenerateAnnotatedSelection(selection, annotations, frame, error);
        this->completionBusy_ = false;
        this->UpdateCaptureGate();
        if (this->overlayInvalidated_ || this->shuttingDown_)
        {
            this->CloseOverlay();
            return;
        }
        if (!generated)
            throw std::runtime_error("OCR input generation failed");
        this->textSession_->Begin(frame, GetStringSetting("ocr.model").value_or("fast"),
                                  GetStringSetting("ocr.language").value_or("chi_sim+eng+jpn"));
        this->RefreshCaptureToolbar();
    }
    catch (...)
    {
        this->completionBusy_ = false;
        this->UpdateCaptureGate();
        OPEN_ST_LOG_WARNING("Cannot prepare OCR interaction.");
        this->ShowSimpleMessage([]() { return GetUiText("ocr.unavailable"); }, []() { return GetUiText("ocr.title"); },
                                MB_OK | MB_ICONERROR);
    }
#endif
}
// 唯一客户端按需建立；通知和补收计时器只依赖稳定宿主，不借用编辑窗口。
// 入参：无。返回：借用客户端，关闭翻译功能时为空。
TranslationClient* App::EnsureTranslationClient()
{
#ifdef OPEN_ST_HAS_TRANSLATION
    if (!this->translation_)
        this->translation_ = std::make_unique<AppTranslationState>();
    if (!this->translation_->client)
        this->translation_->client = std::make_unique<TranslationClient>(
            GetExecutableDirectory(),
            // 编号唤醒由客户端生成，窗口只查询自己的发布资格。
            // 入参：id 为请求编号。返回：无。
            [owner = this->messageWindow_](std::uint64_t id)
            { (void)PostMessageW(owner, TextWakeMessage, static_cast<WPARAM>(id), 0); });
    return this->translation_->client.get();
#else
    return nullptr;
#endif
}
// 所有翻译调用者共用一次退出，不因截图窗口关闭停止客户端。
// 入参：无。返回：无。
void App::ShutdownTranslation() noexcept
{
#ifdef OPEN_ST_HAS_TRANSLATION
    if (this->translation_ && this->translation_->client)
    {
        this->translation_->probeRequest = 0;
        this->translation_->client->RequestShutdown();
        if (!this->translation_->client->ShutdownComplete())
            (void)SetTimer(this->messageWindow_, TranslationPollTimer, 100, nullptr);
    }
#endif
}
// 应用级退出等待包含接口测试和普通翻译的同一个后台。
// 入参：无。返回：可安全析构为真。
bool App::TranslationShutdownComplete() const noexcept
{
#ifdef OPEN_ST_HAS_TRANSLATION
    return !this->translation_ || !this->translation_->client || this->translation_->client->ShutdownComplete();
#else
    return true;
#endif
}
} // namespace open_st
