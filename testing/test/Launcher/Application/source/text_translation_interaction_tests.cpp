// 使用真实结果窗口与会话编排、可控 OCR 边界验证输入归属和迟到结果，不识别或写真实剪贴板。
#include "../Translation/source/translation_test_defaults.h"
#include "text_result_window.h"
#include "text_session.h"
#include <clipboard_writer.h>
#include <gtest/gtest.h>
#include <sdr_selection_frame.h>
#include <text_task_fake.h>
#include <translation_client.h>
#include <ui_text.h>
#include <vector>

#ifdef OPEN_ST_TEST_TRANSLATION_ONLY
#define TextTranslationInteractionTest StandaloneTextTranslationInteractionTest
#endif

namespace
{
struct OcrBoundaryState : open_st::test_text::TextTaskState
{
    int copies{};
    bool copySucceeds{true};
    std::wstring copied;
};
OcrBoundaryState* boundary{};
} // namespace

namespace open_st
{
// 把文本键直接用作稳定测试文案，不读取真实用户资源。
// 入参：key 为文本键；arguments 未使用。
// 返回：文本键的宽字符形式。
std::wstring GetUiText(std::string_view key, std::initializer_list<UiTextArgument> arguments)
{
    (void)arguments;
    return std::wstring(key.begin(), key.end());
}
// 替换系统剪贴板边界，仅记录结果窗口传入的当前草稿。
// 入参：owner 为结果 HWND；text 为当前 UTF-16；error 接收测试错误。
// 返回：测试指定的发布结果。
bool CopyTextToClipboard(HWND owner, std::wstring_view text, std::wstring& error)
{
    EXPECT_TRUE(IsWindow(owner));
    ++boundary->copies;
    boundary->copied = text;
    error = boundary->copySucceeds ? L"" : L"Injected clipboard failure";
    return boundary->copySucceeds;
}
} // namespace open_st

namespace
{
struct WindowSearch
{
    HWND owner{};
    HWND found{};
    HWND excluded{};
};
// 按本测试独有 owner 查找实际 Renderer，避免误操作其他桌面窗口。
// 入参：window 为当前线程窗口；data 指向查找状态。
// 返回：找到后停止枚举。
BOOL CALLBACK FindResult(HWND window, LPARAM data)
{
    auto& search = *reinterpret_cast<WindowSearch*>(data);
    wchar_t type[64]{};
    GetClassNameW(window, type, 64);
    if (window != search.excluded && GetWindow(window, GW_OWNER) == search.owner &&
        std::wstring_view(type) == L"OpenST.WindowRenderer")
    {
        search.found = window;
        return FALSE;
    }
    return TRUE;
}

class TextTranslationInteractionTest : public testing::Test
{
  protected:
    OcrBoundaryState state_;
    HWND owner_{};
    HWND primaryWindow_{};
    std::unique_ptr<open_st::TextSession> peer_;
    nlohmann::json settings_ = open_st::TestDefaultSettings();
    bool settingsReadable_{true};
    unsigned settingsReads_{};
    std::unique_ptr<open_st::TranslationClient> translator_;
    std::unique_ptr<open_st::TextSession> session_;
    open_st::SdrSelectionFrame frame_{{0, 0, 2, 2}, std::vector<std::uint8_t>(16, 255)};

