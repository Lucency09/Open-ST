// 验证截图实时设置面板、临时参数隔离和后台预览资格；只使用自造像素及本测试窗口。
#include "capture_visual_session.h"
#include "settings_internal.h"
#include "json_file_test_access.h"
#include <settings.h>
#include <filesystem>
#include <fstream>
#include <Windows.h>
#include <commctrl.h>
#include <frozen_desktop_frame.h>
#include <gtest/gtest.h>
#include <sdr_selection_frame.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <limits>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
// 构造四像素HDR或SDR冻结输入，不读取真实屏幕和用户配置。
// 入参：hdr选择scRGB半精度白色或SDR字节。返回：不可变共享帧。
std::shared_ptr<const open_st::FrozenDesktopFrame> VisualDesktop(bool hdr)
{
    const open_st::RectI bounds{0, 0, 4, 1};
    std::vector<std::uint8_t> bytes;
    for (unsigned index = 0; index < 4; ++index)
    {
        if (hdr)
            bytes.insert(bytes.end(), {0x00, 0x3c, 0x00, 0x3c, 0x00, 0x3c, 0x00, 0x3c});
        else
            bytes.insert(bytes.end(), {40, 80, 120, 255});
    }
    open_st::OutputColorMetadata metadata{};
    metadata.displayColorSpace = hdr ? open_st::CapturedColorSpace::ScRgb : open_st::CapturedColorSpace::SdrGamma22P709;
    metadata.maximumLuminance = 1000.0F;
    metadata.sdrWhiteLevelNits = 80.0F;
    metadata.hasSdrWhiteLevel = true;
    std::vector<open_st::CapturedOutputPlane> planes;
    planes.emplace_back(bounds,
                        hdr ? open_st::CapturedPixelFormat::Rgba16FloatScRgb : open_st::CapturedPixelFormat::Bgra8Unorm,
                        metadata.displayColorSpace, metadata, std::move(bytes));
    return std::make_shared<const open_st::FrozenDesktopFrame>(bounds, std::move(planes));
}

