// 文件职责：真实关于／确认窗口的自动交互契约，网络与系统启动均替换，不访问线上或执行安装器。

#include "about_window.h"
#include "json_file_test_access.h"
#include "ui_text_internal.h"
#include "update_transport.h"

#include <ui_text.h>
#include <window_renderer.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <windows.h>

namespace
{
struct AboutNetworkState
{
    std::atomic<unsigned> queries{}, assets{};
    std::atomic<bool> cancellationObserved{}, lateResponseProduced{};
    std::string version = "1.1.0";
    bool holdQuery = false;
};
std::shared_ptr<AboutNetworkState> activeNetwork;

class AboutTransport final : public open_st::update_detail::UpdateTransport
{
  public:
    // 固定当前用例的隔离网络状态，后台任务只访问此共享对象。
    // 入参：state 为用例状态。
    // 返回：无。
    explicit AboutTransport(std::shared_ptr<AboutNetworkState> state) : state_(std::move(state)) {}

    // 返回内存发行元数据或 abc 安装包，延后模式在取消后故意返回迟到成功。
    // 入参：request 为真实客户端参数；stop 为真实取消令牌；sink 为真实接收回调。
    // 返回：替身响应；不建立网络连接。
    open_st::update_detail::UpdateHttpResult Get(const open_st::update_detail::UpdateHttpRequest& request,
                                                 std::stop_token stop,
                                                 const open_st::update_detail::UpdateHttpSink& sink) override
    {
        using namespace open_st::update_detail;
        if (!this->state_)
            return {UpdateHttpError::Network, 0, 0, {}, {}};
        std::string bytes;
        if (request.url.ends_with(L"/releases/latest"))
        {
            ++this->state_->queries;
            if (this->state_->holdQuery)
            {
                std::mutex mutex;
                std::condition_variable_any ready;
                std::unique_lock lock(mutex);
                (void)ready.wait(lock, stop,
                                 // 等待真实取消，不按固定时间自动完成请求。
                                 // 入参：无。
                                 // 返回：始终 false，仅 stop_token 可唤醒。
                                 []() { return false; });
                this->state_->cancellationObserved = stop.stop_requested();
                this->state_->lateResponseProduced = true;
            }
            const std::string& version = this->state_->version;
            const std::string prefix = "Open-ST-" + version + "-win-x64";
            const nlohmann::json release{
                {"id", 17},
                {"draft", false},
                {"prerelease", false},
                {"tag_name", "v" + version},
                {"html_url", "https://github.com/Lucency09/Open-ST/releases/tag/v" + version},
                {"assets", nlohmann::json::array(
                               {{{"id", 31},
                                 {"size", 3},
                                 {"state", "uploaded"},
                                 {"name", prefix + "-Setup.exe"},
                                 {"digest", "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"}},
                                {{"id", 32}, {"size", 3}, {"state", "uploaded"}, {"name", prefix + ".zip"}}})}};
            bytes = release.dump();
        }
        else
        {
            ++this->state_->assets;
            bytes = "abc";
        }
        const bool accepted = sink({reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()});
        return {accepted ? UpdateHttpError::None : UpdateHttpError::SinkRejected, 0, 200, bytes.size(), {}};
    }

  private:
    std::shared_ptr<AboutNetworkState> state_;
};

struct TextSearch
{
    std::wstring text;
    HWND found{};
};

// 只在当前测试线程的窗口或子控件中按已知文字定位，不操作用户其他窗口。
// 入参：window 为枚举句柄；data 指向同步借用的搜索状态。
// 返回：找到时 FALSE 终止枚举，其余 TRUE。
BOOL CALLBACK FindText(HWND window, LPARAM data)
{
    TextSearch& search = *reinterpret_cast<TextSearch*>(data);
    std::array<wchar_t, 256> text{};
    if (IsWindowVisible(window) && GetWindowTextW(window, text.data(), static_cast<int>(text.size())) > 0 &&
        search.text == text.data())
    {
        search.found = window;
        return FALSE;
    }
    return TRUE;
}

// 获取当前线程指定标题的活动窗口。
// 入参：title 为测试资源中的独有标题。
// 返回：匹配 HWND 或空。
HWND FindTop(std::wstring title)
{
    TextSearch search{std::move(title)};
    EnumThreadWindows(GetCurrentThreadId(), FindText, reinterpret_cast<LPARAM>(&search));
    return search.found;
}

// 查找目标窗口下带固定文字的控件。
// 入参：window 为测试窗口；text 为标签。
// 返回：匹配子控件 HWND 或空。
HWND FindChild(HWND window, std::wstring text)
{
    TextSearch search{std::move(text)};
    EnumChildWindows(window, FindText, reinterpret_cast<LPARAM>(&search));
    return search.found;
}

enum class AboutScenario
{
    InstallConfirm,
    InstallCancel,
    PortableConfirm,
    Current,
    ClosePending
};

class AboutUpdateIntegrationTest : public testing::Test
{
  protected:
    std::filesystem::path root_;
    std::shared_ptr<AboutNetworkState> network_;
    open_st::WindowRenderer* about_{};
    AboutScenario scenario_{};
    unsigned step_{}, prompts_{}, launches_{}, pages_{};
    bool stopped_{}, timedOut_{};
    UINT_PTR timer_{};
    std::wstring openedPage_;
    std::chrono::steady_clock::time_point deadline_;

