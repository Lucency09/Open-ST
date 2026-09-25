// 用宽字符只读句柄读取并散列固定模型，后续引擎仅消费校验过的内存。
#include "ocr_models.h"
#include "ocr_engine.h"
#include "ocr_model_table.h"
#include <algorithm>
#include <array>
#include <memory>
#include <sha256.h>
#include <windows.h>

namespace open_st::ocr_detail
{
namespace
{
struct HandleCloser
{
    // 关闭仅由模型读取拥有的文件句柄。
    // 入参：value 为有效句柄。
    // 返回：无。
    void operator()(void* value) const noexcept
    {
        CloseHandle(value);
    }
};
using FileHandle = std::unique_ptr<void, HandleCloser>;

// 散列只与编译期常量逐字节比较，不接受运行期重新指定校验值。
// 入参：digest 为最终 SHA-256；expected 为生成头中的小写十六进制。
// 返回：完整匹配时 true。
bool DigestMatches(const std::array<std::byte, 32>& digest, std::string_view expected) noexcept
{
    constexpr std::string_view HEX = "0123456789abcdef";
    if (expected.size() != digest.size() * 2)
        return false;
    for (std::size_t index = 0; index < digest.size(); ++index)
    {
        const unsigned int value = std::to_integer<unsigned int>(digest[index]);
        if (expected[index * 2] != HEX[value >> 4] || expected[index * 2 + 1] != HEX[value & 15])
            return false;
    }
    return true;
}
// 持有拒绝写入与删除的同一文件句柄直至读取和校验完成。
// 入参：path 为宽字符文件路径；record 为编译期白名单；cancel 为取消；bytes 输出已验证内存。
// 返回：分类结果，未验证字节不会交给引擎。
OcrError ReadModel(const std::filesystem::path& path, const ModelRecord& record, const std::atomic_bool& cancel,
                   std::vector<char>& bytes)
{
    const HANDLE raw = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (raw == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND
                   ? OcrError::ModelMissing
                   : OcrError::ModelIntegrity;
    const FileHandle file(raw);
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    LARGE_INTEGER size{};
    if (!GetFileInformationByHandleEx(raw, FileAttributeTagInfo, &attributes, sizeof(attributes)) ||
        (attributes.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0 ||
        !GetFileSizeEx(raw, &size) || size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) != record.size)
        return OcrError::ModelIntegrity;
    Sha256 hash;
    if (!hash.IsValid())
        return OcrError::Unavailable;
    bytes.resize(static_cast<std::size_t>(record.size));
    std::size_t offset = 0;
    while (offset < bytes.size())
    {
        if (cancel.load())
            return OcrError::Cancelled;
        const DWORD count = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 1024 * 1024));
        DWORD read = 0;
        if (!ReadFile(raw, bytes.data() + offset, count, &read, nullptr) || read != count ||
            !hash.Append(std::as_bytes(std::span(bytes.data() + offset, read))))
            return OcrError::ModelIntegrity;
        offset += read;
    }
    std::array<std::byte, 32> digest{};
    return hash.Finish(digest) && DigestMatches(digest, record.sha256) ? OcrError::None : OcrError::ModelIntegrity;
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
        const OcrError error = ReadModel(root / std::filesystem::path(record.file), record, cancel, model.bytes);
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
