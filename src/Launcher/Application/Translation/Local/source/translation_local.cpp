// 使用固定 OPUS-MT INT8 权重执行同步中英翻译，共用验证字节读取并拥有唯一方向缓存。
#include "translation_model_table.h"
#include <algorithm>
#include <ctranslate2/models/model.h>
#include <ctranslate2/models/model_reader.h>
#include <ctranslate2/models/sequence_to_sequence.h>
#include <ctranslate2/utils.h>
#include <new>
#include <sentencepiece_processor.h>
#include <translation_lines.h>
#include <translation_language.h>
#include <translation_local.h>
#include <vector>
#include <verified_file.h>
#include <windows.h>

namespace open_st::translation_local
{
namespace
{
using Clock = std::chrono::steady_clock;
using detail::StopReason;
// 适配通用只读验证错误，不包含网络回退决策。
// 入参：error 为文件读取分类。
// 返回：本地引擎领域错误。
Error ModelError(VerifiedFileError error) noexcept
{
    switch (error)
    {
    case VerifiedFileError::None:
        return Error::None;
    case VerifiedFileError::Missing:
        return Error::ModelMissing;
    case VerifiedFileError::Integrity:
        return Error::ModelIntegrity;
    case VerifiedFileError::Cancelled:
        return Error::Cancelled;
    case VerifiedFileError::OutOfMemory:
        return Error::OutOfMemory;
    default:
        return Error::Unavailable;
    }
}
// 检查独立公开边界的编码与绝对字节预算，不做语言猜测。
// 入参：text 为 UTF-8 正文。
// 返回：非空、无 NUL、编码合法且有界时 true。
bool ValidText(std::string_view text) noexcept
{
    return !text.empty() && text.size() <= 40000 && text.find('\0') == std::string_view::npos &&
           MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0) >
               0;
}
} // namespace

// 查询固定包的14份文件元数据，真正散列/加载仍只在第一次执行时发生。
// 入参：root 为应用绝对资源根。
// 返回：None 不代表内容完整性、已加载状态或翻译质量认可。
Error QueryModelFiles(const std::filesystem::path& root) noexcept
{
    try
    {
        if (!root.is_absolute())
            return Error::Unavailable;
        for (const ModelFileRecord& file : MODEL_FILES)
        {
            const std::filesystem::path path = root / std::filesystem::path(file.file);
            WIN32_FILE_ATTRIBUTE_DATA attributes{};
            if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes))
            {
                const DWORD error = GetLastError();
                return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? Error::ModelMissing
                                                                                      : Error::Unavailable;
            }
            if ((attributes.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0)
                return Error::ModelIntegrity;
            const std::uint64_t size =
                (static_cast<std::uint64_t>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
            if (size != file.size)
                return Error::ModelIntegrity;
        }
        return Error::None;
    }
    catch (...)
    {
        return Error::Unavailable;
    }
}
struct Engine::Impl
{
    std::filesystem::path root;
    const ModelRecord* record{};
    std::unique_ptr<sentencepiece::SentencePieceProcessor> source, target;
    std::unique_ptr<ctranslate2::models::SequenceToSequenceReplica> replica;
    Clock::time_point releaseAt{Clock::time_point::max()};