    // 建立隔离本地化、缓存目录和替身，不触碰实际运行资源。
    // 入参：无。
    // 返回：无。
    void SetUp() override
    {
        open_st::ShutdownUiText();
        ASSERT_TRUE(open_st::JsonFileTestAccess::ReleaseFile("localization.ui_text"));
        const testing::TestInfo* info = testing::UnitTest::GetInstance()->current_test_info();
        this->root_ = std::filesystem::temp_directory_path() /
                      ("Open-ST-about-contract-" + std::to_string(GetCurrentProcessId()) + "-" + info->name());
        std::filesystem::create_directories(this->root_ / "resources");
        std::filesystem::create_directories(this->root_ / "data");
        const nlohmann::json texts{{"about.title", {{"en-US", "About contract"}}},
                                   {"about.body", {{"en-US", "Version {version}"}}},
                                   {"update.title", {{"en-US", "Update contract"}}},
                                   {"update.check", {{"en-US", "Check updates"}}},
                                   {"update.cancel_download", {{"en-US", "Cancel download"}}},
                                   {"dialog.ok", {{"en-US", "Confirm"}}},
                                   {"dialog.cancel", {{"en-US", "Cancel"}}},
                                   {"update.confirm_install", {{"en-US", "Download {current} to {latest}"}}},
                                   {"update.confirm_portable", {{"en-US", "Page {current} to {latest}"}}},
                                   {"update.current", {{"en-US", "Already current"}}},
                                   {"update.checking", {{"en-US", "Checking"}}},
                                   {"update.downloading", {{"en-US", "Downloading {percent}"}}},
                                   {"update.cancelled", {{"en-US", "Cancelled"}}},
                                   {"update.installer_opened", {{"en-US", "Opened setup"}}},
                                   {"update.portable_instructions", {{"en-US", "Extract portable"}}}};
        const nlohmann::json document{{"schemaVersion", 1}, {"languages", {"en-US"}}, {"texts", texts}};
        {
            std::ofstream output(this->root_ / "resources/ui_text.json", std::ios::binary);
            output << document.dump();
            ASSERT_TRUE(output.good());
        }
        ASSERT_TRUE(open_st::InitializeUiText(this->root_));
        ASSERT_TRUE(open_st::IsUiTextAvailable());
        this->network_ = std::make_shared<AboutNetworkState>();
        activeNetwork = this->network_;
    }

    // 回收测试定时器和本地化缓存，再删除本用例目录。
    // 入参：无。
    // 返回：无。
    void TearDown() override
    {
        if (this->timer_ != 0)
            KillTimer(nullptr, this->timer_);
        activeNetwork.reset();
        open_st::ShutdownUiText();
        EXPECT_TRUE(open_st::JsonFileTestAccess::ReleaseFile("localization.ui_text"));
        std::error_code error;
        std::filesystem::remove_all(this->root_, error);
        EXPECT_FALSE(error);
    }

    // 点击已定位的真实按钮，缺失时使本用例明确失败。
    // 入参：window 为测试窗口；text 为控件标签。
    // 返回：找到并执行点击时 true。
    bool Click(HWND window, std::wstring text)
    {
        const HWND button = FindChild(window, std::move(text));
        EXPECT_NE(button, nullptr);
        if (button == nullptr)
            return false;
        SendMessageW(button, BM_CLICK, 0, 0);
        return true;
    }

