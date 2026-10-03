// 使用真实结果窗口与会话编排、可控 OCR 边界验证输入归属和迟到结果，不识别或写真实剪贴板。
#include "capture_text_session.h"
#include "../Translation/source/translation_test_defaults.h"
#include "text_result_window.h"
#include <clipboard_writer.h>
#include <gtest/gtest.h>
#include <ocr_client.h>
#include <sdr_selection_frame.h>
#include <translation_client.h>
#include <ui_text.h>
#include <vector>

namespace
{
struct OcrBoundaryState
{
    open_st::OcrSnapshot snapshot;
    open_st::OcrOptions options;
    int creates{};
    int destroys{};
    int submissions{};
    int cancellations{};
    int copies{};
    open_st::TranslationSnapshot translation;
    open_st::TranslationRequest request;
    unsigned translationSubmissions{}, translationCancellations{};
    bool translationStopped{};
    bool stopping{};
    bool stopped{};
    bool copySucceeds{true};
    std::wstring copied;
};
OcrBoundaryState* boundary{};
} // namespace

namespace open_st
{
// 独立链接替身只拥有空实现，真实后台线程由 OCR 模块专项覆盖。
struct OcrClient::Impl
{
};
// 建立可控任务边界，不加载模型或启动后台线程。
// 入参：root 和 notify 为真实协调器传入的资源路径及通知。
// 返回：未提交状态。
OcrClient::OcrClient(std::filesystem::path root, std::function<void(std::uint64_t)> notify)
    : impl_(std::make_unique<Impl>())
{
    (void)root;
    (void)notify;
    ++boundary->creates;
}
// 记录客户端释放，检验关闭窗口不会提前析构工作所有者。
// 入参：无。
// 返回：无。
OcrClient::~OcrClient()
{
    ++boundary->destroys;
}
// 模拟单任务准入，故意保持任务运行至测试明确发布结果。
// 入参：image/options 为真实会话参数；requestId/error 为输出。
// 返回：忙或停止时拒绝，其他情况接受。
bool OcrClient::Submit(OcrImageView image, const OcrOptions& options, std::uint64_t& requestId,
                       OcrError& error) noexcept
{
    requestId = 0;
    error = OcrError::None;
    ++boundary->submissions;
    if (boundary->snapshot.busy || boundary->stopping)
    {
        error = boundary->stopping ? OcrError::Unavailable : OcrError::Busy;
        return false;
    }
    if (image.width != 2 || image.height != 2 || image.stride != 8 || image.pixels.size() != 16)
    {
        error = OcrError::InvalidInput;
        return false;
    }
    boundary->options = options;
    ++boundary->snapshot.requestId;
    ++boundary->snapshot.revision;
    boundary->snapshot.phase = OcrPhase::Preparing;
    boundary->snapshot.busy = true;
    boundary->snapshot.text.reset();
    requestId = boundary->snapshot.requestId;
    return true;
}
// 保持 busy，模拟取消不能立即中断的模型初始化。
// 入参：无。
// 返回：无，只撤销可发布正文。
void OcrClient::Cancel() noexcept
{
    ++boundary->cancellations;
    boundary->snapshot.phase = OcrPhase::Cancelled;
    boundary->snapshot.text.reset();
    ++boundary->snapshot.revision;
}
// 请求退出但不虚构后台已完成。
// 入参：无。
// 返回：无。
void OcrClient::RequestShutdown() noexcept
{
    boundary->stopping = true;
    this->Cancel();
}
// 返回测试明确控制的释放状态。
// 入参：无。
// 返回：后台释放完成时为 true。
bool OcrClient::ShutdownComplete() const noexcept
{
    return boundary->stopped;
}
// 提供不可变正文及阶段快照，供真实会话执行代次检查。
// 入参：无。
// 返回：当前测试快照。
OcrSnapshot OcrClient::Snapshot() const
{
    return boundary->snapshot;
}
struct TranslationClient::Impl
{
};
// 不创建线程或网络，只观测真实窗口提交。
// 入参：程序目录和通知。返回：可控替身。
TranslationClient::TranslationClient(std::filesystem::path, std::function<void(std::uint64_t)>)
    : impl_(std::make_unique<Impl>())
{
}
// 释放空替身。入参：无。返回：无。
TranslationClient::~TranslationClient() = default;
// 真实唯一任务准入的替身，保存独立请求快照。
// 入参：request 为原文和资格戳，id/error 为输出。返回：空闲时接受。
bool TranslationClient::Submit(const TranslationRequest& request, std::uint64_t& id, TranslationError& error) noexcept
{
    ++boundary->translationSubmissions;
    id = 0;
    error = TranslationError::None;
    if (boundary->translation.busy)
    {
        error = TranslationError::Busy;
        return false;
    }
    try
    {
        boundary->request = request;
        ++boundary->translation.requestId;
        ++boundary->translation.revision;
        boundary->translation.phase = TranslationPhase::Running;
        boundary->translation.busy = true;
        boundary->translation.result.reset();
        id = boundary->translation.requestId;
        return true;
    }
    catch (...)
    {
        error = TranslationError::OutOfMemory;
        return false;
    }
}
// 保持忙槽位，取消不代表底层已经收尾。
// 入参：无。返回：无。
void TranslationClient::Cancel() noexcept
{
    ++boundary->translationCancellations;
    boundary->translation.result.reset();
    boundary->translation.phase = TranslationPhase::Cancelled;
    ++boundary->translation.revision;
}
// 提供权威任务快照。入参：无。返回：快照副本。
TranslationSnapshot TranslationClient::Snapshot() const
{
    return boundary->translation;
}
// 退出只撤销资格。入参：无。返回：无。
void TranslationClient::RequestShutdown() noexcept
{
    this->Cancel();
}
// 等测试显式通知真实收尾。入参：无。返回：释放状态。
bool TranslationClient::ShutdownComplete() const noexcept
{
    return boundary->translationStopped;
}
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
};
// 按本测试独有 owner 查找实际 Renderer，避免误操作其他桌面窗口。
// 入参：window 为当前线程窗口；data 指向查找状态。
// 返回：找到后停止枚举。
BOOL CALLBACK FindResult(HWND window, LPARAM data)
{
    auto& search = *reinterpret_cast<WindowSearch*>(data);
    wchar_t type[64]{};
    GetClassNameW(window, type, 64);
    if (GetWindow(window, GW_OWNER) == search.owner && std::wstring_view(type) == L"OpenST.WindowRenderer")
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
    nlohmann::json settings_ = open_st::TestDefaultSettings();
    bool settingsReadable_{true};
    unsigned settingsReads_{};
    std::unique_ptr<open_st::TranslationClient> translator_;
    std::unique_ptr<open_st::CaptureTextSession> session_;
    open_st::SdrSelectionFrame frame_{{0, 0, 2, 2}, std::vector<std::uint8_t>(16, 255)};

    // 创建隐藏稳定宿主及真实协调器。
    // 入参：无。
    // 返回：无；失败由断言记录。
    void SetUp() override
    {
        boundary = &this->state_;
        this->owner_ = CreateWindowExW(0, L"STATIC", L"OCR integration owner", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                                       GetModuleHandleW(nullptr), nullptr);
        ASSERT_NE(this->owner_, nullptr);
        this->translator_ =
            std::make_unique<open_st::TranslationClient>(std::filesystem::path{}, std::function<void(std::uint64_t)>{});
        this->session_ = std::make_unique<open_st::CaptureTextSession>(
            this->owner_, nullptr,
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
        this->state_.snapshot.busy = false;
        this->state_.stopped = true;
        this->state_.translationStopped = true;
        this->session_->Shutdown();
        this->Pump();
        this->session_.reset();
        this->translator_.reset();
        DestroyWindow(this->owner_);
        boundary = nullptr;
    }
    // 查找本会话窗口，窗口仍以真实公共组件创建。
    // 入参：无。
    // 返回：借用句柄或空。
    HWND Window() const
    {
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
            if (message.message != WM_QUIT && !this->session_->Process(message))
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
        ASSERT_NO_THROW(this->session_->Begin(this->frame_, "best", "eng"));
        ASSERT_NE(this->Window(), nullptr);
        ShowWindow(this->Window(), SW_HIDE);
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
        this->state_.snapshot.phase = open_st::OcrPhase::Succeeded;
        this->state_.snapshot.busy = false;
        this->state_.snapshot.text = std::make_shared<const std::wstring>(std::move(text));
        ++this->state_.snapshot.revision;
        this->session_->Poll();
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
    EXPECT_EQ(KillTimer(this->owner_, open_st::TextPollTimer), FALSE);
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
    EXPECT_FALSE(this->session_->ShutdownComplete());
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
    this->settings_["translation.network.proxy_mode"] = "custom";
    this->settings_["translation.network.proxy_address"] = "localhost:3456";
    this->session_->ConfigurationChanged();
    EXPECT_EQ(this->TranslationValue(), L"translated");
    EXPECT_TRUE(this->HasText(L"translation.stale"));
    this->TranslateSucceeded("obsolete");
    EXPECT_EQ(this->TranslationValue(), L"translated");
    EXPECT_EQ(this->Value(), L"original");
    EXPECT_EQ(this->state_.translationSubmissions, 2U);
    this->ClickTranslate();
    EXPECT_GT(this->state_.request.configurationRevision, oldRevision);
    EXPECT_EQ(this->state_.request.configuration.proxyMode, "custom");
    EXPECT_EQ(this->state_.request.configuration.proxyAddress, "localhost:3456");
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
    this->session_->EndCapture();
    EXPECT_EQ(this->state_.translationCancellations, before);
}