    // 创建隐藏稳定宿主及真实协调器。
    // 入参：无。
    // 返回：无；失败由断言记录。
    void SetUp() override
    {
        boundary = &this->state_;
        open_st::test_text::state = &this->state_;
        this->owner_ = CreateWindowExW(0, L"STATIC", L"OCR integration owner", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                                       GetModuleHandleW(nullptr), nullptr);
        ASSERT_NE(this->owner_, nullptr);
        this->translator_ =
            std::make_unique<open_st::TranslationClient>(std::filesystem::path{}, std::function<void(std::uint64_t)>{});
        this->session_ = std::make_unique<open_st::TextSession>(
            this->owner_, nullptr, open_st::CaptureTextPollTimer,
            // 每次提供同一文件的独立设置快照，失败不回退默认网络配置。
            // 入参：无。返回：内存设置或读取失败。
            [this]() -> std::optional<nlohmann::json>
            {
                ++this->settingsReads_;
                return this->settingsReadable_ ? std::optional<nlohmann::json>(this->settings_) : std::nullopt;
            },
            this->translator_.get());
    }
    // 先停止边界并关闭会话，随后销毁稳定 HWND。
    // 入参：无。
    // 返回：无；不留下窗口、计时器或后台任务。
    void TearDown() override
    {
#ifdef OPEN_ST_HAS_OCR
        this->state_.snapshot.busy = false;
#endif
        this->state_.stopped = true;
        this->state_.translationStopped = true;
        if (this->peer_)
            this->peer_->Shutdown();
        this->session_->Shutdown();
        this->Pump();
        this->peer_.reset();
        this->session_.reset();
        this->translator_.reset();
        DestroyWindow(this->owner_);
        boundary = nullptr;
        open_st::test_text::state = nullptr;
    }
    // 查找本会话窗口，窗口仍以真实公共组件创建。
    // 入参：无。
    // 返回：借用句柄或空。
    HWND Window() const
    {
        if (this->primaryWindow_)
            return IsWindow(this->primaryWindow_) ? this->primaryWindow_ : nullptr;
        WindowSearch search{this->owner_};
        EnumThreadWindows(GetCurrentThreadId(), FindResult, reinterpret_cast<LPARAM>(&search));
        return search.found;
    }
    // 定位结果编辑框，不依赖生成控件编号。
    // 入参：无。
    // 返回：借用编辑框 HWND。
    HWND Edit() const
    {
        const HWND viewport = FindWindowExW(this->Window(), nullptr, L"OpenST.WindowRendererPage", nullptr);
        return FindWindowExW(viewport, nullptr, L"EDIT", nullptr);
    }
    // 查找确定测试文字的底部按钮或状态。
    // 入参：type 为原生类；text 为稳定文案。
    // 返回：根窗口直属控件。
    HWND Control(const wchar_t* type, const wchar_t* text) const
    {
        return FindWindowExW(this->Window(), nullptr, type, text);
    }
    // 读取实际可见编辑文本。
    // 入参：无。
    // 返回：独立 UTF-16 文本。
    std::wstring Value() const
    {
        std::wstring text(static_cast<std::size_t>(GetWindowTextLengthW(this->Edit())) + 1, L'\0');
        text.resize(static_cast<std::size_t>(GetWindowTextW(this->Edit(), text.data(), static_cast<int>(text.size()))));
        return text;
    }
    // 有界泵送原生消息，包括延迟关闭；不等待识别线程。
    // 入参：无。
    // 返回：无。
    void Pump()
    {
        MSG message{};
        for (int index = 0; index < 256 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++index)
        {
            if (message.message != WM_QUIT && !this->session_->Process(message) &&
                (!this->peer_ || !this->peer_->Process(message)))
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
    }
    // 开始并立即隐藏真实结果窗口，避免让自动化窗口停留桌面。
    // 入参：无。
    // 返回：无；窗口不能创建时为测试失败。
    void Begin()
    {
        this->primaryWindow_ = nullptr;
#ifdef OPEN_ST_HAS_OCR
        ASSERT_NO_THROW(this->session_->BeginOcr(this->frame_, "best", "eng"));
#else
        ASSERT_NO_THROW(this->session_->BeginText());
#endif
        ASSERT_NE(this->Window(), nullptr);
        ShowWindow(this->Window(), SW_HIDE);
    }
    // 手动入口只初始化草稿，不生成识别输入。入参：无。返回：无。
    void BeginManual()
    {
        this->primaryWindow_ = nullptr;
        ASSERT_NO_THROW(this->session_->BeginText());
        ASSERT_NE(this->Window(), nullptr);
        this->primaryWindow_ = this->Window();
        ShowWindow(this->Window(), SW_HIDE);
    }
    // 第二实例借用同一客户端与宿主，使用独立 timer。入参：无。返回：第二窗口。
    HWND BeginPeer()
    {
        this->primaryWindow_ = this->Window();
        this->peer_ = std::make_unique<open_st::TextSession>(
            this->owner_, nullptr, open_st::StandaloneTextPollTimer,
            [this]() -> std::optional<nlohmann::json> { return this->settings_; }, this->translator_.get());
        this->peer_->BeginText();
        WindowSearch search{this->owner_};
        search.excluded = this->primaryWindow_;
        EnumThreadWindows(GetCurrentThreadId(), FindResult, reinterpret_cast<LPARAM>(&search));
        return search.found;
    }
    // 通过真实结果窗按钮显式翻译；OCR 完成和窗口激活都不能暗中替代此动作。
    // 入参：无。返回：无，缺失或禁用按钮作为测试失败。
    void ClickTranslate()
    {
        const HWND button = this->Control(L"BUTTON", L"translation.action");
        ASSERT_NE(button, nullptr);
        ASSERT_TRUE(IsWindowEnabled(button));
        SendMessageW(button, BM_CLICK, 0, 0);
    }
    // 发布一次成功快照，真实协调器负责填写草稿。
    // 入参：text 为模拟识别结果。
    // 返回：无。
    void Succeed(std::wstring text)
    {
#ifdef OPEN_ST_HAS_OCR
        this->state_.snapshot.phase = open_st::OcrPhase::Succeeded;
        this->state_.snapshot.busy = false;
        this->state_.snapshot.text = std::make_shared<const std::wstring>(std::move(text));
        ++this->state_.snapshot.revision;
        this->session_->Poll();
#else
        this->Replace(text.c_str());
#endif
    }
    // 找到只读译文控件，不依赖生成编号。
    // 入参：无。返回：译文 HWND。
    HWND TranslationEdit() const
    {
        const HWND viewport = FindWindowExW(this->Window(), nullptr, L"OpenST.WindowRendererPage", nullptr);
        return FindWindowExW(viewport, this->Edit(), L"EDIT", nullptr);
    }
    // 读取只读输出实际文本。
    // 入参：无。返回：完整 UTF-16。
    std::wstring TranslationValue() const
    {
        std::wstring text(static_cast<std::size_t>(GetWindowTextLengthW(this->TranslationEdit())) + 1, L'\0');
        text.resize(static_cast<std::size_t>(
            GetWindowTextW(this->TranslationEdit(), text.data(), static_cast<int>(text.size()))));
        return text;
    }
    // 查找来源/状态提示中的稳定资源键，控件可在滚动内容中。
    // 入参：value 为资源键。返回：存在为 true。
    bool HasText(std::wstring_view value) const
    {
        std::pair<std::wstring_view, bool> search{value, false};
        EnumChildWindows(
            this->Window(),
            // 枚举只读控件文本，不解释为命令。
            // 入参：window 为候选，data 为查询。返回：未找到时继续。
            [](HWND window, LPARAM data) -> BOOL
            {
                auto& query = *reinterpret_cast<std::pair<std::wstring_view, bool>*>(data);
                std::wstring text(static_cast<std::size_t>(GetWindowTextLengthW(window)) + 1, L'\0');
                text.resize(
                    static_cast<std::size_t>(GetWindowTextW(window, text.data(), static_cast<int>(text.size()))));
                query.second = text.find(query.first) != std::wstring::npos;
                return !query.second;
            },
            reinterpret_cast<LPARAM>(&search));
        return search.second;
    }
    // 故意允许发布已取消的迟到结果，检验真实会话资格屏障。
    // 入参：text 为合成译文。返回：无。
    void TranslateSucceeded(std::string text)
    {
        auto result = std::make_shared<open_st::TranslationResult>();
        result->text = std::move(text);
        result->profileName = "fixture";
        result->requestId = this->state_.translation.requestId;
        result->sessionId = this->state_.request.sessionId;
        result->sourceRevision = this->state_.request.sourceRevision;
        result->configurationRevision = this->state_.request.configurationRevision;
        result->options = this->state_.request.options;
        this->state_.translation.result = std::move(result);
        this->state_.translation.busy = false;
        this->state_.translation.phase = open_st::TranslationPhase::Succeeded;
        ++this->state_.translation.revision;
        this->session_->Poll();
    }
    // 模拟可撤销的用户文本编辑，触发真实 EN_CHANGE。
    // 入参：text 为替换文本。
    // 返回：无。
    void Replace(const wchar_t* text)
    {
        SendMessageW(this->Edit(), EM_SETSEL, 0, -1);
        SendMessageW(this->Edit(), EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(text));
    }
};

// OCR 完成、重复激活及状态补收均零提交，只有结果窗口显式点击才翻译一次。
// 入参：无。返回：断言结果。
#ifdef OPEN_ST_HAS_OCR
TEST_F(TextTranslationInteractionTest, ocr_completion_and_activation_require_explicit_translation_click)
{
    this->Begin();
    ASSERT_NE(this->Window(), nullptr);
    ASSERT_NE(this->TranslationEdit(), nullptr);
    EXPECT_TRUE(this->session_->ActivateExisting());
    EXPECT_TRUE(this->session_->ActivateExisting());
    EXPECT_EQ(this->state_.translationSubmissions, 0U);
    this->Succeed(L"original");
    EXPECT_TRUE(this->session_->ActivateExisting());
    this->session_->Poll();
    this->session_->Poll();
    EXPECT_EQ(this->state_.translationSubmissions, 0U);
    this->ClickTranslate();
    EXPECT_EQ(this->state_.translationSubmissions, 1U);
    EXPECT_EQ(this->state_.request.text, "original");
    this->session_->Poll();
    EXPECT_EQ(this->state_.translationSubmissions, 1U);
    this->TranslateSucceeded("translated");
    EXPECT_EQ(this->TranslationValue(), L"translated");
    EXPECT_TRUE((GetWindowLongPtrW(this->TranslationEdit(), GWL_STYLE) & ES_READONLY) != 0);
    SendMessageW(this->Control(L"BUTTON", L"translation.copy_result"), BM_CLICK, 0, 0);
    EXPECT_EQ(this->state_.copied, L"translated");
}
#endif
// 编辑立即撤销翻译资格，迟到结果不得覆盖正文或重新发送。
// 入参：无。返回：断言结果。
TEST_F(TextTranslationInteractionTest, editing_source_rejects_late_translation)
{
    this->Begin();
    this->Succeed(L"original");
    this->ClickTranslate();
    ASSERT_EQ(this->state_.translationSubmissions, 1U);
    const unsigned cancelled = this->state_.translationCancellations;
    this->Replace(L"edited");
    EXPECT_GT(this->state_.translationCancellations, cancelled);
    EXPECT_TRUE(this->state_.translation.busy);
    this->TranslateSucceeded("obsolete");
    EXPECT_EQ(this->Value(), L"edited");
    EXPECT_EQ(this->TranslationValue(), L"");
    EXPECT_EQ(this->state_.translationSubmissions, 1U);
    SendMessageW(this->Edit(), WM_UNDO, 0, 0);
    EXPECT_EQ(this->Value(), L"original");
}
// 清空后任务收尾停掉补收定时器，再输入仍应立即恢复翻译按钮。
// 入参：无。返回：断言结果。
TEST_F(TextTranslationInteractionTest, editing_after_idle_empty_source_reenables_translation)
{
    this->Begin();
    this->Succeed(L"original");
    this->ClickTranslate();
    this->Replace(L"");
    this->TranslateSucceeded("obsolete");
    const HWND translate = this->Control(L"BUTTON", L"translation.action");
    ASSERT_NE(translate, nullptr);
    EXPECT_FALSE(IsWindowEnabled(translate));
    EXPECT_EQ(KillTimer(this->owner_, open_st::CaptureTextPollTimer), FALSE);
    this->Replace(L"new source");
    EXPECT_TRUE(IsWindowEnabled(translate));
    EXPECT_EQ(this->state_.translationSubmissions, 1U);
}
// Close 撤销逻辑准入发生在异步销毁 HWND 之前，迟到按钮消息不能翻译旧草稿。
// 入参：无。返回：断言结果。
TEST_F(TextTranslationInteractionTest, requested_close_immediately_revokes_translation_admission)
{
    this->Begin();
    this->Succeed(L"original");
    this->ClickTranslate();
    this->TranslateSucceeded("translated");
    const HWND window = this->Window();
    const HWND translate = this->Control(L"BUTTON", L"translation.action");
    ASSERT_NE(window, nullptr);
    ASSERT_NE(translate, nullptr);
    SendMessageW(window, WM_CLOSE, 0, 0);
    EXPECT_FALSE(this->session_->ActivateExisting());
    SendMessageW(translate, BM_CLICK, 0, 0);
    EXPECT_EQ(this->state_.translationSubmissions, 1U);
    this->Pump();
    EXPECT_EQ(this->Window(), nullptr);
}
// OCR退出由会话负责，借用的翻译后台由宿主独立等待，关闭窗口不冒充线程结束。
// 入参：无。返回：断言结果。
TEST_F(TextTranslationInteractionTest, shutdown_waits_for_owned_ocr_and_leaves_shared_translator_lifetime_to_host)
{
    this->Begin();
    this->Succeed(L"original");
    this->ClickTranslate();
    this->session_->Shutdown();
    this->Pump();
    EXPECT_EQ(this->Window(), nullptr);
#ifdef OPEN_ST_HAS_OCR
    EXPECT_FALSE(this->session_->ShutdownComplete());
#else
    EXPECT_TRUE(this->session_->ShutdownComplete());
#endif
    this->state_.stopped = true;
    EXPECT_TRUE(this->session_->ShutdownComplete());
    EXPECT_FALSE(this->translator_->ShutdownComplete());
    this->translator_->RequestShutdown();
    this->state_.translationStopped = true;
    EXPECT_TRUE(this->translator_->ShutdownComplete());
}
// 读取完整设置快照失败时保持纯 OCR 可用，但点击翻译也不能退回默认网络配置。
// 入参：无。返回：断言结果。
TEST_F(TextTranslationInteractionTest, failed_settings_snapshot_never_submits_translation)
{
    this->settingsReadable_ = false;
    this->Begin();
    this->Succeed(L"original");
    EXPECT_EQ(this->Value(), L"original");
    this->ClickTranslate();
    EXPECT_EQ(this->state_.translationSubmissions, 0U);
    this->settingsReadable_ = true;
    this->settings_["translation.source_language"] = "ja";
    this->settings_["translation.target_language"] = "en";
    this->ClickTranslate();
    EXPECT_EQ(this->state_.translationSubmissions, 1U);
    EXPECT_EQ(this->state_.request.options.sourceLanguage, "ja");
    EXPECT_EQ(this->state_.request.options.targetLanguage, "en");
}
// 临时语言改变取消整轮，用户明确重译时才使用新语种。
// 入参：无。返回：断言结果。
TEST_F(TextTranslationInteractionTest, language_change_cancels_without_automatic_resubmission)
{
    this->Begin();
    this->Succeed(L"original");
    this->ClickTranslate();
    this->TranslateSucceeded("translated");
    this->ClickTranslate();
    const HWND viewport = FindWindowExW(this->Window(), nullptr, L"OpenST.WindowRendererPage", nullptr);
    const HWND source = FindWindowExW(viewport, nullptr, L"COMBOBOX", nullptr);
    const HWND target = FindWindowExW(viewport, source, L"COMBOBOX", nullptr);
    ASSERT_NE(source, nullptr);
    ASSERT_NE(target, nullptr);
    const unsigned cancelled = this->state_.translationCancellations;
    SendMessageW(target, CB_SETCURSEL, 1, 0);
    SendMessageW(GetParent(target), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(target), CBN_SELCHANGE),
                 reinterpret_cast<LPARAM>(target));
    EXPECT_GT(this->state_.translationCancellations, cancelled);
    EXPECT_TRUE(this->HasText(L"translation.stale"));
    EXPECT_EQ(this->state_.translationSubmissions, 2U);
    this->TranslateSucceeded("obsolete");
    EXPECT_EQ(this->TranslationValue(), L"translated");
    this->ClickTranslate();
    EXPECT_EQ(this->state_.request.options.targetLanguage, "en");
    EXPECT_EQ(this->state_.translationSubmissions, 3U);
}
// 配置变化保留旧译文并标过期，下一轮读取新的固定代理并推进配置资格戳。
// 入参：无。返回：断言结果。
TEST_F(TextTranslationInteractionTest, configuration_change_preserves_stale_translation_until_explicit_retry)
{
    this->Begin();
    this->Succeed(L"original");
    this->ClickTranslate();
    this->TranslateSucceeded("translated");
    this->ClickTranslate();
    const std::uint64_t oldRevision = this->state_.request.configurationRevision;
    this->settings_["translation.interfaces"][0]["network"]["proxy_mode"] = "custom";
    this->settings_["translation.interfaces"][0]["network"]["proxy_address"] = "localhost:3456";
    this->session_->ConfigurationChanged();
    EXPECT_EQ(this->TranslationValue(), L"translated");
    EXPECT_TRUE(this->HasText(L"translation.stale"));
    this->TranslateSucceeded("obsolete");
    EXPECT_EQ(this->TranslationValue(), L"translated");
    EXPECT_EQ(this->Value(), L"original");
    EXPECT_EQ(this->state_.translationSubmissions, 2U);
    this->ClickTranslate();
    EXPECT_GT(this->state_.request.configurationRevision, oldRevision);
    EXPECT_EQ(this->state_.request.configuration.interfaces[0]["network"]["proxy_mode"], "custom");
    EXPECT_EQ(this->state_.request.configuration.interfaces[0]["network"]["proxy_address"], "localhost:3456");
}
// 旧任务取消未收尾时重开窗口仍禁用翻译按钮，迟到旧译文不得进入新截图。
// 入参：无。返回：断言结果。
TEST_F(TextTranslationInteractionTest, reopen_cannot_bypass_unfinished_translation)
{
    this->Begin();
    this->Succeed(L"original");
    this->ClickTranslate();
    const std::uint64_t oldSession = this->state_.request.sessionId;
    SendMessageW(this->Window(), WM_CLOSE, 0, 0);
    this->Pump();
    EXPECT_TRUE(this->state_.translation.busy);
    this->Begin();
    this->Succeed(L"new capture");
    EXPECT_EQ(this->state_.translationSubmissions, 1U);
    const HWND translate = this->Control(L"BUTTON", L"translation.action");
    ASSERT_NE(translate, nullptr);
    EXPECT_FALSE(IsWindowEnabled(translate));
    SendMessageW(translate, BM_CLICK, 0, 0);
    EXPECT_EQ(this->state_.translationSubmissions, 1U);
    this->TranslateSucceeded("obsolete");
    EXPECT_EQ(this->TranslationValue(), L"");
    EXPECT_EQ(this->Value(), L"new capture");
    this->ClickTranslate();
    EXPECT_EQ(this->state_.translationSubmissions, 2U);
    EXPECT_GT(this->state_.request.sessionId, oldSession);
}
// 尚未发送前就显示明文 HTTP 风险，配置保存后立刻更新提示。
// 入参：无。返回：断言结果。
TEST_F(TextTranslationInteractionTest, insecure_http_warning_tracks_configuration_before_submission)
{
    nlohmann::json profile = open_st::TestTranslationProfile("custom_http");
    profile["enabled"] = true;
    profile["configuration"]["url"] = "http://example.invalid/translate";
    this->settings_["translation.interfaces"] = nlohmann::json::array({profile});
    this->Begin();
    EXPECT_TRUE(this->HasText(L"translation.http_warning"));
    EXPECT_EQ(this->state_.translationSubmissions, 0U);
    this->settings_["translation.interfaces"][0]["enabled"] = false;
    this->session_->ConfigurationChanged();
    EXPECT_FALSE(this->HasText(L"translation.http_warning"));
    this->Succeed(L"original");
    EXPECT_EQ(this->state_.translationSubmissions, 0U);
}
} // namespace

