// 复用公共表单展示唯一原文与只读译文，不拥有识别或翻译任务。
#include "text_result_window.h"
#include "diagnostic_text.h"
#include <clipboard_writer.h>
#include <ui_text.h>
#include <window_renderer.h>
#ifdef OPEN_ST_HAS_TRANSLATION
#include <translation_client.h>
#endif
namespace open_st
{
// 建立空草稿。入参：无。返回：未显示对象。
TextResultWindow::TextResultWindow() = default;
// 回收原生窗口和业务草稿。入参：无。返回：无。
TextResultWindow::~TextResultWindow() = default;
// 绑定公共窗口控件，业务回调仅通知协调器。
// 入参：owner/icon为稳定宿主资源，callbacks为动作，source/target为语言。返回：创建成功为true。
bool TextResultWindow::Show(HWND owner, HICON icon, TextResultCallbacks callbacks, std::string source,
                            std::string target)
{
    this->closing_ = false;
    this->callbacks_ = std::move(callbacks);
    this->source_ = std::move(source);
    this->target_ = std::move(target);
    this->renderer_ = std::make_unique<WindowRenderer>();
    WindowRenderer& renderer = *this->renderer_;
    nlohmann::json layout = nlohmann::json::parse(R"({
      "schemaVersion":1,"window":{"titleKey":"ocr.title","initialSize":[620,520],"minSize":[440,360],"resizable":true},
      "content":{"type":"column","id":"body","padding":16,"gap":12,"children":[
        {"type":"text","id":"hint","textKey":"ocr.snapshot_help"},
        {"type":"edit","id":"result","labelKey":"ocr.result","multiline":true,"visibleLines":14,"verticalScroll":true,"maxLength":1000000,"width":"fill"}]},
      "footer":{"leading":[{"type":"button","id":"copy","textKey":"ocr.copy_all"}],
        "trailing":[{"type":"button","id":"close","textKey":"ocr.close"}]}})");
#ifdef OPEN_ST_HAS_TRANSLATION
    layout["window"]["titleKey"] = "translation.window.title";
    layout["window"]["initialSize"] = {900, 640};
    layout["window"]["minSize"] = {720, 480};
    auto& body = layout["content"]["children"];
    const auto original = body[1];
    body.erase(1);
    body.push_back(nlohmann::json::parse(R"({"type":"row","id":"languages","gap":16,"children":[
      {"type":"select","id":"sourceLanguage","labelKey":"translation.source","width":"fill"},
      {"type":"select","id":"targetLanguage","labelKey":"translation.target","width":"fill"}]})"));
    body.push_back({{"type", "text"}, {"id", "networkHint"}, {"textKey", "translation.network_hint"}});
    body.push_back({{"type", "row"},
                    {"id", "texts"},
                    {"gap", 16},
                    {"children", nlohmann::json::array({original,
                                                        {{"type", "edit"},
                                                         {"id", "translated"},
                                                         {"labelKey", "translation.result"},
                                                         {"multiline", true},
                                                         {"readOnly", true},
                                                         {"visibleLines", 14},
                                                         {"verticalScroll", true},
                                                         {"maxLength", 1000000},
                                                         {"width", "fill"}}})}});
    body.push_back({{"type", "text"}, {"id", "provenance"}, {"textKey", "translation.provenance"}});
    auto& actions = layout["footer"]["leading"];
    actions[0]["textKey"] = "translation.copy_source";
    actions.push_back({{"type", "button"}, {"id", "copyTranslation"}, {"textKey", "translation.copy_result"}});
    actions.push_back({{"type", "button"}, {"id", "translate"}, {"textKey", "translation.action"}});
    actions.push_back({{"type", "button"}, {"id", "cancelTranslation"}, {"textKey", "translation.cancel"}});
#endif
    if (!renderer.LoadLayout(layout) ||
        !renderer.SetTextResolver(
            // 只动态组合安全的来源/过期说明，不把原文或凭据放入状态。
            // 入参：key为资源键。返回：当前语言文本。
            [this](std::string_view key)
            {
                if (key == "translation.provenance")
                {
                    if (this->translated_.empty())
                        return GetUiText("translation.no_result");
                    const std::wstring origin = GetUiText("translation.origin", {{L"name", this->origin_}});
                    return this->translatedRevision_ == this->revision_
                               ? origin
                               : origin + L"  " + GetUiText("translation.stale");
                }
                if (key == "translation.network_hint")
                    return GetUiText("translation.route_help") +
                           (this->insecure_ ? L"\n" + GetUiText("translation.http_warning") : L"");
                return GetUiText(key);
            }) ||
        !renderer.BindString(
            "result",
            // 读取唯一原文草稿。入参：无。返回：UTF8正文。
            [this]() { return RendererStringResult{true, this->draft_, {}}; },
            // 原文变化撤销整轮资格，原生撤销仍由控件持有。
            // 入参：value为当前原文。返回：接受结果。
            [this](std::string_view value)
            {
                if (this->draft_ != value)
                {
                    this->draft_ = value;
                    this->Changed();
                }
                (void)this->renderer_->SetEnabled("copy", !this->draft_.empty());
                return RendererChangeResult{};
            }))
        return false;
