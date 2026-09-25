// 将已验证内存模型与 BGRX 输入交给 Tesseract/Leptonica，缓存仅属于当前工作线程。
#include "ocr_engine.h"
#include "ocr_models.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cwctype>
#include <leptonica/allheaders.h>
#include <memory>
#include <tesseract/baseapi.h>
#include <tesseract/ocrclass.h>
#include <utility>
#include <windows.h>

namespace open_st::ocr_detail
{
namespace
{
using Clock = std::chrono::steady_clock;
thread_local const std::vector<ModelBytes>* activeModels = nullptr;
thread_local const std::atomic_bool* activeCancel = nullptr;

// Tesseract 的 C 风格回调只返回已验证内存，任何附加模型或文件请求均失败。
// 入参：filename 为引擎请求名；data 输出完整模型字节。
// 返回：命中本次固定白名单且未取消为 true；异常不跨越第三方调用边界。
bool ReadVerified(const char* filename, std::vector<char>* data) noexcept
{
    try
    {
        if (!filename || !data || !activeModels || !activeCancel || activeCancel->load())
            return false;
        std::string_view name(filename);
        const std::size_t slash = name.find_last_of("/\\");
        if (slash != std::string_view::npos)
            name.remove_prefix(slash + 1);
        for (const ModelBytes& model : *activeModels)
        {
            if (name == model.language + ".traineddata")
            {
                *data = model.bytes;
                return true;
            }
        }
    }
    catch (...)
    {
        return false;
    }
    return false;
}
struct ReaderScope
{
    // 在当前工作线程安装有界内存读取上下文。
    // 入参：models 为校验过的模型；cancel 为本次合作取消标记。
    // 返回：无。
    ReaderScope(const std::vector<ModelBytes>& models, const std::atomic_bool& cancel)
    {
        activeModels = &models;
        activeCancel = &cancel;
    }
    // 在 Init 离开后立即清除线程局部借用，避免缓存悬空模型指针。
    // 入参：无。
    // 返回：无。
    ~ReaderScope()
    {
        activeModels = nullptr;
        activeCancel = nullptr;
    }
};
struct PixCloser
{
    // 释放 Leptonica 引用计数图像。
    // 入参：value 为被唯一指针拥有的 Pix。
    // 返回：无。
    void operator()(Pix* value) const noexcept
    {
        pixDestroy(&value);
    }
};
struct PageScope
{
    tesseract::TessBaseAPI& api;
    // 所有退出路径释放当前页面、识别中间数据和图像，不清除已验证模型缓存。
    // 入参：无。
    // 返回：无。
    ~PageScope()
    {
        this->api.Clear();
    }
};
// 引擎识别监视器仅读取原子取消，不触碰窗口或任务锁。
// 入参：context 为本次取消标记；words 为引擎已找到的词数，不记录正文。
// 返回：应中断时 true。
bool CancelRecognition(void* context, int /*words*/)
{
    return static_cast<const std::atomic_bool*>(context)->load();
}
// 测量实际阶段耗时，不推算假进度。
// 入参：start 为阶段起点。
// 返回：经过的毫秒数。
double Elapsed(Clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
// 把完整 UTF-8 结果转换为有界 UTF-16，禁止部分截断或非法编码替换。
// 入参：source 为引擎零结尾输出；text 输出完整正文。
// 返回：成功或明确超限/识别错误。
OcrError Decode(const char* source, std::wstring& text)
{
    if (!source)
        return OcrError::Recognition;
    constexpr std::size_t MAX_BYTES = MAX_TEXT_UNITS * 4;
    const std::size_t length = strnlen_s(source, MAX_BYTES + 1);
    if (length > MAX_BYTES)
        return OcrError::ResultTooLarge;
    if (length == 0)
        return OcrError::None;
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, source, static_cast<int>(length), nullptr, 0);
    if (count <= 0)
        return OcrError::Recognition;
    if (static_cast<std::size_t>(count) > MAX_TEXT_UNITS)
        return OcrError::ResultTooLarge;
    text.resize(static_cast<std::size_t>(count));
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, source, static_cast<int>(length), text.data(), count) !=
        count)
        return OcrError::Recognition;
    // 空白页可只产生换行，不把它冒充有可编辑正文的成功结果。
    // 入参：value 为 UTF-16 单元。
    // 返回：空白单元为 true。
    if (std::all_of(text.begin(), text.end(), [](wchar_t value) { return std::iswspace(value) != 0; }))
        text.clear();
    return OcrError::None;
}

