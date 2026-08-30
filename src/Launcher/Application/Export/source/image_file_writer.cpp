#include "file_write_api.h"
#include "image_encoder.h"

#include <algorithm>
#include <limits>

namespace open_st
{
namespace
{
// 文件句柄在成功与失败路径均关闭，不通过文件名删除以避免替换竞争。
class OutputFile final
{
  public:
    // 接管已经打开的文件句柄。
    OutputFile(HANDLE handle, const FileWriteApi& api) : handle_(handle), api_(api) {}
    // 关闭句柄；新文件清理标记将在此生效。
    ~OutputFile()
    {
        this->api_.close(this->handle_);
    }
    // 禁止复制句柄所有权。
    OutputFile(const OutputFile&) = delete;
    // 禁止赋值句柄所有权。
    OutputFile& operator=(const OutputFile&) = delete;

  private:
    HANDLE handle_{};
    const FileWriteApi& api_;
};

// 记录写入故障并仅对本次新建文件设置关闭删除，覆盖目标始终保留。
bool FileFailure(HANDLE file, bool created, const FileWriteApi& api, const wchar_t* operation, DWORD code,
                 std::wstring& error)
{
    error = std::wstring(operation) + L" Win32=" + std::to_wstring(code);
    if (created)
    {
        FILE_DISPOSITION_INFO disposition{TRUE};
        if (api.setInformation(file, FileDispositionInfo, &disposition, sizeof(disposition)) == FALSE)
        {
            error += L"; incomplete file cleanup failed Win32=" + std::to_wstring(GetLastError());
        }
    }
    return false;
}
} // namespace

// 先尝试独占创建，再仅覆盖已经存在的路径；写入采用有上界的块循环。
bool WriteEncodedFile(const std::filesystem::path& path, std::span<const std::uint8_t> bytes, const FileWriteApi& api,
                      std::wstring& error)
{
    error.clear();
    if (path.empty() || bytes.empty())
    {
        error = L"Empty image path or encoded image";
        return false;
    }
    HANDLE file =
        api.create(path.c_str(), GENERIC_WRITE | DELETE, 0U, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool created = file != INVALID_HANDLE_VALUE;
    if (!created)
    {
        const DWORD createError = GetLastError();
        if (createError != ERROR_FILE_EXISTS && createError != ERROR_ALREADY_EXISTS)
        {
            error = L"Create image file Win32=" + std::to_wstring(createError);
            return false;
        }
        file = api.create(path.c_str(), GENERIC_WRITE, 0U, nullptr, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            error = L"Open image overwrite target Win32=" + std::to_wstring(GetLastError());
            return false;
        }
    }
    const OutputFile guard(file, api);
    std::size_t position{};
    constexpr std::size_t CHUNK_BYTES = 1024U * 1024U;
    while (position < bytes.size())
    {
        const DWORD count = static_cast<DWORD>((std::min)(CHUNK_BYTES, bytes.size() - position));
        DWORD written{};
        if (api.write(file, bytes.data() + position, count, &written, nullptr) == FALSE)
        {
            return FileFailure(file, created, api, L"Write image bytes", GetLastError(), error);
        }
        if (written == 0U || written > count)
        {
            return FileFailure(file, created, api, L"Invalid image write length", ERROR_WRITE_FAULT, error);
        }
        position += written;
    }
    if (api.flush(file) == FALSE)
    {
        return FileFailure(file, created, api, L"Flush image file", GetLastError(), error);
    }
    return true;
}

// 编码全部成功后才触碰目标文件，编码失败不会截断旧文件。
bool WriteImageWithApi(const SdrImageView& image, const std::filesystem::path& path, ImageFileFormat format,
                       const FileWriteApi& api, std::wstring& error)
{
    std::vector<std::uint8_t> encoded;
    if (!EncodeSdrImage(image, format, encoded, error))
    {
        return false;
    }
    return WriteEncodedFile(path, encoded, api, error);
}

// 产品入口绑定真实文件边界，保持内存编码先于文件创建的顺序。
bool WriteImageFile(const SdrImageView& image, const std::filesystem::path& path, ImageFileFormat format,
                    std::wstring& error)
{
    return WriteImageWithApi(image, path, format, FileWriteApi{}, error);
}
} // namespace open_st