#ifdef OPEN_ST_HAS_TRANSLATION
    for (const bool isSource : {true, false})
    {
        const std::string_view id = isSource ? "sourceLanguage" : "targetLanguage";
        if (!renderer.BindString(
                id,
                // 查询本窗口临时语言。入参：无。返回：选择值。
                [this, isSource]()
                {
                    const std::string& selected = isSource ? this->source_ : this->target_;
                    return RendererStringResult{!selected.empty(), selected, {}};
                },
                // 改语言仅取消，不自动重发。入参：value为候选。返回：接受状态。
                [this, isSource](std::string_view value)
                {
                    std::string& selected = isSource ? this->source_ : this->target_;
                    if (selected != value)
                    {
                        selected = value;
                        this->Changed();
                    }
                    return RendererChangeResult{};
                }) ||
            !renderer.BindOptions(
                id,
                // 合法语言集合仅来自领域目录。入参：无。返回：本地化候选。
                [isSource]()
                {
                    RendererOptionsResult result;
                    for (auto value :
                         TranslationChoices(isSource ? "translation.source_language" : "translation.target_language"))
                        result.options.push_back(
                            {std::string(value), GetUiText("translation.language." + std::string(value))});
                    return result;
                }))
            return false;
    }
    if (!renderer.BindString("translated",
                             // 只读输出只允许宿主填充。入参：无。返回：显示译文。
                             [this]() { return RendererStringResult{true, this->translated_, {}}; }, {}) ||
        !renderer.BindAction("copyTranslation", [this]() { this->Copy(this->translated_); }) ||
        !renderer.BindAction("translate",
                             [this]()
                             {
                                 if (this->callbacks_.translate)
                                     this->callbacks_.translate();
                             }) ||
        !renderer.BindAction("cancelTranslation",
                             [this]()
                             {
                                 if (this->callbacks_.cancelTranslation)
                                     this->callbacks_.cancelTranslation();
                             }))
        return false;
#endif
    if (!renderer.BindAction("copy", [this]() { this->Copy(this->draft_); }) ||
        !renderer.BindAction("close", [this]() { this->Close(); }) ||
        !renderer.SetCloseHandler([this]() { this->Close(); }) || !renderer.SetDefaultAction("close"))
        return false;
    if (!renderer.Show({owner, icon, SW_SHOWNORMAL, true}))
        return false;
    (void)renderer.SetEnabled("result", false);
    (void)renderer.SetEnabled("copy", false);
#ifdef OPEN_ST_HAS_TRANSLATION
    (void)renderer.SetEnabled("copyTranslation", false);
    this->TranslationActivity(false);
