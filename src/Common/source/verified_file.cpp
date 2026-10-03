// 用唯一只读句柄和公共 SHA-256 读取可信资源，供不同模型消费者共用。
#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <new>
#include <sha256.h>
#include <span>
#include <verified_file.h>
#include <windows.h>

namespace open_st
{
namespace
{
struct HandleCloser
{
    // 回收独占拥有的只读文件句柄。
    // 入参：value 为句柄。
    // 返回：无。
    void operator()(void* value) const noexcept
    {
        CloseHandle(value);
    }
};
// 将摘要与受信任的十六进制常量比较。
// 入参：digest 为摘要；expected 为小写 SHA-256。
// 返回：完全匹配时 true。
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
} // namespace
// 在同一只读身份内读取并散列；取消后不交付部分数据。
// 入参：path 为路径；size/sha256 为预期；cancel 为停止谓词；bytes 为输出。
// 返回：完整成功或分类错误；失败输出为空。
VerifiedFileError ReadVerifiedFile(const std::filesystem::path& path, std::uint64_t size, std::string_view sha256,
                                   const std::function<bool()>& cancel, std::string& bytes) noexcept
{
    bytes.clear();
    try
    {
        if (cancel && cancel())
            return VerifiedFileError::Cancelled;
        if (size == 0 || size > std::numeric_limits<std::size_t>::max() || sha256.size() != 64)
            return VerifiedFileError::Integrity;
        const HANDLE raw = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                       FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (raw == INVALID_HANDLE_VALUE)
            return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND
                       ? VerifiedFileError::Missing
                       : VerifiedFileError::Integrity;
        const std::unique_ptr<void, HandleCloser> file(raw);
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        LARGE_INTEGER actual{};
        if (!GetFileInformationByHandleEx(raw, FileAttributeTagInfo, &attributes, sizeof(attributes)) ||
            (attributes.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0 ||
            !GetFileSizeEx(raw, &actual) || actual.QuadPart < 0 || static_cast<std::uint64_t>(actual.QuadPart) != size)
            return VerifiedFileError::Integrity;
        Sha256 hash;
        if (!hash.IsValid())
            return VerifiedFileError::Unavailable;
        std::string candidate(static_cast<std::size_t>(size), '\0');
        std::size_t offset = 0;
        while (offset < candidate.size())
        {
            if (cancel && cancel())
                return VerifiedFileError::Cancelled;
            const DWORD count = static_cast<DWORD>(std::min<std::size_t>(candidate.size() - offset, 1024 * 1024));
            DWORD read = 0;
            if (!ReadFile(raw, candidate.data() + offset, count, &read, nullptr) || read != count ||
                !hash.Append(std::as_bytes(std::span(candidate.data() + offset, read))))
                return VerifiedFileError::Integrity;
            offset += read;
        }
        std::array<std::byte, 32> digest{};
        if (!hash.Finish(digest) || !DigestMatches(digest, sha256))
            return VerifiedFileError::Integrity;
        if (cancel && cancel())
            return VerifiedFileError::Cancelled;
        bytes = std::move(candidate);
        return VerifiedFileError::None;
    }
    catch (const std::bad_alloc&)
    {
        return VerifiedFileError::OutOfMemory;
    }
    catch (...)
    {
        return VerifiedFileError::Unavailable;
    }
}
} // namespace open_st