    // 释放唯一方向全部状态；调用方确保无推理在途。
    // 入参：无。
    // 返回：无。
    void Reset() noexcept
    {
        this->replica.reset();
        this->source.reset();
        this->target.reset();
        this->record = nullptr;
        this->releaseAt = Clock::time_point::max();
    }
    // 从固定清单读取相同字节加载新方向，不按已验证路径重新打开。
    // 入参：selected 为方向；deadline/stop 为整轮预算与取消。
    // 返回：成功或模型/停止错误；新方向失败不保留半加载状态。
    Error Load(const ModelRecord& selected, Clock::time_point deadline, std::stop_token stop)
    {
        if (this->record == &selected)
            return Error::None;
        this->Reset();
        if (!this->root.is_absolute())
            return Error::Unavailable;
        ctranslate2::models::ModelMemoryReader reader(std::string(selected.direction));
        auto sourceCandidate = std::make_unique<sentencepiece::SentencePieceProcessor>();
        auto targetCandidate = std::make_unique<sentencepiece::SentencePieceProcessor>();
        for (const ModelFileRecord& file : MODEL_FILES)
        {
            if (file.direction != selected.direction)
                continue;
            std::string bytes;
            // 公共读取每 MiB 检查统一取消和时限，不把部分字节交给库。
            // 入参：无。
            // 返回：应停止时 true。
            const auto cancelled = [deadline, stop] { return StopReason(deadline, stop) != Error::None; };
            const Error error = ModelError(ReadVerifiedFile(this->root / std::filesystem::path(file.file), file.size,
                                                            file.sha256, cancelled, bytes));
            if (error == Error::OutOfMemory && !stop.stop_requested())
                return error;
            if (const Error stopped = StopReason(deadline, stop); stopped != Error::None)
                return stopped;
            if (error != Error::None)
                return error;
            if (file.name == "source.spm")
            {
                if (!sourceCandidate->LoadFromSerializedProto(bytes).ok())
                    return Error::ModelLoad;
            }
            else if (file.name == "target.spm")
            {
                if (!targetCandidate->LoadFromSerializedProto(bytes).ok())
                    return Error::ModelLoad;
            }
            else if (file.name != "LICENSE" && file.name != "README.md")
                reader.register_file(std::string(file.name), std::move(bytes));
        }
        // 直接模型副本同步执行，避免 Translator 再建自己的任务工作线程。
        const std::shared_ptr<const ctranslate2::models::Model> model =
            ctranslate2::models::Model::load(reader, ctranslate2::Device::CPU, 0, ctranslate2::ComputeType::INT8);
        std::unique_ptr<ctranslate2::models::SequenceToSequenceReplica> candidate = model->as_sequence_to_sequence();
        if (const Error stopped = StopReason(deadline, stop); stopped != Error::None)
            return stopped;
        this->replica = std::move(candidate);
        this->source = std::move(sourceCandidate);
        this->target = std::move(targetCandidate);
        this->record = &selected;
        return Error::None;
    }
    // 整文和每个内容行共用同一分词/特殊标记预算，不截断、不切分长行。
    // 入参：text 为正文，selected 为固定模型，tokens 接收完整输入。返回：分类结果。
    Error Tokenize(std::string_view text, const ModelRecord& selected, std::vector<std::string>& tokens) const
    {
        // SentencePiece 自带固定 nmt_nfkc 规范化表，不执行外部预处理脚本。
        if (!this->source->Encode(std::string(text), &tokens).ok() || tokens.empty())
            return Error::InvalidInput;
        if (!selected.sourcePrefix.empty())
            tokens.insert(tokens.begin(), std::string(selected.sourcePrefix));
        // 转换配置 add_source_eos=true，因此包括库随后添加的 EOS 计费。
        return tokens.size() + 1 > selected.maxSourceTokens ? Error::InputTooLong : Error::None;
    }
    // 同步推理一条原始内容行，加载及缓存由外层 Execute 唯一拥有。
    // 入参：text 为非空内容，selected/options/deadline/stop 在所有行保持不变。返回：完整单行或错误。
    Result TranslateLine(std::string_view text, const ModelRecord& selected, const Options& inferenceOptions,
                         Clock::time_point deadline, std::stop_token stop)
    {
        if (const Error stopped = StopReason(deadline, stop); stopped != Error::None)
            return {stopped};
        std::vector<std::string> tokens;
        if (const Error input = this->Tokenize(text, selected, tokens); input != Error::None)
            return {input};
        if (const Error stopped = StopReason(deadline, stop); stopped != Error::None)
            return {stopped};
        ctranslate2::TranslationOptions options;
        options.beam_size = inferenceOptions.beamSize;
        options.max_input_length = 0;
        // decoder_start_token 占一个位置，返回的 EOS 也计入输出预算。
        options.max_decoding_length = selected.maxOutputTokens - 1;
        options.return_end_token = true;
        options.end_token = std::string("</s>");
        // 所有束宽共用编码边界和每个解码步的合作停止检查，不依赖仅支持贪心的生成回调。
        // 入参：无。返回：取消或整项到期时 true；库丢弃未完成候选并抛出停止异常。
        options.should_stop = [deadline, stop] { return StopReason(deadline, stop) != Error::None; };
        const Clock::time_point started = Clock::now();
        const std::vector<ctranslate2::TranslationResult> translated = this->replica->translate({tokens}, {}, options);
        Result result;
        result.inferenceMilliseconds = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
        if (const Error stopped = StopReason(deadline, stop); stopped != Error::None)
            return {stopped};
        if (translated.size() != 1 || translated.front().hypotheses.size() != 1)
            return {Error::Inference};
        std::vector<std::string> output = translated.front().hypotheses.front();
        if (output.empty() || output.back() != "</s>")
            return {Error::Truncated};
        output.pop_back();
        if (!this->target->Decode(output, &result.text).ok() || result.text.empty())
            return {Error::Inference};
        if (result.text.size() > 1024 * 1024)
            return {Error::Truncated};
        if (const Error stopped = StopReason(deadline, stop); stopped != Error::None)
            return {stopped};
        return result;
    }
};