// 截图生命周期只能取消自己的请求，编辑器随后提交的测试仍由应用拥有。
// 入参：无。返回：断言结果。
TEST_F(TextTranslationInteractionTest, ending_capture_does_not_cancel_foreign_translation_request)
{
    this->Begin();
    this->Succeed(L"original");
    this->ClickTranslate();
    ++this->state_.translation.requestId;
    const unsigned before = this->state_.translationCancellations;
    this->session_->Close();
    EXPECT_EQ(this->state_.translationCancellations, before);
}

// 手动草稿即刻可编辑；重复打开保留正文、撤销与零后台提交。
// 入参：无。返回：断言结果。
TEST_F(TextTranslationInteractionTest, manual_input_is_ready_without_ocr_and_reactivation_preserves_undo)
{
    this->BeginManual();
    EXPECT_TRUE(IsWindowEnabled(this->Edit()));
    EXPECT_EQ(this->Value(), L"");
    EXPECT_FALSE(IsWindowEnabled(this->Control(L"BUTTON", L"translation.action")));
    this->Replace(L"first");
    this->Replace(L"draft");
    const HWND window = this->Window();
    this->session_->BeginText();
    EXPECT_EQ(this->Window(), window);
    EXPECT_EQ(this->Value(), L"draft");
    EXPECT_EQ(this->state_.creates, 0);
    EXPECT_EQ(this->state_.submissions, 0);
    EXPECT_EQ(this->state_.translationSubmissions, 0U);
    SendMessageW(this->Edit(), WM_UNDO, 0, 0);
    EXPECT_EQ(this->Value(), L"first");
}