class CaptureVisualSessionTest : public testing::Test
{
  protected:
    // 创建本线程的隐藏所有者窗口及正式会话，不注册热键或调用截图入口。
    void SetUp() override
    {
        this->owner_ = CreateWindowExW(0, L"STATIC", L"Capture visual test owner", WS_OVERLAPPEDWINDOW, 0, 0, 400, 300,
                                       nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ASSERT_NE(this->owner_, nullptr);
        this->session_ = std::make_unique<open_st::CaptureVisualSession>([]() {});
    }
    // 先释放会话及后台线程，随后销毁测试所有者，防止迟到通知借用无效窗口。
    void TearDown() override
    {
        this->session_.reset();
        if (this->owner_)
            DestroyWindow(this->owner_);
        this->Pump();
    }
    // 开始独立会话并提交全帧选区，回调只计数和重提当前测试选区。
    void Begin(bool hdr, open_st::CaptureVisualDefaults defaults = {100, 70, {4000U}})
    {
        this->frame_ = VisualDesktop(hdr);
        ASSERT_TRUE(this->frame_->IsValid());
        this->selection_ = this->frame_->Bounds();
        this->session_->Begin(this->frame_, defaults,
                              [this]()
                              {
                                  ++this->changes_;
                                  EXPECT_TRUE(this->session_->Request(this->selection_));
                              });
        ASSERT_TRUE(this->session_->Request(this->selection_));
    }
    // 使用正式公开Show入口，文本替身返回键名方便查找按钮。
    void ShowPanel()
    {
        ASSERT_TRUE(this->session_->Show(this->owner_, this->owner_, nullptr,
                                         [](std::string_view key) { return std::wstring(key.begin(), key.end()); }));
        ASSERT_NE(this->session_->Window(), nullptr);
        this->Pump();
    }
    // 只分派当前测试线程消息，保持非模态表单的正式输入入口。
    void Pump()
    {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            if (message.message == WM_QUIT)
                continue;
            if (!this->session_ || !this->session_->Process(message))
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
    }
    struct Search final
    {
        std::wstring type;
        std::wstring text;
        std::vector<HWND> matches;
    };
    // 通过Win32控件类/文字定位，不依赖Renderer内部ID或会话私有成员。
    static BOOL CALLBACK Find(HWND window, LPARAM data)
    {
        Search& search = *reinterpret_cast<Search*>(data);
        std::array<wchar_t, 128> type{}, text{};
        GetClassNameW(window, type.data(), static_cast<int>(type.size()));
        GetWindowTextW(window, text.data(), static_cast<int>(text.size()));
        if (search.type == type.data() && (search.text.empty() || search.text == text.data()))
            search.matches.push_back(window);
        return TRUE;
    }
    // 按物理垂直位置排序两条真实Trackbar，以验证用户可操作控件。
    std::vector<HWND> Sliders()
    {
        Search search{TRACKBAR_CLASSW, {}, {}};
        EnumChildWindows(this->session_->Window(), Find, reinterpret_cast<LPARAM>(&search));
        std::sort(search.matches.begin(), search.matches.end(),
                  [](HWND left, HWND right)
                  {
                      RECT a{}, b{};
                      GetWindowRect(left, &a);
                      GetWindowRect(right, &b);
                      return a.top < b.top;
                  });
        return search.matches;
    }
    // 向真实滑条父视口发送持续拖动通知，覆盖产品实际处理链路。
    void Drag(HWND slider, int value)
    {
        SendMessageW(slider, TBM_SETPOS, TRUE, value);
        SendMessageW(GetParent(slider), WM_HSCROLL, TB_THUMBTRACK, reinterpret_cast<LPARAM>(slider));
        this->Pump();
    }
    // 点击本面板内的实际按钮。
    void Click(const wchar_t* text)
    {
        Search search{L"Button", text, {}};
        EnumChildWindows(this->session_->Window(), Find, reinterpret_cast<LPARAM>(&search));
        ASSERT_EQ(search.matches.size(), 1U);
        SendMessageW(search.matches.front(), BM_CLICK, 0, 0);
        this->Pump();
    }
    // 等待有限时间消费正式后台转换，失败时返回可诊断错误。
    bool WaitPreview(std::wstring& error)
    {
        const std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        do
        {
            this->Pump();
            if (this->session_->Poll(error))
                return true;
            if (!error.empty())
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    }
    HWND owner_{};
    unsigned changes_{};
    open_st::RectI selection_{};
    std::shared_ptr<const open_st::FrozenDesktopFrame> frame_;
    std::unique_ptr<open_st::CaptureVisualSession> session_;
};

// 验证真实面板可显示，两条滑条每个拖动通知立即更新本次参数。
// 入参：无。返回：通过值、回调次数及预览版本断言持续生效。
TEST_F(CaptureVisualSessionTest, panel_continuous_sliders_update_only_current_session)
{
    this->Begin(true);
    this->ShowPanel();
    const std::vector<HWND> sliders = this->Sliders();
    ASSERT_EQ(sliders.size(), 2U);
    ASSERT_TRUE(IsWindowEnabled(sliders[0]));
    ASSERT_TRUE(IsWindowEnabled(sliders[1]));
    const std::uint64_t initial = this->session_->Version();
    this->Drag(sliders[0], 85);
    EXPECT_EQ(this->session_->Brightness(), 85U);
    this->Drag(sliders[0], 60);
    EXPECT_EQ(this->session_->Brightness(), 60U);
    EXPECT_GT(this->session_->Version(), initial);
    this->Drag(sliders[1], 80);
    EXPECT_EQ(this->session_->Mask(), 80U);
    this->Drag(sliders[1], 90);
    EXPECT_EQ(this->session_->Mask(), 90U);
    EXPECT_EQ(this->changes_, 4U);
}

// 验证关闭重开保留临时值，恢复取本次启动快照，新截图重新初始化默认。
// 入参：无。返回：用户会话状态与默认快照互不覆盖。
TEST_F(CaptureVisualSessionTest, close_reopen_restore_and_new_capture_follow_snapshot_lifetime)
{
    this->Begin(true, {90, 65, {4000U}});
    this->ShowPanel();
    std::vector<HWND> sliders = this->Sliders();
    ASSERT_EQ(sliders.size(), 2U);
    this->Drag(sliders[0], 55);
    this->Drag(sliders[1], 85);
    this->Click(L"capture.settings.close");
    EXPECT_EQ(this->session_->Window(), nullptr);
    this->ShowPanel();
    EXPECT_EQ(this->session_->Brightness(), 55U);
    EXPECT_EQ(this->session_->Mask(), 85U);
    this->Click(L"capture.settings.restore");
    EXPECT_EQ(this->session_->Brightness(), 90U);
    EXPECT_EQ(this->session_->Mask(), 65U);
    sliders = this->Sliders();
    ASSERT_EQ(sliders.size(), 2U);
    this->Drag(sliders[1], 88);
    this->session_->End();
    this->Begin(true, {110, 75, {4000U}});
    EXPECT_EQ(this->session_->Brightness(), 110U);
    EXPECT_EQ(this->session_->Mask(), 75U);
}

// 验证纯SDR禁用HDR滑条但允许遮罩调节；暂停交互时两者均禁用。
// 入参：无。返回：原生禁用状态和临时参数符合选区能力。
TEST_F(CaptureVisualSessionTest, sdr_disables_hdr_but_keeps_mask_and_interaction_gate)
{
    this->Begin(false);
    EXPECT_FALSE(this->session_->HasHdr(this->selection_));
    this->ShowPanel();
    const std::vector<HWND> sliders = this->Sliders();
    ASSERT_EQ(sliders.size(), 2U);
    EXPECT_FALSE(IsWindowEnabled(sliders[0]));
    EXPECT_TRUE(IsWindowEnabled(sliders[1]));
    this->Drag(sliders[1], 82);
    EXPECT_EQ(this->session_->Mask(), 82U);
    EXPECT_EQ(this->session_->Brightness(), 100U);
    this->session_->SetInteractive(false);
    EXPECT_FALSE(IsWindowEnabled(sliders[0]));
    EXPECT_FALSE(IsWindowEnabled(sliders[1]));
    this->session_->SetInteractive(true);
    EXPECT_FALSE(IsWindowEnabled(sliders[0]));
    EXPECT_TRUE(IsWindowEnabled(sliders[1]));
    EXPECT_EQ(this->session_->Mask(), 82U);
}

// 验证同一冻结输入快速改变请求后，只能发布最新选区结果。
// 入参：无。返回：当前版本以及输出边界严格对应最后一次请求。
TEST_F(CaptureVisualSessionTest, rapid_requests_publish_only_latest_selection)
{
    this->Begin(true);
    const std::uint64_t first = this->session_->Version();
    ASSERT_TRUE(this->session_->Request({0, 0, 2, 1}));
    ASSERT_TRUE(this->session_->Request({3, 0, 4, 1}));
    EXPECT_GT(this->session_->Version(), first);
    std::wstring error;
    ASSERT_TRUE(this->WaitPreview(error)) << error;
    const auto preview = this->session_->Preview();
    ASSERT_TRUE(preview);
    EXPECT_EQ(preview->Bounds().left, 3);
    EXPECT_EQ(preview->Bounds().right, 4);
    EXPECT_EQ(preview->Pixels().size(), 4U);
}

// 验证结束会话立即撤销结果资格，迟到HDR转换不会污染随后的SDR会话。
// 入参：无。返回：取消后无发布、无借用结果，后台最终安全停止。
TEST_F(CaptureVisualSessionTest, end_rejects_late_results_and_next_session_starts_clean)
{
    this->Begin(true);
    this->session_->End();
    EXPECT_FALSE(this->session_->Request({0, 0, 1, 1}));
    EXPECT_EQ(this->session_->Window(), nullptr);
    this->Begin(false, {80, 60, {4000U}});
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::wstring error;
    do
    {
        EXPECT_FALSE(this->session_->Poll(error));
        EXPECT_TRUE(error.empty());
        EXPECT_FALSE(this->session_->Preview());
        if (!this->session_->HasWork())
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (std::chrono::steady_clock::now() < deadline);
    EXPECT_FALSE(this->session_->HasWork());
    EXPECT_FALSE(this->session_->Poll(error));
    EXPECT_EQ(this->session_->Brightness(), 80U);
    EXPECT_EQ(this->session_->Mask(), 60U);
}
} // namespace

// 验证会话持有策略值副本，后续调用方草稿修改不会影响已提交预览。
// 入参：无。返回：断言不同会话明确接收不同高光上限。
TEST_F(CaptureVisualSessionTest, tone_mapping_policy_is_a_session_value_snapshot)
{
    open_st::CaptureVisualDefaults defaults{100U, 70U, {1000U}};
    this->Begin(true, defaults);
    defaults.toneMapping.highlightCeilingNits = 10000U;
    EXPECT_EQ(this->session_->ToneMappingOptions().highlightCeilingNits, 1000U);
    this->Begin(true, defaults);
    EXPECT_EQ(this->session_->ToneMappingOptions().highlightCeilingNits, 10000U);
}

// 正式配置快照仅补缺失HDR字段，显式非法值拒绝且不会重置用户已有亮度。
// 入参：无。返回：隔离配置读写与快照断言，不访问用户文件。
TEST(CaptureVisualConfigurationTest, rejects_invalid_policy_and_backfills_only_missing_fields)
{
    const std::filesystem::path root = std::filesystem::path(OPEN_ST_TEST_SOURCE_ROOT) / "testing/testoutput" /
                                       ("hdr-policy-" + std::to_string(GetCurrentProcessId()));
    struct Guard
    {
        std::filesystem::path root;
        ~Guard()
        {
            open_st::ShutdownSettings();
            (void)open_st::JsonFileTestAccess::ReleaseFile("settings.user");
            (void)open_st::JsonFileTestAccess::ReleaseFile("settings.default");
            std::error_code ignored;
            std::filesystem::remove_all(this->root, ignored);
        }
    } guard{root};
    open_st::ShutdownSettings();
    (void)open_st::JsonFileTestAccess::ReleaseFile("settings.user");
    (void)open_st::JsonFileTestAccess::ReleaseFile("settings.default");
    std::filesystem::create_directories(root / "resources");
    const nlohmann::json defaults{{"schemaVersion", 1},
                                  {"settings",
                                   {{"capture.hdr_brightness_percent", 100},
                                    {"capture.mask_opacity_percent", 70},
                                    {"capture.hdr_tone_mapping", {{"highlight_ceiling_nits", 6000}}}}}};
    {
        std::ofstream file(root / "resources/default_settings.json");
        file << defaults.dump();
        ASSERT_TRUE(file.good());
    }
    ASSERT_TRUE(open_st::InitializeSettings(root));
    // 直接模拟运行期间外部修改，让读取逻辑而不是启动迁移负责缺项处理。
    const auto writePolicy = [&root](const nlohmann::json& policy)
    {
        std::ofstream file(root / "data/settings.json", std::ios::trunc);
        file << nlohmann::json{{"schemaVersion", 1},
                               {"settings",
                                {{"capture.hdr_brightness_percent", 150},
                                 {"capture.mask_opacity_percent", 50},
                                 {"capture.hdr_tone_mapping", policy}}}}
                    .dump();
        ASSERT_TRUE(file.good());
    };
    writePolicy(nlohmann::json::object());
    std::optional<open_st::CaptureVisualDefaults> snapshot = open_st::ReadCaptureVisualDefaults();
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot->brightness, 150U);
    EXPECT_EQ(snapshot->toneMapping.highlightCeilingNits, 6000U);
    for (const unsigned int value : {1000U, 8000U, 10000U})
    {
        writePolicy({{"highlight_ceiling_nits", value}});
        snapshot = open_st::ReadCaptureVisualDefaults();
        ASSERT_TRUE(snapshot);
        EXPECT_EQ(snapshot->toneMapping.highlightCeilingNits, value);
    }
    const std::vector<nlohmann::json> invalid{nullptr,
                                              "bad",
                                              12,
                                              nlohmann::json::array(),
                                              {{"highlight_ceiling_nits", nullptr}},
                                              {{"highlight_ceiling_nits", "4000"}},
                                              {{"highlight_ceiling_nits", 4000.0}},
                                              {{"highlight_ceiling_nits", 999}},
                                              {{"highlight_ceiling_nits", 10001}},
                                              {{"highlight_ceiling_nits", -1}},
                                              {{"highlight_ceiling_nits", std::numeric_limits<std::uint64_t>::max()}}};
    for (const nlohmann::json& policy : invalid)
    {
        SCOPED_TRACE(policy.dump());
        writePolicy(policy);
        EXPECT_FALSE(open_st::ReadCaptureVisualDefaults());
    }
    {
        std::ofstream file(root / "data/settings.json", std::ios::trunc);
        file << "{broken";
    }
    EXPECT_FALSE(open_st::ReadCaptureVisualDefaults());
}
