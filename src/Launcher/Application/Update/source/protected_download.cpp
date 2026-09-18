// 文件职责：保护更新缓存的目录身份与访问权限，以同一文件校验 SHA-256 并完成只读启动交接。

#include "protected_download.h"

#include <aclapi.h>
#include <algorithm>
#include <array>
#include <bcrypt.h>
#include <cstring>
#include <limits>
#include <sddl.h>
#include <string>
#include <utility>
#include <vector>
#include <windows.h>

namespace open_st::update_detail
{
namespace
{
struct HandleCloser
{
    // 关闭有效 Win32 句柄。
    // 入参：handle 为句柄或空。
    // 返回：无；不传播错误。
    void operator()(void* handle) const noexcept
    {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
    }
};
using Handle = std::unique_ptr<void, HandleCloser>;

struct LocalCloser
{
    // 释放 Win32 分配的 SID 或安全描述符。
    // 入参：value 为 LocalAlloc 系列指针或空。
    // 返回：无。
    void operator()(void* value) const noexcept
    {
        if (value != nullptr)
            LocalFree(value);
    }
};
using LocalMemory = std::unique_ptr<void, LocalCloser>;

struct AlgorithmCloser
{
    // 释放 SHA-256 算法提供者。
    // 入参：value 为 BCrypt 算法句柄。
    // 返回：无。
    void operator()(void* value) const noexcept
    {
        if (value != nullptr)
            BCryptCloseAlgorithmProvider(value, 0);
    }
};

struct HashCloser
{
    // 释放增量散列及由 BCrypt 分配的内部缓冲。
    // 入参：value 为散列句柄。
    // 返回：无。
    void operator()(void* value) const noexcept
    {
        if (value != nullptr)
            BCryptDestroyHash(value);
    }
};

// 将文件系统错误分类为用户可处理的更新错误，不创建窗口或记录私有路径。
// 入参：error 为输出分类；code 为 Win32 错误码。
// 返回：false，供失败分支统一返回。
bool FileFailure(UpdateDownloadError& error, DWORD code)
{
    error = code == ERROR_ACCESS_DENIED                                       ? UpdateDownloadError::AccessDenied
            : code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION ? UpdateDownloadError::Busy
                                                                              : UpdateDownloadError::Io;
    return false;
}

// 检查句柄指向正常目录或单链接常规文件，拒绝重解析点和文件硬链接。
// 入参：handle 为已打开对象；directory 指定对象类型；error 为错误输出。
// 返回：对象符合预期时 true，否则 false。
bool CheckObject(HANDLE handle, bool directory, UpdateDownloadError& error)
{
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(handle, &information))
        return FileFailure(error, GetLastError());
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != directory ||
        (!directory && information.nNumberOfLinks != 1))
    {
        error = UpdateDownloadError::InvalidPath;
        return false;
    }
    return true;
}

