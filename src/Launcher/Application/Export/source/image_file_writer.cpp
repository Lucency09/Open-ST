// 将编码后的图像可靠写入目标文件，处理短写、刷新失败及不完整新文件清理。

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
    // 接管输出文件的关闭职责，确保成功与提前返回路径均关闭句柄。
    // 入参：handle：已经成功打开并交由本对象关闭的文件句柄；api：在守卫存活期间保持有效的借用文件接口表。
    // 返回：无返回值；析构时通过 api.close 关闭该句柄。
    OutputFile(HANDLE handle, const FileWriteApi& api) : handle_(handle), api_(api) {}
    // 关闭本次图像写入的文件句柄，使已有的关闭删除标记生效。
    // 入参：无。
    // 返回：无返回值；调用注入的 close 接口，关闭失败不通过本析构函数报告。
    ~OutputFile()
    {
        this->api_.close(this->handle_);
    }
    // 禁止复制文件句柄的管理职责，避免重复释放或关闭。
    // 入参：未命名 const OutputFile 引用：拟复制的源对象。
    // 返回：无；函数已删除，尝试调用会产生编译错误。
    OutputFile(const OutputFile&) = delete;
    // 禁止复制文件句柄的管理职责，避免重复释放或关闭。
    // 入参：未命名 const OutputFile 引用：拟复制的源对象。
    // 返回：无；函数已删除，尝试调用会产生编译错误。
    OutputFile& operator=(const OutputFile&) = delete;

  private:
    HANDLE handle_{};
    const FileWriteApi& api_;
};

// 记录文件写入错误，并为本次新建的不完整文件请求关闭时删除。
// 入参：file：仍打开的目标文件句柄；created：是否由本次操作新建；api：文件系统接口表；operation：失败操作名称；code：原始 Win32
// 错误码；error：输出参数，接收写入故障及删除标记失败详情。
// 返回：始终返回 false；仅新建文件尝试设置删除标记，已有覆盖目标保留当前内容且不回滚。
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

// 将已经编码的图像字节完整写入目标文件并刷新文件缓冲区。
// 入参：path：目标文件路径，已有文件会截断覆盖；bytes：调用期间借用的非空编码字节；api：调用期间有效的文件系统接口表；error：输出参数，失败时接收供日志记录的诊断，不直接用于界面显示。
// 返回：全部字节写入且刷新成功时为 true；失败时为 false，尝试删除本次新建的不完整文件，覆盖失败不回滚旧文件。
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

// 先在内存完成 SDR 图像编码，再经注入接口写入目标文件，调用线程须已初始化 COM。
// 入参：image：调用期间借用的顶向下 SDR/sRGB BGRX 图像，宽高为像素数、stride 为行字节跨度，第四字节不表示透明度；path：目标路径，覆盖确认由调用方完成；format：PNG 或 JPEG
// 格式；api：调用期间有效的文件系统接口表；error：输出参数，失败时接收供日志记录的诊断，不直接用于界面显示。
// 返回：编码、写入及刷新全部成功时为 true；否则为 false，编码失败不触碰目标，覆盖写入失败不保证回滚。
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

// 将 SDR 图像保存为 PNG 或 JPEG 文件，调用线程须已初始化 COM。
// 入参：image：调用期间借用的顶向下 SDR/sRGB BGRX 图像，宽高为像素数、stride 为行字节跨度，第四字节不表示透明度；path：目标路径，已有文件的覆盖确认由调用方负责；format：PNG 或
// JPEG 格式；error：输出参数，失败时接收供日志记录的诊断，不直接用于界面显示。
// 返回：完整编码、写入和刷新成功时为 true；否则为 false，编码失败不触碰目标，覆盖过程不保证原子性。
bool WriteImageFile(const SdrImageView& image, const std::filesystem::path& path, ImageFileFormat format,
                    std::wstring& error)
{
    return WriteImageWithApi(image, path, format, FileWriteApi{}, error);
}
} // namespace open_st
