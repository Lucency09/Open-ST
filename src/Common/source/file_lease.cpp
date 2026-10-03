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
    // 锚定父目录并验证最终文件身份；失败由 error 说明。
    // 入参：路径、创建选项、Win32 打开参数及错误输出。
    // 返回：成功时持有最终文件句柄。
    bool Open(const std::filesystem::path& path, bool createParents, DWORD access, DWORD sharing, DWORD disposition,
              FileLeaseError* error);
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

// 复用路径锚定和文件身份校验，使获取租约与清理使用相同边界。
// 入参：path 为锁路径；其余参数指定创建策略、访问和共享权限；error 输出失败原因。
// 返回：安全打开文件时为 true；失败保留句柄至对象析构统一释放。
bool FileLease::Impl::Open(const std::filesystem::path& path, bool createParents, DWORD access, DWORD sharing,
                           DWORD disposition, FileLeaseError* error)
{
    const std::filesystem::path absolute = std::filesystem::absolute(path).lexically_normal();
    if (absolute.filename().empty() || absolute.native().find(L':', 2) != std::wstring::npos ||
        absolute.native().starts_with(L"\\\\"))
        return Fail(ERROR_INVALID_NAME, error);
    std::filesystem::path current = absolute.root_path();
    for (const std::filesystem::path& component : absolute.parent_path().relative_path())
    {
        current /= component;
        HANDLE directory =
            CreateFileW(current.c_str(), FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
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
            this->directories.push_back(directory);
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
    this->file = CreateFileW(absolute.c_str(), access, sharing, nullptr, disposition,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (this->file == INVALID_HANDLE_VALUE)
        return Fail(GetLastError(), error);
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(this->file, &info))
        return Fail(GetLastError(), error);
    if ((info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0 ||
        info.nNumberOfLinks != 1)
        return Fail(ERROR_ACCESS_DENIED, error);
    return true;
}

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
        auto candidate = std::make_unique<Impl>();
        if (!candidate->Open(path, createParents, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             OPEN_ALWAYS, error))
            return false;
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

// 只删除未被任何协作者打开的既有锁文件，不先释放保护再按路径删除。
// 入参：path 为锁文件；error 输出 Busy、访问或文件系统错误。
// 返回：文件已缺失或安全标记删除为 true；占用或身份不安全为 false。
bool FileLease::CleanupIdle(const std::filesystem::path& path, FileLeaseError* error) noexcept
{
    if (error != nullptr)
        *error = {};
    if (path.empty())
        return Fail(ERROR_INVALID_PARAMETER, error);
    try
    {
        Impl candidate;
        FileLeaseError openError;
        if (!candidate.Open(path, false, DELETE | FILE_READ_ATTRIBUTES, 0, OPEN_EXISTING, &openError))
        {
            if (openError.systemCode == ERROR_FILE_NOT_FOUND || openError.systemCode == ERROR_PATH_NOT_FOUND)
                return true;
            return Fail(openError.systemCode, error);
        }
        FILE_DISPOSITION_INFO disposition{TRUE};
        if (!SetFileInformationByHandle(candidate.file, FileDispositionInfo, &disposition, sizeof(disposition)))
            return Fail(GetLastError(), error);
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
