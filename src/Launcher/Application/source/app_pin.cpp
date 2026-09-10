// 将冻结选区接入独立贴图，并用稳定请求代次和模态快照协调复制保存。
#include "capture_overlay_session.h"
#include "save_image_dialog.h"
#include <app.h>
#include <clipboard_writer.h>
#include <image_file_writer.h>
#include <limits>
#include <log.h>
#include <pin_image.h>
#include <pin_window_manager.h>
#include <selection_model.h>
#include <selection_output_renderer.h>
#include <settings.h>
#include <settings_window.h>
#include <ui_text.h>
#include <utility>

namespace open_st
{
namespace
{
constexpr UINT PIN_COMMAND_MESSAGE = WM_APP + 5;
}

struct App::PinOperation final
{
    App& app;
    bool modal{};
    bool active{};
    HWND settings{};
    bool restoreSettings{};
    // 在贴图业务期间阻止新截图和设置操作，按需保留模态 owner。
    // 入参：ownerApp 为宿主；owner 为贴图 ID，零表示创建新贴图而不进入 Manager 模态。
    // 返回：无返回值；active 表示准入成功，失败时不改变 App 状态。
    PinOperation(App& ownerApp, PinId owner) : app(ownerApp)
    {
        if (owner != 0)
        {
            if (!this->app.pinManager_->BeginModal(owner))
                return;
            this->modal = true;
        }
        this->active = true;
        this->app.completionBusy_ = true;
        this->app.UpdateCaptureGate();
        this->settings = this->app.settingsWindow_ ? this->app.settingsWindow_->NativeHandle() : nullptr;
        this->restoreSettings = this->settings && IsWindowEnabled(this->settings);
        if (this->restoreSettings)
            EnableWindow(this->settings, FALSE);
    }
    // 在正常和异常返回时恢复输入，模态 owner 最后才允许销毁。
    // 入参：无。
    // 返回：无返回值；退出中保持拒绝新业务。
    ~PinOperation()
    {
        if (!this->active)
            return;
        if (this->restoreSettings && IsWindow(this->settings))
            EnableWindow(this->settings, TRUE);
        if (this->modal)
            this->app.pinManager_->EndModal();
        this->app.completionBusy_ = false;
        this->app.UpdateCaptureGate();
    }
};

// 从冻结原生帧生成独立 SDR 图像，准备成功后关闭截图并恢复贴图交互。
// 入参：无。
// 返回：无返回值；准备失败保留仍有效的选区，成功才显示新贴图。
void App::PinSelection()
{
    if (!this->CanSubmitToolbarCommand() || !this->frozenDesktopFrame_ || !this->outputRenderer_ || !this->pinManager_)
        return;
    PinId id{};
    bool prepared = false;
    {
        PinOperation operation(*this, 0);
        std::wstring error;
        try
        {
            const RectI rectangle = this->selectionModel_->Snapshot().rectangle;
            SdrSelectionFrame frame;
            if (this->outputRenderer_->Render(*this->frozenDesktopFrame_, rectangle, frame, error))
            {
                const std::shared_ptr<const PinImage> image = PinImage::Create(
                    frame.Bounds().Width(), frame.Bounds().Height(), frame.Stride(), frame.Pixels(), error);
                prepared = image && !this->overlayInvalidated_ && !this->shuttingDown_ &&
                           this->pinManager_->Prepare(image, {rectangle.left, rectangle.top}, id, error);
            }
        }
        catch (...)
        {
            OPEN_ST_LOG_ERROR("Exception preparing pinned selection.");
        }
        this->outputRenderer_->ReleaseImageResources();
        if (!prepared && !this->overlayInvalidated_ && !this->shuttingDown_)
        {
            OPEN_ST_LOG_ERROR("Cannot prepare pinned selection.");
            // 显示本地化失败原因，详细像素及截图内容不进入日志。
            // 入参：无。
            // 返回：当前语言错误文字。
            this->ShowSimpleMessage([]() { return GetUiText("pin.create_failed"); },
                                    // 查询错误提示标题。
                                    // 入参：无。
                                    // 返回：应用标题。
                                    []() { return GetUiText("app.title"); }, MB_OK | MB_ICONERROR);
        }
    }
    if (prepared || this->overlayInvalidated_ || this->shuttingDown_)
        this->CloseOverlay();
    if (prepared && !this->shuttingDown_)
        this->pinManager_->Show(id);
    else if (this->overlaySession_)
    {
        this->overlaySession_->RestoreFocus();
        this->RefreshCaptureToolbar();
    }
}

// 为存活图像预订一个独立请求，消息中不存储 HWND 或对象指针。
// 入参：id 为稳定贴图 ID；command 为复制或保存。
// 返回：成功投递 true；失败解除预订，旧消息不能消费随后新请求。
bool App::PostPinCommand(std::uint64_t id, PinCommand command) noexcept
{
    if (!IsWindow(this->messageWindow_) || (command != PinCommand::Copy && command != PinCommand::Save) ||
        !this->pinManager_ || !this->pinManager_->Image(id) || this->completionBusy_ || this->dialogActive_ ||
        this->settingsBusy_ || this->welcoming_ || this->shuttingDown_ || this->overlaySession_ ||
        this->overlayPreparing_ || this->pendingPinId_ != 0 ||
        this->pinRequestSerial_ == (std::numeric_limits<std::uint64_t>::max)())
        return false;
    this->pendingPinId_ = id;
    this->pendingPinCommand_ = command;
    const std::uint64_t request = ++this->pinRequestSerial_;
    if (PostMessageW(this->messageWindow_, PIN_COMMAND_MESSAGE, static_cast<WPARAM>(command),
                     static_cast<LPARAM>(request)))
        return true;
    this->pendingPinId_ = 0;
    OPEN_ST_LOG_WARNING("Cannot queue pinned image command. win32_error=", GetLastError());
    return false;
}

// 消费有效请求，以原图快照执行已有 Export 服务，保存取消或失败均保留贴图。
// 入参：command 为消息命令；request 为投递时的请求代次。
// 返回：无返回值；过期请求和已关闭窗口不触发输出。
void App::DispatchPinCommand(PinCommand command, std::uint64_t request)
{
    if (request != this->pinRequestSerial_ || this->pendingPinId_ == 0 || command != this->pendingPinCommand_)
        return;
    const PinId id = std::exchange(this->pendingPinId_, 0);
    if (!this->pinManager_ || this->completionBusy_ || this->dialogActive_ || this->settingsBusy_ || this->welcoming_ ||
        this->shuttingDown_ || this->overlaySession_ || this->pinManager_->IsBusy())
        return;
    const std::shared_ptr<const PinImage> image = this->pinManager_->Image(id);
    if (!image || !IsWindowVisible(this->pinManager_->Window(id)))
        return;
    {
        PinOperation operation(*this, id);
        if (!operation.active)
            return;
        const HWND owner = this->pinManager_->Window(id);
        const SdrImageView view{static_cast<std::uint32_t>(image->Width()), static_cast<std::uint32_t>(image->Height()),
                                image->Stride(), image->Pixels()};
        const char* failureKey = nullptr;
        std::wstring error;
        try
        {
            if (command == PinCommand::Copy)
            {
                if (!CopyImageToClipboard(owner, view, error))
                    failureKey = "export.copy_failed";
            }
            else if (command == PinCommand::Save)
            {
                const std::optional<std::string> directory = GetStringSetting("capture.last_save_directory");
                const std::filesystem::path last =
                    directory ? std::filesystem::path(std::u8string_view(
                                    reinterpret_cast<const char8_t*>(directory->data()), directory->size()))
                              : std::filesystem::path{};
                SaveImageTarget target;
                const SaveChoice choice = ShowSaveImageDialog(owner, last, target, error);
                if (choice == SaveChoice::Failed)
                    failureKey = "export.save_failed";
                if (choice == SaveChoice::Accepted && !this->shuttingDown_ && this->pinManager_->Window(id))
                {
                    if (!WriteImageFile(view, target.path, target.format, error))
                        failureKey = "export.save_failed";
                    else
                    {
                        const std::u8string path = target.path.parent_path().u8string();
                        if (!SetStringSetting(
                                "capture.last_save_directory",
                                std::string_view(reinterpret_cast<const char*>(path.data()), path.size())))
                            failureKey = "export.directory_failed";
                    }
                }
            }
        }
        catch (...)
        {
            failureKey = command == PinCommand::Copy ? "export.copy_failed" : "export.save_failed";
            OPEN_ST_LOG_ERROR("Exception exporting pinned image. pin_id=", id);
        }
        if (failureKey && !this->shuttingDown_)
        {
            OPEN_ST_LOG_ERROR("Pinned image export failed. pin_id=", id, " stage=", failureKey);
            // 按失败阶段取得本地化提示，模态图像和 owner 保护保持到提示返回。
            // 入参：无；捕获当前失败资源键。
            // 返回：错误正文。
            this->ShowSimpleMessage([failureKey]() { return GetUiText(failureKey); },
                                    // 查询应用错误提示标题。
                                    // 入参：无。
                                    // 返回：当前语言应用名称。
                                    []() { return GetUiText("app.title"); }, MB_OK | MB_ICONWARNING, owner);
        }
    }
    if (this->overlayInvalidated_ || this->shuttingDown_)
        this->CloseOverlay();
}
} // namespace open_st
