// 在独立测试链接中替换系统输出入口，验证 App 贴图导出而不写真实剪贴板或图片。
#include "capture_annotation_state.h"
#include "capture_overlay_session.h"
#include "capture_storage_options.h"
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
    ImageFileFormat selectedFormat{ImageFileFormat::Png};
    ImageFileFormat initialFormat{ImageFileFormat::Png};
    ImageEncodingOptions encoding;
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
bool WriteImageFile(const SdrImageView& image, const std::filesystem::path& path, const ImageEncodingOptions& options,
                    std::wstring&)
{
    EXPECT_EQ(path, probe.target);
    EXPECT_EQ(options.format, probe.selectedFormat);
    probe.encoding = options;
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
SaveChoice ShowSaveImageDialog(HWND owner, const std::filesystem::path&, ImageFileFormat initialFormat,
                               SaveImageTarget& target, std::wstring&)
{
    ++probe.dialogs;
    probe.initialFormat = initialFormat;
    EXPECT_TRUE(IsWindow(owner));
    if (probe.duringDialog)
        probe.duringDialog(owner);
    EXPECT_TRUE(IsWindow(owner));
    target.path = probe.target;
    target.format = probe.selectedFormat;
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

struct AppAnnotationTestAccess final
{
    // 建立两个真实隐藏遮罩及一条可撤销标注，选区覆盖当前鼠标以确保按下输入前提有效。
    // 入参：app 为已有消息窗口的隔离宿主；windows 接收会话借用句柄。
    // 返回：全部建立成功为 true，不设置鼠标位置或创建图形设备。
    static bool Initialize(App& app, std::array<HWND, 2>& windows)
    {
        POINT cursor{};
        if (!GetCursorPos(&cursor))
        {
            return false;
        }
        app.selectionModel_ = std::make_unique<SelectionModel>();
        app.selectionModel_->SetBounds({cursor.x - 500, cursor.y - 500, cursor.x + 500, cursor.y + 500});
        const RectI crop{cursor.x - 100, cursor.y - 100, cursor.x + 100, cursor.y + 100};
        if (!app.selectionModel_->SelectRectangle(crop))
        {
            return false;
        }
        app.annotation_ = std::make_unique<CaptureAnnotationState>();
        app.annotation_->SetTool(CaptureAnnotationTool::Rectangle);
        if (!app.annotation_->BeginDraw({cursor.x - 40, cursor.y - 40}, crop) ||
            app.annotation_->EndDraw({cursor.x + 40, cursor.y + 40}) != AnnotationCommitResult::Committed)
        {
            return false;
        }
        app.overlaySession_ = std::make_unique<CaptureOverlaySession>();
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = App::OverlayProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = L"OpenST.AnnotationNoticeTest";
        if (RegisterClassW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            return false;
        }
        app.overlayPreparing_ = true;
        for (HWND& window : windows)
        {
            window = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, windowClass.lpszClassName, L"", WS_POPUP,
                                     crop.left, crop.top, 200, 200, nullptr, nullptr, windowClass.hInstance, &app);
            if (window == nullptr)
            {
                app.overlayPreparing_ = false;
                return false;
            }
            app.overlaySession_->Add(window);
        }
        app.overlayPreparing_ = false;
        return !app.overlayInvalidated_;
    }

    // 触发真实标注错误提示编排，显示边界由本文件的替身接收。
    // 入参：app 为隔离宿主。
    // 返回：无。
    static void Report(App& app)
    {
        app.annotationFailure_ = true;
        app.ReportAnnotationFailure();
    }

    // 查询保护所有遮罩输入的完成忙状态。
    // 入参：app 为隔离宿主。
    // 返回：整个提示期间应为 true。
    static bool Busy(const App& app)
    {
        return app.completionBusy_;
    }

    // 借用标注文档及历史，用于确认跨窗口消息不会修改编辑状态。
    // 入参：app 为隔离宿主。
    // 返回：会话持有的状态引用。
    static CaptureAnnotationState& State(App& app)
    {
        return *app.annotation_;
    }

    // 调用实际撤销入口，明确验证存在可撤销历史时的忙状态门禁。
    // 入参：app 为隔离宿主。
    // 返回：无。
    static void Undo(App& app)
    {
        app.RestoreAnnotationEdit(false);
    }

    // 检查截图会话及共享模型是否仍存活。
    // 入参：app 为隔离宿主。
    // 返回：两者均存在为 true。
    static bool Active(const App& app)
    {
        return app.overlaySession_ != nullptr && app.selectionModel_ != nullptr && app.annotation_ != nullptr;
    }

    // 检查跨屏窗口变化是否已经登记为待回收状态。
    // 入参：app 为隔离宿主。
    // 返回：已登记失效为 true。
    static bool Invalidated(const App& app)
    {
        return app.overlayInvalidated_;
    }

    // 确认当前鼠标确实位于裁剪内，避免用无效按下掩盖消息门禁缺陷。
    // 入参：app 为隔离宿主。
    // 返回：当前物理鼠标可启动绘制为 true。
    static bool CursorInside(const App& app)
    {
        POINT cursor{};
        return GetCursorPos(&cursor) && app.selectionModel_->Contains({cursor.x, cursor.y});
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
        std::ofstream(this->root_ / "resources/default_settings.json")
            << R"({"schemaVersion":1,"settings":{"export.default_format":"jpeg","export.jpeg_quality":95}})";
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

// 验证标注错误提示保护非 owner 遮罩，真实鼠标按下与直接撤销都无法修改有效编辑文档。
// 入参：无；提示窗口由替身接收，鼠标位置只读且须位于选区内。
// 返回：无；提示结束后原历史仍可实际撤销，证明忙期间未消费历史。
TEST_F(PinExportIntegrationTest, annotation_notice_blocks_other_overlay_input_and_undo)
{
    std::array<HWND, 2> windows{};
    ASSERT_TRUE(AppAnnotationTestAccess::Initialize(*this->app_, windows));
    const AnnotationSnapshot original = AppAnnotationTestAccess::State(*this->app_).Committed();
    ASSERT_NE(original, nullptr);
    ASSERT_TRUE(AppAnnotationTestAccess::State(*this->app_).CanUndo());
    // 在提示调用栈内向另一个窗口发送真实输入，并验证独立撤销入口的业务门禁。
    // 入参：owner 为错误提示的实际 owner；捕获会话句柄与原快照只在本次同步调用期间借用。
    // 返回：无；不设置键盘状态，因此 Z 消息只验证输入拦截，撤销语义由直接入口确认。
    probe.duringNotice = [this, windows, original](HWND owner)
    {
        ASSERT_TRUE(owner == windows[0] || owner == windows[1]);
        const HWND other = owner == windows[0] ? windows[1] : windows[0];
        EXPECT_TRUE(AppAnnotationTestAccess::Busy(*this->app_));
        ASSERT_TRUE(AppAnnotationTestAccess::CursorInside(*this->app_));
        SendMessageW(other, WM_LBUTTONDOWN, MK_LBUTTON, 0);
        EXPECT_FALSE(AppAnnotationTestAccess::State(*this->app_).Active());
        SendMessageW(other, WM_MOUSEMOVE, MK_LBUTTON, 0);
        SendMessageW(other, WM_LBUTTONUP, 0, 0);
        SendMessageW(other, WM_KEYDOWN, 'Z', 0);
        SendMessageW(other, WM_RBUTTONUP, 0, 0);
        AppAnnotationTestAccess::Undo(*this->app_);
        EXPECT_EQ(AppAnnotationTestAccess::State(*this->app_).Committed(), original);
        EXPECT_EQ(AppAnnotationTestAccess::State(*this->app_).Tool(), CaptureAnnotationTool::Rectangle);
        EXPECT_TRUE(AppAnnotationTestAccess::State(*this->app_).CanUndo());
        EXPECT_FALSE(AppAnnotationTestAccess::State(*this->app_).CanRedo());
    };
    AppAnnotationTestAccess::Report(*this->app_);
    EXPECT_EQ(probe.notices, 1);
    EXPECT_FALSE(AppAnnotationTestAccess::Busy(*this->app_));
    ASSERT_TRUE(AppAnnotationTestAccess::Active(*this->app_));
    AppAnnotationTestAccess::Undo(*this->app_);
    EXPECT_EQ(AppAnnotationTestAccess::State(*this->app_).Committed(), nullptr);
    EXPECT_TRUE(AppAnnotationTestAccess::State(*this->app_).CanRedo());
}

// 验证错误提示期间另一屏的显示变化或关闭请求只登记失效，提示返回后才销毁整个截图会话。
// 入参：无；分别使用 WM_DISPLAYCHANGE 和 WM_CLOSE，不创建真实提示窗口。
// 返回：无；回调栈内两个 HWND 及文档保留，外层返回后全部回收。
TEST_F(PinExportIntegrationTest, annotation_notice_defers_other_overlay_invalidation_until_return)
{
    for (UINT message : {WM_DISPLAYCHANGE, WM_CLOSE})
    {
        std::array<HWND, 2> windows{};
        ASSERT_TRUE(AppAnnotationTestAccess::Initialize(*this->app_, windows));
        const AnnotationSnapshot original = AppAnnotationTestAccess::State(*this->app_).Committed();
        // 同步注入非 owner 屏的失效请求，核验提示仍借用的窗口和文档没有提前销毁。
        // 入参：owner 为实际提示 owner；捕获的 message 为本轮失效消息。
        // 返回：无；资源回收只能发生在此回调返回后。
        probe.duringNotice = [this, windows, original, message](HWND owner)
        {
            ASSERT_TRUE(owner == windows[0] || owner == windows[1]);
            const HWND other = owner == windows[0] ? windows[1] : windows[0];
            EXPECT_TRUE(AppAnnotationTestAccess::Busy(*this->app_));
            SendMessageW(other, message, 0, 0);
            EXPECT_TRUE(AppAnnotationTestAccess::Invalidated(*this->app_));
            ASSERT_TRUE(AppAnnotationTestAccess::Active(*this->app_));
            EXPECT_EQ(AppAnnotationTestAccess::State(*this->app_).Committed(), original);
            EXPECT_TRUE(IsWindow(windows[0]));
            EXPECT_TRUE(IsWindow(windows[1]));
        };
        AppAnnotationTestAccess::Report(*this->app_);
        EXPECT_FALSE(AppAnnotationTestAccess::Busy(*this->app_));
        EXPECT_FALSE(AppAnnotationTestAccess::Active(*this->app_));
        EXPECT_FALSE(IsWindow(windows[0]));
        EXPECT_FALSE(IsWindow(windows[1]));
    }
    EXPECT_EQ(probe.notices, 2);
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
// 验证初始默认 JPEG 和配置质量实际到达 App 输出替身，临时格式不写回默认。
// 入参：无。
// 返回：无；断言两次对话框初始选择及编码参数。
TEST_F(PinExportIntegrationTest, storage_defaults_and_temporary_format_are_separate)
{
    probe.choice = SaveChoice::Accepted;
    ASSERT_TRUE(AppPinTestAccess::Execute(*this->app_, this->id_, PinCommand::Save));
    EXPECT_EQ(probe.initialFormat, ImageFileFormat::Jpeg);
    EXPECT_EQ(probe.encoding.format, ImageFileFormat::Png);
    EXPECT_EQ(probe.encoding.jpegQuality, 95);
    EXPECT_EQ(GetStringSetting("export.default_format"), "jpeg");
    probe.selectedFormat = ImageFileFormat::Jpeg;
    ASSERT_TRUE(AppPinTestAccess::Execute(*this->app_, this->id_, PinCommand::Save));
    EXPECT_EQ(probe.initialFormat, ImageFileFormat::Jpeg);
    EXPECT_EQ(probe.encoding.format, ImageFileFormat::Jpeg);
}

// 验证保存对话框期间外部修改设置只影响下一次，本次编码继续使用已取质量。
// 入参：无。
// 返回：无；实际 App 保存栈观察到固定参数。
TEST_F(PinExportIntegrationTest, storage_parameters_remain_fixed_during_dialog)
{
    ASSERT_TRUE(SetIntegerSetting("export.jpeg_quality", 37));
    probe.choice = SaveChoice::Accepted;
    probe.selectedFormat = ImageFileFormat::Jpeg;
    // 模拟模态期间另一个写入者修改配置，不进入设置窗口或真实输出。
    // 入参：未命名 HWND 为测试保存窗口 owner。
    // 返回：无。
    probe.duringDialog = [](HWND)
    {
        ASSERT_TRUE(SetIntegerSetting("export.jpeg_quality", 88));
        ASSERT_TRUE(SetStringSetting("export.default_format", "png"));
    };
    ASSERT_TRUE(AppPinTestAccess::Execute(*this->app_, this->id_, PinCommand::Save));
    EXPECT_EQ(probe.initialFormat, ImageFileFormat::Jpeg);
    EXPECT_EQ(probe.encoding.jpegQuality, 37);
    probe.duringDialog = {};
    ASSERT_TRUE(AppPinTestAccess::Execute(*this->app_, this->id_, PinCommand::Save));
    EXPECT_EQ(probe.initialFormat, ImageFileFormat::Png);
    EXPECT_EQ(probe.encoding.jpegQuality, 88);
}

// 验证无效用户参数回退有效默认，但不会借读取修复用户文件。
// 入参：无。
// 返回：无；断言质量、格式、故障位及原始值均符合约定。
TEST_F(PinExportIntegrationTest, storage_invalid_values_fall_back_without_writing)
{
    ASSERT_TRUE(SetStringSetting("export.default_format", "unknown"));
    ASSERT_TRUE(SetIntegerSetting("export.jpeg_quality", 999));
    const ImageSavePreferences preferences = ReadImageSavePreferences();
    EXPECT_EQ(preferences.defaultFormat, ImageFileFormat::Jpeg);
    EXPECT_EQ(preferences.jpegQuality, 95);
    EXPECT_EQ(preferences.invalidFields, 3U);
    EXPECT_EQ(GetStringSetting("export.default_format"), "unknown");
    EXPECT_EQ(GetIntegerSetting("export.jpeg_quality"), 999);
}

// 验证没有任何有效 JPEG 质量时，PNG 仍能保存，JPEG 在文件写入前失败且贴图保留。
// 入参：无。
// 返回：无；通过真实 App 分支检查写入次数与图像生命周期。
TEST_F(PinExportIntegrationTest, missing_quality_allows_png_but_rejects_jpeg)
{
    ASSERT_TRUE(SetIntegerSetting("export.jpeg_quality", 0));
    std::ofstream(this->root_ / "resources/default_settings.json")
        << R"({"schemaVersion":1,"settings":{"export.default_format":"jpeg","export.jpeg_quality":0}})";
    probe.choice = SaveChoice::Accepted;
    ASSERT_TRUE(AppPinTestAccess::Execute(*this->app_, this->id_, PinCommand::Save));
    EXPECT_EQ(probe.writes, 1);
    EXPECT_EQ(probe.encoding.jpegQuality, 0);
    probe.selectedFormat = ImageFileFormat::Jpeg;
    ASSERT_TRUE(AppPinTestAccess::Execute(*this->app_, this->id_, PinCommand::Save));
    EXPECT_EQ(probe.writes, 1);
    EXPECT_EQ(AppPinTestAccess::Manager(*this->app_).Image(this->id_), this->image_);
}

// 验证默认格式及用户格式均非法时不打开保存窗口，但复制仍不依赖保存设置。
// 入参：无。
// 返回：无；断言保存提前失败而复制可继续。
TEST_F(PinExportIntegrationTest, invalid_format_prevents_dialog_without_blocking_copy)
{
    ASSERT_TRUE(SetStringSetting("export.default_format", "unknown"));
    std::ofstream(this->root_ / "resources/default_settings.json") << R"({"schemaVersion":1,"settings":{}})";
    ASSERT_TRUE(AppPinTestAccess::Execute(*this->app_, this->id_, PinCommand::Save));
    EXPECT_EQ(probe.dialogs, 0);
    EXPECT_EQ(probe.writes, 0);
    ASSERT_TRUE(AppPinTestAccess::Execute(*this->app_, this->id_, PinCommand::Copy));
    EXPECT_EQ(probe.copies, 1);
}

// 验证颜色严格格式、大小写规范化及质量边界，与设置页注入的业务函数相同。
// 入参：无。
// 返回：无；通过合法与非法输入覆盖业务约束。
TEST(CaptureStorageOptionsTest, validates_color_format_and_quality)
{
    EXPECT_EQ(NormalizeSelectionBorderColor("#12abEf"), "#12ABEF");
    for (const std::string_view value : {"", "12ABEF", "#abc", "#000000FF", "#GG0000", " #000000", "#000000 "})
        EXPECT_FALSE(NormalizeSelectionBorderColor(value).has_value());
    EXPECT_TRUE(IsImageFormatSetting("jpeg"));
    EXPECT_TRUE(IsImageFormatSetting("png"));
    EXPECT_FALSE(IsImageFormatSetting("jpg"));
    EXPECT_FALSE(IsImageFormatSetting("PNG"));
    EXPECT_FALSE(IsJpegQualitySetting(0));
    EXPECT_TRUE(IsJpegQualitySetting(1));
    EXPECT_TRUE(IsJpegQualitySetting(100));
    EXPECT_FALSE(IsJpegQualitySetting(101));
}
} // namespace open_st
