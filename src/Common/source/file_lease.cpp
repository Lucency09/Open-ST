// 实现不等待的 Win32 字节锁与完整路径锚定。

#include <file_lease.h>
#include <utility>
#include <vector>
#include <windows.h>

namespace open_st
{
class FileLease::Impl final
{
  public:
    // 释放锚定句柄；关闭锁文件自动释放字节锁。
    // 入参：无。
    // 返回：无返回值。
    ~Impl()
    {
        if (this->file != INVALID_HANDLE_VALUE)
            CloseHandle(this->file);
        for (HANDLE directory : this->directories)
            CloseHandle(directory);
    }
    HANDLE file{INVALID_HANDLE_VALUE};
    std::vector<HANDLE> directories;
};

namespace
{
// 转换文件协调错误，不记录日志以避免 Logger 的递归依赖。
// 入参：code：系统错误；error：可选输出。
// 返回：恒为 false。
bool Fail(DWORD code, FileLeaseError* error) noexcept
{
    if (error != nullptr)
    {
        error->systemCode = code;
        error->code = code == ERROR_LOCK_VIOLATION || code == ERROR_SHARING_VIOLATION ? FileLeaseErrorCode::Busy
                      : code == ERROR_ACCESS_DENIED                                   ? FileLeaseErrorCode::AccessDenied
                                                                                      : FileLeaseErrorCode::Io;
    }
    return false;
}
} // namespace

// 创建空租约。
// 入参：无。
// 返回：无返回值。
FileLease::FileLease() noexcept = default;
// 释放系统租约及目录锚点。
// 入参：无。
// 返回：无返回值。
FileLease::~FileLease() = default;
// 转移租约唯一所有权。
// 入参：other：源租约。
// 返回：无返回值。
FileLease::FileLease(FileLease&& other) noexcept = default;
// 释放旧租约并接收新租约。
// 入参：other：源租约。
// 返回：当前对象。
FileLease& FileLease::operator=(FileLease&& other) noexcept = default;

// 锚定路径并立即尝试稳定锁文件的第零字节。
// 入参：path：锁路径；mode：共享或独占；error：可选分类错误输出。
// 返回：成功 true，失败 false；不等待、不重试。
bool FileLease::TryAcquire(const std::filesystem::path& path, FileLeaseMode mode, FileLeaseError* error,
                           bool createParents) noexcept
{
    if (error != nullptr)
        *error = {};
    if (this->IsHeld() || path.empty())
        return Fail(ERROR_INVALID_PARAMETER, error);
    try
    {
        const std::filesystem::path absolute = std::filesystem::absolute(path).lexically_normal();
        if (absolute.filename().empty() || absolute.native().find(L':', 2) != std::wstring::npos ||
            absolute.native().starts_with(L"\\\\"))
            return Fail(ERROR_INVALID_NAME, error);
        auto candidate = std::make_unique<Impl>();
        std::filesystem::path current = absolute.root_path();
        for (const std::filesystem::path& component : absolute.parent_path().relative_path())
        {
            current /= component;
            HANDLE directory = CreateFileW(current.c_str(), FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
                                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (directory == INVALID_HANDLE_VALUE)
            {
                const DWORD openError = GetLastError();
                if (!createParents || (openError != ERROR_FILE_NOT_FOUND && openError != ERROR_PATH_NOT_FOUND))
                    return Fail(openError, error);
                // 此时前一级目录仍由已有句柄锚定；创建后重新以不跟随链接的句柄验证。
                if (!CreateDirectoryW(current.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
                    return Fail(GetLastError(), error);
                directory = CreateFileW(current.c_str(), FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
                if (directory == INVALID_HANDLE_VALUE)
                    return Fail(GetLastError(), error);
            }
            try
            {
                candidate->directories.push_back(directory);
            }
            catch (...)
            {
                CloseHandle(directory);
                throw;
            }
            BY_HANDLE_FILE_INFORMATION info{};
            if (!GetFileInformationByHandle(directory, &info))
                return Fail(GetLastError(), error);
            if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
                (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
                return Fail(ERROR_ACCESS_DENIED, error);
        }
        candidate->file =
            CreateFileW(absolute.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (candidate->file == INVALID_HANDLE_VALUE)
            return Fail(GetLastError(), error);
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(candidate->file, &info))
            return Fail(GetLastError(), error);
        if ((info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0 ||
            info.nNumberOfLinks != 1)
            return Fail(ERROR_ACCESS_DENIED, error);
        OVERLAPPED position{};
        const DWORD flags =
            LOCKFILE_FAIL_IMMEDIATELY | (mode == FileLeaseMode::Exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0U);
        if (!LockFileEx(candidate->file, flags, 0, 1, 0, &position))
            return Fail(GetLastError(), error);
        this->impl_ = std::move(candidate);
        return true;
    }
    catch (...)
    {
        return Fail(ERROR_GEN_FAILURE, error);
    }
}

// 显式释放当前租约但保留磁盘锁文件。
// 入参：无。
// 返回：无返回值。
void FileLease::Reset() noexcept
{
    this->impl_.reset();
}
// 查询当前对象是否持有租约。
// 入参：无。
// 返回：已持有为 true。
bool FileLease::IsHeld() const noexcept
{
    return this->impl_ != nullptr;
}
} // namespace open_st