// 刷新界面文字不改变正在运行的请求资格、草稿或撤销。
// 入参：无。返回：断言结果。
TEST_F(TextTranslationInteractionTest, localized_text_refresh_preserves_running_request_and_draft)
{
    this->BeginManual();
    this->Replace(L"original");
    this->ClickTranslate();
    const std::uint64_t request = this->state_.translation.requestId;
    const unsigned cancellations = this->state_.translationCancellations;
    this->session_->RefreshTexts();
    this->session_->RefreshTexts();
    EXPECT_EQ(this->state_.translation.requestId, request);
    EXPECT_EQ(this->state_.translationCancellations, cancellations);
    EXPECT_EQ(this->Value(), L"original");
    this->TranslateSucceeded("translated");
    EXPECT_EQ(this->TranslationValue(), L"translated");
    this->Replace(L"edited");
    this->session_->RefreshTexts();
    SendMessageW(this->Edit(), WM_UNDO, 0, 0);
    EXPECT_EQ(this->Value(), L"original");
}

// 两实例共用唯一任务槽；另一窗口编辑、取消和关闭不能取消或接收本窗任务。
// 入参：无。返回：断言结果。
TEST_F(TextTranslationInteractionTest, second_instance_cannot_cancel_or_receive_foreign_request)
{
    this->BeginManual();
    this->Replace(L"owner draft");
    const HWND peer = this->BeginPeer();
    ASSERT_NE(peer, nullptr);
    ASSERT_NE(peer, this->Window());
    const HWND viewport = FindWindowExW(peer, nullptr, L"OpenST.WindowRendererPage", nullptr);
    const HWND edit = FindWindowExW(viewport, nullptr, L"EDIT", nullptr);
    ASSERT_NE(edit, nullptr);
    SendMessageW(edit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"peer draft"));
    this->ClickTranslate();
    this->peer_->Poll();
    const HWND action = FindWindowExW(peer, nullptr, L"BUTTON", L"translation.action");
    const HWND cancel = FindWindowExW(peer, nullptr, L"BUTTON", L"translation.cancel");
    ASSERT_NE(action, nullptr);
    EXPECT_FALSE(IsWindowEnabled(action));
    if (cancel)
        EXPECT_FALSE(IsWindowEnabled(cancel));
    const unsigned cancellations = this->state_.translationCancellations;
    SendMessageW(edit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L" changed"));
    if (cancel)
        SendMessageW(cancel, BM_CLICK, 0, 0);
    this->peer_->Close();
    EXPECT_EQ(this->state_.translationCancellations, cancellations);
    this->TranslateSucceeded("owner result");
    EXPECT_EQ(this->Value(), L"owner draft");
    EXPECT_EQ(this->TranslationValue(), L"owner result");
    this->peer_->Poll();
    EXPECT_EQ(this->state_.translationSubmissions, 1U);
}