#endif
    this->Status("ocr.preparing");
    return true;
}
// 仅在首次配置读取失败后补入已恢复的设置，不覆盖用户临时选择。
// 入参：source/target为宿主验证过的配置语言。返回：无。
void TextResultWindow::InitializeMissingLanguages(const std::string& source, const std::string& target)
{
    if (this->source_.empty())
    {
        this->source_ = source;
        (void)this->renderer_->RefreshValue("sourceLanguage");
    }
    if (this->target_.empty())
    {
        this->target_ = target;
        (void)this->renderer_->RefreshValue("targetLanguage");
    }
}
// 原文/选择变化统一推进修订并保留旧译文。入参：无。返回：无。
void TextResultWindow::Changed()
{
    ++this->revision_;
    if (this->callbacks_.changed)
        this->callbacks_.changed();
    (void)this->renderer_->RefreshTexts();
}
// 复用系统文本复制。入参：text为UTF8草稿。返回：无。
void TextResultWindow::Copy(const std::string& text)
{
    const std::wstring wide = Utf8ToWide(text);
    std::wstring error;
    this->Status(!text.empty() && !wide.empty() && CopyTextToClipboard(this->renderer_->NativeHandle(), wide, error)
                     ? "ocr.copied"
                     : "ocr.copy_failed");
}
// 查询并显示安全资源文字。入参：key。返回：无。
void TextResultWindow::Status(std::string_view key)
{
    this->StatusText(GetUiText(key));
}
// 更新状态而不刷新正文。入参：text。返回：无。
void TextResultWindow::StatusText(std::wstring text)
{
    if (this->renderer_)
        (void)this->renderer_->SetStatus(std::move(text));
}
// 设置一次OCR结果，程序填充不触发网络请求。入参：text。返回：无。
void TextResultWindow::SetSource(std::wstring_view text)
{
    this->draft_ = WideToUtf8(text);
    this->sourceReady_ = true;
    ++this->revision_;
    (void)this->renderer_->RefreshValue("result");
    (void)this->renderer_->SetEnabled("result", true);
    (void)this->renderer_->SetEnabled("copy", !this->draft_.empty());
    this->TranslationActivity(false);
}
// 发布完整译文并记录其对应修订。入参：text/origin。返回：无。
void TextResultWindow::SetTranslation(std::string text, std::wstring origin)
{
    this->translated_ = std::move(text);
    this->origin_ = std::move(origin);
    this->translatedRevision_ = this->revision_;
#ifdef OPEN_ST_HAS_TRANSLATION
    (void)this->renderer_->RefreshValue("translated");
    (void)this->renderer_->SetEnabled("copyTranslation", !this->translated_.empty());
    (void)this->renderer_->RefreshTexts();
#endif
}
// 配置改变使已有译文过期。入参：无。返回：无。
void TextResultWindow::ConfigurationChanged()
{
    this->Changed();
}
// 仅把权威任务状态投影为控件可用性。入参：busy。返回：无。
void TextResultWindow::TranslationActivity(bool busy)
{
#ifdef OPEN_ST_HAS_TRANSLATION
    if (this->IsOpen())
    {
        (void)this->renderer_->SetEnabled("translate", !busy && this->sourceReady_ && !this->draft_.empty());
        (void)this->renderer_->SetEnabled("cancelTranslation", busy);
    }
#else
    (void)busy;
#endif
}
// 明文传输提示始终由当前配置决定。入参：insecure。返回：无。
void TextResultWindow::SetNetworkWarning(bool insecure)
{
    this->insecure_ = insecure;
    if (this->IsOpen())
        (void)this->renderer_->RefreshTexts();
}
// 提供只读原文。入参：无。返回：草稿引用。
const std::string& TextResultWindow::SourceText() const noexcept
{
    return this->draft_;
}
// 提供临时源语言。入参：无。返回：引用。
const std::string& TextResultWindow::SourceLanguage() const noexcept
{
    return this->source_;
}
// 提供临时目标语言。入参：无。返回：引用。
const std::string& TextResultWindow::TargetLanguage() const noexcept
{
    return this->target_;
}
// 查询完整输入修订。入参：无。返回：修订号。
std::uint64_t TextResultWindow::Revision() const noexcept
{
    return this->revision_;
}
// 查询原文可编辑状态。入参：无。返回：就绪为true。
bool TextResultWindow::SourceReady() const noexcept
{
    return this->sourceReady_;
}
// 先取消再异步关闭。入参：无。返回：无。
void TextResultWindow::Close() noexcept
{
    if (this->closing_)
        return;
    this->closing_ = true;
    try
    {
        if (this->callbacks_.close)
            this->callbacks_.close();
        if (this->renderer_)
            (void)this->renderer_->RequestClose();
    }
    catch (...)
    {
    }
}
// 激活存活结果。入参：无。返回：无。
void TextResultWindow::Activate() noexcept
{
    if (this->IsOpen())
    {
        ShowWindow(this->renderer_->NativeHandle(), SW_RESTORE);
        SetForegroundWindow(this->renderer_->NativeHandle());
    }
}
// 提供借用原生句柄，关闭中的窗口不再参与宿主排序。
// 入参：无。返回：当前结果窗口或空。
HWND TextResultWindow::NativeHandle() const noexcept
{
    return this->IsOpen() ? this->renderer_->NativeHandle() : nullptr;
}
// 查询原生寿命。入参：无。返回：存活为true。
bool TextResultWindow::IsOpen() const noexcept
{
    return !this->closing_ && this->renderer_ && IsWindow(this->renderer_->NativeHandle());
}
// 复用公共消息路由。入参：message。返回：消费为true。
bool TextResultWindow::Process(MSG& message)
{
    return this->renderer_ && this->renderer_->ProcessDialogMessage(message);
}
// 注册组合只在实际前台结果窗生效。入参：modifiers/key。返回：该窗口是否消费。
bool TextResultWindow::RegisteredHotkey(UINT modifiers, UINT key) noexcept
{
    if (!this->renderer_ || GetForegroundWindow() != this->renderer_->NativeHandle())
        return false;
    if (IsWindowEnabled(this->renderer_->NativeHandle()))
        (void)this->renderer_->ProcessRegisteredEditHotkey(modifiers, key);
    return true;
}
// 全部模态入口共用输入暂停。入参：paused。返回：无。
void TextResultWindow::Pause(bool paused) noexcept
{
    if (this->IsOpen())
        EnableWindow(this->renderer_->NativeHandle(), !paused);
}
} // namespace open_st
