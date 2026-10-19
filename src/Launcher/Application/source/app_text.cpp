// 编排截图文字与独立文本窗口，两类会话借用应用唯一翻译客户端。
#include "app_translation_state.h"
#include "capture_annotation_state.h"
#include "text_session.h"
#include <app.h>
#include <log.h>
#include <sdr_selection_frame.h>
#include <selection_model.h>
#include <settings.h>
#include <stdexcept>
#include <ui_text.h>
#include <windows_util.h>
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
        (void)this->EnsureTextSession(this->textSession_, CaptureTextPollTimer);
        if (this->textSession_->ActivateExisting())
        {
            this->RefreshCaptureToolbar();
            return;
        }
        // OCR等待最新底图发布后才固定输入。
        // 入参：无。返回：无。
        if (this->DeferCaptureOutput([this]() { this->ProcessSelectionText(); }))
            return;
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
        this->textSession_->BeginOcr(frame, GetStringSetting("ocr.model").value_or("fast"),
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
// 复用配置快照适配及唯一客户端，OCR资源由会话在真正识别时建立。
// 入参：session为实例所有权槽；pollTimer为本实例补收计时器。返回：借用会话，分配失败抛异常。
TextSession& App::EnsureTextSession(std::unique_ptr<TextSession>& session, UINT_PTR pollTimer)
{
    if (!session)
    {
        std::function<std::optional<nlohmann::json>()> read;
#ifdef OPEN_ST_HAS_TRANSLATION
        // 同一提交使用完整领域快照，读取失败不得回退到另一条代理路径。
        // 入参：无。返回：完整配置副本或失败空值。
        read = []() { return ReadSettingsSnapshot(TranslationSettingKeys()); };
#endif
        session = std::make_unique<TextSession>(this->messageWindow_, this->largeIcon_, pollTimer, std::move(read),
                                                this->EnsureTranslationClient());
    }
    return *session;
}
// 托盘手动入口独立于截图，重复打开保留草稿和原生编辑历史。
// 入参：无。返回：无；截图或模态忙期间不显示，创建失败使用统一提示。
void App::ShowTranslationWindow()
{
#ifdef OPEN_ST_HAS_TRANSLATION
    if (this->CompletionBusy() || this->dialogActive_ || this->welcoming_ || this->shuttingDown_ ||
        this->settingsBusy_ || this->hotkeyRecording_ || this->overlayPreparing_ || this->overlayRendering_ ||
        this->annotationPreparing_ || this->overlaySession_)
        return;
    try
    {
        this->EnsureTextSession(this->translationWindowSession_, StandaloneTextPollTimer).BeginText();
        this->UpdateCaptureGate();
    }
    catch (...)
    {
        OPEN_ST_LOG_WARNING("Cannot open standalone text translation window.");
        this->ShowSimpleMessage([]() { return GetUiText("translation.open_failed"); },
                                []() { return GetUiText("translation.manual.title"); }, MB_OK | MB_ICONERROR);
    }
#endif
}
// 同一唤醒补收两个实例，避免依赖后台通知到达顺序。
// 入参：无。返回：无，各会话自行核验请求和修订。
void App::PollTextSessions() noexcept
{
    if (this->textSession_)
        this->textSession_->Poll();
    if (this->translationWindowSession_)
        this->translationWindowSession_->Poll();
}
// 关闭两个窗口并撤销各自请求资格，共享客户端仍由App统一停止。
// 入参：无。返回：无，不在消息线程等待后台。
void App::ShutdownTextSessions() noexcept
{
    if (this->textSession_)
        this->textSession_->Shutdown();
    if (this->translationWindowSession_)
        this->translationWindowSession_->Shutdown();
}
// 组合两个会话真实后台状态，不另存忙状态镜像。
// 入参：无。返回：两个会话都可析构为true。
bool App::TextSessionsShutdownComplete() const noexcept
{
    return (!this->textSession_ || this->textSession_->ShutdownComplete()) &&
           (!this->translationWindowSession_ || this->translationWindowSession_->ShutdownComplete());
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
