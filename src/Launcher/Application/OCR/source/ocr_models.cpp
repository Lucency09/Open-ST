// 用宽字符只读句柄读取并散列固定模型，后续引擎仅消费校验过的内存。
#include "ocr_models.h"
#include "ocr_engine.h"
#include "ocr_model_table.h"
#include <verified_file.h>

namespace open_st::ocr_detail
{
namespace
{
// 将通用固定字节验证结果映射到识别领域错误。
// 入参：error 为公共读取结果。
// 返回：对应的 OCR 错误，不重复实现句柄、散列或取消读取。
OcrError ModelError(VerifiedFileError error) noexcept
{
    switch (error)
    {
    case VerifiedFileError::None:
        return OcrError::None;
    case VerifiedFileError::Missing:
        return OcrError::ModelMissing;
    case VerifiedFileError::Integrity:
        return OcrError::ModelIntegrity;
    case VerifiedFileError::Cancelled:
        return OcrError::Cancelled;
    default:
        return OcrError::Unavailable;
    }
}
} // namespace

// 只构造清单中的固定相对路径，模型缺失不下载、不使用环境变量回退。
// 入参：root 为绝对程序根；options 为固定选择；cancel 为取消；models 输出完整候选。
// 返回：None 表示全部选中模型验证完成，失败不发布部分集合。
OcrError LoadModels(const std::filesystem::path& root, const OcrOptions& options, const std::atomic_bool& cancel,
                    std::vector<ModelBytes>& models)
{
    models.clear();
    if (!root.is_absolute() || !AreOcrOptionsValid(options))
        return OcrError::InvalidOptions;
    std::vector<ModelBytes> candidate;
    for (const ModelRecord& record : MODEL_RECORDS)
    {
        if (record.family != options.model ||
            (options.language != "chi_sim+eng+jpn" && record.language != options.language))
            continue;
        if (cancel.load())
            return OcrError::Cancelled;
        ModelBytes model;
        model.language = record.language;
        // 将识别取消映射到通用有界文件读取，不传递业务状态。
        // 入参：无。
        // 返回：请求已取消时 true。
        const auto stopped = [&cancel] { return cancel.load(); };
        const OcrError error = ModelError(ReadVerifiedFile(root / std::filesystem::path(record.file), record.size,
                                                           record.sha256, stopped, model.bytes));
        if (error != OcrError::None)
            return error;
        candidate.push_back(std::move(model));
    }
    if (candidate.size() != (options.language == "chi_sim+eng+jpn" ? 3U : 1U))
        return OcrError::ModelMissing;
    models = std::move(candidate);
    return OcrError::None;
}
} // namespace open_st::ocr_detail
