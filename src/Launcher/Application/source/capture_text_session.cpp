// 协调截图、OCR及翻译的发布资格；原文草稿归结果窗口，后台状态归领域模块。
#include "capture_text_session.h"
#include "diagnostic_text.h"
#include "text_result_window.h"
#include <log.h>
#include <sdr_selection_frame.h>
#include <stdexcept>
#include <ui_text.h>
#include <windows_util.h>
#ifdef OPEN_ST_HAS_OCR
#include <ocr_client.h>
#endif
#ifdef OPEN_ST_HAS_TRANSLATION
#include <translation_client.h>
#endif
namespace open_st
{
struct CaptureTextSession::Impl
{
    HWND owner{};
    HICON icon{};
    std::unique_ptr<TextResultWindow> window;
    std::function<std::optional<nlohmann::json>()> read;
    std::uint64_t request{}, seen{}, session{}, configurationRevision{};
    bool stopping{};
#ifdef OPEN_ST_HAS_OCR
    std::unique_ptr<OcrClient> client;
#endif
#ifdef OPEN_ST_HAS_TRANSLATION
    TranslationClient* translator{}; // 借用应用唯一客户端，应用须晚于本会话释放。
    std::uint64_t translationRequest{}, translationSeen{};
    std::optional<TranslationSettings> settings;
    // 将同一文件快照交给领域解释，读取失败不得使用系统代理默认值。
    // 入参：无。返回：独立配置；读取失败抛出无敏感数据的异常。
    TranslationSettings ReadConfiguration() const
    {
        if (!this->read)
            throw std::runtime_error("Translation settings reader unavailable");
        const std::optional<nlohmann::json> snapshot = this->read();
        if (!snapshot || !snapshot->is_object())
            throw std::runtime_error("Translation settings snapshot unavailable");
        return ReadTranslationSettings(
            // 各字段来自同一个固定对象，不再次读取文件。
            // 入参：key为领域键。返回：副本或缺失。
            [&snapshot](std::string_view key) -> std::optional<nlohmann::json>
            {
                const auto found = snapshot->find(key);
                return found == snapshot->end() ? std::nullopt : std::optional<nlohmann::json>(*found);
            });
    }
#endif
};
// 建立跨窗口存活的OCR所有者并借用翻译客户端，不加载模型或访问网络。
// 入参：owner/icon为宿主资源；read为设置读取适配。返回：空闲协调器。
CaptureTextSession::CaptureTextSession(HWND owner, HICON icon, std::function<std::optional<nlohmann::json>()> read,
                                       TranslationClient* translator)
    : impl_(std::make_unique<Impl>())
{
    this->impl_->owner = owner;
    this->impl_->icon = icon;
    this->impl_->read = std::move(read);
#ifdef OPEN_ST_HAS_OCR
    this->impl_->client = std::make_unique<OcrClient>(
        GetExecutableDirectory(),
        // 通知只携带编号，结果可定时补收。入参：id为请求编号。返回：无。
        [owner](std::uint64_t id) { (void)PostMessageW(owner, TextWakeMessage, static_cast<WPARAM>(id), 0); });
#endif
#ifdef OPEN_ST_HAS_TRANSLATION
    this->impl_->translator = translator;
#else
    (void)translator;
#endif
}
// 消息宿主销毁前回收任务；正常退出已异步等待后台释放。
// 入参：无。返回：无。
CaptureTextSession::~CaptureTextSession()
{
    KillTimer(this->impl_->owner, TextPollTimer);
}
// 固定正式图像，只执行OCR并填充可编辑原文。
// 入参：frame/model/language为OCR参数。返回：无，不发起翻译。
void CaptureTextSession::Begin(const SdrSelectionFrame& frame, const std::string& model, const std::string& language)
{
#ifdef OPEN_ST_HAS_OCR
    if (this->impl_->stopping || this->ActivateExisting())
        return;
    ++this->impl_->session;
    this->impl_->window = std::make_unique<TextResultWindow>();
    std::string source, target;
#ifdef OPEN_ST_HAS_TRANSLATION
    try
    {
        this->impl_->settings = this->impl_->ReadConfiguration();
        source = this->impl_->settings->options.sourceLanguage;
        target = this->impl_->settings->options.targetLanguage;
    }
    catch (...)
    {
        // 翻译配置失败不阻断纯本地OCR；提交翻译时必须重新取得有效完整快照。
        this->impl_->settings.reset();
    }
#endif
    TextResultCallbacks callbacks;
    // 关窗撤销资格，后台所有者继续存活。入参：无。返回：无。
    callbacks.close = [this]()
    {
        this->impl_->request = 0;
        this->impl_->client->Cancel();
        this->CancelTranslation();
    };
    // 编辑仅取消，不自动重发。入参：无。返回：无。
    callbacks.changed = [this]()
    {
        this->CancelTranslation();
        this->Poll();
    };
    // 翻译仅由结果窗按钮明确请求。入参：无。返回：无。
    callbacks.translate = [this]() { this->Translate(); };
    // 取消当前请求，不等待后台。入参：无。返回：无。
    callbacks.cancelTranslation = [this]() { this->CancelTranslation(); };
    if (!this->impl_->window->Show(this->impl_->owner, this->impl_->icon, std::move(callbacks), source, target))
    {
        this->impl_->window.reset();
        throw std::runtime_error("Cannot create text result window");
    }
    this->RefreshNetworkWarning();
    if (!SetTimer(this->impl_->owner, TextPollTimer, 100, nullptr))
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
    }
    this->impl_->seen = 0;
    this->Poll();
#else
    (void)frame;
    (void)model;
    (void)language;
#endif
}
// 查询当前结果窗口，不缓存可能关闭重开的原生句柄。
// 入参：无。返回：存活结果窗口或空。
HWND CaptureTextSession::ResultWindowHandle() const noexcept
{
    return this->impl_->window ? this->impl_->window->NativeHandle() : nullptr;
}
// 只激活现有结果，不隐式提交翻译。
// 入参：无。返回：存在窗口为true。
bool CaptureTextSession::ActivateExisting() noexcept
{
    if (!this->impl_->window || !this->impl_->window->IsOpen())
        return false;
    this->impl_->window->Activate();
    return true;
}
// 设置更改立即失效，不覆盖用户原文或临时语言。
// 入参：无。返回：无。
void CaptureTextSession::ConfigurationChanged() noexcept
{
    ++this->impl_->configurationRevision;
    this->CancelTranslation();
#ifdef OPEN_ST_HAS_TRANSLATION
    this->impl_->settings.reset();
#endif
    try
    {
        if (this->impl_->window && this->impl_->window->IsOpen())
        {
            this->impl_->window->ConfigurationChanged();
#ifdef OPEN_ST_HAS_TRANSLATION
            this->impl_->settings = this->impl_->ReadConfiguration();
#endif
            this->RefreshNetworkWarning();
        }
    }
    catch (...)
    {
        OPEN_ST_LOG_WARNING("Cannot refresh changed translation configuration.");
    }
}
// 固定唯一草稿和配置快照，旧任务未结束不接受新轮次。
// 入参：无。返回：无，失败保留草稿与旧译文。
void CaptureTextSession::Translate() noexcept
{
#ifdef OPEN_ST_HAS_TRANSLATION
    try
    {
        if (this->impl_->stopping || !this->impl_->translator || !this->impl_->window || !this->impl_->window->IsOpen())
            return;
        TextResultWindow& window = *this->impl_->window;
        if (!window.SourceReady())
            return;
        if (this->impl_->translator->Snapshot().busy)
        {
            window.Status(TranslationErrorTextKey(TranslationError::Busy));
            return;
        }
        TranslationSettings settings = this->impl_->ReadConfiguration();
        if (this->impl_->settings &&
            (this->impl_->settings->configuration.interfaces != settings.configuration.interfaces ||
             this->impl_->settings->configuration.proxyMode != settings.configuration.proxyMode ||
             this->impl_->settings->configuration.proxyAddress != settings.configuration.proxyAddress ||
             this->impl_->settings->options.sourceLanguage != settings.options.sourceLanguage ||
             this->impl_->settings->options.targetLanguage != settings.options.targetLanguage))
        {
            ++this->impl_->configurationRevision;
            window.ConfigurationChanged();
        }
        window.InitializeMissingLanguages(settings.options.sourceLanguage, settings.options.targetLanguage);
        this->impl_->settings = settings;
        this->RefreshNetworkWarning();
        if (!SetTimer(this->impl_->owner, TextPollTimer, 100, nullptr))
        {
            window.Status(TranslationErrorTextKey(TranslationError::Unavailable));
            return;
        }
        TranslationRequest request;
        request.text = window.SourceText();
        request.options = {window.SourceLanguage(), window.TargetLanguage()};
        request.configuration = std::move(settings.configuration);
        request.sessionId = this->impl_->session;
        request.sourceRevision = window.Revision();
        request.configurationRevision = this->impl_->configurationRevision;
        TranslationError error{};
        if (!this->impl_->translator->Submit(request, this->impl_->translationRequest, error))
            window.Status(TranslationErrorTextKey(error));
        this->impl_->translationSeen = 0;
        this->Poll();
    }
    catch (...)
    {
        this->CancelTranslation();
        try
        {
            if (this->impl_->window)
                this->impl_->window->Status("translation.error.configuration");
        }
        catch (...)
        {
        }
    }
#endif
}
// 取消立即撤销显示资格；后台实际收尾不在UI等待。
// 入参：无。返回：无。
void CaptureTextSession::CancelTranslation() noexcept
{
#ifdef OPEN_ST_HAS_TRANSLATION
    const bool active = this->impl_->translationRequest != 0;
    const std::uint64_t owned = this->impl_->translationRequest;
    this->impl_->translationRequest = 0;
    try
    {
        if (active && this->impl_->translator && this->impl_->translator->Snapshot().requestId == owned)
            this->impl_->translator->Cancel();
        if (active && this->impl_->window && this->impl_->window->IsOpen())
            this->impl_->window->Status(TranslationErrorTextKey(TranslationError::Cancelled));
    }
    catch (...)
    {
    }
#endif
}
// 用同一领域协议判断明文传输，界面保持提示直至配置改变。
// 入参：无。返回：无，不校验或加载本地模型。
void CaptureTextSession::RefreshNetworkWarning()
{
#ifdef OPEN_ST_HAS_TRANSLATION
    if (!this->impl_->window || !this->impl_->window->IsOpen())
        return;
    if (!this->impl_->settings)
        return;
    bool insecure = false;
    const nlohmann::json& profiles = this->impl_->settings->configuration.interfaces;
    if (profiles.is_array())
        for (const nlohmann::json& profile : profiles)
            if (profile.is_object() && profile.contains("enabled") && profile["enabled"] == true &&
                TranslationProfileUsesInsecureHttp(profile))
                insecure = true;
    this->impl_->window->SetNetworkWarning(insecure);
#endif
}
// 截图关闭同步取消OCR及翻译，延迟销毁结果窗口。
// 入参：无。返回：无。
void CaptureTextSession::EndCapture() noexcept
{
    this->impl_->request = 0;
    this->CancelTranslation();
    if (this->impl_->window)
        this->impl_->window->Close();
#ifdef OPEN_ST_HAS_OCR
    this->impl_->client->Cancel();
#endif
}
// 按编号和修订分派，OCR只首次填充，状态不刷新编辑控件。
// 入参：无。返回：无，异常不逃出主循环。
void CaptureTextSession::Poll() noexcept
{
    try
    {
        bool busy = false;
        const bool open = this->impl_->window && this->impl_->window->IsOpen();
        if (!open && this->impl_->request != 0)
            this->EndCapture();
#ifdef OPEN_ST_HAS_TRANSLATION
        if (!open && this->impl_->translationRequest != 0)
            this->CancelTranslation();
#endif
#ifdef OPEN_ST_HAS_OCR
        const OcrSnapshot snapshot = this->impl_->client->Snapshot();
        busy = snapshot.busy;
        if (open && this->impl_->request != 0 && snapshot.requestId == this->impl_->request &&
            snapshot.revision != this->impl_->seen)
        {
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
                    this->impl_->window->SetSource(*snapshot.text);
                status = "ocr.completed";
                this->impl_->request = 0;
                break;
            case OcrPhase::Empty:
                status = "ocr.empty";
                this->impl_->request = 0;
                break;
            case OcrPhase::Failed:
                status = snapshot.error == OcrError::ModelMissing || snapshot.error == OcrError::ModelIntegrity
                             ? "ocr.model_error"
                         : snapshot.error == OcrError::ResultTooLarge ? "ocr.too_large"
                                                                      : "ocr.failed";
                this->impl_->request = 0;
                break;
            case OcrPhase::Cancelled:
                status = "ocr.cancelled";
                this->impl_->request = 0;
                break;
            default:
                break;
            }
            this->impl_->window->Status(status);
        }
#endif
#ifdef OPEN_ST_HAS_TRANSLATION
        const TranslationSnapshot translation =
            this->impl_->translator ? this->impl_->translator->Snapshot() : TranslationSnapshot{};
        busy = busy || translation.busy;
        if (open)
        {
            TextResultWindow& window = *this->impl_->window;
            window.TranslationActivity(translation.busy);
            if (this->impl_->translationRequest != 0 && translation.requestId == this->impl_->translationRequest &&
                translation.revision != this->impl_->translationSeen)
            {
                this->impl_->translationSeen = translation.revision;
                if (translation.phase == TranslationPhase::Succeeded && translation.result)
                {
                    const TranslationResult& result = *translation.result;
                    if (result.requestId == this->impl_->translationRequest &&
                        result.sessionId == this->impl_->session && result.sourceRevision == window.Revision() &&
                        result.configurationRevision == this->impl_->configurationRevision &&
                        result.options.sourceLanguage == window.SourceLanguage() &&
                        result.options.targetLanguage == window.TargetLanguage())
                    {
                        window.SetTranslation(
                            result.text, Utf8ToWide(result.profileName) + L" · " +
                                             GetUiText(result.network ? "translation.online" : "translation.local"));
                        window.Status("translation.completed");
                    }
                }
                else if (translation.phase == TranslationPhase::Running)
                    window.StatusText(GetUiText("translation.attempting",
                                                {{L"name", Utf8ToWide(translation.currentProfileName)},
                                                 {L"index", std::to_wstring(translation.currentIndex)},
                                                 {L"total", std::to_wstring(translation.totalInterfaces)}}));
                else if (translation.phase == TranslationPhase::Failed ||
                         translation.phase == TranslationPhase::Cancelled)
                {
                    std::wstring text = GetUiText(TranslationErrorTextKey(translation.error));
                    for (const TranslationAttempt& attempt : translation.attempts)
                        text += L"\n" + Utf8ToWide(attempt.profileName) + L": " +
                                GetUiText(TranslationErrorTextKey(attempt.error));
                    if (translation.unattempted != 0)
                        text += L"\n" + GetUiText("translation.unattempted",
                                                  {{L"count", std::to_wstring(translation.unattempted)}});
                    window.StatusText(std::move(text));
                }
            }
        }
#endif
        if (!busy && (!this->impl_->stopping || this->ShutdownComplete()))
            KillTimer(this->impl_->owner, TextPollTimer);
    }
    catch (...)
    {
        OPEN_ST_LOG_WARNING("Text result state refresh failed.");
    }
}
// 复用窗口消息导航。入参：message为主循环消息。返回：消费为true。
bool CaptureTextSession::Process(MSG& message)
{
    return this->impl_->window && this->impl_->window->Process(message);
}
// 转交已核验全局编辑组合。入参：modifiers/key为组合。返回：消费为true。
bool CaptureTextSession::RegisteredHotkey(UINT modifiers, UINT key) noexcept
{
    return this->impl_->window && this->impl_->window->RegisteredHotkey(modifiers, key);
}
// 请求后台释放，不在UI线程等待。入参：无。返回：无。
void CaptureTextSession::Shutdown() noexcept
{
    this->impl_->stopping = true;
    this->EndCapture();
#ifdef OPEN_ST_HAS_OCR
    this->impl_->client->RequestShutdown();
#endif
    if (!this->ShutdownComplete())
        (void)SetTimer(this->impl_->owner, TextPollTimer, 100, nullptr);
}
// 只查询本会话拥有的OCR后台；共享翻译的退出等待由App执行。入参：无。返回：完成为true。
bool CaptureTextSession::ShutdownComplete() const noexcept
{
#ifdef OPEN_ST_HAS_OCR
    if (!this->impl_->client->ShutdownComplete())
        return false;
#endif
    return true;
}
// 参与统一模态输入门禁。入参：paused为暂停标志。返回：无。
void CaptureTextSession::Pause(bool paused) noexcept
{
    if (this->impl_->window)
        this->impl_->window->Pause(paused);
}
} // namespace open_st