class LocalEngine final : public Engine
{
  public:
    // 串行加载模型、转换像素并识别；调用者保证唯一工作线程。
    // 入参：root 为资源根；image 为紧凑图；options 为固定选项；cancel 为取消；phase 为阶段回调。
    // 返回：完整原始结果或分类错误，不保存图像或正文文件。
    Recognition Recognize(const std::filesystem::path& root, const Image& image, const OcrOptions& options,
                          const std::atomic_bool& cancel, const std::function<void(OcrPhase)>& phase) override
    {
        Recognition result;
        phase(OcrPhase::Loading);
        Clock::time_point start = Clock::now();
        result.error = this->Load(root, options, cancel);
        result.loadMilliseconds = Elapsed(start);
        if (result.error != OcrError::None || cancel.load())
        {
            if (cancel.load())
                result.error = OcrError::Cancelled;
            return result;
        }
        start = Clock::now();
        std::unique_ptr<Pix, PixCloser> pix(
            pixCreate(static_cast<l_int32>(image.width), static_cast<l_int32>(image.height), 32));
        if (!pix)
        {
            result.error = OcrError::Unavailable;
            return result;
        }
        l_uint32* const data = pixGetData(pix.get());
        const std::size_t wordsPerLine = static_cast<std::size_t>(pixGetWpl(pix.get()));
        for (std::size_t row = 0; row < image.height; ++row)
        {
            if (cancel.load())
            {
                result.error = OcrError::Cancelled;
                return result;
            }
            for (std::size_t column = 0; column < image.width; ++column)
            {
                const std::size_t offset = (row * image.width + column) * 4;
                const l_uint32 blue = std::to_integer<l_uint32>(image.pixels[offset]);
                const l_uint32 green = std::to_integer<l_uint32>(image.pixels[offset + 1]);
                const l_uint32 red = std::to_integer<l_uint32>(image.pixels[offset + 2]);
                data[row * wordsPerLine + column] = (red << 24) | (green << 16) | (blue << 8);
            }
        }
        // 自有 18 像素混合文字基准表明两倍插值可避免 fast 把小字识别为空白；
        // 仅处理有界常规选区，保留原图与输出语义，不启动第二轮识别或静默缩小大图。
        const bool scaleAllowed = image.width <= 1600 && image.height <= 1200 &&
                                  static_cast<std::uint64_t>(image.width) * image.height * 4 <= MAX_PIXELS;
        if (scaleAllowed)
        {
            std::unique_ptr<Pix, PixCloser> scaled(pixScale(pix.get(), 2.0f, 2.0f));
            if (!scaled)
            {
                result.error = OcrError::Unavailable;
                return result;
            }
            pix = std::move(scaled);
        }
        if (cancel.load())
        {
            result.error = OcrError::Cancelled;
            return result;
        }
        PageScope page{*this->api_};
        this->api_->SetImage(pix.get());
        result.preprocessMilliseconds = Elapsed(start);
        phase(OcrPhase::Recognizing);
        start = Clock::now();
        tesseract::ETEXT_DESC monitor;
        monitor.cancel = CancelRecognition;
        monitor.cancel_this = const_cast<std::atomic_bool*>(&cancel);
        const int status = cancel.load() ? -1 : this->api_->Recognize(&monitor);
        if (cancel.load())
            result.error = OcrError::Cancelled;
        else if (status != 0)
            result.error = OcrError::Recognition;
        else
        {
            const std::unique_ptr<char[]> text(this->api_->GetUTF8Text());
            result.error = Decode(text.get(), result.text);
        }
        result.recognizeMilliseconds = Elapsed(start);
        if (result.error != OcrError::None)
            result.text.clear();
        return result;
    }

  private:
    // 参数切换先释放旧实例，校验全部所需模型后仅从内存初始化。
    // 入参：root 为资源根；options 为选择；cancel 为合作取消。
    // 返回：初始化完整且实际语言集合一致时 None。
    OcrError Load(const std::filesystem::path& root, const OcrOptions& options, const std::atomic_bool& cancel)
    {
        if (this->api_ && this->options_.model == options.model && this->options_.language == options.language)
            return OcrError::None;
        this->api_.reset();
        std::vector<ModelBytes> models;
        const OcrError error = LoadModels(root, options, cancel, models);
        if (error != OcrError::None)
            return error;
        if (cancel.load())
            return OcrError::Cancelled;
        auto candidate = std::make_unique<tesseract::TessBaseAPI>();
        const ReaderScope reader(models, cancel);
        // 使用绝对虚拟目录避免 TESSDATA_PREFIX 介入；读取回调从不打开该目录。
        const std::vector<std::string> names{"debug_file", "tessedit_write_images"};
        const std::vector<std::string> values{"NUL", "0"};
        if (candidate->Init("C:/open-st-verified-models/", 0, options.language.c_str(), tesseract::OEM_LSTM_ONLY,
                            nullptr, 0, &names, &values, false, ReadVerified) != 0)
            return cancel.load() ? OcrError::Cancelled : OcrError::ModelLoad;
        std::vector<std::string> loaded;
        candidate->GetLoadedLanguagesAsVector(&loaded);
        if (loaded.size() != models.size())
            return OcrError::ModelLoad;
        for (const ModelBytes& model : models)
            if (std::find(loaded.begin(), loaded.end(), model.language) == loaded.end())
                return OcrError::ModelLoad;
        candidate->SetPageSegMode(tesseract::PSM_AUTO);
        this->options_ = options;
        this->api_ = std::move(candidate);
        return OcrError::None;
    }
    std::unique_ptr<tesseract::TessBaseAPI> api_;
    OcrOptions options_;
};
} // namespace

// 实例外壳仅拥有空模型指针，不在应用启动时初始化 Tesseract。
// 入参：无。
// 返回：独占本地引擎。
std::unique_ptr<Engine> CreateEngine()
{
    return std::make_unique<LocalEngine>();
}
} // namespace open_st::ocr_detail
