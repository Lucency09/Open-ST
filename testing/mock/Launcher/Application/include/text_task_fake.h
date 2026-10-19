// 会话与 App 专项测试共用任务边界；窗口、文案和剪贴板由各测试自行提供。
#pragma once
#include <translation_client.h>
#ifdef OPEN_ST_HAS_OCR
#include <ocr_client.h>
#endif

namespace open_st::test_text
{
struct TextTaskState
{
#ifdef OPEN_ST_HAS_OCR
    OcrSnapshot snapshot;
    OcrOptions options;
#endif
    int creates{};
    int destroys{};
    int submissions{};
    int cancellations{};
    TranslationSnapshot translation;
    TranslationRequest request;
    unsigned translationSubmissions{}, translationCancellations{};
    unsigned translationCreates{}, translationDestroys{}, translationShutdownRequests{};
    bool translationStopped{};
    bool stopping{};
    bool stopped{};
};
extern TextTaskState* state;
} // namespace open_st::test_text