// 打开目录锚点，拒绝目录自身被删除、重命名或用重解析点替换。
// 入参：path 为已验证父链下的路径；error 输出错误；extraAccess 供旧缓存清理追加 DELETE。
// 返回：具备读取属性和安全描述符能力的独占句柄，失败为空。
Handle OpenDirectory(const std::filesystem::path& path, UpdateDownloadError& error, DWORD extraAccess = 0)
{
    Handle handle(CreateFileW(path.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | READ_CONTROL | extraAccess,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                              FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (handle.get() == INVALID_HANDLE_VALUE)
    {
        FileFailure(error, GetLastError());
        return {};
    }
    if (!CheckObject(handle.get(), true, error))
        return {};
    return handle;
}

// 建立不可继承外层宽松权限的显式安全描述符。
// 入参：sid 为当前账户 SID 文本；readOnly 指定文件只读执行及删除权限；error 输出错误。
// 返回：LocalFree 所有权的安全描述符，失败为空。
LocalMemory MakeSecurity(std::wstring_view sid, bool readOnly, UpdateDownloadError& error)
{
    const std::wstring descriptor = L"O:" + std::wstring(sid) + L"D:P(A;;" + (readOnly ? L"0x1300a9" : L"FA") + L";;;" +
                                    std::wstring(sid) + L")(A;;FA;;;SY)(A;;FA;;;BA)";
    PSECURITY_DESCRIPTOR security = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(descriptor.c_str(), SDDL_REVISION_1, &security, nullptr))
    {
        FileFailure(error, GetLastError());
        return {};
    }
    return LocalMemory(security);
}

// 核验私有目录确由本账户创建且使用本次完整保护 DACL，防止创建与打开间的目录替换。
// 入参：directory 为持有中的目录；expected 为创建使用的安全描述符；error 输出错误。
// 返回：所有者及 DACL 完全匹配且禁止继承时 true，否则 false。
bool VerifyPrivateDirectory(HANDLE directory, PSECURITY_DESCRIPTOR expected, UpdateDownloadError& error)
{
    PSID actualOwner = nullptr;
    PACL actualAcl = nullptr;
    PSECURITY_DESCRIPTOR actual = nullptr;
    const DWORD queried =
        GetSecurityInfo(directory, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &actualOwner,
                        nullptr, &actualAcl, nullptr, &actual);
    const LocalMemory actualMemory(actual);
    if (queried != ERROR_SUCCESS)
        return FileFailure(error, queried);
    PSID expectedOwner = nullptr;
    PACL expectedAcl = nullptr;
    BOOL defaulted = FALSE;
    BOOL present = FALSE;
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    if (!GetSecurityDescriptorOwner(expected, &expectedOwner, &defaulted) ||
        !GetSecurityDescriptorDacl(expected, &present, &expectedAcl, &defaulted) ||
        !GetSecurityDescriptorControl(actual, &control, &revision) || !present || expectedAcl == nullptr ||
        actualAcl == nullptr || actualOwner == nullptr || !EqualSid(actualOwner, expectedOwner) ||
        (control & SE_DACL_PROTECTED) == 0 || actualAcl->AclSize != expectedAcl->AclSize ||
        std::memcmp(actualAcl, expectedAcl, actualAcl->AclSize) != 0)
    {
        error = UpdateDownloadError::AccessDenied;
        return false;
    }
    return true;
}

// 将完整 SHA-256 文本解码为固定 32 字节，不接受前缀、空白或不足长度。
// 入参：text 为预期散列；output 为解码输出。
// 返回：格式有效时 true，否则 false。
bool ParseDigest(std::string_view text, std::array<unsigned char, 32>& output)
{
    if (text.size() != output.size() * 2)
        return false;
    for (std::size_t index = 0; index < text.size(); ++index)
    {
        const char character = text[index];
        const unsigned value = character >= '0' && character <= '9'   ? static_cast<unsigned>(character - '0')
                               : character >= 'a' && character <= 'f' ? static_cast<unsigned>(character - 'a' + 10)
                               : character >= 'A' && character <= 'F' ? static_cast<unsigned>(character - 'A' + 10)
                                                                      : 16;
        if (value > 15)
            return false;
        if (index % 2 == 0)
            output[index / 2] = static_cast<unsigned char>(value << 4);
        else
            output[index / 2] |= static_cast<unsigned char>(value);
    }
    return true;
}
} // namespace

struct ProtectedDownload::Impl
{
    std::vector<Handle> ancestors;
    Handle requestDirectory;
    std::filesystem::path directory;
    std::filesystem::path path;
    Handle file;
    LocalMemory fullSecurity;
    LocalMemory readSecurity;
    std::unique_ptr<void, AlgorithmCloser> algorithm;
    std::unique_ptr<void, HashCloser> hash;
    std::array<unsigned char, 32> expectedDigest{};
    std::uint64_t expectedSize = 0;
    std::uint64_t receivedSize = 0;
    bool ownsDirectory = false;
    bool ownsFile = false;
    bool failed = false;
    bool complete = false;
    bool preserve = false;

    // 在路径锚点仍存活时释放自己的文件并清理本次私有目录。
    // 入参：无。
    // 返回：无；成功启动后只释放句柄，文件保留给安装器。
    ~Impl()
    {
        this->file.reset();
        if (!this->preserve && this->ownsFile)
            DeleteFileW(this->path.c_str());
        this->requestDirectory.reset();
        if (!this->preserve && this->ownsDirectory)
            RemoveDirectoryW(this->directory.c_str());
    }

