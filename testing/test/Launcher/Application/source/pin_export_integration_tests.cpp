// 在独立测试链接中替换系统输出入口，验证 App 贴图导出而不写真实剪贴板或图片。
#include "json_file_test_access.h"
#include "save_image_dialog.h"
#include "settings_internal.h"
#include "simple_message_window.h"
#include "ui_text_internal.h"
#include <app.h>
#include <array>
#include <clipboard_writer.h>
#include <desktop_capturer.h>
#include <fstream>
#include <functional>
#include <gtest/gtest.h>
#include <image_file_writer.h>
#include <objbase.h>
#include <pin_image.h>
#include <pin_window_manager.h>
#include <settings.h>
#include <ui_text.h>
#include <vector>

namespace open_st
{
namespace
{
struct ExportProbe final
{
    int copies{};
    int writes{};
    int dialogs{};
    int notices{};
    int captures{};
    bool copyResult{true};
    bool writeResult{true};
    SaveChoice choice{SaveChoice::Cancelled};
    std::vector<std::uint8_t> pixels;
    std::function<void(HWND)> duringDialog;
    std::function<void(HWND)> duringNotice;
    std::function<void()> duringCapture;
    std::filesystem::path target;
    std::wstring noticeText;
};
ExportProbe probe;
} // namespace

// 在正式 App 捕获入口处观察旧贴图状态，随后模拟捕获失败以验证回收。
// 入参：frame 为失败时清空的帧，error 为测试诊断输出。
// 返回：固定 false，不读取用户桌面。
bool DesktopCapturer::Capture(FrozenDesktopFrame& frame, std::wstring& error) const
{
    ++probe.captures;
    if (probe.duringCapture)
        probe.duringCapture();
    frame.Clear();
    error = L"Injected desktop capture failure.";
    return false;
}

// 隔离系统剪贴板，仅记录正式 App 传入的原始图像。
// 入参：owner 为受保护 HWND，image 为原图视图，error 未使用。
// 返回：模拟复制成功，不调用系统剪贴板。
bool CopyImageToClipboard(HWND owner, const SdrImageView& image, std::wstring&)
{
    EXPECT_TRUE(IsWindow(owner));
    ++probe.copies;
    probe.pixels.assign(image.pixels.begin(), image.pixels.end());
    return probe.copyResult;
}

// 隔离编码写入，记录正式输出像素而不创建图片文件。
// 入参：image 为原图，path 为保存位置，format 为选择格式，error 未使用。
// 返回：配置的模拟写入结果，不创建文件。
bool WriteImageFile(const SdrImageView& image, const std::filesystem::path& path, ImageFileFormat format, std::wstring&)
{
    EXPECT_EQ(path, probe.target);
    EXPECT_EQ(format, ImageFileFormat::Png);
    ++probe.writes;
    probe.pixels.assign(image.pixels.begin(), image.pixels.end());
    return probe.writeResult;
}

// 隔离故障提示的窗口显示，保留正式 App 模态门禁和传入的 owner。
// 入参：owner 为借用 HWND，其余文本、Renderer 与消息回调不在此替身中执行。
// 返回：模拟提示正常关闭，不等待人工确认。
bool TryShowSimpleMessageWindow(HWND owner, HICON, const std::function<std::wstring()>&,
                                const std::function<std::wstring()>& message, const std::function<std::wstring()>&,
                                WindowRenderer*&, const std::function<bool(MSG&)>&) noexcept
{
    ++probe.notices;
    probe.noticeText = message();
    EXPECT_TRUE(IsWindow(owner));
    if (probe.duringNotice)
        probe.duringNotice(owner);
    EXPECT_TRUE(IsWindow(owner));
    return true;
}

// 模拟系统对话框返回，并在正式 PinOperation 栈内注入关闭事件。
// 入参：owner 为受保护 HWND，lastDirectory 未使用，target 接收目标，error 未使用。
// 返回：配置的确认或取消结果，不打开系统对话框。
SaveChoice ShowSaveImageDialog(HWND owner, const std::filesystem::path&, SaveImageTarget& target, std::wstring&)
{
    ++probe.dialogs;
    EXPECT_TRUE(IsWindow(owner));
    if (probe.duringDialog)
        probe.duringDialog(owner);
    EXPECT_TRUE(IsWindow(owner));
    target.path = probe.target;
    target.format = ImageFileFormat::Png;
    return probe.choice;
}

struct AppPinTestAccess final
{
    // 创建真实消息窗口并接入生产 Manager 回调。
    // 入参：app 为隔离宿主。
    // 返回：消息窗口创建成功时 true。
    static bool Initialize(App& app)
    {
        if (!app.CreateMessageWindow())
            return false;
        app.pinManager_ = std::make_unique<PinWindowManager>(GetModuleHandleW(nullptr), app.MakePinCallbacks());
        return true;
    }
    // 借用管理器用于检查窗口存活及模态保护。
    // 入参：app 为隔离宿主。
    // 返回：宿主存活期间有效的引用。
    static PinWindowManager& Manager(App& app)
    {
        return *app.pinManager_;
    }
    // 通过正式消息队列提交并消费输出请求。
    // 入参：app 为宿主，id 为可见贴图，command 为复制或保存。
    // 返回：成功投递且找到对应消息时 true。
    static bool Execute(App& app, PinId id, PinCommand command)
    {
        if (!app.PostPinCommand(id, command))
            return false;
        MSG message{};
        if (!PeekMessageW(&message, app.messageWindow_, WM_APP + 5, WM_APP + 5, PM_REMOVE))
            return false;
        DispatchMessageW(&message);
        return true;
    }
    // 通过正式窗口入口请求退出，验证业务栈内的延迟清理。
    // 入参：app 为隔离宿主。
    // 返回：无返回值。
    static void Exit(App& app)
    {
        SendMessageW(app.messageWindow_, WM_CLOSE, 0, 0);
    }
    // 进入正式截图流程以验证调用捕获时的窗口状态。
    // 入参：app 为隔离宿主。
    // 返回：无返回值，测试捕获替身负责返回失败。
    static void Capture(App& app)
    {
        app.StartCapture();
    }
};

class PinExportIntegrationTest : public testing::Test
{
  protected:
    // 建立隔离设置、COM 和可见小贴图，所有输出仍由本文件替身接收。
    // 入参：无。
    // 返回：无返回值，初始化失败通过测试断言报告。
    void SetUp() override
    {
        probe = {};
        this->com_ = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        ASSERT_TRUE(SUCCEEDED(this->com_));
        this->root_ =
            std::filesystem::temp_directory_path() /
            ("open_st_pin_export_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(GetTickCount64()));
        ASSERT_TRUE(std::filesystem::create_directories(this->root_ / "resources"));
        std::ofstream(this->root_ / "resources/default_settings.json") << R"({"schemaVersion":1,"settings":{}})";
        ShutdownSettings();
        ASSERT_TRUE(JsonFileTestAccess::ReleaseFile("settings.user"));
        ASSERT_TRUE(JsonFileTestAccess::ReleaseFile("settings.default"));
        ASSERT_TRUE(InitializeSettings(this->root_));
        ShutdownUiText();
        ASSERT_TRUE(JsonFileTestAccess::ReleaseFile("localization.ui_text"));
        std::ofstream(this->root_ / "resources/ui_text.json")
            << R"({"schemaVersion":1,"languages":["en-US"],"texts":{"export.directory_failed":{"en-US":"Directory warning"},"export.save_failed":{"en-US":"Save failed"}}})";
        ASSERT_TRUE(InitializeUiText(this->root_));
        probe.target = this->root_ / "capture.png";
        this->app_ = std::make_unique<App>(GetModuleHandleW(nullptr));
        ASSERT_TRUE(AppPinTestAccess::Initialize(*this->app_));
        std::wstring error;
        this->image_ = PinImage::Create(2, 2, 8, this->pixels_, error);
        ASSERT_NE(this->image_, nullptr);
        PinWindowManager& manager = AppPinTestAccess::Manager(*this->app_);
        ASSERT_TRUE(manager.Prepare(this->image_, {80, 80}, this->id_, error)) << error;
        manager.Show(this->id_);
        ASSERT_TRUE(IsWindowVisible(manager.Window(this->id_)));
    }
    // 清理自有窗口、设置绑定和本次唯一临时目录，并消费测试退出消息。
    // 入参：无。
    // 返回：无返回值，不触碰用户真实设置或图片。
    void TearDown() override
    {
        this->app_.reset();
        this->image_.reset();
        ShutdownSettings();
        ShutdownUiText();
        EXPECT_TRUE(JsonFileTestAccess::ReleaseFile("localization.ui_text"));
        EXPECT_TRUE(JsonFileTestAccess::ReleaseFile("settings.user"));
        EXPECT_TRUE(JsonFileTestAccess::ReleaseFile("settings.default"));
        if (!this->root_.empty())
        {
            std::error_code error;
            std::filesystem::remove_all(this->root_, error);
            EXPECT_FALSE(error);
        }
        MSG message{};
        while (PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE))
        {
        }
        probe = {};
        if (SUCCEEDED(this->com_))
            CoUninitialize();
    }
    const std::array<std::uint8_t, 16> pixels_{1, 2, 3, 0, 4, 5, 6, 17, 7, 8, 9, 0, 10, 11, 12, 63};
    std::filesystem::path root_;
    std::unique_ptr<App> app_;
    std::shared_ptr<const PinImage> image_;
    PinId id_{};
    HRESULT com_{E_FAIL};
};

// 验证正式复制分派传出原图全部字节，复制后贴图仍可继续使用。
// 入参：无运行入参；宏参数用于测试注册。
// 返回：无返回值，以逐字节断言覆盖 BGRX 第四字节保留。
TEST_F(PinExportIntegrationTest, copy_uses_original_pixels_and_keeps_pin)
{
    ASSERT_TRUE(AppPinTestAccess::Execute(*this->app_, this->id_, PinCommand::Copy));
    EXPECT_EQ(probe.copies, 1);
    EXPECT_EQ(probe.pixels, (std::vector<std::uint8_t>(this->pixels_.begin(), this->pixels_.end())));
    EXPECT_EQ(AppPinTestAccess::Manager(*this->app_).Image(this->id_), this->image_);
    EXPECT_FALSE(AppPinTestAccess::Manager(*this->app_).IsBusy());
}

// 验证保存取消不写图片或目录记忆，模态结束后仍能复制同一贴图。
// 入参：无运行入参；宏参数用于测试注册。
// 返回：无返回值，正式保存栈的取消分支被实际执行。
TEST_F(PinExportIntegrationTest, cancelled_save_preserves_pin_and_allows_next_command)
{
    ASSERT_TRUE(AppPinTestAccess::Execute(*this->app_, this->id_, PinCommand::Save));
    EXPECT_EQ(probe.dialogs, 1);
    EXPECT_EQ(probe.writes, 0);
    EXPECT_FALSE(GetStringSetting("capture.last_save_directory").has_value());
    EXPECT_EQ(AppPinTestAccess::Manager(*this->app_).Image(this->id_), this->image_);
    ASSERT_TRUE(AppPinTestAccess::Execute(*this->app_, this->id_, PinCommand::Copy));
    EXPECT_EQ(probe.copies, 1);
}

// 验证保存成功只输出原图并记录隔离目录，不改变贴图生命周期。
// 入参：无运行入参；宏参数用于测试注册。
// 返回：无返回值；不实际创建 PNG 文件。
TEST_F(PinExportIntegrationTest, accepted_save_uses_original_and_remembers_directory)
{
    probe.choice = SaveChoice::Accepted;
    ASSERT_TRUE(AppPinTestAccess::Execute(*this->app_, this->id_, PinCommand::Save));
    EXPECT_EQ(probe.writes, 1);
    EXPECT_EQ(probe.pixels, (std::vector<std::uint8_t>(this->pixels_.begin(), this->pixels_.end())));
    const std::u8string expected = this->root_.u8string();
    EXPECT_EQ(GetStringSetting("capture.last_save_directory"),
              std::string(reinterpret_cast<const char*>(expected.data()), expected.size()));
    EXPECT_EQ(AppPinTestAccess::Manager(*this->app_).Image(this->id_), this->image_);
    EXPECT_FALSE(std::filesystem::exists(probe.target));
}

// 验证保存失败提示使用仍受保护的原贴图 HWND，关闭后仍能再次输出。
// 入参：无运行入参；宏参数用于测试注册。
// 返回：无返回值；错误提示被替身正常关闭。
TEST_F(PinExportIntegrationTest, write_failure_keeps_pin_and_protects_error_owner)
{
    PinWindowManager& manager = AppPinTestAccess::Manager(*this->app_);
    const HWND owner = manager.Window(this->id_);
    probe.choice = SaveChoice::Accepted;
    probe.writeResult = false;
    // 核对提示处于 Manager 模态中并沿用导出 owner。
    // 入参：window 为 App 传入的提示 HWND。
    // 返回：无返回值。
    probe.duringNotice = [&manager, owner](HWND window)
    {
        EXPECT_EQ(window, owner);
        EXPECT_TRUE(manager.IsBusy());
    };
    ASSERT_TRUE(AppPinTestAccess::Execute(*this->app_, this->id_, PinCommand::Save));
    EXPECT_EQ(probe.notices, 1);
    EXPECT_EQ(manager.Image(this->id_), this->image_);
    EXPECT_FALSE(manager.IsBusy());
    EXPECT_FALSE(GetStringSetting("capture.last_save_directory").has_value());
    ASSERT_TRUE(AppPinTestAccess::Execute(*this->app_, this->id_, PinCommand::Copy));
    EXPECT_EQ(probe.copies, 1);
}

// 验证文件成功保存后目录编码异常只触发次要警告，贴图仍能继续复制。
// 入参：无运行入参；宏参数用于测试注册。
// 返回：无返回值。
TEST_F(PinExportIntegrationTest, saved_image_survives_directory_conversion_exception)
{
    PinWindowManager& manager = AppPinTestAccess::Manager(*this->app_);
    probe.choice = SaveChoice::Accepted;
    probe.target = this->root_ / std::wstring(1, static_cast<wchar_t>(0xD800)) / L"capture.png";
    EXPECT_THROW((void)probe.target.parent_path().u8string(), std::system_error);
    ASSERT_TRUE(AppPinTestAccess::Execute(*this->app_, this->id_, PinCommand::Save));
    EXPECT_EQ(probe.writes, 1);
    EXPECT_EQ(probe.notices, 1);
    EXPECT_EQ(probe.noticeText, L"Directory warning");
    EXPECT_EQ(manager.Image(this->id_), this->image_);
    EXPECT_TRUE(IsWindowVisible(manager.Window(this->id_)));
    EXPECT_FALSE(manager.IsBusy());
    EXPECT_FALSE(GetStringSetting("capture.last_save_directory").has_value());
    ASSERT_TRUE(AppPinTestAccess::Execute(*this->app_, this->id_, PinCommand::Copy));
    EXPECT_EQ(probe.pixels, (std::vector<std::uint8_t>(this->pixels_.begin(), this->pixels_.end())));
}

// 验证对话框内关闭全部使 ID 失效，但 owner 保留至保存栈返回且不得写出。
// 入参：无运行入参；宏参数用于测试注册。
// 返回：通过断言报告输出与窗口生命周期。
TEST_F(PinExportIntegrationTest, close_all_during_save_defers_owner_and_discards_output)
{
    PinWindowManager& manager = AppPinTestAccess::Manager(*this->app_);
    const HWND owner = manager.Window(this->id_);
    probe.choice = SaveChoice::Accepted;
    // 在正式保存调用栈中模拟管理器关闭请求。
    // 入参：window 为保存对话框的所属 HWND。
    // 返回：无返回值；窗口保留但图像不可再提交新业务。
    probe.duringDialog = [&manager](HWND window)
    {
        manager.CloseAll();
        EXPECT_EQ(manager.Count(), 0U);
        EXPECT_TRUE(manager.IsBusy());
        EXPECT_TRUE(IsWindow(window));
    };
    ASSERT_TRUE(AppPinTestAccess::Execute(*this->app_, this->id_, PinCommand::Save));
    EXPECT_EQ(probe.writes, 0);
    EXPECT_FALSE(IsWindow(owner));
    EXPECT_FALSE(manager.IsBusy());
}

// 验证 App 退出请求在保存期间延迟 owner 销毁，确认返回后也不输出图片。
// 入参：无运行入参；宏参数用于测试注册。
// 返回：无返回值；正式 WM_CLOSE 入口负责设置退出状态。
TEST_F(PinExportIntegrationTest, app_exit_during_save_preserves_owner_until_return)
{
    PinWindowManager& manager = AppPinTestAccess::Manager(*this->app_);
    const HWND owner = manager.Window(this->id_);
    probe.choice = SaveChoice::Accepted;
    // 在正式保存栈内向 App 发送退出消息。
    // 入参：window 为被保护的所属 HWND。
    // 返回：无返回值。
    probe.duringDialog = [this](HWND window)
    {
        AppPinTestAccess::Exit(*this->app_);
        EXPECT_TRUE(IsWindow(window));
        EXPECT_FALSE(AppPinTestAccess::Execute(*this->app_, this->id_, PinCommand::Copy));
    };
    ASSERT_TRUE(AppPinTestAccess::Execute(*this->app_, this->id_, PinCommand::Save));
    EXPECT_EQ(probe.writes, 0);
    EXPECT_EQ(probe.copies, 0);
    EXPECT_FALSE(IsWindow(owner));
    EXPECT_EQ(manager.Count(), 0U);
    EXPECT_FALSE(manager.IsBusy());
}
// 验证 App 真正调用捕获时旧图仍显示，捕获失败后仍可复制原图。
// 入参：无运行入参；宏参数用于测试注册。
// 返回：无返回值；断言捕获边界而不是仅直接调用 Manager 的暂停接口。
TEST_F(PinExportIntegrationTest, recapture_keeps_existing_pin_visible_until_pixels_are_captured)
{
    PinWindowManager& manager = AppPinTestAccess::Manager(*this->app_);
    const HWND window = manager.Window(this->id_);
    // 在桌面捕获调用内部核对旧窗口仍然可见。
    // 入参：无。
    // 返回：无返回值。
    probe.duringCapture = [window]() { EXPECT_TRUE(IsWindowVisible(window)); };
    AppPinTestAccess::Capture(*this->app_);
    EXPECT_EQ(probe.captures, 1);
    EXPECT_EQ(probe.notices, 1);
    EXPECT_TRUE(IsWindowVisible(window));
    EXPECT_EQ(manager.Image(this->id_), this->image_);
    ASSERT_TRUE(AppPinTestAccess::Execute(*this->app_, this->id_, PinCommand::Copy));
    EXPECT_EQ(probe.copies, 1);
}
} // namespace open_st