    // 由真实模态消息转发驱动明确场景步骤，时限仅负责失败收敛而不代替验收。
    // 入参：无。
    // 返回：无。
    void Drive()
    {
        const HWND about = this->about_ ? this->about_->NativeHandle() : nullptr;
        const HWND prompt = FindTop(L"Update contract");
        if (std::chrono::steady_clock::now() >= this->deadline_)
        {
            if (!this->timedOut_)
                ADD_FAILURE() << "Automatic about-window contract did not finish its expected interaction.";
            this->timedOut_ = true;
            this->stopped_ = true;
            if (prompt)
                PostMessageW(prompt, WM_CLOSE, 0, 0);
            if (about)
                PostMessageW(about, WM_CLOSE, 0, 0);
            return;
        }
        if (about == nullptr)
            return;
        if (this->step_ == 0)
        {
            this->step_ = 1;
            (void)this->Click(about, L"Check updates");
            return;
        }
        if (this->scenario_ == AboutScenario::ClosePending && this->network_->queries != 0)
        {
            this->step_ = 3;
            PostMessageW(about, WM_CLOSE, 0, 0);
            return;
        }
        if (this->step_ == 1 && prompt != nullptr)
        {
            ++this->prompts_;
            EXPECT_EQ(this->network_->assets.load(), 0U);
            EXPECT_EQ(this->launches_, 0U);
            EXPECT_EQ(this->pages_, 0U);
            if (this->scenario_ == AboutScenario::Current)
            {
                EXPECT_NE(FindChild(prompt, L"Already current"), nullptr);
                EXPECT_EQ(FindChild(prompt, L"Cancel"), nullptr);
                this->step_ = 3;
                (void)this->Click(prompt, L"Confirm");
            }
            else if (this->scenario_ == AboutScenario::InstallCancel)
            {
                EXPECT_NE(FindChild(prompt, L"Download 1.0.0 to 1.1.0"), nullptr);
                this->step_ = 3;
                (void)this->Click(prompt, L"Cancel");
            }
            else
            {
                EXPECT_NE(FindChild(prompt, this->scenario_ == AboutScenario::PortableConfirm
                                                ? L"Page 1.0.0 to 1.1.0"
                                                : L"Download 1.0.0 to 1.1.0"),
                          nullptr);
                this->step_ = 2;
                (void)this->Click(prompt, L"Confirm");
            }
            return;
        }
        if (this->step_ == 2 && (this->launches_ != 0 || this->pages_ != 0))
            this->step_ = 3;
        if (this->step_ == 3 && prompt == nullptr)
            PostMessageW(about, WM_CLOSE, 0, 0);
    }

    // 使用真实关于窗口和共享 Renderer，只将外部网络／启动副作用替换。
    // 入参：scenario 为本次自动交互场景。
    // 返回：无，检查窗口正常结束和外部借用清除。
    void Run(AboutScenario scenario)
    {
        this->scenario_ = scenario;
        this->network_->holdQuery = scenario == AboutScenario::ClosePending;
        this->deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        this->timer_ = SetTimer(nullptr, 0, 15, nullptr);
        ASSERT_NE(this->timer_, 0U);
        open_st::AboutWindowOptions options;
        options.version = scenario == AboutScenario::Current ? "1.1.0" : "1.0.0";
        options.distribution = scenario == AboutScenario::PortableConfirm ? "portable" : "installed";
        options.cacheRoot = this->root_ / "data/updates";
        options.activeRenderer = &this->about_;
        // 模拟应用退出请求，只有测试失败超时才主动触发。
        // 入参：无。
        // 返回：是否要求停止。
        options.stopping = [this]() { return this->stopped_; };
        // 复用生产的模态消息转发，让驱动在按钮回调之外执行。
        // 入参：message 为真实线程消息。
        // 返回：只消费当前测试定时器。
        options.processThreadMessage = [this](MSG& message)
        {
            if (message.hwnd == nullptr && message.message == WM_TIMER && message.wParam == this->timer_)
            {
                this->Drive();
                return true;
            }
            return false;
        };
        // 记录具体发行页，不调用浏览器或 Shell。
        // 入参：page 为产品校验过的 URL。
        // 返回：模拟启动成功。
        options.openPage = [this](const std::wstring& page)
        {
            ++this->pages_;
            this->openedPage_ = page;
            return true;
        };
        // 验证系统交接时文件已经校验、存在且仍被保护，不执行其中的测试字节。
        // 入参：file 为下载路径；owner 为真实关于窗口。
        // 返回：模拟交接成功。
        options.launchInstaller = [this](const std::filesystem::path& file, HWND owner)
        {
            ++this->launches_;
            EXPECT_TRUE(IsWindow(owner));
            EXPECT_EQ(file.extension(), L".exe");
            std::ifstream input(file, std::ios::binary);
            std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
            EXPECT_EQ(bytes, "abc");
            const HANDLE write = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                             OPEN_EXISTING, 0, nullptr);
            EXPECT_EQ(write, INVALID_HANDLE_VALUE);
            if (write != INVALID_HANDLE_VALUE)
                CloseHandle(write);
            return true;
        };
        EXPECT_TRUE(open_st::ShowAboutWindow(options));
        EXPECT_FALSE(this->timedOut_);
        EXPECT_EQ(this->about_, nullptr);
        KillTimer(nullptr, this->timer_);
        this->timer_ = 0;
    }
};
} // namespace