// 仅保存程序根，不在应用启动时读盘或初始化模型。
// 入参：root 为绝对程序目录。
// 返回：空闲引擎。
Engine::Engine(std::filesystem::path root) : impl_(std::make_unique<Impl>())
{
    this->impl_->root = std::move(root);
}
// 依次释放解码器及分词器，不拥有工作线程。
// 入参：无。
// 返回：无。
Engine::~Engine() = default;

// 执行明确语言方向的一次完整翻译，取消和超时均在计算真正结束后返回。
// 入参：text 为 UTF-8；source 可自动识别，target 为明确语种；options 为显式束宽；deadline 为预算；stop 为取消。
// 返回：完整结果或分类错误，不截断输入或输出。
Result Engine::Execute(std::string_view text, std::string_view source, std::string_view target, const Options& options,
                       Clock::time_point deadline, std::stop_token stop)
{
    Result result;
    result.error = StopReason(deadline, stop);
    if (result.error != Error::None)
        return result;
    if (options.beamSize == 0 || !ValidText(text))
        return {Error::InvalidInput};
    try
    {
        result.detectedLanguage = std::string(source);
        if (source == "auto")
        {
            result = detail::DetectSourceLanguage(text, deadline, stop);
            if (result.error != Error::None)
                return result;
        }
        const ModelRecord* selected = nullptr;
        for (const ModelRecord& model : MODELS)
            if (model.source == result.detectedLanguage && model.target == target)
                selected = &model;
        if (selected == nullptr)
        {
            result.error = Error::NotApplicable;
            return result;
        }
        ctranslate2::set_num_threads(2);
        const Clock::time_point loadStarted = Clock::now();
        result.error = this->impl_->Load(*selected, deadline, stop);
        result.loadMilliseconds = std::chrono::duration<double, std::milli>(Clock::now() - loadStarted).count();
        if (result.error != Error::None)
            return result;
        this->impl_->releaseAt = Clock::now() + std::chrono::minutes(5);
        std::vector<std::string> tokens;
        // 保留原有整文 512 token 门禁，换行不能扩大已批准的输入能力。
        const Error input = this->impl_->Tokenize(text, *selected, tokens);
        if (input != Error::None)
            return {input};
        if (const Error stopped = StopReason(deadline, stop); stopped != Error::None)
            return {stopped};
        Result translated =
            detail::TranslateLines(text, deadline, stop,
                                   // 每行借用同一模型和同一取消/期限；不得创建额外任务或按行重置预算。
                                   // 入参：line 为剥除原始缩进的内容行。返回：完整单行译文。
                                   [this, selected, &options, deadline, stop](std::string_view line)
                                   { return this->impl_->TranslateLine(line, *selected, options, deadline, stop); });
        this->impl_->releaseAt = Clock::now() + std::chrono::minutes(5);
        translated.loadMilliseconds = result.loadMilliseconds;
        translated.detectedLanguage = result.detectedLanguage;
        return translated;
    }
    catch (const std::bad_alloc&)
    {
        this->impl_->Reset();
        return {Error::OutOfMemory};
    }
    catch (const std::exception& error)
    {
        // 固定 CT2 4.6 的原生对齐分配器用 runtime_error 报告内存不足；不回显异常正文。
        const std::string_view message(error.what());
        const bool allocation = message.find("failed to allocate memory") != std::string_view::npos ||
                                message.find("failed to allocated memory") != std::string_view::npos;
        const Error stopped = StopReason(deadline, stop);
        const Error fallback = allocation                       ? Error::OutOfMemory
                               : this->impl_->record == nullptr ? Error::ModelLoad
                                                                : Error::Inference;
        this->impl_->Reset();
        return {allocation ? Error::OutOfMemory : stopped == Error::None ? fallback : stopped};
    }
    catch (...)
    {
        const Error stopped = StopReason(deadline, stop);
        const Error fallback = this->impl_->record == nullptr ? Error::ModelLoad : Error::Inference;
        this->impl_->Reset();
        return {stopped == Error::None ? fallback : stopped};
    }
}
// 交付可供条件变量等待的维护时刻，不进行轮询。
// 入参：无。
// 返回：下次维护时刻或 max。
Clock::time_point Engine::NextMaintenance() const noexcept
{
    return this->impl_->releaseAt;
}
// 在宿主空闲安全点卸载超过五分钟的唯一方向缓存。
// 入参：now 为单调时间。
// 返回：无。
void Engine::Maintain(Clock::time_point now) noexcept
{
    if (now >= this->impl_->releaseAt)
        this->impl_->Reset();
}
// 主动卸载，不修改任何模型文件或用户设置。
// 入参：无。
// 返回：无。
void Engine::Release() noexcept
{
    this->impl_->Reset();
}
} // namespace open_st::translation_local
