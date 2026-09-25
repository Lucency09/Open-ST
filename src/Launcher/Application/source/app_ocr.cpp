// 将截图完成入口适配到独立OCR会话，模型及线程状态不进入App。
#include "capture_annotation_state.h"
#include "ocr_session.h"
#include <app.h>
#include <log.h>
#include <sdr_selection_frame.h>
#include <selection_model.h>
#include <settings.h>
#include <stdexcept>
#include <ui_text.h>
namespace open_st
{
// 固定当前选区与已提交标注，生成后立即释放编辑准入锁。
// 入参：无。返回：无，异步识别不占用截图完成忙状态。
void App::RecognizeSelection()
{
#ifdef OPEN_ST_HAS_OCR
    if (!this->CanSubmitToolbarCommand())
        return;
    try
    {
        if (!this->ocrSession_)
            this->ocrSession_ = std::make_unique<OcrSession>(this->messageWindow_, this->largeIcon_);
        if (this->ocrSession_->ActivateExisting())
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
        this->ocrSession_->Begin(frame, GetStringSetting("ocr.model").value_or("fast"),
                                 GetStringSetting("ocr.language").value_or("chi_sim+eng+jpn"));
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
} // namespace open_st