namespace open_st::update_detail
{
// 独立测试目标在链接时替代唯一生产工厂，避免 WinHTTP 实现目标被静态库抽入。
// 入参：无。
// 返回：仅消费内存响应的替身，产品接口保持不变。
std::unique_ptr<UpdateTransport> MakeWinHttpUpdateTransport()
{
    return std::make_unique<AboutTransport>(activeNetwork);
}
} // namespace open_st::update_detail

// 安装版只有确认新版后才请求资产，并在校验成功后把受保护文件交给宿主。
// 入参：无。
// 返回：自动窗口契约断言，不代表人工视觉验收。
TEST_F(AboutUpdateIntegrationTest, installed_downloads_and_hands_off_only_after_confirmation)
{
    this->Run(AboutScenario::InstallConfirm);
    EXPECT_EQ(this->prompts_, 1U);
    EXPECT_EQ(this->network_->queries.load(), 1U);
    EXPECT_EQ(this->network_->assets.load(), 1U);
    EXPECT_EQ(this->launches_, 1U);
    EXPECT_EQ(this->pages_, 0U);
}

// 取消确认窗口后不得请求安装包或产生任何外部启动意图。
// 入参：无。
// 返回：自动窗口契约断言。
TEST_F(AboutUpdateIntegrationTest, cancelling_confirmation_downloads_and_opens_nothing)
{
    this->Run(AboutScenario::InstallCancel);
    EXPECT_EQ(this->prompts_, 1U);
    EXPECT_EQ(this->network_->assets.load(), 0U);
    EXPECT_EQ(this->launches_, 0U);
    EXPECT_EQ(this->pages_, 0U);
}

// 便携版确认后只打开所查询版本页面，绝不请求 Setup 或触发安装器回调。
// 入参：无。
// 返回：自动窗口契约断言。
TEST_F(AboutUpdateIntegrationTest, portable_confirmation_opens_fixed_release_page_without_download)
{
    this->Run(AboutScenario::PortableConfirm);
    EXPECT_EQ(this->prompts_, 1U);
    EXPECT_EQ(this->pages_, 1U);
    EXPECT_EQ(this->openedPage_, L"https://github.com/Lucency09/Open-ST/releases/tag/v1.1.0");
    EXPECT_EQ(this->network_->assets.load(), 0U);
    EXPECT_EQ(this->launches_, 0U);
}

// 已是最新版本显示普通提示，确认提示不会被误认为确认下载。
// 入参：无。
// 返回：自动窗口契约断言。
TEST_F(AboutUpdateIntegrationTest, current_version_message_never_triggers_download)
{
    this->Run(AboutScenario::Current);
    EXPECT_EQ(this->prompts_, 1U);
    EXPECT_EQ(this->network_->assets.load(), 0U);
    EXPECT_EQ(this->launches_, 0U);
    EXPECT_EQ(this->pages_, 0U);
}

// 关闭关于窗口取消待处理查询，即使边界随后返回成功也不能弹确认或启动外部动作。
// 入参：无。
// 返回：自动窗口契约断言。
TEST_F(AboutUpdateIntegrationTest, closing_about_cancels_pending_query_and_discards_late_success)
{
    this->Run(AboutScenario::ClosePending);
    EXPECT_TRUE(this->network_->cancellationObserved);
    EXPECT_TRUE(this->network_->lateResponseProduced);
    EXPECT_EQ(this->prompts_, 0U);
    EXPECT_EQ(this->network_->assets.load(), 0U);
    EXPECT_EQ(this->launches_, 0U);
    EXPECT_EQ(this->pages_, 0U);
}