    // 打开从本地盘符根到缓存目录的每一级锚点，只允许按需创建最后一级缓存目录。
    // 入参：root 为调用方指定绝对路径；error 输出错误；createFinal 决定是否允许创建缓存根本级。
    // 返回：路径均为非重解析目录且保活成功时 true，否则 false。
    bool Anchor(const std::filesystem::path& root, UpdateDownloadError& error, bool createFinal = true)
    {
        const std::wstring original = root.native();
        if (!root.is_absolute() || original.size() < 3 || original[1] != L':' ||
            ((original[0] < L'A' || original[0] > L'Z') && (original[0] < L'a' || original[0] > L'z')) ||
            original.find(L'\0') != std::wstring::npos || original.find(L':', 2) != std::wstring::npos ||
            original.find_first_of(L"*?<>|\"") != std::wstring::npos)
        {
            error = UpdateDownloadError::InvalidPath;
            return false;
        }
        std::filesystem::path absolute = root.lexically_normal();
        while (absolute.filename().empty() && absolute != absolute.root_path())
            absolute = absolute.parent_path();
        if (absolute == absolute.root_path())
        {
            error = UpdateDownloadError::InvalidPath;
            return false;
        }
        std::filesystem::path current = absolute.root_path();
        const UINT type = GetDriveTypeW(current.c_str());
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE)
        {
            error = UpdateDownloadError::InvalidPath;
            return false;
        }
        Handle anchor = OpenDirectory(current, error);
        if (!anchor)
            return false;
        this->ancestors.push_back(std::move(anchor));
        for (const std::filesystem::path& component : absolute.relative_path())
        {
            if (component.empty())
                continue;
            const std::wstring name = component.native();
            if (name == L"." || name == L".." || name.back() == L'.' || name.back() == L' ')
            {
                error = UpdateDownloadError::InvalidPath;
                return false;
            }
            current /= component;
            if (createFinal && current == absolute && !CreateDirectoryW(current.c_str(), nullptr) &&
                GetLastError() != ERROR_ALREADY_EXISTS)
                return FileFailure(error, GetLastError());
            anchor = OpenDirectory(current, error);
            if (!anchor)
                return false;
            this->ancestors.push_back(std::move(anchor));
        }
        this->directory = absolute;
        return true;
    }

    // 获取当前账户身份并创建后续目录、文件及清理核验共用的安全描述符。
    // 入参：error 输出错误。
    // 返回：两份安全描述符就绪时 true，否则 false。
    bool PrepareSecurity(UpdateDownloadError& error)
    {
        HANDLE rawToken = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken))
            return FileFailure(error, GetLastError());
        const Handle token(rawToken);
        DWORD tokenBytes = 0;
        (void)GetTokenInformation(token.get(), TokenUser, nullptr, 0, &tokenBytes);
        if (tokenBytes == 0)
            return FileFailure(error, GetLastError());
        std::vector<std::byte> tokenBuffer(tokenBytes);
        if (!GetTokenInformation(token.get(), TokenUser, tokenBuffer.data(), tokenBytes, &tokenBytes))
            return FileFailure(error, GetLastError());
        wchar_t* sidText = nullptr;
        if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(tokenBuffer.data())->User.Sid, &sidText))
            return FileFailure(error, GetLastError());
        const LocalMemory sidMemory(sidText);
        this->fullSecurity = MakeSecurity(sidText, false, error);
        this->readSecurity = MakeSecurity(sidText, true, error);
        if (!this->fullSecurity || !this->readSecurity)
            return false;
        return true;
    }

    // 使用本账户受保护 DACL 创建随机请求目录，并核验身份后创建 .part 文件。
    // 入参：error 输出错误。
    // 返回：目录和文件均被保护时 true，否则 false。
    bool CreatePrivateDirectory(UpdateDownloadError& error)
    {
        if (!this->PrepareSecurity(error))
            return false;
        std::array<unsigned char, 24> random{};
        if (BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()),
                            BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        {
            error = UpdateDownloadError::Io;
            return false;
        }
        std::wstring name = L"request-";
        constexpr wchar_t HEX[] = L"0123456789abcdef";
        for (const unsigned char value : random)
        {
            name += HEX[value >> 4];
            name += HEX[value & 15];
        }
        this->directory /= name;
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), this->fullSecurity.get(), FALSE};
        if (!CreateDirectoryW(this->directory.c_str(), &attributes))
            return FileFailure(error, GetLastError());
        this->requestDirectory = OpenDirectory(this->directory, error);
        if (!this->requestDirectory ||
            !VerifyPrivateDirectory(this->requestDirectory.get(), this->fullSecurity.get(), error))
            return false;
        // 只有核验为本账户的正确私有目录后，失败清理才取得删除该目录的资格。
        this->ownsDirectory = true;
        this->path = this->directory / L"setup.part";
        this->file.reset(CreateFileW(this->path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE | WRITE_DAC,
                                     FILE_SHARE_READ, &attributes, CREATE_NEW,
                                     FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (this->file.get() == INVALID_HANDLE_VALUE)
            return FileFailure(error, GetLastError());
        this->ownsFile = true;
        return CheckObject(this->file.get(), false, error);
    }

    // 将已核验文件按句柄改名，并在关闭写句柄前收紧 DACL，完成无跨用户替换窗口的只读转换。
    // 入参：error 输出错误。
    // 返回：可执行路径已由最终只读拒写／拒删除句柄保护时 true，否则 false。
    bool Seal(UpdateDownloadError& error)
    {
        const std::filesystem::path executable = this->directory / L"setup.exe";
        const std::wstring& name = executable.native();
        const std::size_t nameBytes = name.size() * sizeof(wchar_t);
        std::vector<std::byte> storage(offsetof(FILE_RENAME_INFO, FileName) + nameBytes + sizeof(wchar_t));
        FILE_RENAME_INFO* rename = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
        rename->ReplaceIfExists = FALSE;
        rename->RootDirectory = nullptr;
        rename->FileNameLength = static_cast<DWORD>(nameBytes);
        std::memcpy(rename->FileName, name.data(), nameBytes);
        if (!SetFileInformationByHandle(this->file.get(), FileRenameInfo, rename, static_cast<DWORD>(storage.size())))
            return FileFailure(error, GetLastError());
        this->path = executable;
        BOOL present = FALSE;
        BOOL defaulted = FALSE;
        PACL acl = nullptr;
        if (!GetSecurityDescriptorDacl(this->readSecurity.get(), &present, &acl, &defaulted) || !present ||
            acl == nullptr)
            return FileFailure(error, ERROR_INVALID_SECURITY_DESCR);
        const DWORD secured = SetSecurityInfo(this->file.get(), SE_FILE_OBJECT,
                                              DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr,
                                              nullptr, acl, nullptr);
        if (secured != ERROR_SUCCESS)
            return FileFailure(error, secured);
        BY_HANDLE_FILE_INFORMATION original{};
        if (!GetFileInformationByHandle(this->file.get(), &original))
            return FileFailure(error, GetLastError());
        // 原写句柄含 DELETE，桥接句柄须允许该已有访问；原写句柄仍阻止外部写入或删除。
        Handle bridge(CreateFileW(this->path.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (bridge.get() == INVALID_HANDLE_VALUE)
            return FileFailure(error, GetLastError());
        this->file.reset();
        Handle sealed(CreateFileW(this->path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (sealed.get() == INVALID_HANDLE_VALUE)
            return FileFailure(error, GetLastError());
        BY_HANDLE_FILE_INFORMATION finalInformation{};
        if (!CheckObject(sealed.get(), false, error))
            return false;
        if (!GetFileInformationByHandle(sealed.get(), &finalInformation))
            return FileFailure(error, GetLastError());
        if (original.dwVolumeSerialNumber != finalInformation.dwVolumeSerialNumber ||
            original.nFileIndexHigh != finalInformation.nFileIndexHigh ||
            original.nFileIndexLow != finalInformation.nFileIndexLow)
        {
            error = UpdateDownloadError::InvalidPath;
            return false;
        }
        this->file = std::move(sealed);
        return true;
    }
};

// 只清理名称、所有权、权限和内容均可核实且没有占用的旧请求。
// 入参：cacheRoot 为显式更新操作指定的缓存目录。
// 返回：无；目录或文件验证失败时保持原状，不递归删除或重试。
void ProtectedDownload::CleanupOwned(const std::filesystem::path& cacheRoot) noexcept
{
    try
    {
        Impl guard;
        UpdateDownloadError error = UpdateDownloadError::None;
        if (!guard.Anchor(cacheRoot, error, false) || !guard.PrepareSecurity(error))
            return;
        std::error_code iterationError;
        std::filesystem::directory_iterator iterator(guard.directory, iterationError);
        const std::filesystem::directory_iterator end;
        for (unsigned count = 0; count < 128 && !iterationError && iterator != end;
             ++count, iterator.increment(iterationError))
        {
            const std::filesystem::path requestPath = iterator->path();
            const std::wstring name = requestPath.filename().native();
            if (name.size() != 56 || !name.starts_with(L"request-") ||
                name.find_first_not_of(L"0123456789abcdef", 8) != std::wstring::npos)
                continue;
            Handle directory = OpenDirectory(requestPath, error, DELETE);
            if (!directory || !VerifyPrivateDirectory(directory.get(), guard.fullSecurity.get(), error))
                continue;
            std::error_code childError;
            std::filesystem::directory_iterator child(requestPath, childError);
            if (childError)
                continue;
            if (child != end)
            {
                const std::filesystem::path filePath = child->path();
                const std::wstring filename = filePath.filename().native();
                if (filename != L"setup.exe" && filename != L"setup.part")
                    continue;
                child.increment(childError);
                if (childError || child != end)
                    continue;
                Handle file(CreateFileW(filePath.c_str(), DELETE | READ_CONTROL | FILE_READ_ATTRIBUTES, 0, nullptr,
                                        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
                if (file.get() == INVALID_HANDLE_VALUE || !CheckObject(file.get(), false, error))
                    continue;
                if (!VerifyPrivateDirectory(file.get(), guard.fullSecurity.get(), error) &&
                    !VerifyPrivateDirectory(file.get(), guard.readSecurity.get(), error))
                    continue;
                FILE_DISPOSITION_INFO disposition{TRUE};
                if (!SetFileInformationByHandle(file.get(), FileDispositionInfo, &disposition, sizeof(disposition)))
                    continue;
            }
            FILE_DISPOSITION_INFO disposition{TRUE};
            (void)SetFileInformationByHandle(directory.get(), FileDispositionInfo, &disposition, sizeof(disposition));
        }
    }
    catch (...)
    {
        // 可选缓存清理不能扩大删除范围或阻止用户发起正常更新。
    }
}

// 创建并校验独占下载事务；不允许非本地路径、零大小或超过发行上限的安装包。
// 入参：cacheRoot 为缓存目录；expectedSize 为精确大小；expectedSHA256 为摘要；error 输出错误。
// 返回：受保护下载对象或空，失败时仅回收本次状态。
std::unique_ptr<ProtectedDownload> ProtectedDownload::Create(const std::filesystem::path& cacheRoot,
                                                             std::uint64_t expectedSize,
                                                             std::string_view expectedSHA256,
                                                             UpdateDownloadError& error)
{
    error = UpdateDownloadError::None;
    try
    {
        auto impl = std::make_unique<Impl>();
        if (expectedSize == 0 || expectedSize > 512ULL * 1024 * 1024 ||
            !ParseDigest(expectedSHA256, impl->expectedDigest))
        {
            error = UpdateDownloadError::SizeMismatch;
            return {};
        }
        impl->expectedSize = expectedSize;
        if (!impl->Anchor(cacheRoot, error) || !impl->CreatePrivateDirectory(error))
            return {};
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        {
            error = UpdateDownloadError::Io;
            return {};
        }
        impl->algorithm.reset(algorithm);
        BCRYPT_HASH_HANDLE hash = nullptr;
        if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0)
        {
            error = UpdateDownloadError::Io;
            return {};
        }
        impl->hash.reset(hash);
        return std::make_unique<ProtectedDownload>(ConstructionKey{}, std::move(impl));
    }
    catch (...)
    {
        error = UpdateDownloadError::Io;
        return {};
    }
}

// 接管初始化完整的缓存事务状态。
// 入参：ConstructionKey 为类内令牌；impl 为独占实现。
// 返回：无。
ProtectedDownload::ProtectedDownload(ConstructionKey, std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

// 释放事务；具体清理由 Impl 按完成交接标志执行。
// 入参：无。
// 返回：无。
ProtectedDownload::~ProtectedDownload() = default;

// 写入并散列有界响应块；中途失败后拒绝继续提交。
// 入参：bytes 为网络层借用数据；error 输出错误。
// 返回：所有字节成功写入并计入摘要时 true，否则 false。
bool ProtectedDownload::Append(std::span<const std::byte> bytes, UpdateDownloadError& error)
{
    error = UpdateDownloadError::None;
    if (this->impl_->failed || this->impl_->complete ||
        bytes.size() > this->impl_->expectedSize - this->impl_->receivedSize)
    {
        this->impl_->failed = true;
        error = UpdateDownloadError::SizeMismatch;
        return false;
    }
    std::size_t offset = 0;
    while (offset < bytes.size())
    {
        const DWORD count = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 1024 * 1024));
        DWORD written = 0;
        if (!WriteFile(this->impl_->file.get(), bytes.data() + offset, count, &written, nullptr) || written != count ||
            BCryptHashData(this->impl_->hash.get(),
                           reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data() + offset)), count, 0) < 0)
        {
            this->impl_->failed = true;
            error = UpdateDownloadError::Io;
            return false;
        }
        offset += count;
        this->impl_->receivedSize += count;
    }
    return true;
}

// 完成精确长度和散列校验，然后转换为安装器可读取且无法被他人替换的文件。
// 入参：error 输出错误。
// 返回：成功获得最终只读保护时 true；任何失败都不能 Preserve 或启动。
bool ProtectedDownload::Complete(UpdateDownloadError& error)
{
    error = UpdateDownloadError::None;
    if (this->impl_->complete)
        return true;
    if (this->impl_->failed || this->impl_->receivedSize != this->impl_->expectedSize)
    {
        this->impl_->failed = true;
        error = UpdateDownloadError::SizeMismatch;
        return false;
    }
    this->impl_->failed = true;
    try
    {
        LARGE_INTEGER actual{};
        if (!GetFileSizeEx(this->impl_->file.get(), &actual) || !FlushFileBuffers(this->impl_->file.get()))
            return FileFailure(error, GetLastError());
        if (!CheckObject(this->impl_->file.get(), false, error))
            return false;
        if (actual.QuadPart < 0 || static_cast<std::uint64_t>(actual.QuadPart) != this->impl_->expectedSize)
        {
            error = UpdateDownloadError::SizeMismatch;
            return false;
        }
        std::array<unsigned char, 32> actualDigest{};
        if (BCryptFinishHash(this->impl_->hash.get(), actualDigest.data(), static_cast<ULONG>(actualDigest.size()), 0) <
            0)
        {
            error = UpdateDownloadError::Io;
            return false;
        }
        if (actualDigest != this->impl_->expectedDigest)
        {
            error = UpdateDownloadError::HashMismatch;
            return false;
        }
        if (!this->impl_->Seal(error))
            return false;
        this->impl_->complete = true;
        this->impl_->failed = false;
        return true;
    }
    catch (...)
    {
        error = UpdateDownloadError::Io;
        return false;
    }
}

// 返回当前受事务所有权保护的文件位置。
// 入参：无。
// 返回：对象内路径引用。
const std::filesystem::path& ProtectedDownload::Path() const noexcept
{
    return this->impl_->path;
}

// 仅在完整性校验和只读转换完成后保留系统已启动的安装文件。
// 入参：无。
// 返回：无；未完成事务保持清理行为。
void ProtectedDownload::Preserve() noexcept
{
    if (this->impl_->complete)
        this->impl_->preserve = true;
}
} // namespace open_st::update_detail