// 一实例关闭只停自己的 timer；通知丢失后显式补收仍发布到原窗口。
// 入参：无。返回：断言结果。
TEST_F(TextTranslationInteractionTest, timers_are_independent_and_poll_recovers_missing_notification)
{
    this->BeginManual();
    this->Replace(L"source");
    const HWND peer = this->BeginPeer();
    ASSERT_NE(peer, nullptr);
    this->ClickTranslate();
    this->peer_->Poll();
    // 明确布置第二实例自己的补收计时器，验证析构只移除本实例编号。
    ASSERT_NE(SetTimer(this->owner_, open_st::StandaloneTextPollTimer, 100, nullptr), 0U);
    ASSERT_TRUE(KillTimer(this->owner_, open_st::CaptureTextPollTimer));
    ASSERT_NE(SetTimer(this->owner_, open_st::CaptureTextPollTimer, 100, nullptr), 0U);
    this->peer_.reset();
    this->Pump();
    EXPECT_EQ(KillTimer(this->owner_, open_st::StandaloneTextPollTimer), FALSE);
    EXPECT_TRUE(KillTimer(this->owner_, open_st::CaptureTextPollTimer));
    this->TranslateSucceeded("recovered");
    EXPECT_EQ(this->TranslationValue(), L"recovered");
    EXPECT_EQ(this->state_.translationSubmissions, 1U);
}
